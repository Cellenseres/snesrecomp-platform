/* See snesrecomp_platform/rom_verify.h. */
#include "snesrecomp_platform/rom_verify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snesrecomp_platform/sha1.h"

static bool snesrecomp_rom_spec_is_usable(const SnesRecompRomSpec *spec) {
    return spec && spec->payload_size > 0 && spec->accepted_count > 0 &&
           spec->accepted_sha1_hex != NULL;
}

static bool snesrecomp_rom_sha1_accepted(const SnesRecompRomSpec *spec,
                                         const char *sha1_hex) {
    for (int i = 0; i < spec->accepted_count; i++) {
        const char *candidate = spec->accepted_sha1_hex[i];
        if (candidate && strcmp(candidate, sha1_hex) == 0)
            return true;
    }
    return false;
}

const char *snesrecomp_rom_verdict_text(SnesRecompRomVerdict verdict) {
    switch (verdict) {
    case SNESRECOMP_ROM_OK:
        return "accepted";
    case SNESRECOMP_ROM_UNREADABLE:
        return "could not be read";
    case SNESRECOMP_ROM_WRONG_SIZE:
        return "has the wrong size";
    case SNESRECOMP_ROM_WRONG_SHA1:
        return "is not the supported release";
    case SNESRECOMP_ROM_OUT_OF_MEMORY:
        return "could not be loaded (out of memory)";
    case SNESRECOMP_ROM_BAD_SPEC:
        return "cannot be checked (the build declares no accepted ROM)";
    default:
        return "was rejected";
    }
}

static bool snesrecomp_rom_fail(SnesRecompRomReport *report,
                                SnesRecompRomVerdict verdict) {
    if (report)
        report->verdict = verdict;
    return false;
}

bool snesrecomp_rom_load_verified(const SnesRecompRomSpec *spec,
                                  const char *path,
                                  uint8_t **rom_out,
                                  uint32_t *size_out,
                                  bool quiet,
                                  SnesRecompRomReport *report) {
    if (rom_out) *rom_out = NULL;
    if (size_out) *size_out = 0;
    if (report) {
        memset(report, 0, sizeof(*report));
        report->verdict = SNESRECOMP_ROM_UNREADABLE;
    }

    if (!snesrecomp_rom_spec_is_usable(spec)) {
        if (!quiet) {
            fprintf(stderr,
                    "[rom] this build declares no accepted ROM image\n");
        }
        return snesrecomp_rom_fail(report, SNESRECOMP_ROM_BAD_SPEC);
    }

    const char *name = spec->display_name ? spec->display_name : "the game";

    if (!path || !*path)
        return snesrecomp_rom_fail(report, SNESRECOMP_ROM_UNREADABLE);

    FILE *f = fopen(path, "rb");
    if (!f) {
        if (!quiet)
            fprintf(stderr, "Could not open ROM: %s\n", path);
        return snesrecomp_rom_fail(report, SNESRECOMP_ROM_UNREADABLE);
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return snesrecomp_rom_fail(report, SNESRECOMP_ROM_UNREADABLE);
    }

    long file_size_long = ftell(f);
    if (file_size_long <= 0) {
        fclose(f);
        return snesrecomp_rom_fail(report, SNESRECOMP_ROM_UNREADABLE);
    }
    rewind(f);

    const size_t file_size = (size_t)file_size_long;
    if (report)
        report->file_size = (uint64_t)file_size;

    uint8_t *file_data = (uint8_t *)malloc(file_size);
    if (!file_data) {
        fclose(f);
        return snesrecomp_rom_fail(report, SNESRECOMP_ROM_OUT_OF_MEMORY);
    }

    if (fread(file_data, 1, file_size, f) != file_size) {
        free(file_data);
        fclose(f);
        if (!quiet)
            fprintf(stderr, "Short read on ROM: %s\n", path);
        return snesrecomp_rom_fail(report, SNESRECOMP_ROM_UNREADABLE);
    }
    fclose(f);

    size_t payload_offset = 0;
    size_t payload_size = file_size;

    /* Accept a legacy copier header when the game declares one. */
    if (spec->copier_header_size != 0 &&
        file_size == (size_t)spec->payload_size + spec->copier_header_size) {
        payload_offset = spec->copier_header_size;
        payload_size = spec->payload_size;
        if (report)
            report->copier_header_stripped = true;
    }

    if (payload_size != (size_t)spec->payload_size) {
        if (!quiet) {
            if (spec->copier_header_size != 0) {
                fprintf(stderr,
                    "Wrong %s ROM size: %zu bytes "
                    "(expected %u unheadered, or %u with copier header)\n",
                    name, file_size, (unsigned)spec->payload_size,
                    (unsigned)(spec->payload_size + spec->copier_header_size));
            } else {
                fprintf(stderr,
                    "Wrong %s ROM size: %zu bytes (expected %u)\n",
                    name, file_size, (unsigned)spec->payload_size);
            }
        }
        free(file_data);
        return snesrecomp_rom_fail(report, SNESRECOMP_ROM_WRONG_SIZE);
    }

    uint8_t digest[SNESRECOMP_SHA1_DIGEST_SIZE];
    char sha1_hex[SNESRECOMP_SHA1_HEX_SIZE];
    snesrecomp_sha1_compute(file_data + payload_offset, payload_size, digest);
    snesrecomp_sha1_hex(digest, sha1_hex);
    if (report)
        memcpy(report->sha1_hex, sha1_hex, sizeof(sha1_hex));

    if (!snesrecomp_rom_sha1_accepted(spec, sha1_hex)) {
        if (!quiet) {
            fprintf(stderr,
                "ROM SHA-1 mismatch.\n"
                "Expected: %s\n"
                "Actual:   %s\n",
                spec->accepted_sha1_hex[0], sha1_hex);
        }
        free(file_data);
        return snesrecomp_rom_fail(report, SNESRECOMP_ROM_WRONG_SHA1);
    }

    uint8_t *payload = file_data;
    if (payload_offset != 0) {
        payload = (uint8_t *)malloc(payload_size);
        if (!payload) {
            free(file_data);
            return snesrecomp_rom_fail(report, SNESRECOMP_ROM_OUT_OF_MEMORY);
        }
        memcpy(payload, file_data + payload_offset, payload_size);
        free(file_data);
        fprintf(stderr,
            "[rom] stripped %u-byte copier header before boot/hash\n",
            (unsigned)spec->copier_header_size);
    }

    if (rom_out) *rom_out = payload;
    else free(payload);
    if (size_out) *size_out = (uint32_t)payload_size;
    if (report) report->verdict = SNESRECOMP_ROM_OK;
    return true;
}

bool snesrecomp_rom_path_is_verified(const SnesRecompRomSpec *spec,
                                     const char *path) {
    uint8_t *data = NULL;
    uint32_t size = 0;
    if (!snesrecomp_rom_load_verified(spec, path, &data, &size, true, NULL))
        return false;
    free(data);
    return true;
}
