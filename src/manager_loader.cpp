#define APH_BUILD_MANAGER
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "manager_loader.h"
#include "hub_internal.h"
#include "manager_events.h"
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <algorithm>
#include <vector>

namespace {
volatile LONG boot_state = APH_COLD, boot_result = APH_PENDING, scan_count;
SRWLOCK plugin_lock = SRWLOCK_INIT;
std::vector<loader::Plugin> plugins;
unsigned plugin_count;
std::vector<unsigned char> desired, requested;
HANDLE loader_wake;
wchar_t plugin_directory[MAX_PATH]{};
manager_config::Settings initial_preferences;

// Catalog growth occurs only during preflight, before management/UI workers.
bool append_plugin(const wchar_t* filename) noexcept {
    try {
        desired.resize(size_t(plugin_count)+1);
        requested.resize(size_t(plugin_count)+1);
        plugins.emplace_back();
        wcscpy_s(plugins.back().filename,filename);
        ++plugin_count;
        return true;
    } catch(const std::bad_alloc&) { return false; }
      catch(const std::length_error&) { return false; }
}

bool range(uint32_t rva, size_t bytes, uint32_t image_size) {
    return rva < image_size && bytes <= image_size - rva;
}
// Inspect a non-executable PE image mapping. Dependencies and rejected ABI never
// run DllMain or static constructors. Manifest is fixed DATA, not a function.
APH_Result read_data_export(const unsigned char* base, const char* name, void* out, size_t bytes) {
    __try {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 0x100000) return APH_E_ABI;
        const IMAGE_NT_HEADERS64* pe = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        if (pe->Signature != IMAGE_NT_SIGNATURE || pe->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            pe->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            pe->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return APH_E_ABI;
        uint32_t size = pe->OptionalHeader.SizeOfImage;
        IMAGE_DATA_DIRECTORY d = pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!d.VirtualAddress) return APH_E_UNAVAILABLE;
        if (!range(d.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY), size) || !range(d.VirtualAddress, d.Size, size)) return APH_E_ABI;
        const IMAGE_EXPORT_DIRECTORY* exports = (const IMAGE_EXPORT_DIRECTORY*)(base + d.VirtualAddress);
        if (exports->NumberOfNames > 65536 || exports->NumberOfFunctions > 65536 ||
            !range(exports->AddressOfNames, size_t(exports->NumberOfNames) * 4, size) ||
            !range(exports->AddressOfNameOrdinals, size_t(exports->NumberOfNames) * 2, size) ||
            !range(exports->AddressOfFunctions, size_t(exports->NumberOfFunctions) * 4, size)) return APH_E_ABI;
        const DWORD* names = (const DWORD*)(base + exports->AddressOfNames);
        const WORD* ordinals = (const WORD*)(base + exports->AddressOfNameOrdinals);
        const DWORD* functions = (const DWORD*)(base + exports->AddressOfFunctions);
        for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
            if (!range(names[i], strlen(name)+1, size) || memcmp(base + names[i], name, strlen(name)+1)) continue;
            if (ordinals[i] >= exports->NumberOfFunctions) return APH_E_ABI;
            DWORD rva = functions[ordinals[i]];
            if (!range(rva, bytes, size) || (rva >= d.VirtualAddress && rva - d.VirtualAddress < d.Size)) return APH_E_ABI;
            const auto sections=IMAGE_FIRST_SECTION(pe);
            bool readonly=false;
            for(unsigned j=0;j<pe->FileHeader.NumberOfSections;++j) {
                const auto& section=sections[j];
                if(rva>=section.VirtualAddress&&rva-section.VirtualAddress<section.Misc.VirtualSize&&
                    bytes<=section.Misc.VirtualSize-(rva-section.VirtualAddress)&&
                    !(section.Characteristics&(IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE)))readonly=true;
            }
            if(!readonly)return APH_E_ABI;
            memcpy(out, base + rva, bytes);
            return APH_OK;
        }
        return APH_E_UNAVAILABLE;
    } __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ?
               EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return APH_E_ABI; }
}
APH_Result preflight(const wchar_t* path, loader::Plugin* plugin) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {plugin->win32_error=GetLastError();return APH_E_ARGUMENT;}
    HANDLE mapping = nullptr; void* base = nullptr; APH_Result result = APH_E_ABI;
    __try {
        mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, 0, 0, nullptr);
        if (mapping) {
            base = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
            if (base) {
                result=read_data_export((const unsigned char*)base,"APH_PluginManifest",&plugin->manifest,sizeof(plugin->manifest));
                if(result==APH_OK) {
                    auto info=read_data_export((const unsigned char*)base,"APH_PluginInfo",&plugin->info,sizeof(plugin->info));
                    plugin->has_info=info==APH_OK;
                    if(info!=APH_OK&&info!=APH_E_UNAVAILABLE)result=info;
                }
            }
            if(!base)plugin->win32_error=GetLastError();
        } else plugin->win32_error=GetLastError();
    } __finally {
        if (base) UnmapViewOfFile(base);
        if (mapping) CloseHandle(mapping);
        CloseHandle(file);
    }
    return result;
}
bool valid_manifest(const APH_Manifest& m) {
    if (m.size != sizeof(m) || m.abi_major != APH_ABI_MAJOR || m.required_minor > APH_ABI_MINOR ||
        m.architecture != APH_ARCH_X64 || (m.flags & ~APH_PLUGIN_FLAGS) || ((m.flags & ~APH_PLUGIN_LIFECYCLE) && !(m.flags & APH_PLUGIN_LIFECYCLE)) || !m.id[0] || !memchr(m.id, 0, sizeof(m.id)) ||
        !memchr(m.display_name, 0, sizeof(m.display_name))) return false;
    for (const char* p = m.id; *p; ++p)
        if (!(*p >= 'a' && *p <= 'z') && !(*p >= '0' && *p <= '9') && *p != '.' && *p != '-' && *p != '_') return false;
    return true;
}
void save_plugin(unsigned index, const loader::Plugin& p) {
    bool changed=false;
    AcquireSRWLockExclusive(&plugin_lock);
    __try { changed=memcmp(&plugins[index],&p,sizeof(p))!=0;plugins[index] = p; }
    __finally { ReleaseSRWLockExclusive(&plugin_lock); }
    if(!changed)return;
    manager_events::notify();
    char filename[MAX_PATH * 3]{};
    WideCharToMultiByte(CP_UTF8, 0, p.filename, -1, filename, sizeof(filename), nullptr, nullptr);
    char message[1024];
    _snprintf_s(message, sizeof(message), _TRUNCATE,
        "[APH] file=%s id=%.64s owner=%llu state=%s loaded=%u attached=%u capabilities=%u result=%d stage=%s win32=%u detail=%s\n",
        filename, p.manifest.id, (unsigned long long)p.owner, loader::state_name(p.state),
        (unsigned)p.loaded, (unsigned)p.attached, (unsigned)p.capabilities_ready, p.result,p.stage,p.win32_error,p.detail);
    hub::log_line(message);
}
bool valid_info(const loader::Plugin& p) {
    if(!p.has_info)return true;
    const auto& info=p.info;
    if(p.manifest.required_minor<1||info.size!=sizeof(info)||info.format!=1||info.reserved||
        info.dependency_count>APH_MAX_DEPENDENCIES||info.plugin_version>>48)return false;
    for(unsigned i=0;i<info.dependency_count;++i) {
        const auto& d=info.dependencies[i];
        APH_Manifest id=p.manifest;memcpy(id.id,d.id,sizeof(id.id));
        if(!valid_manifest(id)||d.flags&~APH_DEP_OPTIONAL||d.reserved||d.min_version>>48||d.max_version>>48||
            (d.max_version&&d.max_version<=d.min_version))return false;
        for(unsigned j=0;j<i;++j)if(!strcmp(d.id,info.dependencies[j].id))return false;
    }
    return true;
}
void explain(loader::Plugin& p,const char* stage,const char* text,uint32_t win32=0) {
    strncpy_s(p.stage,stage,_TRUNCATE);strncpy_s(p.detail,text,_TRUNCATE);p.win32_error=win32;
}
int dependency_index(const char* id) {
    AcquireSRWLockShared(&plugin_lock);
    int found=-1;
    for(unsigned i=0;i<plugin_count;++i)if(plugins[i].inspected&&plugins[i].state!=loader::Duplicate&&
        !strcmp(plugins[i].manifest.id,id)){found=(int)i;break;}
    ReleaseSRWLockShared(&plugin_lock);return found;
}
APH_Result check_cycle(unsigned index) noexcept {
    try {
        std::vector<unsigned char> seen(plugin_count);
        std::vector<unsigned> pending{index};
        while(!pending.empty()) {
            const auto current=pending.back();pending.pop_back();
            if(seen[current])continue;seen[current]=true;
            const auto& p=plugins[current];
            if(!valid_info(p)||!valid_manifest(p.manifest))continue;
            for(unsigned i=0;i<p.info.dependency_count;++i) {
                const auto& d=p.info.dependencies[i];if(d.flags&APH_DEP_OPTIONAL)continue;
                const auto target=dependency_index(d.id);if(target<0)continue;
                if((unsigned)target==index)return APH_E_CYCLE;
                if(!seen[target])pending.push_back((unsigned)target);
            }
        }
        return APH_OK;
    } catch(const std::bad_alloc&) { return APH_E_QUEUE_FULL; }
      catch(const std::length_error&) { return APH_E_QUEUE_FULL; }
}
APH_Result dependencies_ready(loader::Plugin& p) {
    for(unsigned i=0;i<p.info.dependency_count;++i) {
        const auto& d=p.info.dependencies[i];if(d.flags&APH_DEP_OPTIONAL)continue;
        const int index=dependency_index(d.id);loader::Plugin provider{};
        APH_Result result=APH_E_DEPENDENCY;
        if(index>=0&&loader::snapshot((unsigned)index,&provider)) {
            const auto version=provider.has_info?provider.info.plugin_version:0;
            if(version<d.min_version||(d.max_version&&version>=d.max_version))result=APH_E_VERSION;
            else if(provider.state==loader::Active)continue;
        }
        char detail[256];
        if(result==APH_E_VERSION) {
            const auto v=provider.has_info?provider.info.plugin_version:0;
            _snprintf_s(detail,sizeof(detail),_TRUNCATE,
                "Requires %s >=%u.%u.%u; max-exclusive=%u.%u.%u (0=any); found=%u.%u.%u",
                d.id,(unsigned)(d.min_version>>32),(unsigned)((d.min_version>>16)&65535),(unsigned)(d.min_version&65535),
                (unsigned)(d.max_version>>32),(unsigned)((d.max_version>>16)&65535),(unsigned)(d.max_version&65535),
                (unsigned)(v>>32),(unsigned)((v>>16)&65535),(unsigned)(v&65535));
        } else _snprintf_s(detail,sizeof(detail),_TRUNCATE,"Requires %s: %s",d.id,
            index<0?"not installed":loader::state_name(provider.state));
        explain(p,"dependencies",detail);return result;
    }
    return APH_OK;
}
void enable_plugin(unsigned i,loader::Plugin p) {
    auto dependencies=dependencies_ready(p);
    if(dependencies!=APH_OK){p.state=loader::WaitingDependencies;p.result=dependencies;
        // A provider may disappear during Attach. Preserve an unstarted owner
        // as Waiting so that dependency recovery can actually resume Enable.
        hub::set_owner_state(p.owner,APH_OWNER_WAITING,APH_PENDING);save_plugin(i,p);return;}
    explain(p,"enable","");
    p.capabilities_ready=(p.manifest.required_capabilities&~hub::capabilities())==0;
    if(!p.capabilities_ready){p.state=loader::WaitingCapabilities;p.result=APH_PENDING;
        hub::set_owner_state(p.owner,APH_OWNER_WAITING,APH_PENDING);save_plugin(i,p);return;}
    uint64_t epoch=hub::begin_enable(p.owner);
    if(!epoch){p.result=APH_E_REVOKED;save_plugin(i,p);return;}
    p.state=loader::Enabling;p.result=APH_PENDING;save_plugin(i,p);
    auto enable=(APH_EnableFn)GetProcAddress(p.module,"APH_Enable");
    p.result=hub::enable(p.owner,enable,epoch);hub::finish_enable(p.owner,p.result,epoch);
    APH_OwnerStatus s{};hub::api()->owner_status(p.owner,sizeof(s),&s);
    p.result=s.result;
    p.state=s.state==APH_OWNER_ACTIVE?loader::Active:s.state==APH_OWNER_ENABLING?loader::Enabling:
        s.state==APH_OWNER_DISABLING?loader::Disabling:s.state==APH_OWNER_DISABLED?loader::Disabled:
        s.state==APH_OWNER_NEEDS_RESTART?loader::NeedsRestart:loader::Failed;
    save_plugin(i,p);
}
void load_plugin(unsigned i,bool late) {
        loader::Plugin p{}; loader::snapshot(i, &p);
        if(!p.inspected||p.state==loader::Rejected||p.state==loader::Duplicate||p.state==loader::Dependency)return;
        p.enabled = true;
        if(late)explain(p,"late-load","");
        wchar_t path[MAX_PATH];
        if(_snwprintf_s(path,_countof(path),_TRUNCATE,L"%s\\%s",plugin_directory,p.filename)<0)return;
        auto deps=dependencies_ready(p);
        if(deps!=APH_OK){p.state=loader::WaitingDependencies;p.result=deps;save_plugin(i,p);return;}
        if (late && (p.manifest.flags & (APH_PLUGIN_LIFECYCLE|APH_PLUGIN_LATE_LOAD)) != (APH_PLUGIN_LIFECYCLE|APH_PLUGIN_LATE_LOAD)) {
            p.state=loader::Disabled;p.result=APH_E_UNAVAILABLE;explain(p,"late-load","Plugin does not support first load after startup");save_plugin(i,p);return;
        }
        explain(p,"load","");p.result=APH_OK;
        bool duplicate = false;
        for (unsigned j = 0; j < plugin_count; ++j) {
            loader::Plugin previous{}; loader::snapshot(j, &previous);
            if (j != i && previous.owner && !strcmp(previous.manifest.id, p.manifest.id)) duplicate = true;
        }
        if (duplicate) { p.state = loader::Duplicate; p.result = APH_E_DUPLICATE; save_plugin(i, p); return; }
        p.module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!p.module) { const DWORD error=GetLastError();p.state=loader::LoadFailed;p.result=APH_E_UNAVAILABLE;
            wchar_t wide[256]{};char message[768]{};
            FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,error,0,wide,_countof(wide),nullptr);
            WideCharToMultiByte(CP_UTF8,0,wide,-1,message,sizeof(message),nullptr,nullptr);
            explain(p,"LoadLibraryEx",message,error);save_plugin(i,p);return; }
        p.loaded = true; p.state = loader::Loaded; save_plugin(i, p);
        // Keep every loaded module resident, including partial initialization failures.
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)p.module, &pinned)) {
            p.state = loader::NeedsRestart; p.result = APH_E_NEEDS_RESTART; save_plugin(i, p); return;
        }
        APH_Manifest actual{};
        auto manifest = (const APH_Manifest*)GetProcAddress(p.module, "APH_PluginManifest");
        auto attach = (APH_AttachFn)GetProcAddress(p.module, "APH_Attach");
        auto enable = (APH_EnableFn)GetProcAddress(p.module, "APH_Enable");
        if (!hub::copy(&actual, manifest, sizeof(actual)) || memcmp(&actual, &p.manifest, sizeof(actual)) || !attach || !enable) {
            p.state = loader::Rejected; p.result = APH_E_ABI; save_plugin(i, p); return;
        }
        if(p.has_info) {
            APH_PluginInfoData actual_info{};
            if(!hub::copy(&actual_info,GetProcAddress(p.module,"APH_PluginInfo"),sizeof(actual_info))||memcmp(&actual_info,&p.info,sizeof(p.info))) {
                p.state=loader::Rejected;p.result=APH_E_ABI;explain(p,"preflight","PluginInfo changed after mapping");save_plugin(i,p);return;
            }
        }
        p.owner = hub::add_owner(p.manifest, p.module);
        if (!p.owner) {
            const bool ownerExists=hub::find_owner(p.manifest.id)!=0;
            p.state=ownerExists?loader::Duplicate:loader::AttachFailed;
            p.result=ownerExists?APH_E_DUPLICATE:APH_E_QUEUE_FULL;
            explain(p,"owner",ownerExists?"Duplicate stable plugin ID":"Cannot allocate owner record");
            save_plugin(i,p);return;
        }
        explain(p,"attach","");
        p.result = hub::attach(p.owner, attach);
        if (p.result != APH_OK) {
            hub::fail_owner(p.owner, p.result);
            p.state = p.result == APH_E_NEEDS_RESTART ? loader::NeedsRestart : loader::AttachFailed;
            save_plugin(i, p); return;
        }
        p.attached = true; p.state = loader::Attached; save_plugin(i, p);
        enable_plugin(i,p);
}
APH_Result load_plugins() {
    InterlockedIncrement(&scan_count);
    wchar_t pattern[MAX_PATH];
    swprintf_s(pattern, L"%s\\*.dll", plugin_directory);
    WIN32_FIND_DATAW entry{};
    HANDLE find = FindFirstFileW(pattern, &entry);
    if (find == INVALID_HANDLE_VALUE) return APH_OK;
    __try {
        do {
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !_wcsicmp(entry.cFileName, L"applepie_manager.dll")) continue;
            AcquireSRWLockExclusive(&plugin_lock);
            __try {
                if (!append_plugin(entry.cFileName)) return APH_E_QUEUE_FULL;
            } __finally { ReleaseSRWLockExclusive(&plugin_lock); }
        } while (FindNextFileW(find, &entry));
    } __finally { FindClose(find); }
    // Deterministic winner for duplicate stable IDs. No plugin code during sort.
    AcquireSRWLockExclusive(&plugin_lock);
    __try {
        std::sort(plugins.begin(), plugins.end(), [](const loader::Plugin& a, const loader::Plugin& b) {
            return _wcsicmp(a.filename, b.filename) < 0;
        });
    } __finally { ReleaseSRWLockExclusive(&plugin_lock); }
    // Inspect the complete catalog before running any DLL. Required providers
    // become Active before dependent DllMain/Attach/Enable can run.
    for(unsigned i=0;i<plugin_count;++i) {
        auto p=plugins[i];wchar_t path[MAX_PATH];swprintf_s(path,L"%s\\%s",plugin_directory,p.filename);
        p.enabled=manager_config::plugin_enabled(initial_preferences,p.filename);desired[i]=p.enabled;
        p.result=preflight(path,&p);p.inspected=true;
        if(p.result==APH_E_UNAVAILABLE)p.state=loader::Dependency;
        else if(p.result!=APH_OK||!valid_manifest(p.manifest)||!valid_info(p)) {
            p.state=loader::Rejected;p.result=APH_E_ABI;explain(p,"preflight","Invalid manifest, version or dependency declaration",p.win32_error);
        } else {
            p.state=p.enabled?loader::WaitingDependencies:loader::Disabled;p.result=APH_OK;
            for(unsigned j=0;j<i;++j)if(plugins[j].manifest.id[0]&&!strcmp(plugins[j].manifest.id,p.manifest.id)) {
                p.state=loader::Duplicate;p.result=APH_E_DUPLICATE;explain(p,"preflight","Duplicate stable plugin ID");break;
            }
        }
        save_plugin(i,p);
    }
    for(unsigned i=0;i<plugin_count;++i)if(plugins[i].has_info&&plugins[i].state!=loader::Rejected&&plugins[i].state!=loader::Duplicate) {
        const auto checked=check_cycle(i);if(checked==APH_E_QUEUE_FULL)return checked;
        if(checked==APH_E_CYCLE){auto p=plugins[i];p.state=loader::Rejected;p.result=checked;
            explain(p,"dependencies","Required dependency cycle");save_plugin(i,p);}
    }
    for(unsigned pass=0;pass<plugin_count;++pass) {
        bool progress=false;
        for(unsigned i=0;i<plugin_count;++i)if(desired[i]&&!plugins[i].loaded&&plugins[i].state==loader::WaitingDependencies) {
            load_plugin(i,false);progress|=plugins[i].loaded;
        }
        if(!progress)break;
    }
    return APH_OK;
}
DWORD WINAPI management_thread(void*) {
    for(;;) {
        WaitForSingleObject(loader_wake,20);
        for(unsigned i=0;i<loader::count();++i) {
            bool want=false,pending=false;
            AcquireSRWLockShared(&plugin_lock);want=desired[i];pending=requested[i];ReleaseSRWLockShared(&plugin_lock);
            loader::Plugin p{};loader::snapshot(i,&p);
            if((p.state==loader::Active||p.state==loader::Enabling)&&p.info.dependency_count&&dependencies_ready(p)!=APH_OK) {
                // A provider may fail or be disabled through its own SDK. Close
                // dependents; service calls are revoked immediately by Core.
                p.result=hub::services::lifecycle_api()->disable(p.owner);
                if(p.result<0)hub::quarantine_owner(p.owner,APH_E_NEEDS_RESTART);
                save_plugin(i,p);
                AcquireSRWLockExclusive(&plugin_lock);desired[i]=false;requested[i]=true;ReleaseSRWLockExclusive(&plugin_lock);
                continue;
            }
            if(!pending&&p.state!=loader::WaitingCapabilities&&p.state!=loader::WaitingDependencies)continue;
            if(!want&&!p.loaded){p.state=loader::Disabled;p.result=APH_OK;save_plugin(i,p);}
            if(!p.loaded) {
                if(want&&(pending||p.state==loader::WaitingDependencies))load_plugin(i,pending);
            } else if(p.attached) {
                APH_OwnerStatus s{};hub::api()->owner_status(p.owner,sizeof(s),&s);
                if(want&&(s.state==APH_OWNER_DISABLED||s.state==APH_OWNER_WAITING))enable_plugin(i,p);
                else if(!want&&(s.state==APH_OWNER_ACTIVE||s.state==APH_OWNER_ENABLING)) {
                    p.result=hub::services::lifecycle_api()->disable(p.owner);
                    if(p.result<0)save_plugin(i,p);
                }
            }
            loader::snapshot(i,&p);
            if((want&&p.state==loader::Active)||(!want&&p.state==loader::Disabled)||p.state==loader::NeedsRestart||p.state==loader::Failed||(p.result<0&&p.state!=loader::WaitingDependencies)) {
                AcquireSRWLockExclusive(&plugin_lock);
                if((desired[i]!=0)==want)requested[i]=false;
                ReleaseSRWLockExclusive(&plugin_lock);
                manager_events::notify();
            }
        }
    }
}
DWORD WINAPI bootstrap_thread(void*) {
    // Exe-relative absolute path; current working directory never participates.
    DWORD length = GetModuleFileNameW(nullptr, plugin_directory, _countof(plugin_directory));
    if (!length || length >= _countof(plugin_directory)) {
        InterlockedExchange(&boot_result, APH_E_ARGUMENT); InterlockedExchange(&boot_state, APH_BOOT_FAILED); return 1;
    }
    wchar_t* slash = wcsrchr(plugin_directory, L'\\');
    if (!slash || size_t(slash - plugin_directory) + 48 >= _countof(plugin_directory)) {
        InterlockedExchange(&boot_result, APH_E_ARGUMENT); InterlockedExchange(&boot_state, APH_BOOT_FAILED); return 1;
    }
    wcscpy_s(slash, _countof(plugin_directory) - (slash - plugin_directory), L"\\plugin");
    APH_Result result = hub::initialize();
    InterlockedExchange(&boot_result, result);
    if (result != APH_OK) {
        hub::diagnostic("BootstrapFailed", 0, 0, result);
        InterlockedExchange(&boot_state, APH_BOOT_FAILED); return 1;
    }
    InterlockedExchange(&boot_state, APH_CORE_READY);
    hub::diagnostic("CoreReady", 0, 0, APH_OK);
    try { initial_preferences = manager_config::load(plugin_directory); }
    catch (...) { initial_preferences.load_error = ERROR_NOT_ENOUGH_MEMORY; }
    if (initial_preferences.load_error || initial_preferences.save_error) {
        char message[160];
        sprintf_s(message, "[APH] Manager config: load=%lu save=%lu\n",
            initial_preferences.load_error, initial_preferences.save_error);
        hub::log_line(message);
    }
    result=load_plugins();
    if(result!=APH_OK) {
        InterlockedExchange(&boot_result,result);InterlockedExchange(&boot_state,APH_BOOT_FAILED);
        hub::diagnostic("catalog-failed",0,0,result);return 1;
    }
    InterlockedExchange(&boot_state, APH_SCANNED);
    loader_wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    HANDLE manager=loader_wake?CreateThread(nullptr,0,management_thread,nullptr,0,nullptr):nullptr;
    if(manager)CloseHandle(manager);else hub::diagnostic("management-worker-failed",0,0,APH_E_UNAVAILABLE);
    loader::start_features(); // game initialization is independent of CoreReady
    return 0;
}
}
namespace loader {
unsigned count() {
    AcquireSRWLockShared(&plugin_lock);
    unsigned value = plugin_count;
    ReleaseSRWLockShared(&plugin_lock);
    return value;
}
bool snapshot(unsigned index, Plugin* out) {
    AcquireSRWLockShared(&plugin_lock);
    __try { if (index >= plugin_count) return false; *out = plugins[index]; }
    __finally { ReleaseSRWLockShared(&plugin_lock); }
    if (out->owner) {
        APH_OwnerStatus status{};
        if (hub::api()->owner_status(out->owner, sizeof(status), &status) == APH_OK) {
            if (status.state == APH_OWNER_NEEDS_RESTART) out->state = NeedsRestart;
            else if (status.state == APH_OWNER_FAULTED) out->state = Failed;
            else if (status.state == APH_OWNER_FAILED) out->state = Failed;
            else if(status.state==APH_OWNER_ACTIVE)out->state=Active;
            else if(status.state==APH_OWNER_DISABLING)out->state=Disabling;
            else if(status.state==APH_OWNER_DISABLED)out->state=Disabled;
            else if(status.state==APH_OWNER_ENABLING)out->state=Enabling;
            out->result=status.result;
        }
    }
    return true;
}
APH_Result request_enabled(unsigned index,bool enabled) {
    if(!loader_wake||!hub::runtime_mutations_allowed())return APH_E_UNAVAILABLE;
    if(!enabled) {
        Plugin provider{};if(!snapshot(index,&provider))return APH_E_ARGUMENT;
        for(unsigned i=0;i<count();++i) {
            Plugin dependent{};snapshot(i,&dependent);
            if(dependent.state!=Active&&dependent.state!=Enabling)continue;
            for(unsigned j=0;j<dependent.info.dependency_count;++j) {
                const auto& d=dependent.info.dependencies[j];
                if(!(d.flags&APH_DEP_OPTIONAL)&&!strcmp(d.id,provider.manifest.id)) {
                    char message[256];_snprintf_s(message,sizeof(message),_TRUNCATE,"Disable dependent %s first",dependent.manifest.id);
                    explain(provider,"disable",message);provider.result=APH_E_BUSY;save_plugin(index,provider);
                    return APH_E_BUSY;
                }
            }
        }
    }
    APH_Owner owner=0;
    AcquireSRWLockExclusive(&plugin_lock);
    __try {
        if(index>=plugin_count)return APH_E_ARGUMENT;
        Plugin& p=plugins[index];
        if(p.loaded&&!(p.manifest.flags&APH_PLUGIN_LIFECYCLE))return APH_E_UNAVAILABLE;
        if(p.state==NeedsRestart||p.state==Rejected||p.state==Duplicate||p.state==AttachFailed||p.state==LoadFailed)return APH_E_REVOKED;
        desired[index]=enabled;requested[index]=true;
        if(!enabled&&p.attached)owner=p.owner;
    } __finally {ReleaseSRWLockExclusive(&plugin_lock);}
    // The loader may still be inside APH_Enable. Close admission now rather
    // than queuing cancellation behind that callback. Disable invokes no plugin
    // code here; notification and restoration stay on their existing workers.
    if(owner) {
        APH_OwnerStatus status{};
        if(hub::api()->owner_status(owner,sizeof(status),&status)==APH_OK&&status.state==APH_OWNER_ENABLING)
            hub::services::lifecycle_api()->disable(owner);
    }
    SetEvent(loader_wake);return APH_PENDING;
}
const wchar_t* directory() { return plugin_directory; }
const manager_config::Settings& initial_config() { return initial_preferences; }
const char* state_name(uint32_t state) {
    static const char* names[] = {"Disabled", "Dependency", "Rejected", "Duplicate", "LoadFailed", "Loaded",
        "AttachFailed", "Attached", "WaitingCapabilities", "Enabling", "Active", "Failed", "NeedsRestart", "Disabling", "Queued", "WaitingDependencies"};
    return state < _countof(names) ? names[state] : "Unknown";
}
}
extern "C" APH_Result APH_CALL APH_Bootstrap(uint32_t major) {
    if (major != APH_ABI_MAJOR) return APH_E_ABI;
    if (InterlockedCompareExchange(&boot_state, APH_STARTING, APH_COLD) == APH_COLD) {
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               (LPCWSTR)&APH_Bootstrap, &self)) {
            InterlockedExchange(&boot_result, APH_E_UNAVAILABLE); InterlockedExchange(&boot_state, APH_BOOT_FAILED);
            return APH_E_UNAVAILABLE;
        }
        HANDLE thread = CreateThread(nullptr, 0, bootstrap_thread, nullptr, 0, nullptr);
        if (!thread) {
            InterlockedExchange(&boot_result, APH_E_UNAVAILABLE); InterlockedExchange(&boot_state, APH_BOOT_FAILED);
            return APH_E_UNAVAILABLE;
        }
        CloseHandle(thread);
    }
    LONG state = InterlockedCompareExchange(&boot_state, 0, 0);
    if (state == APH_BOOT_FAILED) return InterlockedCompareExchange(&boot_result, 0, 0);
    return state >= APH_CORE_READY ? APH_OK : APH_PENDING;
}
extern "C" APH_Result APH_CALL APH_GetBootStatus(uint32_t bytes, APH_BootStatus* out) {
    if (bytes < sizeof(APH_BootStatus)) return APH_E_ABI;
    APH_BootStatus status{sizeof(status), (uint32_t)InterlockedCompareExchange(&boot_state, 0, 0),
        InterlockedCompareExchange(&boot_result, 0, 0), (uint32_t)InterlockedCompareExchange(&scan_count, 0, 0),
        hub::ready() ? hub::api()->runtime_instance : 0, hub::capabilities()};
    return hub::copy(out, &status, sizeof(status)) ? APH_OK : APH_E_ARGUMENT;
}
