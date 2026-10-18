#pragma once
#include "manager_loader.h"
#include <stdexcept>
#include <vector>
// UI projection only. Loader and Core own runtime truth; no legacy entry calls.
static std::vector<unsigned char> g_pluginEnabled;
static bool InitializePluginView() {
    try { g_pluginEnabled.resize(loader::count()); }
    catch(const std::bad_alloc&) { return false; }
    catch(const std::length_error&) { return false; }
    for (unsigned i = 0; i < loader::count(); ++i) {
        loader::Plugin plugin{};
        if (loader::snapshot(i, &plugin)) g_pluginEnabled[i] = plugin.enabled;
    }
    return true;
}
