#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>

// ============================================================================
// Logging
// ============================================================================
#include "bounded_log.h"
static BoundedLog g_managerLog;
void Log(const char* fmt, ...) {
    char buf[4096]; va_list args; va_start(args, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args); va_end(args);
    g_managerLog.write(buf);
}

// ============================================================================
// Modules
// ============================================================================
#include "plugin_registry.h"
#include "il2cpp_text_scanner.h"
manager_config::Settings g_managerConfig;
#include "native_settings.h"

// ============================================================================
// Main Initialization Thread
// ============================================================================
static DWORD WINAPI InitThread(LPVOID) {
    // ── Phase 1: Initialize logging ──
    wchar_t logPath[MAX_PATH];
    swprintf_s(logPath, L"%s\\applepie_manager_log.txt", loader::directory());
    g_managerLog.open(logPath);
    Log("=== Applepie Manager / CoreReady ===");
    if(!InitializePluginView()){Log("[AM] Cannot allocate plugin settings view");return 1;}
    g_managerConfig = loader::initial_config();
    Log("[AM] Config: %ls migrated=%u load_error=%lu save_error=%lu", g_managerConfig.path.c_str(),
        unsigned(g_managerConfig.migrated), g_managerConfig.load_error, g_managerConfig.save_error);
    native_settings::preferenceError.store(g_managerConfig.load_error ? g_managerConfig.load_error : g_managerConfig.save_error);
    const auto language = g_managerConfig.values.find("language");
    native_settings::english = language != g_managerConfig.values.end() && language->second == "en";
    g_uidHidden      = manager_config::boolean(g_managerConfig, "hide_uid",       true);
    g_pingHidden     = manager_config::boolean(g_managerConfig, "hide_ping",      false);
    g_menuUidHidden  = manager_config::boolean(g_managerConfig, "hide_menu_uid",  true);
    g_menuNameHidden = manager_config::boolean(g_managerConfig, "hide_menu_name", true);
    g_cardUidHidden  = manager_config::boolean(g_managerConfig, "hide_card_uid",  true);
    g_cardNameHidden = manager_config::boolean(g_managerConfig, "hide_card_name", true);

    // Bootstrap excludes VM shutdown; all native menu objects stay on the game thread.
    HANDLE uidWorker=CreateThread(nullptr,0,[](LPVOID)->DWORD {
        bool ok=false;
        __try {ok=InitIL2CppScanner()&&InstallSetTextHook();}
        __except(APH_EntryFilter(&g_uidHooks,g_uidOwner,0,GetExceptionInformation(),APH_ORIGINAL_NOT_ENTERED,APH_RECOVERY_NONE)){}
        if(!ok){g_hookDisabled=true;if(g_uidOwner)hub::fail_owner(g_uidOwner,APH_E_UNAVAILABLE);}
        Log("[AM] UID runtime: %s",ok?"ready":"unavailable (see Core error details)");
        if (ok) {
            bool menu = false;
            __try { menu = native_settings::install(); }
            __except(APH_EntryFilter(&g_uidHooks,g_uidOwner,0,GetExceptionInformation(),APH_ORIGINAL_NOT_ENTERED,APH_RECOVERY_NONE)) {}
            Log("[AM] Native settings: %s",menu?"waiting for game UI registration":"unavailable (see Core error details)");
        }
        return 0;
    },nullptr,0,nullptr);
    if(uidWorker)CloseHandle(uidWorker);

    Log("[AM] Core initialized. Native settings registration is pending.");
    return 0;
}

// ============================================================================
// DLL Entry Point
// ============================================================================
namespace loader {
void start_features() {
    HANDLE worker = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    if (worker) CloseHandle(worker);
}
}
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    // No startup, wait or cleanup under Loader Lock. Modules are process resident.
    return TRUE;
}
