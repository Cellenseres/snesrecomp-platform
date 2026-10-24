/* v17 is a bounded envelope around the existing capture codec. Both contexts
 * retain all raster extensions; there are no ABI-dependent structures on disk
 * and no recursive envelopes or temporary files. Diagnostic path only. */
static void PutPrimitive(uint8_t **p,const SnesRecompPpuPrimitive *r) {
    PutU16(p,r->source_x);PutU16(p,r->source_y);PutU16(p,r->x);PutU16(p,r->y);
    PutU16(p,r->width);PutU16(p,r->height);PutU16(p,r->priority);PutU16(p,r->material);
    PutU16(p,r->source);PutU16(p,r->owner);PutU8(p,r->kind);PutU8(p,r->reserved_byte);PutU16(p,r->reserved);
}
static void GetPrimitive(const uint8_t **p,SnesRecompPpuPrimitive *r) {
    r->source_x=GetU16(p);r->source_y=GetU16(p);r->x=GetU16(p);r->y=GetU16(p);
    r->width=GetU16(p);r->height=GetU16(p);r->priority=GetU16(p);r->material=GetU16(p);
    r->source=GetU16(p);r->owner=GetU16(p);r->kind=GetU8(p);r->reserved_byte=GetU8(p);r->reserved=GetU16(p);
}
static void PutSliver(uint8_t **p,const SnesRecompObjSliver *s) {
    PutU16(p,(uint16_t)s->screen_x);PutU16(p,s->line);PutU16(p,s->tile);
    PutU8(p,s->row);PutU8(p,s->palette_base);PutU8(p,s->priority);PutU8(p,s->flip_h);
    PutU8(p,s->oam_slot);PutU8(p,s->math_eligible);
}
static void GetSliver(const uint8_t **p,SnesRecompObjSliver *s) {
    s->screen_x=(int16_t)GetU16(p);s->line=GetU16(p);s->tile=GetU16(p);
    s->row=GetU8(p);s->palette_base=GetU8(p);s->priority=GetU8(p);s->flip_h=GetU8(p);
    s->oam_slot=GetU8(p);s->math_eligible=GetU8(p);
}
static bool WritePrimitiveTrace(const char *path,const SnesRecompPpuJobView *v,
    uint32_t frame_id,uint32_t accepted,uint32_t reject_reason) {
    if(!path || !v->graphics_frame || v->overlay_count || v->source_count ||
        (v->blits && v->blits->rect_count)) return false;
    const SnesRecompPpuFrame *frames[2]={v->frame,v->graphics_frame};
    uint8_t *captures[2]={NULL,NULL};size_t sizes[2]={0,0},bytes=TRACE_PRIMITIVE_HEADER_BYTES;
    const SnesRecompObjFrame *objects[2];uint32_t flags=0;
    for(unsigned i=0;i<2;++i) {
        if(!EncodeTrace(captures+i,sizes+i,snesrecomp_ppu_frame_capture(frames[i]),
            frame_id,accepted,reject_reason,NULL)) {free(captures[0]);free(captures[1]);return false;}
        objects[i]=snesrecomp_ppu_frame_obj(frames[i]);
        if(objects[i]) {
            flags|=(1u|(objects[i]->range_over?2u:0u)|(objects[i]->time_over?4u:0u))<<(3u*i);
            bytes+=(size_t)objects[i]->count*12u+snesrecomp_ppu_frame_capture(frames[i])->visible_height;
        }
        bytes+=sizes[i];
    }
    bytes+=(size_t)v->primitive_count*24u+(size_t)v->material_count*8u+(size_t)v->image_count*8u;
    for(unsigned i=0;i<v->image_count;++i) bytes+=(size_t)v->images[i].width*v->images[i].height*4u;
    if(bytes>TRACE_PRIMITIVE_MAX_BYTES) {free(captures[0]);free(captures[1]);return false;}
    uint8_t *buf=calloc(1,bytes);
    if(!buf) {free(captures[0]);free(captures[1]);return false;}
    uint8_t *p=buf+TRACE_PRIMITIVE_HEADER_BYTES;
    for(unsigned i=0;i<2;++i) {memcpy(p,captures[i],sizes[i]);p+=sizes[i];free(captures[i]);}
    for(uint32_t i=0;i<v->primitive_count;++i) PutPrimitive(&p,v->primitives+i);
    for(unsigned i=0;i<v->material_count;++i) {
        const SnesRecompPpuMaterial *m=v->materials+i;
        PutU8(&p,m->main_enable);PutU8(&p,m->occluding_bg);PutU8(&p,m->brightness_reduction);PutU8(&p,m->flags);PutU32(&p,m->reserved);
    }
    for(unsigned i=0;i<2;++i) if(objects[i]) {
        for(unsigned k=0;k<objects[i]->count;++k) PutSliver(&p,objects[i]->slivers+k);
        const unsigned height=snesrecomp_ppu_frame_capture(frames[i])->visible_height;
        memcpy(p,objects[i]->line_priority,height);p+=height;
    }
    for(unsigned i=0;i<v->image_count;++i) {
        const SnesRecompPpuJobImage *image=v->images+i;
        PutU16(&p,image->width);PutU16(&p,image->height);PutU32(&p,(uint32_t)image->width*image->height);
    }
    for(unsigned i=0;i<v->image_count;++i) {
        const SnesRecompPpuJobImage *image=v->images+i;
        for(unsigned y=0;y<image->height;++y) for(unsigned x=0;x<image->width;++x)
            PutU32(&p,image->pixels[(size_t)y*image->pitch+x]);
    }
    const uint64_t hash=snesrecomp_ppu_trace_hash(buf+TRACE_PRIMITIVE_HEADER_BYTES,bytes-TRACE_PRIMITIVE_HEADER_BYTES);
    p=buf;memcpy(p,SNES_PPU_TRACE_MAGIC,SNES_PPU_TRACE_MAGIC_BYTES);p+=SNES_PPU_TRACE_MAGIC_BYTES;
    PutU32(&p,SNES_PPU_TRACE_PRIMITIVE_JOB_VERSION);PutU32(&p,TRACE_PRIMITIVE_HEADER_BYTES);PutU64(&p,bytes);
    PutU32(&p,frame_id);PutU32(&p,accepted);PutU32(&p,reject_reason);
    PutU32(&p,(uint32_t)sizes[0]);PutU32(&p,(uint32_t)sizes[1]);
    PutU32(&p,v->primitive_count);PutU32(&p,v->material_count);PutU32(&p,v->image_count);
    PutU32(&p,objects[0]?objects[0]->count:0);PutU32(&p,objects[1]?objects[1]->count:0);
    PutU64(&p,v->sequence);PutU64(&p,hash);PutU32(&p,flags);
    return WriteBuffer(path,buf,bytes);
}
static bool DecodePrimitiveTrace(uint8_t *buf,size_t bytes,SnesPpuTrace *out,
    SnesRecompPpuJob **job,char *error,size_t error_bytes) {
    if(!job) {free(buf);return Fail(out,error,error_bytes,"primitive trace requires job reader");}
    const uint8_t *p=buf+16;
    const uint32_t header=GetU32(&p);const uint64_t total=GetU64(&p);
    if(header!=TRACE_PRIMITIVE_HEADER_BYTES || total!=bytes || bytes<header) {
        free(buf);return Fail(out,error,error_bytes,"primitive header layout mismatch");
    }
    const uint32_t frame_id=GetU32(&p),accepted=GetU32(&p),reason=GetU32(&p);
    const uint32_t sizes[2]={GetU32(&p),GetU32(&p)};
    const uint32_t primitive_count=GetU32(&p),material_count=GetU32(&p),image_count=GetU32(&p);
    const uint32_t counts[2]={GetU32(&p),GetU32(&p)};
    const uint64_t sequence=GetU64(&p),hash=GetU64(&p);const uint32_t flags=GetU32(&p);
    const size_t fixed=(size_t)sizes[0]+sizes[1]+(size_t)primitive_count*24u+
        (size_t)material_count*8u+(size_t)image_count*8u+(size_t)(counts[0]+counts[1])*12u;
    if(!primitive_count || primitive_count>SNESRECOMP_PPU_JOB_MAX_PRIMITIVES ||
       !material_count || material_count>SNESRECOMP_PPU_JOB_MAX_MATERIALS ||
       image_count>SNESRECOMP_PPU_JOB_MAX_IMAGES || sizes[0]<TRACE_HEADER_V2_BYTES ||
       sizes[1]<TRACE_HEADER_V2_BYTES || sizes[0]>TRACE_LEGACY_MAX_BYTES || sizes[1]>TRACE_LEGACY_MAX_BYTES ||
       counts[0]>SNESRECOMP_OBJ_MAX_SLIVERS || counts[1]>SNESRECOMP_OBJ_MAX_SLIVERS ||
       flags>63u || (!(flags&1u) && ((flags&6u) || counts[0])) ||
       (!(flags&8u) && ((flags&48u) || counts[1])) || fixed>bytes-header ||
       snesrecomp_ppu_trace_hash(p,bytes-header)!=hash) {
        free(buf);return Fail(out,error,error_bytes,"invalid primitive payload");
    }
    SnesPpuTrace traces[2]={{0},{0}};
    SnesRecompPpuFrame *frames[2]={NULL,NULL};
    SnesRecompObjFrame objects[2]={{0},{0}};
    SnesRecompPpuPrimitive *primitives=NULL;SnesRecompPpuMaterial materials[SNESRECOMP_PPU_JOB_MAX_MATERIALS];
    SnesRecompPpuJobImage images[SNESRECOMP_PPU_JOB_MAX_IMAGES]={0};
    uint32_t *pixels=NULL;SnesRecompPpuJob *decoded=NULL;bool ok=false;
    const char *failure="primitive capture allocation failed";
    char failure_detail[128];
    for(unsigned i=0;i<2;++i) {
        uint8_t *copy=malloc(sizes[i]);if(!copy) goto cleanup;
        memcpy(copy,p,sizes[i]);p+=sizes[i];
        if(!DecodeTrace(copy,sizes[i],traces+i,NULL,error,error_bytes)) {failure="invalid nested primitive capture";goto cleanup;}
        failure="primitive capture metadata mismatch";
        if(traces[i].frame_id!=frame_id || traces[i].accepted!=accepted || traces[i].reject_reason!=reason) goto cleanup;
    }
    size_t expected=header+fixed;
    for(unsigned i=0;i<2;++i) if(flags&(1u<<(3u*i))) expected+=traces[i].capture.visible_height;
    if(expected>bytes) goto cleanup;
    failure="primitive record allocation failed";
    primitives=calloc(primitive_count,sizeof *primitives);if(!primitives) goto cleanup;
    for(uint32_t i=0;i<primitive_count;++i) GetPrimitive(&p,primitives+i);
    for(unsigned i=0;i<material_count;++i) {
        SnesRecompPpuMaterial *m=materials+i;
        m->main_enable=GetU8(&p);m->occluding_bg=GetU8(&p);m->brightness_reduction=GetU8(&p);m->flags=GetU8(&p);m->reserved=GetU32(&p);
    }
    for(unsigned i=0;i<2;++i) if(flags&(1u<<(3u*i))) {
        objects[i].count=objects[i].capacity=counts[i];
        objects[i].line_capacity=traces[i].capture.visible_height;
        objects[i].slivers=counts[i]?calloc(counts[i],sizeof *objects[i].slivers):NULL;
        objects[i].line_priority=malloc(objects[i].line_capacity);
        if((counts[i] && !objects[i].slivers) || !objects[i].line_priority) goto cleanup;
        for(unsigned k=0;k<counts[i];++k) GetSliver(&p,objects[i].slivers+k);
        memcpy(objects[i].line_priority,p,objects[i].line_capacity);p+=objects[i].line_capacity;
        objects[i].range_over=(flags&(2u<<(3u*i)))!=0;objects[i].time_over=(flags&(4u<<(3u*i)))!=0;
    }
    unsigned pixel_count=0;
    for(unsigned i=0;i<image_count;++i) {
        images[i].width=GetU16(&p);images[i].height=GetU16(&p);images[i].pitch=images[i].width;
        const uint32_t count=GetU32(&p);
        if(!images[i].width || images[i].width>SNESRECOMP_PPU_MAX_WIDTH || !images[i].height ||
           images[i].height>SNES_PPU_MAX_BANDS || count!=(uint32_t)images[i].width*images[i].height ||
           count>SNESRECOMP_PPU_JOB_MAX_PIXELS-pixel_count) goto cleanup;
        pixel_count+=count;
    }
    failure="primitive image payload size mismatch";
    if(expected+(size_t)pixel_count*4u!=bytes) goto cleanup;
    pixels=pixel_count?malloc((size_t)pixel_count*sizeof *pixels):NULL;
    if(pixel_count && !pixels) goto cleanup;
    unsigned at=0;
    for(unsigned i=0;i<image_count;++i) {
        images[i].pixels=pixels+at;
        for(unsigned k=0;k<(unsigned)images[i].width*images[i].height;++k) pixels[at++]=GetU32(&p);
    }
    for(unsigned i=0;i<2;++i) {
        frames[i]=snesrecomp_ppu_frame_create();
        failure="primitive frame allocation failed";if(!frames[i]) goto cleanup;
        const SnesRecompPpuResult result=snesrecomp_ppu_frame_freeze(frames[i],&traces[i].capture,
            flags&(1u<<(3u*i))?objects+i:NULL,sequence);
        if(result!=SNESRECOMP_PPU_OK) {failure=i?"invalid primitive graphics frame":"invalid primitive base frame";goto cleanup;}
    }
    decoded=snesrecomp_ppu_job_create_primitives(pixel_count,primitive_count);
    failure="primitive job allocation failed";if(!decoded) goto cleanup;
    const SnesRecompPpuResult result=snesrecomp_ppu_job_freeze_primitives(decoded,frames[0],frames[1],images,
        (uint16_t)image_count,primitives,primitive_count,materials,(uint16_t)material_count,sequence);
    if(result!=SNESRECOMP_PPU_OK) {
        snprintf(failure_detail,sizeof failure_detail,"primitive records: %s (%u); count=%lu materials=%lu first=%u,%u/%u,%u z=%04x kind=%u material=%u",
            snesrecomp_ppu_result_text(result),(unsigned)result,(unsigned long)primitive_count,(unsigned long)material_count,
            primitives[0].x,primitives[0].y,primitives[0].width,primitives[0].height,primitives[0].priority,
            primitives[0].kind,primitives[0].material);
        failure=failure_detail;goto cleanup;
    }
    *out=traces[0];memset(traces,0,sizeof traces[0]);out->format_version=SNES_PPU_TRACE_PRIMITIVE_JOB_VERSION;
    *job=decoded;decoded=NULL;ok=true;
cleanup:
    for(unsigned i=0;i<2;++i) {
        snesrecomp_ppu_trace_free(traces+i);snesrecomp_ppu_frame_release(frames[i]);
        free(objects[i].slivers);free(objects[i].line_priority);
    }
    snesrecomp_ppu_job_release(decoded);free(primitives);free(pixels);free(buf);
    return ok || Fail(out,error,error_bytes,failure);
}
