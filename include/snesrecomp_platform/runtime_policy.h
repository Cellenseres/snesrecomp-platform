#ifndef SNESRECOMP_PLATFORM_RUNTIME_POLICY_H
#define SNESRECOMP_PLATFORM_RUNTIME_POLICY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Return nonzero when an interpreter run nested below an owning cooperative
 * scheduler must yield at its frame deadline. The API deliberately accepts
 * primitive state instead of framework CpuState or game types, keeping the
 * policy reusable by windowed, headless, and alternate-platform hosts.
 */
int snesrecomp_platform_nested_lle_deadline_due(
    int local_yield_active,
    int stop_on_rti,
    int scheduler_depth,
    int bounce_owner_depth,
    uint64_t deadline_master_cycles,
    uint64_t current_master_cycles);

#ifdef __cplusplus
}
#endif

#endif
