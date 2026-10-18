#define NOMINMAX
#include "hub_internal.h"
#include <initializer_list>
namespace {
struct Patch { APH_Owner owner; APH_Patch token; APH_PointerRequest request; uint32_t state; APH_Result result; bool admitted,cleanup; };
SRWLOCK lock=SRWLOCK_INIT;
hub::ResidentTable<Patch> patches;
hub::TokenIndex<Patch> patch_tokens;
Patch* find(APH_Owner owner,APH_Patch id) {
    Patch* p=patch_tokens.find(id);return p&&p->owner==owner?p:nullptr;
}
bool pin(const void* address) {
    HMODULE module=nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,(LPCWSTR)address,&module)!=FALSE;
}
bool data_slot(const APH_PointerRequest& r) {
    if(!r.slot||((uintptr_t)r.slot&7))return false;
    MEMORY_BASIC_INFORMATION m{};
    if(!VirtualQuery(r.slot,&m,sizeof(m))||m.Type!=MEM_IMAGE||m.State!=MEM_COMMIT||(m.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    const DWORD writable=PAGE_READWRITE|PAGE_WRITECOPY;
    if(!(m.Protect&writable)||(uintptr_t)r.slot+sizeof(void*)>(uintptr_t)m.BaseAddress+m.RegionSize)return false;
    uint32_t timestamp=0,size=0;
    if(!APH_ModuleBuild((APH_NativeProc)r.slot,&timestamp,&size)||timestamp!=r.module_timestamp||size!=r.module_image_size||!pin(r.slot))return false;
    if(!r.expected||!r.replacement||r.expected==r.replacement)return false;
    for(const void* address:{r.expected,r.replacement}) {
        if(!VirtualQuery(address,&m,sizeof(m))||m.State!=MEM_COMMIT||m.Type!=MEM_IMAGE||(m.Protect&(PAGE_GUARD|PAGE_NOACCESS))||
            !(m.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))||!pin(address))return false;
    }
    return true;
}
bool cas(void** slot,void* replacement,void* expected,void** previous) {
    __try {*previous=InterlockedCompareExchangePointer((void* volatile*)slot,replacement,expected);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
APH_Result APH_CALL request(APH_Owner owner,const APH_PointerRequest* input,APH_Patch* out) {
    APH_Patch zero=0;APH_PointerRequest r{};
    if(!hub::copy(out,&zero,sizeof(zero))||!hub::services::input(input,&r))return APH_E_ARGUMENT;
    if(r.scope!=APH_PATCH_TEMPORARY&&r.scope!=APH_PATCH_PERSISTENT)return APH_E_ARGUMENT;
    // Every patch has an explicit target/runtime/owner ticket. Release is memory
    // cleanup of a pinned PE slot and remains permitted after target invalidation.
    if((r.access.flags&(APH_ACCESS_RUNTIME|APH_ACCESS_TARGET|APH_ACCESS_GAME_THREAD))!=7u)return APH_E_ARGUMENT;
    APH_Result valid=hub::services::runtime_api()->validate(owner,&r.access);if(valid<0)return valid;
    if((hub::owner_flags(owner)&APH_PLUGIN_LIFECYCLE)&&!hub::services::has_write(owner,APH_ANIMATION))return APH_E_REVOKED;
    if(!data_slot(r))return APH_E_BUILD;
    AcquireSRWLockExclusive(&lock);
    __try {
        size_t free=patches.size();
        for(size_t i=0;i<patches.size();++i) {
            Patch& p=patches[i];
            if(p.owner&&p.state!=APH_PATCH_RESTORED&&p.request.slot==r.slot)return APH_E_BUSY;
            if(free==patches.size()&&(!p.owner||p.state==APH_PATCH_RESTORED))free=i;
        }
        if(!patches.ensure(free))return APH_E_QUEUE_FULL;
        valid=hub::services::runtime_api()->validate(owner,&r.access);if(valid<0)return valid;
        Patch& p=patches[free];APH_Patch id=patch_tokens.issue(&p,p.token);
        if(!id)return APH_E_QUEUE_FULL;
        bool cleanup=false;
        if(!hub::admit_owner(owner,r.access.activation_epoch)) {
            if(!hub::admit_cleanup_resource(owner,r.access.activation_epoch)){patch_tokens.cancel(id,p.token);return APH_E_REVOKED;}cleanup=true;
        }
        patch_tokens.forget(p.token);p.token=id;
        p.owner=owner;p.request=r;p.state=APH_PATCH_RESTORED;p.result=APH_OK;p.admitted=true;p.cleanup=cleanup;
        void* prior=nullptr;
        if(!cas(r.slot,r.replacement,r.expected,&prior)) {
            p.state=APH_PATCH_UNCERTAIN;p.result=APH_E_NEEDS_RESTART;hub::resource_delta(owner,1);
            hub::copy(out,&id,sizeof(id));
            hub::services::fault(owner,0xe0410010u,(uint64_t)(uintptr_t)r.slot,APH_ORIGINAL_NOT_ENTERED,false);return p.result;
        }
        if(prior!=r.expected){p.result=APH_E_EXPECTED;p.admitted=false;if(p.cleanup)hub::leave_cleanup_resource(owner);else hub::leave_owner(owner);return p.result;}
        p.state=APH_PATCH_APPLIED;hub::resource_delta(owner,1);
        if(hub::copy(out,&id,sizeof(id)))return APH_OK;
        if(cas(r.slot,r.expected,r.replacement,&prior)&&prior==r.replacement){p.state=APH_PATCH_RESTORED;p.admitted=false;if(p.cleanup)hub::leave_cleanup_resource(owner);else hub::leave_owner(owner);hub::resource_delta(owner,-1);return APH_E_ARGUMENT;}
        p.state=APH_PATCH_UNCERTAIN;p.result=APH_E_NEEDS_RESTART;
        hub::services::fault(owner,0xe0410011u,(uint64_t)(uintptr_t)r.slot,APH_ORIGINAL_NOT_ENTERED,false);return p.result;
    } __finally {ReleaseSRWLockExclusive(&lock);}
}
APH_Result APH_CALL release(APH_Owner owner,APH_Patch id) {
    AcquireSRWLockExclusive(&lock);
    __try {
        Patch* p=find(owner,id);if(!p)return APH_E_STALE;
        if(p->state==APH_PATCH_RESTORED)return APH_OK;
        // Unknown state is an intentionally retained claim, never force-repaired.
        if(p->state==APH_PATCH_UNCERTAIN)return APH_E_NEEDS_RESTART;
        void* prior=nullptr;
        if(cas(p->request.slot,p->request.expected,p->request.replacement,&prior)&&prior==p->request.replacement) {
            p->state=APH_PATCH_RESTORED;p->result=APH_OK;p->admitted=false;if(p->cleanup)hub::leave_cleanup_resource(owner);else hub::leave_owner(owner);hub::resource_delta(owner,-1);return APH_OK;
        }
        p->state=APH_PATCH_UNCERTAIN;p->result=APH_E_NEEDS_RESTART;
        hub::services::fault(owner,0xe0410012u,(uint64_t)(uintptr_t)p->request.slot,APH_ORIGINAL_RETURNED,false);return p->result;
    } __finally {ReleaseSRWLockExclusive(&lock);}
}
APH_Result APH_CALL query(APH_Owner owner,APH_Patch id,uint32_t bytes,APH_PointerStatus* out) {
    APH_PointerStatus value{};
    AcquireSRWLockShared(&lock);
    __try {Patch* p=find(owner,id);if(!p)return APH_E_STALE;
        value={sizeof(value),p->state,p->result,0,p->request.expected,p->request.replacement};
    } __finally {ReleaseSRWLockShared(&lock);}
    return hub::services::output(bytes,out,value);
}
void APH_CALL wrapper_fault(APH_Owner owner,APH_Patch id,uint32_t code,uint64_t address) {
    // The filter never locks. A valid SDK caller owns this handle; original/wrapper
    // faults always propagate, even when the memory slot can be restored.
    (void)id;hub::services::fault(owner,code,address,APH_ORIGINAL_RUNNING,false);
}
const APH_PointerApi api{sizeof(api),1,request,release,query,wrapper_fault};
}
namespace hub::services {
const APH_PointerApi* pointer_api(){return &::api;}
bool pointer_pending(APH_Owner owner) {
    AcquireSRWLockShared(&lock);
    __try {for(auto& p:patches)if(p.owner==owner&&p.state!=APH_PATCH_RESTORED)return true;return false;}
    __finally {ReleaseSRWLockShared(&lock);}
}
}
