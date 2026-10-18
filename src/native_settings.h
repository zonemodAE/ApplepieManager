#pragma once
#include "hub_internal.h"
#include "manager_events.h"
#include "config_key_input.h"
#include "../sdk/applepie_endfield_adapters.h"
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <winver.h>

extern manager_config::Settings g_managerConfig;
namespace native_settings {
static constexpr char Prefix[] = "@ApplepieMenu/v1/";
static APH_Hook loadClaim;
using MenuUpdate = applepie::endfield::GameAppUpdate<111>;
static applepie::Subscription updateObserver;
static void* (*loadOriginal)(void*, void*, void*);
static void (*staticFieldValue)(void*, void*);
static void* (*fieldType)(void*);
static uint32_t (*fieldFlags)(void*);
static bool (*isLuaGCing)(void*);
static void* luaInstanceField;
static void* doStringMethod;
static void *luaKeyMethod, *encryptMethod, *decryptMethod, *stringClass;
static uintptr_t (*arrayLength)(void*);
static void* (*objectClass)(void*);
static uint32_t arrayHeaderBytes;
static void* (*methods)(void*, void**);
static const char* (*methodName)(void*);
static void* (*methodParam)(void*, uint32_t);
static char* (*typeName)(void*);
static void (*freeMemory)(void*);
static int32_t (*stringLength)(void*);
static const wchar_t* (*stringChars)(void*);
static void (*formatException)(void*, char*, int);
static APH_WindowApi windows{};
static APH_Route windowRoute;
static std::string source, message;
static bool installed, failed, visible, english, keyFinished;
static int selected = -1;
static ULONGLONG nextAttempt;
static bool encodingChecked;
static std::string startupStatus;
static bool updateObserved, scopeObserved;
static __declspec(thread) bool pumping;
// Window notifications only record a key. Saving and Lua calls run after Update.
static std::atomic<int> captureResult{0}; // -2 waiting, -1 cancelled, >0 VK
static unsigned capturePlugin, captureField;
static uint64_t captureRevision;
static std::mutex preferenceLock;
static manager_config::Settings queuedPreferences;
static bool preferenceWorker, preferenceQueued;
static std::atomic<DWORD> preferenceError{0};

static std::string quote(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 32 || c == 127) { char escaped[5]; sprintf_s(escaped, "\\%03u", unsigned(c)); out += escaped; }
        else out += char(c);
    }
    return out + '"';
}
static std::string_view resourceData(HMODULE module, unsigned id) {
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    const auto handle = resource ? LoadResource(module, resource) : nullptr;
    const auto bytes = resource ? SizeofResource(module, resource) : 0;
    const auto contents = handle ? static_cast<const char*>(LockResource(handle)) : nullptr;
    return contents ? std::string_view(contents, bytes) : std::string_view{};
}
static std::string base64(std::string_view bytes) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out((bytes.size() + 2) / 3 * 4, '=');
    for (size_t i = 0, j = 0; i < bytes.size(); i += 3, j += 4) {
        const uint32_t value = (uint32_t(static_cast<unsigned char>(bytes[i])) << 16)
            | (i + 1 < bytes.size() ? uint32_t(static_cast<unsigned char>(bytes[i + 1])) << 8 : 0)
            | (i + 2 < bytes.size() ? uint32_t(static_cast<unsigned char>(bytes[i + 2])) : 0);
        out[j] = alphabet[value >> 18];
        out[j + 1] = alphabet[(value >> 12) & 63];
        if (i + 1 < bytes.size()) out[j + 2] = alphabet[(value >> 6) & 63];
        if (i + 2 < bytes.size()) out[j + 3] = alphabet[value & 63];
    }
    return out;
}
static std::string utf8(const wchar_t* s, int length) {
    int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, length, nullptr, 0, nullptr, nullptr);
    if (!bytes && length) throw std::runtime_error("Invalid UTF-16");
    std::string result(bytes, '\0');
    if (bytes) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, length, result.data(), bytes, nullptr, nullptr);
    return result;
}
static std::string productVersion(HMODULE module) {
    std::vector<wchar_t> path(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (!length) return {};
        if (length < path.size()) break;
        path.resize(path.size() * 2);
    }
    const DWORD bytes = GetFileVersionInfoSizeW(path.data(), nullptr);
    if (!bytes) return {};
    std::vector<unsigned char> data(bytes);
    if (!GetFileVersionInfoW(path.data(), 0, bytes, data.data())) return {};
    void* value = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\StringFileInfo\\040904b0\\ProductVersion", &value, &length) || !length) return {};
    const auto text = static_cast<const wchar_t*>(value);
    if (text[length - 1] != L'\0') return {};
    return utf8(text, static_cast<int>(length - 1));
}
static std::string managedString(void* value) {
    if (!value) return {};
    const int count = stringLength(value);
    if (count < 0 || count > 2048) return {};
    return utf8(stringChars(value), count);
}
static void result(APH_Result r) {
    message = r < 0 ? std::string(APH_ResultName(r)) + " (" + std::to_string(r) + ")" : "";
    manager_events::notify();
}
static DWORD WINAPI savePreferences(void*) {
    try {
    for (;;) {
        manager_config::Settings cfg;
        {
            std::lock_guard<std::mutex> guard(preferenceLock);
            if (!preferenceQueued) { preferenceWorker = false; return 0; }
            cfg = queuedPreferences; preferenceQueued = false;
        }
        DWORD error = 0;
        try {
            error = manager_config::save(cfg);
        } catch (...) { error = ERROR_NOT_ENOUGH_MEMORY; }
        preferenceError.store(error, std::memory_order_release);
        manager_events::notify();
        if (error) Log("[AM-Menu] Manager config save failed: win32=%lu", error);
    }
    } catch (...) {
        std::lock_guard<std::mutex> guard(preferenceLock);
        preferenceWorker = false;
        preferenceError.store(ERROR_NOT_ENOUGH_MEMORY);
        manager_events::notify();
        return 1;
    }
}
static void persistPreferences() {
    g_managerConfig.values["language"] = english ? "en" : "zh";
    const char* names[] = {"hide_uid", "hide_ping", "hide_menu_uid", "hide_menu_name", "hide_card_uid", "hide_card_name"};
    const bool values[] = {g_uidHidden, g_pingHidden, g_menuUidHidden, g_menuNameHidden, g_cardUidHidden, g_cardNameHidden};
    for (unsigned i = 0; i < 6; ++i) g_managerConfig.values[names[i]] = values[i] ? "true" : "false";
    for (unsigned i = 0; i < loader::count(); ++i) {
        loader::Plugin p{}; if (!loader::snapshot(i, &p)) continue;
        const auto key = manager_config::plugin_key(p.filename);
        if (!key.empty()) g_managerConfig.values[key] = g_pluginEnabled[i] ? "true" : "false";
    }
    std::lock_guard<std::mutex> guard(preferenceLock);
    queuedPreferences = g_managerConfig; preferenceQueued = true;
    if (!preferenceWorker) {
        preferenceWorker = true;
        if (!QueueUserWorkItem(savePreferences, nullptr, WT_EXECUTEDEFAULT)) {
            preferenceWorker = false; preferenceError.store(GetLastError());
        }
    }
    manager_events::notify();
}
static std::string configData(const loader::Plugin& p) {
    const auto api = hub::services::config_api();
    APH_ConfigSnapshot state{};
    if (!p.owner || api->snapshot(p.owner, sizeof(state), &state, 0, nullptr) != APH_OK) return "nil";
    std::vector<APH_ConfigValue> values(state.field_count);
    if (api->snapshot(p.owner, sizeof(state), &state, uint32_t(values.size()), values.data()) != APH_OK) return "nil";
    std::string out = "{revision=" + quote(std::to_string(state.revision)) + ",pending=" + (state.pending ? "true" : "false") +
        ",error=" + quote(state.last_result < 0 ? std::string(APH_ResultName(state.last_result)) + " / Win32 " + std::to_string(state.win32_error) : "") + ",fields={";
    for (unsigned i = 0; i < values.size(); ++i) {
        APH_ConfigField field{}; if (api->field(p.owner, i, sizeof(field), &field) != APH_OK) continue;
        auto& value = values[i];
        std::string literal;
        if (field.type == APH_CONFIG_FLOAT) { char s[64]; auto r = std::to_chars(s, s + sizeof(s), value.number); literal.assign(s, r.ptr); }
        else if (field.type == APH_CONFIG_TEXT || field.type == APH_CONFIG_ENUM) literal = quote(value.text);
        else literal = std::to_string(value.integer);
        out += "{index=" + std::to_string(i) + ",type=" + std::to_string(field.type) + ",label=" + quote(field.label) +
            ",description=" + quote(field.description) + ",value=" + literal + ",display=" + quote(ConfigKeyName(value.integer, !english)) + ",options={";
        for (unsigned j = 0; j < field.option_count; ++j) {
            APH_ConfigOption option{};
            if (api->option(p.owner, i, j, sizeof(option), &option) == APH_OK) out += quote(option.value) + ",";
        }
        out += "}},";
    }
    return out + "}}";
}
static std::string pluginData(unsigned index, bool full) {
    loader::Plugin p{}; if (!loader::snapshot(index, &p)) return "nil";
    const auto filename = utf8(p.filename, int(wcslen(p.filename)));
    const bool active = p.state == loader::Active || p.state == loader::Enabling || p.state == loader::Queued || p.state == loader::WaitingDependencies;
    const bool canToggle = p.inspected && p.state != loader::Rejected && p.state != loader::Duplicate && p.state != loader::Dependency &&
        p.state != loader::LoadFailed && p.state != loader::AttachFailed && p.state != loader::NeedsRestart && p.state != loader::Disabling &&
        (!p.loaded || (p.manifest.flags & APH_PLUGIN_LIFECYCLE));
    std::string name = p.manifest.display_name[0] ? p.manifest.display_name : filename;
    std::string out = "{index=" + std::to_string(index) + ",name=" + quote(name) + ",state=" + quote(loader::state_name(p.state)) +
        ",active=" + (active ? "true" : "false") + ",canToggle=" + (canToggle ? "true" : "false");
    if (full) {
        std::string detail = filename + "\n" + p.manifest.id + "\n" + loader::state_name(p.state) + " / " + APH_ResultName(p.result) +
            "\nLoaded=" + (p.loaded ? "1" : "0") + ", Attached=" + (p.attached ? "1" : "0") + ", Capabilities=" + (p.capabilities_ready ? "1" : "0") +
            "\n" + p.stage + "\n" + p.detail;
        if (p.has_info) {
            const auto version = p.info.plugin_version;
            detail += "\nv" + std::to_string((version >> 32) & 0xffff) + "." + std::to_string((version >> 16) & 0xffff) + "." + std::to_string(version & 0xffff);
            for (unsigned i = 0; i < p.info.dependency_count; ++i) detail += "\nDependency: " + std::string(p.info.dependencies[i].id);
        }
        APH_ErrorDetail error{};
        if (p.owner && hub::services::diagnostics_api()->last_error(p.owner, sizeof(error), &error) == APH_OK) {
            detail += "\n" + std::string(error.operation) + " / " + error.resource + "\n" + error.message + "\nWin32=" + std::to_string(error.win32_error) + ", backend=" + std::to_string(error.backend_status);
            char address[80]; sprintf_s(address, "\ntarget=0x%llx, hook=%llu", (unsigned long long)error.target, (unsigned long long)error.hook); detail += address;
        }
        out += ",details=" + quote(detail) + ",config=" + configData(p);
    }
    return out + "}";
}
static std::string snapshot() {
    const DWORD ioError = preferenceError.load(std::memory_order_acquire);
    std::string out = "{page=" + std::to_string(selected) + ",chinese=" + (!english ? "true" : "false") +
        ",keyFinished=" + (keyFinished ? "true" : "false") + ",message=" + quote(ioError ? "Manager config: Win32 " + std::to_string(ioError) : message) + ",plugins={";
    keyFinished = false;
    for (unsigned i = 0; i < loader::count(); ++i) out += pluginData(i, false) + ",";
    out += "},plugin=" + (selected >= 0 ? pluginData(unsigned(selected), true) : "nil") + ",preferences={";
    const char* names[] = {"hide_uid", "hide_ping", "hide_menu_uid", "hide_menu_name", "hide_card_uid", "hide_card_name"};
    const bool values[] = {g_uidHidden, g_pingHidden, g_menuUidHidden, g_menuNameHidden, g_cardUidHidden, g_cardNameHidden};
    for (unsigned i = 0; i < 6; ++i) out += std::string(names[i]) + "=" + (values[i] ? "true," : "false,");
    return out + "}}";
}
template<class T> static T number(const std::string& s) {
    T value{}; auto r = std::from_chars(s.data(), s.data() + s.size(), value);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size()) throw std::runtime_error("Invalid number");
    return value;
}
static std::string unhex(const std::string& s) {
    if (s.size() % 2 || s.size() > (APH_CONFIG_TEXT_BYTES - 1) * 2) throw std::runtime_error("Invalid setting value length");
    std::string out;
    for (size_t i = 0; i < s.size(); i += 2) {
        unsigned c = 0; auto r = std::from_chars(s.data() + i, s.data() + i + 2, c, 16);
        if (r.ec != std::errc() || r.ptr != s.data() + i + 2 || !c) throw std::runtime_error("Invalid setting encoding");
        out += char(c);
    }
    return out;
}
static APH_Result saveField(unsigned index, uint64_t revision, unsigned fieldIndex, const std::string& value, bool defaults = false) {
    loader::Plugin p{}; if (!loader::snapshot(index, &p) || !p.owner) return APH_E_UNAVAILABLE;
    const auto api = hub::services::config_api();
    APH_ConfigSnapshot state{};
    auto r = api->snapshot(p.owner, sizeof(state), &state, 0, nullptr); if (r != APH_OK) return r;
    if (state.pending) return APH_E_BUSY;
    if (state.revision != revision) return APH_E_STALE;
    std::vector<APH_ConfigValue> values(state.field_count);
    r = api->snapshot(p.owner, sizeof(state), &state, uint32_t(values.size()), values.data()); if (r != APH_OK) return r;
    if (!defaults && fieldIndex >= values.size()) return APH_E_ARGUMENT;
    for (unsigned i = 0; i < values.size(); ++i) {
        if (!defaults && i != fieldIndex) continue;
        APH_ConfigField field{}; r = api->field(p.owner, i, sizeof(field), &field); if (r != APH_OK) return r;
        if (defaults) values[i] = field.initial;
        else if (field.type == APH_CONFIG_FLOAT) values[i].number = number<double>(value);
        else if (field.type == APH_CONFIG_TEXT || field.type == APH_CONFIG_ENUM) {
            if (value.size() >= sizeof(values[i].text)) return APH_E_ARGUMENT;
            strcpy_s(values[i].text, value.c_str());
        } else values[i].integer = number<int32_t>(value);
    }
    return api->save(p.owner, revision, uint32_t(values.size()), values.data());
}
static void APH_CALL windowEvent(APH_Owner, const APH_WindowEvent* event, void*) {
    if (event->phase == APH_WINDOW_AFTER && event->message == WM_NCDESTROY) { StopUidRuntime(); return; }
    if (event->phase != APH_WINDOW_BEFORE || captureResult.load() != -2) return;
    const auto msg = event->message;
    int value = 0;
    if (msg == WM_KILLFOCUS || (msg == WM_ACTIVATEAPP && !event->wparam)) value = -1;
    else if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(event->lparam & (INT64_C(1) << 30)) && event->wparam > 0 && event->wparam < 256)
        value = event->wparam == VK_ESCAPE ? -1 : int(event->wparam);
    if (value) { captureResult.store(value); manager_events::notify(); }
}
static bool ensureWindowRoute() {
    if (windowRoute) {
        APH_WindowStatus state{};
        return windows.query(g_uidOwner, windowRoute, sizeof(state), &state) == APH_OK && state.state == APH_WINDOW_BOUND;
    }
    HWND target = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM arg)->BOOL {
        wchar_t name[64]{};
        if (GetClassNameW(window, name, 64) && !wcscmp(name, L"UnityWndClass")) { *reinterpret_cast<HWND*>(arg) = window; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&target));
    if (!target) return false;
    APH_WindowRequest request{sizeof(request), APH_RECOVERY_NONE, uint64_t(uintptr_t(target)), windowEvent, nullptr};
    if (windows.subscribe(g_uidOwner, &request, &windowRoute) < 0) return false;
    APH_WindowStatus state{};
    return windows.query(g_uidOwner, windowRoute, sizeof(state), &state) == APH_OK && state.state == APH_WINDOW_BOUND;
}
static void command(const std::string& path) {
    try {
        std::vector<std::string> parts;
        size_t from = sizeof(Prefix) - 1;
        for (;;) { auto slash = path.find('/', from); parts.push_back(path.substr(from, slash - from)); if (slash == std::string::npos) break; from = slash + 1; }
        const auto& verb = parts.at(0);
        if (verb == "session-ended" && parts.size() == 1) {
            // Lua state, panel references and callbacks belong to one game
            // session. Returning to login does not unload this DLL.
            installed = failed = visible = keyFinished = encodingChecked = false;
            selected = -1; captureResult = 0;
            capturePlugin = captureField = 0; captureRevision = 0;
            message.clear(); startupStatus.clear(); nextAttempt = 0;
            manager_events::notify();
            Log("[AM-Menu] Lua session ended; menu registration will restart in the next environment");
            return;
        }
        if (verb == "ready" && parts.size() == 1) { installed = true; Log("[AM-Menu] Native settings bridge ready; panel hooks verified"); return; }
        if (verb == "tab-ready" && parts.size() == 1) { Log("[AM-Menu] Applepie tab attached to GameSetting panel"); return; }
        if (verb == "panel-error" && parts.size() == 2) { Log("[AM-Menu] Panel integration failed: %s", unhex(parts[1]).c_str()); return; }
        if (verb == "icon-error" && parts.size() == 2) { Log("[AM-Menu] Tab icon unavailable: %s", unhex(parts[1]).c_str()); return; }
        if (verb == "close" && parts.size() == 1) { visible = false; captureResult = 0; return; }
        if (verb == "cancel-key" && parts.size() == 1) { captureResult = 0; return; }
        if (verb == "open" && parts.size() == 2) {
            int index = number<int>(parts[1]);
            if (index < -2 || (index >= 0 && unsigned(index) >= loader::count())) throw std::runtime_error("Unknown plugin");
            visible = true; selected = index; message.clear(); captureResult = 0; manager_events::notify(); return;
        }
        if (!visible) return;
        if (verb == "refresh" && parts.size() == 1) { manager_events::notify(); return; }
        message.clear();
        if (verb == "enable" && parts.size() == 3) {
            const auto index = number<unsigned>(parts[1]); const auto on = number<unsigned>(parts[2]);
            if (index >= loader::count() || on > 1) throw std::runtime_error("Invalid plugin switch");
            const auto requested = loader::request_enabled(index, on != 0);
            result(requested);
            // One switch controls the session and the next launch. A rejected
            // request must not silently change the saved startup preference.
            if (requested >= 0) { g_pluginEnabled[index] = on != 0; persistPreferences(); }
        } else if (verb == "set" && parts.size() == 5) {
            result(saveField(number<unsigned>(parts[1]), number<uint64_t>(parts[2]), number<unsigned>(parts[3]), unhex(parts[4])));
        } else if (verb == "defaults" && parts.size() == 3) {
            result(saveField(number<unsigned>(parts[1]), number<uint64_t>(parts[2]), 0, "", true));
        } else if (verb == "key" && parts.size() == 4) {
            capturePlugin = number<unsigned>(parts[1]); captureRevision = number<uint64_t>(parts[2]); captureField = number<unsigned>(parts[3]);
            loader::Plugin p{}; APH_ConfigField field{};
            if (!loader::snapshot(capturePlugin, &p) || hub::services::config_api()->field(p.owner, captureField, sizeof(field), &field) != APH_OK || field.type != APH_CONFIG_KEY || !ensureWindowRoute())
                throw std::runtime_error("Keyboard capture unavailable");
            captureResult = -2;
        } else if (verb == "preference" && parts.size() == 3) {
            const auto& key = parts[1]; const auto& value = parts[2];
            if (key == "language") { if (value != "en" && value != "zh") throw std::runtime_error("Invalid language"); english = value == "en"; }
            else {
                if (value != "0" && value != "1") throw std::runtime_error("Invalid switch");
                bool on = value == "1";
                if (key == "hide_uid") SetUidVisible(!on);
                else if (key == "hide_ping") SetPingVisible(!on);
                else if (key == "hide_menu_uid") g_menuUidHidden = on;
                else if (key == "hide_menu_name") g_menuNameHidden = on;
                else if (key == "hide_card_uid") g_cardUidHidden = on;
                else if (key == "hide_card_name") g_cardNameHidden = on;
                else throw std::runtime_error("Unknown manager setting");
            }
            persistPreferences();
        } else throw std::runtime_error("Unknown menu command");
    } catch (const std::exception& e) {
        message = e.what();
        if (path.compare(sizeof(Prefix) - 1, 4, "key/") == 0) { captureResult = 0; keyFinished = true; }
        manager_events::notify();
    }
}

