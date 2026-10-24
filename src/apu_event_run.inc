/* Only delay cycles without a device event may be combined. */
bool snesrecomp_apu_events_enabled = true;

bool apu_runToGuestCycle(Apu *apu, uint64_t guest_cycle, uint32_t max_cycles) {
    static int reference = -1;
    if (reference < 0) reference = getenv("SNESRECOMP_APU_EVENT_REFERENCE") != NULL;
    if (reference || !snesrecomp_apu_events_enabled)
        return snesrecomp_apu_run_reference(apu, guest_cycle, max_cycles);
    if (!apu->portTimeValid || guest_cycle < apu->portGuestAnchor) return true;
    uint64_t target = apu->portTargetAnchor + (guest_cycle - apu->portGuestAnchor);
    if (target < apu->portLastTarget) target = apu->portLastTarget;
    for (;;) {
        const bool queued = apu->portQHead != apu->portQTail;
        const uint64_t event = queued ?
            apu->portQueue[apu->portQHead & (APU_PORT_QUEUE_LEN - 1)].target_cycle : UINT64_MAX;
        if (apu->portClock >= target && (!queued || event > target)) return true;
        if (!max_cycles) return false;
        /* Frequent CPU-port synchronizations only owe a few clocks. Keep
         * those on the short original loop instead of scanning boundaries. */
        if (target <= apu->portClock || target-apu->portClock < 8u) {
            apu_cycle(apu);
            --max_cycles;
            continue;
        }
        unsigned delay = apu->cpuCyclesLeft;
        const unsigned dsp_delay = (32u - (apu->cycles & 31u)) & 31u;
        if (delay > dsp_delay) delay = dsp_delay;
        for (unsigned i = 0; i < 3; ++i)
            if (delay > apu->timer[i].cycles) delay = apu->timer[i].cycles;
        if (apu->portClock >= target || (queued && event <= apu->portClock)) delay = 0;
        if (delay && target - apu->portClock < delay) delay = (unsigned)(target - apu->portClock);
        if (delay && queued && event - apu->portClock < delay) delay = (unsigned)(event - apu->portClock);
        if (delay > max_cycles) delay = max_cycles;
        if (delay) {
            apu->cpuCyclesLeft -= delay;
            for (unsigned i = 0; i < 3; ++i) apu->timer[i].cycles -= delay;
            apu->cycles += delay;
            apu->portClock += delay;
            max_cycles -= delay;
        } else {
            apu_cycle(apu);
            --max_cycles;
        }
    }
}
