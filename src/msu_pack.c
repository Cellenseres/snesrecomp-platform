/* MSU-1 pack policy. See msu_pack.h. */
#include "snesrecomp_platform/msu_pack.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>   /* FindFirstFileA; MSVC has no <dirent.h> */
#  include <direct.h>
#  define msu_mkdir(p) _mkdir(p)
#else
#  include <dirent.h>
#  include <strings.h>   /* strncasecmp */
#  include <sys/types.h>
#  define msu_mkdir(p) mkdir((p), 0775)
#endif

#ifndef S_ISDIR
#  define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif

#define MSU_ENV "SNESRECOMP_MSU1"

static SnesRecompMsuStatus s_status;

/* helpers */

static bool eq_nocase(const char *a, const char *b) {
    if (!a || !b) return false;
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
    }
    return *a == 0 && *b == 0;
}

static const char *skip_spaces(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    return p;
}

static void trim_trailing(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' ||
                 s[n-1] == '\r' || s[n-1] == '\n'))
        s[--n] = 0;
}

static bool copy_path(char *out, size_t capacity, const char *text) {
    if (!out || capacity == 0) return false;
    const int n = snprintf(out, capacity, "%s", text ? text : "");
    return n >= 0 && (size_t)n < capacity;
}

static bool is_directory(const char *path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool set_env(const char *name, const char *value) {
#ifdef _WIN32
    return _putenv_s(name, value) == 0;
#else
    return setenv(name, value, 1) == 0;
#endif
}

/* setting */

SnesRecompMsuMode snesrecomp_msu_parse_mode(const char *text) {
    if (!text) return SNESRECOMP_MSU_AUTO;
    const char *p = skip_spaces(text);
    if (eq_nocase(p, "off") || eq_nocase(p, "0") || eq_nocase(p, "no") ||
        eq_nocase(p, "false") || eq_nocase(p, "disabled"))
        return SNESRECOMP_MSU_OFF;
    if (eq_nocase(p, "on") || eq_nocase(p, "1") || eq_nocase(p, "yes") ||
        eq_nocase(p, "true") || eq_nocase(p, "enabled"))
        return SNESRECOMP_MSU_ON;
    return SNESRECOMP_MSU_AUTO;
}

const char *snesrecomp_msu_mode_name(SnesRecompMsuMode mode) {
    switch (mode) {
        case SNESRECOMP_MSU_OFF: return "off";
        case SNESRECOMP_MSU_ON:  return "on";
        default:                 return "auto";
    }
}

/* default directory */

bool snesrecomp_msu_default_directory(const char *anchor_directory,
                                      char *out,
                                      size_t capacity) {
    if (!out || capacity == 0) return false;

    if (anchor_directory && anchor_directory[0]) {
        const size_t len = strlen(anchor_directory);
        const char *sep = (anchor_directory[len-1] == '/' ||
                           anchor_directory[len-1] == '\\') ? "" : "/";
        const int n = snprintf(out, capacity, "%s%smsu", anchor_directory, sep);
        return n >= 0 && (size_t)n < capacity;
    }

    /* Desktop boot chdir's to the exe folder, so relative lands right. */
    return copy_path(out, capacity, "msu");
}

void snesrecomp_msu_ensure_directory(const char *directory) {
    if (!directory || !directory[0] || is_directory(directory)) return;
    (void)msu_mkdir(directory);
}

/* pack discovery
 *
 * msu1.c scans too, but only after arming. We need it before, to decide
 * whether to arm at all. */

/* "<base>-<N>.pcm" -> "<base>". Only the final "-<digits>.pcm" is stripped,
 * so a base may contain '-'. */
static bool match_track_name(const char *name, char *out, size_t capacity) {
    const size_t len = strlen(name);
    if (len < 7) return false;                       /* "x-1.pcm" */
    if (name[len-4] != '.' ||
        tolower((unsigned char)name[len-3]) != 'p' ||
        tolower((unsigned char)name[len-2]) != 'c' ||
        tolower((unsigned char)name[len-1]) != 'm')
        return false;

    size_t i = len - 4;
    if (i == 0 || !isdigit((unsigned char)name[i-1])) return false;
    while (i > 0 && isdigit((unsigned char)name[i-1])) i--;
    if (i == 0 || name[i-1] != '-') return false;

    const size_t base_len = i - 1;
    if (base_len == 0 || base_len >= capacity) return false;
    memcpy(out, name, base_len);
    out[base_len] = 0;
    return true;
}

enum { MSU_MAX_CANDIDATES = 16 };

typedef struct PackCandidate {
    char base[SNESRECOMP_MSU_PATH_MAX];
    int  count;
} PackCandidate;

static void tally(PackCandidate *cand, int *ncand, const char *base) {
    for (int i = 0; i < *ncand; i++) {
        if (strcmp(cand[i].base, base) == 0) { cand[i].count++; return; }
    }
    if (*ncand >= MSU_MAX_CANDIDATES) return;
    snprintf(cand[*ncand].base, sizeof(cand[*ncand].base), "%s", base);
    cand[*ncand].count = 1;
    (*ncand)++;
}

bool snesrecomp_msu_scan_pack(const char *directory,
                              char *base_out,
                              size_t base_capacity,
                              int *track_count) {
    if (track_count) *track_count = 0;
    if (base_out && base_capacity) base_out[0] = 0;
    if (!is_directory(directory)) return false;

    char dir[SNESRECOMP_MSU_PATH_MAX];
    if (!copy_path(dir, sizeof(dir), directory)) return false;
    size_t dlen = strlen(dir);
    while (dlen && (dir[dlen-1] == '/' || dir[dlen-1] == '\\')) dir[--dlen] = 0;

    PackCandidate cand[MSU_MAX_CANDIDATES];
    int ncand = 0;
    char base[SNESRECOMP_MSU_PATH_MAX];

#ifdef _WIN32
    char pattern[SNESRECOMP_MSU_PATH_MAX + 8];
    if (snprintf(pattern, sizeof(pattern), "%s\\*.pcm", dir) >=
        (int)sizeof(pattern))
        return false;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (match_track_name(fd.cFileName, base, sizeof(base)))
            tally(cand, &ncand, base);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return false;
    for (struct dirent *e = readdir(d); e; e = readdir(d)) {
        if (match_track_name(e->d_name, base, sizeof(base)))
            tally(cand, &ncand, base);
    }
    closedir(d);
#endif

    int best = -1;
    for (int i = 0; i < ncand; i++)
        if (best < 0 || cand[i].count > cand[best].count) best = i;
    if (best < 0) return false;

    if (track_count) *track_count = cand[best].count;
    if (base_out && base_capacity) {
        const int n =
            snprintf(base_out, base_capacity, "%s/%s", dir, cand[best].base);
        if (n < 0 || (size_t)n >= base_capacity) return false;
    }
    return true;
}

/* config.ini */

static bool parse_section(const char *line, char *out, size_t capacity) {
    const char *p = skip_spaces(line);
    if (*p != '[') return false;
    p++;
    const char *end = strchr(p, ']');
    if (!end) return false;
    const size_t n = (size_t)(end - p);
    if (n >= capacity) return false;
    memcpy(out, p, n);
    out[n] = 0;
    trim_trailing(out);
    return true;
}

/* "Key = value" -> value, case-insensitive. */
static const char *parse_value(const char *line, const char *key) {
    const char *p = skip_spaces(line);
    const size_t klen = strlen(key);
#ifdef _WIN32
    if (_strnicmp(p, key, klen) != 0) return NULL;
#else
    if (strncasecmp(p, key, klen) != 0) return NULL;
#endif
    p = skip_spaces(p + klen);
    if (*p != '=') return NULL;
    return skip_spaces(p + 1);
}

bool snesrecomp_msu_read_settings(const char *ini_path,
                                  SnesRecompMsuMode *mode,
                                  char *directory,
                                  size_t directory_capacity) {
    if (directory && directory_capacity) directory[0] = 0;
    if (!ini_path || !ini_path[0]) return false;

    FILE *f = fopen(ini_path, "rb");
    if (!f) return false;

    char section[128] = "";
    char line[SNESRECOMP_MSU_PATH_MAX + 128];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        trim_trailing(line);
        if (parse_section(line, section, sizeof(section))) continue;
        const char *p = skip_spaces(line);
        if (*p == '#' || *p == ';' || *p == 0) continue;
        if (!eq_nocase(section, SNESRECOMP_MSU_INI_SECTION)) continue;

        const char *value = parse_value(line, SNESRECOMP_MSU_INI_KEY_MODE);
        if (value) {
            char text[64];
            snprintf(text, sizeof(text), "%s", value);
            trim_trailing(text);
            if (mode) *mode = snesrecomp_msu_parse_mode(text);
            found = true;
            continue;
        }
        value = parse_value(line, SNESRECOMP_MSU_INI_KEY_DIR);
        if (value && directory && directory_capacity) {
            snprintf(directory, directory_capacity, "%s", value);
            trim_trailing(directory);
            found = true;
        }
    }
    fclose(f);
    return found;
}

