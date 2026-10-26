#ifndef SNESRECOMP_PLATFORM_SHA256_H
#define SNESRECOMP_PLATFORM_SHA256_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* One-shot SHA-256. Input may be unaligned; NULL is valid only for zero bytes.
 * NULL arguments outside this contract leave the output untouched. No allocation or shared state. */
bool snesrecomp_sha256(const uint8_t *data,size_t bytes,uint8_t hash[32]);
#ifdef __cplusplus
}
#endif
#endif
