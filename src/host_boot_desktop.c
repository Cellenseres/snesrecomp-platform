/* Desktop host boot: ROM argument, --launcher, SkipLauncher, the cached path,
 * the recomp-ui launcher window, config.ini and keybinds.ini.
 *
 * The only file here that talks to recomp-ui, and never part of the library.
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Include paths come from the game target. */
#include "desktop/sdl_compat.h"
#include "desktop/config.h"
#include "host_paths.h"
#include "host_report.h"
#include "launcher_cache.h"
#include "common_rtl.h"

/* recomp-ui. */
#include "recomp_launcher.h"
#include "launcher_profile.h"
#include "common/keybinds.h"
#include "common/launcher_binds.h"
#include "snesrecomp_platform/host_boot.h"
#include "snesrecomp_platform/rom_verify.h"

bool snesrecomp_host_runtime_init(const SnesRecompHostGame *game,
                                  const char *build_stamp) {
    (void)game;
    (void)build_stamp;
    return true;
}

const char *snesrecomp_host_logs_root(void) { return NULL; }
const char *snesrecomp_host_saves_root(void) { return NULL; }
void snesrecomp_host_log_file_probe(const char *path) { (void)path; }

bool snesrecomp_host_set_environment(const char *name, const char *value) {
    if (!name || !name[0] || !value)
        return false;
#if defined(_WIN32)
    return _putenv_s(name, value) == 0;
#else
    return setenv(name, value, 1) == 0;
#endif
}

bool snesrecomp_host_data_file_exists(const SnesRecompHostGame *game,
                                      const char *relative_path) {
    (void)game;
    FILE *probe = relative_path ? fopen(relative_path, "rb") : NULL;
    if (!probe)
        return false;
    fclose(probe);
    return true;
}

bool snesrecomp_host_resolve_data_file(const SnesRecompHostGame *game,
                                       const char *user_relative_path,
                                       const char *packaged_relative_path,
                                       char *path,
                                       size_t path_capacity) {
    const char *resolved = NULL;
    if (snesrecomp_host_data_file_exists(game, user_relative_path))
        resolved = user_relative_path;
    else if (snesrecomp_host_data_file_exists(game, packaged_relative_path))
        resolved = packaged_relative_path;
    if (!path || !path_capacity || !resolved) {
        return false;
    }
    snprintf(path, path_capacity, "%s", resolved);
    return true;
}

bool snesrecomp_host_native_presenter_enabled(void) { return false; }
bool snesrecomp_host_gpu_backgrounds_enabled(void) { return false; }
bool snesrecomp_host_gpu_backdrop_only(void) { return false; }
void snesrecomp_host_apply_performance_profile(void) {}
bool snesrecomp_host_parallel_workers_enabled(void) { return false; }
bool snesrecomp_host_pin_main_thread(void) { return false; }
void snesrecomp_host_pin_helper_thread(unsigned index) { (void)index; }
void snesrecomp_host_pin_audio_thread(void) {}

enum {
    SNESRECOMP_HOST_DEFAULT_WINDOW_SCALE = 3,

    SNESRECOMP_HOST_PLAYER_INPUT_NONE = 0,
    SNESRECOMP_HOST_PLAYER_INPUT_KEYBOARD,
    SNESRECOMP_HOST_PLAYER_INPUT_GAMEPAD,
};

static const char *const kDefaultRendererLabels[] = {
    "SDL Accelerated",
    "SDL Software",
    "OpenGL 3.3",
};

static bool s_vsync_enabled = true;
static int s_volume_percent = 100;

static const char *Or(const char *value, const char *fallback) {
    return (value && *value) ? value : fallback;
}

static const char *ConfigPath(const SnesRecompHostGame *game) {
    return Or(game->config_path, "config.ini");
}

static const char *PlatformConfigPath(const SnesRecompHostGame *game) {
    return Or(game->platform_config_path, "platform.ini");
}

static const char *ShortName(const SnesRecompHostGame *game) {
    return Or(game->short_name, Or(game->display_name, "SNESRecomp"));
}

