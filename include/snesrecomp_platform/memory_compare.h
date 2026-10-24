#ifndef SNESRECOMP_PLATFORM_MEMORY_COMPARE_H
#define SNESRECOMP_PLATFORM_MEMORY_COMPARE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Typed snapshot comparisons. ARM11's small libc compares one word at a time;
 * batch equal words without aliasing or unaligned casts.
 * Other hosts retain their architecture-optimized libc implementation. */
static inline bool snesrecomp_u8_equal(const uint8_t *a, const uint8_t *b, size_t count) {
#if defined(__arm__) && !defined(__ARM_NEON)
    /* Equal alignment permits word loads after at most three prefix bytes.
     * memcpy reads object representations without aliasing a byte array as a
     * uint32 array. The asserted alignment keeps ARM11 away from unaligned
     * loads; every load remains within the caller's supplied byte range. */
    if ((((uintptr_t)a ^ (uintptr_t)b) & 3u) == 0) {
        while (count && ((uintptr_t)a & 3u)) {
            if (*a++ != *b++) return false;
            --count;
        }
        /* A short prefix may exhaust the range before reaching alignment. */
        if (!count) return true;
        const uint8_t *wa = (const uint8_t *)__builtin_assume_aligned(a, 4);
        const uint8_t *wb = (const uint8_t *)__builtin_assume_aligned(b, 4);
        while (count >= 32) {
            uint32_t left[8], right[8];
            __builtin_memcpy(left, wa, sizeof left);
            __builtin_memcpy(right, wb, sizeof right);
            if ((left[0] ^ right[0]) | (left[1] ^ right[1]) |
                (left[2] ^ right[2]) | (left[3] ^ right[3]) |
                (left[4] ^ right[4]) | (left[5] ^ right[5]) |
                (left[6] ^ right[6]) | (left[7] ^ right[7])) return false;
            wa += 32; wb += 32; count -= 32;
        }
        while (count >= 4) {
            uint32_t left, right;
            __builtin_memcpy(&left, wa, sizeof left);
            __builtin_memcpy(&right, wb, sizeof right);
            if (left != right) return false;
            wa += 4; wb += 4; count -= 4;
        }
        while (count--) if (*wa++ != *wb++) return false;
        return true;
    }
    while (count >= 8) {
        if ((a[0] ^ b[0]) | (a[1] ^ b[1]) | (a[2] ^ b[2]) | (a[3] ^ b[3]) |
            (a[4] ^ b[4]) | (a[5] ^ b[5]) | (a[6] ^ b[6]) | (a[7] ^ b[7])) return false;
        a += 8; b += 8; count -= 8;
    }
    while (count--) if (*a++ != *b++) return false;
    return true;
#else
    return !count || !memcmp(a, b, count);
#endif
}
static inline bool snesrecomp_u16_equal(const uint16_t *a, const uint16_t *b, size_t count) {
#if defined(__arm__) && !defined(__ARM_NEON)
    if (count <= SIZE_MAX / sizeof *a && (((uintptr_t)a ^ (uintptr_t)b) & 3u) == 0)
        return snesrecomp_u8_equal((const uint8_t *)a, (const uint8_t *)b, count * sizeof *a);
    while (count >= 8) {
        if ((a[0] ^ b[0]) | (a[1] ^ b[1]) | (a[2] ^ b[2]) | (a[3] ^ b[3]) |
            (a[4] ^ b[4]) | (a[5] ^ b[5]) | (a[6] ^ b[6]) | (a[7] ^ b[7])) return false;
        a += 8; b += 8; count -= 8;
    }
    while (count--) if (*a++ != *b++) return false;
    return true;
#else
    return !count || !memcmp(a, b, count * sizeof *a);
#endif
}
#endif
