/* Full-column validation with FIFO-only fixed row-block demand cache. */
#include "terra_circuit_geometry.h"
#include <stddef.h>
#include <string.h>
extern int read_tile_at(TxWorld*,uint32_t*,uint32_t,TxTile*);
#define CX_BLOCK_ROWS 128u
#define CX_BLOCK_ENTRIES 16384u
#define CX_BLOCK_BYTES 144u
#define CX_BLOCK_ARENA_BYTES (CX_GEOMETRY_COLD_BYTES-CX_GEOMETRY_ORDER_CACHE_BYTES)
#define CX_BLOCK_SLOTS (CX_BLOCK_ARENA_BYTES/CX_BLOCK_BYTES)
/* Round the borrowed tail start up, never into the block prefix. Any alignment
 * padding reduces the returned tail size rather than growing its reservation. */
#define CX_ORDER_ARENA_ALIGNMENT _Alignof(max_align_t)
#define CX_ORDER_ARENA_OFFSET (((CX_BLOCK_ARENA_BYTES+CX_ORDER_ARENA_ALIGNMENT-1u)/CX_ORDER_ARENA_ALIGNMENT)*CX_ORDER_ARENA_ALIGNMENT)
_Static_assert(CX_GEOMETRY_ORDER_CACHE_BYTES<CX_GEOMETRY_COLD_BYTES,"order tail must leave block storage");
_Static_assert(CX_BLOCK_SLOTS>0u,"block arena must contain a slot");
_Static_assert(CX_ORDER_ARENA_OFFSET<CX_GEOMETRY_COLD_BYTES,"aligned order tail must be nonempty");
typedef struct CxBlock {uint32_t valid,column,block,slot,source_end;} CxBlock;
typedef struct CxColumnEnd {uint32_t valid,column,end;} CxColumnEnd;
struct CxOrderGeometry {
    uint8_t *cells,*compiler_axis,*window,*cold_bytes;CxBlock* blocks;uint32_t* slot_owner;CxColumnEnd* ends;
    uint32_t column[3],source_end[3],valid[3],age[3];
    uint32_t active,slot,cursor,end,row,run_left,window_offset,window_length,ticket;
    uint32_t scan_column,target_column,sequential_valid,sequential_column,sequential_cursor;
    uint32_t point_mode,point_row,block_phase,block_row,block_height,block_slot,block_entry,block_written,block_accumulator,block_bits,block_head,block_victim;
    CxGeometryStats stats;TxWorld decoder;uint8_t packed,axis;CxGeometryRead pending;
};
static int usable(const CxWorld* w){return w&&w->geometry&&w->phase!=CX_CANCELLED&&w->phase!=CX_FAILED;}
void* cx_geometry_order_cache_arena(CxWorld* w,uint32_t* size){
    if(!size)return NULL;
    *size=0;
    if(!w||!w->geometry||!w->geometry->cold_bytes)return NULL;
    *size=(uint32_t)(CX_GEOMETRY_COLD_BYTES-CX_ORDER_ARENA_OFFSET);
    return w->geometry->cold_bytes+CX_ORDER_ARENA_OFFSET;
}
/* Explicit policy change: trueLRU, matching the selected counterfactual model. */
static void touch(CxOrderGeometry* g,uint32_t slot){uint32_t prior=g->age[slot];for(uint32_t i=0;i<3;i++)if(g->valid[i]&&i!=slot&&g->age[i]<prior)++g->age[i];g->age[slot]=0;}
static void remove_hot(CxOrderGeometry* g,uint32_t slot){if(!g->valid[slot])return;uint32_t rank=g->age[slot];g->valid[slot]=0;for(uint32_t i=0;i<3;i++)if(g->valid[i]&&g->age[i]>rank)--g->age[i];}
static int hot_slot(CxOrderGeometry* g,uint32_t x){for(uint32_t i=0;i<3;i++)if(g->valid[i]&&g->column[i]==x)return (int)i;return -1;}
static uint32_t block_set(uint32_t x,uint32_t block){uint32_t h=x*0x9e3779b1u^block*0x85ebca6bu;h^=h>>16;return (h&(CX_BLOCK_ENTRIES/4u-1u))*4u;}
static CxBlock* block_find(CxOrderGeometry* g,uint32_t x,uint32_t block){if(!g->blocks)return NULL;uint32_t set=block_set(x,block);for(uint32_t i=0;i<4;i++){CxBlock* e=g->blocks+set+i;if(e->valid&&e->column==x&&e->block==block)return e;}return NULL;}
static int block_get(CxWorld* w,uint32_t x,uint32_t y,uint8_t* word,uint8_t* axis){
    CxOrderGeometry* g=w->geometry;CxBlock* e=block_find(g,x,y/CX_BLOCK_ROWS);if(!e)return TCW_STATE;
    if(e->slot>=CX_BLOCK_SLOTS||g->slot_owner[e->slot]!=(uint32_t)(e-g->blocks)+1u)return TCW_FORMAT;
    uint32_t start=e->block*CX_BLOCK_ROWS,h=w->height-start;if(h>CX_BLOCK_ROWS)h=CX_BLOCK_ROWS;
    uint32_t row=y-start,bit=row*9u,at=bit/8u,shift=bit%8u,bytes=(h*9u+7u)/8u;
    if(row>=h||at>=bytes)return TCW_FORMAT;const uint8_t* data=g->cold_bytes+e->slot*CX_BLOCK_BYTES;uint32_t pair=data[at];
    if(shift+9u>8u){if(at+1u>=bytes)return TCW_FORMAT;pair|=(uint32_t)data[at+1u]<<8;}
    pair=(pair>>shift)&511u;uint32_t tail=h*9u%8u;if(tail&&(data[bytes-1u]>>tail))return TCW_FORMAT;
    *word=(uint8_t)pair;*axis=(uint8_t)(pair>>8);++g->stats.cold_hits;return TCW_OK;
}
static void block_invalidate(CxOrderGeometry* g,uint32_t index){CxBlock* e=g->blocks+index;if(!e->valid)return;if(g->slot_owner[e->slot]==index+1u)g->slot_owner[e->slot]=0;e->valid=0;}
static void start_block(CxWorld* w,CxOrderGeometry* g){uint32_t start=g->point_row/CX_BLOCK_ROWS*CX_BLOCK_ROWS;g->block_height=w->height-start;if(g->block_height>CX_BLOCK_ROWS)g->block_height=CX_BLOCK_ROWS;g->block_phase=1u;g->block_row=g->block_written=g->block_accumulator=g->block_bits=0;}
static int block_step(CxWorld* w,uint32_t* work){CxOrderGeometry* g=w->geometry;
    while(*work){
        if(g->block_phase==1u){uint32_t slot=g->block_head,owner=g->slot_owner[slot];
            if(owner){CxBlock* e=g->blocks+owner-1u;if(e->valid&&e->slot==slot){++g->stats.cold_overlap_evictions;block_invalidate(g,owner-1u);}else g->slot_owner[slot]=0;}
            uint32_t set=block_set(g->target_column,g->point_row/CX_BLOCK_ROWS),index=set+g->block_victim++%4u;
            for(uint32_t i=0;i<4;i++)if(!g->blocks[set+i].valid){index=set+i;break;}
            if(g->blocks[index].valid){++g->stats.cold_conflict_evictions;block_invalidate(g,index);}
            g->block_slot=slot;g->block_entry=index;g->block_phase=2u;--*work;continue;
        }
        uint8_t* data=g->cold_bytes+g->block_slot*CX_BLOCK_BYTES;
        if(g->block_row<g->block_height){uint32_t y=g->point_row/CX_BLOCK_ROWS*CX_BLOCK_ROWS+g->block_row,at=g->slot*w->height+y;
            uint32_t pair=g->cells[at]|((uint32_t)g->compiler_axis[at]<<8);g->block_accumulator|=pair<<g->block_bits;g->block_bits+=9u;
            while(g->block_bits>=8u){data[g->block_written++]=(uint8_t)g->block_accumulator;g->block_accumulator>>=8;g->block_bits-=8u;}
            ++g->block_row;--*work;continue;
        }
        if(g->block_bits){data[g->block_written++]=(uint8_t)g->block_accumulator;g->block_bits=g->block_accumulator=0;}
        if(g->block_written!=(g->block_height*9u+7u)/8u)return TCW_FORMAT;
        g->blocks[g->block_entry]=(CxBlock){1u,g->target_column,g->point_row/CX_BLOCK_ROWS,g->block_slot,g->source_end[g->slot]};
        g->slot_owner[g->block_slot]=g->block_entry+1u;g->block_head=(g->block_slot+1u)%CX_BLOCK_SLOTS;g->block_phase=0;
        ++g->stats.cold_fills;++g->stats.cold_raw9_fills;return CX_GEOMETRY_READY;
    }
    return CX_GEOMETRY_MORE;
}
void cx_geometry_cancel(CxWorld* w){if(!w||!w->geometry)return;CxOrderGeometry* g=w->geometry;if(g->active)remove_hot(g,g->slot);g->active=g->block_phase=g->run_left=g->sequential_valid=0;memset(&g->pending,0,sizeof(g->pending));}
void cx_geometry_destroy(CxWorld* w){if(!w||!w->geometry)return;CxOrderGeometry* g=w->geometry;cx_free(w,g->ends);cx_free(w,g->slot_owner);cx_free(w,g->blocks);cx_free(w,g->cold_bytes);cx_free(w,g->cells);cx_free(w,g->compiler_axis);cx_free(w,g->window);cx_free(w,g);w->geometry=NULL;}
static int cold_budget_enabled(uint32_t maximum,uint64_t resident,uint64_t charge){return charge<=(uint64_t)maximum/16u&&resident<=maximum&&charge<=maximum-resident;}
int cx_geometry_create(CxWorld* w){
    if(!w||!w->world||(w->world_source?!w->world->stream_columns:(!w->world->file||!w->checkpoint_index))||!w->width||!w->height||w->twld_state||w->world->legacy_wld||w->phase==CX_CANCELLED||w->phase==CX_FAILED)return TCW_STATE;
    if(w->geometry)return TCW_OK;if(w->height>UINT32_MAX/3u)return TCW_MEMORY;
    CxOrderGeometry* g=cx_alloc(w,sizeof(*g));if(!g)return TCW_MEMORY;memset(g,0,sizeof(*g));
    g->cells=cx_alloc(w,w->height*3u);if(g->cells)g->compiler_axis=cx_alloc(w,w->height*3u);if(g->compiler_axis)g->window=cx_alloc(w,CX_GEOMETRY_WINDOW);
    w->geometry=g;if(!g->cells||!g->compiler_axis||!g->window){cx_geometry_destroy(w);return TCW_MEMORY;}
    /* Preserve F's optional admission boundary, including its larger owner
     * array. G allocates fewer owner slots below, but must not newly enable
     * the entire cold layer for budgets where F deliberately omitted it. */
    uint64_t charge=cx_allocation_charge(CX_GEOMETRY_COLD_BYTES)+cx_allocation_charge(CX_BLOCK_ENTRIES*sizeof(CxBlock))+cx_allocation_charge((CX_GEOMETRY_COLD_BYTES/CX_BLOCK_BYTES)*4u)+cx_allocation_charge(64u*sizeof(CxColumnEnd));
    if(cold_budget_enabled(w->maximum,cx_resident_bytes(w),charge)){
        g->cold_bytes=cx_alloc(w,CX_GEOMETRY_COLD_BYTES);if(g->cold_bytes)g->blocks=cx_alloc(w,CX_BLOCK_ENTRIES*sizeof(CxBlock));
        if(g->blocks)g->slot_owner=cx_alloc(w,CX_BLOCK_SLOTS*4u);if(g->slot_owner)g->ends=cx_alloc(w,64u*sizeof(CxColumnEnd));
        if(!g->cold_bytes||!g->blocks||!g->slot_owner||!g->ends){cx_geometry_destroy(w);return TCW_MEMORY;}
        memset(g->blocks,0,CX_BLOCK_ENTRIES*sizeof(CxBlock));memset(g->slot_owner,0,CX_BLOCK_SLOTS*4u);memset(g->ends,0,64u*sizeof(CxColumnEnd));
        memset(g->cold_bytes+CX_ORDER_ARENA_OFFSET,0,CX_GEOMETRY_COLD_BYTES-CX_ORDER_ARENA_OFFSET);
    }
    g->decoder.version=w->world->version;g->decoder.original_version=w->world->original_version;g->decoder.tile_type_count=w->world->tile_type_count;g->decoder.important=w->world->important;g->decoder.important_len=w->world->important_len;return TCW_OK;
}
static int predecessor_end(CxWorld* w,uint32_t column,uint32_t row,uint32_t* out){
    CxOrderGeometry* g=w->geometry;if(!column)return TCW_STATE;uint32_t x=column-1u,value=0;int hot=hot_slot(g,x);if(hot>=0)value=g->source_end[hot];
    if(!value&&g->ends){CxColumnEnd* e=g->ends+x%64u;if(e->valid&&e->column==x)value=e->end;}
    if(!value){CxBlock* e=block_find(g,x,row/CX_BLOCK_ROWS);if(e)value=e->source_end;}
    if(!value)return TCW_STATE;if(value<=w->tile_start||value>=w->tile_end)return TCW_FORMAT;*out=value;return TCW_OK;
}
static int begin_source(CxWorld* w,uint32_t column,uint32_t row,uint32_t point){
    CxOrderGeometry* g=w->geometry;uint32_t scan=column,start,end;
    if(w->world_source){start=w->world->stream_columns[column];end=w->world->stream_columns[column+1u];}
    else{CxCheckpoint* cp=w->checkpoint_index+column/CX_CHECK_COLUMNS;scan=cp->column;start=cp->source_offset;end=w->tile_end;
        if(scan>column||column-scan>=CX_CHECK_COLUMNS||end>w->world->file_len)return TCW_FORMAT;
        if(g->sequential_valid&&g->sequential_column==column){scan=column;start=g->sequential_cursor;}
        else if(column>scan){uint32_t prior;int s=predecessor_end(w,column,row,&prior);if(s==TCW_OK){if(prior<cp->source_offset)return TCW_FORMAT;scan=column;start=prior;}else if(s!=TCW_STATE)return s;}
    }
    if(start<w->tile_start||end>w->tile_end||start>=end)return TCW_FORMAT;uint32_t slot=0;
    for(uint32_t i=0;i<3;i++){if(!g->valid[i]){slot=i;break;}if(g->age[i]>g->age[slot])slot=i;}
    ++g->stats.misses;if(g->valid[slot])++g->stats.evictions;g->scan_column=scan;g->target_column=column;remove_hot(g,slot);g->column[slot]=column;g->slot=slot;g->active=1;g->cursor=start;g->end=end;g->row=g->run_left=g->block_phase=0;g->point_mode=point;g->point_row=row;
    memset(&g->pending,0,sizeof(g->pending));return CX_GEOMETRY_MORE;
}
int cx_geometry_begin(CxWorld* w,uint32_t column){if(!usable(w))return TCW_STATE;if(column>=w->width)return TCW_INVALID;CxOrderGeometry* g=w->geometry;if(g->active)return TCW_STATE;
    int hot=hot_slot(g,column);if(hot>=0){touch(g,(uint32_t)hot);++g->stats.hits;return CX_GEOMETRY_READY;}return begin_source(w,column,0,0);
}
int cx_geometry_point_begin(CxWorld* w,uint32_t column,uint32_t row){if(!usable(w))return TCW_STATE;if(column>=w->width||row>=w->height)return TCW_INVALID;CxOrderGeometry* g=w->geometry;if(g->active)return TCW_STATE;
    int hot=hot_slot(g,column);uint8_t word,axis;int cold=block_get(w,column,row,&word,&axis);if(cold<0&&cold!=TCW_STATE)return cold;
    if(hot>=0){if(!g->blocks||cold==TCW_OK)return CX_GEOMETRY_READY;g->slot=(uint32_t)hot;g->target_column=column;g->point_mode=1;g->point_row=row;g->active=1;start_block(w,g);return CX_GEOMETRY_MORE;}
    if(cold==TCW_OK)return CX_GEOMETRY_READY;return begin_source(w,column,row,1);
}
static int bad_column(CxWorld* w){cx_geometry_cancel(w);return TCW_FORMAT;}
int cx_geometry_step(CxWorld* w,uint32_t work,CxGeometryRead* out){if(!usable(w))return TCW_STATE;if(!work||!out)return TCW_INVALID;CxOrderGeometry* g=w->geometry;memset(out,0,sizeof(*out));
    if(g->pending.ticket){*out=g->pending;return CX_GEOMETRY_READ;}if(!g->active)return CX_GEOMETRY_READY;
    while(work){if(g->block_phase){int s=block_step(w,&work);if(s<0)return bad_column(w);if(s==CX_GEOMETRY_MORE)return s;g->active=0;return CX_GEOMETRY_READY;}
        if(g->run_left){uint32_t n=g->run_left<work?g->run_left:work;if(g->scan_column==g->target_column){memset(g->cells+g->slot*w->height+g->row,g->packed,n);memset(g->compiler_axis+g->slot*w->height+g->row,g->axis,n);}g->row+=n;g->run_left-=n;work-=n;continue;}
        if(g->row==w->height){
            if(g->scan_column<g->target_column){++g->scan_column;g->row=0;continue;}
            if((w->world_source||g->target_column+1u==w->width)&&g->cursor!=g->end)return bad_column(w);
            g->source_end[g->slot]=g->cursor;g->valid[g->slot]=1;g->age[g->slot]=3u;touch(g,g->slot);
            if(g->ends)g->ends[g->target_column%64u]=(CxColumnEnd){1u,g->target_column,g->cursor};
            g->sequential_valid=1;g->sequential_column=g->target_column+1u;g->sequential_cursor=g->cursor;
            if(g->point_mode&&g->blocks){start_block(w,g);continue;}
            g->active=0;return CX_GEOMETRY_READY;
        }
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

        --work;
    }
    return CX_GEOMETRY_MORE;
}
int cx_geometry_supply(CxWorld* w,const CxGeometryRead* request,const uint8_t* data,uint32_t length){if(!usable(w))return TCW_STATE;CxOrderGeometry* g=w->geometry;
    if(!request||!data||!g->active||!g->pending.ticket||request->ticket!=g->pending.ticket||request->source!=g->pending.source||request->offset!=g->pending.offset||request->length!=g->pending.length||length!=request->length)return TCW_INVALID;
    memcpy(g->window,data,length);g->window_offset=request->offset;g->window_length=length;memset(&g->pending,0,sizeof(g->pending));return TCW_OK;
}
static int hot_get(CxWorld* w,uint32_t column,uint32_t row,uint8_t* out,uint8_t* axis){if(!usable(w))return TCW_STATE;if(!out||!axis||column>=w->width||row>=w->height)return TCW_INVALID;CxOrderGeometry* g=w->geometry;int slot=hot_slot(g,column);if(slot<0)return TCW_STATE;uint32_t at=(uint32_t)slot*w->height+row;*out=g->cells[at];*axis=g->compiler_axis[at];touch(g,(uint32_t)slot);return TCW_OK;}
int cx_geometry_get(CxWorld* w,uint32_t column,uint32_t row,uint8_t* out){uint8_t axis;return hot_get(w,column,row,out,&axis);}
int cx_geometry_point_get(CxWorld* w,uint32_t column,uint32_t row,uint8_t* out){if(!usable(w))return TCW_STATE;uint8_t axis;int s=hot_get(w,column,row,out,&axis);if(s!=TCW_STATE)return s;return block_get(w,column,row,out,&axis);}
int cx_geometry_get_connectivity(CxWorld* w,uint32_t column,uint32_t row,uint8_t* word,uint8_t* route){if(!route)return TCW_INVALID;uint8_t axis;int s=hot_get(w,column,row,word,&axis);if(s)return s;*route=axis?1u:(*word>>4)&3u;return TCW_OK;}
int cx_geometry_stats(CxWorld* w,CxGeometryStats* out){if(!w||!w->geometry||!out)return TCW_INVALID;*out=w->geometry->stats;return TCW_OK;}
