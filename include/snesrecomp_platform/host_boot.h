/* Which ROM to boot, host configuration, and the pre-loop services that differ
 * per platform. A game supplies a descriptor and calls the same functions
 * everywhere, so it needs no platform #ifdefs.
 *
 *   host_boot_desktop.c  recomp-ui launcher, ROM cache, config.ini, keybinds
 *   target adapter       one fixed ROM path, none of the above
 *
 * Both are attached to the game target by
 * snesrecomp_platform_target_host_boot(), not linked into the library, so the
 * library stays free of recomp-ui and of any console SDK.
 */
#ifndef SNESRECOMP_PLATFORM_HOST_BOOT_H
#define SNESRECOMP_PLATFORM_HOST_BOOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "snesrecomp_platform/rom_verify.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SNES pad bitmask. */
enum {
    SNESRECOMP_PAD_B      = 0x001,
    SNESRECOMP_PAD_Y      = 0x002,
    SNESRECOMP_PAD_SELECT = 0x004,
    SNESRECOMP_PAD_START  = 0x008,
    SNESRECOMP_PAD_UP     = 0x010,
    SNESRECOMP_PAD_DOWN   = 0x020,
    SNESRECOMP_PAD_LEFT   = 0x040,
    SNESRECOMP_PAD_RIGHT  = 0x080,
    SNESRECOMP_PAD_A      = 0x100,
    SNESRECOMP_PAD_X      = 0x200,
    SNESRECOMP_PAD_L      = 0x400,
    SNESRECOMP_PAD_R      = 0x800,
};

/* Own labels are fine, this order is not: the index is the contract with the
 * runner's kOutputMethod_* values. */
enum {
    SNESRECOMP_HOST_RENDERER_SDL = 0,
    SNESRECOMP_HOST_RENDERER_SDL_SOFTWARE,
    SNESRECOMP_HOST_RENDERER_OPENGL,
    SNESRECOMP_HOST_RENDERER_COUNT,
};

typedef struct SnesRecompHostGame {
    const char *game_id;               /* SRAM migration, crash reports */
    const char *display_name;          /* shown in the launcher */
    const char *region;                /* "(USA)", optional */
    const char *short_name;            /* file headers; defaults to the title */
    const char *launcher_profile;      /* recomp-ui console, e.g. "snes" */
    const SnesRecompRomSpec *rom;

    /* NULL uses the default renderer list above. */
    const char *const *renderer_labels;
    int renderer_count;

    /* NULL selects the default named beside each. */
    const char *sram_path;             /* "saves/save.srm" */
    const char *config_path;           /* "config.ini"     */
    const char *keybinds_path;         /* "keybinds.ini"   */
    const char *platform_config_path;  /* "platform.ini"   */

    /* Written when config_path is absent. NULL uses the standard template. */
    const char *default_config_ini;

    int widescreen_supported;
    int num_players;                   /* 0 means 1 */

    /* Used by hosts with no file picker. The adapter owns the platform path
     * syntax and combines these game-owned names with its data root. */
    const char *data_directory_name;
    const char *rom_filename;
} SnesRecompHostGame;

/* Host lifecycle and path services. Desktop implements these as ordinary
 * paths/no-ops; a target adapter can provide writable roots and native
 * hardware policy without exposing that platform to the game. */
bool snesrecomp_host_runtime_init(const SnesRecompHostGame *game,
                                  const char *build_stamp);
const char *snesrecomp_host_logs_root(void); /* NULL when not redirected */
const char *snesrecomp_host_saves_root(void); /* NULL keeps runner default */
void snesrecomp_host_log_file_probe(const char *path);
bool snesrecomp_host_set_environment(const char *name, const char *value);

bool snesrecomp_host_data_file_exists(const SnesRecompHostGame *game,
                                      const char *relative_path);
bool snesrecomp_host_resolve_data_file(const SnesRecompHostGame *game,
                                       const char *user_relative_path,
                                       const char *packaged_relative_path,
                                       char *path,
                                       size_t path_capacity);

bool snesrecomp_host_native_presenter_enabled(void);
bool snesrecomp_host_gpu_backgrounds_enabled(void);
bool snesrecomp_host_gpu_backdrop_only(void);
void snesrecomp_host_apply_performance_profile(void);
bool snesrecomp_host_parallel_workers_enabled(void);
bool snesrecomp_host_pin_main_thread(void);
void snesrecomp_host_pin_helper_thread(unsigned index);
void snesrecomp_host_pin_audio_thread(void);

/* Called once, before SDL_Init. On true, `rom_path` is the image the caller
 * loads through snesrecomp_rom_load_verified() and g_config is populated.
 * False means "do not boot" ? Quit and --help return that way too. */
bool snesrecomp_host_resolve_rom(const SnesRecompHostGame *game,
                                 int argc,
                                 char **argv,
                                 char *rom_path,
                                 size_t rom_path_capacity);

/* Verification succeeded; the desktop remembers the path for SkipLauncher. */
void snesrecomp_host_rom_accepted(const SnesRecompHostGame *game,
                                  const char *rom_path);

/* After SDL_Init, before the main loop. */
void snesrecomp_host_init_input(const SnesRecompHostGame *game);

/* SNESRECOMP_PAD_* bits; zero on hosts without a keyboard. */
uint32_t snesrecomp_host_read_keyboard_pad(void);

/* No-op where there is no settings file. */
void snesrecomp_host_persist_int(const SnesRecompHostGame *game,
                                 const char *section,
                                 const char *key,
                                 int value);

void snesrecomp_host_persist_config(const SnesRecompHostGame *game);

/* Resolved while bringing configuration up. */
bool snesrecomp_host_vsync_enabled(void);
int snesrecomp_host_volume_percent(void);

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_PLATFORM_HOST_BOOT_H */
