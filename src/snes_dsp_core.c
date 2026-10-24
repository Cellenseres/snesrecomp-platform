/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Derived from snesrecomp 678b0656, runner/src/snes/dsp.c and dsp.h.
 * Copyright (c) 2026 Matthew Stanley. See licenses/SNES_DSP_NOTICE.md.
 * The reference is deliberately sample-level, not a transistor-level model.
 */
#include "snesrecomp_platform/snes_dsp_core.h"

typedef SrSnesDspU16 U16;
typedef SrSnesDspS16 S16;
typedef SrSnesDspU32 U32;
typedef SrSnesDspS32 S32;

#ifdef __teak__
#define SR_CORE_Y_RO __attribute__((section(".yrodata")))
#else
#define SR_CORE_Y_RO
static S32 product(S16 a, S16 b) { return (S32)a * (S32)b; }
#endif

/* Arithmetic shifts and wraps are explicit: no negative left shifts, signed
 * overflow, host int-width promotion or implementation-defined narrowing.
 */
static S32 sar(S32 value, U16 bits) {
#ifdef __teak__
    /* Keep this opaque to the pinned optimizer. When a later result wraps16,
     * it may replace an inlined arithmetic shift by a logical 40-bit shift.
     * A subsequent 32-bit temporary spill then saturates before that wrap.
     * The target helper normalizes signed32 and performs the arithmetic shift.
     */
    extern S32 sr_dsp_sar32(S32 value, U16 bits);
    return sr_dsp_sar32(value, bits);
#else
    if (value >= 0) return (S32)((U32)value >> bits);
    return -1L - (S32)((U32)(-1L - value) >> bits);
#endif
}
/* Keep the multiply and arithmetic shift in one Teak leaf call. */
static S32 product_shift(S16 a, S16 b, U16 bits) {
#ifdef __teak__
    extern S32 sr_dsp_product_shift(S16, S16, U16);
    return sr_dsp_product_shift(a, b, bits);
#else
    return sar(product(a, b), bits);
#endif
}

static S16 wrap16(S32 value) {
    U16 low = (U16)(U32)value;
    return low < 0x8000u ? (S16)low : (S16)((S32)low - 65536L);
}
static S16 signed8(U16 value) {
    value &= 255u;
    return value < 128u ? (S16)value : (S16)((S32)value - 256L);
}
static S16 clamp16(S32 value) {
    return value < -32768L ? -32768 : value > 32767L ? 32767 : (S16)value;
}
/* Every accumulated voice bus is clamped after EACH voice, in order. */
static S16 mix_voice(S16 accum, S16 sample, S16 volume) {
#ifdef __teak__
    extern S16 sr_dsp_mix_voice(S16, S16, S16);
    return sr_dsp_mix_voice(accum, sample, volume);
#else
    return clamp16((S32)accum + product_shift(sample, volume, 6));
#endif
}

static S16 wrap15(S32 value) {
    U16 low = (U16)(U32)value & 0x7fffu;
    return low < 0x4000u ? (S16)low : (S16)((S32)low - 32768L);
}
#ifdef SR_SNES_DSP_TEAK_MODULE
/* The pinned Teak backend cannot select indirect C calls. The static module
 * binds the exact same context operations to its ordered event engine. */
extern U16 sr_snes_dsp_teak_read8(void *context, U16 address);
extern void sr_snes_dsp_teak_write8(void *context, U16 address, U16 value);
#endif
static U16 read8(SrSnesDspCore *dsp, U16 address) {
#ifdef SR_SNES_DSP_TEAK_MODULE
    return sr_snes_dsp_teak_read8(dsp->memory.context, address) & 255u;
#else
    return dsp->memory.read8(dsp->memory.context, address) & 255u;
#endif
}
static void write8(SrSnesDspCore *dsp, U16 address, U16 value) {
#ifdef SR_SNES_DSP_TEAK_MODULE
    sr_snes_dsp_teak_write8(dsp->memory.context, address, value & 255u);
#else
    dsp->memory.write8(dsp->memory.context, address, value & 255u);
#endif
}
static U16 read16(SrSnesDspCore *dsp, U16 address) {
#ifdef SR_SNES_DSP_TEAK_MODULE
    extern U16 sr_snes_dsp_teak_read16(void *, U16);
    return sr_snes_dsp_teak_read16(dsp->memory.context, address);
#else
    U16 low = read8(dsp, address);
    return low | (U16)(read8(dsp, (U16)(address + 1u)) << 8);
#endif
}

static const U16 rates[32] SR_CORE_Y_RO = {
    0, 2048, 1536, 1280, 1024, 768, 640, 512,
    384, 320, 256, 192, 160, 128, 96, 80,
    64, 48, 40, 32, 24, 20, 16, 12,
    10, 8, 6, 5, 4, 3, 2, 1
};

