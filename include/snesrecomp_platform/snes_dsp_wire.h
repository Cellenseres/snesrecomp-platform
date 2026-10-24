/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef SNESRECOMP_PLATFORM_SNES_DSP_WIRE_H
#define SNESRECOMP_PLATFORM_SNES_DSP_WIRE_H

#include "snesrecomp_platform/snes_dsp_core.h"

/* All offsets and lengths below are OCTETS, including on word-addressed Teak.
 * Buffers are packed little-endian 16-bit words, never C wire structures.
 * On a big-endian host use the byte encoder, not a raw word-array DMA copy.
 */
#define SR_DSP_ABI 1u
#define SR_DSP_MAGIC 0x53445053UL /* bytes SPDS */
#define SR_DSP_REPLY_MAGIC 0x52504453UL /* bytes SDPR */
#define SR_DSP_CONTROL_MAGIC 0x43504453UL /* bytes SDPC */
#define SR_DSP_SLOT_COUNT 4u
#define SR_DSP_SLOT_BYTES 8192u
#define SR_DSP_SLOT_WORDS 4096u
#define SR_DSP_HEADER_BYTES 64u
#define SR_DSP_EVENT_BYTES 16u
#define SR_DSP_MAX_EVENTS 128u
#define SR_DSP_MAX_PAYLOAD 2048u
#define SR_DSP_MAX_SAMPLES 128u
#define SR_DSP_MAX_ECHO 512u
#define SR_DSP_ECHO_BYTES 8u /* address:u16,value:u16,sample_time:u32 */
#define SR_DSP_REQUEST_EVENTS 64u
#define SR_DSP_REQUEST_PAYLOAD 2112u
#define SR_DSP_REPLY_REGS 64u
#define SR_DSP_REPLY_PCM 192u
#define SR_DSP_REPLY_ECHO 1216u
#define SR_DSP_REPLY_STATE 5312u
#define SR_DSP_CONTROL_BYTES 256UL
#define SR_DSP_REQUEST_BASE 256UL
#define SR_DSP_REPLY_BASE 33024UL
#define SR_DSP_ARAM_BASE 65792UL
#define SR_DSP_BOOT_STATE_BASE 131328UL
#define SR_DSP_ARENA_BYTES 132480UL
#define SR_DSP_FCRAM_BEGIN 0x20000000UL
#define SR_DSP_FCRAM_END 0x28000000UL

/* Control: each side owns entire separate 32-byte cache lines. A counter is
 * a 32-bit monotonic modulo-2^32 ticket; outstanding distance MUST be <=4.
 * ARM request producer at 32; DSP reply producer at 64; ARM reply consumer at
 * 96. DSP request consumption equals reply production. Reset uses new arena
 * contents only after acknowledged quiescence; epoch zero is forbidden.
 */
#define SR_DSP_CTL_EPOCH 8u
#define SR_DSP_CTL_PRODUCER 32u
#define SR_DSP_CTL_COMPLETED 64u
#define SR_DSP_CTL_STATUS 68u
#define SR_DSP_CTL_CONSUMER 96u
#define SR_DSP_CTL_BOOT_TIME 12u
#define SR_DSP_CTL_BOOT_NEXT 16u
#define SR_DSP_CTL_BOOT_STATE_WORDS 20u
#define SR_DSP_CTL_FIRST_SEQUENCE 24u

/* Request header offsets. Sequence is the ring ticket (not modulo slot).
 * Each event advances exclusively to its timestamp, then performs its action.
 * Equal-time actions execute in array order. ADVANCE(T) generates samples at
 * scheduled boundaries strictly less than T: a write at T precedes sample T.
 */
#define SR_DSP_REQ_EPOCH 12u
#define SR_DSP_REQ_SEQUENCE 16u
#define SR_DSP_REQ_START_TIME 20u
#define SR_DSP_REQ_EVENTS 24u
#define SR_DSP_REQ_PAYLOAD_BYTES 26u
#define SR_DSP_REQ_SAMPLE_LIMIT 28u
#define SR_DSP_REQ_FLAGS 30u
#define SR_DSP_REQ_CRC 60u
#define SR_DSP_FLAG_SNAPSHOT 1u
#define SR_DSP_EVENT_WRITE_REG 1u
#define SR_DSP_EVENT_WRITE_ARAM 2u
#define SR_DSP_EVENT_ADVANCE 3u
/* Event: type:u16, reserved:u16, time:u32, a:u32, b:u32.
 * REG a=address 0..127,b=value0..255. ARAM a=address0..65535,
 * b=(count<<16)|payload_offset; count>0, no crossing65536 (split it).
 * ADVANCE a=b=0. All times forward modulo32 with distance<2^31.
 */