static bool FileExists(const char *path) {
    if (!path || !*path)
        return false;
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

static char *TrimAscii(char *text) {
    while (*text && isspace((unsigned char)*text))
        text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return text;
}

static bool AsciiEqualsNoCase(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static bool ReadIniBool(
    const char *path,
    const char *wanted_section,
    const char *wanted_key,
    bool fallback) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return fallback;

    char section[64] = "";
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *comment = strpbrk(line, "#;");
        if (comment)
            *comment = '\0';
        char *text = TrimAscii(line);
        const size_t length = strlen(text);
        if (length >= 2 && text[0] == '[' && text[length - 1] == ']') {
            text[length - 1] = '\0';
            snprintf(section, sizeof(section), "%s", TrimAscii(text + 1));
            continue;
        }
        if (!AsciiEqualsNoCase(section, wanted_section))
            continue;

        char *equals = strchr(text, '=');
        if (!equals)
            continue;
        *equals = '\0';
        if (!AsciiEqualsNoCase(TrimAscii(text), wanted_key))
            continue;

        char *value = TrimAscii(equals + 1);
        fclose(f);
        if (AsciiEqualsNoCase(value, "1") ||
            AsciiEqualsNoCase(value, "true") ||
            AsciiEqualsNoCase(value, "on")) {
            return true;
        }
        if (AsciiEqualsNoCase(value, "0") ||
            AsciiEqualsNoCase(value, "false") ||
            AsciiEqualsNoCase(value, "off")) {
            return false;
        }
        return fallback;
    }

    fclose(f);
    return fallback;
}

/* Only the banner names the game; the keys are the runner's vocabulary, so
 * one template serves every game that does not override it. */
static bool WriteDefaultConfig(const SnesRecompHostGame *game,
                               const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;

    bool ok;
    if (game->default_config_ini) {
        const size_t n = strlen(game->default_config_ini);
        ok = fwrite(game->default_config_ini, 1, n, f) == n;
    } else {
        static const char kBody[] =
            "# The launcher edits the supported settings in-place.\n"
            "\n"
            "[General]\n"
            "SkipLauncher = 0\n"
            "DisplayPerfInTitle = 0\n"
            "DisableFrameDelay = 0\n"
            "\n"
            "[Graphics]\n"
            "WindowScale = 3\n"
            "Fullscreen = 0\n"
            "IgnoreAspectRatio = 0\n"
            "DisplayAspect = 4:3\n"
            "OutputMethod = OpenGL\n"
            "LinearFiltering = 0\n"
            "Shader =\n"
            "NewRenderer = 0\n"
            "NoSpriteLimits = 0\n"
            "Widescreen = 0\n"
            "\n"
            "[Sound]\n"
            "EnableAudio = 1\n"
            "AudioFreq = 32040\n"
            "AudioChannels = 2\n"
            "AudioSamples = 512\n"
            "\n"
            "[GamepadMap]\n"
            "EnableGamepad1 = true\n"
            "EnableGamepad2 = false\n"
            "GamepadDeadzone = 10000\n"
            "\n"
            "[KeyMap]\n"
            "# Controller buttons are stored separately in keybinds.ini.\n"
            "# Unsafe/unfinished host actions begin unbound.\n"
            "Load =\n"
            "Save =\n"
            "Reset =\n"
            "ToggleWidescreen =\n"
            "Fullscreen = Alt+Return\n"
            "Pause = Shift+p\n"
            "PauseDimmed = p\n"
            "Turbo = Tab\n"
            "DisplayPerf = f\n"
            "ToggleRenderer = F8\n"
            "WindowBigger =\n"
            "WindowSmaller =\n"
            "VolumeUp =\n"
            "VolumeDown =\n";

        ok = fprintf(f, "# %s desktop configuration\n", ShortName(game)) > 0;
        const size_t n = sizeof(kBody) - 1;
        ok = ok && fwrite(kBody, 1, n, f) == n;
    }

    fclose(f);
    if (ok)
        fprintf(stderr, "[config] created %s\n", path);
    return ok;
}

static bool EnsureDefaultConfig(const SnesRecompHostGame *game,
                                const char *path) {
    if (FileExists(path))
        return true;
    return WriteDefaultConfig(game, path);
}

