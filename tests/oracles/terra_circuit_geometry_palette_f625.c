/* Bounded immutable source geometry with independent resumable decoding. */
#include "terra_circuit_geometry.h"
#include <string.h>
extern int read_tile_at(TxWorld*,uint32_t*,uint32_t,TxTile*);
typedef struct CxColdColumn { uint32_t valid,column,offset,length,runs,format,source_end; } CxColdColumn;
typedef struct CxColdScratch { uint16_t map[512];uint64_t seen[8]; } CxColdScratch;
struct CxOrderGeometry {
    CxColdScratch* cold_scratch;uint32_t cold_count,cold_bits,cold_accumulator,cold_accum_bits,cold_palette_scan;
    uint8_t* cold_bytes;CxColdColumn* cold_entries;
    uint32_t cold_head,cold_victim,cold_phase,cold_scan,cold_row,cold_runs,cold_offset,cold_length,cold_format,cold_written;
    uint16_t cold_last;
    uint8_t *cells,*compiler_axis,*window;
    uint32_t column[CX_GEOMETRY_SLOTS],source_end[CX_GEOMETRY_SLOTS],valid[CX_GEOMETRY_SLOTS],age[CX_GEOMETRY_SLOTS];
    uint32_t active,slot,cursor,end,row,run_left,window_offset,window_length,ticket;
    uint32_t scan_column,target_column;
    uint32_t sequential_valid,sequential_column,sequential_cursor;
    CxGeometryStats stats;
    TxWorld decoder; /* zero-initialized once, never copies an owner */
    uint8_t packed,axis;
    CxGeometryRead pending;
};
static uint32_t cold_set(uint32_t column){return ((column*0x9e3779b1u)^(column>>16))&(CX_GEOMETRY_COLD_ENTRIES/CX_GEOMETRY_COLD_WAYS-1u);}
static uint16_t cold_u16(const uint8_t* p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
static uint32_t cold_u32(const uint8_t* p){return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void cold_put16(uint8_t* p,uint16_t n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void cold_put32(uint8_t* p,uint32_t n){for(uint32_t i=0;i<4;i++)p[i]=(uint8_t)(n>>(8u*i));}
/* L2 payload is immutable after publication; no external source pointers. */
static int cold_get(CxWorld* w,uint32_t column,uint32_t row,uint8_t* word,uint8_t* axis){
    CxOrderGeometry* g=w->geometry;if(!g->cold_entries)return TCW_STATE;uint32_t set=cold_set(column)*CX_GEOMETRY_COLD_WAYS;
    for(uint32_t i=0;i<CX_GEOMETRY_COLD_WAYS;i++){
        CxColdColumn* e=g->cold_entries+set+i;if(!e->valid||e->column!=column)continue;
        if(e->offset>CX_GEOMETRY_COLD_BYTES||e->length>CX_GEOMETRY_COLD_BYTES-e->offset)return TCW_FORMAT;
        const uint8_t* data=g->cold_bytes+e->offset;uint16_t pair;
        if(!e->format){if(e->length!=(uint64_t)w->height*2u)return TCW_FORMAT;pair=cold_u16(data+row*2u);}
        else if(e->format==1u){
            if(!e->runs||e->runs>w->height||e->length!=(uint64_t)e->runs*6u||cold_u32(data+(e->runs-1u)*6u)!=w->height)return TCW_FORMAT;
            uint32_t lo=0,hi=e->runs;
            while(lo<hi){uint32_t mid=lo+(hi-lo)/2u;if(cold_u32(data+mid*6u)<=row)lo=mid+1u;else hi=mid;}
            if(lo>=e->runs)return TCW_FORMAT;uint32_t end=cold_u32(data+lo*6u),start=lo?cold_u32(data+(lo-1u)*6u):0;
            if(start>row||end<=row||end>w->height)return TCW_FORMAT;pair=cold_u16(data+lo*6u+4u);
        }else if(e->format==2u||e->format==3u){
            uint32_t bits=9u,table=0;
            if(e->format==3u){if(!e->runs||e->runs>512u)return TCW_FORMAT;bits=0;while((1u<<bits)<e->runs)++bits;table=e->runs*2u;}
            uint64_t bit_length=(uint64_t)w->height*bits,index_bytes=(bit_length+7u)/8u;
            if(e->length!=(uint64_t)table+index_bytes)return TCW_FORMAT;
            uint32_t index=0;
            if(bits){uint64_t bit=(uint64_t)row*bits;uint32_t at=(uint32_t)(bit/8u),shift=(uint32_t)(bit%8u);
                if(at>=index_bytes)return TCW_FORMAT;index=data[table+at];
                if(shift+bits>8u){if((uint64_t)at+1u>=index_bytes)return TCW_FORMAT;index|=(uint32_t)data[table+at+1u]<<8;}
                index=(index>>shift)&((1u<<bits)-1u);
                uint32_t tail=(uint32_t)(bit_length%8u);if(tail&&(data[table+(uint32_t)index_bytes-1u]>>tail))return TCW_FORMAT;
            }
            if(e->format==3u){if(index>=e->runs)return TCW_FORMAT;pair=cold_u16(data+index*2u);}else pair=(uint16_t)index;
        }else return TCW_FORMAT;
        if(pair&~0x1ffu)return TCW_FORMAT;*word=(uint8_t)pair;*axis=(uint8_t)(pair>>8);++g->stats.cold_hits;return TCW_OK;
    }
    return TCW_STATE;
}
/* Only completed same-owner columns publish these source endpoints. */
static int predecessor_end(CxWorld* w,uint32_t column,uint32_t* end){
    CxOrderGeometry* g=w->geometry;if(!column)return TCW_STATE;uint32_t previous=column-1u,value=0;
    for(uint32_t i=0;i<CX_GEOMETRY_SLOTS;i++)if(g->valid[i]&&g->column[i]==previous){value=g->source_end[i];break;}
    if(!value&&g->cold_entries){uint32_t set=cold_set(previous)*CX_GEOMETRY_COLD_WAYS;
        for(uint32_t i=0;i<CX_GEOMETRY_COLD_WAYS;i++){CxColdColumn* e=g->cold_entries+set+i;if(e->valid&&e->column==previous){value=e->source_end;break;}}
    }
    if(!value)return TCW_STATE;if(value<=w->tile_start||value>=w->tile_end)return TCW_FORMAT;*end=value;return TCW_OK;
}
static uint16_t hot_pair(const CxWorld* w,const CxOrderGeometry* g,uint32_t row){uint32_t at=g->slot*w->height+row;return g->cells[at]|((uint16_t)g->compiler_axis[at]<<8);}
static uint64_t cold_select(CxWorld* w,CxOrderGeometry* g){
    uint64_t bytes=(uint64_t)w->height*2u,rle=(uint64_t)g->cold_runs*6u,raw9=((uint64_t)w->height*9u+7u)/8u;
    g->cold_format=0;g->cold_bits=0;
    if(raw9<bytes){bytes=raw9;g->cold_format=2u;g->cold_bits=9u;}
    if(rle<bytes){bytes=rle;g->cold_format=1u;g->cold_bits=0;}
    if(g->cold_scratch&&g->cold_count){uint32_t bits=0;while((1u<<bits)<g->cold_count)++bits;
        uint64_t palette=(uint64_t)g->cold_count*2u+((uint64_t)w->height*bits+7u)/8u;
        if(palette<bytes){bytes=palette;g->cold_format=3u;g->cold_bits=bits;}
    }
    return bytes;
}
/* Invalidations and encoding consume the caller's work. Metadata is published
 * only after every payload byte is complete; cancelled writes stay unreachable. */
static int cold_step(CxWorld* w,uint32_t* work){
    CxOrderGeometry* g=w->geometry;
    while(*work){
        if(g->cold_phase==1u){
            if(g->cold_scan<CX_GEOMETRY_COLD_ENTRIES){CxColdColumn* e=g->cold_entries+g->cold_scan++;
                if(e->valid&&e->offset<g->cold_offset+g->cold_length&&g->cold_offset<e->offset+e->length){e->valid=0;++g->stats.cold_overlap_evictions;}
                --*work;continue;
            }
            g->cold_phase=g->cold_format==3u?3u:2u;g->cold_row=g->cold_written=g->cold_accumulator=g->cold_accum_bits=g->cold_palette_scan=0;
            if(g->cold_format==3u)g->cold_written=g->cold_count*2u;continue;
        }
        if(g->cold_phase==3u){
            if(g->cold_palette_scan<512u){uint32_t value=g->cold_palette_scan++;
                if(g->cold_scratch->seen[value>>6]&(UINT64_C(1)<<(value&63u))){uint32_t index=g->cold_scratch->map[value];if(index>=g->cold_count)return TCW_FORMAT;cold_put16(g->cold_bytes+g->cold_offset+index*2u,(uint16_t)value);}
                --*work;continue;
            }
            g->cold_phase=2u;continue;
        }
        if(g->cold_row<w->height){uint16_t pair=hot_pair(w,g,g->cold_row);
            if(!g->cold_format)cold_put16(g->cold_bytes+g->cold_offset+g->cold_row*2u,pair);
            else if(g->cold_format==1u&&g->cold_row&&pair!=g->cold_last){
                if(g->cold_written+6u>g->cold_length)return TCW_FORMAT;
                uint8_t* at=g->cold_bytes+g->cold_offset+g->cold_written;cold_put32(at,g->cold_row);cold_put16(at+4,g->cold_last);g->cold_written+=6u;
            }
            if(g->cold_format>=2u){uint32_t value=g->cold_format==3u?g->cold_scratch->map[pair]:pair;
                if(g->cold_format==3u&&value>=g->cold_count)return TCW_FORMAT;
                g->cold_accumulator|=value<<g->cold_accum_bits;g->cold_accum_bits+=g->cold_bits;
                while(g->cold_accum_bits>=8u){if(g->cold_written>=g->cold_length)return TCW_FORMAT;g->cold_bytes[g->cold_offset+g->cold_written++]=(uint8_t)g->cold_accumulator;g->cold_accumulator>>=8;g->cold_accum_bits-=8u;}
            }
            g->cold_last=pair;++g->cold_row;--*work;continue;
        }
        if(g->cold_format==1u){if(g->cold_written+6u!=g->cold_length)return TCW_FORMAT;uint8_t* at=g->cold_bytes+g->cold_offset+g->cold_written;cold_put32(at,w->height);cold_put16(at+4,g->cold_last);}
        if(g->cold_format>=2u){
            if(g->cold_accum_bits){if(g->cold_written>=g->cold_length)return TCW_FORMAT;g->cold_bytes[g->cold_offset+g->cold_written++]=(uint8_t)g->cold_accumulator;g->cold_accumulator=g->cold_accum_bits=0;}
            if(g->cold_written!=g->cold_length)return TCW_FORMAT;
        }
        uint32_t set=cold_set(g->target_column)*CX_GEOMETRY_COLD_WAYS,slot=set+g->cold_victim++%CX_GEOMETRY_COLD_WAYS;
        for(uint32_t i=0;i<CX_GEOMETRY_COLD_WAYS;i++)if(!g->cold_entries[set+i].valid){slot=set+i;break;}
        if(g->cold_entries[slot].valid)++g->stats.cold_conflict_evictions;
        g->cold_entries[slot]=(CxColdColumn){1u,g->target_column,g->cold_offset,g->cold_length,g->cold_format==3u?g->cold_count:g->cold_runs,g->cold_format,g->cursor};
        g->cold_head=g->cold_offset+g->cold_length;g->cold_phase=0;++g->stats.cold_fills;
        if(g->cold_format==1u)++g->stats.cold_rle_fills;else if(g->cold_format==2u)++g->stats.cold_raw9_fills;else if(g->cold_format==3u)++g->stats.cold_palette_fills;else ++g->stats.cold_raw_fills;return CX_GEOMETRY_READY;
    }
    return CX_GEOMETRY_MORE;
}
static int usable(const CxWorld* w) {
    return w&&w->geometry&&w->phase!=CX_CANCELLED&&w->phase!=CX_FAILED;
}
static void touch(CxOrderGeometry* g,uint32_t slot) {
    for(uint32_t i=0;i<CX_GEOMETRY_SLOTS;i++)if(g->valid[i]&&i!=slot&&g->age[i]<CX_GEOMETRY_SLOTS)++g->age[i];
    g->age[slot]=0;
}
void cx_geometry_cancel(CxWorld* w) {
    if(!w||!w->geometry)return;CxOrderGeometry* g=w->geometry;
    if(g->active)g->valid[g->slot]=0;
    g->active=0;g->cold_phase=0;g->run_left=0;g->sequential_valid=0;memset(&g->pending,0,sizeof(g->pending));
}
void cx_geometry_destroy(CxWorld* w) {
    if(!w||!w->geometry)return;CxOrderGeometry* g=w->geometry;
    cx_free(w,g->cold_scratch);cx_free(w,g->cold_bytes);cx_free(w,g->cold_entries);cx_free(w,g->cells);cx_free(w,g->compiler_axis);cx_free(w,g->window);cx_free(w,g);w->geometry=NULL;
}
static int cold_budget_enabled(uint32_t maximum,uint64_t resident,uint64_t charge){
    return charge<=(uint64_t)maximum/16u&&resident<=maximum&&charge<=maximum-resident;
}
int cx_geometry_create(CxWorld* w) {
    if(!w||!w->world||(w->world_source?!w->world->stream_columns:(!w->world->file||!w->checkpoint_index))||!w->width||!w->height||w->twld_state||w->world->legacy_wld||w->phase==CX_CANCELLED||w->phase==CX_FAILED)return TCW_STATE;
    if(w->geometry)return TCW_OK;
    if(w->height>UINT32_MAX/CX_GEOMETRY_SLOTS)return TCW_MEMORY;
    CxOrderGeometry* g=cx_alloc(w,sizeof(*g));if(!g)return TCW_MEMORY;memset(g,0,sizeof(*g));
    g->cells=cx_alloc(w,w->height*CX_GEOMETRY_SLOTS);
    if(g->cells)g->compiler_axis=cx_alloc(w,w->height*CX_GEOMETRY_SLOTS);
    if(g->compiler_axis)g->window=cx_alloc(w,CX_GEOMETRY_WINDOW);
    if(!g->cells||!g->compiler_axis||!g->window){cx_free(w,g->cells);cx_free(w,g->compiler_axis);cx_free(w,g->window);cx_free(w,g);return TCW_MEMORY;}
    const uint32_t metadata=CX_GEOMETRY_COLD_ENTRIES*sizeof(CxColdColumn);
    uint64_t charge=cx_allocation_charge(CX_GEOMETRY_COLD_BYTES)+cx_allocation_charge(metadata)+cx_allocation_charge(sizeof(CxColdScratch)),resident=cx_resident_bytes(w);
    /* Decide BEFORE allocating optional storage, never swallow a failed alloc. */
    if(cold_budget_enabled(w->maximum,resident,charge)){
        g->cold_bytes=cx_alloc(w,CX_GEOMETRY_COLD_BYTES);
        if(g->cold_bytes)g->cold_entries=cx_alloc(w,metadata);
        if(g->cold_entries)g->cold_scratch=cx_alloc(w,sizeof(CxColdScratch));
        if(!g->cold_bytes||!g->cold_entries||!g->cold_scratch){cx_free(w,g->cold_scratch);cx_free(w,g->cold_bytes);cx_free(w,g->cold_entries);cx_free(w,g->cells);cx_free(w,g->compiler_axis);cx_free(w,g->window);cx_free(w,g);return TCW_MEMORY;}
        memset(g->cold_entries,0,metadata);
    }
    g->decoder.version=w->world->version;g->decoder.original_version=w->world->original_version;
    g->decoder.tile_type_count=w->world->tile_type_count;
    g->decoder.important=w->world->important;g->decoder.important_len=w->world->important_len;
    w->geometry=g;return TCW_OK;
}
int cx_geometry_begin(CxWorld* w,uint32_t column) {
    if(!usable(w))return TCW_STATE;if(column>=w->width)return TCW_INVALID;
    CxOrderGeometry* g=w->geometry;if(g->active)return TCW_STATE;
    for(uint32_t i=0;i<CX_GEOMETRY_SLOTS;i++)if(g->valid[i]&&g->column[i]==column){touch(g,i);++g->stats.hits;return CX_GEOMETRY_READY;}
    uint8_t cached,axis;int cold=cold_get(w,column,0,&cached,&axis);if(cold==TCW_OK)return CX_GEOMETRY_READY;if(cold!=TCW_STATE)return cold;
    uint32_t scan=column,start,end;
    if(w->world_source){start=w->world->stream_columns[column];end=w->world->stream_columns[column+1u];}
    else{CxCheckpoint* cp=w->checkpoint_index+column/CX_CHECK_COLUMNS;scan=cp->column;start=cp->source_offset;end=w->tile_end;
        if(scan>column||column-scan>=CX_CHECK_COLUMNS||end>w->world->file_len)return TCW_FORMAT;
        /* Carry only the prior completed decoder cursor, never a width index. */
        if(g->sequential_valid&&g->sequential_column==column){scan=column;start=g->sequential_cursor;}
        else if(column>scan){uint32_t prior;int status=predecessor_end(w,column,&prior);
            if(status==TCW_OK){if(prior<cp->source_offset)return TCW_FORMAT;scan=column;start=prior;}
            else if(status!=TCW_STATE)return status;
        }
    }
    if(start<w->tile_start||end>w->tile_end||start>=end)return TCW_FORMAT;
    uint32_t slot=0;
    for(uint32_t i=0;i<CX_GEOMETRY_SLOTS;i++){if(!g->valid[i]){slot=i;break;}if(g->age[i]>g->age[slot])slot=i;}
    ++g->stats.misses;if(g->valid[slot])++g->stats.evictions;g->scan_column=scan;g->target_column=column;
    g->valid[slot]=0;g->column[slot]=column;g->slot=slot;g->active=1;g->cursor=start;g->end=end;g->row=g->run_left=g->cold_runs=g->cold_phase=0;
    g->cold_count=0;if(g->cold_scratch)memset(g->cold_scratch->seen,0,sizeof(g->cold_scratch->seen));
    memset(&g->pending,0,sizeof(g->pending));return CX_GEOMETRY_MORE;
}
static int bad_column(CxWorld* w) {cx_geometry_cancel(w);return TCW_FORMAT;}
int cx_geometry_step(CxWorld* w,uint32_t work,CxGeometryRead* out) {
    if(!usable(w))return TCW_STATE;if(!work||!out)return TCW_INVALID;
    CxOrderGeometry* g=w->geometry;memset(out,0,sizeof(*out));
    if(g->pending.ticket){*out=g->pending;return CX_GEOMETRY_READ;}
    if(!g->active)return CX_GEOMETRY_READY;
    while(work){
        if(g->cold_phase){int status=cold_step(w,&work);if(status<0)return bad_column(w);if(status==CX_GEOMETRY_MORE)return status;
            g->source_end[g->slot]=g->cursor;g->valid[g->slot]=1;touch(g,g->slot);g->active=0;g->sequential_valid=1;g->sequential_column=g->target_column+1u;g->sequential_cursor=g->cursor;return CX_GEOMETRY_READY;
        }
        if(g->run_left){uint32_t n=g->run_left<work?g->run_left:work;if(g->scan_column==g->target_column){memset(g->cells+g->slot*w->height+g->row,g->packed,n);memset(g->compiler_axis+g->slot*w->height+g->row,g->axis,n);}g->row+=n;g->run_left-=n;work-=n;continue;}
        if(g->row==w->height){
            if(g->scan_column<g->target_column){++g->scan_column;g->row=0;continue;}
            if((w->world_source||g->target_column+1u==w->width)&&g->cursor!=g->end)return bad_column(w);
            uint64_t bytes=cold_select(w,g);
            if(g->cold_bytes&&bytes<=CX_GEOMETRY_COLD_BYTES){g->cold_length=(uint32_t)bytes;g->cold_offset=g->cold_head;
                if(g->cold_offset>CX_GEOMETRY_COLD_BYTES-g->cold_length)g->cold_offset=0;
                g->cold_scan=0;g->cold_phase=1u;continue;
            }
            if(g->cold_bytes)++g->stats.cold_oversize;g->source_end[g->slot]=g->cursor;g->valid[g->slot]=1;touch(g,g->slot);g->active=0;g->sequential_valid=1;g->sequential_column=g->target_column+1u;g->sequential_cursor=g->cursor;return CX_GEOMETRY_READY;}
        if(g->cursor>=g->end)return bad_column(w);
        uint32_t need=g->end-g->cursor;if(need>32u)need=32u;
        if(w->world_source&&(g->cursor<g->window_offset||g->cursor-g->window_offset>g->window_length||need>g->window_length-(g->cursor-g->window_offset))){
            if(g->ticket==UINT32_MAX){cx_geometry_cancel(w);return TCW_STATE;}
            uint32_t n=g->end-g->cursor;if(n>CX_GEOMETRY_WINDOW)n=CX_GEOMETRY_WINDOW;
            ++g->stats.read_requests;g->pending=(CxGeometryRead){++g->ticket,w->world_source,g->cursor,n};*out=g->pending;return CX_GEOMETRY_READ;
        }
        /* Minimal read-only decoder context, not a shallow owner copy.
         * Audited read_tile_at (sectioned branch) writes only this local
         * tile_decode_calls, p and tile. tile_important reads important[];
         * tx_world_is_future reads only version/original_version. No allocator,
         * section override, prepared output or shared cache pointer is copied. */
        TxWorld* view=&g->decoder;view->file=w->world_source?g->window:w->world->file;view->file_len=w->world_source?g->window_length:w->world->file_len;
        uint32_t base=w->world_source?g->window_offset:0u,p=g->cursor-base;TxTile tile;
        ++g->stats.decoded_records;
        if(!read_tile_at(view,&p,w->world_source?g->window_length:g->end,&tile))return bad_column(w);
        uint32_t next=base+p,run=(uint32_t)tile.same+1u;
        if(next<=g->cursor||next>g->end||run>w->height-g->row)return bad_column(w);
        g->cursor=next;g->run_left=run;
        uint32_t route=0;if(tile.active&&(tile.type==424u||tile.type==445u)){int r=tile.type==424u?tile.frame_x/18:0;if(r>=0&&r<=2)route=(uint32_t)r+1u;}
        uint32_t mask=cx_wire_mask(&tile);g->axis=tile.active&&tile.type==0xfffeu;
        /* Gate numbering includes unwired active420. Traversal uses only low wire bits. */
        g->packed=(mask?(uint8_t)(mask|(route<<4)|(tile.active&&tile.type==419u?64u:0u)):0)|(tile.active&&tile.type==420u?128u:0u);
        if(g->scan_column==g->target_column){uint16_t pair=g->packed|((uint16_t)g->axis<<8);if(!g->row||pair!=g->cold_last)++g->cold_runs;g->cold_last=pair;if(g->cold_scratch){uint64_t bit=UINT64_C(1)<<(pair&63u);if(!(g->cold_scratch->seen[pair>>6]&bit)){g->cold_scratch->seen[pair>>6]|=bit;g->cold_scratch->map[pair]=(uint16_t)g->cold_count++;}}}
        --work;
    }
    return CX_GEOMETRY_MORE;
}
int cx_geometry_supply(CxWorld* w,const CxGeometryRead* request,const uint8_t* data,uint32_t length) {
    if(!usable(w))return TCW_STATE;CxOrderGeometry* g=w->geometry;
    if(!request||!data||!g->active||!g->pending.ticket||request->ticket!=g->pending.ticket||request->source!=g->pending.source||request->offset!=g->pending.offset||request->length!=g->pending.length||length!=request->length)return TCW_INVALID;
    memcpy(g->window,data,length);g->window_offset=request->offset;g->window_length=length;memset(&g->pending,0,sizeof(g->pending));return TCW_OK;
}
static int geometry_lookup(CxWorld* w,uint32_t column,uint32_t row,uint8_t* out,uint8_t* axis){
    if(!usable(w))return TCW_STATE;if(!out||!axis||column>=w->width||row>=w->height)return TCW_INVALID;
    CxOrderGeometry* g=w->geometry;
    for(uint32_t i=0;i<CX_GEOMETRY_SLOTS;i++)if(g->valid[i]&&g->column[i]==column){uint32_t at=i*w->height+row;*out=g->cells[at];*axis=g->compiler_axis[at];touch(g,i);return TCW_OK;}
    return cold_get(w,column,row,out,axis);
}
int cx_geometry_get(CxWorld* w,uint32_t column,uint32_t row,uint8_t* out){uint8_t axis;return geometry_lookup(w,column,row,out,&axis);}
int cx_geometry_stats(CxWorld* w,CxGeometryStats* out){if(!w||!w->geometry||!out)return TCW_INVALID;*out=w->geometry->stats;return TCW_OK;}
int cx_geometry_get_connectivity(CxWorld* w,uint32_t column,uint32_t row,uint8_t* word,uint8_t* route){
    if(!route)return TCW_INVALID;uint8_t axis;int s=geometry_lookup(w,column,row,word,&axis);if(s)return s;*route=axis?1u:(*word>>4)&3u;return TCW_OK;
}
