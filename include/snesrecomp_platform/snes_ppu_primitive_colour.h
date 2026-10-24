#ifndef SNESRECOMP_PLATFORM_PPU_PRIMITIVE_COLOUR_H
#define SNESRECOMP_PLATFORM_PPU_PRIMITIVE_COLOUR_H
#include "snesrecomp_platform/snes_ppu_job.h"
#include "snesrecomp_platform/abi.h"
#ifdef __cplusplus
extern "C" {
#endif
enum {
    SNESRECOMP_PRIMITIVE_COLOUR_MATH=1,
    SNESRECOMP_PRIMITIVE_COLOUR_SUBSCREEN=2,
    SNESRECOMP_PRIMITIVE_COLOUR_SUBTRACT=4,
    SNESRECOMP_PRIMITIVE_COLOUR_HALF=8,
    SNESRECOMP_PRIMITIVE_COLOUR_BASE=16,
    SNESRECOMP_PRIMITIVE_COLOUR_LIMIT=65536
};
/* Ready for native vertices: source-context UV, destination rectangle, exact
 * RGB5 main colour and fixed operand. A genuine subscreen operand is sampled
 * at source_x/y, never at the relocated destination. Geometry is opaque and
 * constant original Z; the consumer has already qualified its native indices.
 * Commands preserve primitive order, including equal-depth ties. */
typedef struct SnesRecompPpuPrimitiveColour {
    uint16_t source_x,source_y,x,y,width,height,priority,colour,fixed_colour;
    uint8_t flags,brightness,main_enable,reserved_byte;
    uint16_t reserved;
} SnesRecompPpuPrimitiveColour;
SNESRECOMP_STATIC_ASSERT(sizeof(SnesRecompPpuPrimitiveColour)==24,"primitive colour SDK boundary");
typedef struct SnesRecompPpuPrimitiveColourPlan {
    const SnesRecompPpuPrimitiveColour *commands;
    uint32_t count;
    uint64_t generation;
    uint16_t width,height;
} SnesRecompPpuPrimitiveColourPlan;
typedef struct SnesRecompPpuPrimitiveColourCache SnesRecompPpuPrimitiveColourCache;
typedef struct SnesRecompPpuPrimitiveColourStats {
    uint64_t prepares, cache_hits, generated_commands, reused_primitives;
    uint64_t retained_copy_calls, retained_copy_bytes;
} SnesRecompPpuPrimitiveColourStats;
SnesRecompPpuPrimitiveColourCache *snesrecomp_ppu_primitive_colour_create(uint32_t capacity);
void snesrecomp_ppu_primitive_colour_destroy(SnesRecompPpuPrimitiveColourCache *cache);
void snesrecomp_ppu_primitive_colour_stats(const SnesRecompPpuPrimitiveColourCache *cache,
    SnesRecompPpuPrimitiveColourStats *stats);
/* No pixels, guest work, GPU writes or allocation. Output is empty on failure.
 * Precompute windows once per band; palette changes split only affected runs. */
SnesRecompPpuResult snesrecomp_ppu_primitive_colour_prepare(
    SnesRecompPpuPrimitiveColourCache *cache,const SnesRecompPpuJob *job,
    SnesRecompPpuPrimitiveColourPlan *plan);
/* Exact CPU oracle for a prepared command and an ORIGINAL RGB5 subscreen
 * operand. This tests composition, not sprite selection or main BG ranking. */
uint32_t snesrecomp_ppu_primitive_colour_sample(const SnesRecompPpuPrimitiveColour *command,
    uint16_t sub_colour,bool genuine_subscreen,uint32_t base_argb);
#ifdef __cplusplus
}
#endif
#endif