// Helpers run inside the game-thread VM boundary. No managed roots survive Update.
struct ManagedRoot {
    uint32_t handle;
    explicit ManagedRoot(void* object) : handle(object ? p_il2cpp_gchandle_new(object, 0) : 0) {
        if (!handle) throw std::runtime_error("Native menu: managed allocation/root failed");
    }
    ~ManagedRoot() { p_il2cpp_gchandle_free(handle); }
    ManagedRoot(const ManagedRoot&) = delete;
    ManagedRoot& operator=(const ManagedRoot&) = delete;
    void* get() const { return p_il2cpp_gchandle_get_target(handle); }
};
static void* invoke(void* method, void* self, void** args, const char* stage) {
    void* exception = nullptr;
    void* value = p_il2cpp_runtime_invoke(method, self, args, &exception);
    if (!exception) return value;
    ManagedRoot error(exception);
    char description[2048] = "Managed exception";
    if (formatException) formatException(error.get(), description, sizeof(description));
    throw std::runtime_error(std::string(stage) + ": " + description);
}
static void menuFailure(const char* description) {
    failed = true;
    Log("[AM-Menu] %s", description);
    hub::error_detail(g_uidOwner, "native-menu", APH_E_UNAVAILABLE, "GameSettingCtrl", 0, 0, 0, 0, description);
}
static void runLua(void* manager, const std::string& code, std::string* status = nullptr) {
    ManagedRoot text(p_il2cpp_string_new(code.c_str()));
    // This client decrypts *all* DoString input, including C# strings. A plain
    // chunk is rejected inside DecryptionFunc without a runtime_invoke exception.
    // Resolve the current game key/encoder instead of hardcoding a resource key
    // or changing the VM's global decryption setting.
    ManagedRoot key(invoke(luaKeyMethod, nullptr, nullptr, "LuaCypher.GetKey"));
    if (stringLength(key.get()) <= 0) throw std::runtime_error("LuaCypher.GetKey returned an empty key");
    void* packArgs[]{text.get(), key.get()};
    ManagedRoot packed(invoke(encryptMethod, nullptr, packArgs, "XXTEA.Encode"));
    if (stringLength(packed.get()) <= 0) throw std::runtime_error("Lua script encoding returned no data");
    if (!encodingChecked) {
        void* decodeArgs[]{packed.get()};
        ManagedRoot decoded(invoke(decryptMethod, nullptr, decodeArgs, "LuaCypher.Verify"));
        const int length = stringLength(text.get());
        if (length != stringLength(decoded.get()) || memcmp(stringChars(text.get()), stringChars(decoded.get()), size_t(length) * sizeof(wchar_t)))
            throw std::runtime_error("Lua script encoding does not match the current game decoder");
        encodingChecked = true;
        Log("[AM-Menu] Game Lua encoding round-trip verified");
    }
    void* args[]{packed.get()};
    void* output = invoke(doStringMethod, manager, args, "LuaManager.DoString");
    if (!status) return;
    if (!output) { *status = "waiting:LuaEnv"; return; }
    ManagedRoot returned(output);
    if (arrayLength(returned.get()) != 1) throw std::runtime_error("Menu bootstrap returned no status; script did not complete");
    // Use the runtime's array header size, not a guessed object layout. DoString
    // is ABI-checked to return Object[] and our bootstrap returns one string.
    void* first = *reinterpret_cast<void**>(static_cast<char*>(returned.get()) + arrayHeaderBytes);
    if (!first || objectClass(first) != stringClass) throw std::runtime_error("Menu bootstrap returned an invalid status");
    *status = managedString(first);
}
static void startupWaiting(const std::string& status) {
    if (status != startupStatus) { startupStatus = status; Log("[AM-Menu] Menu bootstrap: %s", status.c_str()); }
}
static void pump() {
    try {
        // Resolve an existing instance only when initialization or a UI event has
        // work. Never invoke a generic singleton getter or retain a raw instance.
        if (!installed) nextAttempt = GetTickCount64() + 1000;
        void* instance = nullptr;
        staticFieldValue(luaInstanceField, &instance);
        if (!instance) { if (!installed) startupWaiting("waiting:LuaManager.instance"); return; }
        ManagedRoot manager(instance);
        // Lua GC can run asynchronously in this client. Leave events queued while
        // it is running instead of entering Lua or blocking the game thread.
        if (isLuaGCing(nullptr)) return;
        if (!installed) {
            std::string status;
            runLua(manager.get(), source, &status);
            startupWaiting(status);
            if (status == "ready" && !installed) throw std::runtime_error("Menu script loaded but the native command bridge did not acknowledge it");
            if (status != "ready" && status.compare(0, 8, "waiting:") != 0) throw std::runtime_error("Menu bootstrap returned an unknown status");
            if (!installed) return;
            ensureWindowRoute();
        }
        const int key = captureResult.load();
        if (key == -1 || key > 0) {
            captureResult = 0; keyFinished = true;
            if (key > 0 && visible) result(saveField(capturePlugin, captureRevision, captureField, std::to_string(key)));
            manager_events::notify();
        }
        if (!manager_events::changed.exchange(false, std::memory_order_acq_rel) || !visible) return;
        runLua(manager.get(), "assert(_G.ApplepieNativeSettings, 'Native settings script disappeared').OnEvent(" + snapshot() + ")");
    } catch (const std::exception& e) { menuFailure(e.what()); }
}
static int beginScope() {
    if (!UidRuntimeOpen() || !UidOnGameThread()) return 0;
    if (g_uidVmHeld) return 2;
    AcquireSRWLockShared(&g_uidVmLock); g_uidVmHeld = true;
    if (!UidRuntimeOpen()) { g_uidVmHeld = false; ReleaseSRWLockShared(&g_uidVmLock); return 0; }
    return 1;
}
static void endScope(int scope) { if (scope == 1) { g_uidVmHeld = false; ReleaseSRWLockShared(&g_uidVmLock); } }
// C++ containers and managed operations live in helpers, outside the thin SEH
// entries. Original functions execute once; native faults never replay them.
static bool dispatchPath(void* path) noexcept {
    try {
        auto name = managedString(path);
        if (name.compare(0, sizeof(Prefix) - 1, Prefix) != 0) return false;
        // A failed old environment must still be able to end its session.
        if (!failed || name == "@ApplepieMenu/v1/session-ended") command(name);
        return true;
    } catch (const std::exception& e) {
        Log("[AM-Menu] Command decoding failed: %s", e.what());
        return false;
    }
}
static void* loadEntry(void* self, void* path, void* method) {
    APH_HookCall call{}; const auto admitted = g_uidHooks.enter_call(g_uidOwner, loadClaim, &call);
    volatile uint32_t phase = APH_ORIGINAL_NOT_ENTERED;
    int scope = 0; void* out = nullptr;
    __try {
        __try {
            if (admitted == APH_OK && (call.flags & APH_CALL_BUSINESS)) {
                scope = beginScope();
                if (scope && dispatchPath(path)) __leave;
                endScope(scope); scope = 0;
            }
            phase = APH_ORIGINAL_RUNNING; out = loadOriginal(self, path, method); phase = APH_ORIGINAL_RETURNED;
        } __except(APH_EntryFilter(&g_uidHooks, g_uidOwner, loadClaim, GetExceptionInformation(), phase, APH_RECOVERY_NONE)) {}
    } __finally { endScope(scope); if (admitted == APH_OK) g_uidHooks.leave_call(g_uidOwner, &call); }
    return out;
}
static APH_Result APH_CALL onUpdate(APH_Owner, const APH_SharedEvent* event, void*) {
    if (!event || event->phase != APH_AFTER || event->payload_bytes != sizeof(APH_EfVoid)) return APH_E_ARGUMENT;
    int scope = 0;
    bool ownsPump = false;
    // The shared provider calls original exactly once and supplies the native
    // fault boundary. This subscriber never installs a competing Update hook.
    __try {
        if (!updateObserved) {
            updateObserved = true;
            Log("[AM-Menu] Shared GameApp.Update reached: thread=%lu", GetCurrentThreadId());
        }
        if (!failed && !pumping &&
            (installed ? manager_events::changed.load(std::memory_order_acquire) : GetTickCount64() >= nextAttempt)) {
            scope = beginScope();
            if (scope) {
                if (!scopeObserved) { scopeObserved = true; Log("[AM-Menu] Game-thread menu dispatch ready"); }
                pumping = ownsPump = true;
                pump();
            }
        }
    } __finally { if (ownsPump) pumping = false; endScope(scope); }
    return APH_OK;
}
static bool namedType(void* type, const char* expected) {
    char* name = typeName(type); if (!name) return false;
    bool matches = !strcmp(name, expected); freeMemory(name); return matches;
}
static void* findMethod(void* klass, const char* name, const char* returns, std::initializer_list<const char*> parameters, bool isStatic = false) {
    if (!klass) return nullptr;
    void* iterator = nullptr; void* match = nullptr;
    for (void* method; (method = methods(klass, &iterator)) != nullptr;) {
        if (strcmp(methodName(method), name)) continue;
        uint32_t impl = 0;
        if (!!(p_il2cpp_method_get_flags(method, &impl) & 0x10) != isStatic || p_il2cpp_method_get_param_count(method) != parameters.size() ||
            !namedType(p_il2cpp_method_get_return_type(method), returns)) continue;
        bool matches = true; uint32_t index = 0;
        for (const auto parameter : parameters) matches &= namedType(methodParam(method, index++), parameter);
        if (!matches) continue;
        if (match) return nullptr; match = method;
    }
    return match;
}
static bool resolve(void* domain, void** loadTarget, void** updateTarget) {
#define MENU_EXPORT(name, exportName) name = reinterpret_cast<decltype(name)>(GetProcAddress(g_hGameAssembly, exportName)); if (!name) return false
    MENU_EXPORT(methods, "il2cpp_class_get_methods"); MENU_EXPORT(methodName, "il2cpp_method_get_name");
    MENU_EXPORT(methodParam, "il2cpp_method_get_param"); MENU_EXPORT(typeName, "il2cpp_type_get_name");
    MENU_EXPORT(freeMemory, "il2cpp_free"); MENU_EXPORT(stringLength, "il2cpp_string_length"); MENU_EXPORT(stringChars, "il2cpp_string_chars");
    MENU_EXPORT(arrayLength, "il2cpp_array_length"); MENU_EXPORT(objectClass, "il2cpp_object_get_class");
    MENU_EXPORT(staticFieldValue, "il2cpp_field_static_get_value"); MENU_EXPORT(fieldType, "il2cpp_field_get_type");
    MENU_EXPORT(fieldFlags, "il2cpp_field_get_flags");
#undef MENU_EXPORT
    const auto headerSize = reinterpret_cast<uint32_t(*)()>(GetProcAddress(g_hGameAssembly, "il2cpp_array_object_header_size"));
    if (!headerSize) return false;
    arrayHeaderBytes = headerSize();
    if (arrayHeaderBytes < 4 * sizeof(void*) || arrayHeaderBytes > 256 || arrayHeaderBytes % alignof(void*)) return false;
    formatException = reinterpret_cast<decltype(formatException)>(GetProcAddress(g_hGameAssembly, "il2cpp_format_exception"));
    size_t count = 0; auto assemblies = p_il2cpp_domain_get_assemblies(domain, &count);
    for (size_t i = 0; i < count; ++i) {
        auto image = p_il2cpp_assembly_get_image(assemblies[i]);
        const char* imageName = p_il2cpp_image_get_name(image);
        if (!imageName) continue;
        if (!strcmp(imageName, "Lua.Beyond.dll")) {
            auto klass = p_il2cpp_class_from_name(image, "Beyond.Lua", "LuaManager");
            auto load = findMethod(klass, "LoadLua", "System.Byte[]", {"System.String"});
            auto gc = findMethod(klass, "IsLuaGCing", "System.Boolean", {}, true);
            doStringMethod = findMethod(klass, "DoString", "System.Object[]", {"System.String"});
            luaInstanceField = klass ? p_il2cpp_class_get_field_from_name(klass, "<instance>k__BackingField") : nullptr;
            if (!load || !gc || !doStringMethod || !luaInstanceField || !(fieldFlags(luaInstanceField) & 0x10) ||
                !namedType(fieldType(luaInstanceField), "Beyond.Lua.LuaManager")) return false;
            *loadTarget = *reinterpret_cast<void**>(load);
            isLuaGCing = reinterpret_cast<decltype(isLuaGCing)>(*reinterpret_cast<void**>(gc));
        } else if (!strcmp(imageName, "Entry.Beyond.dll")) {
            auto update = findMethod(p_il2cpp_class_from_name(image, "Beyond", "GameApp"), "Update", "System.Void", {});
            if (!update) return false;
            *updateTarget = *reinterpret_cast<void**>(update);
        } else if (!strcmp(imageName, "XLua.Runtime.dll")) {
            auto cypher = p_il2cpp_class_from_name(image, "Beyond.Lua", "LuaCypher");
            luaKeyMethod = findMethod(cypher, "GetKey", "System.String", {}, true);
            decryptMethod = findMethod(cypher, "DecryptLuaString", "System.String", {"System.String"}, true);
            encryptMethod = findMethod(p_il2cpp_class_from_name(image, "Security", "XXTEA"), "EncryptToBase64String",
                "System.String", {"System.String", "System.String"}, true);
        } else if (!strcmp(imageName, "mscorlib.dll")) {
            stringClass = p_il2cpp_class_from_name(image, "System", "String");
        }
    }
    return *loadTarget && *updateTarget && isLuaGCing && doStringMethod && luaKeyMethod && decryptMethod && encryptMethod && stringClass;
}
static bool metadata(void** load, void** update) {
    bool ok = false; void* attached = nullptr;
    AcquireSRWLockShared(&g_uidVmLock); g_uidVmHeld = true;
    __try {
        if (UidRuntimeOpen() && g_uidBootstrap.Ready()) {
            void* domain = g_uidBootstrap.AttachWorker(&attached);
            if (domain) ok = resolve(domain, load, update);
        }
    } __finally { g_uidBootstrap.DetachWorker(attached); g_uidVmHeld = false; ReleaseSRWLockShared(&g_uidVmLock); }
    return ok;
}
static bool install() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(install), &module)) return false;
    const auto script = resourceData(module, 102), icon = resourceData(module, 103);
    if (script.empty() || icon.empty()) { Log("[AM-Menu] Embedded menu script or icon is missing"); return false; }
    const auto version = productVersion(module);
    source = "local applepieVersion = " + quote(version.empty() ? "" : "v" + version) + "\n";
    source += "local applepieTabIconBase64 = " + quote(base64(icon)) + "\n";
    source.append(script);
    void* loadTarget = nullptr; void* updateTarget = nullptr;
    if (!metadata(&loadTarget, &updateTarget)) { Log("[AM-Menu] GameApp / LuaManager / LuaCypher / XXTEA ABI unavailable; no menu hooks installed"); return false; }
    if (hub::api()->copy_service(g_uidOwner, APH_SERVICE_WINDOWS, 1, sizeof(windows), &windows) != APH_OK) return false;
    APH_NativeProc original = nullptr;
    if (!PrepareUidHook(loadTarget, reinterpret_cast<APH_NativeProc>(loadEntry), 110,
        "ptr(ptr,ptr,ptr) LuaManager.LoadLua(string)+MethodInfo", &loadClaim, &original)) return false;
    loadOriginal = reinterpret_cast<decltype(loadOriginal)>(original);
    if (g_uidHooks.activate_hook(g_uidOwner, loadClaim, original) < 0 || !WaitUidHook(loadClaim, APH_HOOK_ACTIVE)) return false;
    if (MenuUpdate::Request(g_uidClient, 111, reinterpret_cast<void(*)(void*, void*)>(updateTarget)) < 0) return false;
    // Finish provider/member activation before opening the callback, so every
    // dependency above is published even when another plugin already owns it.
    for (unsigned i = 0; i < 500 && UidRuntimeOpen(); ++i) {
        const auto status = MenuUpdate::Advance();
        if (status == APH_OK) {
            if (MenuUpdate::Listen(updateObserver, onUpdate, nullptr, APH_AFTER) != APH_OK) return false;
            Log("[AM-Menu] GameApp.Update / LuaManager ABI checked; shared menu dispatch active");
            return true;
        }
        if (status < 0 && status != APH_E_BUSY) return false;
        Sleep(10);
    }
    return false;
}
}
