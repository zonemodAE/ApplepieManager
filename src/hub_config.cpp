#define NOMINMAX
#include "hub_internal.h"
#include "manager_events.h"
#include "config_file.h"
#include <charconv>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <set>
#include <vector>

namespace {
struct Field {
    APH_ConfigField desc{};
    std::vector<APH_ConfigOption> options;
};
struct Record {
    APH_Owner owner = 0;
    std::wstring directory, path;
    std::vector<Field> fields;
    APH_ConfigSnapshot current{};
    std::vector<APH_ConfigValue> values, queued;
    std::map<std::string, std::string> stored; // Preserve unknown keys across schema revisions.
    APH_ConfigNotifyFn notify = nullptr;
    void* notify_context = nullptr;
    SRWLOCK notify_lock = SRWLOCK_INIT;
};
// Records and optional notification contexts live with their pinned DLL.
std::mutex config_lock;
std::vector<std::unique_ptr<Record>> records;
int notify_filter(APH_Owner owner, EXCEPTION_POINTERS* e) {
    hub::hooks_api()->record_fault(owner, 0, e->ExceptionRecord->ExceptionCode,
        (uint64_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress, APH_ORIGINAL_NOT_ENTERED, 0);
    return e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        !(e->ExceptionRecord->ExceptionFlags & EXCEPTION_NONCONTINUABLE) && e->ExceptionRecord->NumberParameters >= 2 &&
        e->ExceptionRecord->ExceptionInformation[0] == 0 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
void invoke_notification(Record& r, APH_ConfigNotifyFn fn, void* context) {
    // Never hold config/registry locks across client code. Serial delivery also
    // prevents an earlier callback overwriting a newer plugin-side snapshot.
    AcquireSRWLockExclusive(&r.notify_lock);
    __try {
        if (!hub::admit_module(r.owner)) __leave;
        __try {
            __try { fn(r.owner, context); }
            __except(notify_filter(r.owner, GetExceptionInformation())) {}
        } __finally { hub::leave_module(r.owner); }
    } __finally { ReleaseSRWLockExclusive(&r.notify_lock); }
}
void notify_completion(Record& r) {
    manager_events::notify();
    APH_ConfigNotifyFn fn = nullptr; void* context = nullptr;
    { std::lock_guard<std::mutex> guard(config_lock); fn = r.notify; context = r.notify_context; }
    if (fn) invoke_notification(r, fn, context);
}
bool text(const char* p, size_t capacity, bool nonempty = false) {
    const auto end = static_cast<const char*>(memchr(p, 0, capacity));
    if (!end || (nonempty && end == p)) return false;
    for (auto q = p; q != end; ++q) if (static_cast<unsigned char>(*q) < 32 || *q == 127) return false;
    return end == p || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, int(end - p), nullptr, 0) != 0;
}
bool key(const char* p, size_t capacity) {
    if (!text(p, capacity, true)) return false;
    for (; *p; ++p) if (!(*p >= 'a' && *p <= 'z') && !(*p >= 'A' && *p <= 'Z') &&
        !(*p >= '0' && *p <= '9') && *p != '_' && *p != '-' && *p != '.') return false;
    return true;
}
bool normalize(const Field& field, const APH_ConfigValue& in, APH_ConfigValue& out) {
    const auto& f = field.desc;
    if (in.type != f.type) return false;
    out = {}; out.type = f.type;
    switch (f.type) {
    case APH_CONFIG_BOOL:
        if (in.integer != 0 && in.integer != 1) return false;
        out.integer = in.integer; return true;
    case APH_CONFIG_INT:
        if (in.integer < f.minimum || in.integer > f.maximum) return false;
        out.integer = in.integer; return true;
    case APH_CONFIG_KEY:
        if (in.integer < 0 || in.integer > 255) return false;
        out.integer = in.integer; return true;
    case APH_CONFIG_FLOAT:
        if (!std::isfinite(in.number) || in.number < f.minimum || in.number > f.maximum) return false;
        out.number = in.number; return true;
    case APH_CONFIG_ENUM:
        if (!text(in.text, sizeof(in.text), true)) return false;
        for (const auto& option : field.options) if (!strcmp(in.text, option.value)) {
            strcpy_s(out.text, in.text); return true;
        }
        return false;
    case APH_CONFIG_TEXT:
        if (!text(in.text, sizeof(in.text)) || strlen(in.text) > f.text_max_bytes) return false;
        strcpy_s(out.text, in.text); return true;
    default: return false;
    }
}
bool validate(Field& field) {
    auto& f = field.desc;
    if (f.size != sizeof(f) || !key(f.key, sizeof(f.key)) || !text(f.label, sizeof(f.label), true) ||
        !text(f.description, sizeof(f.description))) return false;
    if (f.type == APH_CONFIG_INT || f.type == APH_CONFIG_FLOAT) {
        if (!std::isfinite(f.minimum) || !std::isfinite(f.maximum) || f.minimum > f.maximum) return false;
        if (f.type == APH_CONFIG_INT && (f.minimum < INT32_MIN || f.maximum > INT32_MAX ||
            std::trunc(f.minimum) != f.minimum || std::trunc(f.maximum) != f.maximum)) return false;
    }
    if (f.type == APH_CONFIG_TEXT && (!f.text_max_bytes || f.text_max_bytes >= APH_CONFIG_TEXT_BYTES)) return false;
    if (f.type == APH_CONFIG_ENUM) {
        if (!f.option_count || !f.options) return false;
        field.options.resize(f.option_count);
        if (!hub::copy(field.options.data(), f.options, field.options.size() * sizeof(APH_ConfigOption))) return false;
        std::set<std::string> unique;
        for (const auto& option : field.options) {
            if (!text(option.value, sizeof(option.value), true) || !unique.insert(option.value).second) return false;
        }
    } else if (f.option_count || f.options) return false;
    f.options = nullptr;
    APH_ConfigValue value{};
    if (!normalize(field, f.initial, value)) return false;
    f.size = sizeof(f); f.initial = value;
    return true;
}
bool decode(const Field& field, const std::string& s, APH_ConfigValue& out) {
    const auto& f = field.desc;
    APH_ConfigValue v{}; v.type = f.type;
    if (f.type == APH_CONFIG_BOOL) {
        if (s == "true" || s == "1") v.integer = 1;
        else if (s != "false" && s != "0") return false;
    } else if (f.type == APH_CONFIG_INT || f.type == APH_CONFIG_KEY) {
        const auto r = std::from_chars(s.data(), s.data() + s.size(), v.integer);
        if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return false;
    } else if (f.type == APH_CONFIG_FLOAT) {
        const auto r = std::from_chars(s.data(), s.data() + s.size(), v.number);
        if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return false;
    } else {
        if (s.size() >= sizeof(v.text)) return false;
        memcpy(v.text, s.c_str(), s.size() + 1);
    }
    return normalize(field, v, out);
}
std::string encode(const APH_ConfigValue& v) {
    if (v.type == APH_CONFIG_BOOL) return v.integer ? "true" : "false";
    if (v.type == APH_CONFIG_ENUM || v.type == APH_CONFIG_TEXT) return v.text;
    char buffer[64];
    const auto r = (v.type == APH_CONFIG_INT || v.type == APH_CONFIG_KEY) ? std::to_chars(buffer, buffer + sizeof(buffer), v.integer) :
        std::to_chars(buffer, buffer + sizeof(buffer), v.number);
    return std::string(buffer, r.ptr);
}
std::string json_string(const std::string& value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '\\' || c == '"') { out += '\\'; out += char(c); }
        else if (c < 32) {
            const char hex[] = "0123456789abcdef";
            out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15];
        } else out += char(c);
    }
    return out + '"';
}
DWORD describe_schema(const Record& r, const wchar_t* module_path, const std::string& id) noexcept {
    try {
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExW(module_path, GetFileExInfoStandard, &info)) return GetLastError();
        const uint64_t size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        const uint64_t stamp = (uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
        std::string data = "{\"schemaVersion\":1,\"pluginId\":" + json_string(id) +
            ",\"moduleSize\":" + std::to_string(size) + ",\"moduleWriteTime\":\"" + std::to_string(stamp) + "\",\"fields\":[";
        bool first = true;
        for (const auto& field : r.fields) {
            const auto& f = field.desc;
            if (!first) data += ',';
            first = false;
            data += "{\"key\":" + json_string(f.key) + ",\"label\":" + json_string(f.label) +
                ",\"description\":" + json_string(f.description) + ",\"type\":" + std::to_string(f.type) +
                ",\"defaultValue\":" + json_string(encode(f.initial));
            if (f.type == APH_CONFIG_INT || f.type == APH_CONFIG_FLOAT) {
                APH_ConfigValue number{}; number.type = APH_CONFIG_FLOAT;
                number.number = f.minimum; data += ",\"minimum\":" + encode(number);
                number.number = f.maximum; data += ",\"maximum\":" + encode(number);
            }
            if (f.type == APH_CONFIG_TEXT) data += ",\"textMaxBytes\":" + std::to_string(f.text_max_bytes);
            data += ",\"options\":[";
            bool first_option = true;
            for (const auto& option : field.options) {
                if (!first_option) data += ',';
                first_option = false; data += json_string(option.value);
            }
            data += "]}";
        }
        data += "]}\n";
        const auto path = r.directory + L"\\applepie." + std::wstring(id.begin(), id.end()) + L".schema.json";
        // A one-time description for the offline editor. Never modifies the INI
        // and never polls configuration or plugin memory.
        return config_file::write(r.directory, path, data);
    } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
}
DWORD read_file(Record& r) {
    std::string contents;
    DWORD error = config_file::read(r.path, contents);
    if (config_file::missing(error)) return ERROR_SUCCESS;
    if (error) return error;
    error = config_file::parse(contents, r.stored);
    if (error) return error;
    for (const auto& item : r.stored) {
        const auto& name = item.first;
        const auto& value = item.second;
        if (name.size() >= 48 || !key(name.c_str(), name.size() + 1) ||
            value.size() >= APH_CONFIG_TEXT_BYTES || !text(value.c_str(), value.size() + 1)) return ERROR_INVALID_DATA;
    }
    for (unsigned i = 0; i < r.current.field_count; ++i) {
        auto it = r.stored.find(r.fields[i].desc.key);
        if (it != r.stored.end()) {
            APH_ConfigValue value{};
            if (decode(r.fields[i], it->second, value)) r.values[i] = value;
            else ++r.current.defaulted_fields;
        }
    }
    return ERROR_SUCCESS;
}
DWORD WINAPI save_worker(void* context) {
    auto& r = *static_cast<Record*>(context);
    DWORD error = ERROR_SUCCESS;
    try {
        // pending excludes another writer. Everything here is host-owned;
        // the GUI, plugin snapshots and Core control thread never wait for I/O.
        std::map<std::string, std::string> stored;
        std::vector<APH_ConfigValue> values;
        {
            std::lock_guard<std::mutex> guard(config_lock);
            stored = r.stored; values = r.queued;
        }
        for (unsigned i = 0; i < values.size(); ++i) stored[r.fields[i].desc.key] = encode(values[i]);
        error = config_file::write(r.directory, r.path, config_file::encode(stored));
        if (!error) {
            {
                std::lock_guard<std::mutex> guard(config_lock);
                r.stored.swap(stored);
                r.values.swap(values);
                ++r.current.revision;
                r.current.defaulted_fields = 0;
                r.current.last_result = APH_OK; r.current.win32_error = 0; r.current.pending = 0;
            }
        }
    } catch (...) { error = ERROR_NOT_ENOUGH_MEMORY; }
    if (error) {
        std::lock_guard<std::mutex> guard(config_lock);
        r.current.last_result = error ? APH_E_IO : APH_OK;
        r.current.win32_error = error;
        r.current.pending = 0;
    }
    if (error) hub::error_detail(r.owner, "config-save", APH_E_IO, "basic settings", 0, 0, 0, error, "Configuration was not saved; previous values retained");
    notify_completion(r);
    return 0;
}
APH_Result APH_CALL register_schema(APH_Owner owner, const APH_ConfigSchema* input) {
    APH_ConfigSchema schema{};
    if (!hub::services::input(input, &schema) || !schema.field_count ||
        !schema.fields || !owner) return APH_E_ARGUMENT;
    if (!hub::owner_resident(owner)) return APH_E_REVOKED;
    try {
        { std::lock_guard<std::mutex> guard(config_lock);
          if(owner>=records.size())records.resize(owner+1);
          if (records[owner]) return APH_E_DUPLICATE; }
        auto r = std::make_unique<Record>();
        r->owner = owner; r->current.size = sizeof(r->current); r->current.field_count = schema.field_count;
        r->current.revision = 1;
        std::vector<APH_ConfigField> fields(schema.field_count);
        if (!hub::copy(fields.data(), schema.fields, fields.size() * sizeof(APH_ConfigField))) return APH_E_ARGUMENT;
        r->fields.resize(schema.field_count); r->values.resize(schema.field_count);
        std::set<std::string> unique;
        for (unsigned i = 0; i < schema.field_count; ++i) {
            r->fields[i].desc = fields[i];
            if (!validate(r->fields[i])) return APH_E_ARGUMENT;
            if (!unique.insert(r->fields[i].desc.key).second) return APH_E_DUPLICATE;
            r->values[i] = r->fields[i].desc.initial;
        }
        APH_Manifest manifest{}; HMODULE module = nullptr;
        if (!hub::owner_identity(owner, &manifest, &module) || !key(manifest.id, sizeof(manifest.id))) return APH_E_ARGUMENT;
        if (!strcmp(manifest.id, "manager")) {
            hub::error_detail(owner, "config-register", APH_E_ARGUMENT, "basic settings", 0, 0, 0, 0,
                "Configuration ID manager is reserved for Manager preferences");
            return APH_E_ARGUMENT;
        }
        wchar_t module_path[MAX_PATH];
        const DWORD length = GetModuleFileNameW(module, module_path, MAX_PATH);
        if (!length || length >= MAX_PATH) return APH_E_UNAVAILABLE;
        const auto slash = wcsrchr(module_path, L'\\');
        if (!slash) return APH_E_UNAVAILABLE;
        r->directory.assign(module_path, slash); r->directory += L"\\config";
        const std::string id(manifest.id);
        r->path = r->directory + L"\\applepie." + std::wstring(id.begin(), id.end()) + L".ini";
        const DWORD error = read_file(*r);
        if (error) {
            hub::error_detail(owner, "config-load", APH_E_IO, "basic settings", 0, 0, 0, error, "Cannot read configuration; file left unchanged");
            return APH_E_IO;
        }
        const auto committed = r.get();
        {
            std::lock_guard<std::mutex> guard(config_lock);
            if (records[owner]) return APH_E_DUPLICATE;
            records[owner] = std::move(r);
        }
        // Metadata failure must not prevent a valid plugin configuration from registering.
        const DWORD description_error = describe_schema(*committed, module_path, id);
        if (description_error) {
            char message[192];
            _snprintf_s(message, sizeof(message), _TRUNCATE, "[APH] config-description owner=%llu win32=%lu (configuration remains available)\n",
                (unsigned long long)owner, description_error);
            hub::log_line(message);
        }
        manager_events::notify();
        return APH_OK;
    } catch (const std::bad_alloc&) { return APH_E_QUEUE_FULL; }
      catch (const std::length_error&) { return APH_E_QUEUE_FULL; }
      catch (...) { return APH_E_UNAVAILABLE; }
}
APH_Result APH_CALL field(APH_Owner owner, uint32_t index, uint32_t bytes, APH_ConfigField* out) {
    APH_ConfigField value{};
    {
        std::lock_guard<std::mutex> guard(config_lock);
        if (!owner || owner >= records.size() || !records[owner]) return APH_E_UNAVAILABLE;
        if (index >= records[owner]->current.field_count) return APH_E_ARGUMENT;
        value = records[owner]->fields[index].desc;
    }
    return hub::services::output(bytes, out, value);
}
APH_Result APH_CALL option(APH_Owner owner, uint32_t field_index, uint32_t index, uint32_t bytes, APH_ConfigOption* out) {
    APH_ConfigOption value{};
    {
        std::lock_guard<std::mutex> guard(config_lock);
        if (!owner || owner >= records.size() || !records[owner]) return APH_E_UNAVAILABLE;
        const auto& fields = records[owner]->fields;
        if (field_index >= fields.size() || index >= fields[field_index].options.size()) return APH_E_ARGUMENT;
        value = fields[field_index].options[index];
    }
    return hub::services::output(bytes, out, value);
}
APH_Result APH_CALL snapshot(APH_Owner owner, uint32_t bytes, APH_ConfigSnapshot* out, uint32_t capacity, APH_ConfigValue* values) {
    std::lock_guard<std::mutex> guard(config_lock);
    if (!owner || owner >= records.size() || !records[owner]) return APH_E_UNAVAILABLE;
    const auto& r = *records[owner];
    if (values ? capacity < r.values.size() : capacity != 0) return APH_E_ARGUMENT;
    if (bytes < sizeof(*out)) return APH_E_ABI;
    if (values && !hub::copy(values, r.values.data(), r.values.size() * sizeof(APH_ConfigValue))) return APH_E_ARGUMENT;
    return hub::services::output(bytes, out, r.current);
}
APH_Result APH_CALL save(APH_Owner owner, uint64_t revision, uint32_t count, const APH_ConfigValue* input) {
    if (!owner || !count) return APH_E_ARGUMENT;
    try {
        std::lock_guard<std::mutex> guard(config_lock);
        if(owner>=records.size())return APH_E_UNAVAILABLE;
        auto r = records[owner].get();
        if (!r) return APH_E_UNAVAILABLE;
        if (count != r->current.field_count) return APH_E_ARGUMENT;
        if (r->current.pending) return APH_E_BUSY;
        if (revision != r->current.revision) return APH_E_STALE;
        std::vector<APH_ConfigValue> values(count), normalized(count);
        if (!hub::copy(values.data(), input, values.size() * sizeof(APH_ConfigValue))) return APH_E_ARGUMENT;
        for (unsigned i = 0; i < count; ++i) if (!normalize(r->fields[i], values[i], normalized[i])) return APH_E_ARGUMENT;
        r->queued.swap(normalized);
        r->current.pending = 1; r->current.last_result = APH_PENDING; r->current.win32_error = 0;
        if (!QueueUserWorkItem(save_worker, r, WT_EXECUTEDEFAULT)) {
            r->current.pending = 0; r->current.last_result = APH_E_UNAVAILABLE; r->current.win32_error = GetLastError();
            return APH_E_UNAVAILABLE;
        }
        return APH_PENDING;
    } catch (...) { return APH_E_UNAVAILABLE; }
}
APH_Result APH_CALL set_notifier(APH_Owner owner, APH_ConfigNotifyFn callback, void* context) {
    if (!owner || !callback || !hub::owner_callback(owner, (const void*)callback)) return APH_E_ARGUMENT;
    if (!(hub::owner_flags(owner) & APH_PLUGIN_MODULE_OBSERVER)) return APH_E_CAPABILITY;
    if (!hub::owner_resident(owner)) return APH_E_REVOKED;
    std::lock_guard<std::mutex> guard(config_lock);
    if(owner>=records.size())return APH_E_UNAVAILABLE;
    auto r = records[owner].get();
    if (!r) return APH_E_UNAVAILABLE;
    if (r->notify) return APH_E_DUPLICATE;
    if (r->current.pending) return APH_E_BUSY;
    r->notify = callback; r->notify_context = context;
    return APH_OK;
}
const APH_ConfigApi config_table{sizeof(config_table), 1, register_schema, field, option, snapshot, save, set_notifier};
}
namespace hub::services {
const APH_ConfigApi* config_api() { return &config_table; }
}
