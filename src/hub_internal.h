#pragma once
#include "hub_core.h"
#include "resident_table.h"
#include "../sdk/applepie_seh.h"

namespace hub {
// Lifecycle lock must precede these registry transactions.
uint64_t start_enable(APH_Owner);
bool commit_enable(APH_Owner, uint64_t epoch, APH_Result);
}
namespace hub::services {
template<class T> bool input(const T* source, T* dest) {
    uint32_t bytes = 0;
    return hub::copy(&bytes, source, sizeof(bytes)) && bytes >= sizeof(T) && hub::copy(dest, source, sizeof(T));
}
template<class T> APH_Result output(uint32_t bytes, T* dest, const T& value) {
    if (bytes < sizeof(T)) return APH_E_ABI;
    return hub::copy(dest, &value, sizeof(value)) ? APH_OK : APH_E_ARGUMENT;
}
constexpr LONG64 Closed = (LONG64)(UINT64_C(1) << 63);
inline LONG64 read(volatile LONG64* gate) { return InterlockedCompareExchange64(gate, 0, 0); }
inline bool admit(volatile LONG64* gate) {
    LONG64 prior = read(gate);
    while (!(prior & Closed)) {
        LONG64 actual = InterlockedCompareExchange64(gate, prior + 1, prior);
        if (actual == prior) return true;
        prior = actual;
    }
    return false;
}
inline void close(volatile LONG64* gate) { InterlockedOr64(gate, Closed); }
inline uint32_t count(volatile LONG64* gate) { return (uint32_t)(read(gate) & ~Closed); }
inline void fault(APH_Owner owner, uint32_t code, uint64_t address, uint32_t phase, bool safe) {
    hub::hooks_api()->record_fault(owner, 0, code, address, phase, safe ? 1u : 0u);
}
const APH_SharedApi* shared_api();
const APH_PointerApi* pointer_api();
const APH_RuntimeApi* runtime_api();
const APH_PluginServiceApi* plugin_api();
const APH_DiagnosticsApi* diagnostics_api();
const APH_ConfigApi* config_api();
const APH_WindowApi* window_api();
void shared_tick();
void window_tick();
void shared_epoch_close(APH_Owner);
void window_epoch_close(APH_Owner);
bool pointer_pending(APH_Owner);
APH_Result begin_target_change(APH_Owner,APH_RuntimeSnapshot*);
APH_Result finish_target_change(const APH_RuntimeSnapshot&);
void cleanup_runtime_snapshot(APH_RuntimeSnapshot*);
const APH_LifecycleApi* lifecycle_api();
const APH_WriteApi* write_api();
bool registered(APH_Owner);
void tick();
bool access_cleanup(APH_Owner,uint64_t epoch);
bool restoring_runtime();
bool has_write(APH_Owner,unsigned domain);
bool writes_pending(APH_Owner);
void epoch_close(APH_Owner);
}
