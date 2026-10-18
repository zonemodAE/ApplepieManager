#define NOMINMAX
#include "hub_internal.h"
#include <atomic>
#include <cstring>
#include <cstdio>
namespace {
SRWLOCK lock = SRWLOCK_INIT;
std::atomic<uint32_t> state{APH_RUNTIME_WAITING};
static_assert(std::atomic<uint32_t>::is_always_lock_free,"runtime probe is lock-free");
APH_RuntimeSnapshot runtime{sizeof(runtime), APH_RUNTIME_WAITING, 1, 1, 0, 0, 0, 0};
bool target_changing=false;
void invalidate() { ++runtime.target_generation; runtime.target_identity=0; runtime.target_ready=0;target_changing=false; }
void read_snapshot(APH_RuntimeSnapshot& value,bool cleanup) {
    AcquireSRWLockShared(&lock);
    __try { value=runtime;if(target_changing&&!cleanup)value.target_ready=0; }
    __finally { ReleaseSRWLockShared(&lock); }
}
APH_Result APH_CALL snapshot(uint32_t bytes, APH_RuntimeSnapshot* out) {
    APH_RuntimeSnapshot value{};read_snapshot(value,hub::services::restoring_runtime());
    return hub::services::output(bytes,out,value);
}
APH_Result APH_CALL claim_source(APH_Owner owner) {
    if(!hub::owner_usable(owner))return APH_E_REVOKED;
    AcquireSRWLockExclusive(&lock);
    __try {
        if(runtime.state>=APH_RUNTIME_STOPPING)return APH_E_REVOKED;
        if(runtime.source&&runtime.source!=owner)return APH_E_CONFLICT;
        runtime.source=owner;return APH_OK;
    } __finally {ReleaseSRWLockExclusive(&lock);}
}
APH_Result APH_CALL update(APH_Owner owner,const APH_RuntimeUpdate* input) {
    APH_RuntimeUpdate u{};
    if(!hub::services::input(input,&u)||u.reserved)return APH_E_ARGUMENT;
    // The selected source's pinned native shutdown adapter outlives its
    // business admission. It may still close the runtime after owner failure.
    if(u.operation<APH_RUNTIME_BEGIN_STOP&&!hub::owner_resident(owner))return APH_E_REVOKED;
    AcquireSRWLockExclusive(&lock);
    __try {
        if(!owner||runtime.source!=owner)return APH_E_CONFLICT;
        if(runtime.state==APH_RUNTIME_DEAD)return APH_E_REVOKED;
        switch(u.operation) {
        case APH_RUNTIME_SET_LIVE:
            if(runtime.state!=APH_RUNTIME_WAITING)return APH_E_CONFLICT;
            if(!u.verified_thread||u.verified_thread!=GetCurrentThreadId())return APH_E_THREAD;
            runtime.verified_thread=u.verified_thread;runtime.state=APH_RUNTIME_LIVE;break;
        case APH_TARGET_INVALIDATE:
            invalidate();break;
        case APH_TARGET_BIND:
            if(runtime.state!=APH_RUNTIME_LIVE)return APH_E_UNAVAILABLE;
            if(!u.target_identity)return APH_E_ARGUMENT;
            if(u.verified_thread!=runtime.verified_thread||u.verified_thread!=GetCurrentThreadId())return APH_E_THREAD;
            if(target_changing)return APH_PENDING; // Never rebind beneath an old-target restore.
            if(!runtime.target_ready||runtime.target_identity!=u.target_identity)invalidate();
            runtime.target_identity=u.target_identity;runtime.target_ready=1;break;
        case APH_RUNTIME_BEGIN_STOP:
            if(runtime.state<APH_RUNTIME_STOPPING){invalidate();++runtime.runtime_epoch;runtime.state=APH_RUNTIME_STOPPING;runtime.verified_thread=0;}
            break;
        case APH_RUNTIME_SET_DEAD:
            if(runtime.state!=APH_RUNTIME_STOPPING)return APH_E_CONFLICT;
            invalidate();runtime.state=APH_RUNTIME_DEAD;break;
        default:return APH_E_ARGUMENT;
        }
        state.store(runtime.state,std::memory_order_release);return APH_OK;
    } __finally {ReleaseSRWLockExclusive(&lock);}
}
APH_Result APH_CALL validate(APH_Owner owner,const APH_Access* input) {
    APH_Access a{};APH_RuntimeSnapshot s{};
    if(!hub::services::input(input,&a)||(a.flags&~7u))return APH_E_ARGUMENT;
    if((!hub::owner_usable(owner)&&!hub::services::access_cleanup(owner,a.activation_epoch))||!a.activation_epoch||a.activation_epoch!=hub::owner_epoch(owner))return APH_E_STALE;
    read_snapshot(s,hub::services::access_cleanup(owner,a.activation_epoch));
    if(a.flags) {
        if(s.state!=APH_RUNTIME_LIVE||!s.source||!hub::owner_resident(s.source))return APH_E_UNAVAILABLE;
        if(a.runtime_epoch!=s.runtime_epoch)return APH_E_STALE;
        if((a.flags&APH_ACCESS_TARGET)&&(!s.target_ready||a.target_generation!=s.target_generation))return APH_E_STALE;
        if((a.flags&APH_ACCESS_GAME_THREAD)&&(!s.verified_thread||s.verified_thread!=GetCurrentThreadId()))return APH_E_THREAD;
    }
    return APH_OK;
}
APH_Result APH_CALL probe_open(APH_Owner owner) {
    if(!hub::owner_runtime_open(owner))return APH_E_REVOKED;
    return state.load(std::memory_order_acquire)<APH_RUNTIME_STOPPING?APH_OK:APH_E_UNAVAILABLE;
}
const APH_RuntimeApi api{sizeof(api),1,snapshot,claim_source,update,validate,probe_open};
}
namespace hub::services {
const APH_RuntimeApi* runtime_api(){return &::api;}
void cleanup_runtime_snapshot(APH_RuntimeSnapshot* out){read_snapshot(*out,true);}
APH_Result begin_target_change(APH_Owner source,APH_RuntimeSnapshot* before) {
    AcquireSRWLockExclusive(&lock);
    __try {
        if(runtime.source!=source||runtime.verified_thread!=GetCurrentThreadId())return APH_E_THREAD;
        if(runtime.state!=APH_RUNTIME_LIVE)return APH_E_UNAVAILABLE;
        // Preserve the OLD cleanup context, but expose no target to ordinary
        // callbacks/commands until every captured writer has restored.
        target_changing=true;*before=runtime;return APH_OK;
    } __finally {ReleaseSRWLockExclusive(&lock);}
}
APH_Result finish_target_change(const APH_RuntimeSnapshot& before) {
    AcquireSRWLockExclusive(&lock);
    __try {
        if(runtime.state!=APH_RUNTIME_LIVE||runtime.source!=before.source||
           runtime.runtime_epoch!=before.runtime_epoch||runtime.target_generation!=before.target_generation)
            return APH_E_STALE;
        invalidate();return APH_OK; // Also revokes queued commands for a same-identity rebind.
    } __finally {ReleaseSRWLockExclusive(&lock);}
}
}
namespace hub {
bool runtime_mutations_allowed(){return state.load(std::memory_order_acquire)<APH_RUNTIME_STOPPING;}
void extensions_tick() {
    hub::services::shared_tick();hub::services::window_tick();hub::services::tick();
    // Pointer leases are deliberately excluded: a temporary wrapper may still
    // be running. Its __finally owns conditional restore and the admission count.
}
APH_Result copy_extension(APH_Owner owner,uint64_t service,uint32_t major,uint32_t bytes,void* out) {
    if(!owner_usable(owner))return APH_E_REVOKED;
    if(major!=1)return APH_E_ABI;
    if(service==APH_SERVICE_SHARED)return hub::services::output(bytes,(APH_SharedApi*)out,*hub::services::shared_api());
    if(service==APH_SERVICE_POINTERS)return hub::services::output(bytes,(APH_PointerApi*)out,*hub::services::pointer_api());
    if(service==APH_SERVICE_RUNTIME) {
        // Preserve the original 40-byte service v1 prefix for already built
        // plugins. New clients get the optional probe; no write past old size.
        if(bytes<APH_RUNTIME_API_V1_PREFIX_BYTES)return APH_E_ABI;
        auto value=*hub::services::runtime_api();
        const size_t copied=bytes<sizeof(value)?APH_RUNTIME_API_V1_PREFIX_BYTES:sizeof(value);
        value.size=(uint32_t)copied;
        return hub::copy(out,&value,copied)?APH_OK:APH_E_ARGUMENT;
    }
    if(service==APH_SERVICE_WINDOWS)return hub::services::output(bytes,(APH_WindowApi*)out,*hub::services::window_api());
    if(service==APH_SERVICE_LIFECYCLE)return hub::services::output(bytes,(APH_LifecycleApi*)out,*hub::services::lifecycle_api());
    if(service==APH_SERVICE_WRITES)return hub::services::output(bytes,(APH_WriteApi*)out,*hub::services::write_api());
    if(service==APH_SERVICE_PLUGINS)return hub::services::output(bytes,(APH_PluginServiceApi*)out,*hub::services::plugin_api());
    if(service==APH_SERVICE_DIAGNOSTICS)return hub::services::output(bytes,(APH_DiagnosticsApi*)out,*hub::services::diagnostics_api());
    if(service==APH_SERVICE_CONFIG) {
        if(bytes<APH_CONFIG_API_V1_PREFIX_BYTES)return APH_E_ABI;
        auto value=*hub::services::config_api();
        const size_t copied=bytes<sizeof(value)?APH_CONFIG_API_V1_PREFIX_BYTES:sizeof(value);
        value.size=(uint32_t)copied;
        return hub::copy(out,&value,copied)?APH_OK:APH_E_ARGUMENT;
    }
    return APH_E_UNAVAILABLE;
}
}


