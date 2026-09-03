#ifndef SNESRECOMP_PLATFORM_MSU_PACK_H
#define SNESRECOMP_PLATFORM_MSU_PACK_H

#include <stdbool.h>
#include <stddef.h>

/* Where an MSU-1 pack lives and whether there is one. The chip is in the
 * shared runtime (runner/src/snes/msu1.c); this only sets SNESRECOMP_MSU1
 * before RtlRegisterGame() arms it. */

#ifdef __cplusplus
extern "C" {
#endif

enum { SNESRECOMP_MSU_PATH_MAX = 1024 };

#define SNESRECOMP_MSU_INI_SECTION  "Sound"
#define SNESRECOMP_MSU_INI_KEY_MODE "Msu1"
#define SNESRECOMP_MSU_INI_KEY_DIR  "Msu1Dir"

typedef enum SnesRecompMsuMode {
    SNESRECOMP_MSU_OFF = 0,
    SNESRECOMP_MSU_AUTO,   /* default: use a pack if one is there */
    SNESRECOMP_MSU_ON,
} SnesRecompMsuMode;

typedef struct SnesRecompMsuRequest {
    const char *ini_path;          /* NULL skips the config file */
    const char *directory;         /* beats the config and the default */
    const char *anchor_directory;  /* exe folder, or a console data root */
    SnesRecompMsuMode mode;
    bool has_mode;                 /* false takes the mode from the config */
    bool driver_present;           /* the game can drive the registers */
} SnesRecompMsuRequest;

typedef struct SnesRecompMsuStatus {
    SnesRecompMsuMode mode;
    char  directory[SNESRECOMP_MSU_PATH_MAX];
    bool  directory_exists;
    bool  pack_found;
    char  pack_base[SNESRECOMP_MSU_PATH_MAX];  /* "<dir>/<name>", no -N.pcm */
    int   track_count;
    bool  armed;
    bool  driver_present;
    const char *reason;                        /* one line for the boot log */
} SnesRecompMsuStatus;

/* off/0/no/false -> OFF, on/1/yes/true -> ON, anything else -> AUTO. */
SnesRecompMsuMode snesrecomp_msu_parse_mode(const char *text);
const char *snesrecomp_msu_mode_name(SnesRecompMsuMode mode);

/* <anchor>/msu, or a relative "msu" without one. */
bool snesrecomp_msu_default_directory(const char *anchor_directory,
                                      char *out,
                                      size_t capacity);

void snesrecomp_msu_ensure_directory(const char *directory);

/* The "<name>" behind the most "<name>-<N>.pcm" files. */
bool snesrecomp_msu_scan_pack(const char *directory,
                              char *base_out,
                              size_t base_capacity,
                              int *track_count);

/* [Sound] Msu1 and Msu1Dir. Writing them is the host's job. */
bool snesrecomp_msu_read_settings(const char *ini_path,
                                  SnesRecompMsuMode *mode,
                                  char *directory,
                                  size_t directory_capacity);

/* Caller re-runs msu1_init() afterwards so the runtime picks it up. */
const SnesRecompMsuStatus *snesrecomp_msu_resolve(
    const SnesRecompMsuRequest *request);

const SnesRecompMsuStatus *snesrecomp_msu_status(void);

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_PLATFORM_MSU_PACK_H */