const SrSnesDspU16 sr_snes_dsp_gauss[512] SR_CORE_Y_RO = {
  0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
  0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x002, 0x002, 0x002, 0x002, 0x002,
  0x002, 0x002, 0x003, 0x003, 0x003, 0x003, 0x003, 0x004, 0x004, 0x004, 0x004, 0x004, 0x005, 0x005, 0x005, 0x005,
  0x006, 0x006, 0x006, 0x006, 0x007, 0x007, 0x007, 0x008, 0x008, 0x008, 0x009, 0x009, 0x009, 0x00A, 0x00A, 0x00A,
  0x00B, 0x00B, 0x00B, 0x00C, 0x00C, 0x00D, 0x00D, 0x00E, 0x00E, 0x00F, 0x00F, 0x00F, 0x010, 0x010, 0x011, 0x011,
  0x012, 0x013, 0x013, 0x014, 0x014, 0x015, 0x015, 0x016, 0x017, 0x017, 0x018, 0x018, 0x019, 0x01A, 0x01B, 0x01B,
  0x01C, 0x01D, 0x01D, 0x01E, 0x01F, 0x020, 0x020, 0x021, 0x022, 0x023, 0x024, 0x024, 0x025, 0x026, 0x027, 0x028,
  0x029, 0x02A, 0x02B, 0x02C, 0x02D, 0x02E, 0x02F, 0x030, 0x031, 0x032, 0x033, 0x034, 0x035, 0x036, 0x037, 0x038,
  0x03A, 0x03B, 0x03C, 0x03D, 0x03E, 0x040, 0x041, 0x042, 0x043, 0x045, 0x046, 0x047, 0x049, 0x04A, 0x04C, 0x04D,
  0x04E, 0x050, 0x051, 0x053, 0x054, 0x056, 0x057, 0x059, 0x05A, 0x05C, 0x05E, 0x05F, 0x061, 0x063, 0x064, 0x066,
  0x068, 0x06A, 0x06B, 0x06D, 0x06F, 0x071, 0x073, 0x075, 0x076, 0x078, 0x07A, 0x07C, 0x07E, 0x080, 0x082, 0x084,
  0x086, 0x089, 0x08B, 0x08D, 0x08F, 0x091, 0x093, 0x096, 0x098, 0x09A, 0x09C, 0x09F, 0x0A1, 0x0A3, 0x0A6, 0x0A8,
  0x0AB, 0x0AD, 0x0AF, 0x0B2, 0x0B4, 0x0B7, 0x0BA, 0x0BC, 0x0BF, 0x0C1, 0x0C4, 0x0C7, 0x0C9, 0x0CC, 0x0CF, 0x0D2,
  0x0D4, 0x0D7, 0x0DA, 0x0DD, 0x0E0, 0x0E3, 0x0E6, 0x0E9, 0x0EC, 0x0EF, 0x0F2, 0x0F5, 0x0F8, 0x0FB, 0x0FE, 0x101,
  0x104, 0x107, 0x10B, 0x10E, 0x111, 0x114, 0x118, 0x11B, 0x11E, 0x122, 0x125, 0x129, 0x12C, 0x130, 0x133, 0x137,
  0x13A, 0x13E, 0x141, 0x145, 0x148, 0x14C, 0x150, 0x153, 0x157, 0x15B, 0x15F, 0x162, 0x166, 0x16A, 0x16E, 0x172,
  0x176, 0x17A, 0x17D, 0x181, 0x185, 0x189, 0x18D, 0x191, 0x195, 0x19A, 0x19E, 0x1A2, 0x1A6, 0x1AA, 0x1AE, 0x1B2,
  0x1B7, 0x1BB, 0x1BF, 0x1C3, 0x1C8, 0x1CC, 0x1D0, 0x1D5, 0x1D9, 0x1DD, 0x1E2, 0x1E6, 0x1EB, 0x1EF, 0x1F3, 0x1F8,
  0x1FC, 0x201, 0x205, 0x20A, 0x20F, 0x213, 0x218, 0x21C, 0x221, 0x226, 0x22A, 0x22F, 0x233, 0x238, 0x23D, 0x241,
  0x246, 0x24B, 0x250, 0x254, 0x259, 0x25E, 0x263, 0x267, 0x26C, 0x271, 0x276, 0x27B, 0x280, 0x284, 0x289, 0x28E,
  0x293, 0x298, 0x29D, 0x2A2, 0x2A6, 0x2AB, 0x2B0, 0x2B5, 0x2BA, 0x2BF, 0x2C4, 0x2C9, 0x2CE, 0x2D3, 0x2D8, 0x2DC,
  0x2E1, 0x2E6, 0x2EB, 0x2F0, 0x2F5, 0x2FA, 0x2FF, 0x304, 0x309, 0x30E, 0x313, 0x318, 0x31D, 0x322, 0x326, 0x32B,
  0x330, 0x335, 0x33A, 0x33F, 0x344, 0x349, 0x34E, 0x353, 0x357, 0x35C, 0x361, 0x366, 0x36B, 0x370, 0x374, 0x379,
  0x37E, 0x383, 0x388, 0x38C, 0x391, 0x396, 0x39B, 0x39F, 0x3A4, 0x3A9, 0x3AD, 0x3B2, 0x3B7, 0x3BB, 0x3C0, 0x3C5,
  0x3C9, 0x3CE, 0x3D2, 0x3D7, 0x3DC, 0x3E0, 0x3E5, 0x3E9, 0x3ED, 0x3F2, 0x3F6, 0x3FB, 0x3FF, 0x403, 0x408, 0x40C,
  0x410, 0x415, 0x419, 0x41D, 0x421, 0x425, 0x42A, 0x42E, 0x432, 0x436, 0x43A, 0x43E, 0x442, 0x446, 0x44A, 0x44E,
  0x452, 0x455, 0x459, 0x45D, 0x461, 0x465, 0x468, 0x46C, 0x470, 0x473, 0x477, 0x47A, 0x47E, 0x481, 0x485, 0x488,
  0x48C, 0x48F, 0x492, 0x496, 0x499, 0x49C, 0x49F, 0x4A2, 0x4A6, 0x4A9, 0x4AC, 0x4AF, 0x4B2, 0x4B5, 0x4B7, 0x4BA,
  0x4BD, 0x4C0, 0x4C3, 0x4C5, 0x4C8, 0x4CB, 0x4CD, 0x4D0, 0x4D2, 0x4D5, 0x4D7, 0x4D9, 0x4DC, 0x4DE, 0x4E0, 0x4E3,
  0x4E5, 0x4E7, 0x4E9, 0x4EB, 0x4ED, 0x4EF, 0x4F1, 0x4F3, 0x4F5, 0x4F6, 0x4F8, 0x4FA, 0x4FB, 0x4FD, 0x4FF, 0x500,
  0x502, 0x503, 0x504, 0x506, 0x507, 0x508, 0x50A, 0x50B, 0x50C, 0x50D, 0x50E, 0x50F, 0x510, 0x511, 0x511, 0x512,
  0x513, 0x514, 0x514, 0x515, 0x516, 0x516, 0x517, 0x517, 0x517, 0x518, 0x518, 0x518, 0x518, 0x518, 0x519, 0x519
};

