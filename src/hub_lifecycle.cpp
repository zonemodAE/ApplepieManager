#define NOMINMAX
#include "hub_internal.h"
namespace {
struct Life {APH_LifecycleDesc desc;uint64_t epoch,deadline;bool registered,notified,restoring,closing;};
struct Writer {APH_Owner owner;APH_WriteLease id;APH_Access access;uint32_t state,flights;APH_Result result;uint64_t deadline;};
struct Frame {APH_WriteCall call;bool cleanup;};
SRWLOCK lock=SRWLOCK_INIT;
hub::ResidentTable<Life> lives;Writer writers[2]{};uint64_t next_lease,next_call;
bool target_changing=false;APH_RuntimeSnapshot target_before{};
__declspec(thread) Frame frames[32];
__declspec(thread) unsigned depth;
__declspec(thread) APH_Owner restoring_owner;
__declspec(thread) uint64_t restoring_epoch;
__declspec(thread) unsigned restoring_domains;
unsigned index_of(unsigned domain){return domain==APH_ANIMATION?0:domain==APH_CAMERA?1:2;}
uint32_t timeout(APH_Owner o){auto l=lives.get(o);return l&&l->registered?l->desc.timeout_ms:10000;}
APH_Result APH_CALL registration(APH_Owner o,const APH_LifecycleDesc* input) {
    APH_LifecycleDesc d{};
    if(!o||!(hub::owner_flags(o)&APH_PLUGIN_LIFECYCLE)||!hub::owner_usable(o)||!hub::services::input(input,&d))return APH_E_ARGUMENT;
    if(d.timeout_ms<100||d.timeout_ms>60000||!hub::owner_callback(o,(void*)d.restore)||
        (d.disabling&&!hub::owner_callback(o,(void*)d.disabling)))return APH_E_ARGUMENT;
    AcquireSRWLockExclusive(&lock);
    __try {if(!lives.ensure(o))return APH_E_QUEUE_FULL;
        Life& l=lives[o];if(l.registered)return APH_E_DUPLICATE;l.desc=d;l.registered=true;return APH_OK;}
    __finally {ReleaseSRWLockExclusive(&lock);}
}
APH_Result APH_CALL complete_enable(APH_Owner o,uint64_t epoch,APH_Result result) {
    if(!epoch||result==APH_PENDING)return APH_E_STALE;
    const APH_Result completed=hub::finish_enable(o,result,epoch);
    hub::diagnostic("enable-complete",o,0,completed);return completed;
}
// Lock order is lifecycle/writers -> core registry. Never invoke client code
// under either lock. Disable and the final write admission share this lock.
void close_writes_locked(APH_Owner o) {
    for(auto& w:writers)if(w.owner==o&&w.state==APH_WRITE_OWNED){w.state=APH_WRITE_RELEASING;w.deadline=GetTickCount64()+timeout(o);}
}
APH_Result APH_CALL disable(APH_Owner o) {
    if(!hub::services::registered(o))return APH_E_UNAVAILABLE;
    AcquireSRWLockExclusive(&lock);
    __try {
        APH_OwnerStatus s{};hub::api()->owner_status(o,sizeof(s),&s);
        if(s.state==APH_OWNER_DISABLING)return APH_PENDING;
        APH_Result result=hub::suspend_owner(o);if(result!=APH_PENDING)return result;
        Life& l=lives[o];l.epoch=s.activation_epoch;l.deadline=GetTickCount64()+l.desc.timeout_ms;
        l.notified=false;l.closing=true;close_writes_locked(o);
    }
    __finally {ReleaseSRWLockExclusive(&lock);}
    hub::services::epoch_close(o);
    AcquireSRWLockExclusive(&lock);lives[o].closing=false;ReleaseSRWLockExclusive(&lock);
    return APH_PENDING;
}
uint32_t APH_CALL restoring(APH_Owner o){return restoring_owner==o?restoring_domains|0x80000000u:0;}
int cleanup_filter(APH_Owner o,EXCEPTION_POINTERS* e) {
    hub::hooks_api()->record_fault(o,0,e->ExceptionRecord->ExceptionCode,(uint64_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress,APH_ORIGINAL_NOT_ENTERED,0);
    return e->ExceptionRecord->ExceptionCode==EXCEPTION_ACCESS_VIOLATION&&
        !(e->ExceptionRecord->ExceptionFlags&EXCEPTION_NONCONTINUABLE)&&e->ExceptionRecord->NumberParameters>=2&&
        e->ExceptionRecord->ExceptionInformation[0]==0?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH;
}
APH_Result invoke_restore(APH_Owner o,const APH_Cleanup* c,APH_RestoreFn fn,void* context) {
    APH_Result result=APH_E_NEEDS_RESTART;
    __try {result=fn(o,c,context);} __except(cleanup_filter(o,GetExceptionInformation())){}
    return result;
}
void uncertain_locked(APH_Owner o) {
    for(auto& w:writers)if(w.owner==o){w.state=APH_WRITE_UNCERTAIN;w.result=APH_E_NEEDS_RESTART;}
    hub::quarantine_owner(o,APH_E_NEEDS_RESTART);
}
void uncertain(APH_Owner o,APH_Result r) {
    AcquireSRWLockExclusive(&lock);
    __try {uncertain_locked(o);}
    __finally {ReleaseSRWLockExclusive(&lock);}
    hub::diagnostic("cleanup-uncertain",o,0,r);
}
bool same_lease(const Writer& current,const Writer& captured) {
    return current.owner==captured.owner&&current.id==captured.id&&
        current.access.activation_epoch==captured.access.activation_epoch&&
        current.access.runtime_epoch==captured.access.runtime_epoch&&
        current.access.target_generation==captured.access.target_generation;
}
APH_Result finish_checkpoint_locked(APH_Owner o,const APH_Cleanup& cleanup,const Writer (&captured)[2],APH_Result result) {
    APH_OwnerStatus owner{};hub::api()->owner_status(o,sizeof(owner),&owner);
    if(APH_OwnerHasFault(owner.state)||owner.activation_epoch!=cleanup.access.activation_epoch) {
        uncertain_locked(o);return APH_E_NEEDS_RESTART;
    }
    if(result==APH_E_THREAD||result==APH_PENDING)return APH_PENDING;
    if(result!=APH_OK){uncertain_locked(o);return APH_E_NEEDS_RESTART;}
    // Check every captured lease before committing any success. A concurrent
    // timeout/fault must retain its uncertain records even if restore says OK.
    for(unsigned i=0;i<2;++i)if(cleanup.domains&(1u<<i)) {
        const Writer& w=writers[i];
        if(!same_lease(w,captured[i])||w.state!=APH_WRITE_RELEASING||w.flights) {
            uncertain_locked(o);return APH_E_NEEDS_RESTART;
        }
    }
    for(unsigned i=0;i<2;++i)if(cleanup.domains&(1u<<i))writers[i]={};
    bool remaining=false,releasing=false;
    for(const auto& w:writers)if(w.owner==o){remaining=true;releasing|=w.state==APH_WRITE_RELEASING;}
    if(owner.state==APH_OWNER_DISABLING) {
        // Disable may have started while a RELEASE callback was running. Its
        // later domains and plugin cancellation need their own checkpoint; this
        // is ordinary pending work, not failed restoration or a drained owner.
        if(cleanup.reason!=APH_CLEANUP_DISABLE||lives[o].closing||!lives[o].notified||
            remaining||hub::services::pointer_pending(o))return APH_PENDING;
        return hub::disable_drained(o);
    }
    return releasing?APH_PENDING:APH_OK;
}
APH_Result checkpoint_impl(APH_Owner o,unsigned reason) {
    if(!o||!lives.get(o)||restoring_owner)return APH_E_BUSY;
    APH_Cleanup cleanup{sizeof(cleanup),reason,0,0,{sizeof(APH_Access),0,0,0,0}};
    APH_LifecycleDesc desc{};Writer captured[2]{};
    AcquireSRWLockExclusive(&lock);
    __try {
        Life& life=lives[o];if(!life.registered||life.restoring)return APH_E_BUSY;
        APH_OwnerStatus owner{};if(hub::api()->owner_status(o,sizeof(owner),&owner)!=APH_OK)return APH_E_ARGUMENT;
        if(APH_OwnerHasFault(owner.state))return APH_E_NEEDS_RESTART;
        const bool disabling=owner.state==APH_OWNER_DISABLING;
        if(disabling&&(life.closing||!life.notified||hub::owner_business_flights(o)))return APH_PENDING;
        cleanup.access.activation_epoch=owner.activation_epoch;
        if(target_changing)cleanup.reason=APH_CLEANUP_TARGET_CHANGE;
        if(disabling)cleanup.reason=APH_CLEANUP_DISABLE;
        for(unsigned i=0;i<2;++i)if(writers[i].owner==o) {
            Writer& w=writers[i];if(w.state==APH_WRITE_UNCERTAIN)return APH_E_NEEDS_RESTART;
            if(w.state==APH_WRITE_RELEASING){if(w.flights)return APH_PENDING;cleanup.domains|=1u<<i;cleanup.access=w.access;captured[i]=w;}
        }
        if(!disabling&&!cleanup.domains)return APH_OK;
        desc=life.desc;life.restoring=true;
    } __finally {ReleaseSRWLockExclusive(&lock);}
    APH_Result result=APH_E_NEEDS_RESTART;
    __try {
        if(hub::services::pointer_pending(o)){result=APH_PENDING;__leave;}
        if(cleanup.domains) {
            APH_RuntimeSnapshot s{};hub::services::cleanup_runtime_snapshot(&s);
            if(s.state!=APH_RUNTIME_LIVE||!s.target_ready||s.runtime_epoch!=cleanup.access.runtime_epoch||s.target_generation!=cleanup.access.target_generation){result=APH_E_STALE;__leave;}
            if(GetCurrentThreadId()!=s.verified_thread){result=APH_E_THREAD;__leave;}
        }
        restoring_owner=o;restoring_epoch=cleanup.access.activation_epoch;restoring_domains=cleanup.domains;
        result=invoke_restore(o,&cleanup,desc.restore,desc.context);
    } __finally {
        restoring_owner=0;restoring_epoch=0;restoring_domains=0;
        AcquireSRWLockExclusive(&lock);
        __try {result=finish_checkpoint_locked(o,cleanup,captured,result);}
        __finally {lives[o].restoring=false;ReleaseSRWLockExclusive(&lock);}
    }
    if(result==APH_E_NEEDS_RESTART)hub::diagnostic("cleanup-uncertain",o,0,result);
    else if(cleanup.reason==APH_CLEANUP_DISABLE&&result==APH_OK)hub::diagnostic("disable-complete",o,0,result);
    return result;
}
APH_Result APH_CALL checkpoint(APH_Owner o){return checkpoint_impl(o,APH_CLEANUP_RELEASE);}
APH_Result APH_CALL prepare_target_change(APH_Owner source) {
    APH_RuntimeSnapshot s{};hub::services::runtime_api()->snapshot(sizeof(s),&s);
    if(s.source!=source||!hub::owner_resident(source)||!s.verified_thread||s.verified_thread!=GetCurrentThreadId())return APH_E_THREAD;
    APH_Owner owners[2]{};
    AcquireSRWLockExclusive(&lock);
    __try {
        if(!target_changing) {
            const auto started=hub::services::begin_target_change(source,&target_before);
            if(started!=APH_OK)return started;
            target_changing=true;
        }
        for(unsigned i=0;i<2;++i){Writer& w=writers[i];owners[i]=w.owner;if(w.owner&&w.state==APH_WRITE_OWNED){w.state=APH_WRITE_RELEASING;w.deadline=GetTickCount64()+timeout(w.owner);}}
    }
    __finally {ReleaseSRWLockExclusive(&lock);}
    APH_Result result=APH_OK;
    for(unsigned i=0;i<2;++i)if(owners[i]&&(i==0||owners[i]!=owners[0])) {
        APH_Result r=checkpoint_impl(owners[i],APH_CLEANUP_TARGET_CHANGE);
        if(r==APH_PENDING||r==APH_E_BUSY||r==APH_E_THREAD) {
            if(result==APH_OK)result=APH_PENDING;
        } else if(r!=APH_OK){uncertain(owners[i],r);result=APH_E_NEEDS_RESTART;}
    }
    AcquireSRWLockExclusive(&lock);
    __try {
        // A domain restored early still cannot be reacquired while the other
        // owner/domain is draining. No callback or wait holds this lock.
        for(const auto& w:writers)if(w.owner)
            return w.state==APH_WRITE_UNCERTAIN?APH_E_NEEDS_RESTART:(result<0?result:APH_PENDING);
        result=hub::services::finish_target_change(target_before);
        target_changing=false;target_before={};
    } __finally {ReleaseSRWLockExclusive(&lock);}
    return result;
}
APH_Result APH_CALL request(APH_Owner o,const APH_WriteRequest* input,APH_WriteLease* out) {
    APH_WriteLease zero=0;APH_WriteRequest r{};
    if(!hub::copy(out,&zero,sizeof(zero))||!hub::services::input(input,&r)||index_of(r.domain)==2||!hub::services::registered(o)||r.access.flags!=7)return APH_E_ARGUMENT;
    APH_OwnerStatus s{};hub::api()->owner_status(o,sizeof(s),&s);if(s.state!=APH_OWNER_ACTIVE)return APH_E_REVOKED;
    APH_Result valid=hub::services::runtime_api()->validate(o,&r.access);if(valid<0)return valid;
    bool admitted=false;
    AcquireSRWLockExclusive(&lock);
    __try {
        if(target_changing)return APH_E_BUSY;
        // Linearize against Disable's gate close + writer scan. An admitted
        // request publishes under this lock before that scan, or is rejected.
        if(!hub::admit_owner(o,r.access.activation_epoch))return APH_E_REVOKED;
        admitted=true;
        valid=hub::services::runtime_api()->validate(o,&r.access);if(valid<0)return valid;
        Writer& w=writers[index_of(r.domain)];
        if(w.owner) {
            if(w.owner!=o)return APH_E_CONFLICT;
            if(w.state!=APH_WRITE_OWNED||w.access.activation_epoch!=r.access.activation_epoch||w.access.target_generation!=r.access.target_generation)return APH_E_STALE;
        } else {w.owner=o;w.id=++next_lease;w.access=r.access;w.state=APH_WRITE_OWNED;w.result=APH_OK;}
        if(!hub::copy(out,&w.id,sizeof(w.id))){w.state=APH_WRITE_UNCERTAIN;w.result=APH_E_NEEDS_RESTART;return APH_E_NEEDS_RESTART;}
        return APH_OK;
    } __finally {if(admitted)hub::leave_owner(o);ReleaseSRWLockExclusive(&lock);}
}
APH_Result APH_CALL query(uint32_t domain,uint32_t bytes,APH_WriteStatus* out) {
    unsigned i=index_of(domain);if(i==2)return APH_E_ARGUMENT;APH_WriteStatus s{};
    AcquireSRWLockShared(&lock);
    __try {Writer& w=writers[i];s={sizeof(s),w.state,domain,w.flights,w.owner,w.id,w.access.activation_epoch,w.access.target_generation,w.result,0};}
    __finally {ReleaseSRWLockShared(&lock);}
    return hub::services::output(bytes,out,s);
}
APH_Result APH_CALL enter(APH_Owner o,APH_WriteLease lease,const APH_Access* input,APH_WriteCall* out) {
    APH_Access a{};if(!hub::services::input(input,&a)||depth==32||(a.flags&3)!=3)return APH_E_ARGUMENT;
    APH_Result valid=hub::services::runtime_api()->validate(o,&a);if(valid<0)return valid;
    AcquireSRWLockExclusive(&lock);
    __try {
        for(unsigned i=0;i<2;++i) {
            Writer& w=writers[i];if(w.owner!=o||w.id!=lease)continue;
            bool restore=restoring_owner==o&&(restoring_domains&(1u<<i));
            if((w.state!=APH_WRITE_OWNED&&!restore)||w.access.activation_epoch!=a.activation_epoch||w.access.target_generation!=a.target_generation)return APH_E_STALE;
            if(!(restore?hub::admit_cleanup_resource(o,a.activation_epoch):hub::admit_owner(o,a.activation_epoch)))return APH_E_REVOKED;
            APH_WriteCall call{sizeof(call),1u<<i,o,lease,++next_call};
            if(!hub::copy(out,&call,sizeof(call))){if(restore)hub::leave_cleanup_resource(o);else hub::leave_owner(o);return APH_E_ARGUMENT;}
            ++w.flights;frames[depth++]={call,restore};return APH_OK;
        }
        return APH_E_STALE;
    } __finally {ReleaseSRWLockExclusive(&lock);}
}
APH_Result APH_CALL leave(const APH_WriteCall* input) {
    APH_WriteCall c{};if(!hub::services::input(input,&c)||!depth)return APH_E_ARGUMENT;
    Frame f=frames[depth-1];if(c.serial!=f.call.serial||c.owner!=f.call.owner||c.lease!=f.call.lease)return APH_E_STALE;
    AcquireSRWLockExclusive(&lock);
    __try {Writer& w=writers[index_of(f.call.domain)];if(w.id!=f.call.lease||!w.flights)return APH_E_STALE;--w.flights;--depth;}
    __finally {ReleaseSRWLockExclusive(&lock);}
    if(f.cleanup)hub::leave_cleanup_resource(c.owner);else hub::leave_owner(c.owner);return APH_OK;
}
APH_Result APH_CALL release(APH_Owner o,APH_WriteLease lease) {
    AcquireSRWLockExclusive(&lock);
    __try {for(auto& w:writers)if(w.owner==o&&w.id==lease){if(w.state==APH_WRITE_UNCERTAIN)return APH_E_NEEDS_RESTART;
        if(w.state==APH_WRITE_OWNED){w.state=APH_WRITE_RELEASING;w.deadline=GetTickCount64()+timeout(o);}return APH_PENDING;}return APH_E_STALE;}
    __finally {ReleaseSRWLockExclusive(&lock);}
}
uint32_t APH_CALL current(APH_Owner o) {
    unsigned domains=restoring_owner==o?restoring_domains:0;
    for(unsigned i=0;i<depth;++i)if(frames[i].call.owner==o)domains|=frames[i].call.domain;
    return domains;
}
const APH_LifecycleApi lifecycle{sizeof(lifecycle),1,registration,complete_enable,disable,checkpoint,prepare_target_change,restoring};
const APH_WriteApi writes{sizeof(writes),1,request,query,enter,leave,release,current};
void notify_disabling(APH_Owner o,APH_DisablingFn fn,uint64_t epoch,void* context) {
    __try {if(fn)fn(o,epoch,context);} __except(cleanup_filter(o,GetExceptionInformation())){}
}
}
namespace hub {
uint64_t begin_enable(APH_Owner o) {
    if(!o||!owner_resident(o))return 0;
    AcquireSRWLockExclusive(&lock);
    __try {
        if(!lives.ensure(o))return 0;
        const uint64_t epoch=start_enable(o);
        if(epoch) {
            Life& l=lives[o];l.epoch=epoch;l.notified=false;l.closing=false;
            l.deadline=l.registered?GetTickCount64()+l.desc.timeout_ms:0;
        }
        return epoch;
    } __finally {ReleaseSRWLockExclusive(&lock);}
}
APH_Result finish_enable(APH_Owner o,APH_Result result,uint64_t epoch) {
    if(!o||!lives.get(o))return APH_E_ARGUMENT;
    AcquireSRWLockExclusive(&lock);
    __try {
        Life& l=lives[o];
        if(!epoch)epoch=owner_epoch(o);
        if(result==APH_PENDING&&!l.registered)result=APH_E_UNAVAILABLE;
        if(!commit_enable(o,epoch,result))return APH_E_STALE;
        // Publishing Active/Failed and changing the deadline must be atomic
        // with Disable and timeout. Late Pending never resets cancellation.
        if(result!=APH_PENDING)l.deadline=0;
        return result;
    } __finally {ReleaseSRWLockExclusive(&lock);}
}
}
namespace hub::services {
const APH_LifecycleApi* lifecycle_api(){return &lifecycle;}
const APH_WriteApi* write_api(){return &writes;}
bool registered(APH_Owner o){if(!o)return false;AcquireSRWLockShared(&lock);auto l=lives.get(o);bool r=l&&l->registered;ReleaseSRWLockShared(&lock);return r;}
bool access_cleanup(APH_Owner o,uint64_t epoch){return restoring_owner==o&&restoring_epoch==epoch;}
bool restoring_runtime(){return restoring_owner!=0;}
bool has_write(APH_Owner o,unsigned domain){return (current(o)&domain)==domain;}
bool writes_pending(APH_Owner o){AcquireSRWLockShared(&lock);bool r=writers[0].owner==o||writers[1].owner==o;ReleaseSRWLockShared(&lock);return r;}
void epoch_close(APH_Owner o){hub::services::shared_epoch_close(o);hub::services::window_epoch_close(o);}
void tick() {
    const size_t limit=lives.size();
    for(APH_Owner o=1;o<limit;++o) {
        if(!registered(o))continue;
        APH_OwnerStatus s{};if(hub::api()->owner_status(o,sizeof(s),&s)!=APH_OK)continue;
        APH_RuntimeSnapshot runtime{};
        APH_DisablingFn fn=nullptr;void* context=nullptr;bool notify=false,expired=false;
        AcquireSRWLockExclusive(&lock);
        __try {Life& l=lives[o];
            hub::api()->owner_status(o,sizeof(s),&s);
            hub::services::cleanup_runtime_snapshot(&runtime);
            if(s.state==APH_OWNER_DISABLING&&!l.closing&&!l.notified){notify=true;fn=l.desc.disabling;context=l.desc.context;}
            if((s.state==APH_OWNER_DISABLING||s.state==APH_OWNER_ENABLING)&&l.deadline&&GetTickCount64()>=l.deadline)expired=true;
            for(auto& w:writers)if(w.owner==o&&w.state!=APH_WRITE_UNCERTAIN&&((w.state==APH_WRITE_RELEASING&&GetTickCount64()>=w.deadline)||APH_OwnerHasFault(s.state)||
                runtime.state!=APH_RUNTIME_LIVE||!runtime.target_ready||runtime.runtime_epoch!=w.access.runtime_epoch||runtime.target_generation!=w.access.target_generation)) {
                w.state=APH_WRITE_UNCERTAIN;w.result=APH_E_NEEDS_RESTART;expired=true;
            }
            // Commit the quarantine while the state/leases just checked are
            // still locked. A completed checkpoint cannot slip between a stale
            // expiry decision and a later quarantine of the next epoch.
            if(expired)uncertain_locked(o);
        } __finally {ReleaseSRWLockExclusive(&lock);}
        if(expired){hub::diagnostic("cleanup-uncertain",o,0,APH_E_BUSY);continue;}
        if(notify) {
            notify_disabling(o,fn,s.activation_epoch,context);
            AcquireSRWLockExclusive(&lock);lives[o].notified=true;ReleaseSRWLockExclusive(&lock);
        }
        if(s.state==APH_OWNER_DISABLING&&!writes_pending(o))checkpoint(o); // pure tools need no Actor
    }
}
}
