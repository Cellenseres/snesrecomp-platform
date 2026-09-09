#include "snesrecomp_platform/task.h"

#include <SDL3/SDL.h>

typedef struct TaskWorker {
    SDL_Thread *thread;
    SDL_Semaphore *request;
    SDL_Semaphore *done;
    void (*fn)(void *);
    void *arg;
    uint64_t busy_us;
    bool pending;            /* owned by the submitting thread alone */
} TaskWorker;

static TaskWorker s_workers[SNESRECOMP_TASK_WORKERS];
static SDL_AtomicInt s_running;
static unsigned s_worker_count;
static bool s_enabled;
static void (*s_thread_hook)(unsigned index);

/* The performance frequency is fixed for the process, so reading it back from
 * SDL on every measurement only pays for a cross-library call that can never
 * return anything new. Caching it is idempotent, which is what makes the
 * unguarded store safe when a worker thread and the main thread first measure
 * at the same moment.
 *
 * The scaling is split into whole seconds plus a remainder rather than the
 * obvious `counter * 1000000 / freq`: at the 10 MHz counter Windows reports,
 * that product overflows 64 bits after about three weeks of host uptime and
 * the measurement wraps into nonsense. */
static uint64_t now_us(void) {
    static uint64_t s_freq;
    uint64_t counter;
    if (!s_freq) {
        s_freq = SDL_GetPerformanceFrequency();
        if (!s_freq)
            return 0;
    }
    counter = SDL_GetPerformanceCounter();
    return (counter / s_freq) * UINT64_C(1000000) +
           (counter % s_freq) * UINT64_C(1000000) / s_freq;
}

uint64_t snesrecomp_now_us(void) {
    return now_us();
}

void snesrecomp_task_set_thread_hook(void (*hook)(unsigned index)) {
    s_thread_hook = hook;
}

static int SDLCALL task_worker(void *param) {
    TaskWorker *w = (TaskWorker *)param;
    if (s_thread_hook)
        s_thread_hook((unsigned)(w - s_workers));
    for (;;) {
        SDL_WaitSemaphore(w->request);
        if (!SDL_GetAtomicInt(&s_running))
            break;
        if (w->fn) {
            const uint64_t begin = now_us();
            w->fn(w->arg);
            w->busy_us += now_us() - begin;
        }
        SDL_SignalSemaphore(w->done);
    }
    return 0;
}

bool snesrecomp_task_enable(bool enabled) {
    if (!enabled) {
        snesrecomp_task_shutdown();
        return true;
    }
    if (s_enabled)
        return true;

    SDL_SetAtomicInt(&s_running, 1);
    s_worker_count = 0;
    for (unsigned i = 0; i < SNESRECOMP_TASK_WORKERS; i++) {
        TaskWorker *w = &s_workers[i];
        w->request = SDL_CreateSemaphore(0);
        w->done = SDL_CreateSemaphore(0);
        if (!w->request || !w->done)
            break;
        /* Naming them apart keeps them tellable in a thread dump. */
        char name[32];
        SDL_snprintf(name, sizeof name, "snesrecomp-task%u", i);
        w->thread = SDL_CreateThread(task_worker, name, w);
        if (!w->thread)
            break;
        s_worker_count++;
    }

    /* Partial success is usable: the caller runs whatever it could not hand
     * over. Total failure is not, so unwind to the clean single-threaded
     * state rather than leaving half-built workers behind. */
    if (s_worker_count == 0) {
        SDL_SetAtomicInt(&s_running, 0);
        snesrecomp_task_shutdown();
        return false;
    }

    s_enabled = true;
    return true;
}

unsigned snesrecomp_task_worker_count(void) {
    return s_enabled ? s_worker_count : 0u;
}

bool snesrecomp_task_submit(unsigned slot, void (*fn)(void *),
                                     void *arg) {
    TaskWorker *w;
    if (!s_enabled || !fn || slot >= s_worker_count)
        return false;
    w = &s_workers[slot];
    if (w->pending)
        return false;
    w->fn = fn;
    w->arg = arg;
    w->pending = true;
    SDL_SignalSemaphore(w->request);
    return true;
}

void snesrecomp_task_wait(void) {
    for (unsigned i = 0; i < SNESRECOMP_TASK_WORKERS; i++) {
        TaskWorker *w = &s_workers[i];
        if (!w->pending)
            continue;
        SDL_WaitSemaphore(w->done);
        w->pending = false;
    }
}

void snesrecomp_task_shutdown(void) {
    snesrecomp_task_wait();
    SDL_SetAtomicInt(&s_running, 0);

    /* Every worker has to be woken before any is joined: they all block on
     * their own semaphore, and a thread still waiting would never observe the
     * cleared running flag. */
    for (unsigned i = 0; i < SNESRECOMP_TASK_WORKERS; i++)
        if (s_workers[i].thread)
            SDL_SignalSemaphore(s_workers[i].request);

    for (unsigned i = 0; i < SNESRECOMP_TASK_WORKERS; i++) {
        TaskWorker *w = &s_workers[i];
        if (w->thread) {
            SDL_WaitThread(w->thread, NULL);
            w->thread = NULL;
        }
        if (w->request) {
            SDL_DestroySemaphore(w->request);
            w->request = NULL;
        }
        if (w->done) {
            SDL_DestroySemaphore(w->done);
            w->done = NULL;
        }
        w->fn = NULL;
        w->arg = NULL;
        w->pending = false;
    }
    s_enabled = false;
    s_worker_count = 0;
}

uint64_t snesrecomp_task_busy_us(unsigned slot) {
    if (slot >= SNESRECOMP_TASK_WORKERS)
        return 0;
    return s_workers[slot].busy_us;
}
