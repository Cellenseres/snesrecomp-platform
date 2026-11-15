/* Private part of the native APU overlay; no SDK or guest-game dependency. */
#if defined(SNESRECOMP_PLATFORM_APU_CPU_SPANS)
static struct {
  Apu *owner;
  uint64_t first;
  uint32_t limit;
} s_apu_cpu_span;
static SnesRecompApuCpuSpanStats s_apu_cpu_span_stats;
extern int snesrecomp_apu_span_trace_producer(void);

static void apu_program_span_finish(bool forced) {
  Apu *apu=s_apu_cpu_span.owner;
  if(!apu)return;
  const uint64_t target=apu->portClock;
  apu->portClock=s_apu_cpu_span.first;
  s_apu_cpu_span.owner=NULL;
  ++s_apu_cpu_span_stats.finished;
  if(forced)++s_apu_cpu_span_stats.forced_finishes;
  const int producer=snesrecomp_apu_span_trace_producer();
  audio_trace_set_producer(AUDIO_TRACE_PRODUCER_CPU);
  while(apu->portClock<target) {
    const uint32_t left=(uint32_t)(target-apu->portClock);
    if(left>1u && apu_program_skip(apu,target,left))continue;
    apu_cycle(apu);
  }
  audio_trace_set_producer(producer);
}
static void apu_program_span_before_access(const Apu *apu) {
  (void)apu;
  if(s_apu_cpu_span.owner)apu_program_span_finish(true);
}
bool snesrecomp_apu_cpu_span_begin(Apu *apu,uint32_t max_cycles) {
#if defined(SNES_COSIM) || defined(SNESRECOMP_INTERP_PROFILE) || \
    (defined(SNESRECOMP_TRACE) && SNESRECOMP_TRACE) || \
    (defined(SNESRECOMP_REVERSE_DEBUG) && SNESRECOMP_REVERSE_DEBUG)
  (void)apu;(void)max_cycles;return false;
#endif
  if(s_apu_cpu_span.owner || !apu || !max_cycles || max_cycles>1024u ||
     apu->portClock>UINT64_MAX-max_cycles || apu->portQHead!=apu->portQTail ||
     !s_apu_program || s_apu_program_poked ||
     (s_apu_program_capabilities & (SNESRECOMP_APU_IDLE_STABLE_DSP|
       SNESRECOMP_APU_CPU_INDEPENDENT)) !=
       (SNESRECOMP_APU_IDLE_STABLE_DSP|SNESRECOMP_APU_CPU_INDEPENDENT) ||
     !s_apu_program->active(s_apu_program->context,apu))return false;
  s_apu_cpu_span.owner=apu;s_apu_cpu_span.first=apu->portClock;
  s_apu_cpu_span.limit=max_cycles;++s_apu_cpu_span_stats.started;return true;
}
void snesrecomp_apu_cpu_span_end(Apu *apu) {
  if(apu && s_apu_cpu_span.owner==apu)apu_program_span_finish(false);
}
static bool apu_program_span_defer(Apu *apu,uint64_t cycles) {
  if(!apu || s_apu_cpu_span.owner!=apu)return false;
  if(snesrecomp_apu_span_trace_producer()!=AUDIO_TRACE_PRODUCER_CPU) {
    apu_program_span_finish(true);return false;
  }
  const uint64_t used=apu->portClock-s_apu_cpu_span.first;
  if(apu->portQHead!=apu->portQTail || used>s_apu_cpu_span.limit ||
      cycles>s_apu_cpu_span.limit-used) {apu_program_span_finish(true);return false;}
  apu->portClock+=cycles;s_apu_cpu_span_stats.cycles_deferred+=cycles;return true;
}
uint32_t snesrecomp_apu_cpu_span_defer_cycles(Apu *apu,uint32_t cycles) {
  if(!apu_program_span_defer(apu,cycles))return 0;
  ++s_apu_cpu_span_stats.relative_calls;return cycles;
}
static bool apu_program_span_target(Apu *apu,uint64_t target,uint32_t maximum) {
  if(s_apu_cpu_span.owner!=apu)return false;
  const uint64_t cycles=target>apu->portClock?target-apu->portClock:0;
  if(cycles>maximum) {apu_program_span_finish(true);return false;}
  if(!apu_program_span_defer(apu,cycles))return false;
  ++s_apu_cpu_span_stats.absolute_calls;return true;
}
void snesrecomp_apu_cpu_span_stats(SnesRecompApuCpuSpanStats *out) {
  if(out)*out=s_apu_cpu_span_stats;
}
bool snesrecomp_apu_cpu_span_open(const Apu *apu) {
  return apu && s_apu_cpu_span.owner==apu;
}
#else
static void apu_program_span_before_access(const Apu *apu) {(void)apu;}
bool snesrecomp_apu_cpu_span_begin(Apu *apu,uint32_t max_cycles) {
  (void)apu;(void)max_cycles;return false;
}
void snesrecomp_apu_cpu_span_end(Apu *apu) {(void)apu;}
bool snesrecomp_apu_cpu_span_open(const Apu *apu) {(void)apu;return false;}
void snesrecomp_apu_cpu_span_stats(SnesRecompApuCpuSpanStats *out) {
  if(out)memset(out,0,sizeof *out);
}
#endif
