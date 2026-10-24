/* Bounded field serialization; no SDK/host structure layout on disk. */
enum { TRACE_INDEXED_HEADER_BYTES=152 };
static void PutIndexedClaim(uint8_t **p,const SnesRecompPpuIndexedClaim *r) {
    PutU16(p,r->x);PutU16(p,r->y);PutU16(p,r->width);PutU16(p,r->height);PutU16(p,r->owner);
    PutU8(p,r->plane);PutU8(p,r->coverage);
}
static void GetIndexedClaim(const uint8_t **p,SnesRecompPpuIndexedClaim *r) {
    r->x=GetU16(p);r->y=GetU16(p);r->width=GetU16(p);r->height=GetU16(p);r->owner=GetU16(p);
    r->plane=GetU8(p);r->coverage=GetU8(p);
}
static void PutIndexedObject(uint8_t **p,const SnesRecompPpuIndexedObject *r) {
    PutU32(p,(uint32_t)r->x);PutU32(p,(uint32_t)r->y);PutU32(p,r->x_first);PutU32(p,r->y_first);
    PutU16(p,r->source_x);PutU16(p,r->source_y);PutU16(p,r->owner);PutU16(p,r->x_count);PutU16(p,r->y_count);
    PutU8(p,r->plane);PutU8(p,r->reserved);
}
static void GetIndexedObject(const uint8_t **p,SnesRecompPpuIndexedObject *r) {
    r->x=(int32_t)GetU32(p);r->y=(int32_t)GetU32(p);r->x_first=GetU32(p);r->y_first=GetU32(p);
    r->source_x=GetU16(p);r->source_y=GetU16(p);r->owner=GetU16(p);r->x_count=GetU16(p);r->y_count=GetU16(p);
    r->plane=GetU8(p);r->reserved=GetU8(p);
}
static bool WriteIndexedTrace(const char *path,const SnesRecompPpuJobView *v,
    uint32_t frame_id,uint32_t accepted,uint32_t reason) {
    const SnesRecompPpuIndexedLayout *l=v->indexed_layout;
    if(!path || !l || !v->graphics_frame || v->image_count || v->overlay_count || v->source_count ||
        v->primitive_count || v->blits) return false;
    const SnesRecompPpuFrame *frames[]={v->frame,v->graphics_frame};
    uint8_t *captures[2]={NULL,NULL};size_t sizes[2]={0,0},bytes=TRACE_INDEXED_HEADER_BYTES;
    const SnesRecompObjFrame *objects[2];uint32_t flags=0;
    for(unsigned i=0;i<2;++i) {
        if(!EncodeTrace(captures+i,sizes+i,snesrecomp_ppu_frame_capture(frames[i]),frame_id,accepted,reason,NULL)) {
            free(captures[0]);free(captures[1]);return false;
        }
        objects[i]=snesrecomp_ppu_frame_obj(frames[i]);
        if(objects[i]) {
            flags|=(1u|(objects[i]->range_over?2u:0u)|(objects[i]->time_over?4u:0u))<<(i*3u);
            bytes+=(size_t)objects[i]->count*12u+snesrecomp_ppu_frame_capture(frames[i])->visible_height;
        }
        bytes+=sizes[i];
    }
    bytes+=128u+(size_t)l->claim_count*12u+(size_t)l->object_count*28u+(size_t)l->axis_count*8u+
        (size_t)l->obj_run_count*20u+(size_t)l->material_count*8u+(size_t)l->band_count*l->plane_count*2u;
    if(bytes>TRACE_PRIMITIVE_MAX_BYTES) {free(captures[0]);free(captures[1]);return false;}
    uint8_t *buf=calloc(1,bytes);if(!buf) {free(captures[0]);free(captures[1]);return false;}
    uint8_t *p=buf+TRACE_INDEXED_HEADER_BYTES;
    for(unsigned i=0;i<2;++i) {memcpy(p,captures[i],sizes[i]);p+=sizes[i];free(captures[i]);}
    for(unsigned i=0;i<2;++i) if(objects[i]) {
        for(unsigned k=0;k<objects[i]->count;++k) PutSliver(&p,objects[i]->slivers+k);
        const unsigned height=snesrecomp_ppu_frame_capture(frames[i])->visible_height;
        memcpy(p,objects[i]->line_priority,height);p+=height;
    }
    memcpy(p,l->oam_owners,128);p+=128;
    for(uint32_t i=0;i<l->claim_count;++i) PutIndexedClaim(&p,l->claims+i);
    for(uint32_t i=0;i<l->object_count;++i) PutIndexedObject(&p,l->objects+i);
    for(uint32_t i=0;i<l->axis_count;++i) {
        const SnesRecompPpuAxisRun *r=l->axes+i;
        PutU16(&p,r->destination);PutU16(&p,r->source);PutU16(&p,r->count);PutU8(&p,r->step);PutU8(&p,r->reserved);
    }
    for(uint32_t i=0;i<l->obj_run_count;++i) {
        const SnesRecompPpuObjRun *r=l->obj_runs+i;PutSliver(&p,&r->sliver);PutU16(&p,r->owner);
        PutU8(&p,r->begin);PutU8(&p,r->end);PutU8(&p,r->overlay_begin);PutU8(&p,r->overlay_end);
        PutU8(&p,r->flags);PutU8(&p,r->reserved);
    }
    for(unsigned i=0;i<l->material_count;++i) {
        const SnesRecompPpuMaterial *m=l->materials+i;
        PutU8(&p,m->main_enable);PutU8(&p,m->occluding_bg);PutU8(&p,m->brightness_reduction);PutU8(&p,m->flags);PutU32(&p,m->reserved);
    }
    for(unsigned i=0;i<(unsigned)l->band_count*l->plane_count;++i) PutU16(&p,l->band_materials[i]);
    if((size_t)(p-buf)!=bytes) {free(buf);return false;}
    const uint64_t hash=snesrecomp_ppu_trace_hash(buf+TRACE_INDEXED_HEADER_BYTES,bytes-TRACE_INDEXED_HEADER_BYTES);
    p=buf;memcpy(p,SNES_PPU_TRACE_MAGIC,SNES_PPU_TRACE_MAGIC_BYTES);p+=SNES_PPU_TRACE_MAGIC_BYTES;
    PutU32(&p,SNES_PPU_TRACE_INDEXED_JOB_VERSION);PutU32(&p,TRACE_INDEXED_HEADER_BYTES);PutU64(&p,bytes);
    PutU32(&p,frame_id);PutU32(&p,accepted);PutU32(&p,reason);PutU32(&p,(uint32_t)sizes[0]);PutU32(&p,(uint32_t)sizes[1]);
    PutU32(&p,l->claim_count);PutU32(&p,l->object_count);PutU32(&p,l->axis_count);PutU32(&p,l->obj_run_count);
    PutU32(&p,l->material_count);PutU32(&p,l->band_count);
    PutU32(&p,objects[0]?objects[0]->count:0);PutU32(&p,objects[1]?objects[1]->count:0);
    PutU64(&p,v->sequence);PutU64(&p,hash);PutU32(&p,flags);PutU16(&p,l->width);PutU16(&p,l->height);
    PutU8(&p,l->plane_count);PutU8(&p,l->owner_planes);PutU16(&p,l->reserved);
    for(unsigned i=0;i<5;++i) PutU8(&p,l->source_layers[i]);PutU8(&p,0);PutU16(&p,0);
    for(unsigned i=0;i<8;++i) PutU32(&p,l->qualified_lines[i]);PutU32(&p,0);
    if((size_t)(p-buf)!=TRACE_INDEXED_HEADER_BYTES) {free(buf);return false;}
    return WriteBuffer(path,buf,bytes);
}
static bool DecodeIndexedTrace(uint8_t *buf,size_t bytes,SnesPpuTrace *out,
    SnesRecompPpuJob **job,char *error,size_t error_bytes) {
    if(!job || bytes<TRACE_INDEXED_HEADER_BYTES) {free(buf);return Fail(out,error,error_bytes,"indexed trace requires job reader/header");}
    const uint8_t *p=buf+16;
    const uint32_t header=GetU32(&p);const uint64_t total=GetU64(&p);
    const uint32_t id=GetU32(&p),accepted=GetU32(&p),reason=GetU32(&p),sizes[2]={GetU32(&p),GetU32(&p)};
    SnesRecompPpuIndexedLayout l={0};
    l.claim_count=GetU32(&p);l.object_count=GetU32(&p);l.axis_count=GetU32(&p);l.obj_run_count=GetU32(&p);
    const uint32_t materials=GetU32(&p),bands=GetU32(&p),counts[2]={GetU32(&p),GetU32(&p)};
    const uint64_t sequence=GetU64(&p),hash=GetU64(&p);const uint32_t flags=GetU32(&p);
    l.width=GetU16(&p);l.height=GetU16(&p);l.plane_count=GetU8(&p);l.owner_planes=GetU8(&p);
    const uint16_t reserved=GetU16(&p);
    for(unsigned i=0;i<5;++i) l.source_layers[i]=GetU8(&p);
    const uint8_t padding_byte=GetU8(&p);const uint16_t padding_word=GetU16(&p);
    for(unsigned i=0;i<8;++i) l.qualified_lines[i]=GetU32(&p);
    const uint32_t trailing=GetU32(&p);
    if(header!=TRACE_INDEXED_HEADER_BYTES || total!=bytes || reserved || padding_byte || padding_word || trailing ||
        l.claim_count>SNESRECOMP_PPU_INDEXED_MAX_CLAIMS || l.object_count>SNESRECOMP_PPU_INDEXED_MAX_OBJECTS ||
        l.axis_count>SNESRECOMP_PPU_INDEXED_MAX_AXES || l.obj_run_count>SNESRECOMP_OBJ_MAX_SLIVERS ||
        materials>SNESRECOMP_PPU_JOB_MAX_MATERIALS || bands>SNES_PPU_MAX_BANDS ||
        !l.plane_count || l.plane_count>5 || counts[0]>SNESRECOMP_OBJ_MAX_SLIVERS || counts[1]>SNESRECOMP_OBJ_MAX_SLIVERS ||
        sizes[0]<TRACE_HEADER_V2_BYTES || sizes[1]<TRACE_HEADER_V2_BYTES || sizes[0]>TRACE_LEGACY_MAX_BYTES || sizes[1]>TRACE_LEGACY_MAX_BYTES ||
        flags>63 || (!(flags&1) && ((flags&6) || counts[0])) || (!(flags&8) && ((flags&48) || counts[1])) ||
        snesrecomp_ppu_trace_hash(p,bytes-header)!=hash) {free(buf);return Fail(out,error,error_bytes,"invalid indexed envelope");}
    l.material_count=(uint16_t)materials;l.band_count=(uint16_t)bands;
    size_t fixed=header+(size_t)sizes[0]+sizes[1]+128u+(size_t)l.claim_count*12u+(size_t)l.object_count*28u+
        (size_t)l.axis_count*8u+(size_t)l.obj_run_count*20u+(size_t)materials*8u+(size_t)bands*l.plane_count*2u+
        (size_t)(counts[0]+counts[1])*12u;
    if(fixed>bytes) {free(buf);return Fail(out,error,error_bytes,"indexed payload size mismatch");}
    SnesPpuTrace traces[2]={{0},{0}};SnesRecompPpuFrame *frames[2]={NULL,NULL};
    SnesRecompObjFrame objects[2]={{0},{0}};SnesRecompPpuJob *decoded=NULL;bool ok=false;
    SnesRecompPpuIndexedClaim *claims=NULL;SnesRecompPpuIndexedObject *placements=NULL;
    SnesRecompPpuAxisRun *axes=NULL;SnesRecompPpuObjRun *runs=NULL;
    SnesRecompPpuMaterial m[SNESRECOMP_PPU_JOB_MAX_MATERIALS];uint16_t mapping[SNES_PPU_MAX_BANDS*5];uint8_t owners[128];
    const char *failure="invalid indexed capture or payload";
    for(unsigned i=0;i<2;++i) {
        uint8_t *copy=malloc(sizes[i]);if(!copy)goto cleanup;memcpy(copy,p,sizes[i]);p+=sizes[i];
        if(!DecodeTrace(copy,sizes[i],traces+i,NULL,error,error_bytes))goto cleanup;
        if(traces[i].frame_id!=id || traces[i].accepted!=accepted || traces[i].reject_reason!=reason)goto cleanup;
        if(flags&(1u<<(i*3u))) fixed+=traces[i].capture.visible_height;
    }
    if(fixed!=bytes)goto cleanup;
    for(unsigned i=0;i<2;++i) if(flags&(1u<<(i*3u))) {
        objects[i].count=objects[i].capacity=counts[i];objects[i].line_capacity=traces[i].capture.visible_height;
        objects[i].slivers=counts[i]?calloc(counts[i],sizeof(*objects[i].slivers)):NULL;
        objects[i].line_priority=malloc(objects[i].line_capacity);
        if((counts[i] && !objects[i].slivers) || !objects[i].line_priority)goto cleanup;
        for(unsigned k=0;k<counts[i];++k)GetSliver(&p,objects[i].slivers+k);
        memcpy(objects[i].line_priority,p,objects[i].line_capacity);p+=objects[i].line_capacity;
        objects[i].range_over=(flags&(2u<<(i*3u)))!=0;objects[i].time_over=(flags&(4u<<(i*3u)))!=0;
    }
    memcpy(owners,p,128);p+=128;
    claims=l.claim_count?calloc(l.claim_count,sizeof(*claims)):NULL;
    placements=l.object_count?calloc(l.object_count,sizeof(*placements)):NULL;
    axes=l.axis_count?calloc(l.axis_count,sizeof(*axes)):NULL;runs=l.obj_run_count?calloc(l.obj_run_count,sizeof(*runs)):NULL;
    if((l.claim_count && !claims) || (l.object_count && !placements) || (l.axis_count && !axes) || (l.obj_run_count && !runs))goto cleanup;
    for(unsigned i=0;i<l.claim_count;++i)GetIndexedClaim(&p,claims+i);
    for(unsigned i=0;i<l.object_count;++i)GetIndexedObject(&p,placements+i);
    for(unsigned i=0;i<l.axis_count;++i) {
        axes[i].destination=GetU16(&p);axes[i].source=GetU16(&p);axes[i].count=GetU16(&p);axes[i].step=GetU8(&p);axes[i].reserved=GetU8(&p);
    }
    for(unsigned i=0;i<l.obj_run_count;++i) {
        SnesRecompPpuObjRun *r=runs+i;GetSliver(&p,&r->sliver);r->owner=GetU16(&p);
        r->begin=GetU8(&p);r->end=GetU8(&p);r->overlay_begin=GetU8(&p);r->overlay_end=GetU8(&p);r->flags=GetU8(&p);r->reserved=GetU8(&p);
    }
    for(unsigned i=0;i<materials;++i) {m[i].main_enable=GetU8(&p);m[i].occluding_bg=GetU8(&p);
        m[i].brightness_reduction=GetU8(&p);m[i].flags=GetU8(&p);m[i].reserved=GetU32(&p);}
    for(unsigned i=0;i<bands*l.plane_count;++i)mapping[i]=GetU16(&p);
    l.claims=claims;l.objects=placements;l.axes=axes;l.obj_runs=runs;l.oam_owners=owners;l.materials=m;l.band_materials=mapping;
    for(unsigned i=0;i<2;++i) {
        frames[i]=snesrecomp_ppu_frame_create();if(!frames[i])goto cleanup;
        if(snesrecomp_ppu_frame_freeze(frames[i],&traces[i].capture,flags&(1u<<(i*3u))?objects+i:NULL,sequence)!=SNESRECOMP_PPU_OK)goto cleanup;
    }
    decoded=snesrecomp_ppu_job_create_indexed(l.claim_count?l.claim_count:1,l.object_count?l.object_count:1,l.axis_count?l.axis_count:1);
    if(!decoded || snesrecomp_ppu_job_freeze_indexed(decoded,frames[0],frames[1],&l,sequence)!=SNESRECOMP_PPU_OK)goto cleanup;
    *out=traces[0];memset(traces,0,sizeof traces[0]);out->format_version=SNES_PPU_TRACE_INDEXED_JOB_VERSION;
    *job=decoded;decoded=NULL;ok=true;
cleanup:
    for(unsigned i=0;i<2;++i) {snesrecomp_ppu_trace_free(traces+i);snesrecomp_ppu_frame_release(frames[i]);
        free(objects[i].slivers);free(objects[i].line_priority);}
    snesrecomp_ppu_job_release(decoded);free(claims);free(placements);free(axes);free(runs);free(buf);
    return ok || Fail(out,error,error_bytes,failure);
}
