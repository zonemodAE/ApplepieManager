#define APH_BUILD_MANAGER
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "hub_core.h"
#include "hub_internal.h"
#include "manager_events.h"
#include "bounded_log.h"
#include "../sdk/applepie_seh.h"
#include "MinHook.h"
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cstdlib>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
constexpr unsigned MaxDepth = 32;
constexpr LONG64 Closed = (LONG64)(UINT64_C(1) << 63);
struct Owner {
    // IDs/modules are never recycled. Published (1) and terminally closed (2)
    // are monotonic bits so a racing fault cannot be undone by publication.
    std::atomic<uint32_t> runtime_gate{0};
    APH_Manifest manifest;
    HMODULE module;
    std::atomic<uint32_t> state{0};
    APH_Result result;
    uint64_t epoch;
    volatile LONG64 gate; // closed bit and admitted count in ONE CAS domain
    volatile LONG lifecycle;
    volatile LONG module_flights, cleanup_resources;
    volatile LONG extra_resources;
    volatile LONG fault_record; // 0 empty, 1 being written, 2 published; first fault wins
    volatile LONG unknown_fault; // a subsequent original/write fault may escalate a read fault
    uint32_t fault_code, fault_phase, fault_thread, recoverable;
    uint64_t fault_address;
    APH_Hook fault_hook;
    bool fault_handled;
};
static_assert(std::atomic<uint32_t>::is_always_lock_free,"fault-safe runtime gate");
struct Hook {
    APH_HookRequest request;
    APH_Owner owner;
    APH_NativeProc original;
    uint32_t state, operation;
    APH_Result result;
    MH_STATUS backend;
    bool ever_enabled, release_requested, executing;
    volatile LONG closed, in_flight;
    unsigned char entry_bytes[16];
};
struct Subscription {
    APH_SubscribeDesc desc;
    APH_Owner owner;
    uint64_t epoch;
    APH_Subscription token;
    volatile LONG64 gate;
    bool pending_notice;
};
struct CallFrame { APH_Owner owner; APH_Hook hook; Hook* record; bool admitted; };
SRWLOCK registry = SRWLOCK_INIT;
hub::ResidentTable<Owner> owners;
// Only the index grows. Published records stay at the same address for their
// entire module lifetime, including backend work outside registry and leave_call.
std::vector<std::unique_ptr<Hook>> hooks;
hub::ResidentTable<Subscription> subscriptions;
hub::TokenIndex<Subscription> subscription_tokens;
size_t owner_count, subscription_count;
HANDLE wake_event;
BoundedLog core_log;
volatile LONG is_ready, backend_frozen;
uint64_t instance;
__declspec(thread) CallFrame call_stack[MaxDepth];
__declspec(thread) unsigned call_depth;
APH_Api core_api{};
APH_HookApi hook_api{};
void write_log(const char* message) { core_log.write(message); }