namespace {
SRWLOCK detail_lock=SRWLOCK_INIT;
hub::ResidentTable<APH_ErrorDetail> errors;
APH_Result APH_CALL last_error(APH_Owner owner,uint32_t bytes,APH_ErrorDetail* out) {
    if(!owner)return APH_E_ARGUMENT;
    APH_ErrorDetail d{};
    AcquireSRWLockShared(&detail_lock);if(auto e=errors.get(owner))d=*e;ReleaseSRWLockShared(&detail_lock);
    if(!d.size)return APH_E_UNAVAILABLE;
    return hub::services::output(bytes,out,d);
}
APH_Result APH_CALL report_error(APH_Owner owner,const APH_ErrorDetail* input) {
    APH_ErrorDetail d{};
    if(!owner||!hub::services::input(input,&d)||d.result>=0||
        !memchr(d.operation,0,sizeof(d.operation))||!memchr(d.resource,0,sizeof(d.resource))||
        !memchr(d.message,0,sizeof(d.message)))return APH_E_ARGUMENT;
    APH_OwnerStatus status{};if(hub::api()->owner_status(owner,sizeof(status),&status)!=APH_OK)return APH_E_ARGUMENT;
    d.size=sizeof(d);d.timestamp_ms=GetTickCount64();
    AcquireSRWLockExclusive(&detail_lock);
    if(!errors.ensure(owner)){ReleaseSRWLockExclusive(&detail_lock);return APH_E_QUEUE_FULL;}
    auto previous=errors[owner];previous.timestamp_ms=d.timestamp_ms;
    const bool repeated=memcmp(&previous,&d,sizeof(d))==0;
    errors[owner]=d;ReleaseSRWLockExclusive(&detail_lock);
    if(repeated)return APH_OK;
    char line[640];_snprintf_s(line,sizeof(line),_TRUNCATE,
        "[APH] error owner=%llu op=%s resource=%s target=%p result=%d (%s) backend=%u win32=%u detail=%s\n",
        owner,d.operation,d.resource,(void*)d.target,d.result,APH_ResultName(d.result),d.backend_status,d.win32_error,d.message);
    hub::log_line(line);return APH_OK;
}
const APH_DiagnosticsApi detail_api{sizeof(detail_api),1,last_error,report_error};

// Service records are reusable only after admitted callbacks leave. Serial
// handles and provider epochs make cached handles stale across republication.
SRWLOCK service_lock=SRWLOCK_INIT;
struct PluginService {APH_Owner owner;APH_PluginService serial;APH_ServiceDesc desc;uint32_t flights;bool closed;};
hub::ResidentTable<PluginService> service_records;
uint64_t service_serial;
__declspec(thread) unsigned service_depth;
bool service_active(const PluginService& r) {
    APH_OwnerStatus s{};
    return r.owner&&!r.closed&&hub::api()->owner_status(r.owner,sizeof(s),&s)==APH_OK&&
        s.state==APH_OWNER_ACTIVE&&s.activation_epoch==r.desc.activation_epoch;
}
APH_Result APH_CALL publish_service(APH_Owner owner,const APH_ServiceDesc* input,APH_PluginService* out) {
    APH_ServiceDesc d{};APH_PluginService zero=0;
    if(!hub::copy(out,&zero,sizeof(zero))||!hub::services::input(input,&d)||
        !d.name[0]||!memchr(d.name,0,sizeof(d.name))||!d.major||d.reserved||
        d.thread_policy>APH_SERVICE_GAME_THREAD||d.recovery>APH_RECOVERY_READ_ONLY||
        !hub::owner_callback(owner,(void*)d.invoke))return APH_E_ARGUMENT;
    if(!hub::runtime_mutations_allowed())return APH_E_UNAVAILABLE;
    AcquireSRWLockExclusive(&service_lock);
    __try {
        APH_OwnerStatus s{};
        if(hub::api()->owner_status(owner,sizeof(s),&s)!=APH_OK||s.activation_epoch!=d.activation_epoch||
            !d.activation_epoch||(s.state!=APH_OWNER_ENABLING&&s.state!=APH_OWNER_ACTIVE))return APH_E_REVOKED;
        PluginService* free=nullptr;
        for(auto& r:service_records) {
            if(r.owner==owner&&!r.closed&&r.desc.activation_epoch==d.activation_epoch&&
                r.desc.major==d.major&&!strcmp(r.desc.name,d.name))return APH_E_DUPLICATE;
            if(!free&&!r.flights&&(!r.owner||r.closed||(!service_active(r)&&
                (r.owner!=owner||r.desc.activation_epoch!=d.activation_epoch)))) {
                APH_OwnerStatus prior{};
                // Other plugins may still be preparing publications in Enabling.
                if(!r.owner||r.closed||hub::api()->owner_status(r.owner,sizeof(prior),&prior)!=APH_OK||
                    prior.activation_epoch!=r.desc.activation_epoch||
                    (prior.state!=APH_OWNER_ACTIVE&&prior.state!=APH_OWNER_ENABLING))free=&r;
            }
        }
        if(!free)free=service_records.ensure(service_records.size());
        if(!free||service_serial==UINT64_MAX)return APH_E_QUEUE_FULL;
        const auto serial=++service_serial;
        if(!hub::copy(out,&serial,sizeof(serial)))return APH_E_ARGUMENT;
        *free={owner,serial,d,0,false};return APH_OK;
    } __finally {ReleaseSRWLockExclusive(&service_lock);}
}
APH_Result APH_CALL withdraw_service(APH_Owner owner,APH_PluginService serial) {
    AcquireSRWLockExclusive(&service_lock);
    __try {for(auto& r:service_records)if(r.serial==serial&&r.owner==owner){r.closed=true;return r.flights?APH_PENDING:APH_OK;}return APH_E_STALE;}
    __finally {ReleaseSRWLockExclusive(&service_lock);}
}
APH_Result APH_CALL find_service(APH_Owner owner,const APH_ServiceQuery* input,APH_PluginService* out) {
    APH_ServiceQuery q{};APH_PluginService zero=0;
    if(!hub::copy(out,&zero,sizeof(zero))||!hub::services::input(input,&q)||q.reserved||!q.major||
        !q.provider[0]||!q.name[0]||!memchr(q.provider,0,sizeof(q.provider))||!memchr(q.name,0,sizeof(q.name)))return APH_E_ARGUMENT;
    if(!hub::owner_usable(owner)||!hub::runtime_mutations_allowed())return APH_E_REVOKED;
    const auto provider=hub::find_owner(q.provider);APH_Result result=APH_E_UNAVAILABLE;
    AcquireSRWLockShared(&service_lock);
    __try {
        for(const auto& r:service_records)if(r.owner==provider&&!strcmp(r.desc.name,q.name)&&service_active(r)) {
            result=APH_E_VERSION;
            if(r.desc.major==q.major&&r.desc.minor>=q.min_minor)
                return hub::copy(out,&r.serial,sizeof(r.serial))?APH_OK:APH_E_ARGUMENT;
        }
        return result;
    } __finally {ReleaseSRWLockShared(&service_lock);}
}
APH_Result invoke_service_body(const PluginService& r,uint32_t operation,const void* in,uint32_t in_bytes,void* out,uint32_t out_bytes) {
    APH_Result result=APH_E_FAULT;
    __try {result=r.desc.invoke(r.desc.context,operation,in,in_bytes,out,out_bytes);}
    __except(APH_EntryFilter(hub::hooks_api(),r.owner,0,GetExceptionInformation(),APH_ORIGINAL_NOT_ENTERED,r.desc.recovery)){}
    return result;
}
APH_Result APH_CALL invoke_service(APH_Owner owner,uint64_t epoch,APH_PluginService serial,uint32_t operation,
    const void* input,uint32_t input_bytes,void* output,uint32_t output_bytes) {
    if(input_bytes>APH_SERVICE_MAX_BYTES||output_bytes>APH_SERVICE_MAX_BYTES||
        (input_bytes&&!input)||(output_bytes&&!output))return APH_E_ARGUMENT;
    if(service_depth>=16)return APH_E_BUSY;
    if(!hub::runtime_mutations_allowed())return APH_E_UNAVAILABLE;
    // Stack copies isolate the synchronous POD contract; nothing is queued or retained.
    alignas(16) unsigned char in[APH_SERVICE_MAX_BYTES],out[APH_SERVICE_MAX_BYTES];
    if(output_bytes)memset(out,0,output_bytes);
    if(input_bytes&&!hub::copy(in,input,input_bytes))return APH_E_ARGUMENT;
    PluginService captured{};PluginService* record=nullptr;
    AcquireSRWLockExclusive(&service_lock);
    __try {
        for(auto& r:service_records)if(r.serial==serial&&serial){record=&r;break;}
        if(!record||!service_active(*record))return APH_E_STALE;
        if(record->desc.thread_policy==APH_SERVICE_GAME_THREAD) {
            APH_RuntimeSnapshot s{};snapshot(sizeof(s),&s);
            if(s.state!=APH_RUNTIME_LIVE||!s.source||!hub::owner_resident(s.source))return APH_E_UNAVAILABLE;
            if(s.verified_thread!=GetCurrentThreadId())return APH_E_THREAD;
        }
        if(!hub::admit_owner(owner,epoch))return APH_E_REVOKED;
        if(!hub::admit_owner(record->owner,record->desc.activation_epoch)){hub::leave_owner(owner);return APH_E_REVOKED;}
        ++record->flights;captured=*record;
    } __finally {ReleaseSRWLockExclusive(&service_lock);}
    APH_Result result=APH_E_FAULT;
    ++service_depth;
    __try {
        result=invoke_service_body(captured,operation,in,input_bytes,out,output_bytes);
        if(result==APH_OK&&output_bytes&&!hub::copy(output,out,output_bytes))result=APH_E_ARGUMENT;
    } __finally {
        --service_depth;
        AcquireSRWLockExclusive(&service_lock);--record->flights;ReleaseSRWLockExclusive(&service_lock);
        hub::leave_owner(captured.owner);hub::leave_owner(owner);
    }
    if(result<0)hub::error_detail(captured.owner,"service-invoke",result,captured.desc.name);
    return result;
}
const APH_PluginServiceApi plugin_services{sizeof(plugin_services),1,publish_service,withdraw_service,find_service,invoke_service};
}
namespace hub::services {
const APH_PluginServiceApi* plugin_api(){return &plugin_services;}
const APH_DiagnosticsApi* diagnostics_api(){return &detail_api;}
}
namespace hub {
void error_detail(APH_Owner owner,const char* operation,APH_Result result,const char* resource,uint64_t target,
    APH_Hook hook,uint32_t backend,uint32_t win32,const char* message) {
    APH_ErrorDetail d{};d.size=sizeof(d);d.result=result;d.target=target;d.hook=hook;
    d.backend_status=backend;d.win32_error=win32;
    strncpy_s(d.operation,operation,_TRUNCATE);strncpy_s(d.resource,resource,_TRUNCATE);strncpy_s(d.message,message,_TRUNCATE);
    report_error(owner,&d);
}
}
