#include "snesrecomp_platform/present_timeline.h"

#include <string.h>

enum {
    /* Beyond this is a stall, not a slow frame. */
    TIMELINE_RESYNC_US = 250000u,
    /* Longest idle handed back to the caller. */
    TIMELINE_MAX_SLEEP_US = 100000u,
    /* Decouple only above this much of the guest rate. */
    TIMELINE_DECOUPLE_PERCENT = 102u,
};

static void SetPeriod(uint64_t numerator, uint64_t denominator,
                      uint64_t *period_us, uint64_t *period_rem) {
    *period_us = numerator / denominator;
    *period_rem = numerator % denominator;
}

/* Advance a deadline by its exact rational period. */
static void Advance(uint64_t *deadline_us, uint64_t *remainder,
                    uint64_t period_us, uint64_t period_rem,
                    uint64_t divisor) {
    *deadline_us += period_us;
    if (!period_rem || !divisor)
        return;
    *remainder += period_rem;
    if (*remainder >= divisor) {
        *remainder -= divisor;
        (*deadline_us)++;
    }
}

static void TakeGuestFrame(SnesRecompPresentTimeline *timeline,
                           uint64_t now_us) {
    const uint64_t late = now_us - timeline->next_guest_us;

    if (late > timeline->guest_period_us) {
        timeline->late_guest_frames++;
        if (late > timeline->worst_guest_late_us)
            timeline->worst_guest_late_us = late;
    }
    timeline->last_guest_interval_us = now_us - timeline->last_guest_us;
    timeline->last_guest_us = now_us;
    timeline->guest_frames++;
    timeline->guest_pending = true;
    Advance(&timeline->next_guest_us, &timeline->guest_remainder,
            timeline->guest_period_us, timeline->guest_period_rem,
            timeline->guest_rate_num);
}

static void TakePresentSlot(SnesRecompPresentTimeline *timeline,
                            uint64_t now_us) {
    Advance(&timeline->next_present_us, &timeline->present_remainder,
            timeline->present_period_us, timeline->present_period_rem,
            timeline->present_divisor);
    if (now_us > timeline->next_present_us &&
        now_us - timeline->next_present_us > timeline->present_period_us)
        timeline->next_present_us = now_us + timeline->present_period_us;
}

static float AlphaAt(const SnesRecompPresentTimeline *timeline,
                     uint64_t now_us) {
    const uint64_t elapsed = now_us - timeline->last_guest_us;

    if (elapsed >= timeline->guest_period_us)
        return 0.999999f;
    return (float)((double)elapsed / (double)timeline->guest_period_us);
}

static uint64_t SleepUntilNext(const SnesRecompPresentTimeline *timeline,
                               uint64_t now_us) {
    uint64_t wake_us = timeline->next_guest_us;

    if (snesrecomp_present_timeline_is_decoupled(timeline) &&
        timeline->next_present_us < wake_us)
        wake_us = timeline->next_present_us;
    if (wake_us <= now_us)
        return 0u;
    return wake_us - now_us > TIMELINE_MAX_SLEEP_US
        ? (uint64_t)TIMELINE_MAX_SLEEP_US
        : wake_us - now_us;
}

bool snesrecomp_present_timeline_init(SnesRecompPresentTimeline *timeline,
                                      unsigned guest_rate_num,
                                      unsigned guest_rate_den) {
    if (!timeline || !guest_rate_num || !guest_rate_den)
        return false;
    memset(timeline, 0, sizeof *timeline);
    timeline->guest_rate_num = guest_rate_num;
    timeline->guest_rate_den = guest_rate_den;
    SetPeriod(1000000ull * guest_rate_den, guest_rate_num,
              &timeline->guest_period_us, &timeline->guest_period_rem);
    return true;
}