LONG64 read_gate(volatile LONG64* gate) { return InterlockedCompareExchange64(gate, 0, 0); }
bool admit(volatile LONG64* gate) {
    LONG64 old = read_gate(gate);
    while (!(old & Closed)) {
        LONG64 value = InterlockedCompareExchange64(gate, old + 1, old);
        if (value == old) return true;
        old = value;
    }
    return false;
}
void close_gate(volatile LONG64* gate) { InterlockedOr64(gate, Closed); }
uint32_t flights(volatile LONG64* gate) { return (uint32_t)(read_gate(gate) & ~Closed); }
Owner* owner_at(APH_Owner id) { return id && id <= owner_count ? &owners[id - 1] : nullptr; }
Hook* hook_at(APH_Owner owner, APH_Hook id) {
    return id && id <= hooks.size() && hooks[id - 1]->owner == owner ? hooks[id - 1].get() : nullptr;
}
// Called under registry, before publishing a claim. Keep C++ allocation failures
// inside this helper rather than allowing them across the C/SEH API boundary.
Hook* append_hook() noexcept {
    try {
        hooks.emplace_back(std::make_unique<Hook>());
        return hooks.back().get();
    } catch (const std::bad_alloc&) { return nullptr; }
      catch (const std::length_error&) { return nullptr; }
}
bool usable(Owner* o) {
    return o && o->state < APH_OWNER_FAILED && !InterlockedCompareExchange(&o->fault_record, 0, 0);
}
bool resident(Owner* o) {
    return o && !APH_OwnerHasFault(o->state) && !InterlockedCompareExchange(&o->fault_record,0,0);
}
bool terminated(const char* text, size_t bytes) { return memchr(text, 0, bytes) && text[0]; }
bool executable(const void* p, HMODULE* module) {
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQuery(p, &m, sizeof(m)) || m.State != MEM_COMMIT || m.Type != MEM_IMAGE ||
        (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(m.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                             (LPCWSTR)p, module) != FALSE;
}
// Only caller-buffer accesses are recoverable here. No registry mutation in this leaf.
int buffer_filter(DWORD code) {
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ?
        EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
bool safe_copy(void* dst, const void* src, size_t bytes) {
    if (!dst || !src) return false;
    __try { memcpy(dst, src, bytes); return true; }
    __except(buffer_filter(GetExceptionCode())) { return false; }
}
template<class T> bool read_input(const T* src, T* dst) {
    uint32_t size = 0;
    return safe_copy(&size, src, sizeof(size)) && size >= sizeof(T) && safe_copy(dst, src, sizeof(T));
}
template<class T> APH_Result output(uint32_t bytes, T* out, const T& value) {
    if (bytes < sizeof(T)) return APH_E_ABI;
    return safe_copy(out, &value, sizeof(T)) ? APH_OK : APH_E_ARGUMENT;
}
void APH_CALL record_fault(APH_Owner id, APH_Hook hook, uint32_t code,
                           uint64_t address, uint32_t phase, uint32_t recoverable) {
    // IDs and published records are never recycled. No lock/log/allocation here.
    Owner* o = id ? owners.get(id - 1) : nullptr;
    if (!o || !o->runtime_gate.load(std::memory_order_acquire)) return;
    o->runtime_gate.fetch_or(2,std::memory_order_release);
    close_gate(&o->gate);
    if (!recoverable) InterlockedExchange(&o->unknown_fault, 1);
    if (InterlockedCompareExchange(&o->fault_record, 1, 0) == 0) {
        o->fault_code = code; o->fault_address = address; o->fault_phase = phase;
        o->fault_thread = GetCurrentThreadId(); o->fault_hook = hook;
        o->recoverable = recoverable;
        InterlockedExchange(&o->fault_record, 2);
    }
}
int backend_filter(DWORD code) {
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR) return EXCEPTION_CONTINUE_SEARCH;
    InterlockedExchange(&backend_frozen, 1);
    return EXCEPTION_EXECUTE_HANDLER;
}
APH_Result backend_operation(uint32_t op, Hook* h, MH_STATUS* status, APH_NativeProc* original = nullptr) {
    // MinHook is only called on this control path, never while holding registry.
    __try {
        if (op == 0) *status = MH_Initialize();
        if (op == 1) *status = MH_CreateHook((void*)h->request.target, (void*)h->request.native_entry, (void**)original);
        if (op == 2) *status = MH_EnableHook((void*)h->request.target);
        if (op == 3) *status = h->ever_enabled ? MH_DisableHook((void*)h->request.target) : MH_RemoveHook((void*)h->request.target);
        return *status == MH_OK ? APH_OK : APH_E_BACKEND;
    } __except(backend_filter(GetExceptionCode())) { return APH_E_NEEDS_RESTART; }
}
APH_Result request_hook_impl(APH_Owner id, const APH_HookRequest* input, APH_Hook* out, bool shared = false) {
    APH_Hook zero = 0;
    APH_HookRequest r{};
    if (!safe_copy(out, &zero, sizeof(zero)) || !read_input(input, &r)) return APH_E_ARGUMENT;
    if (!r.request_key || !r.target || !r.native_entry || r.reserved ||
        !terminated(r.native_signature, sizeof(r.native_signature)) ||
        !terminated(r.diagnostic_name, sizeof(r.diagnostic_name)) || r.recovery > APH_RECOVERY_READ_ONLY)
        return APH_E_ARGUMENT;
    HMODULE target_module = nullptr, entry_module = nullptr;
    uint32_t timestamp = 0, image_size = 0;
    if (!executable((void*)r.target, &target_module) || !executable((void*)r.native_entry, &entry_module) ||
        !APH_ModuleBuild(r.target, &timestamp, &image_size)) return APH_E_ARGUMENT;
    if (!r.module_image_size || r.module_timestamp != timestamp || r.module_image_size != image_size) return APH_E_BUILD;
    APH_Result result = APH_E_UNAVAILABLE;
    AcquireSRWLockExclusive(&registry);
    __try {
        Owner* o = owner_at(id);
        if (!usable(o)) return APH_E_REVOKED;
        if (!hub::runtime_mutations_allowed()) return APH_E_REVOKED;
        if (entry_module != o->module) return APH_E_ARGUMENT;
        if (backend_frozen) return APH_E_NEEDS_RESTART;
        for (size_t i = 0; i < hooks.size(); ++i) {
            Hook* h = hooks[i].get();
            if (h->owner == id && h->request.request_key == r.request_key) {
                if (h->request.target != r.target || h->request.native_entry != r.native_entry ||
                    h->request.mode != r.mode || h->request.recovery != r.recovery ||
                    strcmp(h->request.native_signature, r.native_signature) ||
                    h->request.module_image_size != r.module_image_size || h->request.module_timestamp != r.module_timestamp)
                    return APH_E_KEY_REUSED;
                APH_Hook value = i + 1;
                if (!safe_copy(out, &value, sizeof(value))) return APH_E_ARGUMENT;
                return h->operation || h->executing ? APH_PENDING : h->result;
            }
            if (h->request.target == r.target && h->state != APH_HOOK_RELEASED && h->state != APH_HOOK_FAILED) {
                if (h->request.mode != r.mode) return APH_E_MODE;
                if (strcmp(h->request.native_signature, r.native_signature)) return APH_E_SIGNATURE;
                return APH_E_CONFLICT;
            }
            if (h->request.native_entry == r.native_entry && h->state != APH_HOOK_RELEASED && h->state != APH_HOOK_FAILED)
                return APH_E_CONFLICT; // one typed entry/context binds exactly one live target
        }
        if (r.mode != APH_EXCLUSIVE && !(shared && r.mode == APH_SHARED_TYPED)) return APH_E_MODE;
        unsigned char entry_bytes[16]{};
        if (!safe_copy(entry_bytes, (void*)r.target, sizeof(entry_bytes))) return APH_E_ARGUMENT;
        Hook* h = append_hook();
        if (!h) return APH_E_QUEUE_FULL;
        // Native export/import thunks also start with JMP. Let the backend
        // relocate the requested entry; activation still checks this snapshot.
        APH_Hook value = hooks.size();
        if (!safe_copy(out, &value, sizeof(value))) { hooks.pop_back(); return APH_E_ARGUMENT; }
        memcpy(h->entry_bytes, entry_bytes, sizeof(entry_bytes));
        h->request = r; h->owner = id; h->state = APH_HOOK_PENDING;
        h->operation = 1; h->result = APH_PENDING; h->closed = 1;
        // Target lookup, reservation and publication are one critical section.
        result = APH_PENDING;
    } __finally { ReleaseSRWLockExclusive(&registry); }
    SetEvent(wake_event);
    return result;
}
APH_Result APH_CALL request_hook(APH_Owner id, const APH_HookRequest* input, APH_Hook* out) {
    APH_Result result = request_hook_impl(id, input, out);
    APH_HookRequest request{}; APH_Hook claim = 0;
    read_input(input, &request); safe_copy(&claim, out, sizeof(claim));
    char message[384];
    _snprintf_s(message, sizeof(message), _TRUNCATE,
        "[APH] op=request owner=%llu target=%p mode=%u key=%llu claim=%llu result=%d\n",
        (unsigned long long)id, (void*)request.target, request.mode,
        (unsigned long long)request.request_key, (unsigned long long)claim, result);
    write_log(message);
    if(result<0)hub::error_detail(id,"request-hook",result,request.diagnostic_name,(uint64_t)request.target,claim);
    return result;
}
APH_Result APH_CALL query_hook(APH_Owner owner, APH_Hook id, uint32_t bytes, APH_HookStatus* out) {
    APH_HookStatus value{};
    AcquireSRWLockShared(&registry);
    __try {
        Hook* h = hook_at(owner, id);
        if (!h) return APH_E_ARGUMENT;
        value = {sizeof(value), h->state, h->result, h->request.mode, id, owner,
                 h->original, (uint32_t)InterlockedCompareExchange(&h->in_flight, 0, 0), (uint32_t)h->backend};
        if (h->operation || h->executing || (h->state == APH_HOOK_RETIRED_PINNED && value.in_flight)) value.result = APH_PENDING;
    } __finally { ReleaseSRWLockShared(&registry); }
    return output(bytes, out, value);
}
APH_Result APH_CALL activate_hook(APH_Owner owner, APH_Hook id, APH_NativeProc saved) {
    AcquireSRWLockExclusive(&registry);
    __try {
        Hook* h = hook_at(owner, id); Owner* o = owner_at(owner);
        if (!h || !saved || saved != h->original) return APH_E_ARGUMENT;
        if (!usable(o) || h->release_requested) return APH_E_REVOKED;
        if (!hub::runtime_mutations_allowed()) return APH_E_REVOKED;
        if (backend_frozen) return APH_E_NEEDS_RESTART;
        if (o->state != APH_OWNER_ENABLING && o->state != APH_OWNER_ACTIVE) return APH_E_UNAVAILABLE;
        if (h->state == APH_HOOK_ACTIVE) return APH_OK;
        if (h->operation || h->executing) return APH_PENDING;
        if (h->state != APH_HOOK_PREPARED) return APH_E_UNAVAILABLE;
        h->operation = 2; h->result = APH_PENDING;
    } __finally { ReleaseSRWLockExclusive(&registry); }
    SetEvent(wake_event);
    return APH_PENDING;
}
APH_Result APH_CALL release_hook(APH_Owner owner, APH_Hook id) {
    AcquireSRWLockExclusive(&registry);
    __try {
        Hook* h = hook_at(owner, id);
        if (!h) return APH_E_ARGUMENT;
        InterlockedExchange(&h->closed, 1);
        if (h->state == APH_HOOK_RELEASED || h->state == APH_HOOK_FAILED) return APH_OK;
        if (h->state == APH_HOOK_RETIRED_PINNED) return h->in_flight ? APH_PENDING : APH_OK;
        if (backend_frozen || h->state == APH_HOOK_NEEDS_RESTART) return APH_E_NEEDS_RESTART;
        h->release_requested = true; h->result = APH_PENDING;
    } __finally { ReleaseSRWLockExclusive(&registry); }
    SetEvent(wake_event);
    return APH_PENDING;
}
APH_Result APH_CALL enter_call(APH_Owner owner, APH_Hook id, APH_HookCall* out) {
    APH_HookCall value{};
    AcquireSRWLockShared(&registry);
    __try {
        Hook* h = hook_at(owner, id); Owner* o = owner_at(owner);
        if (!h || !o || !h->original || call_depth == MaxDepth) return APH_E_BUSY;
        bool recursive = false;
        for (unsigned i = 0; i < call_depth; ++i) if (call_stack[i].hook == id) recursive = true;
        InterlockedIncrement(&h->in_flight);
        bool native_ready = !InterlockedCompareExchange(&h->closed, 0, 0);
        bool admitted = !recursive && native_ready && admit(&o->gate);
        value = {sizeof(value), (admitted ? APH_CALL_BUSINESS : 0u) | (recursive ? APH_CALL_RECURSIVE : 0u) |
            (native_ready ? APH_CALL_NATIVE_READY : 0u), id, o->epoch, call_depth, 0};
        if (!safe_copy(out, &value, sizeof(value))) {
            if (admitted) InterlockedDecrement64(&o->gate);
            InterlockedDecrement(&h->in_flight);
            return APH_E_ARGUMENT;
        }
        call_stack[call_depth++] = {owner, id, h, admitted};
    } __finally { ReleaseSRWLockShared(&registry); }
    return APH_OK;
}
APH_Result APH_CALL leave_call(APH_Owner owner, const APH_HookCall* input) {
    APH_HookCall call{};
    if (!read_input(input, &call) || !call_depth || call.depth != call_depth - 1) return APH_E_ARGUMENT;
    CallFrame f = call_stack[call_depth - 1];
    if (f.owner != owner || f.hook != call.hook) return APH_E_ARGUMENT;
    --call_depth;
    if (f.admitted) InterlockedDecrement64(&owners[owner - 1].gate);
    // Do not read the growing index without registry. The admitted record is
    // permanent even if another thread appends hooks while this call executes.
    InterlockedDecrement(&f.record->in_flight);
    return APH_OK;
}
APH_Result APH_CALL owner_status(APH_Owner id, uint32_t bytes, APH_OwnerStatus* out) {
    APH_OwnerStatus value{};
    AcquireSRWLockShared(&registry);
    __try {
        Owner* o = owner_at(id);
        if (!o) return APH_E_ARGUMENT;
        value.size = sizeof(value); value.state = o->state; value.activation_epoch = o->epoch;
        value.result = o->result; value.in_flight = flights(&o->gate) + o->lifecycle + o->cleanup_resources;
        value.resource_count = (uint32_t)InterlockedCompareExchange(&o->extra_resources, 0, 0);
        if (InterlockedCompareExchange(&o->fault_record, 0, 0) == 2) {
            value.fault_code = o->fault_code; value.fault_phase = o->fault_phase;
            value.fault_thread = o->fault_thread; value.fault_address = o->fault_address;
            value.fault_hook = o->fault_hook;
            bool recoverable = o->recoverable && !InterlockedCompareExchange(&o->unknown_fault, 0, 0);
            value.state = recoverable ? APH_OWNER_FAULTED : APH_OWNER_NEEDS_RESTART;
            value.result = recoverable ? APH_E_FAULT : APH_E_NEEDS_RESTART;
        }
        for (const auto& h : hooks)
            if (h->owner == id && h->state != APH_HOOK_RELEASED && h->state != APH_HOOK_FAILED) ++value.resource_count;
        for(size_t i=0;i<subscription_count; ++i)
            if (subscriptions[i].owner == id && (!(read_gate(&subscriptions[i].gate) & Closed) || flights(&subscriptions[i].gate))) ++value.resource_count;
    } __finally { ReleaseSRWLockShared(&registry); }
    return output(bytes, out, value);
}
APH_Result APH_CALL subscribe(APH_Owner id, const APH_SubscribeDesc* input, APH_Subscription* out) {
    APH_Subscription zero = 0;
    APH_SubscribeDesc desc{};
    if (!safe_copy(out, &zero, sizeof(zero)) || !read_input(input, &desc)) return APH_E_ARGUMENT;
    if (desc.kind != APH_NOTICE_CAPABILITIES || desc.recovery != APH_RECOVERY_READ_ONLY || desc.reserved) return APH_E_ARGUMENT;
    HMODULE module = nullptr;
    if (!executable((void*)desc.callback, &module)) return APH_E_ARGUMENT;
    AcquireSRWLockExclusive(&registry);
    __try {
        Owner* o = owner_at(id);
        if (!usable(o)) return APH_E_REVOKED;
        if (module != o->module) return APH_E_ARGUMENT;
        size_t slot=subscription_count;
        if(o->manifest.flags&APH_PLUGIN_LIFECYCLE)for(size_t i=0;i<subscription_count;++i)
            if(subscriptions[i].owner==id&&(read_gate(&subscriptions[i].gate)&Closed)&&!flights(&subscriptions[i].gate)){slot=i;break;}
        Subscription* s = subscriptions.ensure(slot);
        if (!s) return APH_E_QUEUE_FULL;
        APH_Subscription value = subscription_tokens.issue(s,s->token);
        if (!value) return APH_E_QUEUE_FULL;
        if (!safe_copy(out, &value, sizeof(value))) { subscription_tokens.cancel(value,s->token); return APH_E_ARGUMENT; }
        subscription_tokens.forget(s->token);
        if(slot==subscription_count)++subscription_count;
        *s={};s->token=value;
        s->owner = id; s->desc = desc; s->epoch = o->epoch ? o->epoch : 1;
        s->pending_notice = true;
    } __finally { ReleaseSRWLockExclusive(&registry); }
    SetEvent(wake_event);
    hub::diagnostic("subscribe", id, 0, APH_OK);
    return APH_OK;
}
APH_Result APH_CALL unsubscribe(APH_Owner owner, APH_Subscription id) {
    AcquireSRWLockExclusive(&registry);
    __try {
        Subscription* s = subscription_tokens.find(id);
        if (!s) return APH_E_STALE;
        if (s->owner != owner) return APH_E_ARGUMENT;
        close_gate(&s->gate); s->pending_notice = false;
        return flights(&s->gate) ? APH_PENDING : APH_OK;
    } __finally { ReleaseSRWLockExclusive(&registry); }
}
APH_Result APH_CALL copy_service(APH_Owner id, uint64_t service, uint32_t major, uint32_t bytes, void* out) {
    AcquireSRWLockShared(&registry);
    __try { if (!owner_at(id)) return APH_E_ARGUMENT; }
    __finally { ReleaseSRWLockShared(&registry); }
    if (service != APH_SERVICE_HOOKS) return hub::copy_extension(id, service, major, bytes, out);
    if (major != 1) return APH_E_ABI;
    return output(bytes, (APH_HookApi*)out, hook_api);
}
void close_owner_resources(APH_Owner id, bool cleanup) {
    Owner* o = &owners[id - 1]; close_gate(&o->gate);
    for(size_t i=0;i<subscription_count; ++i) if (subscriptions[i].owner == id) close_gate(&subscriptions[i].gate);
    for (size_t i = hooks.size(); i > 0; --i) if (hooks[i - 1]->owner == id) {
        Hook* h = hooks[i - 1].get();
        // Shared provider is a module-lifetime adapter, not this owner's business
        // callback. Its listeners independently use owner admission below.
        if (h->request.mode == APH_SHARED_TYPED) continue;
        InterlockedExchange(&h->closed, 1);
        if (cleanup && h->state != APH_HOOK_RELEASED && h->state != APH_HOOK_FAILED && h->state != APH_HOOK_RETIRED_PINNED)
            h->release_requested = true;
        else if (!cleanup && h->state != APH_HOOK_RELEASED && h->state != APH_HOOK_FAILED && h->state != APH_HOOK_RETIRED_PINNED) {
            h->release_requested = false; h->operation = 0;
            h->state = APH_HOOK_NEEDS_RESTART; h->result = APH_E_NEEDS_RESTART;
        }
    }
}
void invoke_notice(Subscription* s, Owner* o) {
    APH_Notice notice{sizeof(notice), APH_NOTICE_CAPABILITIES, hub::capabilities(), s->epoch};
    __try {
        __try { s->desc.callback(s->owner, &notice, s->desc.user_data); }
        __except(APH_EntryFilter(&hook_api, s->owner, 0, GetExceptionInformation(), APH_ORIGINAL_NOT_ENTERED, s->desc.recovery)) {}
    } __finally {
        InterlockedDecrement64(&s->gate);
        InterlockedDecrement64(&o->gate);
    }
}
void dispatch_notices() {
    AcquireSRWLockShared(&registry);
    const size_t limit = subscription_count;
    ReleaseSRWLockShared(&registry);
    for (size_t i = 0; i < limit; ++i) {
        Subscription* s = nullptr; Owner* o = nullptr;
        AcquireSRWLockExclusive(&registry);
        __try {
            if (i >= subscription_count) break;
            Subscription* candidate = &subscriptions[i]; Owner* owner = owner_at(candidate->owner);
            if (candidate->pending_notice && owner->state == APH_OWNER_ACTIVE && candidate->epoch == owner->epoch && admit(&owner->gate)) {
                if (admit(&candidate->gate)) { s = candidate; o = owner; candidate->pending_notice = false; }
                else InterlockedDecrement64(&owner->gate);
            }
        } __finally { ReleaseSRWLockExclusive(&registry); }
        if (s) invoke_notice(s, o); // plugin code is NEVER called under registry
    }
}
void process_faults() {
    AcquireSRWLockExclusive(&registry);
    __try {
        for(size_t j=0;j<owner_count; ++j) {
            Owner* o = &owners[j];
            bool unknown = InterlockedCompareExchange(&o->unknown_fault, 0, 0) != 0;
            if (InterlockedCompareExchange(&o->fault_record, 0, 0) == 2 &&
                (!o->fault_handled || (unknown && o->state != APH_OWNER_NEEDS_RESTART))) {
                o->fault_handled = true;
                o->runtime_gate.fetch_or(2,std::memory_order_release);
                o->state = o->recoverable && !unknown ? APH_OWNER_FAULTED : APH_OWNER_NEEDS_RESTART;
                o->result = o->recoverable && !unknown ? APH_E_FAULT : APH_E_NEEDS_RESTART;
                manager_events::notify();
                close_owner_resources(j + 1, o->recoverable && !unknown);
                char message[384];
                _snprintf_s(message, sizeof(message), _TRUNCATE,
                    "[APH] op=fault owner=%llu hook=%llu code=0x%08x address=%llx phase=%u thread=%u result=%d\n",
                    (unsigned long long)(j + 1), (unsigned long long)o->fault_hook, o->fault_code,
                    (unsigned long long)o->fault_address, o->fault_phase, o->fault_thread, o->result);
                write_log(message);
            }
        }
    } __finally { ReleaseSRWLockExclusive(&registry); }
}
DWORD WINAPI control_thread(void*) {
    for (;;) {
        WaitForSingleObject(wake_event, 20);
        process_faults();
        // Bound this pass so concurrent requests cannot starve other services.
        AcquireSRWLockShared(&registry);
        const size_t scan_count = hooks.size();
        ReleaseSRWLockShared(&registry);
        for (size_t i = 0; i < scan_count; ++i) {
            Hook* h = nullptr; uint32_t operation = 0;
            AcquireSRWLockExclusive(&registry);
            __try {
                h = hooks[i].get();
                if (h->state == APH_HOOK_NEEDS_RESTART || h->state == APH_HOOK_RELEASED || h->state == APH_HOOK_FAILED || h->state == APH_HOOK_RETIRED_PINNED) continue;
                if (backend_frozen) {
                    h->state = APH_HOOK_NEEDS_RESTART; h->result = APH_E_NEEDS_RESTART;
                    Owner* o = &owners[h->owner - 1]; o->runtime_gate.fetch_or(2,std::memory_order_release);
                    o->state = APH_OWNER_NEEDS_RESTART; o->result = APH_E_NEEDS_RESTART;
                    close_gate(&o->gate); h->operation = 0; continue;
                }
                if (h->release_requested) {
                    if (!h->original) { h->state = APH_HOOK_RELEASED; h->result = APH_OK; h->operation = 0; continue; }
                    operation = 3; h->state = APH_HOOK_RELEASING;
                } else operation = h->operation;
                h->operation = 0; h->executing = operation != 0;
            } __finally { ReleaseSRWLockExclusive(&registry); }
            if (!operation) continue;
            APH_Result result;
            MH_STATUS backend = MH_OK;
            APH_NativeProc prepared_original = nullptr;
            unsigned char bytes[16]{};
            if (operation == 2 && (!safe_copy(bytes, (void*)h->request.target, sizeof(bytes)) || memcmp(bytes, h->entry_bytes, sizeof(bytes)))) {
                result = APH_E_FOREIGN_PATCH;
                InterlockedExchange(&backend_frozen, 1); // cannot safely disable/remove somebody else's patch
            } else result = backend_operation(operation, h, &backend, &prepared_original);
            AcquireSRWLockExclusive(&registry);
            __try {
                h->executing = false; h->backend = backend; h->result = result;
                if (operation == 1) h->original = prepared_original;
                if (result != APH_OK) {
                    bool unknown = backend_frozen || operation != 1;
                    if (unknown) {
                        InterlockedExchange(&backend_frozen, 1);
                        h->state = APH_HOOK_NEEDS_RESTART; h->result = APH_E_NEEDS_RESTART;
                        Owner* o = &owners[h->owner - 1]; o->runtime_gate.fetch_or(2,std::memory_order_release);
                        o->state = APH_OWNER_NEEDS_RESTART;
                        o->result = APH_E_NEEDS_RESTART; close_gate(&o->gate);
                    } else {
                        h->state = APH_HOOK_FAILED;
                        if (backend == MH_ERROR_ALREADY_CREATED) h->result = APH_E_FOREIGN_PATCH;
                    }
                } else if (operation == 1) h->state = APH_HOOK_PREPARED;
                else if (operation == 2) {
                    h->ever_enabled = true; h->state = APH_HOOK_ACTIVE;
                    if (!h->release_requested) InterlockedExchange(&h->closed, 0);
                } else {
                    h->state = h->ever_enabled ? APH_HOOK_RETIRED_PINNED : APH_HOOK_RELEASED;
                    if (!h->ever_enabled) h->original = nullptr;
                }
            } __finally { ReleaseSRWLockExclusive(&registry); }
            hub::diagnostic(operation == 1 ? "prepare" : operation == 2 ? "activate" : "release", h->owner, i + 1, result);
        }
        dispatch_notices();
        hub::extensions_tick();
    }
}
int lifecycle_filter(APH_Owner id, EXCEPTION_POINTERS* e) {
    // Attach/Enable may have partially mutated plugin state. Record/quarantine,
    // never call it successfully initialized or pretend to undo arbitrary writes.
    DWORD code = e->ExceptionRecord->ExceptionCode;
    record_fault(id, 0, code, (uint64_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress, APH_ORIGINAL_NOT_ENTERED, 0);
    return code == EXCEPTION_ACCESS_VIOLATION && !(e->ExceptionRecord->ExceptionFlags & EXCEPTION_NONCONTINUABLE) &&
        e->ExceptionRecord->NumberParameters >= 2 &&
        e->ExceptionRecord->ExceptionInformation[0] == 0 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

namespace hub {
APH_Owner find_owner(const char* id) {
    AcquireSRWLockShared(&registry);
    __try { for(size_t i=0;i<owner_count;++i)if(!strcmp(owners[i].manifest.id,id))return i+1; return 0; }
    __finally { ReleaseSRWLockShared(&registry); }
}
bool copy(void* dst, const void* src, size_t bytes) { return safe_copy(dst, src, bytes); }
bool ready() { return InterlockedCompareExchange(&is_ready, 0, 0) != 0; }
uint64_t capabilities() { return ready() ? APH_CAP_CORE | APH_CAP_HOOKS | APH_CAP_NOTICES : 0; }
const APH_Api* api() { return &core_api; }
const APH_HookApi* hooks_api() { return &hook_api; }
APH_Result shared_native_request(APH_Owner id, const APH_HookRequest* r, APH_Hook* out) {
    return request_hook_impl(id, r, out, true);
}
bool admit_owner(APH_Owner id, uint64_t epoch) {
    AcquireSRWLockShared(&registry);
    __try { Owner* o = owner_at(id); return o && o->epoch == epoch && admit(&o->gate); }
    __finally { ReleaseSRWLockShared(&registry); }
}
void leave_owner(APH_Owner id) { InterlockedDecrement64(&owners[id - 1].gate); }
uint64_t owner_epoch(APH_Owner id) {
    AcquireSRWLockShared(&registry);
    __try { Owner* o = owner_at(id); return o ? o->epoch : 0; }
    __finally { ReleaseSRWLockShared(&registry); }
}
bool owner_usable(APH_Owner id) {
    AcquireSRWLockShared(&registry);
    __try { return usable(owner_at(id)); }
    __finally { ReleaseSRWLockShared(&registry); }
}
bool owner_resident(APH_Owner id) {
    AcquireSRWLockShared(&registry);
    __try {return resident(owner_at(id));} __finally {ReleaseSRWLockShared(&registry);}
}
bool owner_identity(APH_Owner id, APH_Manifest* manifest, HMODULE* module) {
    AcquireSRWLockShared(&registry);
    __try {
        auto o = owner_at(id);
        if (!o) return false;
        *manifest = o->manifest; *module = o->module;
        return true;
    } __finally { ReleaseSRWLockShared(&registry); }
}
bool owner_runtime_open(APH_Owner id) {
    const Owner* o = id ? owners.get(id - 1) : nullptr;
    return o && o->runtime_gate.load(std::memory_order_acquire)==1;
}
unsigned char owner_failure(APH_Owner id) {
    const Owner* o = id ? owners.get(id - 1) : nullptr;
    if (!o || !o->runtime_gate.load(std::memory_order_acquire)) return 0;
    const auto state = o->state.load(std::memory_order_acquire);
    if (state == APH_OWNER_NEEDS_RESTART) return 2;
    if (state == APH_OWNER_FAILED || state == APH_OWNER_FAULTED) return 1;
    return 0;
}
uint32_t owner_flags(APH_Owner id) {
    AcquireSRWLockShared(&registry);
    __try {Owner* o=owner_at(id);return o?o->manifest.flags:0;} __finally {ReleaseSRWLockShared(&registry);}
}
bool admit_module(APH_Owner id) {
    AcquireSRWLockShared(&registry);
    __try {Owner* o=owner_at(id);if(!resident(o)||!(o->manifest.flags&APH_PLUGIN_MODULE_OBSERVER))return false;
        InterlockedIncrement(&o->module_flights);return true;} __finally {ReleaseSRWLockShared(&registry);}
}
void leave_module(APH_Owner id){InterlockedDecrement(&owners[id-1].module_flights);}
bool admit_cleanup_resource(APH_Owner id,uint64_t epoch) {
    if(!hub::services::access_cleanup(id,epoch))return false;
    InterlockedIncrement(&owners[id-1].cleanup_resources);return true;
}
void leave_cleanup_resource(APH_Owner id){InterlockedDecrement(&owners[id-1].cleanup_resources);}
uint32_t owner_business_flights(APH_Owner id) {
    AcquireSRWLockShared(&registry);
    __try {Owner* o=owner_at(id);return o?flights(&o->gate)+o->lifecycle+o->cleanup_resources:UINT32_MAX;}
    __finally {ReleaseSRWLockShared(&registry);}
}
APH_Result suspend_owner(APH_Owner id) {
    AcquireSRWLockExclusive(&registry);
    __try {Owner* o=owner_at(id);if(!resident(o)||!(o->manifest.flags&APH_PLUGIN_LIFECYCLE))return APH_E_REVOKED;
        if(o->state==APH_OWNER_DISABLED)return APH_OK;
        if(o->state==APH_OWNER_DISABLING)return APH_PENDING;
        if(o->state!=APH_OWNER_ACTIVE&&o->state!=APH_OWNER_ENABLING)return APH_E_BUSY;
        close_gate(&o->gate);o->state=APH_OWNER_DISABLING;o->result=APH_PENDING;
        for(size_t i=0;i<subscription_count;++i)if(subscriptions[i].owner==id){close_gate(&subscriptions[i].gate);subscriptions[i].pending_notice=false;}
    } __finally {ReleaseSRWLockExclusive(&registry);}
    SetEvent(wake_event);return APH_PENDING;
}
APH_Result disable_drained(APH_Owner id) {
    AcquireSRWLockExclusive(&registry);
    __try {Owner* o=owner_at(id);if(!resident(o)||o->state!=APH_OWNER_DISABLING)return APH_E_REVOKED;
        if(flights(&o->gate)||o->lifecycle||o->cleanup_resources)return APH_PENDING;
        o->state=APH_OWNER_DISABLED;o->result=APH_OK;return APH_OK;
    } __finally {ReleaseSRWLockExclusive(&registry);}
}
void quarantine_owner(APH_Owner id,APH_Result result) {
    bool changed=false;
    AcquireSRWLockExclusive(&registry);
    __try {Owner* o=owner_at(id);if(o&&(o->state!=APH_OWNER_NEEDS_RESTART||o->result!=result)){
        o->runtime_gate.fetch_or(2,std::memory_order_release);
        close_gate(&o->gate);o->state=APH_OWNER_NEEDS_RESTART;o->result=result;changed=true;}}
    __finally {ReleaseSRWLockExclusive(&registry);}
    // Reasserting a terminal state must not keep the control worker awake.
    if(changed)SetEvent(wake_event);
}
bool owner_callback(APH_Owner id, const void* callback) {
    HMODULE module = nullptr;
    if (!executable(callback, &module)) return false;
    AcquireSRWLockShared(&registry);
    __try { Owner* o = owner_at(id); return o && o->module == module; }
    __finally { ReleaseSRWLockShared(&registry); }
}
void resource_delta(APH_Owner id, int32_t delta) {
    Owner* o = id ? owners.get(id - 1) : nullptr;
    if (o && o->runtime_gate.load(std::memory_order_acquire)) InterlockedExchangeAdd(&o->extra_resources, delta);
}
void log_line(const char* message) { write_log(message); }
void diagnostic(const char* operation, APH_Owner owner, APH_Hook hook, APH_Result result) {
    if(result<0) {
        APH_HookRequest request{};uint32_t backend=0;
        AcquireSRWLockShared(&registry);
        __try {auto h=hook_at(owner,hook);if(h){request=h->request;backend=h->backend;}}
        __finally {ReleaseSRWLockShared(&registry);}
        APH_ErrorDetail prior{};
        if(hook||hub::services::diagnostics_api()->last_error(owner,sizeof(prior),&prior)!=APH_OK)
            error_detail(owner,operation,result,request.diagnostic_name,(uint64_t)request.target,hook,backend,0,
                backend?MH_StatusToString((MH_STATUS)backend):APH_ResultName(result));
    }
    char text[256];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "[APH] op=%s owner=%llu hook=%llu result=%d thread=%lu\n",
                operation, (unsigned long long)owner, (unsigned long long)hook, result, GetCurrentThreadId());
    write_log(text);
}
APH_Result initialize() {
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, path, _countof(path));
    if (length && length < _countof(path)) {
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash && size_t(slash - path) + 30 < _countof(path)) {
            wcscpy_s(slash, _countof(path) - (slash - path), L"\\plugin\\applepie_core.log");
            core_log.open(path);
        }
    }
    MH_STATUS status = MH_OK;
    APH_Result result = backend_operation(0, nullptr, &status);
    if (result != APH_OK) return result;
    LARGE_INTEGER ticks{}; QueryPerformanceCounter(&ticks);
    instance = (uint64_t)ticks.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32);
    core_api = {sizeof(core_api), 1, APH_ABI_MINOR, instance, copy_service, owner_status, subscribe, unsubscribe};
    hook_api = {sizeof(hook_api), 1, 0, instance, request_hook, query_hook, activate_hook, release_hook, enter_call, leave_call, record_fault};
    wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake_event) return APH_E_UNAVAILABLE;
    HANDLE worker = CreateThread(nullptr, 0, control_thread, nullptr, 0, nullptr);
    if (!worker) return APH_E_UNAVAILABLE;
    CloseHandle(worker);
    InterlockedExchange(&is_ready, 1);
    return APH_OK;
}
APH_Owner add_owner(const APH_Manifest& manifest, HMODULE module) {
    AcquireSRWLockExclusive(&registry);
    __try {
        for (size_t i = 0; i < owner_count; ++i) if (!strcmp(owners[i].manifest.id, manifest.id)) return 0;
        Owner* o = owners.ensure(owner_count);
        if (!o) return 0;
        o->manifest = manifest; o->module = module; o->gate = Closed;
        o->state = APH_OWNER_ATTACHED; o->result = APH_PENDING;
        o->runtime_gate.fetch_or(1,std::memory_order_release);
        return ++owner_count;
    } __finally { ReleaseSRWLockExclusive(&registry); }
}
void set_owner_state(APH_Owner id, uint32_t state, APH_Result result) {
    bool changed = false;
    AcquireSRWLockExclusive(&registry);
    __try { Owner* o = owner_at(id); if (usable(o)) {
        changed = o->state != state || o->result != result;
        if(APH_OwnerHasFault(state))o->runtime_gate.fetch_or(2,std::memory_order_release);
        o->state = state; o->result = result; } }
    __finally { ReleaseSRWLockExclusive(&registry); }
    if (changed) manager_events::notify();
}
uint64_t start_enable(APH_Owner id) {
    uint64_t epoch = 0;
    AcquireSRWLockExclusive(&registry);
    __try {
        Owner* o = owner_at(id);
        if (resident(o) && ((!o->epoch && usable(o)) ||
            (o->state==APH_OWNER_DISABLED&&(o->manifest.flags&APH_PLUGIN_LIFECYCLE))) &&
            !flights(&o->gate)&&!o->lifecycle&&!o->cleanup_resources) {
            epoch = ++o->epoch; o->state = APH_OWNER_ENABLING;o->result=APH_PENDING;
        }
    } __finally { ReleaseSRWLockExclusive(&registry); }
    return epoch;
}
bool commit_enable(APH_Owner id, uint64_t epoch, APH_Result result) {
    bool accepted = false;
    AcquireSRWLockExclusive(&registry);
    __try {
        Owner* o = owner_at(id);
        if (!epoch || !usable(o) || o->state != APH_OWNER_ENABLING || o->epoch != epoch) return false;
        if (result == APH_OK) {
            o->state = APH_OWNER_ACTIVE; o->result = APH_OK; InterlockedExchange64(&o->gate, 0);
        } else if (result != APH_PENDING) {
            o->runtime_gate.fetch_or(2, std::memory_order_release);
            o->state = APH_OWNER_FAILED; o->result = result;
            close_owner_resources(id, true);
        }
        accepted = true;
    } __finally { ReleaseSRWLockExclusive(&registry); }
    SetEvent(wake_event);
    manager_events::notify();
    return accepted;
}
void fail_owner(APH_Owner id, APH_Result result) {
    AcquireSRWLockExclusive(&registry);
    __try {
        Owner* o = owner_at(id);
        if (o) {
            o->runtime_gate.fetch_or(2,std::memory_order_release);
            bool unknown = InterlockedCompareExchange(&o->fault_record, 0, 0) != 0 || o->state == APH_OWNER_NEEDS_RESTART;
            o->state = unknown ? APH_OWNER_NEEDS_RESTART : APH_OWNER_FAILED; o->result = result;
            close_owner_resources(id, !unknown);
        }
    } __finally { ReleaseSRWLockExclusive(&registry); }
    SetEvent(wake_event);
    manager_events::notify();
}
APH_Result attach(APH_Owner id, APH_AttachFn fn) {
    APH_AttachArgs args{sizeof(args), 0, id, &core_api};
    InterlockedIncrement(&owners[id - 1].lifecycle);
    __try {
        __try { return fn(&args); }
        __except(lifecycle_filter(id, GetExceptionInformation())) { return APH_E_NEEDS_RESTART; }
    } __finally { InterlockedDecrement(&owners[id - 1].lifecycle); }
}
APH_Result enable(APH_Owner id, APH_EnableFn fn, uint64_t epoch) {
    AcquireSRWLockExclusive(&registry);
    __try {
        Owner* o = owner_at(id);
        if (!epoch || !usable(o) || o->state != APH_OWNER_ENABLING || o->epoch != epoch) return APH_E_STALE;
        // Cancellation and callback admission are one transaction. Restoration
        // must wait for an admitted Enable, including its synchronous prefix.
        InterlockedIncrement(&o->lifecycle);
    } __finally { ReleaseSRWLockExclusive(&registry); }
    __try {
        __try { return fn(id, epoch); }
        __except(lifecycle_filter(id, GetExceptionInformation())) { return APH_E_NEEDS_RESTART; }
    } __finally { InterlockedDecrement(&owners[id - 1].lifecycle); }
}
}
extern "C" APH_Result APH_CALL APH_GetApi(uint32_t major, uint32_t minor, uint32_t bytes, APH_Api* out) {
    if (bytes < sizeof(APH_Api)) return APH_E_ABI;
    APH_Api empty{};
    if (!safe_copy(out, &empty, sizeof(empty))) return APH_E_ARGUMENT;
    if (major != 1 || minor > APH_ABI_MINOR) return APH_E_ABI;
    if (!hub::ready()) return APH_E_UNAVAILABLE;
    return output(bytes, out, core_api);
}
