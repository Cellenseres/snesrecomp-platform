/* SHA-1 (RFC 3174). Freestanding: no SDL, no allocation. */
#ifndef SNESRECOMP_PLATFORM_SHA1_H
#define SNESRECOMP_PLATFORM_SHA1_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SNESRECOMP_SHA1_DIGEST_SIZE 20
#define SNESRECOMP_SHA1_HEX_SIZE 41 /* 40 hex digits + NUL */

void snesrecomp_sha1_compute(const void *data, size_t length,
                             uint8_t digest[SNESRECOMP_SHA1_DIGEST_SIZE]);

/* 40 lowercase hex digits plus NUL. */
void snesrecomp_sha1_hex(const uint8_t digest[SNESRECOMP_SHA1_DIGEST_SIZE],
                         char out[SNESRECOMP_SHA1_HEX_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_PLATFORM_SHA1_H */
