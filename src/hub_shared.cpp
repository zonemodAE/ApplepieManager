#define NOMINMAX
#include "hub_internal.h"
#include <cstring>
namespace {
constexpr unsigned Depth = 32;
struct Site {
    APH_SharedRequest request;
    APH_Owner provider_owner;
    APH_SharedClaim provider_claim;
    APH_Hook native;
    bool retired;
    volatile LONG faulted;
};
struct Claim { APH_Owner owner; uint64_t key; size_t site; bool active, released; APH_NativeProc entry; volatile LONG published; };
struct Listener {
    APH_SharedListen desc;
    APH_SharedClaim claim;
    APH_Owner owner;
    uint64_t epoch;
    APH_Subscription token;
    volatile LONG64 gate;
    bool counted;
};
struct Frame { uint64_t invocation; size_t site; unsigned flags; APH_HookCall native; bool before, after, suppress; };
SRWLOCK lock = SRWLOCK_INIT;
hub::ResidentTable<Site> sites;
hub::ResidentTable<Claim> claims;
hub::ResidentTable<Listener> listeners;
hub::TokenIndex<Listener> listener_tokens;
size_t site_count, claim_count, listener_count;
volatile LONG64 sequence;
__declspec(thread) Frame frames[Depth];
__declspec(thread) unsigned depth;
Claim* claim_for(APH_Owner owner, APH_SharedClaim id) {
    return id && id <= claim_count && claims[id-1].owner == owner ? &claims[id-1] : nullptr;
}
bool string_ok(const char* p, size_t size) { return p[0] && memchr(p,0,size); }
bool compatible(const APH_SharedRequest& a, const APH_SharedRequest& b) {
    return !strcmp(a.contract,b.contract) && a.contract_version == b.contract_version &&
        a.payload_bytes == b.payload_bytes && a.original_policy == b.original_policy;
}
APH_Result request_impl(APH_Owner owner, const APH_SharedRequest* input, APH_SharedClaim* out) {
    APH_SharedClaim zero = 0; APH_SharedRequest r{};
    if (!hub::copy(out,&zero,sizeof(zero)) || !hub::services::input(input,&r)) return APH_E_ARGUMENT;
    if (!hub::owner_usable(owner) || !hub::runtime_mutations_allowed()) return APH_E_REVOKED;
    if (r.reserved || r.reserved2 || r.native.size < sizeof(r.native) || r.native.mode != APH_SHARED_TYPED ||
        !r.native.target || !r.native.request_key || r.native.reserved || r.native.recovery!=APH_RECOVERY_NONE ||
        !string_ok(r.native.native_signature,sizeof(r.native.native_signature)) ||
        !string_ok(r.contract,sizeof(r.contract)) || !r.contract_version || r.payload_bytes > 256 ||
        (r.original_policy != APH_ORIGINAL_ONCE && r.original_policy != APH_CONTROLLED_VOID)) return APH_E_ARGUMENT;
    uint32_t timestamp=0, image_size=0;
    if (!APH_ModuleBuild(r.native.target,&timestamp,&image_size) || timestamp != r.native.module_timestamp ||
        image_size != r.native.module_image_size) return APH_E_BUILD;
    AcquireSRWLockExclusive(&lock);
    __try {
        for(size_t i=0;i<claim_count;++i) if (claims[i].owner==owner && claims[i].key==r.native.request_key) {
            Site& s=sites[claims[i].site];
            if (s.request.native.target != r.native.target || !compatible(s.request,r) ||
                claims[i].entry!=r.native.native_entry || s.request.native.module_timestamp!=r.native.module_timestamp ||
                s.request.native.module_image_size!=r.native.module_image_size ||
                strcmp(s.request.native.native_signature,r.native.native_signature)) return APH_E_KEY_REUSED;
            APH_SharedClaim value=i+1;
            return hub::copy(out,&value,sizeof(value)) ? APH_OK : APH_E_ARGUMENT;
        }
        if (!claims.ensure(claim_count)) return APH_E_QUEUE_FULL;
        size_t site=site_count;
        for(size_t i=0;i<site_count;++i) if (sites[i].request.native.target==r.native.target) {site=i;break;}
        if (site < site_count) {
            Site& s=sites[site];
            if (s.retired||InterlockedCompareExchange(&s.faulted,0,0)) return APH_E_CONFLICT;
            if (strcmp(s.request.native.native_signature,r.native.native_signature)) return APH_E_SIGNATURE;
            if (!compatible(s.request,r)) return APH_E_CONTRACT;
            if (s.request.native.module_timestamp!=r.native.module_timestamp ||
                s.request.native.module_image_size!=r.native.module_image_size) return APH_E_BUILD;
        } else {
            if (!r.native.native_entry) return APH_E_UNAVAILABLE;
            if (!sites.ensure(site)) return APH_E_QUEUE_FULL;
            Site& s=sites[site];
            APH_Result result=hub::shared_native_request(owner,&r.native,&s.native);
            if (result<0) return result;
            s.request=r; s.provider_owner=owner; s.provider_claim=claim_count+1;
            ++site_count;
        }
        APH_SharedClaim value=claim_count+1;
        if (!hub::copy(out,&value,sizeof(value))) {
            // A new site remains reserved if its output publication fails. Do not
            // accidentally remove another owner's site or claim restoration.
            if (sites[site].provider_claim==value) {
                sites[site].retired=true;
                hub::hooks_api()->release_hook(owner,sites[site].native);
            }
            return APH_E_ARGUMENT;
        }
        Claim& claim=claims[claim_count++];
        claim={owner,r.native.request_key,site,false,false,r.native.native_entry,0};
        InterlockedExchange(&claim.published,1);
        hub::resource_delta(owner,1);
        return APH_PENDING;
    } __finally {ReleaseSRWLockExclusive(&lock);}
}
APH_Result APH_CALL request(APH_Owner owner,const APH_SharedRequest* input,APH_SharedClaim* out) {
    auto result=request_impl(owner,input,out);
    if(result<0){APH_SharedRequest r{};hub::services::input(input,&r);
        hub::error_detail(owner,"request-shared",result,r.contract,(uint64_t)r.native.target);}
    return result;
}
APH_Result APH_CALL query(APH_Owner owner, APH_SharedClaim id, uint32_t bytes, APH_SharedStatus* out) {
    APH_SharedStatus value{};
    AcquireSRWLockShared(&lock);
    __try {
        Claim* c=claim_for(owner,id); if(!c)return APH_E_ARGUMENT;
        Site& s=sites[c->site]; APH_HookStatus native{};
        APH_Result r=hub::hooks_api()->query_hook(s.provider_owner,s.native,sizeof(native),&native);
        if(r<0)return r;
        value.size=sizeof(value);value.state=native.state;value.result=native.result;
        value.is_provider=s.provider_claim==id;value.native_hook=s.native;
        value.original=value.is_provider?native.original:nullptr;value.in_flight=native.in_flight;
        for(size_t i=0;i<claim_count;++i)if(claims[i].site==c->site&&!claims[i].released)++value.members;
        if(c->released){value.state=APH_HOOK_RELEASED;value.result=APH_OK;
            for(size_t i=0;i<listener_count;++i)if(listeners[i].claim==id&&hub::services::count(&listeners[i].gate))value.result=APH_PENDING;
        }
        else if(!c->active&&native.state==APH_HOOK_ACTIVE)value.state=APH_HOOK_PREPARED;
        if(!c->released) {
            APH_OwnerStatus member{};hub::api()->owner_status(owner,sizeof(member),&member);
            if(APH_OwnerHasFault(member.state)){value.state=member.state==APH_OWNER_NEEDS_RESTART?APH_HOOK_NEEDS_RESTART:APH_HOOK_FAILED;value.result=member.result;}
            else if((native.state==APH_HOOK_PENDING||native.state==APH_HOOK_PREPARED)&&!hub::owner_usable(s.provider_owner)){
                value.state=APH_HOOK_FAILED;value.result=APH_E_REVOKED;
            }
        }
        if(InterlockedCompareExchange(&s.faulted,0,0)){value.state=APH_HOOK_NEEDS_RESTART;value.result=APH_E_NEEDS_RESTART;}
    } __finally {ReleaseSRWLockShared(&lock);}
    return hub::services::output(bytes,out,value);
}
APH_Result APH_CALL activate(APH_Owner owner, APH_SharedClaim id, APH_NativeProc original) {
    if(!hub::owner_usable(owner)||!hub::owner_epoch(owner)||!hub::runtime_mutations_allowed())return APH_E_REVOKED;
    AcquireSRWLockExclusive(&lock);
    __try {
        Claim* c=claim_for(owner,id);if(!c)return APH_E_ARGUMENT;
        Site& s=sites[c->site];if(c->released||s.retired||InterlockedCompareExchange(&s.faulted,0,0))return APH_E_REVOKED;
        if(s.provider_claim==id) {
            APH_Result r=hub::hooks_api()->activate_hook(owner,s.native,original);
            if(r<0)return r;c->active=true;return r;
        }
        if(original)return APH_E_ARGUMENT;
        c->active=true;return APH_OK;
    } __finally {ReleaseSRWLockExclusive(&lock);}
}
void close_listener(Listener& l) {
    hub::services::close(&l.gate);
    if(!hub::services::count(&l.gate)&&l.counted){l.counted=false;hub::resource_delta(l.owner,-1);}
}
APH_Result APH_CALL release(APH_Owner owner, APH_SharedClaim id) {
    AcquireSRWLockExclusive(&lock);
    __try {
        Claim* c=claim_for(owner,id);if(!c)return APH_E_ARGUMENT;
        if(!c->released){c->released=true;c->active=false;hub::resource_delta(owner,-1);}
        bool pending=false;
        for(size_t i=0;i<listener_count;++i)if(listeners[i].claim==id){close_listener(listeners[i]);pending|=hub::services::count(&listeners[i].gate)!=0;}
        bool any=false;
        for(size_t i=0;i<claim_count;++i)if(claims[i].site==c->site&&!claims[i].released)any=true;
        if(!any){Site& s=sites[c->site];s.retired=true;if(InterlockedCompareExchange(&s.faulted,0,0))return APH_E_NEEDS_RESTART;
            APH_Result r=hub::hooks_api()->release_hook(s.provider_owner,s.native);if(r!=APH_OK)return r;}
        return pending?APH_PENDING:APH_OK;
    } __finally {ReleaseSRWLockExclusive(&lock);}
}
APH_Result APH_CALL listen(APH_Owner owner, APH_SharedClaim id, const APH_SharedListen* input, APH_Subscription* out) {
    APH_Subscription zero=0;APH_SharedListen desc{};
    if(!hub::copy(out,&zero,sizeof(zero))||!hub::services::input(input,&desc))return APH_E_ARGUMENT;
    if(!hub::owner_usable(owner)||!hub::owner_callback(owner,(void*)desc.callback))return APH_E_REVOKED;
    if(!desc.phases||(desc.phases&~3u)||desc.role>APH_MODULE_OBSERVER||desc.recovery>APH_RECOVERY_READ_ONLY)return APH_E_ARGUMENT;
    if(desc.role==APH_MODULE_OBSERVER&&!(hub::owner_flags(owner)&APH_PLUGIN_MODULE_OBSERVER))return APH_E_REVOKED;
    AcquireSRWLockExclusive(&lock);
    __try {
        Claim* c=claim_for(owner,id);if(!c||c->released)return APH_E_REVOKED;
        Site& s=sites[c->site];
        if(desc.role==APH_CONTROLLER) {
            if(s.request.original_policy!=APH_CONTROLLED_VOID||!(desc.phases&APH_BEFORE))return APH_E_CONTRACT;
            for(size_t i=0;i<listener_count;++i) {
                Listener& l=listeners[i];
                if(claims[l.claim-1].site==c->site&&l.desc.role==APH_CONTROLLER&&
                    (!(hub::services::read(&l.gate)&hub::services::Closed)||hub::services::count(&l.gate)))return APH_E_CONFLICT;
            }
        }
        size_t slot=listener_count;
        if(hub::owner_flags(owner)&APH_PLUGIN_LIFECYCLE)for(size_t i=0;i<listener_count;++i)
            if(listeners[i].owner==owner&&(hub::services::read(&listeners[i].gate)&hub::services::Closed)&&!hub::services::count(&listeners[i].gate)){slot=i;break;}
        if(!listeners.ensure(slot))return APH_E_QUEUE_FULL;
        Listener& l=listeners[slot];APH_Subscription value=listener_tokens.issue(&l,l.token);
        if(!value)return APH_E_QUEUE_FULL;
        if(!hub::copy(out,&value,sizeof(value))){listener_tokens.cancel(value,l.token);return APH_E_ARGUMENT;}
        listener_tokens.forget(l.token);
        if(slot==listener_count)++listener_count;else close_listener(l);
        l.desc=desc;l.claim=id;l.owner=owner;l.token=value;l.gate=0;
        l.epoch=hub::owner_epoch(owner);if(!l.epoch)l.epoch=1;l.counted=true;hub::resource_delta(owner,1);
        return APH_OK;
    } __finally {ReleaseSRWLockExclusive(&lock);}
}
APH_Result APH_CALL unlisten(APH_Owner owner, APH_Subscription id) {
    AcquireSRWLockExclusive(&lock);
    __try {Listener* found=listener_tokens.find(id);if(!found)return APH_E_STALE;
        Listener& l=*found;if(l.owner!=owner)return APH_E_ARGUMENT;
        close_listener(l);return hub::services::count(&l.gate)?APH_PENDING:APH_OK;
    } __finally {ReleaseSRWLockExclusive(&lock);}
}
void APH_CALL native_fault(APH_Owner,APH_SharedClaim,uint32_t,uint64_t,uint32_t);
APH_Result APH_CALL enter(APH_Owner owner, APH_SharedClaim id, APH_SharedCall* out) {
    if(depth==Depth){native_fault(owner,id,0xe0410003u,0,APH_ORIGINAL_NOT_ENTERED);return APH_E_BUSY;}
    APH_SharedCall value{};size_t site=0;
    AcquireSRWLockShared(&lock);
    __try {
        Claim* c=claim_for(owner,id);if(!c)return APH_E_ARGUMENT;site=c->site;Site& s=sites[site];
        if(s.provider_claim!=id)return APH_E_ARGUMENT;
        APH_Result r=hub::hooks_api()->enter_call(owner,s.native,&value.native_call);if(r<0)return r;
        value.size=sizeof(value);value.provider=id;
        value.flags=(value.native_call.flags&APH_CALL_RECURSIVE)?APH_SHARED_RECURSIVE:
            ((s.retired||InterlockedCompareExchange(&s.faulted,0,0)||!(value.native_call.flags&APH_CALL_NATIVE_READY))?0u:APH_SHARED_DISPATCH);
        value.invocation=(uint64_t)InterlockedIncrement64(&sequence);
        if(!hub::copy(out,&value,sizeof(value))) {hub::hooks_api()->leave_call(owner,&value.native_call);return APH_E_ARGUMENT;}
        frames[depth++]={value.invocation,site,value.flags,value.native_call,false,false,false};return APH_OK;
    } __finally {ReleaseSRWLockShared(&lock);}
}
APH_Result invoke(Listener* l, APH_SharedEvent* e) {
    APH_Result result=APH_OK;
    const auto owner=l->owner;const bool module=l->desc.role==APH_MODULE_OBSERVER;
    __try {
        __try {result=l->desc.callback(l->owner,e,l->desc.user_data);}
        __except(APH_EntryFilter(hub::hooks_api(),l->owner,0,GetExceptionInformation(),
            e->phase==APH_BEFORE||e->original_suppressed?APH_ORIGINAL_NOT_ENTERED:APH_ORIGINAL_RETURNED,l->desc.recovery)) {result=APH_OK;}
    } __finally {InterlockedDecrement64(&l->gate);if(module)hub::leave_module(owner);else hub::leave_owner(owner);}
    return result;
}
APH_Result APH_CALL publish(const APH_SharedCall* input, uint32_t phase, const void* payload, uint32_t bytes) {
    APH_SharedCall call{};alignas(16) unsigned char snapshot[256];
    if(!hub::services::input(input,&call)||!depth||frames[depth-1].invocation!=call.invocation)return APH_E_STALE;
    Frame& f=frames[depth-1];Site& s=sites[f.site];
    if(call.provider!=s.provider_claim||call.flags!=f.flags||call.native_call.hook!=f.native.hook)return APH_E_ARGUMENT;
    if(bytes!=s.request.payload_bytes||bytes>sizeof(snapshot)||(bytes&&!hub::copy(snapshot,payload,bytes)))return APH_E_ARGUMENT;
    if(phase!=APH_BEFORE&&phase!=APH_AFTER)return APH_E_ARGUMENT;
    if((phase==APH_BEFORE&&f.before)||(phase==APH_AFTER&&(!f.before||f.after)))return APH_E_CONTRACT;
    if(phase==APH_BEFORE)f.before=true;else f.after=true;
    if(!(f.flags&APH_SHARED_DISPATCH)||InterlockedCompareExchange(&s.faulted,0,0))return APH_OK;
    size_t limit=0;AcquireSRWLockShared(&lock);limit=listener_count;ReleaseSRWLockShared(&lock);
    for(size_t i=0;i<limit;++i) {
        if(InterlockedCompareExchange(&s.faulted,0,0))break;
        Listener* selected=nullptr;
        AcquireSRWLockShared(&lock);
        __try {
            if(i>=listener_count)break;Listener& l=listeners[i];Claim& c=claims[l.claim-1];
            if(c.site==f.site&&c.active&&!c.released&&(l.desc.phases&phase)&&hub::services::admit(&l.gate)) {
                if(l.desc.role==APH_MODULE_OBSERVER?hub::admit_module(l.owner):hub::admit_owner(l.owner,l.epoch))selected=&l;else InterlockedDecrement64(&l.gate);
            }
        } __finally {ReleaseSRWLockShared(&lock);}
        if(!selected)continue;
        const auto owner=selected->owner,callback=(uint64_t)(uintptr_t)selected->desc.callback;
        const auto role=selected->desc.role;
        APH_SharedEvent e{sizeof(e),phase,call.invocation,selected->epoch,snapshot,bytes,f.suppress?1u:0u};
        APH_Result result=invoke(selected,&e);
        if(result==APH_SUPPRESS&&phase==APH_BEFORE&&role==APH_CONTROLLER&&s.request.original_policy==APH_CONTROLLED_VOID)f.suppress=true;
        else if(result!=APH_OK) {
            hub::services::fault(owner,0xe0410002u,callback,
                      phase==APH_BEFORE?APH_ORIGINAL_NOT_ENTERED:APH_ORIGINAL_RETURNED,false);
            return APH_E_CONTRACT;
        }
    }
    return phase==APH_BEFORE&&f.suppress?APH_SUPPRESS:APH_OK;
}
APH_Result APH_CALL leave(const APH_SharedCall* input) {
    APH_SharedCall call{};
    if(!hub::services::input(input,&call)||!depth||frames[depth-1].invocation!=call.invocation)return APH_E_STALE;
    Frame& f=frames[depth-1];Site& s=sites[f.site];
    if(call.provider!=s.provider_claim||call.flags!=f.flags||call.native_call.hook!=f.native.hook)return APH_E_ARGUMENT;
    APH_Result result=hub::hooks_api()->leave_call(s.provider_owner,&f.native);
    if(result==APH_OK)--depth;return result;
}
void APH_CALL native_fault(APH_Owner owner,APH_SharedClaim id,uint32_t code,uint64_t address,uint32_t phase) {
    // Published records stay resident; no lock/allocation in the filter.
    Claim* found=id?claims.get(id-1):nullptr;
    if(!found||!InterlockedCompareExchange(&found->published,0,0))return;
    Claim& c=*found;if(c.owner!=owner)return;
    Site& s=sites[c.site];if(s.provider_claim!=id)return;
    InterlockedExchange(&s.faulted,1);
    hub::hooks_api()->record_fault(owner,s.native,code,address,phase,0);
}
const APH_SharedApi api{sizeof(api),1,request,query,activate,release,listen,unlisten,enter,publish,leave,native_fault};
}
namespace hub::services {
const APH_SharedApi* shared_api(){return &::api;}
void shared_epoch_close(APH_Owner owner) {
    AcquireSRWLockExclusive(&lock);
    __try {for(size_t i=0;i<listener_count;++i)if(listeners[i].owner==owner&&listeners[i].desc.role!=APH_MODULE_OBSERVER)close_listener(listeners[i]);}
    __finally {ReleaseSRWLockExclusive(&lock);}
}
void shared_tick() {
    AcquireSRWLockShared(&lock);const size_t limit=claim_count;ReleaseSRWLockShared(&lock);
    for(size_t i=0;i<limit;++i) {
        APH_Owner owner=0;
        AcquireSRWLockShared(&lock);
        __try {if(i<claim_count&&!claims[i].released&&hub::owner_failure(claims[i].owner)==1)owner=claims[i].owner;}
        __finally {ReleaseSRWLockShared(&lock);}
        if(owner)release(owner,i+1);
    }
    AcquireSRWLockExclusive(&lock);
    __try {for(size_t i=0;i<listener_count;++i)if(hub::services::read(&listeners[i].gate)&hub::services::Closed)close_listener(listeners[i]);}
    __finally {ReleaseSRWLockExclusive(&lock);}
}
}
