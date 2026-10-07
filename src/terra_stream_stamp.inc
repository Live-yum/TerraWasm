/* Sparse overlay controls travel as bounded JSON; cell records stay in an
* immutable host source. Validate once, then replay 64 KiB windows during the
* existing candidate writer. Cancellation never adopts this candidate. */
static int prepare_stamp(StreamTask* t,int length) {
    int32_t values[6];
    const char* names[]= {
        "x","y","width","height","recordCount","recordSourceId"
    };
    for(uint32_t i=0;i<6;i++) {
        int p=json_find_key(t->request,length,names[i]);
        if(p<0||!json_extract_int(t->request,length,p,values+i)||values[i]<0)return 0;
    }
    char mode[16];
    int pos=json_find_key(t->request,length,"mode");
    if(pos<0||!json_extract_str(t->request,length,pos,mode,sizeof(mode))||strcmp(mode,"overlay"))return 0;
    uint32_t x=values[0],y=values[1],width=values[2],height=values[3],count=values[4],source=values[5];
    if(!width||!height||!count||count>1048576u||!source||source==t->source_id ||
       x>=(uint32_t)t->candidate->maxTilesX ||
       y>=(uint32_t)t->candidate->maxTilesY ||
       width>(uint32_t)t->candidate->maxTilesX-x ||
       height>(uint32_t)t->candidate->maxTilesY-y)return 0;
    StreamStamp* s=(StreamStamp*)tx_persistent_alloc(sizeof(*s));
    if(!s)return 0;
    memset(s,0,sizeof(*s));
    s->x=x;
    s->y=y;
    s->width=width;
    s->height=height;
    s->count=count;
    s->source_id=source;
    t->stamp=s;
    int p=json_find_key(t->request,length,"objectSourceId");
    int b=json_find_key(t->request,length,"objectBytes");
    int c=json_find_key(t->request,length,"objectCount");
    if(p>=0||b>=0||c>=0) {
        int32_t id,bytes,objects;
        if(p<0||b<0||c<0 ||
           !json_extract_int(t->request,length,p,&id) ||
           !json_extract_int(t->request,length,b,&bytes) ||
           !json_extract_int(t->request,length,c,&objects) ||
           id<=0||(uint32_t)id==source||(uint32_t)id==t->source_id ||
           bytes<32||bytes>CO_LIMIT||objects<0||objects>CO_COUNT)return 0;
        if(objects&&t->candidate->version!=326)return 0;
        s->object_source=(uint32_t)id;
        s->object_length=(uint32_t)bytes;
        s->object_count=(uint32_t)objects;
        s->objects=tx_persistent_alloc(s->object_length);
        if(!s->objects)return 0;
    }
    return 1;
}
static CoItem* stamp_cell_object(StreamStamp*,uint32_t,uint32_t);
static int stamp_objects_prepare(StreamTask* t) {
    StreamStamp* s=t->stamp;
    if(s->object_loaded<s->object_length) {
        uint32_t n=s->object_length-s->object_loaded;
        if(n>65536)n=65536;
        event(t,TX_STREAM_NEED_SOURCE,s->object_loaded,n,NULL);
        t->event.source_id=s->object_source;
        return 0;
    }
    const uint8_t* p=s->objects;
    uint32_t end=s->object_length;
    if(co_u32(p)!=CO_MAGIC||co_u32(p+4)!=1 ||
       co_u32(p+12)!=s->object_count||co_u32(p+16)!=end||co_u32(p+28) ||
       !co_u32(p+8)||co_u32(p+8)>326 ||
       (s->object_count&&co_u32(p+8)!=326))goto invalid;
    if(co_u32(p+20)>INT32_MAX-s->width||co_u32(p+24)>INT32_MAX-s->height)goto invalid;
    if(s->object_count) {
        s->items=(CoItem*)tx_persistent_alloc(s->object_count*sizeof(CoItem));
        if(!s->items)return fail("TERRAX_WASM_OOM","object index allocation failed");
        memset(s->items,0,s->object_count*sizeof(CoItem));
    }
    uint32_t off=32;
    for(uint32_t i=0;i<s->object_count;i++) {
        if(!co_take(&off,32,end))goto invalid;
        const uint8_t* h=p+off-32;
        CoItem* a=s->items+i;
        a->section=co_u32(h);
        a->kind=co_u32(h+4);
        a->x=co_u32(h+8);
        a->y=co_u32(h+12);
        a->tile=co_u32(h+16);
        a->length=co_u32(h+20);
        a->payload=p+off;
        if(co_u32(h+24)||co_u32(h+28) ||
           !co_section(a->tile)||co_section(a->tile)!=a->section ||
           (a->section==5?co_entity_tile(a->kind)!=a->tile:a->kind!=0) ||
           !co_take(&off,a->length,end))goto invalid;
        uint32_t n=0,shape=co_shape(a->tile),w=shape&255,hh=shape>>8;
        if(!co_payload(a->section,a->kind,a->payload,a->length,&n)||n!=a->length ||
           a->x>=s->width||a->y>=s->height||w>s->width-a->x||hh>s->height-a->y ||
           (a->section==5&&(s->x+a->x>INT16_MAX||s->y+a->y>INT16_MAX)))goto invalid;
    }
    if(off!=end)goto invalid;
    if(s->object_count>1)qsort(s->items,s->object_count,sizeof(CoItem),co_item_compare);
    for(uint32_t i=1;i<s->object_count;i++)if(!co_item_compare(s->items+i-1,s->items+i))goto invalid;
    if(s->object_count) {
        const uint32_t sections[]= {
            2,3,5
        };
        for(uint32_t k=0;k<3;k++) {
            CoCursor c;
            if(!co_cursor(t->candidate,sections[k],&c))goto invalid;
            uint32_t added=0;
            for(uint32_t i=0;i<s->object_count;i++)added+=s->items[i].section==sections[k];
            if((sections[k]==2&&added>8000-c.count)||(sections[k]==3&&added>32000-c.count))goto invalid;
            CoItem a;
            int next;
            while((next=co_next(&c,&a))==1) {
                if(a.section==5) {
                    uint32_t id=co_u32(c.bytes+a.offset+1);
                    if(id>INT32_MAX-s->object_count)goto invalid;
                    if(id>=s->object_max_id)s->object_max_id=id+1;
                }
                if(a.x>=s->x&&a.y>=s->y&&stamp_cell_object(s,a.x-s->x,a.y-s->y)) {
                    return fail("TERRAX_STAMP_OBJECT_CONFLICT",
                                "an object record already occupies the target footprint");
                }
            }
            if(next!=2)goto invalid;
        }
    }
    s->objects_valid=1;
    return 1;
invalid:
    return fail("TERRAX_INVALID_STAMP","malformed, duplicate or incompatible object payload");
}
static CoItem* stamp_cell_object(StreamStamp* s,uint32_t x,uint32_t y) {
    CoItem* found=NULL;
    for(uint32_t dx=0;dx<3&&dx<=x;dx++)for(uint32_t dy=0;dy<4&&dy<=y;dy++) {
        CoItem* a=co_find(s->items,s->object_count,x-dx,y-dy);
        if(a) {
            uint32_t shape=co_shape(a->tile);
            if(dx<(shape&255)&&dy<(shape>>8)) {
                if(found)return NULL;
                found=a;
            }
        }
    }
    return found;
}
static int stamp_object_cell(StreamTask* t,const uint32_t* r,const TxTile* tile) {
    StreamStamp* s=t->stamp;
    CoItem* a=stamp_cell_object(s,r[0],r[1]);
    if(!a||a->tile!=tile->type)return 0;
    uint32_t dx=r[0]-a->x,dy=r[1]-a->y,w=co_shape(a->tile)&255,bit=1u<<(dx+dy*w);
    if(!dx&&!dy) {
        if(!co_root_frame(tile->type,tile->frame_x,tile->frame_y))return 0;
        a->offset=(uint16_t)tile->frame_x|((uint32_t)(uint16_t)tile->frame_y<<16);
    }
    if(!(a->seen&1u)&&bit!=1u)return 0;
    if(tile->frame_x!=(int16_t)a->offset+(int32_t)dx*18||tile->frame_y!=(int16_t)(a->offset>>16)+(int32_t)dy*18)return 0;
    if(t->stage==STAMP_VALIDATE) {
        if(a->seen&bit)return 0;
        a->seen|=bit;
    }
    return 1;
}
/* Validate the complete foreground coverage before any output is visible. */
static uint32_t stamp_objects_size(StreamStamp* s, uint32_t old_size,
                                   uint32_t* new_count) {
    uint64_t total=old_size;
    for(uint32_t i=0;i<s->object_count;i++) {
        CoItem* a=s->items+i;
        uint32_t shape=co_shape(a->tile),cells=(shape&255)*(shape>>8);
        if(a->seen!=(1u<<cells)-1)return 0;
        uint32_t n=a->length+(a->section==5?9:8);
        new_count[a->section]++;
        total+=n;
    }
    return total<=METADATA_LIMIT?(uint32_t)total:0;
}