static void reset_voice(SrSnesDspVoice *v) {
    U16 i;
    v->pitch = v->pitchCounter = v->pitchModulation = 0;
    for (i = 0; i < 19; ++i) v->decodeBuffer[i] = 0;
    v->srcn = v->decodeOffset = v->previousFlags = 0;
    v->old = v->older = 0;
    v->useNoise = 0;
    for (i = 0; i < 4; ++i) v->adsrRates[i] = 0;
    v->rateCounter = v->adsrState = v->sustainLevel = v->useGain = 0;
    v->gainMode = v->directGain = v->gainValue = v->gain = 0;
    v->keyOn = v->keyOff = 0;
    v->sampleOut = v->volumeL = v->volumeR = 0;
    v->echoEnable = 0;
}

void sr_snes_dsp_core_init(SrSnesDspCore *dsp, const SrSnesDspMemory *memory) {
    dsp->memory = *memory;
    sr_snes_dsp_core_reset(dsp);
}

void sr_snes_dsp_core_reset(SrSnesDspCore *dsp) {
    U16 i;
    for (i = 0; i < 128; ++i) dsp->ram[i] = 0;
    dsp->ram[0x7c] = 255;
    for (i = 0; i < 8; ++i) reset_voice(&dsp->channel[i]);
    dsp->dirPage = dsp->evenCycle = 0;
    dsp->mute = dsp->reset = 1;
    dsp->masterVolumeL = dsp->masterVolumeR = 0;
    dsp->noiseSample = -0x4000;
    dsp->noiseRate = dsp->noiseCounter = dsp->echoWrites = 0;
    dsp->echoVolumeL = dsp->echoVolumeR = dsp->feedbackVolume = 0;
    dsp->echoBufferAdr = dsp->echoBufferIndex = dsp->firBufferIndex = 0;
    dsp->echoDelay = dsp->echoRemain = 1;
    for (i = 0; i < 8; ++i) {
        dsp->firValues[i] = dsp->firBufferL[i] = dsp->firBufferR[i] = 0;
    }
}

S16 sr_snes_dsp_core_gaussian(const SrSnesDspVoice *v, U16 sample_num, U16 fraction) {
#ifdef __teak__
    extern S16 sr_dsp_gaussian_teak(const S16 *, const U16 *, U16);
    return sr_dsp_gaussian_teak(v->decodeBuffer + sample_num,
                                sr_snes_dsp_gauss, fraction);
#else
    S32 value;
    /* Three products are rounded independently, then WRAPPED, not clamped. */
    value = product_shift((S16)sr_snes_dsp_gauss[255u - fraction],
                        v->decodeBuffer[sample_num], 10);
    value += product_shift((S16)sr_snes_dsp_gauss[511u - fraction],
                         v->decodeBuffer[sample_num + 1u], 10);
    value += product_shift((S16)sr_snes_dsp_gauss[256u + fraction],
                         v->decodeBuffer[sample_num + 2u], 10);
    value = wrap16(value);
    value += product_shift((S16)sr_snes_dsp_gauss[fraction],
                         v->decodeBuffer[sample_num + 3u], 10);
    return (S16)sar(clamp16(value), 1);
#endif
}

