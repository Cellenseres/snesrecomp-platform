#ifndef SNESRECOMP_PLATFORM_SNES_PPU_IMAGE_CERTIFICATE_H
#define SNESRECOMP_PLATFORM_SNES_PPU_IMAGE_CERTIFICATE_H

#include "snesrecomp_platform/snes_ppu_job.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Exact content certificate for an already submitted logical image.
 * Ordinary owned jobs certify the complete qualified pixel-plane frame, raster
 * metadata, resolved maps/windows, image versions and source/overlay geometry.
 * Their already-composited post images must be part of the retained target.
 * The primitive/indexed
 * variant accepts BG and qualified OBJ-capture primitives, no
 * resolved maps/windows and no post-images. Sliver-source primitives require
 * an additional sliver certificate and are deliberately excluded from the primitive
 * variant. Indexed jobs own exact geometry/material and qualified-sliver versions;
 * both are dependencies alongside the two frame memory certificates. It owns its
 * raster metadata and never retains frames or jobs.
 * A backend commits only after successful submission and invalidates whenever
 * another operation may overwrite the logical render target. Sequence numbers
 * and dirty hints are deliberately not image dependencies. */
typedef struct SnesRecompPpuImageCertificate SnesRecompPpuImageCertificate;
SnesRecompPpuImageCertificate *snesrecomp_ppu_image_certificate_create(void);
void snesrecomp_ppu_image_certificate_destroy(SnesRecompPpuImageCertificate *certificate);
void snesrecomp_ppu_image_certificate_invalidate(SnesRecompPpuImageCertificate *certificate);
bool snesrecomp_ppu_image_certificate_matches(const SnesRecompPpuImageCertificate *certificate,
    const SnesRecompPpuJob *job);
bool snesrecomp_ppu_image_certificate_commit(SnesRecompPpuImageCertificate *certificate,
    const SnesRecompPpuJob *job);

#ifdef __cplusplus
}
#endif
#endif
