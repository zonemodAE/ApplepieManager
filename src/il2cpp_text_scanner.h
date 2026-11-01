#pragma once
// ============================================================================
// il2cpp_text_scanner.h — IL2CPP runtime UID/name control
// ============================================================================
// Hooks TMP_Text.set_text to hide UID/name elements across different UI views:
//   - Watermark (in-game UID)
//   - Menu (ManagerNumber / ManagerName)
//   - Business card (PlayerUidTxt / NameTxt)
// Also provides SetActive-based hide for the in-game UID panel.
// ============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <windows.h>
#include "manager_hooks.h"

// Forward decl
void Log(const char* fmt, ...);

// ============================================================================
// IL2CPP function pointer types
// ============================================================================

#define IL2CPP_DECL(ret, name, ...) \
    typedef ret (*t_##name)(__VA_ARGS__); \
    static UidRuntimeFunction<t_##name> p_##name{}

IL2CPP_DECL(void**, il2cpp_domain_get_assemblies, void*, size_t*);
IL2CPP_DECL(void*,  il2cpp_assembly_get_image, void*);
IL2CPP_DECL(const char*, il2cpp_image_get_name, void*);
IL2CPP_DECL(void*,  il2cpp_class_from_name, void*, const char*, const char*);
IL2CPP_DECL(void*,  il2cpp_class_get_method_from_name, void*, const char*, int);
IL2CPP_DECL(void*,  il2cpp_class_get_field_from_name, void*, const char*);
IL2CPP_DECL(size_t, il2cpp_field_get_offset, void*);
IL2CPP_DECL(void*,  il2cpp_runtime_invoke, void*, void*, void**, void**);
IL2CPP_DECL(void*,  il2cpp_type_get_object, void*);
IL2CPP_DECL(void*,  il2cpp_class_get_type, void*);
IL2CPP_DECL(void*,  il2cpp_string_new, const char*);
IL2CPP_DECL(uint32_t, il2cpp_gchandle_new, void*, int);
IL2CPP_DECL(void,     il2cpp_gchandle_free, uint32_t);
IL2CPP_DECL(void*,    il2cpp_gchandle_get_target, uint32_t);
IL2CPP_DECL(uint32_t, il2cpp_method_get_flags, void*, uint32_t*);
IL2CPP_DECL(uint32_t, il2cpp_method_get_param_count, void*);
IL2CPP_DECL(void*, il2cpp_method_get_return_type, void*);
IL2CPP_DECL(int, il2cpp_type_get_type, void*);
IL2CPP_DECL(void*, il2cpp_object_unbox, void*);

#undef IL2CPP_DECL

// ============================================================================
// State
// ============================================================================

static std::atomic<bool> g_il2cppResolved{false};
static void* g_methodApplicationQuit=nullptr;
static void* g_uidSetTextAddress=nullptr;
static void* g_uidGenerateAddress=nullptr;
static void* g_uidQuitAddress=nullptr;
static bool CaptureUidNative();
static HMODULE g_hGameAssembly = nullptr;

// Cached method pointers
static void* g_methodComponentGetGameObject = nullptr;
static void* g_methodComponentGetTransform = nullptr;
static void* g_methodGameObjectSetActive = nullptr;
static void* g_methodTransformGetName = nullptr;
static void* g_methodTransformGetParent = nullptr;
static void* g_methodGameObjectFind = nullptr;
static void* g_methodGameObjectGetTransform = nullptr;
static void* g_methodTransformGetLocalScale = nullptr;
static void* g_methodTransformSetLocalScale = nullptr;

// TMP_Text class and methods
static void* g_classTMP_Text = nullptr;
static void* g_methodGetText = nullptr;
static void* g_methodSetText = nullptr;
static void* g_methodGenerateTextMesh = nullptr;
static size_t g_fieldOffsetMaxVisibleCharacters = (size_t)-1;

// UID control state
static std::atomic<bool> g_uidLocated{false};
static uint32_t g_uidTextHandle=0,g_uidPingHandle=0;
static std::atomic<uint32_t> g_uidCommand{1};
static std::atomic<bool> g_uidHidden{true},g_pingHidden{false};
static std::atomic<bool> g_menuUidHidden{true},g_menuNameHidden{true},g_cardUidHidden{true},g_cardNameHidden{true};

// ============================================================================
// IL2CPP String reading
// ============================================================================

static int ReadIL2CppString(void* str, char* buf, int bufSz) {
    __try {
        if (!str) { buf[0] = 0; return -1; }
        int32_t len = *(int32_t*)((char*)str + 16);
        if (len <= 0 || len > 2048) { buf[0] = 0; return -1; }
        wchar_t* chars = (wchar_t*)((char*)str + 20);
        int result = WideCharToMultiByte(CP_UTF8, 0, chars, len, buf, bufSz - 1, NULL, NULL);
        if (result <= 0) { buf[0] = 0; return -1; }
        buf[result] = 0;
        return result;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        buf[0] = 0;
        return -2;
    }
}

// ============================================================================
// Safe IL2CPP invoke wrapper
// ============================================================================

static void* SafeInvoke(void* method, void* obj, void** params = nullptr) {
    if (!method) return nullptr;
    __try {
        void* exc = nullptr;
        void* result=p_il2cpp_runtime_invoke(method, obj, params, &exc);
        return exc?nullptr:result;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// ============================================================================
// IL2CPP Resolution
// ============================================================================

static void* FindClassInAssemblies(void** asms, size_t count,
                                    const char* ns, const char* name) {
    for (size_t i = 0; i < count; i++) {
        void* img = p_il2cpp_assembly_get_image(asms[i]);
        if (!img) continue;
        void* klass = p_il2cpp_class_from_name(img, ns, name);
        if (klass) return klass;
    }
    return nullptr;
}

static bool ResolveIL2Cpp() {
    g_hGameAssembly = GetModuleHandleW(L"GameAssembly.dll");
    if (!g_hGameAssembly) return false;

#define R(name) p_##name = (t_##name)GetProcAddress(g_hGameAssembly, #name)
    R(il2cpp_domain_get_assemblies);
    R(il2cpp_assembly_get_image);
    R(il2cpp_image_get_name);
    R(il2cpp_class_from_name);
    R(il2cpp_class_get_method_from_name);
    R(il2cpp_class_get_field_from_name);
    R(il2cpp_field_get_offset);
    R(il2cpp_runtime_invoke);
    R(il2cpp_type_get_object);
    R(il2cpp_class_get_type);
    R(il2cpp_string_new);
    R(il2cpp_gchandle_new);
    R(il2cpp_gchandle_free);R(il2cpp_gchandle_get_target);
    R(il2cpp_method_get_flags);R(il2cpp_method_get_param_count);R(il2cpp_method_get_return_type);R(il2cpp_type_get_type);
    R(il2cpp_object_unbox);
#undef R

    return p_il2cpp_domain_get_assemblies && p_il2cpp_assembly_get_image && p_il2cpp_string_new && p_il2cpp_runtime_invoke &&
           p_il2cpp_class_from_name && p_il2cpp_class_get_method_from_name &&
           p_il2cpp_gchandle_new && p_il2cpp_gchandle_free && p_il2cpp_gchandle_get_target && g_uidBootstrap.Resolve(g_hGameAssembly);
}

static bool ScanUidMetadata(void* dom) {
    size_t asmCount = 0;
    void** asms = p_il2cpp_domain_get_assemblies(dom, &asmCount);
    if (!asms || asmCount == 0) {
        Log("[AM-IL2CPP] No assemblies found");
        return false;
    }
    Log("[AM-IL2CPP] Found %d assemblies", (int)asmCount);

    // GameObject class
    void* goClass = FindClassInAssemblies(asms, asmCount, "UnityEngine", "GameObject");
    if (goClass) {
        g_methodGameObjectSetActive =
            p_il2cpp_class_get_method_from_name(goClass, "SetActive", 1);
        g_methodGameObjectFind =
            p_il2cpp_class_get_method_from_name(goClass, "Find", 1);
        g_methodGameObjectGetTransform =
            p_il2cpp_class_get_method_from_name(goClass, "get_transform", 0);
        Log("[AM-IL2CPP] GameObject.SetActive: %p, Find: %p",
            g_methodGameObjectSetActive, g_methodGameObjectFind);
    }

    // Component.get_gameObject
    void* compClass = FindClassInAssemblies(asms, asmCount, "UnityEngine", "Component");
    if (compClass) {
        g_methodComponentGetGameObject =
            p_il2cpp_class_get_method_from_name(compClass, "get_gameObject", 0);
        g_methodComponentGetTransform =
            p_il2cpp_class_get_method_from_name(compClass, "get_transform", 0);
        Log("[AM-IL2CPP] Component.get_gameObject: %p, get_transform: %p",
            g_methodComponentGetGameObject, g_methodComponentGetTransform);
    }

    // Object.get_name (reused as transform name getter)
    void* objectClass = FindClassInAssemblies(asms, asmCount, "UnityEngine", "Object");
    if (objectClass) {
        g_methodTransformGetName =
            p_il2cpp_class_get_method_from_name(objectClass, "get_name", 0);
        Log("[AM-IL2CPP] Object.get_name: %p", g_methodTransformGetName);
    }

    // Transform.get_parent
    void* trClass = FindClassInAssemblies(asms, asmCount, "UnityEngine", "Transform");
    if (trClass) {
        g_methodTransformGetParent =
            p_il2cpp_class_get_method_from_name(trClass, "get_parent", 0);
        g_methodTransformGetLocalScale =
            p_il2cpp_class_get_method_from_name(trClass, "get_localScale", 0);
        g_methodTransformSetLocalScale =
            p_il2cpp_class_get_method_from_name(trClass, "set_localScale", 1);
        Log("[AM-IL2CPP] Transform.get_parent: %p, localScale: %p/%p", g_methodTransformGetParent,
            g_methodTransformGetLocalScale, g_methodTransformSetLocalScale);
    }

    // TMP_Text class
    g_classTMP_Text = FindClassInAssemblies(asms, asmCount, "TMPro", "TMP_Text");
    if (g_classTMP_Text) {
        g_methodGetText = p_il2cpp_class_get_method_from_name(g_classTMP_Text, "get_text", 0);
        g_methodSetText = p_il2cpp_class_get_method_from_name(g_classTMP_Text, "set_text", 1);
        Log("[AM-IL2CPP] TMP_Text: class=%p get_text=%p set_text=%p",
            g_classTMP_Text, g_methodGetText, g_methodSetText);

        if (p_il2cpp_class_get_field_from_name && p_il2cpp_field_get_offset) {
            __try {
                void* maxVisibleCharactersField = p_il2cpp_class_get_field_from_name(
                    g_classTMP_Text, "m_maxVisibleCharacters");
                if (maxVisibleCharactersField) {
                    g_fieldOffsetMaxVisibleCharacters =
                        p_il2cpp_field_get_offset(maxVisibleCharactersField);
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                g_fieldOffsetMaxVisibleCharacters = (size_t)-1;
            }
        }
        if (g_fieldOffsetMaxVisibleCharacters != (size_t)-1) {
            Log("[AM-IL2CPP] TMP_Text.m_maxVisibleCharacters field resolved dynamically");
        }
        else {
            Log("[AM-IL2CPP] WARNING: TMP_Text.m_maxVisibleCharacters field not found");
        }
    } else {
        Log("[AM-IL2CPP] WARNING: TMPro.TMP_Text not found");
    }

    // Render-stage fallback. UIText inherits TextMeshProUGUI.GenerateTextMesh,
    // so this fires after late Lua/iFix field binding and before the mesh is
    // presented on the first frame.
    void* textMeshProUGUIClass =
        FindClassInAssemblies(asms, asmCount, "TMPro", "TextMeshProUGUI");
    if (textMeshProUGUIClass) {
        g_methodGenerateTextMesh = p_il2cpp_class_get_method_from_name(
            textMeshProUGUIClass, "GenerateTextMesh", 0);
        Log("[AM-IL2CPP] TextMeshProUGUI.GenerateTextMesh: %p",
            g_methodGenerateTextMesh);
    }

    void* application=FindClassInAssemblies(asms,asmCount,"UnityEngine","Application");
    if(application&&p_il2cpp_method_get_flags&&p_il2cpp_method_get_param_count&&p_il2cpp_method_get_return_type&&p_il2cpp_type_get_type) {
        void* method=p_il2cpp_class_get_method_from_name(application,"Internal_ApplicationQuit",0);uint32_t impl=0;
        if(method&&(p_il2cpp_method_get_flags(method,&impl)&0x10)&&!p_il2cpp_method_get_param_count(method)&&
            p_il2cpp_type_get_type(p_il2cpp_method_get_return_type(method))==1)g_methodApplicationQuit=method;
    }
    if(!UidRuntimeOpen()||!g_methodSetText||!CaptureUidNative())return false;
    g_il2cppResolved = true;
    Log("[AM-IL2CPP] Scanner initialized successfully");
    return true;
}

// ============================================================================
// UID Control — SetActive-based
// ============================================================================

static bool InitIL2CppScanner() {
    if(!InitializeUidOwner())return false;
    bool guarded=false,resolved=false;
    const auto deadline=GetTickCount64()+60000;
    while(GetTickCount64()<deadline&&UidRuntimeOpen()) {
        if(!resolved){resolved=ResolveIL2Cpp();if(!resolved){Sleep(50);continue;}}
        if(!guarded) {
            APH_NativeProc shutdown=(APH_NativeProc)GetProcAddress(g_hGameAssembly,"il2cpp_shutdown");
            if(!shutdown||!UidShutdown::Install(shutdown,"void() C-export","il2cpp.runtime.shutdown")) {
                hub::error_detail(g_uidOwner,"uid-startup",APH_E_UNAVAILABLE,"il2cpp_shutdown",0,0,0,0,"Cannot protect the VM shutdown boundary");
                return false;
            }
            guarded=true;
        }
        bool ready=false,scanned=false;void* attached=nullptr;
        AcquireSRWLockShared(&g_uidVmLock);g_uidVmHeld=true;
        __try {
            if(UidRuntimeOpen()&&g_uidBootstrap.Ready()) {
                ready=true;void* domain=g_uidBootstrap.AttachWorker(&attached);
                if(domain)scanned=ScanUidMetadata(domain);
            }
        } __finally {
            // Stop's original waits on this lock; detach only the thread we attached.
            g_uidBootstrap.DetachWorker(attached);g_uidVmHeld=false;ReleaseSRWLockShared(&g_uidVmLock);
        }
        if(scanned)return true;
        if(ready&&!UidRuntimeOpen())return false;
        Sleep(ready?250:50);
    }
    hub::error_detail(g_uidOwner,"uid-bootstrap",APH_E_UNAVAILABLE,"IL2CPP",0,0,0,0,"Timed out waiting for corlib, existing VM threads or TMP metadata");
    return false;
}
static void* UidFindGO(const char* path) {
    if(!UidRuntimeOpen()||!g_methodGameObjectFind)return nullptr;
    void* str=p_il2cpp_string_new(path);void* params[]={str};
    return str?SafeInvoke(g_methodGameObjectFind,nullptr,params):nullptr;
}
static bool UidSetActive(uint32_t& handle,const char* path,bool visible) {
    if(!UidRuntimeOpen()||!g_methodGameObjectSetActive)return false;
    void* go=handle?p_il2cpp_gchandle_get_target(handle):nullptr;
    if(!go) {
        if(handle)p_il2cpp_gchandle_free(handle);handle=0;
        go=UidFindGO(path);if(!go)return false;
        handle=p_il2cpp_gchandle_new(go,1);if(!handle)return false;
    }
    bool value=visible;void* params[]={&value};void* exception=nullptr;
    p_il2cpp_runtime_invoke(g_methodGameObjectSetActive,go,params,&exception);
    if(exception) {p_il2cpp_gchandle_free(handle);handle=0;return false;}
    return UidRuntimeOpen();
}
static void PumpUidCommands() {
    static uint32_t applied=0;static uint64_t retry=0;
    const uint32_t command=g_uidCommand.load(std::memory_order_acquire);
    if(command==applied&&g_uidLocated.load(std::memory_order_relaxed))return;
    const auto now=GetTickCount64();if(command==applied&&now<retry)return;
    applied=command;retry=now+1000;
    const bool uid=UidSetActive(g_uidTextHandle,"UINode/UIRoot/UIDPanelPanel/Main/TextNode/Text",!g_uidHidden.load());
    const bool ping=UidSetActive(g_uidPingHandle,"UINode/UIRoot/UIDPanelPanel/Main/TextNode/PingCon",!g_pingHidden.load());
    g_uidLocated.store(uid&&ping,std::memory_order_release);
}

// ============================================================================
// Battle HUD — zero scale hides the view only; activation, fade, animation
// state and key input stay with the game. Panels can be recreated, so hidden
// parts are checked every frame and looked up again when their object is gone.
// ============================================================================
enum { HUD_LEFT, HUD_CENTER, HUD_RIGHT, HUD_GROUPS };
struct HudVector { float x, y, z; };
struct HudPart { const char* path; int group; uint32_t handle; HudVector original; };
static HudPart g_hudParts[] = {
    {"UINode/UIRoot/SquadIconPanel/Main", HUD_LEFT},
    {"UINode/UIRoot/BattleActionPanel/HpNode", HUD_CENTER},
    {"UINode/UIRoot/BattleActionPanel/ActionsNode/AtbNode", HUD_CENTER},
    {"UINode/UIRoot/BattleActionPanel/ActionsNode/RightBottomNode", HUD_RIGHT}};
static std::atomic<bool> g_hudHidden[HUD_GROUPS]{};
static bool HudScale(void* transform, HudVector* scale) {
    // A destroyed Unity object throws inside runtime_invoke; SafeInvoke returns null.
    void* boxed = SafeInvoke(g_methodTransformGetLocalScale, transform);
    void* value = boxed ? p_il2cpp_object_unbox(boxed) : nullptr;
    if (!value) return false;
    memcpy(scale, value, sizeof(*scale));
    return true;
}
static void HudSetScale(void* transform, HudVector scale) {
    void* params[] = {&scale};
    SafeInvoke(g_methodTransformSetLocalScale, transform, params);
}
static void HudRelease(HudPart& part) { if (part.handle) p_il2cpp_gchandle_free(part.handle); part.handle = 0; }
static bool HudPending() {
    for (const auto& part : g_hudParts) if (part.handle || g_hudHidden[part.group].load(std::memory_order_relaxed)) return true;
    return false;
}
// Game thread inside the VM scope only.
static void PumpHud() {
    if (!g_methodGameObjectGetTransform || !g_methodTransformGetLocalScale || !g_methodTransformSetLocalScale || !p_il2cpp_object_unbox) return;
    static ULONGLONG nextFind = 0;
    const auto now = GetTickCount64();
    const bool find = now >= nextFind;
    for (auto& part : g_hudParts) {
        if (!UidRuntimeOpen()) return;
        const bool hide = g_hudHidden[part.group].load(std::memory_order_relaxed);
        void* transform = part.handle ? p_il2cpp_gchandle_get_target(part.handle) : nullptr;
        HudVector scale{};
        if (!transform || !HudScale(transform, &scale)) {
            HudRelease(part);
            // A recreated panel starts at its own scale. Closed panels are not
            // searched every frame.
            if (!hide || !find) continue;
            nextFind = now + 1000;
            void* go = UidFindGO(part.path);
            transform = go ? SafeInvoke(g_methodGameObjectGetTransform, go) : nullptr;
            if (!transform || !HudScale(transform, &scale)) continue;
            part.handle = p_il2cpp_gchandle_new(transform, 0);
            if (!part.handle) continue;
        }
        const bool zero = scale.x == 0 && scale.y == 0 && scale.z == 0;
        if (hide && !zero) { part.original = scale; HudSetScale(transform, {0, 0, 0}); }
        else if (!hide) {
            if (zero) HudSetScale(transform, part.original);
            HudRelease(part);
        }
    }
}
static void SetHudHidden(int group, bool hidden) { g_hudHidden[group] = hidden; }
static void SetUidVisible(bool visible) {g_uidHidden=!visible;g_uidCommand.fetch_add(1,std::memory_order_release);}
static void SetPingVisible(bool visible) {g_pingHidden=!visible;g_uidCommand.fetch_add(1,std::memory_order_release);}
static bool UidBusinessBegin() {
    if(g_uidVmHeld||!UidOnGameThread()||!UidRuntimeOpen())return false;
    AcquireSRWLockShared(&g_uidVmLock);g_uidVmHeld=true;
    if(!UidRuntimeOpen()){g_uidVmHeld=false;ReleaseSRWLockShared(&g_uidVmLock);return false;}
    return true;
}
static void UidBusinessEnd(){g_uidVmHeld=false;ReleaseSRWLockShared(&g_uidVmLock);}

// ============================================================================
// set_text Hook — cached pointer architecture
// ============================================================================

#include "manager_hooks.h"

struct MInfo { void* mp; };

typedef void* (*tNativeGetGO)(void* __this, void* methodInfo);
typedef void* (*tNativeGetName)(void* __this, void* methodInfo);
typedef void* (*tNativeGetTransform)(void* __this, void* methodInfo);
typedef void* (*tNativeGetParent)(void* __this, void* methodInfo);
typedef void (*tTMPSetText)(void* __this, void* value, void* methodInfo);
typedef void (*tTMPVoidMethod)(void* __this, void* methodInfo);

static tTMPSetText g_origSetText = nullptr;
static bool g_setTextHooked = false;
static tNativeGetGO        g_nativeGetGO = nullptr;
static tNativeGetName      g_nativeGetName = nullptr;
static tNativeGetTransform g_nativeGetTransform = nullptr;
static tNativeGetParent    g_nativeGetParent = nullptr;
static void*          g_cachedEmptyStr = nullptr;
static uint32_t g_emptyTextHandle=0;
static bool EnsureUidEmptyText() {
    if(g_emptyTextHandle)return true;
    void* value=p_il2cpp_string_new("");if(!value)return false;
    g_emptyTextHandle=p_il2cpp_gchandle_new(value,1);
    if(!g_emptyTextHandle)return false;g_cachedEmptyStr=value;return true;
}
static tTMPVoidMethod g_origGenerateTextMesh = nullptr;

// Component pointer cache — avoids native calls for known components
enum HideType {
    HT_MENU_UID = 0,
    HT_MENU_NAME,
    HT_CARD_UID,
    HT_CARD_NAME,
    HT_WATERMARK_UID
};
struct CachedComp { void* comp; HideType type; uint32_t pinnedHandle; };
static CachedComp g_compCache[16] = {};
static volatile int g_compCacheCount = 0;

static bool IsHideActive(HideType t) {
    switch (t) {
        case HT_MENU_UID:  return g_menuUidHidden;
        case HT_MENU_NAME: return g_menuNameHidden;
        case HT_CARD_UID:  return g_cardUidHidden;
        case HT_CARD_NAME: return g_cardNameHidden;
        case HT_WATERMARK_UID: return g_uidHidden;
        default: return false;
    }
}

static void CacheComp(void* comp, HideType type) {
    if (!comp) return;
    int n = g_compCacheCount;
    for (int i = 0; i < n; i++) {
        if (g_compCache[i].comp == comp) return;
    }
    if (n < 16) {
        uint32_t handle = 0;
        if (p_il2cpp_gchandle_new) {
            __try {
                handle = p_il2cpp_gchandle_new(comp, 1);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                handle = 0;
            }
        }
        if (handle == 0) return;
        g_compCache[n].comp = comp;
        g_compCache[n].type = type;
        g_compCache[n].pinnedHandle = handle;
        g_compCacheCount = n + 1;
    }
}

enum IdentityAncestorFlag {
    IAF_PERSONAL_INFO = 1 << 0,
    IAF_WATCH_PANEL   = 1 << 1,
    IAF_TEXT_NODE     = 1 << 2,
    IAF_UID_PANEL     = 1 << 3,
    IAF_PING          = 1 << 4
};

static unsigned GetIdentityAncestorFlags(void* component) {
    if (!component || !g_nativeGetTransform || !g_nativeGetParent ||
        !g_nativeGetName) {
        return 0;
    }

    unsigned flags = 0;
    __try {
        void* cur = g_nativeGetTransform(component, g_methodComponentGetTransform);
        for (int level = 0; cur && level < 16; level++) {
            cur = g_nativeGetParent(cur, g_methodTransformGetParent);
            if (!cur) break;

            void* nameStr = g_nativeGetName(cur, g_methodTransformGetName);
            if (!nameStr) continue;
            char name[48];
            if (ReadIL2CppString(nameStr, name, sizeof(name)) <= 0) continue;

            if (strcmp(name, "PersonalInfoNode") == 0)
                flags |= IAF_PERSONAL_INFO;
            else if (strcmp(name, "WatchPanel") == 0)
                flags |= IAF_WATCH_PANEL;
            else if (strcmp(name, "TextNode") == 0)
                flags |= IAF_TEXT_NODE;
            else if (strcmp(name, "UIDPanelPanel") == 0)
                flags |= IAF_UID_PANEL;
            else if (strcmp(name, "PingCon") == 0)
                flags |= IAF_PING;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return flags;
}

static bool ShouldSuppressIdentityMesh(void* component) {
    if (!component || g_hookDisabled || !UidRuntimeOpen()) return false;

    int n = g_compCacheCount;
    for (int i = 0; i < n; i++) {
        if (g_compCache[i].comp == component) {
            return IsHideActive(g_compCache[i].type);
        }
    }

    if (!g_nativeGetGO || !g_nativeGetName) return false;
    bool anyHidden = g_uidHidden || g_menuUidHidden || g_menuNameHidden ||
                     g_cardUidHidden || g_cardNameHidden;
    if (!anyHidden) return false;

    __try {
        void* go = g_nativeGetGO(component, g_methodComponentGetGameObject);
        if (!go) return false;
        void* nameStr = g_nativeGetName(go, g_methodTransformGetName);
        if (!nameStr) return false;

        char name[64];
        if (ReadIL2CppString(nameStr, name, sizeof(name)) <= 0) return false;

        if (strcmp(name, "ManagerNumber") == 0) {
            CacheComp(component, HT_MENU_UID);
            return g_menuUidHidden;
        }
        else if (strcmp(name, "ManagerName") == 0) {
            CacheComp(component, HT_MENU_NAME);
            return g_menuNameHidden;
        }
        else if (strcmp(name, "PlayerUidTxt") == 0) {
            CacheComp(component, HT_CARD_UID);
            return g_cardUidHidden;
        }

        else if (strcmp(name, "NameText") == 0) {
            unsigned ancestors = GetIdentityAncestorFlags(component);
            if (ancestors & IAF_PERSONAL_INFO) {
                CacheComp(component, HT_CARD_NAME);
                return g_cardNameHidden;
            }
            else if (ancestors & IAF_WATCH_PANEL) {
                CacheComp(component, HT_MENU_NAME);
                return g_menuNameHidden;
            }
        }
        else if (strcmp(name, "NameTxt") == 0) {
            unsigned ancestors = GetIdentityAncestorFlags(component);
            if (ancestors & IAF_PERSONAL_INFO) {
                CacheComp(component, HT_CARD_NAME);
                return g_cardNameHidden;
            }
        }
        else if (strcmp(name, "Text") == 0) {
            unsigned ancestors = GetIdentityAncestorFlags(component);
            if ((ancestors & IAF_TEXT_NODE) &&
                (ancestors & IAF_UID_PANEL) && !(ancestors & IAF_PING)) {
                CacheComp(component, HT_WATERMARK_UID);
                return g_uidHidden;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

static bool TrySetTemporaryMaxVisibleCharacters(void* component,
                                                 int value,
                                                 int* previousValue) {
    if (!component || !previousValue || g_hookDisabled || !UidRuntimeOpen()) return false;
    if (g_fieldOffsetMaxVisibleCharacters == (size_t)-1) return false;

    __try {
        int* field = (int*)((char*)component + g_fieldOffsetMaxVisibleCharacters);
        *previousValue = *field;
        *field = value;
        return true;
    } __except(APH_EntryFilter(&g_uidHooks, g_uidOwner, g_generateClaim,
                              GetExceptionInformation(), APH_ORIGINAL_NOT_ENTERED, APH_RECOVERY_NONE)) {
        return false;
    }
}

static void RestoreMaxVisibleCharacters(void* component, int previousValue) {
    if (!component) return;
    if (g_fieldOffsetMaxVisibleCharacters == (size_t)-1) return;

    // This restoration must run even if shutdown begins during GenerateTextMesh.
    // It performs only a native field write and does not call back into IL2CPP.
    __try {
        int* field = (int*)((char*)component + g_fieldOffsetMaxVisibleCharacters);
        if (InterlockedCompareExchange((volatile LONG*)field, previousValue, 0) != 0) {
            // A changed field is not ours to overwrite. Keep the owner and its
            // native resources quarantined; no successful restoration is claimed.
            g_hookDisabled = true;
            g_uidHooks.record_fault(g_uidOwner, g_generateClaim, 0xe0410001u,
                                    (uint64_t)(uintptr_t)field, APH_ORIGINAL_RETURNED, 0);
        }
    } __except(APH_EntryFilter(&g_uidHooks, g_uidOwner, g_generateClaim,
                              GetExceptionInformation(), APH_ORIGINAL_RETURNED, APH_RECOVERY_NONE)) {}
}

static void hkGenerateTextMesh(void* self, void* methodInfo) {
    APH_HookCall call{};
    APH_Result entered = g_uidHooks.enter_call(g_uidOwner, g_generateClaim, &call);
    volatile uint32_t phase = APH_ORIGINAL_NOT_ENTERED;
    bool temporary = false;
    int previous = 0;
    __try {
        __try {
            if(entered==APH_OK&&(call.flags&APH_CALL_BUSINESS)&&UidBusinessBegin()) {
                __try {PumpUidCommands();if(ShouldSuppressIdentityMesh(self))temporary=TrySetTemporaryMaxVisibleCharacters(self,0,&previous);}
                __finally {UidBusinessEnd();}
            }
            phase = APH_ORIGINAL_RUNNING;
            g_origGenerateTextMesh(self, methodInfo);
            phase = APH_ORIGINAL_RETURNED;
        } __except(APH_EntryFilter(&g_uidHooks, g_uidOwner, g_generateClaim,
                                  GetExceptionInformation(), phase, APH_RECOVERY_NONE)) {}
    } __finally {
        __try {
            if (temporary) RestoreMaxVisibleCharacters(self, previous);
        } __finally {
            if (entered == APH_OK) g_uidHooks.leave_call(g_uidOwner, &call);
        }
    }
}
static void* SelectSetTextValue(void* __this, void* value) {
    // SHUTDOWN: passthrough immediately — g_cachedEmptyStr may be freed by IL2CPP GC
    if (g_hookDisabled || !UidRuntimeOpen()) {
        return value;
    }

    // FAST PATH: cached pointer check (no native calls — deadlock-proof)
    int n = g_compCacheCount;
    for (int i = 0; i < n; i++) {
        if (g_compCache[i].comp == __this && IsHideActive(g_compCache[i].type)) {
            return g_cachedEmptyStr;
        }
    }

    // SLOW PATH: identify new components by GO name (only when game is alive)
    if (!g_hookDisabled && UidRuntimeOpen() && g_nativeGetGO && g_nativeGetName) {
        bool anyHidden = g_uidHidden || g_menuUidHidden || g_menuNameHidden ||
                         g_cardUidHidden || g_cardNameHidden;
        if (anyHidden) {
            {
                void* go = g_nativeGetGO(__this, g_methodComponentGetGameObject);
                if (go) {
                    void* nameStr = g_nativeGetName(go, g_methodTransformGetName);
                    if (nameStr) {
                        char name[64];
                        if (ReadIL2CppString(nameStr, name, sizeof(name)) > 0) {
                            if (strcmp(name, "ManagerNumber") == 0) {
                                CacheComp(__this, HT_MENU_UID);
                                if (g_menuUidHidden) {
                                    return g_cachedEmptyStr;
                                }
                            }
                            else if (strcmp(name, "ManagerName") == 0) {
                                CacheComp(__this, HT_MENU_NAME);
                                if (g_menuNameHidden) {
                                    return g_cachedEmptyStr;
                                }
                            }
                            else if (strcmp(name, "PlayerUidTxt") == 0) {
                                CacheComp(__this, HT_CARD_UID);
                                if (g_cardUidHidden) {
                                    return g_cachedEmptyStr;
                                }
                            }
                            else if (strcmp(name, "NameTxt") == 0) {
                                // Walk up ancestors looking for PersonalInfoNode (business card only)
                                bool isCard = false;
                                if (g_nativeGetTransform && g_nativeGetParent) {
                                    void* cur = g_nativeGetTransform(__this, g_methodComponentGetTransform);
                                    for (int lvl = 0; cur && lvl < 6; lvl++) {
                                        cur = g_nativeGetParent(cur, g_methodTransformGetParent);
                                        if (!cur) break;
                                        void* pName = g_nativeGetName(cur, g_methodTransformGetName);
                                        if (pName) {
                                            char pn[32];
                                            if (ReadIL2CppString(pName, pn, sizeof(pn)) > 0 &&
                                                strcmp(pn, "PersonalInfoNode") == 0) {
                                                isCard = true;
                                                break;
                                            }
                                        }
                                    }
                                }
                                if (isCard) {
                                    CacheComp(__this, HT_CARD_NAME);
                                    if (g_cardNameHidden) {
                                        return g_cachedEmptyStr;
                                    }
                                }
                            }
                            else if (strcmp(name, "NameText") == 0) {
                                unsigned ancestors = GetIdentityAncestorFlags(__this);
                                if (ancestors & IAF_PERSONAL_INFO) {
                                    CacheComp(__this, HT_CARD_NAME);
                                    if (g_cardNameHidden) {
                                        return g_cachedEmptyStr;
                                    }
                                }
                                else if (ancestors & IAF_WATCH_PANEL) {
                                    CacheComp(__this, HT_MENU_NAME);
                                    if (g_menuNameHidden) {
                                        return g_cachedEmptyStr;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return value;
}

static void hkTMPSetText(void* self, void* value, void* methodInfo) {
    APH_HookCall call{};
    APH_Result entered = g_uidHooks.enter_call(g_uidOwner, g_setTextClaim, &call);
    volatile uint32_t phase = APH_ORIGINAL_NOT_ENTERED;
    __try {
        __try {
            if(entered==APH_OK&&(call.flags&APH_CALL_BUSINESS)&&UidBusinessBegin()) {
                __try {PumpUidCommands();if(EnsureUidEmptyText())value=SelectSetTextValue(self,value);}
                __finally {UidBusinessEnd();}
            }
            phase = APH_ORIGINAL_RUNNING;
            g_origSetText(self, value, methodInfo);
            phase = APH_ORIGINAL_RETURNED;
        } __except(APH_EntryFilter(&g_uidHooks, g_uidOwner, g_setTextClaim,
                                  GetExceptionInformation(), phase, APH_RECOVERY_NONE)) {}
    } __finally {
        if (entered == APH_OK) g_uidHooks.leave_call(g_uidOwner, &call);
    }
}
// Called only while the scanner holds the VM boundary, before worker detach.
static bool CaptureUidNative() {
    if(!g_methodSetText||!g_methodComponentGetGameObject||!g_methodTransformGetName)return false;
    g_nativeGetGO=(tNativeGetGO)((MInfo*)g_methodComponentGetGameObject)->mp;
    g_nativeGetName=(tNativeGetName)((MInfo*)g_methodTransformGetName)->mp;
    if(g_methodComponentGetTransform)g_nativeGetTransform=(tNativeGetTransform)((MInfo*)g_methodComponentGetTransform)->mp;
    if(g_methodTransformGetParent)g_nativeGetParent=(tNativeGetParent)((MInfo*)g_methodTransformGetParent)->mp;
    g_uidSetTextAddress=((MInfo*)g_methodSetText)->mp;
    if(g_methodGenerateTextMesh)g_uidGenerateAddress=((MInfo*)g_methodGenerateTextMesh)->mp;
    if(g_methodApplicationQuit)g_uidQuitAddress=((MInfo*)g_methodApplicationQuit)->mp;
    return g_nativeGetGO&&g_nativeGetName&&g_uidSetTextAddress;
}
static bool InstallSetTextHook() {
    if(!UidRuntimeOpen()||!g_uidSetTextAddress)return false;
    if(g_uidQuitAddress&&!UidApplicationQuit::Install((APH_NativeProc)g_uidQuitAddress,
        "void(MethodInfo*) static","unity.Application.Internal_ApplicationQuit"))return false;
    void* mp=g_uidSetTextAddress;
    APH_NativeProc original = nullptr;
    if (!PrepareUidHook(mp, (APH_NativeProc)hkTMPSetText, 1,
                        "win64:void(void*,Il2CppString*,MethodInfo*)", &g_setTextClaim, &original)) {
        hub::fail_owner(g_uidOwner, APH_E_BACKEND); return false;
    }
    g_origSetText = (tTMPSetText)original;
    if (g_uidHooks.activate_hook(g_uidOwner, g_setTextClaim, original) < 0 || !WaitUidHook(g_setTextClaim, APH_HOOK_ACTIVE)) {
        hub::fail_owner(g_uidOwner, APH_E_BACKEND); return false;
    }
    g_setTextHooked = true;
    if (g_uidGenerateAddress && g_fieldOffsetMaxVisibleCharacters != (size_t)-1) {
        void* target = g_uidGenerateAddress;
        if (target && target != mp) {
            if (!PrepareUidHook(target, (APH_NativeProc)hkGenerateTextMesh, 2,
                                "win64:void(void*,MethodInfo*)", &g_generateClaim, &original)) {
                hub::fail_owner(g_uidOwner, APH_E_BACKEND); return false;
            }
            g_origGenerateTextMesh = (tTMPVoidMethod)original;
            if (g_uidHooks.activate_hook(g_uidOwner, g_generateClaim, original) < 0 || !WaitUidHook(g_generateClaim, APH_HOOK_ACTIVE)) {
                hub::fail_owner(g_uidOwner, APH_E_BACKEND); return false;
            }
        }
    }
    hub::finish_enable(g_uidOwner, APH_OK);
    Log("[AM-UID] UID hooks prepared and activated through Core");
    return true;
}

// ============================================================================
// Toggle functions (hook handles blocking by cached pointer + GO name)
// ============================================================================

static void SetMenuUidVisible(bool visible)  { g_menuUidHidden = !visible; }
static void SetMenuNameVisible(bool visible) { g_menuNameHidden = !visible; }
static void SetCardUidVisible(bool visible)  { g_cardUidHidden = !visible; }
static void SetCardNameVisible(bool visible) { g_cardNameHidden = !visible; }
