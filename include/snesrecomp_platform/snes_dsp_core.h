/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * S-DSP synthesis derived from snesrecomp 678b0656, runner/src/snes/dsp.c.
 * Copyright (c) 2026 Matthew Stanley. See licenses/SNES_DSP_NOTICE.md.
 */
#ifndef SNESRECOMP_PLATFORM_SNES_DSP_CORE_H
#define SNESRECOMP_PLATFORM_SNES_DSP_CORE_H

#ifdef __teak__
typedef unsigned short SrSnesDspU16;
typedef signed short SrSnesDspS16;
typedef unsigned long SrSnesDspU32;
typedef signed long SrSnesDspS32;
#else
#include <stdint.h>
typedef uint16_t SrSnesDspU16;
typedef int16_t SrSnesDspS16;
typedef uint32_t SrSnesDspU32;
typedef int32_t SrSnesDspS32;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* These are LOCAL types, never a wire structure. Teak has 16-bit chars/int.
 * ARAM bytes use the low eight bits of a logical U16. All callbacks wrap their
 * addresses at 65536. They are synchronous, infallible after validated setup,
 * and must observe writes from earlier calls before reads return. A remote
 * backend must establish exclusive ownership of its memory before cycling.
 */
typedef struct SrSnesDspMemory {
    void *context;
    SrSnesDspU16 (*read8)(void *context, SrSnesDspU16 address);
    void (*write8)(void *context, SrSnesDspU16 address, SrSnesDspU16 value);
} SrSnesDspMemory;

typedef struct SrSnesDspVoice {
    SrSnesDspU16 pitch, pitchCounter, pitchModulation;
    SrSnesDspS16 decodeBuffer[19];
    SrSnesDspU16 srcn, decodeOffset, previousFlags;
    SrSnesDspS16 old, older;
    SrSnesDspU16 useNoise, adsrRates[4], rateCounter, adsrState;
    SrSnesDspU16 sustainLevel, useGain, gainMode, directGain, gainValue, gain;
    SrSnesDspU16 keyOn, keyOff;
    SrSnesDspS16 sampleOut, volumeL, volumeR;
    SrSnesDspU16 echoEnable;
} SrSnesDspVoice;

typedef struct SrSnesDspCore {
    SrSnesDspU16 ram[128];
    SrSnesDspVoice channel[8];
    SrSnesDspU16 dirPage, evenCycle, mute, reset;
    SrSnesDspS16 masterVolumeL, masterVolumeR, noiseSample;
    SrSnesDspU16 noiseRate, noiseCounter, echoWrites;
    SrSnesDspS16 echoVolumeL, echoVolumeR, feedbackVolume;
    SrSnesDspU16 echoBufferAdr, echoDelay, echoRemain, echoBufferIndex;
    SrSnesDspU16 firBufferIndex;
    SrSnesDspS16 firValues[8], firBufferL[8], firBufferR[8];
    SrSnesDspMemory memory;
} SrSnesDspCore;

/* Versioned portable state, measured in 16-bit WORDS, not sizeof(char).
 * 0:0x5344, 1:0x5350, 2:version 1, 3:544, 4..5:zero reserved.
 * 6..133: 128 registers. 134..501: eight voices (46 words each, fields
 * in declaration order). 502..543: global fields, then FIR arrays, in
 * declaration order. Signed fields are two's-complement 16-bit words.
 * ARAM, output queues, APU time and callbacks are deliberately separate.
 * Wire users explicitly encode/decode every word little-endian.
 */
#define SR_SNES_DSP_CORE_STATE_VERSION 1u
#define SR_SNES_DSP_CORE_STATE_WORDS 544u

void sr_snes_dsp_core_init(SrSnesDspCore *core, const SrSnesDspMemory *memory);
void sr_snes_dsp_core_reset(SrSnesDspCore *core);
/* Exactly one native sample (32 reference APU cycles). Clock ownership and
 * output backpressure belong to the event engine; reserve output first.
 */
void sr_snes_dsp_core_cycle(SrSnesDspCore *core, SrSnesDspS16 stereo[2]);
SrSnesDspU16 sr_snes_dsp_core_read(const SrSnesDspCore *core, SrSnesDspU16 address);
void sr_snes_dsp_core_write(SrSnesDspCore *core, SrSnesDspU16 address,
                            SrSnesDspU16 value);
/* Returns 1 on success, 0 for wrong count/header/invalid state. Loading is
 * transactional on failure and preserves the installed memory callbacks.
 */
int sr_snes_dsp_core_save(const SrSnesDspCore *core, SrSnesDspU16 *words,
                          SrSnesDspU16 count);
int sr_snes_dsp_core_load(SrSnesDspCore *core, const SrSnesDspU16 *words,
                          SrSnesDspU16 count);
/* Pure interpolation, exposed to make exhaustive arithmetic checks possible.
 * sample_num must be 0..15; fraction must be 0..255.
 */
SrSnesDspS16 sr_snes_dsp_core_gaussian(const SrSnesDspVoice *voice,
                                      SrSnesDspU16 sample_num,
                                      SrSnesDspU16 fraction);
extern const SrSnesDspU16 sr_snes_dsp_gauss[512];

#ifdef __cplusplus
}
#endif
#endif
