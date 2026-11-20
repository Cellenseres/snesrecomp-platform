#include "snesrecomp_platform/snes_ppu_image_certificate.h"
#include "snesrecomp_platform/memory_compare.h"
#include "snesrecomp_platform/snes_ppu_frame_key.h"
#include <stdlib.h>
#include <string.h>

typedef struct ImageFrame {
    SnesRecompPpuFrameVersions versions;
    SnesPpuLayoutState layout;
    SnesPpuRasterBand bands[SNES_PPU_MAX_BANDS];
    uint16_t native_width, width, extra, height, band_count, palette_lines;
} ImageFrame;

struct SnesRecompPpuImageCertificate {
    ImageFrame base, graphics;
    uint64_t owner, geometry, indexed_objects;
    uint32_t primitives;
    uint16_t materials;
    bool valid, indexed, resolved;
    SnesRecompPpuFrameKey *resolved_frame;
    uint64_t image_versions[SNESRECOMP_PPU_JOB_MAX_IMAGES];
    uint16_t images, overlays, sources;
};

static bool Admitted(const SnesRecompPpuJobView *v) {
    if(!v || (!v->indexed_layout && !v->primitive_count) || v->image_count || v->source_count ||
       v->overlay_count || (v->blits && v->blits->rect_count)) return false;
    const SnesRecompPpuFrame *frames[]={v->frame,v->graphics_frame};
    for(unsigned i=0;i<2;++i) {
        const SnesPpuFrameCapture *c=snesrecomp_ppu_frame_capture(frames[i]);
        if(!c || !snesrecomp_ppu_frame_versions(frames[i]) || (!v->indexed_layout && !c->obj_pixels) ||
           c->raster_memory_flags || c->resolved_windows || c->resolved_bg_required ||
           !c->band_count || c->band_count>SNES_PPU_MAX_BANDS) return false;
        for(unsigned bg=0;bg<SNES_PPU_BG_COUNT;++bg)
            if(c->resolved_bg[bg]) return false;
    }
    return true;
}

/* Completed ordinary jobs include their post overlays and resolved BG art.
 * Freeze owns every byte and advances geometry/image versions transactionally.
 * Sliver-backed frames remain excluded by the full frame certificate. */
static bool ResolvedAdmitted(const SnesRecompPpuJobView *v) {
    return v && v->frame && !v->graphics_frame && !v->indexed_layout &&
        !v->primitive_count && !v->material_count && !v->blits &&
        v->image_count<=SNESRECOMP_PPU_JOB_MAX_IMAGES &&
        (!v->image_count || v->image_versions);
}
static bool Matches(const ImageFrame *image,const SnesRecompPpuFrame *frame) {
    const SnesPpuFrameCapture *c=snesrecomp_ppu_frame_capture(frame);
    const SnesRecompPpuFrameVersions *v=snesrecomp_ppu_frame_versions(frame);
    return image->versions.owner==v->owner && image->versions.vram==v->vram &&
        image->versions.palette==v->palette && image->versions.objects==v->objects &&
        image->native_width==c->native_width && image->width==c->canvas_width &&
        image->extra==c->canvas_extra && image->height==c->visible_height &&
        image->band_count==c->band_count && image->palette_lines==c->cgram_line_count &&
        snesrecomp_u8_equal((const uint8_t *)&image->layout,(const uint8_t *)&c->layout,sizeof c->layout) &&
        snesrecomp_u8_equal((const uint8_t *)image->bands,(const uint8_t *)c->bands,c->band_count*sizeof(*c->bands));
}

static void Record(ImageFrame *image,const SnesRecompPpuFrame *frame) {
    const SnesPpuFrameCapture *c=snesrecomp_ppu_frame_capture(frame);
    image->versions=*snesrecomp_ppu_frame_versions(frame);
    image->layout=c->layout;
    memcpy(image->bands,c->bands,c->band_count*sizeof(*c->bands));
    image->native_width=c->native_width;image->width=c->canvas_width;
    image->extra=c->canvas_extra;image->height=c->visible_height;
    image->band_count=(uint16_t)c->band_count;image->palette_lines=(uint16_t)c->cgram_line_count;
}

