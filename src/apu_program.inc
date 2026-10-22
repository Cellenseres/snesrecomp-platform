/* Appended to the pinned apu.c by snesrecomp_platform_overlay_apu_program. */

#include "snesrecomp_platform/apu_program.h"

static const SnesRecompApuProgram *s_apu_program;
static bool s_apu_program_poked;

void snesrecomp_apu_set_program(const SnesRecompApuProgram *program) {
  s_apu_program = program;
  s_apu_program_poked = false;
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