/* resolve */

const SnesRecompMsuStatus *snesrecomp_msu_status(void) {
    return &s_status;
}

const SnesRecompMsuStatus *snesrecomp_msu_resolve(
    const SnesRecompMsuRequest *request) {
    SnesRecompMsuRequest req;
    memset(&req, 0, sizeof(req));
    req.mode = SNESRECOMP_MSU_AUTO;
    if (request) req = *request;

    memset(&s_status, 0, sizeof(s_status));
    s_status.mode = SNESRECOMP_MSU_AUTO;
    s_status.driver_present = req.driver_present;

    char configured_dir[SNESRECOMP_MSU_PATH_MAX] = "";
    snesrecomp_msu_read_settings(req.ini_path, &s_status.mode, configured_dir,
                                 sizeof(configured_dir));
    if (req.has_mode) s_status.mode = req.mode;

    const char *chosen = NULL;
    if (req.directory && req.directory[0]) chosen = req.directory;
    else if (configured_dir[0]) chosen = configured_dir;

    if (chosen)
        copy_path(s_status.directory, sizeof(s_status.directory), chosen);
    else
        snesrecomp_msu_default_directory(req.anchor_directory,
                                         s_status.directory,
                                         sizeof(s_status.directory));

    /* No folder, no scan, no environment. */
    if (s_status.mode == SNESRECOMP_MSU_OFF) {
        s_status.reason = "off by setting; original soundtrack";
        return &s_status;
    }

    snesrecomp_msu_ensure_directory(s_status.directory);
    s_status.directory_exists = is_directory(s_status.directory);
    s_status.pack_found = snesrecomp_msu_scan_pack(s_status.directory,
                                                   s_status.pack_base,
                                                   sizeof(s_status.pack_base),
                                                   &s_status.track_count);

    if (!s_status.pack_found) {
        /* The common case, not an error. */
        s_status.reason = "no pack; original soundtrack";
        return &s_status;
    }
    if (!s_status.driver_present) {
        /* Nothing would write $2004/$2007, so a pack would only be silence. */
        s_status.reason = "pack found, but this build has no MSU-1 driver; "
                          "original soundtrack";
        return &s_status;
    }

    /* The base, not the folder, so the log names what plays. */
    s_status.armed = set_env(MSU_ENV, s_status.pack_base);
    s_status.reason = s_status.armed
        ? "pack armed"
        : "could not set the environment; original soundtrack";
    return &s_status;
}
