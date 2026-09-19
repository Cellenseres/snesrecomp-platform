#ifndef SNESRECOMP_PLATFORM_PRESENT_TIMELINE_H
#define SNESRECOMP_PLATFORM_PRESENT_TIMELINE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Guest cadence and display cadence, kept apart. */

typedef struct SnesRecompPresentTimeline {
    /* Exact rationals, so neither cadence drifts. */
    unsigned guest_rate_num;
    unsigned guest_rate_den;
    uint64_t guest_period_us;
    uint64_t guest_period_rem;
    uint64_t guest_remainder;

    unsigned display_millihertz;
    uint64_t present_period_us;
    uint64_t present_period_rem;
    uint64_t present_remainder;
    uint64_t present_divisor;

    uint64_t next_guest_us;
    uint64_t next_present_us;
    uint64_t last_guest_us;
    uint64_t last_present_us;
    bool started;
    /* A guest frame is composed but not yet shown. */
    bool guest_pending;

    uint64_t guest_frames;
    uint64_t presents;
    uint64_t extra_presents;
    uint64_t resyncs;
    uint64_t late_guest_frames;
    uint64_t worst_guest_late_us;
    uint64_t last_guest_interval_us;
    uint64_t last_present_interval_us;
} SnesRecompPresentTimeline;

typedef struct SnesRecompPresentStep {
    /* Run one guest frame, then compose its picture. */
    bool run_guest;
    /* Submit the composed picture. */
    bool present;
    /* This present would show a guest frame nothing has shown yet.
       A caller that cannot repeat a frame must present when set. */
    bool carries_guest_frame;
    /* Where this present falls between two guest frames. */
    float alpha;
    /* Idle at most this long before stepping again. */
    uint64_t sleep_us;
} SnesRecompPresentStep;

/* A zero rate or denominator is rejected. */
bool snesrecomp_present_timeline_init(SnesRecompPresentTimeline *timeline,
                                      unsigned guest_rate_num,
                                      unsigned guest_rate_den);

/* Refresh in millihertz; too slow keeps one present per frame. */
void snesrecomp_present_timeline_set_display(
    SnesRecompPresentTimeline *timeline, unsigned display_millihertz);

/* True once presents are scheduled apart from guest frames. */
bool snesrecomp_present_timeline_is_decoupled(
    const SnesRecompPresentTimeline *timeline);

/* Drop the schedule on reset, load or scene change. */
void snesrecomp_present_timeline_reset(SnesRecompPresentTimeline *timeline,
                                       uint64_t now_us);

/* Decide this iteration and consume the deadlines it answers. */
void snesrecomp_present_timeline_step(SnesRecompPresentTimeline *timeline,
                                      uint64_t now_us,
                                      SnesRecompPresentStep *out_step);

/* Record a present the caller actually submitted. */
void snesrecomp_present_timeline_note_present(
    SnesRecompPresentTimeline *timeline, uint64_t now_us);

typedef struct SnesRecompPresentStats {
    uint64_t guest_frames;
    uint64_t presents;
    /* Presents that carried no new guest frame. */
    uint64_t extra_presents;
    /* Schedules dropped after falling too far behind. */
    uint64_t resyncs;
    uint64_t late_guest_frames;
    uint64_t worst_guest_late_us;
    uint64_t last_guest_interval_us;
    uint64_t last_present_interval_us;
} SnesRecompPresentStats;

void snesrecomp_present_timeline_stats(
    const SnesRecompPresentTimeline *timeline,
    SnesRecompPresentStats *out_stats);

#ifdef __cplusplus
}
#endif

#endif
