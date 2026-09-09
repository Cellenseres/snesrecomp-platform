#ifndef SNESRECOMP_PLATFORM_TASK_H
#define SNESRECOMP_PLATFORM_TASK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Helper threads for work the caller joins within the same frame: split a loop
 * into N pieces, hand N-1 over, run the last here, join. Not a thread pool --
 * the piece count is known and the join always happens, so queueing would be
 * machinery for a problem this does not have.
 */

/* Helper threads, i.e. one fewer than the number of pieces the caller can
 * split into, since the caller runs one itself. */
#define SNESRECOMP_PLATFORM_TASK_WORKERS 2

/* Called once on each helper thread before it takes work, with the helper's
 * index. Core placement is host policy: oversubscribing a small console CPU
 * can make a third band slower than two. */
void snesrecomp_platform_task_set_thread_hook(void (*hook)(unsigned index));

bool snesrecomp_platform_task_enable(bool enabled);

/* How many helpers are actually running. Fewer than requested means thread
 * creation failed; the caller must run those pieces itself, which is the
 * single-threaded behaviour. */
unsigned snesrecomp_platform_task_worker_count(void);

/* Hand `fn(arg)` to helper `slot`. False means it is unavailable or busy --
 * run the piece on the calling thread instead. */
bool snesrecomp_platform_task_submit(unsigned slot, void (*fn)(void *),
                                     void *arg);

/* Block until every submitted piece is done. Safe with nothing in flight. */
void snesrecomp_platform_task_wait(void);

void snesrecomp_platform_task_shutdown(void);

/* Cumulative microseconds helper `slot` spent working. A large gap against
 * the caller's own piece means an unbalanced split, not failed threading. */
uint64_t snesrecomp_platform_task_busy_us(unsigned slot);

/* Monotonic host clock in microseconds, so instrumentation elsewhere shares
 * one time base with the task and audio workers. */
uint64_t snesrecomp_platform_now_us(void);

#ifdef __cplusplus
}
#endif

#endif
