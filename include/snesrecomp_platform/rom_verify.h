/* ROM identity gate. Size, copier header and accepted digests come from a
 * per-game spec, so every host runs the same check. */
#ifndef SNESRECOMP_PLATFORM_ROM_VERIFY_H
#define SNESRECOMP_PLATFORM_ROM_VERIFY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnesRecompRomSpec {
    const char *display_name;
    uint32_t payload_size;          /* size after any header is stripped */
    uint32_t copier_header_size;    /* 0 rejects headered images */
    const char *const *accepted_sha1_hex;   /* 40 lowercase hex digits each */
    int accepted_count;             /* 0 accepts nothing */
} SnesRecompRomSpec;

typedef enum SnesRecompRomVerdict {
    SNESRECOMP_ROM_OK = 0,
    SNESRECOMP_ROM_UNREADABLE,      /* missing, empty, or a short read */
    SNESRECOMP_ROM_WRONG_SIZE,
    SNESRECOMP_ROM_WRONG_SHA1,
    SNESRECOMP_ROM_OUT_OF_MEMORY,
    SNESRECOMP_ROM_BAD_SPEC,
} SnesRecompRomVerdict;

/* `verdict` is read across a static-library boundary, so the compiler's enum
 * width is part of this header's ABI. See presenter.h for the full reasoning;
 * both headers assert it so a target built with a different width fails to
 * build rather than reading bytes the other side never wrote. */
_Static_assert(sizeof(SnesRecompRomVerdict) == sizeof(int),
               "snesrecomp_platform requires int-sized enums; build this "
               "target with -fno-short-enums.");

typedef struct SnesRecompRomReport {
    SnesRecompRomVerdict verdict;
    uint64_t file_size;
    char sha1_hex[41];              /* set whenever the image was hashed */
    bool copier_header_stripped;
} SnesRecompRomReport;

/* On success stores a payload the caller must free(). `report` may be NULL.
 * `quiet` suppresses stderr, for probing a remembered path. */
bool snesrecomp_rom_load_verified(const SnesRecompRomSpec *spec,
                                  const char *path,
                                  uint8_t **rom_out,
                                  uint32_t *size_out,
                                  bool quiet,
                                  SnesRecompRomReport *report);

/* Probe only; never prints. */
bool snesrecomp_rom_path_is_verified(const SnesRecompRomSpec *spec,
                                     const char *path);

/* One line, for host-authored messages. */
const char *snesrecomp_rom_verdict_text(SnesRecompRomVerdict verdict);

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_PLATFORM_ROM_VERIFY_H */