#ifdef __teak__
void
#else
static void
#endif
sr_dsp_decode_brr_voice(SrSnesDspCore *dsp, SrSnesDspVoice *v, U16 ch) {
    U16 i, pointer, header, byte = 0;
#ifndef __teak__
    U16 shift, filter;
#endif
    S32 old = v->old, older = v->older, sample;
    v->decodeBuffer[0] = v->decodeBuffer[16];
    v->decodeBuffer[1] = v->decodeBuffer[17];
    v->decodeBuffer[2] = v->decodeBuffer[18];
    if (v->previousFlags == 1u || v->previousFlags == 3u) {
        pointer = (U16)(dsp->dirPage + (v->srcn << 2));
        v->decodeOffset = read16(dsp, (U16)(pointer + 2u));
        if (v->previousFlags == 1u) {
            v->adsrState = 4;
            v->gain = 0;
        }
        dsp->ram[0x7c] |= (U16)(1u << ch);
    }
    header = read8(dsp, v->decodeOffset++);
#ifndef __teak__
    shift = header >> 4;
    filter = (header >> 2) & 3u;
#endif
    v->previousFlags = header & 3u;
    for (i = 0; i < 16; ++i) {
        if (i & 1u) sample = byte & 15u;
        else {
            byte = read8(dsp, v->decodeOffset++);
            sample = byte >> 4;
        }
#ifdef __teak__
        extern S16 sr_dsp_brr_step(U16, U16, S16, S16);
        sample = sr_dsp_brr_step((U16)sample, header, (S16)old, (S16)older);
#else
        if (sample > 7) sample -= 16;
        if (shift <= 12u) {
            /* Left shifting unsigned magnitude avoids signed-left-shift UB. */
            if (sample < 0) sample = -(S32)((U32)(-sample) << shift);
            else sample = (S32)((U32)sample << shift);
            sample = sar(sample, 1);
        } else sample = sample < 0 ? -2048 : 0;
        switch (filter) {
            case 1: sample += old + sar(-old, 4); break;
            case 2:
                sample += old + old + product_shift(3, (S16)-old, 5)
                          - older + sar(older, 4);
                break;
            case 3:
                sample += old + old + product_shift(13, (S16)-old, 6)
                          - older + product_shift(3, (S16)older, 4);
                break;
            default: break;
        }
        sample = wrap15(clamp16(sample));
#endif
        older = old;
        old = sample;
        v->decodeBuffer[i + 3u] = (S16)sample;
    }
    v->older = (S16)older;
    v->old = (S16)old;
}

#ifndef __teak__
static void handle_gain(SrSnesDspVoice *v, U16 live_gain) {
    S32 gain = v->gain;
    if (v->adsrState == 4u) {
        v->gain = gain < 8 ? 0 : (U16)(gain - 8);
        return;
    }
    if (live_gain) {
        switch (v->gainMode) {
            case 0: gain = gain < 32 ? 0 : gain - 32; break;
            case 1: gain -= sar(gain - 1, 8) + 1; break;
            case 2: gain += 32; break;
            case 3: gain += gain < 0x600 ? 32 : 8; break;
            default: break;
        }
    } else if (v->adsrState == 0u) {
        gain += v->adsrRates[0] == 1u ? 1024 : 32;
    } else gain -= sar(gain - 1, 8) + 1;
    if (gain > 0x7ff) gain = 0x7ff;
    v->gain = (U16)gain;
    if (v->adsrState == 0u && gain >= 0x7e0) v->adsrState = 1;
    else if (v->adsrState == 1u && gain < v->sustainLevel) v->adsrState = 2;
}

#endif

static void cycle_envelope(SrSnesDspVoice *v) {
#ifdef __teak__
    typedef char voice_layout[(sizeof(SrSnesDspVoice) == 46u * sizeof(U16) &&
        __builtin_offsetof(SrSnesDspVoice, adsrRates) == 28u * sizeof(U16) &&
        __builtin_offsetof(SrSnesDspVoice, rateCounter) == 32u * sizeof(U16) &&
        __builtin_offsetof(SrSnesDspVoice, gain) == 39u * sizeof(U16)) ? 1 : -1];
    extern void sr_dsp_envelope_teak(SrSnesDspVoice *);
    (void)sizeof(voice_layout);
    sr_dsp_envelope_teak(v);
#else

    U16 releasing = v->adsrState == 4u;
    U16 live_gain = !releasing && v->useGain;
    U16 direct = live_gain && v->directGain;
    U16 rate, stage;
    if (releasing) rate = 0;
    else if (live_gain) rate = direct ? 0 : v->adsrRates[3];
    else {
        stage = v->adsrState > 2u ? 2u : v->adsrState;
        rate = v->adsrRates[stage];
    }
    if (!releasing && !direct && rate) ++v->rateCounter;
    if (releasing || (!direct && rate && v->rateCounter >= rate)) {
        if (!releasing) v->rateCounter = 0;
        handle_gain(v, live_gain);
    }
    if (direct) v->gain = v->gainValue;
#endif
}

