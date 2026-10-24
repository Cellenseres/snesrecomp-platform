/* Private source-plane helpers. Shared by console backends through the raw
 * tile planner; no SDK objects, game addresses or finished-image readbacks. */
static void SourceDecode(const SnesRecompPpuJobImage *source,unsigned tx,unsigned ty,
    bool rgb,uint32_t pixels[64],bool *opaque) {
    *opaque=false;
    for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x) {
        const unsigned sx=tx*8u+x,sy=ty*8u+y;uint32_t pixel=0;
        if(sx<source->width && sy<source->height) {
            pixel=source->pixels[(size_t)sy*source->pitch+sx];
            if(pixel>>24) {*opaque=true;pixel=0xff000000u|(rgb?(pixel&0x00f8f8f8u):0u);}
            else pixel=0;
        }
        pixels[y*8u+x]=pixel;
    }
}
/* Colour animation does not move quads. Only an opacity-class change can
 * make a previously omitted tile newly visible and requires new geometry. */
static bool SourceRefresh(SnesRecompColourTileCache *c,const SnesRecompPpuJobView *job,
    SnesRecompTilePlan *out) {
    bool coverage_same=true;
    for(unsigned slot=0;slot<SNESRECOMP_COLOUR_TILE_SLOTS;++slot) {
        ColourTileEntry *e=c->entries+slot;
        if(!e->valid || !(e->key&0x80000000u) || e->epoch!=c->epoch || e->image_owner!=job->owner) continue;
        const unsigned image=e->key>>16&255u;
        if(e->image_version==job->image_versions[image]) continue;
        uint32_t pixels[64];bool opaque;
        SourceDecode(job->images+image,e->key&63u,e->key>>6&1023u,
            (e->key&0x40000000u)!=0,pixels,&opaque);
        coverage_same &= c->opaque[slot]==opaque;
        if(memcmp(pixels,c->tiles+slot*64u,sizeof pixels)) {
            memcpy(c->tiles+slot*64u,pixels,sizeof pixels);
            c->dirty[slot]=1;c->generations[slot]=++c->generation;++out->tiles_decoded;
        }
        c->opaque[slot]=opaque;e->image_version=job->image_versions[image];
    }
    return coverage_same;
}
static int SourceTile(SnesRecompColourTileCache *c, unsigned image,
    unsigned tx, unsigned ty, bool rgb, SnesRecompTilePlan *out) {
    const SnesRecompPpuJobView *job=c->source_job;
    const SnesRecompPpuJobImage *source=job->images+image;
    const uint32_t key=0x80000000u | (rgb?0x40000000u:0u) | (image<<16) | (ty<<6) | tx;
    uint32_t hash=key^(uint32_t)job->owner^(uint32_t)(job->owner>>32);
    hash=(hash^(hash>>16))*0x7feb352du;
    hash=(hash^(hash>>15))*0x846ca68bu;
    const unsigned base=((hash^(hash>>16))&(CACHE_BUCKETS-1u))*CACHE_WAYS;
    int chosen=-1;uint32_t oldest=UINT32_MAX;
    for(unsigned way=0;way<CACHE_WAYS;++way) {
        ColourTileEntry *e=c->entries+base+way;
        if(e->valid && e->key==key && e->image_owner==job->owner &&
            e->image_version==job->image_versions[image]) {
            e->epoch=c->epoch;++out->cache_hits;return (int)(base+way);
        }
        if(!e->valid || (e->epoch!=c->epoch && e->epoch<oldest)) {
            chosen=(int)(base+way);oldest=e->valid?e->epoch:0;
        }
    }
    if(chosen<0) return -1;
    ColourTileEntry *e=c->entries+chosen;
    e->valid=true;e->key=key;e->epoch=c->epoch;
    e->image_owner=job->owner;e->image_version=job->image_versions[image];
    SourceDecode(source,tx,ty,rgb,c->tiles+(unsigned)chosen*64u,c->opaque+chosen);
    c->dirty[chosen]=1;c->generations[chosen]=++c->generation;++out->tiles_decoded;
    return chosen;
}
static void SourceRows(SnesRecompColourTileCache *c,const SnesRecompPpuJobView *job) {
    memset(c->source_row_counts,0,sizeof c->source_row_counts);
    if(!job) return;
    for(unsigned i=0;i<job->source_count;++i) {
        const SnesRecompPpuSource *r=job->sources+i;
        for(unsigned screen=0;screen<2;++screen) if(r->screens&(1u<<screen))
            for(unsigned y=r->y;y<r->y+r->height;++y) {
                uint8_t *count=&c->source_row_counts[screen][r->layer][y];
                uint16_t *row=c->source_rows[screen][r->layer][y];
                unsigned at=*count;
                while(at && job->sources[row[at-1]].x>r->x) {row[at]=row[at-1];--at;}
                row[at]=(uint16_t)i;++*count;
            }
    }
}
static bool SourcePiece(SnesRecompColourTileCache *c,SnesRecompTilePlan *out,
    const SnesRecompTileQuad *q,int x0,int y0,int x1,int y1) {
    if(x0>=x1 || y0>=y1) return true;
    if(out->quad_count==SNESRECOMP_TILE_PLAN_QUADS) return false;
    SnesRecompTileQuad piece=*q;
    /* Native repeated columns retain their single source texel. All other
     * native axes are one-to-one, including reversed UVs. */
    if(abs((int)q->u1-q->u0)==q->x1-q->x0) {
        const int direction=q->u1>q->u0?1:-1;
        piece.u0=(uint8_t)(q->u0+direction*(x0-q->x0));
        piece.u1=(uint8_t)(q->u0+direction*(x1-q->x0));
    }
    const int direction=q->v1>q->v0?1:-1;
    piece.v0=(uint8_t)(q->v0+direction*(y0-q->y0));
    piece.v1=(uint8_t)(q->v0+direction*(y1-q->y0));
    piece.x0=(int16_t)x0;piece.x1=(int16_t)x1;
    piece.y0=(int16_t)y0;piece.y1=(int16_t)y1;
    c->quads[out->quad_count++]=piece;return true;
}
static bool SourceClipNative(SnesRecompColourTileCache *c,SnesRecompTilePlan *out,
    const SnesRecompTileQuad *q,unsigned layer,bool sub) {
    for(unsigned y=(unsigned)q->y0;y<(unsigned)q->y1;) {
        const unsigned count=c->source_row_counts[sub][layer][y];
        const uint16_t *row=c->source_rows[sub][layer][y];
        unsigned end=y+1;
        while(end<(unsigned)q->y1 && c->source_row_counts[sub][layer][end]==count &&
            !memcmp(row,c->source_rows[sub][layer][end],count*sizeof(*row))) ++end;
        int left=q->x0;
        for(unsigned i=0;i<count && left<q->x1;++i) {
            const SnesRecompPpuSource *r=c->source_job->sources+row[i];
            if(r->x+r->width<=left) continue;
            if(r->x>=q->x1) break;
            const int right=r->x<q->x1?r->x:q->x1;
            if(!SourcePiece(c,out,q,left,(int)y,right,(int)end)) return false;
            left=r->x+r->width;
        }
        if(!SourcePiece(c,out,q,left,(int)y,q->x1,(int)end)) return false;
        y=end;
    }
    return true;
}
static bool DrawSources(SnesRecompColourTileCache *c,const SnesPpuFrameCapture *cap,
    SnesRecompTilePlan *out) {
    const SnesRecompPpuJobView *job=c->source_job;
    if(!job) return true;
    for(unsigned i=0;i<job->source_count;++i) {
        const SnesRecompPpuSource *r=job->sources+i;
        for(unsigned bi=0;bi<cap->band_count;++bi) {
            const SnesPpuRasterBand *band=cap->bands+bi;
            if(band->forced_blank || band->y_end<=r->y || band->y_begin>=r->y+r->height) continue;
            const unsigned begin=band->y_begin>r->y?band->y_begin:r->y;
            const unsigned end=band->y_end<r->y+r->height?band->y_end:r->y+r->height;
            for(unsigned sub=0;sub<2;++sub) {
                if(!(r->screens&(1u<<sub)) || !((sub?band->sub_enable:band->main_enable)&(1u<<r->layer)) ||
                    (sub && !(band->cgwsel&2u))) continue;
                for(unsigned si=0;si<c->window_counts[bi];++si) {
                    const SnesRecompWindowSpan *span=c->windows[bi]+si;
                    if(!(span->mask&(1u<<(r->layer+sub*3u)))) continue;
                    const unsigned left=span->begin>r->x?span->begin:r->x;
                    const unsigned right=span->end<r->x+r->width?span->end:r->x+r->width;
                    const bool rgb=sub || (span->mask&SNESRECOMP_SEMANTIC_MAIN_RGB);
                    unsigned rank=r->layer==0?(r->priority?5u:3u):
                        r->layer==1?(r->priority?4u:2u):
                        r->priority?(band->bg3_priority?6u:1u):0u;
                    const unsigned pass=sub?SNESRECOMP_RAW_TILE_MAIN_PASSES+rank:
                        rank*2u+!!((span->mask&SNESRECOMP_SEMANTIC_MATH) && (band->cgadsub&(1u<<r->layer)));
                    for(unsigned y=begin;y<end;) {
                        const unsigned sy=r->source_y+(r->source_height==1?0u:y-r->y);
                        unsigned h=r->source_height==1?end-y:8u-(sy&7u);
                        if(h>end-y) h=end-y;
                        for(unsigned x=left;x<right;) {
                            const unsigned sx=r->source_x+(r->source_width==1?0u:x-r->x);
                            unsigned w=r->source_width==1?right-x:8u-(sx&7u);
                            if(w>right-x) w=right-x;
                            const int slot=SourceTile(c,r->image,sx/8u,sy/8u,rgb,out);
                            if(slot<0) return false;
                            if(c->opaque[slot]) {
                                if(out->quad_count==SNESRECOMP_TILE_PLAN_QUADS) return false;
                                c->quads[out->quad_count++]=(SnesRecompTileQuad){
                                    (int16_t)x,(int16_t)y,(int16_t)(x+w),(int16_t)(y+h),(uint16_t)slot,
                                    (uint8_t)(sx&7u),(uint8_t)(sy&7u),
                                    (uint8_t)((sx&7u)+(r->source_width==1?1u:w)),
                                    (uint8_t)((sy&7u)+(r->source_height==1?1u:h)),(uint8_t)pass,0};
                            }
                            x+=w;
                        }
                        y+=h;
                    }
                }
            }
        }
    }
    return true;
}