static bool EnsureDefaultPlatformConfig(const char *path) {
    if (FileExists(path))
        return true;

    FILE *f = fopen(path, "wb");
    if (!f)
        return false;

    static const char kDefaultPlatformConfig[] =
        "[Video]\n"
        "VSync = 1\n";
    const size_t n = sizeof(kDefaultPlatformConfig) - 1;
    const bool ok = fwrite(kDefaultPlatformConfig, 1, n, f) == n;
    fclose(f);
    return ok;
}

static int LauncherRendererFromOutputMethod(int output_method) {
    switch (output_method) {
    case kOutputMethod_SDLSoftware:
        return SNESRECOMP_HOST_RENDERER_SDL_SOFTWARE;
    case kOutputMethod_OpenGL:
        return SNESRECOMP_HOST_RENDERER_OPENGL;
    case kOutputMethod_SDL:
    default:
        return SNESRECOMP_HOST_RENDERER_SDL;
    }
}

static uint8 OutputMethodFromLauncherRenderer(int renderer) {
    switch (renderer) {
    case SNESRECOMP_HOST_RENDERER_SDL_SOFTWARE:
        return kOutputMethod_SDLSoftware;
    case SNESRECOMP_HOST_RENDERER_OPENGL:
        return kOutputMethod_OpenGL;
    case SNESRECOMP_HOST_RENDERER_SDL:
    default:
        return kOutputMethod_SDL;
    }
}

void snesrecomp_host_persist_int(const SnesRecompHostGame *game,
                                 const char *section,
                                 const char *key,
                                 int value) {
    char text[64];
    snprintf(text, sizeof(text), "%d", value);
    launcher_ini_kv_write(ConfigPath(game), section, key, text);
}

void snesrecomp_host_persist_config(const SnesRecompHostGame *game) {
    WriteConfigFile(ConfigPath(game));
}

bool snesrecomp_host_vsync_enabled(void) {
    return s_vsync_enabled;
}

int snesrecomp_host_volume_percent(void) {
    return s_volume_percent;
}

void snesrecomp_host_rom_accepted(const SnesRecompHostGame *game,
                                  const char *rom_path) {
    (void)game;
    snesrecomp_rom_cache_write(rom_path);
}

void snesrecomp_host_init_input(const SnesRecompHostGame *game) {
    (void)game;
    /* Apply bindings changed in the launcher. */
    recompui_keybinds_init(NULL);
}

static bool KeyDown(const uint8_t *keys, SDL_Scancode sc) {
    return keys && sc != SDL_SCANCODE_UNKNOWN && keys[sc] != 0;
}

uint32_t snesrecomp_host_read_keyboard_pad(void) {
    const uint8_t *keys = snesrecomp_sdl_get_keyboard_state();
    const KeyBinds *binds = recompui_keybinds_get();
    if (!keys || !binds)
        return 0;

    const PlayerBinds *b = &binds->p1;
    uint32_t p = 0;

    if (KeyDown(keys, b->up))     p |= SNESRECOMP_PAD_UP;
    if (KeyDown(keys, b->down))   p |= SNESRECOMP_PAD_DOWN;
    if (KeyDown(keys, b->left))   p |= SNESRECOMP_PAD_LEFT;
    if (KeyDown(keys, b->right))  p |= SNESRECOMP_PAD_RIGHT;
    if (KeyDown(keys, b->b))      p |= SNESRECOMP_PAD_B;
    if (KeyDown(keys, b->a))      p |= SNESRECOMP_PAD_A;
    if (KeyDown(keys, b->y))      p |= SNESRECOMP_PAD_Y;
    if (KeyDown(keys, b->x))      p |= SNESRECOMP_PAD_X;
    if (KeyDown(keys, b->l))      p |= SNESRECOMP_PAD_L;
    if (KeyDown(keys, b->r))      p |= SNESRECOMP_PAD_R;
    if (KeyDown(keys, b->start))  p |= SNESRECOMP_PAD_START;
    if (KeyDown(keys, b->select)) p |= SNESRECOMP_PAD_SELECT;

    return p;
}

static void PrintUsage(const SnesRecompHostGame *game, const char *exe) {
    fprintf(stderr,
        "%s\n\n"
        "Usage:\n"
        "  %s                         Open launcher\n"
        "  %s <rom.sfc>               Boot ROM directly (developer path)\n"
        "  %s --launcher              Force launcher even with SkipLauncher=1\n\n"
        "Runtime hotkeys are editable in Launcher -> Settings -> Hotkeys.\n",
        ShortName(game), exe, exe, exe);
}

