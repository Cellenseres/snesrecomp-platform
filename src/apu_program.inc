/* Appended to the pinned apu.c by snesrecomp_platform_overlay_apu_program. */

#include "snesrecomp_platform/apu_program.h"

static const SnesRecompApuProgram *s_apu_program;
static bool s_apu_program_poked;
static uint32_t s_apu_program_capabilities;
#include "apu_cpu_span.inc"

void snesrecomp_apu_set_program(const SnesRecompApuProgram *program) {
  apu_program_span_before_access(NULL);
  s_apu_program = program;
  s_apu_program_capabilities = 0;
  s_apu_program_poked = false;
}

bool snesrecomp_apu_set_program_with_capabilities(
    const SnesRecompApuProgram *program, uint32_t capabilities) {
  uint32_t supported=SNESRECOMP_APU_IDLE_STABLE_DSP;
#if defined(SNESRECOMP_PLATFORM_APU_CPU_SPANS)
  supported|=SNESRECOMP_APU_CPU_INDEPENDENT;
#endif
  if ((capabilities & ~supported) || (!program && capabilities) ||
      ((capabilities & SNESRECOMP_APU_CPU_INDEPENDENT) &&
       !(capabilities & SNESRECOMP_APU_IDLE_STABLE_DSP))) return false;
  snesrecomp_apu_set_program(program);
  s_apu_program_capabilities = capabilities;
  return true;
}

bool snesrecomp_apu_program_supported(void) { return true; }

static void apu_program_poke(void) { s_apu_program_poked = true; }

/* Only loads have peek; saves and hashers do not. */
static void apu_program_saveload(Apu *apu, SaveLoadInfo *sli) {
  const SnesRecompApuProgram *program = s_apu_program;
  if (program && program->loaded && sli->peek)
    program->loaded(program->context, apu);
}

static bool apu_program_take_over(Apu *apu) {
  const SnesRecompApuProgram *program = s_apu_program;
  if (!program || !program->take_over(program->context, apu))
    return false;
  s_apu_program_poked = false;
  program->run(program->context, apu);
  return true;
}

/* Advances program-idle cycles; 0 when the next is an observable event.
 * Stops before every port write, counter tick, opcode and program access.
 * Native idle batches still execute every exact S-DSP sample. */