void snesrecomp_present_timeline_set_display(
    SnesRecompPresentTimeline *timeline, unsigned display_millihertz) {
    uint64_t guest_millihertz;

    if (!timeline || !timeline->guest_rate_den)
        return;
    timeline->display_millihertz = display_millihertz;
    timeline->present_period_us = 0;
    timeline->present_period_rem = 0;
    timeline->present_remainder = 0;
    timeline->present_divisor = 0;

    guest_millihertz =
        1000ull * timeline->guest_rate_num / timeline->guest_rate_den;
    if (!display_millihertz ||
        (uint64_t)display_millihertz * 100ull <
            guest_millihertz * TIMELINE_DECOUPLE_PERCENT)
        return;

    SetPeriod(1000000000ull, display_millihertz,
              &timeline->present_period_us, &timeline->present_period_rem);
    timeline->present_divisor = display_millihertz;
}

bool snesrecomp_present_timeline_is_decoupled(
    const SnesRecompPresentTimeline *timeline) {
    return timeline && timeline->present_period_us != 0;
}

void snesrecomp_present_timeline_reset(SnesRecompPresentTimeline *timeline,
                                       uint64_t now_us) {
    if (!timeline)
        return;
    timeline->guest_remainder = 0;
    timeline->present_remainder = 0;
    timeline->next_guest_us = now_us;
    timeline->next_present_us = now_us;
    timeline->last_guest_us = now_us;
    timeline->last_present_us = now_us;
    timeline->guest_pending = false;
    timeline->started = true;
}

void snesrecomp_present_timeline_note_present(
    SnesRecompPresentTimeline *timeline, uint64_t now_us) {
    if (!timeline)
        return;
    if (timeline->guest_pending)
        timeline->guest_pending = false;
    else
        timeline->extra_presents++;
    timeline->last_present_interval_us = now_us - timeline->last_present_us;
    timeline->last_present_us = now_us;
    timeline->presents++;
}

void snesrecomp_present_timeline_step(SnesRecompPresentTimeline *timeline,
                                      uint64_t now_us,
                                      SnesRecompPresentStep *out_step) {
    SnesRecompPresentStep step;

    if (!out_step)
        return;
    memset(&step, 0, sizeof step);
    if (!timeline || !timeline->guest_period_us) {
        step.run_guest = true;
        step.present = true;
        *out_step = step;
        return;
    }
    if (!timeline->started)
        snesrecomp_present_timeline_reset(timeline, now_us);

    if (now_us > timeline->next_guest_us &&
        now_us - timeline->next_guest_us > TIMELINE_RESYNC_US) {
        timeline->resyncs++;
        snesrecomp_present_timeline_reset(timeline, now_us);
    }

    if (now_us >= timeline->next_guest_us) {
        TakeGuestFrame(timeline, now_us);
        step.run_guest = true;
    }

    if (!snesrecomp_present_timeline_is_decoupled(timeline)) {
        step.present = step.run_guest;
    } else if (now_us >= timeline->next_present_us) {
        /* Free-running; a guest frame must not restart it. */
        TakePresentSlot(timeline, now_us);
        step.present = true;
    }

    step.alpha = AlphaAt(timeline, now_us);
    if (!step.run_guest && !step.present)
        step.sleep_us = SleepUntilNext(timeline, now_us);
    *out_step = step;
}

void snesrecomp_present_timeline_stats(
    const SnesRecompPresentTimeline *timeline,
    SnesRecompPresentStats *out_stats) {
    if (!out_stats)
        return;
    memset(out_stats, 0, sizeof *out_stats);
    if (!timeline)
        return;
    out_stats->guest_frames = timeline->guest_frames;
    out_stats->presents = timeline->presents;
    out_stats->extra_presents = timeline->extra_presents;
    out_stats->resyncs = timeline->resyncs;
    out_stats->late_guest_frames = timeline->late_guest_frames;
    out_stats->worst_guest_late_us = timeline->worst_guest_late_us;
    out_stats->last_guest_interval_us = timeline->last_guest_interval_us;
    out_stats->last_present_interval_us = timeline->last_present_interval_us;
}
