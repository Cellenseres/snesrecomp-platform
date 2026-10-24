/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "snesrecomp_platform/snes_dsp_wire.h"
#ifndef SR_SNES_DSP_TEAK_MODULE
SrSnesDspU32 sr_dsp_u32_join(SrSnesDspU16 low, SrSnesDspU16 high) {
    return (SrSnesDspU32)low | ((SrSnesDspU32)high << 16);
}
SrSnesDspU32 sr_dsp_u32_add(SrSnesDspU32 a, SrSnesDspU32 b) { return a + b; }
SrSnesDspU32 sr_dsp_u32_sub(SrSnesDspU32 a, SrSnesDspU32 b) { return a - b; }
#endif
SrSnesDspU16 sr_dsp_wire_u16(const SrSnesDspU16 *p, SrSnesDspU16 o) {
    return p[o >> 1];
}
SrSnesDspU32 sr_dsp_wire_u32(const SrSnesDspU16 *p, SrSnesDspU16 o) {
    return sr_dsp_u32_join(p[o >> 1], p[(o >> 1) + 1u]);
}
SrSnesDspU16 sr_dsp_wire_u8(const SrSnesDspU16 *p, SrSnesDspU16 o) {
    return (SrSnesDspU16)((p[o >> 1] >> ((o & 1u) * 8u)) & 255u);
}
void sr_dsp_wire_put16(SrSnesDspU16 *p, SrSnesDspU16 o, SrSnesDspU16 v) {
    p[o >> 1] = v;
}
void sr_dsp_wire_put32(SrSnesDspU16 *p, SrSnesDspU16 o, SrSnesDspU32 v) {
    p[o >> 1] = (SrSnesDspU16)v;
    p[(o >> 1) + 1u] = (SrSnesDspU16)(v >> 16);
}
void sr_dsp_wire_put8(SrSnesDspU16 *p, SrSnesDspU16 o, SrSnesDspU16 v) {
    SrSnesDspU16 n = o >> 1;
    if (o & 1u) p[n] = (SrSnesDspU16)((p[n] & 255u) | ((v & 255u) << 8));
    else p[n] = (SrSnesDspU16)((p[n] & 0xff00u) | (v & 255u));
}
#ifdef SR_SNES_DSP_TEAK_MODULE
/* The pinned backend spills the C CRC loop heavily. A leaf routine keeps
 * the CRC in registers with one byte-table update per octet. The table pointer is passed explicitly because the
 * pinned standalone assembler cannot relocate a symbolic data-address MOV. */
