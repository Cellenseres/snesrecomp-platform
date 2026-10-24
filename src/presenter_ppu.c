#include "snesrecomp_platform/presenter_backend.h"
#include <string.h>

static SnesRecompPpuResult Record(SnesRecompPresenter *p, SnesRecompPpuResult r) {
    if ((unsigned)r >= SNESRECOMP_PPU_RESULT_COUNT)
        r = SNESRECOMP_PPU_BACKEND_ERROR;
    if (p) {
        p->ppu_stats.last_result = r;
        if (r != SNESRECOMP_PPU_OK) ++p->ppu_stats.rejected[r];
    }
    return r;
}

uint32_t snesrecomp_presenter_ppu_capabilities(const SnesRecompPresenter *p) {
    return p && p->ops && p->ops->ppu_capabilities && p->ops->ppu_prepare &&
        p->ops->ppu_submit && p->ops->ppu_cancel ? p->ops->ppu_capabilities(p) : 0;
}

bool snesrecomp_presenter_ppu_begin_frame(SnesRecompPresenter *p) {
    if (!p || p->ppu_pending) return false;
    p->ppu_stats.capture_us = p->ppu_stats.cache_us = p->ppu_stats.submit_us = 0;
    p->ppu_stats.complete_us = p->ppu_stats.present_us = p->ppu_stats.wait_us = 0;
    p->ppu_stats.fallback_us = p->ppu_stats.upload_bytes = 0;
    p->ppu_stats.draw_calls = 0;
    p->ppu_stats.timing_valid = 0;
    p->ppu_stats.primitive_commands=p->ppu_stats.primitive_commands_generated=0;
    p->ppu_stats.primitive_sources_reused=p->ppu_stats.primitive_image_reused=0;
    p->ppu_stats.last_result = SNESRECOMP_PPU_OK;
    p->ppu_stats.unsupported = SNES_PPU_SUPPORTED;
    return true;
}

SnesRecompPpuResult snesrecomp_presenter_ppu_prepare(
    SnesRecompPresenter *p, const SnesRecompPpuFrame *frame) {
    if (!p || !snesrecomp_ppu_frame_capture(frame))
        return Record(p, SNESRECOMP_PPU_INVALID);
    if (p->ppu_pending) return Record(p, SNESRECOMP_PPU_BUSY);
    if (!snesrecomp_presenter_ppu_capabilities(p))
        return Record(p, SNESRECOMP_PPU_UNSUPPORTED);
    p->ppu_stats.unsupported = SNES_PPU_SUPPORTED;
    snesrecomp_ppu_frame_retain(frame);
    SnesRecompPpuResult result = p->ops->ppu_prepare(p, frame);
    if (result == SNESRECOMP_PPU_OK) {
        p->ppu_pending = frame;
        ++p->ppu_stats.prepared;
    } else {
        p->ops->ppu_cancel(p);
        snesrecomp_ppu_frame_release(frame);
    }
    return Record(p, result);
}

SnesRecompPpuResult snesrecomp_presenter_ppu_prepare_job(
    SnesRecompPresenter *p,const SnesRecompPpuJob *job) {
    const SnesRecompPpuJobView *v=snesrecomp_ppu_job_view(job);
    if(!p || !v) return Record(p,SNESRECOMP_PPU_INVALID);
    if(p->ppu_pending) return Record(p,SNESRECOMP_PPU_BUSY);
    if(!v->indexed_layout && !v->overlay_count && !v->source_count && !v->primitive_count && (!v->blits || !v->blits->rect_count))
        return snesrecomp_presenter_ppu_prepare(p,v->frame);
    const uint32_t caps=snesrecomp_presenter_ppu_capabilities(p);
    if((v->overlay_count && !(caps&SNESRECOMP_PPU_CAP_POST_OVERLAYS)) ||
        (v->indexed_layout && !(caps&SNESRECOMP_PPU_CAP_INDEXED_LAYOUT)) ||
        (v->source_count && !(caps&SNESRECOMP_PPU_CAP_BG_SOURCES)) ||
        (v->primitive_count && !(caps&SNESRECOMP_PPU_CAP_PRIMITIVES)) ||
        (v->blits && v->blits->rect_count && !(caps&SNESRECOMP_PPU_CAP_POST_BLITS)) ||
        !p->ops->ppu_prepare_job) return Record(p,SNESRECOMP_PPU_UNSUPPORTED);
    for(unsigned i=0;i<v->overlay_count;++i)
        if(v->overlays[i].brightness_reduction && !(caps&SNESRECOMP_PPU_CAP_POST_RGB5_BRIGHTNESS))
            return Record(p,SNESRECOMP_PPU_UNSUPPORTED);
    p->ppu_stats.unsupported=SNES_PPU_SUPPORTED;
    snesrecomp_ppu_job_retain(job);
    const SnesRecompPpuResult r=p->ops->ppu_prepare_job(p,job);
    if(r==SNESRECOMP_PPU_OK) {
        p->ppu_pending=v->frame;p->ppu_pending_job=job;++p->ppu_stats.prepared;
    } else {p->ops->ppu_cancel(p);snesrecomp_ppu_job_release(job);}
    return Record(p,r);
}

static void ReleasePending(SnesRecompPresenter *p) {
    if(p->ppu_pending_job) snesrecomp_ppu_job_release(p->ppu_pending_job);
    else snesrecomp_ppu_frame_release(p->ppu_pending);
    p->ppu_pending_job=NULL;p->ppu_pending=NULL;
}

SnesRecompPpuResult snesrecomp_presenter_ppu_submit(SnesRecompPresenter *p) {
    if (!p || !p->ppu_pending) return Record(p, SNESRECOMP_PPU_INVALID);
    SnesRecompPpuResult result = p->ops->ppu_submit(p);
    if (result == SNESRECOMP_PPU_OK) ++p->ppu_stats.submitted;
    else p->ops->ppu_cancel(p);
    ReleasePending(p);
    return Record(p, result);
}

void snesrecomp_presenter_ppu_cancel(SnesRecompPresenter *p) {
    if (!p || !p->ppu_pending) return;
    p->ops->ppu_cancel(p);
    ReleasePending(p);
    ++p->ppu_stats.cancelled;
}

void snesrecomp_presenter_ppu_stats(const SnesRecompPresenter *p,
                                   SnesRecompPpuStats *out) {
    if (!out) return;
    if (p) *out = p->ppu_stats;
    else memset(out, 0, sizeof(*out));
}