SnesRecompPpuImageCertificate *snesrecomp_ppu_image_certificate_create(void) {
    SnesRecompPpuImageCertificate *c=calloc(1,sizeof *c);
    if(c) c->resolved_frame=snesrecomp_ppu_frame_key_create();
    return c;
}
void snesrecomp_ppu_image_certificate_destroy(SnesRecompPpuImageCertificate *c) {
    if(c) {snesrecomp_ppu_frame_key_destroy(c->resolved_frame);free(c);}
}
void snesrecomp_ppu_image_certificate_invalidate(SnesRecompPpuImageCertificate *c) {if(c)c->valid=false;}
bool snesrecomp_ppu_image_certificate_matches(const SnesRecompPpuImageCertificate *c,
    const SnesRecompPpuJob *job) {
    const SnesRecompPpuJobView *v=snesrecomp_ppu_job_view(job);
    if(c && c->valid && c->resolved) {
        if(!ResolvedAdmitted(v) || c->owner!=v->owner ||
            c->geometry!=v->geometry_version || c->images!=v->image_count ||
            c->overlays!=v->overlay_count || c->sources!=v->source_count ||
            !snesrecomp_ppu_frame_key_matches(c->resolved_frame,v->frame)) return false;
        for(unsigned i=0;i<v->image_count;++i)
            if(c->image_versions[i]!=v->image_versions[i]) return false;
        return true;
    }
    return c && c->valid && Admitted(v) && c->owner==v->owner &&
        c->indexed==(v->indexed_layout!=NULL) &&
        c->geometry==(c->indexed?v->indexed_geometry_version:v->primitive_geometry_version) &&
        (!c->indexed || c->indexed_objects==v->indexed_obj_version) && c->primitives==v->primitive_count &&
        c->materials==v->material_count && Matches(&c->base,v->frame) && Matches(&c->graphics,v->graphics_frame);
}
bool snesrecomp_ppu_image_certificate_commit(SnesRecompPpuImageCertificate *c,
    const SnesRecompPpuJob *job) {
    const SnesRecompPpuJobView *v=snesrecomp_ppu_job_view(job);
    if(!c) return false;
    if(ResolvedAdmitted(v)) {
        c->valid=false;
        if(!snesrecomp_ppu_frame_key_commit(c->resolved_frame,v->frame)) return false;
        c->owner=v->owner;c->geometry=v->geometry_version;
        c->images=v->image_count;c->overlays=v->overlay_count;c->sources=v->source_count;
        for(unsigned i=0;i<v->image_count;++i)c->image_versions[i]=v->image_versions[i];
        c->resolved=c->valid=true;return true;
    }
    c->resolved=false;
    if(!Admitted(v)) {c->valid=false;return false;}
    /* Pixel-plane versions intentionally do not certify the optional sliver
     * list. Scan kinds only when committing; matching owned geometry versions
     * then certify this restriction without scanning every warm primitive. */
    for(uint32_t i=0;i<v->primitive_count;++i)
        if(v->primitives[i].kind!=SNESRECOMP_PPU_PRIMITIVE_BG &&
           v->primitives[i].kind!=SNESRECOMP_PPU_PRIMITIVE_OBJ_CAPTURE) {
            c->valid=false;return false;
        }
    Record(&c->base,v->frame);Record(&c->graphics,v->graphics_frame);
    c->indexed=v->indexed_layout!=NULL;
    c->owner=v->owner;c->geometry=c->indexed?v->indexed_geometry_version:v->primitive_geometry_version;
    c->indexed_objects=v->indexed_obj_version;
    c->primitives=v->primitive_count;c->materials=v->material_count;c->valid=true;
    return true;
}