#ifdef __teak__
void
#else
static void
#endif
sr_dsp_key_start(SrSnesDspCore *dsp, SrSnesDspVoice *v) {
    U16 i;
    v->keyOn = 0;
    v->previousFlags = 0;
    v->decodeOffset = read16(dsp, (U16)(dsp->dirPage + (v->srcn << 2)));
    for (i = 0; i < 19; ++i) v->decodeBuffer[i] = 0;
    v->gain = v->adsrState = 0;
}

static void cycle_key(SrSnesDspCore *dsp, U16 ch) {
    SrSnesDspVoice *v = &dsp->channel[ch];
    if (dsp->evenCycle) {
        if (v->keyOff) v->adsrState = 4;
        else if (v->keyOn) {
            sr_dsp_key_start(dsp, v);
            /* The oracle retains pitchCounter, rateCounter and BRR history. */
        }
    }
    if (dsp->reset) {
        v->adsrState = 4;
        v->gain = 0;
    }
}

static void cycle_channel(SrSnesDspCore *dsp, U16 ch) {
#ifdef __teak__
    typedef char voice_layout[(sizeof(SrSnesDspVoice) == 46u * sizeof(U16) &&
        __builtin_offsetof(SrSnesDspCore, noiseSample) == 502u * sizeof(U16) &&
        __builtin_offsetof(SrSnesDspVoice, sampleOut) == 42u * sizeof(U16) &&
        __builtin_offsetof(SrSnesDspVoice, keyOn) == 40u * sizeof(U16) &&
        __builtin_offsetof(SrSnesDspVoice, keyOff) == 41u * sizeof(U16)) ? 1 : -1];
    extern void sr_dsp_voice_step(SrSnesDspCore *, SrSnesDspVoice *, U16, U16);
    (void)sizeof(voice_layout);
    sr_dsp_voice_step(dsp, &dsp->channel[ch], ch,
                     dsp->evenCycle | (dsp->reset << 1));
#else

    SrSnesDspVoice *v = &dsp->channel[ch];
    U16 pitch = v->pitch;
    S32 counter, factor;
    S16 sample;
    if (ch && v->pitchModulation) {
        factor = sar(dsp->channel[ch - 1u].sampleOut, 4) + 0x400;
        pitch = (U16)product_shift((S16)pitch, (S16)factor, 10);
        if (pitch > 0x3fffu) pitch = 0x3fff;
    }
    counter = (S32)v->pitchCounter + pitch;
    if (counter > 0xffffL) sr_dsp_decode_brr_voice(dsp, v, ch);
    v->pitchCounter = (U16)counter;
    sample = v->useNoise ? dsp->noiseSample :
        sr_snes_dsp_core_gaussian(v, v->pitchCounter >> 12,
                                  (v->pitchCounter >> 4) & 255u);
    cycle_key(dsp, ch);
    cycle_envelope(v);
    dsp->ram[(ch << 4) | 8u] = v->gain >> 4;
    sample = (S16)product_shift(sample, (S16)v->gain, 11);
    dsp->ram[(ch << 4) | 9u] = (U16)sar(sample, 7) & 255u;
    v->sampleOut = sample;
#endif
}

