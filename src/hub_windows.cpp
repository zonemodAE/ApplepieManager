#define NOMINMAX
#include "hub_internal.h"
#include <commctrl.h>
namespace {
constexpr unsigned CommandCount=128;
struct Site {HWND hwnd;DWORD thread;uint64_t generation;uint32_t state;APH_Result result;HHOOK hook;bool installing,scheduling;size_t index;};
struct Route {APH_Owner owner;uint64_t epoch;APH_Route token;size_t site;APH_WindowRequest request;volatile LONG64 gate;bool counted,module;};
struct Command {APH_Route route;uint64_t generation;APH_WindowCommand value;bool used;Route* record;};
SRWLOCK lock=SRWLOCK_INIT;
hub::ResidentTable<Site> sites;
hub::ResidentTable<Route> routes;
hub::TokenIndex<Route> route_tokens;
Command commands[CommandCount]{};
size_t site_count,route_count;UINT wake_message;
uint64_t window_sequence;
HANDLE binding_wake;
Route* route_for(APH_Owner owner,APH_Route id) {
    Route* r=route_tokens.find(id);return r&&r->owner==owner?r:nullptr;
}
const wchar_t* incarnation_property=L"Applepie.Core.Incarnation.4E67B92A";
LRESULT CALLBACK subclass(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
void close_route(Route& r) {
    hub::services::close(&r.gate);
    if(!hub::services::count(&r.gate)&&r.counted){r.counted=false;hub::resource_delta(r.owner,-1);}
}
void bind(Site* site) {
    bool selected=false;
    AcquireSRWLockExclusive(&lock);
    __try {if(site->state==APH_WINDOW_PENDING&&!site->installing&&GetCurrentThreadId()==site->thread){site->installing=true;selected=true;}}
    __finally {ReleaseSRWLockExclusive(&lock);}
    if(!selected)return;
    BOOL ok=IsWindow(site->hwnd)&&GetPropW(site->hwnd,incarnation_property)==site&&GetWindowThreadProcessId(site->hwnd,nullptr)==site->thread&&
        SetWindowSubclass(site->hwnd,subclass,(UINT_PTR)site,(DWORD_PTR)site);
    HHOOK hook=nullptr;
    AcquireSRWLockExclusive(&lock);
    __try {site->state=ok?APH_WINDOW_BOUND:APH_WINDOW_FAILED;site->result=ok?APH_OK:APH_E_BACKEND;hook=site->hook;site->hook=nullptr;}
    __finally {ReleaseSRWLockExclusive(&lock);}
    if(hook)UnhookWindowsHookEx(hook);
}
LRESULT CALLBACK message_hook(int code,WPARAM removed,LPARAM data) {
    if(code>=0&&removed==PM_REMOVE) {
        MSG* msg=(MSG*)data;
        if(msg->message==wake_message) {
            // Records are process resident. Snapshot indexes under the
            // lock, but perform Win32 chain changes on this thread outside it.
            size_t count=0;
            AcquireSRWLockShared(&lock);count=site_count;ReleaseSRWLockShared(&lock);
            for(size_t i=0;i<count;++i)if(sites[i].hwnd==msg->hwnd)bind(&sites[i]);
        }
    }
    return CallNextHookEx(nullptr,code,removed,data);
}
DWORD WINAPI binding_worker(void*) {
    HMODULE module=nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,(LPCWSTR)message_hook,&module);
    for(;;) {
        WaitForSingleObject(binding_wake,50);
        const size_t limit=sites.size();
        for(size_t i=0;i<limit;++i) {
            Site* site=nullptr;
            AcquireSRWLockExclusive(&lock);
            HHOOK stale=nullptr;
            __try {
                if(i<site_count&&sites[i].state==APH_WINDOW_PENDING&&GetPropW(sites[i].hwnd,incarnation_property)!=&sites[i]) {
                    sites[i].state=APH_WINDOW_DEAD;sites[i].generation=++window_sequence;stale=sites[i].hook;sites[i].hook=nullptr;
                    for(size_t j=0;j<route_count;++j)if(routes[j].site==i)close_route(routes[j]);
                }
                if(i<site_count&&sites[i].state==APH_WINDOW_PENDING&&!sites[i].installing&&!sites[i].hook&&!sites[i].scheduling){site=&sites[i];site->scheduling=true;}
            }
            __finally {ReleaseSRWLockExclusive(&lock);}
            if(stale)UnhookWindowsHookEx(stale);
            if(!site)continue;
            // Hooks belong to this resident control thread, not the short-lived
            // caller of subscribe. Windows removes hooks when their installer
            // thread exits, even though the target UI thread is still running.
            HHOOK hook=SetWindowsHookExW(WH_GETMESSAGE,message_hook,module,site->thread);
            bool keep=false;
            AcquireSRWLockExclusive(&lock);
            __try {if(site->state==APH_WINDOW_PENDING){site->hook=hook;keep=true;if(!hook){site->state=APH_WINDOW_FAILED;site->result=APH_E_BACKEND;}}}
            __finally {ReleaseSRWLockExclusive(&lock);}
            if(hook){if(keep)PostMessageW(site->hwnd,wake_message,0,0);else UnhookWindowsHookEx(hook);}
        }
    }
}
void invoke(Route* route,APH_WindowEvent* event) {
    const auto owner=route->owner;const bool module=route->module;
    __try {
        __try {route->request.callback(route->owner,event,route->request.user_data);}
        __except(APH_EntryFilter(hub::hooks_api(),route->owner,0,GetExceptionInformation(),
            event->phase==APH_WINDOW_AFTER?APH_ORIGINAL_RETURNED:APH_ORIGINAL_NOT_ENTERED,route->request.recovery&0xffu)) {}
    } __finally {InterlockedDecrement64(&route->gate);if(module)hub::leave_module(owner);else hub::leave_owner(owner);}
}
Route* admit(Route* record,size_t site,APH_Route token=0) {
    Route* result=nullptr;
    AcquireSRWLockShared(&lock);
    __try {
        if(record) {
            Route& r=*record;
            if((!token||r.token==token)&&r.site==site&&sites[site].state==APH_WINDOW_BOUND&&hub::services::admit(&r.gate)) {
                if(r.module?hub::admit_module(r.owner):hub::admit_owner(r.owner,r.epoch))result=&r;else InterlockedDecrement64(&r.gate);
            }
        }
    } __finally {ReleaseSRWLockShared(&lock);}
    return result;
}
void dispatch(size_t site,APH_WindowEvent event) {
    AcquireSRWLockShared(&lock);const size_t limit=route_count;ReleaseSRWLockShared(&lock);
    for(size_t i=0;i<limit;++i) {
        Route* route=admit(routes.get(i),site);if(!route)continue;
        event.access={sizeof(APH_Access),0,route->epoch,0,0};
        invoke(route,&event);
    }
}
void drain(size_t site) {
    // At most the bounded capacity per wake, including commands posted by a
    // callback. No pointer payload or blocking SendMessage into plugin code.
    for(unsigned i=0;i<CommandCount;++i) {
        Command c{};
        AcquireSRWLockExclusive(&lock);
        __try {if(commands[i].used&&commands[i].record->site==site){c=commands[i];commands[i].used=false;}}
        __finally {ReleaseSRWLockExclusive(&lock);}
        if(!c.used||c.generation!=sites[site].generation)continue;
        Route* route=admit(c.record,site,c.route);if(!route)continue;
        const auto owner=route->owner;
        if(hub::services::runtime_api()->validate(owner,&c.value.access)!=APH_OK) {
            InterlockedDecrement64(&route->gate);hub::leave_owner(owner);continue;
        }
        APH_WindowEvent e{sizeof(e),APH_WINDOW_COMMAND,(uint64_t)(uintptr_t)sites[site].hwnd,c.generation,
            c.value.opcode,0,c.value.wparam,c.value.lparam,0,c.value.access};
        invoke(route,&e);
    }
}
LRESULT CALLBACK subclass(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR ref) {
    Site* site=(Site*)ref;size_t index=site->index;
    if(msg==wake_message){drain(index);return 0;}
    APH_WindowEvent e{sizeof(e),APH_WINDOW_BEFORE,(uint64_t)(uintptr_t)hwnd,site->generation,msg,0,(uint64_t)wp,(int64_t)lp,0,{}};
    dispatch(index,e);
    // A window message is not runtime shutdown evidence. WM_CLOSE can be
    // cancelled by the real window procedure. Always preserve its result.
    LRESULT result=DefSubclassProc(hwnd,msg,wp,lp);
    e.phase=APH_WINDOW_AFTER;e.original_result=result;dispatch(index,e);
    if(msg==WM_NCDESTROY) {
        AcquireSRWLockExclusive(&lock);
        __try {
            site->state=APH_WINDOW_DEAD;site->generation=++window_sequence;
            for(size_t i=0;i<route_count;++i)if(routes[i].site==index)close_route(routes[i]);
            for(auto& c:commands)if(c.used&&c.record->site==index)c.used=false;
        } __finally {ReleaseSRWLockExclusive(&lock);}
        RemoveWindowSubclass(hwnd,subclass,id);
        if(GetPropW(hwnd,incarnation_property)==site)RemovePropW(hwnd,incarnation_property);
    }
    return result;
}
APH_Result APH_CALL subscribe(APH_Owner owner,const APH_WindowRequest* input,APH_Route* out) {
    APH_Route zero=0;APH_WindowRequest r{};
    if(!hub::copy(out,&zero,sizeof(zero))||!hub::services::input(input,&r))return APH_E_ARGUMENT;
    if(!hub::owner_usable(owner)||!hub::owner_callback(owner,(void*)r.callback))return APH_E_REVOKED;
    if((r.recovery&~APH_WINDOW_MODULE_OBSERVER)>APH_RECOVERY_READ_ONLY)return APH_E_ARGUMENT;
    bool module=(r.recovery&APH_WINDOW_MODULE_OBSERVER)!=0;
    if(module&&!(hub::owner_flags(owner)&APH_PLUGIN_MODULE_OBSERVER))return APH_E_REVOKED;
    HWND hwnd=(HWND)(uintptr_t)r.window;DWORD process=0,thread=GetWindowThreadProcessId(hwnd,&process);
    if(!thread||process!=GetCurrentProcessId()||!IsWindow(hwnd))return APH_E_ARGUMENT;
    Site* site=nullptr;
    AcquireSRWLockExclusive(&lock);
    __try {
        if(!wake_message)wake_message=RegisterWindowMessageW(L"Applepie.Core.Window.v1.4E67B92A");
        if(!wake_message)return APH_E_BACKEND;
        size_t slot=route_count;
        if(hub::owner_flags(owner)&APH_PLUGIN_LIFECYCLE)for(size_t i=0;i<route_count;++i)
            if(routes[i].owner==owner&&(hub::services::read(&routes[i].gate)&hub::services::Closed)&&!hub::services::count(&routes[i].gate)){slot=i;break;}
        if(!routes.ensure(slot))return APH_E_QUEUE_FULL;
        size_t n=site_count;
        for(size_t i=0;i<site_count;++i)if(sites[i].hwnd==hwnd&&sites[i].thread==thread&&sites[i].state<APH_WINDOW_DEAD&&GetPropW(hwnd,incarnation_property)==&sites[i]){n=i;break;}
        if(n==site_count) {
            site=sites.ensure(site_count);if(!site)return APH_E_QUEUE_FULL;
            ++site_count;*site={hwnd,thread,++window_sequence,APH_WINDOW_PENDING,APH_PENDING,nullptr,false,false,n};
            if(!SetPropW(hwnd,incarnation_property,site)){site->state=APH_WINDOW_FAILED;site->result=APH_E_BACKEND;return APH_E_BACKEND;}
        } else site=&sites[n];
        Route& route=routes[slot];APH_Route value=route_tokens.issue(&route,route.token);
        if(!value)return APH_E_QUEUE_FULL;
        if(!hub::copy(out,&value,sizeof(value))){route_tokens.cancel(value,route.token);return APH_E_ARGUMENT;}
        route_tokens.forget(route.token);
        if(slot==route_count)++route_count;else close_route(route);
        route.owner=owner;route.epoch=hub::owner_epoch(owner);route.token=value;route.gate=0;route.module=module;
        if(!route.epoch)route.epoch=1;
        route.site=n;route.request=r;route.counted=true;hub::resource_delta(owner,1);
        if(thread!=GetCurrentThreadId()&&!binding_wake) {
            binding_wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
            HANDLE worker=binding_wake?CreateThread(nullptr,0,binding_worker,nullptr,0,nullptr):nullptr;
            if(worker)CloseHandle(worker);
            else {if(binding_wake)CloseHandle(binding_wake);binding_wake=nullptr;site->state=APH_WINDOW_FAILED;site->result=APH_E_BACKEND;return APH_E_BACKEND;}
        }
    } __finally {ReleaseSRWLockExclusive(&lock);}
    if(thread==GetCurrentThreadId())bind(site);
    else SetEvent(binding_wake);
    if(!PostMessageW(hwnd,wake_message,0,0))return APH_E_BACKEND;
    return APH_PENDING;
}
APH_Result APH_CALL query(APH_Owner owner,APH_Route id,uint32_t bytes,APH_WindowStatus* out) {
    APH_WindowStatus value{};
    AcquireSRWLockShared(&lock);
    __try {Route* found=route_for(owner,id);if(!found)return APH_E_STALE;
        Route& r=*found;Site& s=sites[r.site];
        value={sizeof(value),s.state,s.generation,hub::services::count(&r.gate),s.thread,s.result,0};
    } __finally {ReleaseSRWLockShared(&lock);}
    return hub::services::output(bytes,out,value);
}
APH_Result APH_CALL unsubscribe(APH_Owner owner,APH_Route id) {
    AcquireSRWLockExclusive(&lock);
    __try {Route* found=route_for(owner,id);if(!found)return APH_E_STALE;
        Route& r=*found;close_route(r);
        for(auto& c:commands)if(c.used&&c.route==id)c.used=false;
        return hub::services::count(&r.gate)?APH_PENDING:APH_OK;
    } __finally {ReleaseSRWLockExclusive(&lock);}
}
APH_Result APH_CALL post(APH_Owner owner,APH_Route id,const APH_WindowCommand* input) {
    APH_WindowCommand c{};
    if(!hub::services::input(input,&c))return APH_E_ARGUMENT;
    // Validate the same ticket on the destination thread as well. Posting from
    // UI/audio workers need not be on the verified game thread.
    APH_Access admission=c.access;admission.flags&=~APH_ACCESS_GAME_THREAD;
    APH_Result valid=hub::services::runtime_api()->validate(owner,&admission);if(valid<0)return valid;
    AcquireSRWLockExclusive(&lock);
    __try {Route* found=route_for(owner,id);if(!found)return APH_E_STALE;
        Route& r=*found;Site& s=sites[r.site];
        if(r.module)return APH_E_REVOKED;
        if((hub::services::read(&r.gate)&hub::services::Closed)||s.state>=APH_WINDOW_DEAD||c.access.activation_epoch!=r.epoch)return APH_E_STALE;
        for(auto& pending:commands)if(!pending.used) {
            pending={id,s.generation,c,true,&r};
            if(PostMessageW(s.hwnd,wake_message,0,0))return APH_OK;
            pending.used=false;return APH_E_BACKEND;
        }
        return APH_E_QUEUE_FULL;
    } __finally {ReleaseSRWLockExclusive(&lock);}
}
const APH_WindowApi api{sizeof(api),1,subscribe,query,unsubscribe,post};
}
namespace hub::services {
const APH_WindowApi* window_api(){return &::api;}
void window_epoch_close(APH_Owner owner) {
    AcquireSRWLockExclusive(&lock);
    __try {
        for(size_t i=0;i<route_count;++i)if(routes[i].owner==owner&&!routes[i].module)close_route(routes[i]);
        for(auto& c:commands)if(c.used&&c.record->owner==owner)c.used=false;
    } __finally {ReleaseSRWLockExclusive(&lock);}
}
void window_tick() {
    AcquireSRWLockExclusive(&lock);
    __try {
        for(size_t i=0;i<route_count;++i)if(hub::owner_failure(routes[i].owner)||(hub::services::read(&routes[i].gate)&hub::services::Closed))close_route(routes[i]);
        for(auto& c:commands)if(c.used&&(c.record->token!=c.route||(hub::services::read(&c.record->gate)&hub::services::Closed)))c.used=false;
    } __finally {ReleaseSRWLockExclusive(&lock);}
}
}
