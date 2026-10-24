/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef SNESRECOMP_PLATFORM_SNES_DSP_STREAM_H
#define SNESRECOMP_PLATFORM_SNES_DSP_STREAM_H

/* Stream ABI 3: ordered S-DSP commands in Teak data RAM, synthesis and
 * output stay on the DSP. ARAM stays in FCRAM behind a DSP line cache. All addresses are Teak data WORD addresses in
 * bank 0, reached by ARM through the uncached DSP RAM mapping. Every word
 * has exactly one writer. 32-bit values are lo/hi words; readers retry
 * while the high word changes. */
#define SR_DSPS_ABI 3u
#define SR_DSPS_MAGIC_ARM 0x5332u
#define SR_DSPS_MAGIC_DSP 0x5344u

#define SR_DSPS_CTL 0x8410u
#define SR_DSPS_CTL_WORDS 64u
#define SR_DSPS_REPLY 0x8450u
#define SR_DSPS_REPLY_WORDS 944u
#define SR_DSPS_RING 0xB800u
#define SR_DSPS_RING_WORDS 2048u

/* ARM-owned control words */
#define SR_DSPS_A_MAGIC 0u
#define SR_DSPS_A_WRITE 1u
#define SR_DSPS_A_GAIN 2u
#define SR_DSPS_A_TARGET 3u
#define SR_DSPS_A_FLAGS 4u
/* DSP-owned control words */
#define SR_DSPS_D_MAGIC 32u
#define SR_DSPS_D_READ 33u
#define SR_DSPS_D_FENCE 34u
#define SR_DSPS_D_STATUS 35u
#define SR_DSPS_D_SYNTH 36u
#define SR_DSPS_D_PLAYED 37u
#define SR_DSPS_D_FILL 39u
#define SR_DSPS_D_UNDERRUNS 40u
#define SR_DSPS_D_STALLS 41u
#define SR_DSPS_D_BUSY 42u
#define SR_DSPS_D_MAX_CHUNK 44u
#define SR_DSPS_D_FRAMES 45u
#define SR_DSPS_D_PENDING 47u

#define SR_DSPS_FLAG_HOLD 1u

/* Commands. A command is published only when all its words are written. */
#define SR_DSPS_REG(reg, value) ((unsigned)(0x8000u | ((reg) & 127u) << 8 | ((value) & 255u)))
#define SR_DSPS_ADVANCE 0x0000u /* | samples 1..4095 */
#define SR_DSPS_ARAM 0x1000u    /* | bytes 1..2048, address, payload words */
#define SR_DSPS_FENCE 0x2000u   /* | id 0..4095, kind, arg0, arg1, checksum */
#define SR_DSPS_OP_MASK 0xf000u
#define SR_DSPS_ARG_MASK 0x0fffu
#define SR_DSPS_MAX_ARAM 2048u
#define SR_DSPS_FENCE_WORDS 5u

/* Fence kinds. The checksum is the 16-bit sum of every ring word since the
 * previous fence, excluding this fence's own five words. */
#define SR_DSPS_FENCE_NONE 0u
#define SR_DSPS_FENCE_REGS 1u
#define SR_DSPS_FENCE_ARAM 2u   /* arg0 address, arg1 bytes <= 1888 */
#define SR_DSPS_FENCE_SAVE 3u   /* state + ARAM to the FCRAM arena */
#define SR_DSPS_FENCE_LOAD 4u   /* state + ARAM from the FCRAM arena */

#define SR_DSPS_OK 0u
#define SR_DSPS_ERR_COMMAND 1u
#define SR_DSPS_ERR_RANGE 2u
#define SR_DSPS_ERR_CHECKSUM 3u
#define SR_DSPS_ERR_STATE 4u
#define SR_DSPS_ERR_DMA 5u

/* FCRAM arena: state words, staging ARAM, live ARAM. LOAD copies staging
 * to live, SAVE copies live to staging; only the DSP touches live ARAM. */
#define SR_DSPS_ARENA_STATE 0u
#define SR_DSPS_ARENA_ARAM 1088u
#define SR_DSPS_ARENA_LIVE (1088u + 65536u)
#define SR_DSPS_ARENA_BYTES (1088u + 2u * 65536u)

/* Mailbox commands (0x80..0xff module range). */
#define SR_DSPS_CMD_DISCOVER 0x80u
#define SR_DSPS_CMD_ATTACH 0x83u
#define SR_DSPS_CMD_DETACH 0x82u
#define SR_DSPS_CAP_STREAM 0x10u
#define SR_DSPS_MAGIC 0x53525332UL /* SRS2 */

/* Nominal SNES output rate and DSP output rate, as used for resampling. */
#define SR_DSPS_NATIVE_HZ 32040u
#define SR_DSPS_PCM_FRAMES 2048u
#define SR_DSPS_DEFAULT_TARGET 1100u

#endif
