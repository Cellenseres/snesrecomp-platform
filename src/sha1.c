/* SHA-1 (RFC 3174). See snesrecomp_platform/sha1.h. */
#include "snesrecomp_platform/sha1.h"

#include <string.h>

static uint32_t snesrecomp_sha1_rotl(uint32_t value, unsigned bits) {
    return (uint32_t)((value << bits) | (value >> (32u - bits)));
}

static void snesrecomp_sha1_block(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (unsigned i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4 + 0] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (unsigned i = 16; i < 80; i++)
        w[i] = snesrecomp_sha1_rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];

    for (unsigned i = 0; i < 80; i++) {
        uint32_t f;
        uint32_t k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const uint32_t t = snesrecomp_sha1_rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = snesrecomp_sha1_rotl(b, 30);
        b = a;
        a = t;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void snesrecomp_sha1_compute(const void *data, size_t length,
                       uint8_t digest[SNESRECOMP_SHA1_DIGEST_SIZE]) {
    uint32_t state[5] = {
        0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u,
    };

    const uint8_t *bytes = (const uint8_t *)data;
    const size_t whole_blocks = length / 64u;
    for (size_t i = 0; i < whole_blocks; i++)
        snesrecomp_sha1_block(state, bytes + i * 64u);

    /* Tail: remainder, 0x80, zero padding, then the 64-bit big-endian bit
     * count. That never needs more than two blocks. */
    uint8_t tail[128];
    const size_t remainder = length - whole_blocks * 64u;
    memset(tail, 0, sizeof(tail));
    if (remainder != 0)
        memcpy(tail, bytes + whole_blocks * 64u, remainder);
    tail[remainder] = 0x80u;

    const size_t tail_size = (remainder < 56u) ? 64u : 128u;
    const uint64_t bit_length = (uint64_t)length * 8u;
    for (unsigned i = 0; i < 8; i++)
        tail[tail_size - 1u - i] = (uint8_t)(bit_length >> (8u * i));

    snesrecomp_sha1_block(state, tail);
    if (tail_size == 128u)
        snesrecomp_sha1_block(state, tail + 64u);

    for (unsigned i = 0; i < 5; i++) {
        digest[i * 4 + 0] = (uint8_t)(state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)(state[i]);
    }
}

void snesrecomp_sha1_hex(const uint8_t digest[SNESRECOMP_SHA1_DIGEST_SIZE],
                   char out[SNESRECOMP_SHA1_HEX_SIZE]) {
    static const char kHex[] = "0123456789abcdef";
    for (unsigned i = 0; i < SNESRECOMP_SHA1_DIGEST_SIZE; i++) {
        out[i * 2 + 0] = kHex[(digest[i] >> 4) & 0x0Fu];
        out[i * 2 + 1] = kHex[digest[i] & 0x0Fu];
    }
    out[SNESRECOMP_SHA1_DIGEST_SIZE * 2] = '\0';
}