#ifdef __teak__
void sr_dsp_noise_cycle(SrSnesDspCore *dsp) {
#else
static void handle_noise(SrSnesDspCore *dsp) {
#endif
    U16 bits, bit;
    if (dsp->noiseRate) ++dsp->noiseCounter;
    if (dsp->noiseRate && dsp->noiseCounter >= dsp->noiseRate) {
        bits = (U16)dsp->noiseSample;
        bit = (bits ^ (bits >> 1)) & 1u;
        dsp->noiseSample = wrap15((bits >> 1 & 0x3fffu) | (bit << 14));
        dsp->noiseCounter = 0;
    }
}

static S16 echo_fir(SrSnesDspCore *dsp, const S16 *history) {
#ifdef __teak__
    extern S16 sr_dsp_fir_teak(const S16 *, const S16 *, U16);
    return sr_dsp_fir_teak(history, dsp->firValues, dsp->firBufferIndex);
#else
    U16 i;
    S32 sum = 0;
    for (i = 0; i < 7; ++i) {
        sum += product_shift(history[(dsp->firBufferIndex + i + 1u) & 7u],
                           dsp->firValues[i], 6);
    }
    sum = wrap16(sum);
    sum += wrap16(product_shift(history[dsp->firBufferIndex], dsp->firValues[7], 6));
    return wrap16((U16)clamp16(sum) & 0xfffeu);
#endif
}

#ifdef __teak__
void sr_dsp_echo_cycle(SrSnesDspCore *dsp, S16 stereo[2]) {
#else
static void handle_echo(SrSnesDspCore *dsp, S16 stereo[2]) {
#endif
    U16 i, address = (U16)(dsp->echoBufferAdr + (dsp->echoBufferIndex << 2));
    S16 sum_l, sum_r;
    S32 input_l = 0, input_r = 0;
    U16 out_l, out_r;
    dsp->firBufferL[dsp->firBufferIndex] = (S16)sar(wrap16(read16(dsp, address)), 1);
    dsp->firBufferR[dsp->firBufferIndex] =
        (S16)sar(wrap16(read16(dsp, (U16)(address + 2u))), 1);
    sum_l = echo_fir(dsp, dsp->firBufferL);
    sum_r = echo_fir(dsp, dsp->firBufferR);
    stereo[0] = clamp16((S32)stereo[0] + product_shift(sum_l, dsp->echoVolumeL, 7));
    stereo[1] = clamp16((S32)stereo[1] + product_shift(sum_r, dsp->echoVolumeR, 7));
    for (i = 0; i < 8; ++i) {
        SrSnesDspVoice *v = &dsp->channel[i];
        if (v->echoEnable) {
            input_l = mix_voice((S16)input_l, v->sampleOut, v->volumeL);
            input_r = mix_voice((S16)input_r, v->sampleOut, v->volumeR);
        }
    }
    out_l = (U16)clamp16(input_l + product_shift(sum_l, dsp->feedbackVolume, 7)) & 0xfffeu;
    out_r = (U16)clamp16(input_r + product_shift(sum_r, dsp->feedbackVolume, 7)) & 0xfffeu;
    if (dsp->echoWrites) {
#ifdef SR_SNES_DSP_TEAK_MODULE
        extern void sr_snes_dsp_teak_write16(void *, U16, U16);
        sr_snes_dsp_teak_write16(dsp->memory.context, address, out_l);
        sr_snes_dsp_teak_write16(dsp->memory.context, (U16)(address + 2u), out_r);
#else
        write8(dsp, address, out_l);
        write8(dsp, (U16)(address + 1u), out_l >> 8);
        write8(dsp, (U16)(address + 2u), out_r);
        write8(dsp, (U16)(address + 3u), out_r >> 8);
#endif
    }
    dsp->firBufferIndex = (dsp->firBufferIndex + 1u) & 7u;
    ++dsp->echoBufferIndex;
    if (--dsp->echoRemain == 0u) {
        dsp->echoRemain = dsp->echoDelay;
        dsp->echoBufferIndex = 0;
    }
}

void sr_snes_dsp_core_cycle(SrSnesDspCore *dsp, S16 stereo[2]) {
#ifdef __teak__
    typedef char cycle_layout[(__builtin_offsetof(SrSnesDspCore, dirPage) == 496u * sizeof(U16) &&
        __builtin_offsetof(SrSnesDspCore, masterVolumeL) == 500u * sizeof(U16)) ? 1 : -1];
    extern void sr_dsp_cycle_teak(SrSnesDspCore *, S16 *);
    (void)sizeof(cycle_layout);
    sr_dsp_cycle_teak(dsp, stereo);
#else

    U16 i;
    S32 left = 0, right = 0;
    for (i = 0; i < 8; ++i) {
        SrSnesDspVoice *v = &dsp->channel[i];
        cycle_channel(dsp, i);
        left = mix_voice((S16)left, v->sampleOut, v->volumeL);
        right = mix_voice((S16)right, v->sampleOut, v->volumeR);
    }
    stereo[0] = clamp16(product_shift((S16)left, dsp->masterVolumeL, 7));
    stereo[1] = clamp16(product_shift((S16)right, dsp->masterVolumeR, 7));
    handle_echo(dsp, stereo);
    if (dsp->mute) stereo[0] = stereo[1] = 0;
    handle_noise(dsp);
    dsp->evenCycle = !dsp->evenCycle;
#endif
}

U16 sr_snes_dsp_core_read(const SrSnesDspCore *dsp, U16 address) {
    return dsp->ram[address & 127u];
}

static void write_voice(SrSnesDspVoice *v, U16 reg, U16 value) {
    switch (reg) {
        case 0: v->volumeL = signed8(value); break;
        case 1: v->volumeR = signed8(value); break;
        case 2: v->pitch = (v->pitch & 0x3f00u) | value; break;
        case 3: v->pitch = ((v->pitch & 255u) | (value << 8)) & 0x3fffu; break;
        case 4: v->srcn = value; break;
        case 5:
            v->adsrRates[0] = rates[((value & 15u) << 1) + 1u];
            v->adsrRates[1] = rates[(((value >> 4) & 7u) << 1) + 16u];
            v->useGain = (value & 128u) == 0;
            break;
        case 6:
            v->adsrRates[2] = rates[value & 31u];
            v->sustainLevel = (((value >> 5) & 7u) + 1u) << 8;
            break;
        case 7:
            v->directGain = (value & 128u) == 0;
            if (value & 128u) {
                v->gainMode = (value >> 5) & 3u;
                v->adsrRates[3] = rates[value & 31u];
            } else v->gainValue = (value & 127u) << 4;
            break;
        default: break;
    }
}

void sr_snes_dsp_core_write(SrSnesDspCore *dsp, U16 address, U16 value) {
    U16 i, reg;
    address &= 127u;
    value &= 255u;
    reg = address & 15u;
    if (reg < 8u) write_voice(&dsp->channel[address >> 4], reg, value);
    else if (reg == 15u) dsp->firValues[address >> 4] = signed8(value);
    else switch (address) {
        case 0x0c: dsp->masterVolumeL = signed8(value); break;
        case 0x1c: dsp->masterVolumeR = signed8(value); break;
        case 0x2c: dsp->echoVolumeL = signed8(value); break;
        case 0x3c: dsp->echoVolumeR = signed8(value); break;
        case 0x4c:
            for (i = 0; i < 8; ++i) dsp->channel[i].keyOn = !!(value & (1u << i));
            break;
        case 0x5c:
            for (i = 0; i < 8; ++i) dsp->channel[i].keyOff = !!(value & (1u << i));
            break;
        case 0x6c:
            dsp->reset = !!(value & 128u);
            dsp->mute = !!(value & 64u);
            dsp->echoWrites = (value & 32u) == 0;
            dsp->noiseRate = rates[value & 31u];
            break;
        case 0x7c: value = 0; break;
        case 0x0d: dsp->feedbackVolume = signed8(value); break;
        case 0x2d:
            for (i = 0; i < 8; ++i) dsp->channel[i].pitchModulation = !!(value & (1u << i));
            break;
        case 0x3d:
            for (i = 0; i < 8; ++i) dsp->channel[i].useNoise = !!(value & (1u << i));
            break;
        case 0x4d:
            for (i = 0; i < 8; ++i) dsp->channel[i].echoEnable = !!(value & (1u << i));
            break;
        case 0x5d: dsp->dirPage = value << 8; break;
        case 0x6d: dsp->echoBufferAdr = value << 8; break;
        case 0x7d:
            dsp->echoDelay = (value & 15u) << 9;
            if (!dsp->echoDelay) dsp->echoDelay = 1;
            break;
        default: break;
    }
    dsp->ram[address] = value;
}

/* Canonical serialization uses explicit fields; no struct-layout copies. */

static void save_voice(const SrSnesDspVoice *v, U16 *words) {
    U16 i, n = 0;
    words[n++] = (U16)v->pitch;
    words[n++] = (U16)v->pitchCounter;
    words[n++] = (U16)v->pitchModulation;
    for (i = 0; i < 19; ++i) words[n++] = (U16)v->decodeBuffer[i];
    words[n++] = (U16)v->srcn;
    words[n++] = (U16)v->decodeOffset;
    words[n++] = (U16)v->previousFlags;
    words[n++] = (U16)v->old;
    words[n++] = (U16)v->older;
    words[n++] = (U16)v->useNoise;
    for (i = 0; i < 4; ++i) words[n++] = (U16)v->adsrRates[i];
    words[n++] = (U16)v->rateCounter;
    words[n++] = (U16)v->adsrState;
    words[n++] = (U16)v->sustainLevel;
    words[n++] = (U16)v->useGain;
    words[n++] = (U16)v->gainMode;
    words[n++] = (U16)v->directGain;
    words[n++] = (U16)v->gainValue;
    words[n++] = (U16)v->gain;
    words[n++] = (U16)v->keyOn;
    words[n++] = (U16)v->keyOff;
    words[n++] = (U16)v->sampleOut;
    words[n++] = (U16)v->volumeL;
    words[n++] = (U16)v->volumeR;
    words[n++] = (U16)v->echoEnable;
}

static void load_voice(SrSnesDspVoice *v, const U16 *words) {
    U16 i, n = 0;
    v->pitch = words[n++];
    v->pitchCounter = words[n++];
    v->pitchModulation = words[n++];
    for (i = 0; i < 19; ++i) v->decodeBuffer[i] = wrap16(words[n++]);
    v->srcn = words[n++];
    v->decodeOffset = words[n++];
    v->previousFlags = words[n++];
    v->old = wrap16(words[n++]);
    v->older = wrap16(words[n++]);
    v->useNoise = words[n++];
    for (i = 0; i < 4; ++i) v->adsrRates[i] = words[n++];
    v->rateCounter = words[n++];
    v->adsrState = words[n++];
    v->sustainLevel = words[n++];
    v->useGain = words[n++];
    v->gainMode = words[n++];
    v->directGain = words[n++];
    v->gainValue = words[n++];
    v->gain = words[n++];
    v->keyOn = words[n++];
    v->keyOff = words[n++];
    v->sampleOut = wrap16(words[n++]);
    v->volumeL = wrap16(words[n++]);
    v->volumeR = wrap16(words[n++]);
    v->echoEnable = words[n++];
}

static void save_globals(const SrSnesDspCore *v, U16 *words) {
    U16 i, n = 0;
    words[n++] = (U16)v->dirPage;
    words[n++] = (U16)v->evenCycle;
    words[n++] = (U16)v->mute;
    words[n++] = (U16)v->reset;
    words[n++] = (U16)v->masterVolumeL;
    words[n++] = (U16)v->masterVolumeR;
    words[n++] = (U16)v->noiseSample;
    words[n++] = (U16)v->noiseRate;
    words[n++] = (U16)v->noiseCounter;
    words[n++] = (U16)v->echoWrites;
    words[n++] = (U16)v->echoVolumeL;
    words[n++] = (U16)v->echoVolumeR;
    words[n++] = (U16)v->feedbackVolume;
    words[n++] = (U16)v->echoBufferAdr;
    words[n++] = (U16)v->echoDelay;
    words[n++] = (U16)v->echoRemain;
    words[n++] = (U16)v->echoBufferIndex;
    words[n++] = (U16)v->firBufferIndex;
    for (i = 0; i < 8; ++i) words[n++] = (U16)v->firValues[i];
    for (i = 0; i < 8; ++i) words[n++] = (U16)v->firBufferL[i];
    for (i = 0; i < 8; ++i) words[n++] = (U16)v->firBufferR[i];
}

static void load_globals(SrSnesDspCore *v, const U16 *words) {
    U16 i, n = 0;
    v->dirPage = words[n++];
    v->evenCycle = words[n++];
    v->mute = words[n++];
    v->reset = words[n++];
    v->masterVolumeL = wrap16(words[n++]);
    v->masterVolumeR = wrap16(words[n++]);
    v->noiseSample = wrap16(words[n++]);
    v->noiseRate = words[n++];
    v->noiseCounter = words[n++];
    v->echoWrites = words[n++];
    v->echoVolumeL = wrap16(words[n++]);
    v->echoVolumeR = wrap16(words[n++]);
    v->feedbackVolume = wrap16(words[n++]);
    v->echoBufferAdr = words[n++];
    v->echoDelay = words[n++];
    v->echoRemain = words[n++];
    v->echoBufferIndex = words[n++];
    v->firBufferIndex = words[n++];
    for (i = 0; i < 8; ++i) v->firValues[i] = wrap16(words[n++]);
    for (i = 0; i < 8; ++i) v->firBufferL[i] = wrap16(words[n++]);
    for (i = 0; i < 8; ++i) v->firBufferR[i] = wrap16(words[n++]);
}

int sr_snes_dsp_core_save(const SrSnesDspCore *core, U16 *words, U16 count) {
    U16 i;
    if (!core || !words || count != SR_SNES_DSP_CORE_STATE_WORDS) return 0;
    words[0] = 0x5344;
    words[1] = 0x5350;
    words[2] = SR_SNES_DSP_CORE_STATE_VERSION;
    words[3] = SR_SNES_DSP_CORE_STATE_WORDS;
    words[4] = words[5] = 0;
    for (i = 0; i < 128; ++i) words[6u + i] = core->ram[i];
    for (i = 0; i < 8; ++i) save_voice(&core->channel[i], words + 134u + i * 46u);
    save_globals(core, words + 502);
    return 1;
}

static int is_signed8(U16 word) { return word <= 127u || word >= 0xff80u; }
static int is_signed15(U16 word) { return word <= 0x3fffu || word >= 0xc000u; }

static int validate_voice(const U16 *w) {
    U16 i;
    if (w[0] > 0x3fffu || w[2] > 1u || w[22] > 255u || w[24] > 3u ||
        !is_signed15(w[25]) || !is_signed15(w[26]) || w[27] > 1u ||
        w[32] > 2048u || w[33] > 4u || w[34] > 2048u || w[35] > 1u ||
        w[36] > 3u || w[37] > 1u || w[38] > 2032u || w[39] > 2047u ||
        w[40] > 1u || w[41] > 1u || !is_signed8(w[43]) ||
        !is_signed8(w[44]) || w[45] > 1u) return 0;
    for (i = 0; i < 4; ++i) if (w[28u + i] > 2048u) return 0;
    return 1;
}

static int validate_globals(const U16 *w) {
    U16 i;
    if ((w[0] & 255u) || w[1] > 1u || w[2] > 1u || w[3] > 1u ||
        !is_signed8(w[4]) || !is_signed8(w[5]) || !is_signed15(w[6]) ||
        w[7] > 2048u || w[8] > 2048u || w[9] > 1u ||
        !is_signed8(w[10]) || !is_signed8(w[11]) || !is_signed8(w[12]) ||
        (w[13] & 255u) || !w[14] || w[14] > 7680u ||
        (w[14] != 1u && (w[14] & 511u)) || !w[15] || w[15] > 7680u ||
        w[16] >= 7680u || w[17] > 7u) return 0;
    for (i = 0; i < 8; ++i) if (!is_signed8(w[18u + i])) return 0;
    return 1;
}

int sr_snes_dsp_core_load(SrSnesDspCore *core, const U16 *words, U16 count) {
    U16 i;
    if (!core || !words || count != SR_SNES_DSP_CORE_STATE_WORDS ||
        words[0] != 0x5344u || words[1] != 0x5350u ||
        words[2] != SR_SNES_DSP_CORE_STATE_VERSION ||
        words[3] != SR_SNES_DSP_CORE_STATE_WORDS || words[4] || words[5]) return 0;
    for (i = 0; i < 128; ++i) if (words[6u + i] > 255u) return 0;
    for (i = 0; i < 8; ++i) if (!validate_voice(words + 134u + i * 46u)) return 0;
    if (!validate_globals(words + 502)) return 0;
    /* Every rejection happens above. Memory binding survives a valid load. */
    for (i = 0; i < 128; ++i) core->ram[i] = words[6u + i];
    for (i = 0; i < 8; ++i) load_voice(&core->channel[i], words + 134u + i * 46u);
    load_globals(core, words + 502);
    return 1;
}