bool snesrecomp_host_resolve_rom(const SnesRecompHostGame *game,
                                 int argc,
                                 char **argv,
                                 char *rom_path,
                                 size_t rom_path_capacity) {
    const size_t rom_cap = rom_path_capacity;
    const char *config_path = ConfigPath(game);
    char positional_abs[1024];
    positional_abs[0] = '\0';

    bool force_launcher = false;
    const char *positional = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--launcher") == 0) {
            force_launcher = true;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            PrintUsage(game, argv[0]);
            return false;
        } else if (argv[i][0] != '-' && positional == NULL) {
            positional = argv[i];
        }
    }

    /* Resolve relative ROM paths before changing cwd. */
    if (positional) {
        if (!snesrecomp_abspath(positional,
                                positional_abs,
                                sizeof(positional_abs))) {
            fprintf(stderr, "Could not resolve ROM path: %s\n", positional);
            return false;
        }
    }

    if (!snesrecomp_anchor_to_exe_dir()) {
        fprintf(stderr,
            "[host] warning: could not anchor cwd to executable directory\n");
    }
    RtlMigrateLegacySram(game->game_id);


    if (!EnsureDefaultConfig(game, config_path)) {
        fprintf(stderr,
            "[config] warning: could not create default %s\n", config_path);
    }

    ParseConfigFile(config_path);
    if (!EnsureDefaultPlatformConfig(PlatformConfigPath(game))) {
        fprintf(stderr,
            "[config] warning: could not create %s\n",
            PlatformConfigPath(game));
    }
    s_vsync_enabled = ReadIniBool(
        PlatformConfigPath(game), "Video", "VSync", true);

    if (positional_abs[0]) {
        snprintf(rom_path, rom_cap, "%s", positional_abs);
        return true;
    }

    char cached[1024];
    cached[0] = '\0';
    snesrecomp_rom_cache_read(cached, sizeof(cached));

    const char *no_launcher = getenv("SNESRECOMP_NO_LAUNCHER");
    bool want_launcher = !(no_launcher && *no_launcher);

    if (want_launcher && g_config.skip_launcher && !force_launcher &&
        cached[0] && snesrecomp_rom_path_is_verified(game->rom, cached)) {
        snprintf(rom_path, rom_cap, "%s", cached);
        host_report_breadcrumb(
            "launcher skipped (SkipLauncher=1, verified cached ROM)");
        return true;
    }

    if (!want_launcher) {
        if (cached[0] && snesrecomp_rom_path_is_verified(game->rom, cached)) {
            snprintf(rom_path, rom_cap, "%s", cached);
            return true;
        }
        fprintf(stderr,
            "SNESRECOMP_NO_LAUNCHER is set but no verified cached ROM exists.\n");
        return false;
    }

    RecompLauncherCSettings ls;
    memset(&ls, 0, sizeof(ls));
    ls.output_method = g_config.output_method;
    ls.renderer =
        LauncherRendererFromOutputMethod(g_config.output_method);
    ls.window_scale = g_config.window_scale
        ? g_config.window_scale : SNESRECOMP_HOST_DEFAULT_WINDOW_SCALE;
    ls.fullscreen = g_config.fullscreen;
    ls.ignore_aspect = g_config.ignore_aspect_ratio ? 1 : 0;
    ls.linear_filter = g_config.linear_filtering ? 1 : 0;
    ls.widescreen = g_config.widescreen ? 1 : 0;
    ls.enable_audio = g_config.enable_audio ? 1 : 0;
    ls.audio_freq = g_config.audio_freq ? g_config.audio_freq : 32040;
    ls.volume = 100;
    ls.player_src[0] = g_config.enable_gamepad[0]
        ? SNESRECOMP_HOST_PLAYER_INPUT_GAMEPAD
        : SNESRECOMP_HOST_PLAYER_INPUT_KEYBOARD;
    ls.player_src[1] = SNESRECOMP_HOST_PLAYER_INPUT_NONE;
    ls.deadzone[0] = g_config.gamepad_deadzone * 100 / 32767;
    if (ls.deadzone[0] < 0) ls.deadzone[0] = 0;
    if (ls.deadzone[0] > 100) ls.deadzone[0] = 100;
    ls.deadzone[1] = ls.deadzone[0];
    ls.skip_launcher = g_config.skip_launcher ? 1 : 0;
    ls.msu1_enabled = 0;

    RecompLauncherCGameInfo gi;
    memset(&gi, 0, sizeof(gi));
    launcher_profile_apply(Or(game->launcher_profile, "snes"), &gi);

    gi.name = game->display_name;
    gi.region = game->region;
    gi.known_sha1_hex = game->rom->accepted_sha1_hex;
    gi.num_known_sha1 = game->rom->accepted_count;
    gi.sram_path = Or(game->sram_path, "saves/save.srm");
    gi.widescreen_supported = game->widescreen_supported;
    gi.num_players = game->num_players > 0 ? game->num_players : 1;
    gi.msu1_supported = 0;
    gi.config_path = config_path;
    gi.keybinds_path = Or(game->keybinds_path, "keybinds.ini");
    gi.has_renderer = 1;
    gi.renderer_labels = game->renderer_labels
        ? game->renderer_labels : kDefaultRendererLabels;
    gi.num_renderers = game->renderer_labels
        ? game->renderer_count : SNESRECOMP_HOST_RENDERER_COUNT;

    host_report_breadcrumb("launcher: opening recomp-ui");

    char launcher_title[256];
    snprintf(launcher_title, sizeof(launcher_title),
             "%s \xE2\x80\x94 Launcher", game->display_name);

    const int action = recomp_launcher_run_window(
        launcher_title,
        &ls,
        &gi,
        ".",
        cached[0] ? cached : NULL,
        rom_path,
        rom_cap);

    host_report_breadcrumb("launcher: action=%d rom=%s",
        action, rom_path[0] ? rom_path : "(none)");

    if (action == RECOMP_LAUNCHER_RESULT_QUIT)
        return false;

    if (action == RECOMP_LAUNCHER_RESULT_UNAVAILABLE) {
        fprintf(stderr,
            "[launcher] GUI unavailable; trying verified cached ROM.\n");
        if (cached[0] && snesrecomp_rom_path_is_verified(game->rom, cached)) {
            snprintf(rom_path, rom_cap, "%s", cached);
            return true;
        }
        return false;
    }

    if (action != RECOMP_LAUNCHER_RESULT_LAUNCH || !rom_path[0])
        return false;

    g_config.output_method =
        OutputMethodFromLauncherRenderer(ls.renderer);
    g_config.window_scale = (uint8)(ls.window_scale > 0
        ? ls.window_scale : SNESRECOMP_HOST_DEFAULT_WINDOW_SCALE);
    g_config.fullscreen = (uint8)ls.fullscreen;
    g_config.ignore_aspect_ratio = ls.ignore_aspect != 0;
    g_config.linear_filtering = ls.linear_filter != 0;
    g_config.widescreen = ls.widescreen != 0;
    g_config.enable_audio = ls.enable_audio != 0;
    g_config.audio_freq = (uint16)ls.audio_freq;
    g_config.enable_gamepad[0] =
        ls.player_src[0] == SNESRECOMP_HOST_PLAYER_INPUT_GAMEPAD;
    g_config.enable_gamepad[1] = false;
    g_config.gamepad_deadzone = ls.deadzone[0] * 32767 / 100;
    g_config.skip_launcher = ls.skip_launcher != 0;

    s_volume_percent = ls.volume;
    if (s_volume_percent < 0) s_volume_percent = 0;
    if (s_volume_percent > 100) s_volume_percent = 100;

    WriteConfigFile(config_path);

    /* These fields are not persisted by mmx_config.c at this revision. */
    snesrecomp_host_persist_int(game, "Graphics", "Fullscreen",
                                g_config.fullscreen);
    snesrecomp_host_persist_int(game, "Graphics", "IgnoreAspectRatio",
                                g_config.ignore_aspect_ratio ? 1 : 0);

    ConfigReloadKeyMap(config_path);

    snesrecomp_rom_cache_write(rom_path);
    return true;
}
