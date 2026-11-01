#pragma once
#include "config_file.h"
#include <cstring>

namespace manager_config {
struct Settings {
    std::wstring directory, path;
    config_file::Values values;
    DWORD load_error = 0, save_error = 0;
    bool migrated = false;
};
inline config_file::Values defaults() {
    return {{"language", "zh"}, {"hide_uid", "true"}, {"hide_ping", "false"},
        {"hide_menu_uid", "true"}, {"hide_menu_name", "true"},
        {"hide_card_uid", "true"}, {"hide_card_name", "true"},
        {"hide_hud_left", "false"}, {"hide_hud_center", "false"}, {"hide_hud_right", "false"}};
}
inline std::string plugin_key(std::string filename) {
    for (auto& c : filename) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return "plugin." + filename;
}
inline std::string plugin_key(const wchar_t* filename) {
    const int length = int(wcslen(filename));
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, filename, length, nullptr, 0, nullptr, nullptr);
    if (!bytes) return {};
    std::string name(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, filename, length, name.data(), bytes, nullptr, nullptr);
    return plugin_key(name);
}
inline bool boolean(const Settings& cfg, const std::string& key, bool fallback) {
    const auto it = cfg.values.find(key);
    if (it == cfg.values.end()) return fallback;
    if (it->second == "true" || it->second == "1") return true;
    if (it->second == "false" || it->second == "0") return false;
    return fallback;
}
inline bool plugin_enabled(const Settings& cfg, const wchar_t* filename) {
    const auto key = plugin_key(filename);
    return !cfg.load_error && !key.empty() && boolean(cfg, key, true);
}
inline DWORD save(const Settings& cfg, bool replace = true) {
    if (cfg.load_error) return cfg.load_error; // Preserve an unreadable file.
    return config_file::write(cfg.directory, cfg.path, config_file::encode(cfg.values), replace);
}
inline std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? "" : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
// Read known settings once when the new file is absent. The old file is never written.
inline DWORD import_legacy(const std::string& contents, config_file::Values& values) {
    config_file::Values imported;
    std::string section;
    size_t start = contents.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    const auto base = defaults();
    while (start < contents.size()) {
        auto end = contents.find('\n', start);
        if (end == std::string::npos) end = contents.size();
        auto line = trim(contents.substr(start, end - start)); start = end + 1;
        if (line.size() > INT_MAX || line.find('\0') != std::string::npos || (!line.empty() &&
            !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line.data(), int(line.size()), nullptr, 0))) return ERROR_INVALID_DATA;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            for (auto& c : section) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const auto name = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (section == "general" && name == "language" && (value == "zh" || value == "en")) imported.emplace(name, value);
        else if ((section == "uid" && name != "language" && base.count(name)) || (section == "plugins" && !name.empty())) {
            if (!config_file::text(name)) return ERROR_INVALID_DATA;
            const bool on = value == "1" || !_stricmp(value.c_str(), "true");
            imported.emplace(section == "plugins" ? plugin_key(name) : name, on ? "true" : "false");
        }
    }
    values = base;
    for (const auto& item : imported) values[item.first] = item.second;
    return ERROR_SUCCESS;
}
inline Settings load(const wchar_t* plugin_directory) {
    Settings cfg;
    cfg.directory = std::wstring(plugin_directory) + L"\\config";
    cfg.path = cfg.directory + L"\\applepie.manager.ini";
    std::string contents;
    auto error = config_file::read(cfg.path, contents);
    if (!error) {
        cfg.load_error = config_file::parse(contents, cfg.values);
        return cfg;
    }
    if (!config_file::missing(error)) { cfg.load_error = error; return cfg; }
    error = config_file::read(std::wstring(plugin_directory) + L"\\applepie_manager_config.txt", contents);
    if (!error) {
        cfg.load_error = import_legacy(contents, cfg.values);
        cfg.migrated = !cfg.load_error;
    } else if (config_file::missing(error)) cfg.values = defaults();
    else cfg.load_error = error;
    if (!cfg.load_error) {
        cfg.save_error = save(cfg, false);
        if (cfg.save_error == ERROR_ALREADY_EXISTS || cfg.save_error == ERROR_FILE_EXISTS) {
            // Another process created the new file: it takes precedence.
            cfg.load_error = config_file::read(cfg.path, contents);
            if (!cfg.load_error) cfg.load_error = config_file::parse(contents, cfg.values);
            cfg.save_error = 0; cfg.migrated = false;
        }
    }
    return cfg;
}
}
