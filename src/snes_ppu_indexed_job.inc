/* Owned index-atlas projection geometry. No guest reads or pixel work. */
SnesRecompPpuJob *snesrecomp_ppu_job_create_indexed(uint32_t claims,
    uint32_t objects,uint32_t axes) {
    if(!claims || !objects || !axes || claims>SNESRECOMP_PPU_INDEXED_MAX_CLAIMS ||
        objects>SNESRECOMP_PPU_INDEXED_MAX_OBJECTS || axes>SNESRECOMP_PPU_INDEXED_MAX_AXES) return NULL;
    SnesRecompPpuJob *j=Create(0,0,0);if(!j) return NULL;
    for(unsigned i=0;i<2;++i) {
        j->indexed_claims[i]=malloc((size_t)claims*sizeof(*j->indexed_claims[i]));
        j->indexed_objects[i]=malloc((size_t)objects*sizeof(*j->indexed_objects[i]));
        j->indexed_axes[i]=malloc((size_t)axes*sizeof(*j->indexed_axes[i]));
        j->indexed_runs[i]=malloc(SNESRECOMP_OBJ_MAX_SLIVERS*sizeof(*j->indexed_runs[i]));
        if(!j->indexed_claims[i] || !j->indexed_objects[i] || !j->indexed_axes[i] || !j->indexed_runs[i]) {
            snesrecomp_ppu_job_release(j);return NULL;
        }
    }
    j->indexed_claim_capacity=claims;j->indexed_object_capacity=objects;j->indexed_axis_capacity=axes;
    return j;
}
static bool IndexedAxisValid(const SnesRecompPpuIndexedLayout *l,uint32_t first,
    uint16_t count,int32_t destination,uint16_t source,uint16_t extent) {
    if(!count || first>l->axis_count || count>l->axis_count-first) return false;
    uint32_t end=0;
    for(unsigned i=0;i<count;++i) {
        const SnesRecompPpuAxisRun *r=l->axes+first+i;
        if(!r->count || r->reserved || r->step>1 || (i && r->destination!=end) ||
            (int64_t)destination+r->destination<0 ||
            (int64_t)destination+r->destination+r->count>extent ||
            (uint32_t)source+r->source+(r->step?r->count-1u:0u)>=extent) return false;
        end=(uint32_t)r->destination+r->count;
    }
    return true;
}
SnesRecompPpuResult snesrecomp_ppu_job_freeze_indexed(SnesRecompPpuJob *j,
    const SnesRecompPpuFrame *frame,const SnesRecompPpuFrame *graphics,
    const SnesRecompPpuIndexedLayout *layout,uint64_t sequence) {
    if(!j) return SNESRECOMP_PPU_INVALID;
    if(j->references!=1) return SNESRECOMP_PPU_BUSY;
    const SnesPpuFrameCapture *base=snesrecomp_ppu_frame_capture(frame);
    const SnesPpuFrameCapture *source=snesrecomp_ppu_frame_capture(graphics);
    if(!base || !source || !layout) return SNESRECOMP_PPU_INVALID;
    /* Metadata may itself be the previous owned view. Capture it before the
     * ordinary freeze releases its frame/clears incompatible payload kinds. */
    SnesRecompPpuIndexedLayout l=*layout;
    if(l.width!=base->canvas_width || l.height!=base->visible_height ||
        l.width!=source->canvas_width || l.height!=source->visible_height ||
        base->canvas_extra!=source->canvas_extra || !l.plane_count ||
        l.plane_count>SNESRECOMP_PPU_INDEXED_MAX_PLANES || l.reserved ||
        (l.owner_planes&~((1u<<l.plane_count)-1u)) || !l.oam_owners ||
        (l.claim_count && !l.claims) || (l.object_count && !l.objects) ||
        (l.axis_count && !l.axes) || (l.obj_run_count && !l.obj_runs) ||
        (l.material_count && (!l.materials || !l.band_materials || l.band_count!=source->band_count)) ||
        (!l.material_count && (l.band_count || l.object_count || l.claim_count || l.obj_run_count)))
        return SNESRECOMP_PPU_INVALID;
    if(!j->indexed_claim_capacity || l.claim_count>j->indexed_claim_capacity ||
        l.object_count>j->indexed_object_capacity || l.axis_count>j->indexed_axis_capacity ||
        l.obj_run_count>SNESRECOMP_OBJ_MAX_SLIVERS || l.material_count>SNESRECOMP_PPU_JOB_MAX_MATERIALS)
        return SNESRECOMP_PPU_RESOURCE_LIMIT;
    for(unsigned i=0;i<l.material_count;++i) {
        const SnesRecompPpuMaterial *m=l.materials+i;
        if(m->reserved || m->brightness_reduction || (m->main_enable&~31u) ||
            (m->occluding_bg!=255 && m->occluding_bg>=3) ||
            (m->flags!=SNESRECOMP_PPU_MATERIAL_SOURCE_COLOUR && m->flags!=SNESRECOMP_PPU_MATERIAL_PALETTE_COLOUR))
            return SNESRECOMP_PPU_INVALID;
    }
    for(unsigned y=l.height;y<256;++y)
        if(l.qualified_lines[y>>5]&(1u<<(y&31))) return SNESRECOMP_PPU_INVALID;
    bool objects=false;
    for(unsigned i=0;i<SNESRECOMP_PPU_INDEXED_MAX_PLANES;++i) {
        if(i>=l.plane_count) {if(l.source_layers[i]) return SNESRECOMP_PPU_INVALID;continue;}
        if(l.source_layers[i]>4) return SNESRECOMP_PPU_INVALID;
        objects|=l.source_layers[i]==4;
        for(unsigned k=0;k<i;++k) if(l.source_layers[k]==l.source_layers[i]) return SNESRECOMP_PPU_INVALID;
    }
    for(unsigned band=0;band<l.band_count;++band) for(unsigned plane=0;plane<l.plane_count;++plane) {
        const unsigned index=l.band_materials[band*l.plane_count+plane];
        if(index>=l.material_count || !(l.materials[index].main_enable&(1u<<l.source_layers[plane])))
            return SNESRECOMP_PPU_INVALID;
    }
    if(objects)
        for(unsigned b=0;b<source->band_count;++b) if(!source->bands[b].forced_blank)
            for(unsigned y=source->bands[b].y_begin;y<source->bands[b].y_end;++y)
                if(!(l.qualified_lines[y>>5]&(1u<<(y&31)))) return SNESRECOMP_PPU_INCOMPLETE_MEMORY;
    for(uint32_t i=0;i<l.obj_run_count;++i) {
        const SnesRecompPpuObjRun *r=l.obj_runs+i;const SnesRecompObjSliver *s=&r->sliver;
        if(s->line>=l.height || s->tile>=2048 || s->row>7 || s->priority>3 || s->flip_h>1 ||
            s->oam_slot>=128 || s->palette_base<128 || (s->palette_base&15) ||
            s->math_eligible!=(s->palette_base>=192) || r->begin>r->end || r->end>8 ||
            r->overlay_begin<r->begin || r->overlay_begin>r->overlay_end || r->overlay_end>r->end ||
            r->flags>7 || r->reserved ||
            ((r->flags&SNESRECOMP_OBJ_RUN_REMOVE_OVERLAY) && !(r->flags&SNESRECOMP_OBJ_RUN_OVERLAY)) ||
            (!(r->flags&SNESRECOMP_OBJ_RUN_OVERLAY) && r->overlay_begin!=r->overlay_end) ||
            !(l.qualified_lines[s->line>>5]&(1u<<(s->line&31))) ||
            (i && s->line<l.obj_runs[i-1].sliver.line)) return SNESRECOMP_PPU_INVALID;
    }
    for(uint32_t i=0;i<l.claim_count;++i) {
        const SnesRecompPpuIndexedClaim *c=l.claims+i;
        if(c->plane>=l.plane_count || c->coverage>1 || !c->width || !c->height ||
            (uint32_t)c->x+c->width>l.width || (uint32_t)c->y+c->height>l.height)
            return SNESRECOMP_PPU_INVALID;
    }
    for(uint32_t i=0;i<l.object_count;++i) {
        const SnesRecompPpuIndexedObject *o=l.objects+i;
        if(o->reserved || o->plane>=l.plane_count ||
            !IndexedAxisValid(&l,o->x_first,o->x_count,o->x,o->source_x,l.width) ||
            !IndexedAxisValid(&l,o->y_first,o->y_count,o->y,o->source_y,l.height))
            return SNESRECOMP_PPU_INVALID;
    }
    /* Unreferenced runs must also be canonical, so later backend grouping
     * cannot accidentally treat reserved or invalid trailing data as geometry. */
    for(uint32_t i=0;i<l.axis_count;++i)
        if(!l.axes[i].count || l.axes[i].reserved || l.axes[i].step>1) return SNESRECOMP_PPU_INVALID;
    const SnesRecompPpuIndexedLayout *old=&j->indexed;
    const bool same_runs=j->indexed_valid && l.obj_run_count==old->obj_run_count &&
        !memcmp(l.qualified_lines,old->qualified_lines,sizeof l.qualified_lines) &&
        (!l.obj_run_count || snesrecomp_u8_equal((const uint8_t *)l.obj_runs,(const uint8_t *)old->obj_runs,
            (size_t)l.obj_run_count*sizeof(*l.obj_runs)));
    const bool same=j->indexed_valid && l.width==old->width && l.height==old->height &&
        l.plane_count==old->plane_count && l.owner_planes==old->owner_planes && l.claim_count==old->claim_count &&
        l.object_count==old->object_count && l.axis_count==old->axis_count &&
        l.material_count==old->material_count && l.band_count==old->band_count &&
        (!l.material_count || (!memcmp(l.materials,old->materials,(size_t)l.material_count*sizeof(*l.materials)) &&
            !memcmp(l.band_materials,old->band_materials,(size_t)l.band_count*l.plane_count*sizeof(*l.band_materials)))) &&
        !memcmp(l.source_layers,old->source_layers,sizeof l.source_layers) &&
        !memcmp(l.oam_owners,old->oam_owners,128) &&
        (!l.claim_count || snesrecomp_u8_equal((const uint8_t *)l.claims,(const uint8_t *)old->claims,
            (size_t)l.claim_count*sizeof(*l.claims))) &&
        (!l.object_count || snesrecomp_u8_equal((const uint8_t *)l.objects,(const uint8_t *)old->objects,
            (size_t)l.object_count*sizeof(*l.objects))) &&
        (!l.axis_count || snesrecomp_u8_equal((const uint8_t *)l.axes,(const uint8_t *)old->axes,
            (size_t)l.axis_count*sizeof(*l.axes)));
    if(!same && j->view.indexed_geometry_version==UINT64_MAX) return SNESRECOMP_PPU_RESOURCE_LIMIT;
    if(!same_runs && j->view.indexed_obj_version==UINT64_MAX) return SNESRECOMP_PPU_RESOURCE_LIMIT;
    const unsigned next=same?j->indexed_active:j->indexed_active^1u;
    const unsigned next_runs=same_runs?j->indexed_run_active:j->indexed_run_active^1u;
    if(!same_runs && l.obj_run_count)
        memmove(j->indexed_runs[next_runs],l.obj_runs,(size_t)l.obj_run_count*sizeof(*l.obj_runs));
    if(!same) {
        if(l.claim_count) memmove(j->indexed_claims[next],l.claims,(size_t)l.claim_count*sizeof(*l.claims));
        if(l.object_count) memmove(j->indexed_objects[next],l.objects,(size_t)l.object_count*sizeof(*l.objects));
        if(l.axis_count) memmove(j->indexed_axes[next],l.axes,(size_t)l.axis_count*sizeof(*l.axes));
        memmove(j->indexed_owners[next],l.oam_owners,128);
        if(l.material_count) {
            memmove(j->indexed_materials[next],l.materials,(size_t)l.material_count*sizeof(*l.materials));
            memmove(j->indexed_band_materials[next],l.band_materials,
                (size_t)l.band_count*l.plane_count*sizeof(*l.band_materials));
        }
    }
    snesrecomp_ppu_frame_retain(graphics);
    const SnesRecompPpuResult result=snesrecomp_ppu_job_freeze(j,frame,NULL,0,NULL,0,sequence);
    if(result!=SNESRECOMP_PPU_OK) {snesrecomp_ppu_frame_release(graphics);return result;}
    l.claims=j->indexed_claims[next];l.objects=j->indexed_objects[next];
    l.axes=j->indexed_axes[next];l.oam_owners=j->indexed_owners[next];
    l.obj_runs=j->indexed_runs[next_runs];
    l.materials=j->indexed_materials[next];l.band_materials=j->indexed_band_materials[next];
    j->indexed=l;j->view.indexed_layout=&j->indexed;j->view.graphics_frame=graphics;
    j->view.indexed_geometry_version+=!same;j->indexed_active=next;j->indexed_valid=true;
    j->view.indexed_obj_version+=!same_runs;j->indexed_run_active=next_runs;
    return SNESRECOMP_PPU_OK;
}
