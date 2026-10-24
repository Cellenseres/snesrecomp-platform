#include "snesrecomp_platform/ppu_blit.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool Images(const SnesRecompPpuJobImage *images, unsigned count) {
    if (!images || !count || count > SNESRECOMP_BLIT_MAX_IMAGES) return false;
    for (unsigned i = 0; i < count; ++i) {
        const SnesRecompPpuJobImage *s = images + i;
        if (!s->pixels || !s->width || s->width > 512 || !s->height || s->height > 240 ||
            s->pitch < s->width || s->pitch > SIZE_MAX / sizeof(uint32_t) / s->height)
            return false;
    }
    return true;
}
bool snesrecomp_blit_compile(const uint32_t *map, uint32_t pitch,
    const SnesRecompPpuJobImage *images, uint16_t image_count,
    uint16_t width, uint16_t height, SnesRecompBlitRect *rects,
    uint32_t capacity, uint32_t *count) {
    if (!count) return false;
    *count = 0;
    if (!map || !rects || !width || width > 512 || !height || height > 240 ||
        pitch < width || pitch > SIZE_MAX / sizeof(uint32_t) / height ||
        !capacity || capacity > SNESRECOMP_BLIT_MAX_RECTS || !Images(images, image_count))
        return false;
    uint32_t previous[512], current[512], previous_count = 0, used = 0;
    for (unsigned y = 0; y < height; ++y) {
        const uint32_t *row = map + (size_t)y * pitch;
        unsigned current_count = 0, previous_at = 0;
        for (unsigned x = 0; x < width;) {
            const uint32_t sample = row[x];
            if (sample == SNESRECOMP_BLIT_EMPTY) { ++x; continue; }
            const unsigned image = sample >> 17, sx = sample & 511u, sy = (sample >> 9) & 255u;
            if (image >= image_count || sx >= images[image].width || sy >= images[image].height)
                return false;
            unsigned length = 1;
            while (x + length < width && sx + length < images[image].width &&
                   row[x + length] == sample + length) ++length;
            while (previous_at < previous_count && rects[previous[previous_at]].x < x)
                ++previous_at;
            uint32_t index = used;
            if (previous_at < previous_count) {
                const uint32_t old = previous[previous_at];
                SnesRecompBlitRect *r = rects + old;
                if (r->x == x && r->width == length && r->image == image && r->source_x == sx &&
                    r->source_y + r->height == sy && r->y + r->height == y) {
                    ++r->height;
                    index = old;
                }
            }
            if (index == used) {
                if (used == capacity) return false;
                rects[used++] = (SnesRecompBlitRect){(uint16_t)x, (uint16_t)y,
                    (uint16_t)length, 1, (uint16_t)sx, (uint16_t)sy, (uint8_t)image, {0}};
            }
            current[current_count++] = index;
            x += length;
        }
        memcpy(previous, current, current_count * sizeof *current);
        previous_count = current_count;
    }
    *count = used;
    return true;
}