static uint32_t apu_program_skip(Apu *apu, uint64_t target, uint32_t max_cycles) {
  const SnesRecompApuProgram *program = s_apu_program;
  if (apu->portClock >= target)
    return 0;
  const bool native = program && program->active(program->context, apu);
  const bool batch_samples = native &&
      (s_apu_program_capabilities & SNESRECOMP_APU_IDLE_STABLE_DSP) != 0;
  uint32_t idle;
  if (native) {
    if (!program->idle || s_apu_program_poked)
      return 0;
    idle = program->idle(program->context, apu);
  } else {
    /* The SPC-700 only acts when its next opcode is due. */
    idle = apu->cpuCyclesLeft;
  }

  uint64_t span = target - apu->portClock;
  if (span > max_cycles)
    span = max_cycles;
  if (apu->portQHead != apu->portQTail) {
    const uint64_t at = apu->portQueue[apu->portQHead & (APU_PORT_QUEUE_LEN - 1)].target_cycle;
    if (at <= apu->portClock)
      return 0;
    if (at - apu->portClock < span)
      span = at - apu->portClock;
  }
  const uint32_t sample = (32u - (apu->cycles & 0x1f)) & 0x1f;
  /* A certified native idle interval has no program/port/timer event. Its
   * S-DSP samples still run at every exact boundary below, without repeating
   * the scheduler's active/idle/queue/limit discovery for each sample. An
   * interpreted opcode has no such contract and retains the old sample cut. */
  if (!batch_samples && span > sample)
    span = sample;
  if (span > idle)
    span = idle;
  for (int i = 0; i < 3; i++) {
    if (!apu->timer[i].enabled)
      continue;
    const uint32_t period = i == 2 ? 16 : 128;
    uint32_t reloads = (uint8_t)(apu->timer[i].target - apu->timer[i].divider);
    if (reloads == 0)
      reloads = 256;
    const uint32_t tick = apu->timer[i].cycles + (reloads - 1) * period;
    if (span > tick)
      span = tick;
  }
  if (span == 0)
    return 0;

  /* Bound service latency and offset arithmetic even for UINT32_MAX requests. */
  if (batch_samples && span > 1024u) span = 1024u;
  const uint32_t n = (uint32_t)span;
  /* No sample or timer-phase edge can occur in this short native idle.
   * Preserve the original final clock callback without general reload math. */
  if (batch_samples && n <= sample && n <= apu->timer[0].cycles &&
      n <= apu->timer[1].cycles && n <= apu->timer[2].cycles) {
    apu->timer[0].cycles = (uint8_t)(apu->timer[0].cycles - n);
    apu->timer[1].cycles = (uint8_t)(apu->timer[1].cycles - n);
    apu->timer[2].cycles = (uint8_t)(apu->timer[2].cycles - n);
    apu->cycles += n - 1;
    apu->portClock += n - 1;
    if (program->cycle) program->cycle(program->context, apu);
    ++apu->cycles;
    ++apu->portClock;
    return n;
  }
  if (batch_samples) {
    const uint32_t first_cycle=apu->cycles;
    const uint64_t first_clock=apu->portClock;
    /* Native idle promises that cycle only records the clock throughout this
     * interval. No timer counter, queued access or CPU-port event is crossed.
     * S-DSP consumes ARAM/registers, not the SPC's timer-divider scratch. All
     * samples, echo writes and backend events retain their original order. */
    for(uint32_t offset=sample;offset<n;offset+=32u) {
      apu->cycles=first_cycle+offset;apu->portClock=first_clock+offset;
      if(program->cycle)program->cycle(program->context,apu);
      dsp_cycle(apu->dsp);
    }
    apu->cycles=first_cycle;apu->portClock=first_clock;
  }
  for (int i = 0; i < 3; i++) {
    const uint32_t period = i == 2 ? 16 : 128;
    const uint32_t left = apu->timer[i].cycles;
    const uint32_t reloads = n > left ? (n - 1 - left) / period + 1 : 0;
    apu->timer[i].cycles = (uint8_t)(left + reloads * period - n);
    if (apu->timer[i].enabled)
      apu->timer[i].divider = (uint8_t)(apu->timer[i].divider + reloads);
  }
  if (!native)
    apu->cpuCyclesLeft = (uint8_t)(apu->cpuCyclesLeft - n);
  /* The last idle cycle still reports its clock. */
  apu->cycles += n - 1;
  apu->portClock += n - 1;
  if (native && program->cycle)
    program->cycle(program->context, apu);
  apu->cycles++;
  apu->portClock++;
  return n;
}

uint32_t snesrecomp_apu_program_skip(Apu *apu, uint32_t cycles) {
  apu_program_span_before_access(apu);
  return cycles ? apu_program_skip(apu, apu->portClock + cycles, cycles) : 0;
}

/* apu_cycle with the native program instead of the SPC-700. */
static bool apu_program_cycle(Apu *apu) {
  const SnesRecompApuProgram *program = s_apu_program;
  if (!program || !program->active(program->context, apu))
    return false;

  const uint32_t head = apu->portQHead;
  apu_drainPortQueue(apu);
  bool changed = head != apu->portQHead || s_apu_program_poked;
  s_apu_program_poked = false;

  if (program->cycle)
    program->cycle(program->context, apu);
  if ((apu->cycles & 0x1f) == 0)
    dsp_cycle(apu->dsp);

  extern uint64_t g_apu_timer0_total_ticks;
  for (int i = 0; i < 3; i++) {
    if (apu->timer[i].cycles == 0) {
      apu->timer[i].cycles = i == 2 ? 16 : 128;
      if (apu->timer[i].enabled) {
        apu->timer[i].divider++;
        if (apu->timer[i].divider == apu->timer[i].target) {
          apu->timer[i].divider = 0;
          apu->timer[i].counter++;
          apu->timer[i].counter &= 0xf;
          if (i == 0) g_apu_timer0_total_ticks++;
          changed = true;
        }
      }
    }
    apu->timer[i].cycles--;
  }

  if (changed)
    program->run(program->context, apu);
  apu->cycles++;
  apu->portClock++;
  return true;
}
