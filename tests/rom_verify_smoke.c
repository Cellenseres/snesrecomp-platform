/* SHA-1 vectors and the accept/reject rules of the ROM gate. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snesrecomp_platform/rom_verify.h"
#include "snesrecomp_platform/sha1.h"

#define PAYLOAD_SIZE 4096
#define COPIER_HEADER 512

static int g_failures;

static void check(int condition, const char *what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

static void check_sha1(const void *data, size_t len, const char *expected) {
    uint8_t digest[SNESRECOMP_SHA1_DIGEST_SIZE];
    char hex[SNESRECOMP_SHA1_HEX_SIZE];
    snesrecomp_sha1_compute(data, len, digest);
    snesrecomp_sha1_hex(digest, hex);
    if (strcmp(hex, expected) != 0) {
        fprintf(stderr, "FAIL: sha1 of %zu bytes: got %s want %s\n",
                len, hex, expected);
        g_failures++;
    }
}

static const char *write_temp(const char *name, const uint8_t *data,
                              size_t len) {
    FILE *f = fopen(name, "wb");
    if (!f) {
        fprintf(stderr, "FAIL: could not create %s\n", name);
        g_failures++;
        return name;
    }
    if (len && fwrite(data, 1, len, f) != len) {
        fprintf(stderr, "FAIL: short write on %s\n", name);
        g_failures++;
    }
    fclose(f);
    return name;
}

int main(void) {
    /* RFC 3174 / NIST vectors. */
    check_sha1("", 0, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    check_sha1("abc", 3, "a9993e364706816aba3e25717850c26c9cd0d89d");
    check_sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
               "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    {
        char *big = (char *)malloc(1000000);
        if (!big) return 1;
        memset(big, 'a', 1000000);
        check_sha1(big, 1000000,
                   "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
        free(big);
    }

    uint8_t payload[PAYLOAD_SIZE];
    for (int i = 0; i < PAYLOAD_SIZE; i++)
        payload[i] = (uint8_t)(i * 7 + 3);

    uint8_t digest[SNESRECOMP_SHA1_DIGEST_SIZE];
    char payload_sha1[SNESRECOMP_SHA1_HEX_SIZE];
    snesrecomp_sha1_compute(payload, PAYLOAD_SIZE, digest);
    snesrecomp_sha1_hex(digest, payload_sha1);

    const char *accepted[] = { payload_sha1 };
    const SnesRecompRomSpec spec = {
        "Test Game", PAYLOAD_SIZE, COPIER_HEADER, accepted, 1,
    };

    uint8_t *rom = NULL;
    uint32_t size = 0;
    SnesRecompRomReport report;

    const char *good = write_temp("rom_verify_good.bin",
                                  payload, PAYLOAD_SIZE);
    check(snesrecomp_rom_load_verified(&spec, good, &rom, &size, true, &report),
          "clean image accepted");
    check(size == PAYLOAD_SIZE, "accepted size");
    check(rom && memcmp(rom, payload, PAYLOAD_SIZE) == 0, "payload intact");
    check(!report.copier_header_stripped, "no header reported");
    free(rom);
    rom = NULL;

    /* Same payload behind a copier header: hashed after stripping. */
    uint8_t headered[COPIER_HEADER + PAYLOAD_SIZE];
    memset(headered, 0xA5, COPIER_HEADER);
    memcpy(headered + COPIER_HEADER, payload, PAYLOAD_SIZE);
    const char *hdr = write_temp("rom_verify_headered.bin",
                                 headered, sizeof(headered));
    check(snesrecomp_rom_load_verified(&spec, hdr, &rom, &size, true, &report),
          "copier-headered image accepted");
    check(size == PAYLOAD_SIZE, "stripped size");
    check(rom && memcmp(rom, payload, PAYLOAD_SIZE) == 0, "stripped payload");
    check(report.copier_header_stripped, "header reported");
    free(rom);
    rom = NULL;

    /* One flipped byte: must fail on the digest, not the size. */
    uint8_t wrong[PAYLOAD_SIZE];
    memcpy(wrong, payload, PAYLOAD_SIZE);
    wrong[PAYLOAD_SIZE / 2] ^= 0xFF;
    const char *bad = write_temp("rom_verify_wrong.bin", wrong, PAYLOAD_SIZE);
    check(!snesrecomp_rom_load_verified(&spec, bad, &rom, &size, true, &report),
          "wrong ROM rejected");
    check(report.verdict == SNESRECOMP_ROM_WRONG_SHA1, "wrong-sha1 verdict");
    check(rom == NULL && size == 0, "nothing handed back on rejection");

    const char *short_rom = write_temp("rom_verify_short.bin", payload, 128);
    check(!snesrecomp_rom_load_verified(&spec, short_rom, &rom, &size, true,
                                        &report),
          "short ROM rejected");
    check(report.verdict == SNESRECOMP_ROM_WRONG_SIZE, "wrong-size verdict");

    check(!snesrecomp_rom_load_verified(&spec, "rom_verify_missing.bin", &rom,
                                        &size, true, &report),
          "missing ROM rejected");
    check(report.verdict == SNESRECOMP_ROM_UNREADABLE, "unreadable verdict");

    /* No accepted digest means nothing is accepted. */
    const SnesRecompRomSpec empty = { "Test Game", PAYLOAD_SIZE, 0, NULL, 0 };
    check(!snesrecomp_rom_load_verified(&empty, good, &rom, &size, true,
                                        &report),
          "empty spec accepts nothing");
    check(report.verdict == SNESRECOMP_ROM_BAD_SPEC, "bad-spec verdict");

    check(snesrecomp_rom_path_is_verified(&spec, good), "probe accepts");
    check(!snesrecomp_rom_path_is_verified(&spec, bad), "probe rejects");

    remove("rom_verify_good.bin");
    remove("rom_verify_headered.bin");
    remove("rom_verify_wrong.bin");
    remove("rom_verify_short.bin");

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("rom_verify smoke: all checks passed\n");
    return 0;
}
