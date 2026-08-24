#include "snesrecomp_platform/runtime_policy.h"

int snesrecomp_platform_nested_lle_deadline_due(
    int local_yield_active,
    int stop_on_rti,
    int scheduler_depth,
    int bounce_owner_depth,
    uint64_t deadline_master_cycles,
    uint64_t current_master_cycles) {
    return !local_yield_active && !stop_on_rti && scheduler_depth > 0 &&
           bounce_owner_depth > 0 && deadline_master_cycles != 0 &&
           current_master_cycles >= deadline_master_cycles;
}