static const SrSnesDspU32 teak_byte[256] __attribute__((section(".yrodata"))) = {
    0x00000000UL, 0x77073096UL, 0xee0e612cUL, 0x990951baUL,
    0x076dc419UL, 0x706af48fUL, 0xe963a535UL, 0x9e6495a3UL,
    0x0edb8832UL, 0x79dcb8a4UL, 0xe0d5e91eUL, 0x97d2d988UL,
    0x09b64c2bUL, 0x7eb17cbdUL, 0xe7b82d07UL, 0x90bf1d91UL,
    0x1db71064UL, 0x6ab020f2UL, 0xf3b97148UL, 0x84be41deUL,
    0x1adad47dUL, 0x6ddde4ebUL, 0xf4d4b551UL, 0x83d385c7UL,
    0x136c9856UL, 0x646ba8c0UL, 0xfd62f97aUL, 0x8a65c9ecUL,
    0x14015c4fUL, 0x63066cd9UL, 0xfa0f3d63UL, 0x8d080df5UL,
    0x3b6e20c8UL, 0x4c69105eUL, 0xd56041e4UL, 0xa2677172UL,
    0x3c03e4d1UL, 0x4b04d447UL, 0xd20d85fdUL, 0xa50ab56bUL,
    0x35b5a8faUL, 0x42b2986cUL, 0xdbbbc9d6UL, 0xacbcf940UL,
    0x32d86ce3UL, 0x45df5c75UL, 0xdcd60dcfUL, 0xabd13d59UL,
    0x26d930acUL, 0x51de003aUL, 0xc8d75180UL, 0xbfd06116UL,
    0x21b4f4b5UL, 0x56b3c423UL, 0xcfba9599UL, 0xb8bda50fUL,
    0x2802b89eUL, 0x5f058808UL, 0xc60cd9b2UL, 0xb10be924UL,
    0x2f6f7c87UL, 0x58684c11UL, 0xc1611dabUL, 0xb6662d3dUL,
    0x76dc4190UL, 0x01db7106UL, 0x98d220bcUL, 0xefd5102aUL,
    0x71b18589UL, 0x06b6b51fUL, 0x9fbfe4a5UL, 0xe8b8d433UL,
    0x7807c9a2UL, 0x0f00f934UL, 0x9609a88eUL, 0xe10e9818UL,
    0x7f6a0dbbUL, 0x086d3d2dUL, 0x91646c97UL, 0xe6635c01UL,
    0x6b6b51f4UL, 0x1c6c6162UL, 0x856530d8UL, 0xf262004eUL,
    0x6c0695edUL, 0x1b01a57bUL, 0x8208f4c1UL, 0xf50fc457UL,
    0x65b0d9c6UL, 0x12b7e950UL, 0x8bbeb8eaUL, 0xfcb9887cUL,
    0x62dd1ddfUL, 0x15da2d49UL, 0x8cd37cf3UL, 0xfbd44c65UL,
    0x4db26158UL, 0x3ab551ceUL, 0xa3bc0074UL, 0xd4bb30e2UL,
    0x4adfa541UL, 0x3dd895d7UL, 0xa4d1c46dUL, 0xd3d6f4fbUL,
    0x4369e96aUL, 0x346ed9fcUL, 0xad678846UL, 0xda60b8d0UL,
    0x44042d73UL, 0x33031de5UL, 0xaa0a4c5fUL, 0xdd0d7cc9UL,
    0x5005713cUL, 0x270241aaUL, 0xbe0b1010UL, 0xc90c2086UL,
    0x5768b525UL, 0x206f85b3UL, 0xb966d409UL, 0xce61e49fUL,
    0x5edef90eUL, 0x29d9c998UL, 0xb0d09822UL, 0xc7d7a8b4UL,
    0x59b33d17UL, 0x2eb40d81UL, 0xb7bd5c3bUL, 0xc0ba6cadUL,
    0xedb88320UL, 0x9abfb3b6UL, 0x03b6e20cUL, 0x74b1d29aUL,
    0xead54739UL, 0x9dd277afUL, 0x04db2615UL, 0x73dc1683UL,
    0xe3630b12UL, 0x94643b84UL, 0x0d6d6a3eUL, 0x7a6a5aa8UL,
    0xe40ecf0bUL, 0x9309ff9dUL, 0x0a00ae27UL, 0x7d079eb1UL,
    0xf00f9344UL, 0x8708a3d2UL, 0x1e01f268UL, 0x6906c2feUL,
    0xf762575dUL, 0x806567cbUL, 0x196c3671UL, 0x6e6b06e7UL,
    0xfed41b76UL, 0x89d32be0UL, 0x10da7a5aUL, 0x67dd4accUL,
    0xf9b9df6fUL, 0x8ebeeff9UL, 0x17b7be43UL, 0x60b08ed5UL,
    0xd6d6a3e8UL, 0xa1d1937eUL, 0x38d8c2c4UL, 0x4fdff252UL,
    0xd1bb67f1UL, 0xa6bc5767UL, 0x3fb506ddUL, 0x48b2364bUL,
    0xd80d2bdaUL, 0xaf0a1b4cUL, 0x36034af6UL, 0x41047a60UL,
    0xdf60efc3UL, 0xa867df55UL, 0x316e8eefUL, 0x4669be79UL,
    0xcb61b38cUL, 0xbc66831aUL, 0x256fd2a0UL, 0x5268e236UL,
    0xcc0c7795UL, 0xbb0b4703UL, 0x220216b9UL, 0x5505262fUL,
    0xc5ba3bbeUL, 0xb2bd0b28UL, 0x2bb45a92UL, 0x5cb36a04UL,
    0xc2d7ffa7UL, 0xb5d0cf31UL, 0x2cd99e8bUL, 0x5bdeae1dUL,
    0x9b64c2b0UL, 0xec63f226UL, 0x756aa39cUL, 0x026d930aUL,
    0x9c0906a9UL, 0xeb0e363fUL, 0x72076785UL, 0x05005713UL,
    0x95bf4a82UL, 0xe2b87a14UL, 0x7bb12baeUL, 0x0cb61b38UL,
    0x92d28e9bUL, 0xe5d5be0dUL, 0x7cdcefb7UL, 0x0bdbdf21UL,
    0x86d3d2d4UL, 0xf1d4e242UL, 0x68ddb3f8UL, 0x1fda836eUL,
    0x81be16cdUL, 0xf6b9265bUL, 0x6fb077e1UL, 0x18b74777UL,
    0x88085ae6UL, 0xff0f6a70UL, 0x66063bcaUL, 0x11010b5cUL,
    0x8f659effUL, 0xf862ae69UL, 0x616bffd3UL, 0x166ccf45UL,
    0xa00ae278UL, 0xd70dd2eeUL, 0x4e048354UL, 0x3903b3c2UL,
    0xa7672661UL, 0xd06016f7UL, 0x4969474dUL, 0x3e6e77dbUL,
    0xaed16a4aUL, 0xd9d65adcUL, 0x40df0b66UL, 0x37d83bf0UL,
    0xa9bcae53UL, 0xdebb9ec5UL, 0x47b2cf7fUL, 0x30b5ffe9UL,
    0xbdbdf21cUL, 0xcabac28aUL, 0x53b39330UL, 0x24b4a3a6UL,
    0xbad03605UL, 0xcdd70693UL, 0x54de5729UL, 0x23d967bfUL,
    0xb3667a2eUL, 0xc4614ab8UL, 0x5d681b02UL, 0x2a6f2b94UL,
    0xb40bbe37UL, 0xc30c8ea1UL, 0x5a05df1bUL, 0x2d02ef8dUL,
};
static const SrSnesDspU16 crc_zero[2] __attribute__((section(".yrodata"))) = {0, 0};
extern SrSnesDspU32 sr_dsp_crc_words_teak(const SrSnesDspU16 *p,
        SrSnesDspU16 octets, SrSnesDspU32 crc, const SrSnesDspU32 *table);
