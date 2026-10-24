#include "snesrecomp_platform/task.h"
#include <time.h>

/* No workers: submit declines so the caller executes exactly once. */
void snesrecomp_task_set_thread_hook(void (*hook)(unsigned)) { (void)hook; }
bool snesrecomp_task_enable(bool enabled) { return !enabled; }
unsigned snesrecomp_task_worker_count(void) { return 0; }
bool snesrecomp_task_submit(unsigned slot, void (*fn)(void *), void *arg) {
    (void)slot; (void)fn; (void)arg;
    return false;
}
void snesrecomp_task_wait(void) {}
void snesrecomp_task_shutdown(void) {}
uint64_t snesrecomp_task_busy_us(unsigned slot) { (void)slot; return 0; }
/* SDK-free serial fallback measures process time, not GPU/wait durations. */
uint64_t snesrecomp_now_us(void) {
    return (uint64_t)clock() * 1000000u / CLOCKS_PER_SEC;
}
