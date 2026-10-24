#ifndef SNESRECOMP_PLATFORM_BEAM_CLOCK_H
#define SNESRECOMP_PLATFORM_BEAM_CLOCK_H
#include <stdbool.h>
#include <stdint.h>

/* Available when the core overlay publishes SNESRECOMP_PLATFORM_BEAM_HOLD_QUERY. */
#ifdef __cplusplus
extern "C" {
#endif
uint32_t snesrecomp_beam_hold_active(void);
#ifdef __cplusplus
}
#endif

/* Certify a short step strictly before every event handled by the runner's
 * beam walk. This does not advance guest time, APU state or the beam itself.
 * The consumer must still update H position, the busy timer and VBlank state.
 * IRQ-enabled walks stay with the comparator, including its diagnostic flags.
 * Exact HBlank/line edges and automatic-pad latches are deliberately rejected.
 * Invalid/restored positions also stay with the reference walk. */
static inline bool snesrecomp_beam_eventless_step(uint32_t h, uint32_t v,
    uint32_t clocks, bool check_irq, bool irq_enabled, bool auto_joy_read) {
    if (!clocks || h >= 1364u || v >= 262u ||
        (check_irq && irq_enabled) || (auto_joy_read && v == 225u && !h))
        return false;
    const uint32_t edge = check_irq && v < 225u && h < 1024u ? 1024u : 1364u;
    return clocks < edge - h;
}
#endif