static SrSnesDspU32 crc_range(const SrSnesDspU16 *p, SrSnesDspU16 begin,
                              SrSnesDspU16 octets, SrSnesDspU32 crc) {
    if (!begin && octets >= 64u) {
        crc = sr_dsp_crc_words_teak(p, 60u, crc, teak_byte);
        crc = sr_dsp_crc_words_teak(crc_zero, 4u, crc, teak_byte);
        return sr_dsp_crc_words_teak(p + 32u, (SrSnesDspU16)(octets - 64u), crc, teak_byte);
    }
    return sr_dsp_crc_words_teak(p + (begin >> 1), octets, crc, teak_byte);
}
#else
static SrSnesDspU32 crc_range(const SrSnesDspU16 *p, SrSnesDspU16 begin,
                              SrSnesDspU16 octets, SrSnesDspU32 crc) {
    /* Split halves also avoid Teak's saturating 40-to-32 accumulator stores.
     * Every CRC shift/XOR below is exactly 16 bits on both targets. */
    static const SrSnesDspU16 nibble_low[16]
#ifdef __teak__
        __attribute__((section(".yrodata")))
#endif
        = {0x0000u, 0x1064u, 0x20c8u, 0x30acu, 0x4190u, 0x51f4u,
           0x6158u, 0x713cu, 0x8320u, 0x9344u, 0xa3e8u, 0xb38cu,
           0xc2b0u, 0xd2d4u, 0xe278u, 0xf21cu};
    static const SrSnesDspU16 nibble_high[16]
#ifdef __teak__
        __attribute__((section(".yrodata")))
#endif
        = {0x0000u, 0x1db7u, 0x3b6eu, 0x26d9u, 0x76dcu, 0x6b6bu,
           0x4db2u, 0x5005u, 0xedb8u, 0xf00fu, 0xd6d6u, 0xcb61u,
           0x9b64u, 0x86d3u, 0xa00au, 0xbdbdu};
    SrSnesDspU16 i, low = (SrSnesDspU16)crc, high = (SrSnesDspU16)(crc >> 16);
    octets = (SrSnesDspU16)(octets + begin);
    for (i = begin; i < octets; ++i) {
        SrSnesDspU16 v = i >= 60u && i < 64u ? 0u : sr_dsp_wire_u8(p, i);
        SrSnesDspU16 index;
        low ^= v;
        index = low & 15u;
        low = (SrSnesDspU16)(((low >> 4) | (high << 12)) ^ nibble_low[index]);
        high = (SrSnesDspU16)((high >> 4) ^ nibble_high[index]);
        index = low & 15u;
        low = (SrSnesDspU16)(((low >> 4) | (high << 12)) ^ nibble_low[index]);
        high = (SrSnesDspU16)((high >> 4) ^ nibble_high[index]);
    }
    return sr_dsp_u32_join(low, high);
}
#endif
static SrSnesDspU16 rounded(SrSnesDspU16 n) {
    return (SrSnesDspU16)((n + 31u) & 0xffe0u);
}
SrSnesDspU16 sr_dsp_wire_request_head(const SrSnesDspU16 *p) {
    SrSnesDspU16 n = sr_dsp_wire_u16(p, SR_DSP_REQ_EVENTS);
    return n <= SR_DSP_MAX_EVENTS ? rounded((SrSnesDspU16)(64u + n * 16u)) : 0u;
}
SrSnesDspU16 sr_dsp_wire_request_data(const SrSnesDspU16 *p) {
    SrSnesDspU16 n = sr_dsp_wire_u16(p, SR_DSP_REQ_PAYLOAD_BYTES);
    return n <= SR_DSP_MAX_PAYLOAD ? rounded(n) : 0u;
}
SrSnesDspU16 sr_dsp_wire_reply_head(const SrSnesDspU16 *p) {
    SrSnesDspU16 n = sr_dsp_wire_u16(p, SR_DSP_RES_SAMPLES);
    return n <= SR_DSP_MAX_SAMPLES ? rounded((SrSnesDspU16)(192u + n * 4u)) : 0u;
}
SrSnesDspU16 sr_dsp_wire_reply_echo(const SrSnesDspU16 *p) {
    SrSnesDspU16 n = sr_dsp_wire_u16(p, SR_DSP_RES_ECHO_COUNT);
    return n <= SR_DSP_MAX_ECHO ? rounded((SrSnesDspU16)(n * SR_DSP_ECHO_BYTES)) : 0u;
}
SrSnesDspU16 sr_dsp_wire_reply_state(const SrSnesDspU16 *p) {
    SrSnesDspU16 n = sr_dsp_wire_u16(p, SR_DSP_RES_STATE_WORDS);
    return n == SR_SNES_DSP_CORE_STATE_WORDS ? rounded((SrSnesDspU16)(n * 2u)) : 0u;
}
SrSnesDspU32 sr_dsp_wire_crc(const SrSnesDspU16 *p, SrSnesDspU16 octets) {
    SrSnesDspU32 crc = 0xffffffffUL;
    if (octets == SR_DSP_REQUEST_CHECK_BYTES) {
        SrSnesDspU16 n = sr_dsp_wire_u16(p, SR_DSP_REQ_EVENTS);
        SrSnesDspU16 payload = sr_dsp_wire_u16(p, SR_DSP_REQ_PAYLOAD_BYTES);
        if (n > SR_DSP_MAX_EVENTS || payload > SR_DSP_MAX_PAYLOAD) return 0;
        crc = crc_range(p, 0, (SrSnesDspU16)(64u + n * 16u), crc);
        crc = crc_range(p, SR_DSP_REQUEST_PAYLOAD, payload, crc);
    } else if (octets == SR_DSP_REPLY_CHECK_BYTES) {
        SrSnesDspU16 n = sr_dsp_wire_u16(p, SR_DSP_RES_SAMPLES);
        SrSnesDspU16 echo = sr_dsp_wire_u16(p, SR_DSP_RES_ECHO_COUNT);
        SrSnesDspU16 state = sr_dsp_wire_u16(p, SR_DSP_RES_STATE_WORDS);
        if (n > SR_DSP_MAX_SAMPLES || echo > SR_DSP_MAX_ECHO ||
            (state && state != SR_SNES_DSP_CORE_STATE_WORDS)) return 0;
        crc = crc_range(p, 0, (SrSnesDspU16)(192u + n * 4u), crc);
        crc = crc_range(p, SR_DSP_REPLY_ECHO, (SrSnesDspU16)(echo * SR_DSP_ECHO_BYTES), crc);
        crc = crc_range(p, SR_DSP_REPLY_STATE, (SrSnesDspU16)(state * 2u), crc);
    } else return 0;
    return sr_dsp_u32_join((SrSnesDspU16)~crc, (SrSnesDspU16)~(crc >> 16));
}
int sr_dsp_wire_range(SrSnesDspU32 address, SrSnesDspU32 octets) {
    return !(address & 31UL) && octets != 0UL && !(octets & 31UL) &&
        address >= SR_DSP_FCRAM_BEGIN && address < SR_DSP_FCRAM_END &&
        octets <= SR_DSP_FCRAM_END - address;
}
