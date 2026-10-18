#pragma once
#include "../sdk/applepie.h"
#include <windows.h>
namespace hub {
APH_Owner find_owner(const char* id);
void error_detail(APH_Owner, const char* operation, APH_Result, const char* resource = "",
    uint64_t target = 0, APH_Hook hook = 0, uint32_t backend = 0, uint32_t win32 = 0, const char* message = "");
APH_Result initialize();
bool ready();
uint64_t capabilities();
const APH_Api* api();
APH_Owner add_owner(const APH_Manifest&, HMODULE);
unsigned char owner_failure(APH_Owner); // fault-safe, allocation-free published state
void set_owner_state(APH_Owner, uint32_t, APH_Result);
uint64_t begin_enable(APH_Owner);
// Async callers must pass the captured epoch. The default is for synchronous
// Core-owned initialization only.
APH_Result finish_enable(APH_Owner, APH_Result, uint64_t epoch = 0);
void fail_owner(APH_Owner, APH_Result);
APH_Result attach(APH_Owner, APH_AttachFn);
APH_Result enable(APH_Owner, APH_EnableFn, uint64_t);
bool copy(void* dst, const void* src, size_t bytes);
void diagnostic(const char* operation, APH_Owner owner, APH_Hook hook, APH_Result result);
void log_line(const char* message); // internal, never call from an exception filter
const APH_HookApi* hooks_api();
APH_Result shared_native_request(APH_Owner, const APH_HookRequest*, APH_Hook*);
bool admit_owner(APH_Owner, uint64_t epoch);
void leave_owner(APH_Owner);
uint64_t owner_epoch(APH_Owner);
bool owner_usable(APH_Owner);
bool owner_resident(APH_Owner);
bool owner_identity(APH_Owner, APH_Manifest*, HMODULE*); // internal copied identity
bool owner_runtime_open(APH_Owner); // lock-free, terminal failure closes permanently
uint32_t owner_flags(APH_Owner);
bool admit_module(APH_Owner);
void leave_module(APH_Owner);
bool admit_cleanup_resource(APH_Owner,uint64_t);
void leave_cleanup_resource(APH_Owner);
APH_Result suspend_owner(APH_Owner);
APH_Result disable_drained(APH_Owner);
void quarantine_owner(APH_Owner,APH_Result);
uint32_t owner_business_flights(APH_Owner);
bool owner_callback(APH_Owner, const void*);
void resource_delta(APH_Owner, int32_t delta);
APH_Result copy_extension(APH_Owner, uint64_t, uint32_t, uint32_t, void*);
bool runtime_mutations_allowed();
void extensions_tick(); // control thread, outside the core registry
}
