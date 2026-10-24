#ifndef SNESRECOMP_PLATFORM_SNES_PPU_JOB_TYPES_H
#define SNESRECOMP_PLATFORM_SNES_PPU_JOB_TYPES_H
#include <stdint.h>
/* Enum-free payloads may cross a console SDK's short-enum C boundary. */
enum {
    SNESRECOMP_PPU_JOB_MAX_IMAGES = 8,
    SNESRECOMP_PPU_JOB_MAX_OVERLAYS = 32,
    SNESRECOMP_PPU_JOB_MAX_SOURCES = 512,
    SNESRECOMP_PPU_JOB_SOURCES_PER_ROW = 8,
    SNESRECOMP_PPU_JOB_MAX_PRIMITIVES = 32768,
    SNESRECOMP_PPU_JOB_MAX_MATERIALS = 32,
    SNESRECOMP_PPU_PRIMITIVE_BG = 0,
    SNESRECOMP_PPU_PRIMITIVE_OBJ = 1,
    SNESRECOMP_PPU_PRIMITIVE_IMAGE = 2,
    SNESRECOMP_PPU_PRIMITIVE_OBJ_CAPTURE = 3,
    SNESRECOMP_PPU_MATERIAL_SOURCE_COLOUR = 1,
    SNESRECOMP_PPU_MATERIAL_BASE_COLOUR = 2,
    SNESRECOMP_PPU_MATERIAL_PALETTE_COLOUR = 3,
    SNESRECOMP_PPU_OVERLAY_PREMULTIPLIED = 0,
    SNESRECOMP_PPU_OVERLAY_STRAIGHT = 1,
    SNESRECOMP_PPU_OVERLAY_REPLACE = 2,
    SNESRECOMP_PPU_OVERLAY_ALWAYS = 0,
    SNESRECOMP_PPU_OVERLAY_KEY_OR_BLACK = 1
};
/* Host-resolved, unscaled ARGB8888 art. Padding is not pixel data. */
typedef struct SnesRecompPpuJobImage {
    const uint32_t *pixels;
    uint32_t pitch; /* Pixels, not bytes. */
    uint16_t width, height;
} SnesRecompPpuJobImage;
typedef struct SnesRecompPpuOverlay {
    uint16_t x, y, width, height, source_x, source_y;
    /* Dim each source RGB byte by (15-reduction)/15, with integer truncation,
     * before blending. Alpha is unchanged. Zero preserves legacy job output.
     * Backends must decline unsupported colour formats before submission. */
    uint8_t image, blend, condition, brightness_reduction;
    uint32_t key_argb;
} SnesRecompPpuOverlay;
/* A resolved replacement inside a BG plane, before main/sub selection and
 * RGB5 colour mathematics. Transparent image texels erase the replaced BG,
 * rather than revealing its repeated native tiles. Other BGs/OBJ are intact.
 * source_width/height are either destination size (one-to-one) or one (edge
 * clamp). Alpha is binary coverage: zero transparent, any other value opaque.
 * Priority is the original BG low/high bit. Screens: bit0 main, bit1 sub.
 * Rectangles on the same layer/screen must be disjoint. */
typedef struct SnesRecompPpuSource {
    uint16_t x, y, width, height, source_x, source_y, source_width, source_height;
    uint8_t image, layer, screens, priority;
} SnesRecompPpuSource;

/* Native pixel coordinates before scaling. Rectangles do not resample. BG
 * sources name a layer; OBJ sources name an already qualified eight-pixel
 * sliver in graphics_frame; IMAGE sources name owned job art. OBJ_CAPTURE
 * names source zero, an already qualified constant-Z overlay run. Its colour
 * index is in priority; graphics_frame retains the original base main/sub
 * context after capture removal. owner is opaque consumer association metadata.
 * It never causes another sprite evaluation.
 * No selection,
 * live PPU reads or guest callbacks occur during primitive rendering.
 *
 * Priority is the COMPLETE original unsigned Z value, including colour
 * index. Transparent pixels do not write depth. Equal depth uses later
 * primitive order. OBJ pixel winners must be resolved in original OAM order
 * before applying this depth test; BG priority grouping cannot change them.
 *
 * Source colour/window/math context follows source_x/y, not destination_x/y.
 * A rectangle crossing a raster band is split by the consumer. Source bounds
 * and qualification are validated before any GPU work is submitted. */
typedef struct SnesRecompPpuPrimitive {
    uint16_t source_x, source_y, x, y, width, height;
    uint16_t priority, material, source, owner;
    uint8_t kind, reserved_byte;
    uint16_t reserved;
} SnesRecompPpuPrimitive;

/* SOURCE_COLOUR isolates main_enable while preserving the source band's
 * subscreen, windows, RGB5 mathematics and brightness. occluding_bg is either
 * a source-context BG layer or 255 (no additional visibility test). Native
 * source visibility requires priority strictly above that BG's original Z.
 * PALETTE_COLOUR uses the qualified palette index and source brightness only;
 * the consumer selects it for captures preceding window/math composition.
 * BASE_COLOUR resolves the captured base's selected main/sub pixels at the
 * original source, using main_enable as a mask. The primitive still supplies
 * coverage and depth; its captured palette is not the base colour operand.
 * IMAGE materials have flags/main_enable zero and occluding_bg 255; their
 * brightness_reduction truncates each RGB byte by (15-reduction)/15.
 * All reserved fields must be zero; payloads contain no SDK enums. */
typedef struct SnesRecompPpuMaterial {
    uint8_t main_enable, occluding_bg, brightness_reduction, flags;
    uint32_t reserved;
} SnesRecompPpuMaterial;
#endif