/* Payloads retain their source encoding, including arbitrary length UTF-8 and
 * item arrays. Only the relocated anchors and world-local entity IDs change. */
static uint32_t stamp_append_object(uint8_t* output, const CoItem* a,
                                    uint32_t x, uint32_t y, uint32_t* next_id) {
    uint32_t at=0;
    if(a->section==3) {
        memcpy(output,a->payload,a->length);
        co_put(output+a->length,x);
        co_put(output+a->length+4,y);
        return a->length+8;
    }
    if(a->section==5) {
        output[at++]=(uint8_t)a->kind;
        co_put(output+at,(*next_id)++);
        at+=4;
        put16(output+at,x);
        put16(output+at+2,y);
        at+=4;
    } else {
        co_put(output+at,x);
        co_put(output+at+4,y);
        at+=8;
    }
    memcpy(output+at,a->payload,a->length);
    if(a->section==5&&a->kind==0) {
        /* A dummy's NPC slot belongs to the source world's runtime. Terraria
         * recreates it in the destination when the saved slot is -1. */
        output[at]=output[at+1]=255;
    }
    return at+a->length;
}

static uint32_t stamp_append_section(StreamTask* t, uint32_t section_id,
                                     uint32_t count, uint32_t at,
                                     uint32_t* next_id) {
    StreamStamp* s=t->stamp;
    uint8_t* output=t->candidate->file;
    uint32_t length;
    /* section() sees prior queued overrides. compact_copy used the same views
     * to size the candidate, so neither data nor lengths come from stale WLD. */
    const uint8_t* source=section(t->source,section_id,&length);
    memcpy(output+at,source,length);
    if(!count)return at+length;
    count+=section_id==5?co_u32(source):co_u16(source);
    if(section_id==5)co_put(output+at,count);
    else put16(output+at,count);
    at+=length;
    for(uint32_t i=0;i<s->object_count;i++) {
        CoItem* a=s->items+i;
        if(a->section==section_id) {
            at+=stamp_append_object(output+at,a,s->x+a->x,s->y+a->y,next_id);
        }
    }
    return at;
}

