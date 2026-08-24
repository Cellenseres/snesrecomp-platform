#include <stdint.h>
#include <stdio.h>

#include "snesrecomp_platform/runtime_policy.h"

static int expect_due(
    int expected,
    int local_yield_active,
    int stop_on_rti,
    int scheduler_depth,
    int bounce_owner_depth,
    uint64_t deadline,
    uint64_t current,
    const char *name) {
    const int actual = snesrecomp_platform_nested_lle_deadline_due(
        local_yield_active, stop_on_rti, scheduler_depth, bounce_owner_depth,
        deadline, current);
    if (!!actual == !!expected)
        return 1;

    fprintf(stderr, "runtime policy case failed: %s\n", name);
    return 0;
}

int main(void) {
    int passed = 1;
    passed &= expect_due(1, 0, 0, 1, 1, 100, 100, "deadline equality");
    passed &= expect_due(1, 0, 0, 2, 3, 100, 101, "deadline exceeded");
    passed &= expect_due(0, 1, 0, 1, 1, 100, 100, "local yield owns run");
    passed &= expect_due(0, 0, 1, 1, 1, 100, 100, "interrupt handler");
    passed &= expect_due(0, 0, 0, 0, 1, 100, 100, "no scheduler");
    passed &= expect_due(0, 0, 0, 1, 0, 100, 100, "no bounce owner");
    passed &= expect_due(0, 0, 0, 1, 1, 0, 100, "deadline disabled");
    passed &= expect_due(0, 0, 0, 1, 1, 101, 100, "deadline pending");
    return passed ? 0 : 1;
}
