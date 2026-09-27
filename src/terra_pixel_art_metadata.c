/* Preserve untouched records verbatim; a replaced object loses its metadata. */
#include "terra_types.h"
#include "terra_reader.h"
extern int apply_pixel_art_at(TxWorld*,uint32_t,uint32_t,TxTile*);
extern uint16_t rd_u16le(const uint8_t*,uint32_t,uint32_t*);
extern uint32_t rd_u32le(const uint8_t*,uint32_t,uint32_t*);
extern int legacy_skip_string(const uint8_t*,uint32_t,uint32_t*);
extern void buf_init(TxBuf*,uint32_t);
extern void buf_bytes(TxBuf*,const void*,uint32_t);
extern void tx_internal_free(void*);
extern void tx_set_error(const char*,const char*);
extern int set_section_override_data(TxWorld*,int,uint8_t*,uint32_t);
extern uint8_t* tx_alloc(uint32_t);
extern int read_tile_at(TxWorld*,uint32_t*,uint32_t,TxTile*);

/* Dresser metadata uses the chest section but its footprint is 3x2. Build
 * column offsets only when a write could hit a chest's third column. */
static int is_dresser(TxWorld* w,int32_t x,int32_t y,uint32_t** columns) {
    if (x<0 || y<0 || x>=w->maxTilesX || y>=w->maxTilesY) return 0;
    uint8_t* saved=w->file;
    uint32_t saved_len=w->file_len,end=w->ends[1],start=w->starts[1];
    if (w->section_overrides[1].active) {
        w->file=w->section_overrides[1].data; w->file_len=w->section_overrides[1].len;
        start=0; end=w->file_len;
    }
    int result=-1;
    if (!*columns) {
        *columns=(uint32_t*)tx_alloc((uint32_t)w->maxTilesX*4u);
        if (!*columns) goto done;
        uint32_t off=start;
        for (uint32_t cx=0;cx<(uint32_t)w->maxTilesX;cx++) {
            (*columns)[cx]=off;
            for (uint32_t cy=0;cy<(uint32_t)w->maxTilesY;) {
                TxTile tile;
                if (!read_tile_at(w,&off,end,&tile)) goto done;
                cy+=(uint32_t)tile.same+1u;
            }
        }
    }
    uint32_t off=(*columns)[x];
    for (uint32_t cy=0;cy<(uint32_t)w->maxTilesY;) {
        TxTile tile;
        if (!read_tile_at(w,&off,end,&tile)) goto done;
        cy+=(uint32_t)tile.same+1u;
        if (cy>(uint32_t)y) { result=tile.active && tile.type==88u; break; }
    }
done:
    w->file=saved; w->file_len=saved_len;
    return result;
}

static int replaced_object(TxWorld* w,int32_t x,int32_t y,uint32_t width,uint32_t height) {
    for (uint32_t dx=0;dx<width;dx++) for (uint32_t dy=0;dy<height;dy++) {
        TxTile tile={0};
        int64_t cx=(int64_t)x+dx,cy=(int64_t)y+dy;
        if (cx>=0 && cy>=0 && cx<w->maxTilesX && cy<w->maxTilesY &&
            apply_pixel_art_at(w,(uint32_t)cx,(uint32_t)cy,&tile)) return 1;
    }
    return 0;
}

static uint32_t bit_count(uint32_t mask) {
    uint32_t count=0;
    for (;mask;mask>>=1u) count+=mask&1u;
    return count;
}

static int skip_entity(TxWorld* w,const uint8_t* p,uint32_t len,uint32_t* off,uint32_t type) {
    uint32_t bytes;
    switch (type) {
    case 0: case 2: case 9: case 10: bytes=2; break;
    case 1: case 4: case 6: case 8: bytes=5; break;
    case 7: bytes=0; break;
    case 3: {
        uint32_t flags=2u+(w->version>=307u)+(w->version>=308u);
        if (!terra_reader_has(*off,flags,len)) return 0;
        uint32_t count=bit_count(p[*off])+bit_count(p[*off+1]);
        if (w->version>=308u) count+=bit_count(p[*off+3]&7u);
        *off+=flags; bytes=count*5u; break;
    }
    case 5:
        if (!terra_reader_has(*off,1u,len)) return 0;
        bytes=bit_count(p[(*off)++]&15u)*5u; break;
    default: return 0;
    }
    return terra_reader_take(off,bytes,len);
}