#define SR_DSP_RES_EPOCH 8u
#define SR_DSP_RES_SEQUENCE 12u
#define SR_DSP_RES_STATUS 16u
#define SR_DSP_RES_TIME 20u
#define SR_DSP_RES_SAMPLES 24u
#define SR_DSP_RES_ECHO_COUNT 26u
#define SR_DSP_RES_STATE_WORDS 28u
#define SR_DSP_RES_NEXT_SAMPLE 32u
#define SR_DSP_RES_BUSY_TICKS 36u
#define SR_DSP_RES_DMA_BYTES 40u
#define SR_DSP_RES_CRC 60u

#define SR_DSP_OK 0u
#define SR_DSP_BUSY 1u
#define SR_DSP_ERR_ABI 2u
#define SR_DSP_ERR_SIZE 3u
#define SR_DSP_ERR_EPOCH 4u
#define SR_DSP_ERR_SEQUENCE 5u
#define SR_DSP_ERR_TIME 6u
#define SR_DSP_ERR_EVENT 7u
#define SR_DSP_ERR_CAPACITY 8u
#define SR_DSP_ERR_STATE 9u
#define SR_DSP_ERR_ADDRESS 10u
#define SR_DSP_ERR_TRANSPORT 11u
#define SR_DSP_ERR_TIMEOUT 12u
#define SR_DSP_ERR_CANCELLED 13u
#define SR_DSP_ERR_UNSUPPORTED 14u

#define SR_DSP_CMD_DISCOVER 0x80u
#define SR_DSP_CMD_ATTACH 0x81u
#define SR_DSP_CMD_DETACH 0x82u
#define SR_DSP_CAP_ORDERED_EVENTS 1UL
#define SR_DSP_CAP_ECHO_DELTAS 2UL
#define SR_DSP_CAP_SNAPSHOT 4UL
#define SR_DSP_CAP_PCM_RETURN 8UL
#define SR_DSP_CAPABILITIES 15UL

#ifdef __cplusplus
extern "C" {
#endif
/* Modulo-2^32 arithmetic. The Teak implementation normalizes its 40-bit
 * accumulator before a 32-bit return/spill, which otherwise saturates. */
SrSnesDspU32 sr_dsp_u32_join(SrSnesDspU16 low, SrSnesDspU16 high);
SrSnesDspU32 sr_dsp_u32_add(SrSnesDspU32 a, SrSnesDspU32 b);
SrSnesDspU32 sr_dsp_u32_sub(SrSnesDspU32 a, SrSnesDspU32 b);
SrSnesDspU16 sr_dsp_wire_u16(const SrSnesDspU16 *p, SrSnesDspU16 octet);
SrSnesDspU32 sr_dsp_wire_u32(const SrSnesDspU16 *p, SrSnesDspU16 octet);
SrSnesDspU16 sr_dsp_wire_u8(const SrSnesDspU16 *p, SrSnesDspU16 octet);
void sr_dsp_wire_put16(SrSnesDspU16 *p, SrSnesDspU16 octet, SrSnesDspU16 value);
void sr_dsp_wire_put32(SrSnesDspU16 *p, SrSnesDspU16 octet, SrSnesDspU32 value);
void sr_dsp_wire_put8(SrSnesDspU16 *p, SrSnesDspU16 octet, SrSnesDspU16 value);
/* CRC-32/ISO-HDLC; checksum octets60..63 are treated as zero.
 * Request covers header64, then events[count*16], then payload[used bytes].
 * Reply covers header64, regs128, PCM[count*4], echo[count*8], state[words*2].
 * Concatenate those regions in that order; unused gaps/padding are excluded.
 * Counts are bounds checked before hashing; invalid sizes return zero.
 * The *_CHECK_BYTES arguments are MAXIMUM buffer extents selecting the
 * request or reply format, not the number of active bytes being hashed.
 * Integrity detects damaged transport, not an authentication mechanism.
 */
#define SR_DSP_REQUEST_CHECK_BYTES 4160u
#define SR_DSP_REPLY_CHECK_BYTES (SR_DSP_REPLY_STATE + SR_SNES_DSP_CORE_STATE_WORDS * 2u)
SrSnesDspU32 sr_dsp_wire_crc(const SrSnesDspU16 *p, SrSnesDspU16 octets);
/* Validated active spans, rounded to 32-octet DMA/cache lines. Return zero
 * for invalid counts; call only after the corresponding header is present.
 * Request head starts0,payload2112; reply head0,echo1216,state5312.
 */
SrSnesDspU16 sr_dsp_wire_request_head(const SrSnesDspU16 *p);
SrSnesDspU16 sr_dsp_wire_request_data(const SrSnesDspU16 *p);
SrSnesDspU16 sr_dsp_wire_reply_head(const SrSnesDspU16 *p);
SrSnesDspU16 sr_dsp_wire_reply_echo(const SrSnesDspU16 *p);
SrSnesDspU16 sr_dsp_wire_reply_state(const SrSnesDspU16 *p);
int sr_dsp_wire_range(SrSnesDspU32 address, SrSnesDspU32 octets);
#ifdef __cplusplus
}
#endif
#endif