typedef struct BlitRun {
    uint32_t sample;
    uint16_t x, width;
} BlitRun;
struct SnesRecompBlitCache {
    uint16_t width, height, counts[SNESRECOMP_BLIT_MAX_HEIGHT];
    bool valid;
    BlitRun runs[];
};
SnesRecompBlitCache *snesrecomp_blit_cache_create(uint16_t width, uint16_t height) {
    if (!width || width > SNESRECOMP_BLIT_MAX_WIDTH ||
        !height || height > SNESRECOMP_BLIT_MAX_HEIGHT) return NULL;
    SnesRecompBlitCache *c = calloc(1, sizeof *c + (size_t)width * height * sizeof(BlitRun));
    if (c) { c->width = width; c->height = height; }
    return c;
}
void snesrecomp_blit_cache_destroy(SnesRecompBlitCache *cache) { free(cache); }
bool snesrecomp_blit_cache_compile(SnesRecompBlitCache *c,
    const uint32_t *map, uint32_t pitch, const bool *dirty,
    const SnesRecompPpuJobImage *images, uint16_t image_count,
    SnesRecompBlitRect *rects, uint32_t capacity, uint32_t *count) {
    if (!count) { if (c) c->valid = false; return false; }
    *count = 0;
    if (!c) return false;
    const bool valid = c->valid;
    c->valid = false;
    if (!map || !rects || pitch < c->width ||
        pitch > SIZE_MAX / sizeof(uint32_t) / c->height || !capacity ||
        capacity > SNESRECOMP_BLIT_MAX_RECTS || !Images(images, image_count)) return false;
    uint32_t previous[512], current[512], previous_count = 0, used = 0;
    for (unsigned y = 0; y < c->height; ++y) {
        BlitRun *runs = c->runs + (size_t)y * c->width;
        if (!valid || !dirty || dirty[y]) {
            const uint32_t *row = map + (size_t)y * pitch;
            unsigned n = 0;
            for (unsigned x = 0; x < c->width;) {
                const uint32_t sample = row[x];
                if (sample == SNESRECOMP_BLIT_EMPTY) { ++x; continue; }
                const unsigned image = sample >> 17, sx = sample & 511u;
                if (image >= image_count || sx >= images[image].width ||
                    ((sample >> 9) & 255u) >= images[image].height) return false;
                unsigned length = 1;
                while (x + length < c->width && sx + length < images[image].width &&
                       row[x + length] == sample + length) ++length;
                runs[n++] = (BlitRun){sample, (uint16_t)x, (uint16_t)length};
                x += length;
            }
            c->counts[y] = (uint16_t)n;
        }
        unsigned previous_at = 0;
        for (unsigned i = 0; i < c->counts[y]; ++i) {
            const BlitRun *run = runs + i;
            const unsigned image = run->sample >> 17, sx = run->sample & 511u;
            const unsigned sy = (run->sample >> 9) & 255u;
            if (image >= image_count || sx + run->width > images[image].width ||
                sy >= images[image].height) return false;
            while (previous_at < previous_count && rects[previous[previous_at]].x < run->x)
                ++previous_at;
            uint32_t index = used;
            if (previous_at < previous_count) {
                const uint32_t old = previous[previous_at];
                SnesRecompBlitRect *r = rects + old;
                if (r->x == run->x && r->width == run->width && r->image == image &&
                    r->source_x == sx && r->source_y + r->height == sy && r->y + r->height == y) {
                    ++r->height; index = old;
                }
            }
            if (index == used) {
                if (used == capacity) return false;
                rects[used++] = (SnesRecompBlitRect){run->x, (uint16_t)y,
                    run->width, 1, (uint16_t)sx, (uint16_t)sy, (uint8_t)image, {0}};
            }
            current[i] = index;
        }
        previous_count = c->counts[y];
        memcpy(previous, current, previous_count * sizeof *current);
    }
    c->valid = true; *count = used;
    return true;
}
bool snesrecomp_blit_validate(const SnesRecompBlitPlan *p) {
    if (!p || !p->width || p->width > 512 || !p->height || p->height > 240 ||
        p->reserved || p->rect_count > SNESRECOMP_BLIT_MAX_RECTS ||
        (p->rect_count && !p->rects) || !Images(p->images, p->image_count)) return false;
    for (unsigned i = 0; i < p->rect_count; ++i) {
        const SnesRecompBlitRect *r = p->rects + i;
        if (r->image >= p->image_count || !r->width || !r->height ||
            r->reserved[0] || r->reserved[1] || r->reserved[2] ||
            (unsigned)r->x + r->width > p->width || (unsigned)r->y + r->height > p->height)
            return false;
        const SnesRecompPpuJobImage *s = p->images + r->image;
        if ((unsigned)r->source_x + r->width > s->width ||
            (unsigned)r->source_y + r->height > s->height) return false;
    }
    return true;
}
static bool Apply(const SnesRecompBlitPlan *p, uint32_t *argb, uint32_t pitch,
                  bool opaque) {
    if (!argb || !snesrecomp_blit_validate(p) || pitch < p->width ||
        pitch > SIZE_MAX / sizeof *argb / p->height) return false;
    const uintptr_t destination = (uintptr_t)argb;
    const size_t bytes = ((size_t)(p->height - 1u) * pitch + p->width) * sizeof *argb;
    if (destination > UINTPTR_MAX - bytes) return false;
    for (unsigned i = 0; i < p->image_count; ++i) {
        const SnesRecompPpuJobImage *s = p->images + i;
        const uintptr_t source = (uintptr_t)s->pixels;
        const size_t size = ((size_t)(s->height - 1u) * s->pitch + s->width) * sizeof *argb;
        if (source > UINTPTR_MAX - size || (source < destination + bytes && destination < source + size))
            return false;
    }
    for (unsigned i = 0; i < p->rect_count; ++i) {
        const SnesRecompBlitRect *r = p->rects + i;
        const SnesRecompPpuJobImage *s = p->images + r->image;
        for (unsigned y = 0; y < r->height; ++y) {
            uint32_t *dst = argb + (size_t)(r->y + y) * pitch + r->x;
            const uint32_t *src = s->pixels + (size_t)(r->source_y + y) * s->pitch + r->source_x;
            if (opaque)
                for (unsigned x = 0; x < r->width; ++x) dst[x] = src[x] | UINT32_C(0xff000000);
            else memcpy(dst, src, (size_t)r->width * sizeof *dst);
        }
    }
    return true;
}
bool snesrecomp_blit_apply(const SnesRecompBlitPlan *p, uint32_t *argb, uint32_t pitch) {
    return Apply(p, argb, pitch, true);
}
bool snesrecomp_blit_copy(const SnesRecompBlitPlan *p, uint32_t *argb, uint32_t pitch) {
    return Apply(p, argb, pitch, false);
}