static int filter_metadata(TxWorld* w,int section,TxBuf* out,uint32_t** columns) {
    int actual=section;
    if ((section==5 && w->version<116u) || (section==6 && w->version<170u) ||
        (uint32_t)actual>=w->pointer_count) return 1;
    if (actual<0) return 1;
    TxSectionOverride* old=&w->section_overrides[section];
    const uint8_t* p=old->active?old->data:w->file+w->starts[actual];
    uint32_t len=old->active?old->len:w->ends[actual]-w->starts[actual],off=0;
    uint32_t count_bytes=section>=5?4u:2u;
    if (!terra_reader_has(off,count_bytes,len)) return 0;
    uint32_t count=section>=5?rd_u32le(p,len,&off):rd_u16le(p,len,&off),kept=0;
    uint32_t slots=0;
    if (section==2 && w->version<294u) {
        if (!terra_reader_has(off,2u,len)) return 0;
        slots=rd_u16le(p,len,&off);
    }
    buf_init(out,len);
    buf_bytes(out,p,off);
    for (uint32_t i=0;i<count;i++) {
        uint32_t start=off,type=0,width=section==6?1u:2u,height=section==6?1u:2u;
        int32_t x,y;
        if (section==3 && !legacy_skip_string(p,len,&off)) return 0;
        if (section==5 && w->version>=122u) {
            if (!terra_reader_take(&off,5u,len)) return 0;
            type=p[start];
        }
        uint32_t coord_bytes=section==5?4u:8u;
        if (!terra_reader_has(off,coord_bytes,len)) return 0;
        x=section==5?(int16_t)rd_u16le(p,len,&off):(int32_t)rd_u32le(p,len,&off);
        y=section==5?(int16_t)rd_u16le(p,len,&off):(int32_t)rd_u32le(p,len,&off);
        if (section==2) {
            if (!legacy_skip_string(p,len,&off)) return 0;
            if (w->version>=294u) {
                if (!terra_reader_has(off,4u,len)) return 0;
                slots=rd_u32le(p,len,&off);
            }
            if (slots>504u) return 0;
            for (uint32_t slot=0;slot<slots;slot++) {
                if (!terra_reader_has(off,2u,len)) return 0;
                if (rd_u16le(p,len,&off) && !terra_reader_take(&off,5u,len)) return 0;
            }
        } else if (section==5) {
            static const uint8_t widths[]={2,2,1,2,3,3,1,3,1,1,1};
            static const uint8_t heights[]={3,2,1,3,3,4,1,4,2,1,1};
            if (type>10u) return 0;
            width=widths[type]; height=heights[type];
            if (w->version>=122u && !skip_entity(w,p,len,&off,type)) return 0;
        }
        int replaced=replaced_object(w,x,y,width,height);
        if (!replaced && section==2 && x<=INT32_MAX-2 && replaced_object(w,x+2,y,1,2)) {
            int dresser=is_dresser(w,x,y,columns);
            if (dresser<0) return 0;
            replaced=dresser;
        }
        if (!replaced) { buf_bytes(out,p+start,off-start); kept++; }
    }
    if (off!=len || !out->ok) return 0;
    if (kept==count) { tx_internal_free(out->data); out->data=NULL; return 1; }
    for (uint32_t i=0;i<count_bytes;i++) out->data[i]=(uint8_t)(kept>>(8u*i));
    return 1;
}

int txw_filter_pixel_art_metadata(TxWorld* w) {
    const int sections[]={2,3,5,6};
    TxBuf filtered[4]={{0}};
    uint32_t* columns=NULL;
    for (uint32_t i=0;i<4u;i++) {
        if (!filter_metadata(w,sections[i],&filtered[i],&columns)) {
            if (columns) tx_internal_free(columns);
            for (uint32_t j=0;j<4u;j++) if (filtered[j].data) tx_internal_free(filtered[j].data);
            tx_set_error("TERRAX_PARSE_ERROR","pixel art metadata is malformed or unsupported");
            return 0;
        }
    }
    if (columns) tx_internal_free(columns);
    for (uint32_t i=0;i<4u;i++) if (filtered[i].data)
        set_section_override_data(w,sections[i],filtered[i].data,filtered[i].len);
    return 1;
}