static int stamp_objects_finish(StreamTask* t) {
    StreamStamp* s=t->stamp;
    if(!s->object_count)return 1;
    TxWorld* w=t->candidate;
    uint32_t new_count[TX_MAX_SECTIONS]={0};
    uint32_t total=stamp_objects_size(s,w->file_len,new_count);
    if(!total)return 0;
    /* Source metadata stays immutable. Release the temporary compact candidate
     * before allocating its final image: never retain three metadata images.
     * The caller latches any failure; only cancel/close may follow an OOM. */
    memcpy(t->patch,w->file,w->format_len);
    tx_internal_free(w->file);
    w->file=NULL;
    w->file_len=0;
    w->file=tx_persistent_alloc(total);
    if(!w->file)return 0;
    w->file_len=total;
    memcpy(w->file,t->patch,w->format_len);
    uint32_t at=w->format_len,table=w->version>=135?26:6,next_id=s->object_max_id;
    for(uint32_t section_id=0;section_id<w->pointer_count;section_id++) {
        co_put(w->file+table+4*section_id,at);
        if(section_id==1)continue;
        at=stamp_append_section(t,section_id,new_count[section_id],at,&next_id);
    }
    return at==total&&parse_format(w)&&parse_header(w);
}
/* 0=source requested, 1=record present, 2=end. The event range, lease pointer,
* and legacy supply path all use the same independently owned stamp window. */
static int stamp_record(StreamTask* t,uint32_t* record) {
    StreamStamp* s=t->stamp;
    if(s->index==s->count)return 2;
    uint32_t off=s->index*32u;
    if(off<s->offset||off-s->offset>=s->length||s->length-(off-s->offset)<32u) {
        uint32_t n=s->count*32u-off;
        if(n>sizeof(s->bytes))n=sizeof(s->bytes);
        event(t,TX_STREAM_NEED_SOURCE,off,n,NULL);
        t->event.source_id=s->source_id;
        return 0;
    }
    for(uint32_t i=0;i<8;i++)record[i]=u32(s->bytes+off-s->offset+i*4);
    return 1;
}
static int stamp_validate_record(StreamTask* t,const uint32_t* r) {
    StreamStamp* s=t->stamp;
    TxTile tile;
    if(r[0]>=s->width||r[1]>=s->height||!tx_tile_unrecord(r,&tile))return 0;
    if(s->has_previous&&(r[0]<s->previous_x||(r[0]==s->previous_x&&r[1]<=s->previous_y)))return 0;
    if(tile.active&&(tile.type>=t->candidate->tile_type_count||tile.type==0xfffeu))return 0;
    if(tile.active&&tx_tile_needs_section(tile.type)&&!stamp_object_cell(t,r,&tile))return 0;
    /* Minecart.FrameTrack uses -1 for an absent back track (and its newly
    * placed front track). They are signed track IDs, not atlas coordinates. */
    if(tile.active&&tx_tile_framed(t->candidate,tile.type)&&(tile.frame_x<(tile.type==314?-1:0)||tile.frame_y<(tile.type==314?-1:0)))return 0;
    if(t->candidate->version<269u &&
       (tile.liquid_type==4||tile.invisible_block||tile.invisible_wall ||
        tile.fullbright_block||tile.fullbright_wall))return 0;
    s->previous_x=r[0];
    s->previous_y=r[1];
    s->has_previous=1;
    return 1;
}
static int stamp_overlay(StreamTask* t,const uint32_t* r,TxTile* tile) {
    TxTile source;
    if(!tx_tile_unrecord(r,&source))return 0;
    if(source.active) {
        if(tx_tile_needs_section(source.type)&&tile->active&&(tx_tile_framed(t->candidate,tile->type)||tx_tile_needs_section(tile->type)))return 0;
        if(tile->active &&
           (tx_tile_framed(t->candidate,tile->type)||tx_tile_needs_section(tile->type)) &&
           (tile->type!=source.type||tile->frame_x!=source.frame_x ||
            tile->frame_y!=source.frame_y))return 0;
        tile->active=1;
        tile->type=source.type;
        tile->frame_x=source.frame_x;
        tile->frame_y=source.frame_y;
        tile->tile_color=source.tile_color;
        tile->brick_style=source.brick_style;
        tile->actuator=source.actuator;
        tile->inactive=source.inactive;
        tile->invisible_block=source.invisible_block;
        tile->fullbright_block=source.fullbright_block;
    } else if(source.actuator) {
        tile->actuator=1;
        tile->inactive=source.inactive;
    }
    tile->wire_red|=source.wire_red;
    tile->wire_blue|=source.wire_blue;
    tile->wire_green|=source.wire_green;
    tile->wire_yellow|=source.wire_yellow;
    if(source.wall) {
        tile->wall=source.wall;
        tile->wall_color=source.wall_color;
        tile->invisible_wall=source.invisible_wall;
        tile->fullbright_wall=source.fullbright_wall;
    }
    if(source.liquid_amount) {
        tile->liquid_amount=source.liquid_amount;
        tile->liquid_type=source.liquid_type;
    }
    return 1;
}
