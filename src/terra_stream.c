/* Cooperative file-backed WLD reader/writer. No whole tile or output buffer. */
#include "terra_stream.h"
#include "terra_stream_metadata.h"
#include "terra_world.h"
#include "terra_render_task.h"
#include "terra_output.h"
#include "terra_regions.h"
#include "terra_stream_map.h"
#include "terra_stream_png.h"
#include <string.h>
#include <limits.h>

#define WINDOW (1024u*1024u)
#define METADATA_LIMIT (16u*1024u*1024u)
#define RECORD_BYTES 8200u
#define TILE_MAX_BYTES 32u
extern uint8_t* tx_persistent_alloc(uint32_t);
extern void tx_internal_free(void*);
extern uint32_t tx_mark(void);
extern int tx_bridge_range_is_valid(uint32_t,uint32_t);
extern TxWorld* tx_get_world(uint32_t);
extern void tx_set_error(const char*,const char*);
extern void tx_clear_error(void);
extern int parse_format(TxWorld*);
extern int parse_header(TxWorld*);
extern int read_tile_at(TxWorld*,uint32_t*,uint32_t,TxTile*);
extern void write_tile(TxWorld*,TxBuf*,const TxTile*,uint32_t);
extern int same_tile(const TxTile*,const TxTile*);
extern void buf_init(TxBuf*,uint32_t);
extern int apply_pixel_art_at(TxWorld*,uint32_t,uint32_t,TxTile*);
extern int tx_stream_activate_world(TxWorld*,uint32_t*);

enum { OPEN_FORMAT=1,OPEN_PREFIX,OPEN_SUFFIX,OPEN_SCAN,WRITE_PREFIX,WRITE_SCAN,WRITE_SUFFIX,WRITE_PATCH,DONE,CANCELLED,ADOPTED,OP_SCAN,OP_REGION,OP_RESULT,OP_MEDIA,OP_MEDIA_SCAN };
typedef struct StreamTask {
    uint32_t id, stage, is_write, old_handle;
    TxStreamEvent event;
    TxWorld* candidate;
    TxWorld* source;
    uint8_t* input;
    uint32_t input_offset,input_length;
    uint32_t original_start,original_end,source_size,source_id;
    uint32_t load_offset,compact_length,prefix_length,suffix_length;
    uint32_t cursor,x,y,remaining,source_run_end;
    TxTile source_tile,merged;
    uint32_t merged_count,merged_valid;
    TxBuf output;
    uint32_t output_offset,output_positions[TX_MAX_SECTIONS],suffix_offset;
    uint32_t stripe_first,stripe_count,stripe_ready;
    uint8_t* stripe;
    TxPixelArtChunk* stripe_nodes;
    uint32_t stripe_capacity;
    uint8_t patch[8192+TX_MAX_SECTIONS*4+32];
    TxStreamMetadata metadata;
    char* operation;
    char* request;
    uint32_t result_kind,result_length,result_offset;
    uint8_t* result;
    TxTileRule* rules;
    uint32_t rule_count;
    void* region_scan;
    TxMarkerScan* marker_scan;
    MapMarkerEntry* chest_markers;
    uint32_t chest_count,scan_end,media_active,full_png;
    TxStreamMap* map_encoder;
    TxStreamPng* png_encoder;
} StreamTask;
static StreamTask* current;
static uint32_t generation=1;

static int fail(const char* code,const char* message) { tx_set_error(code,message);return -1; }
static int valid_range(const void* ptr,uint32_t bytes) { return ptr&&tx_bridge_range_is_valid((uint32_t)(uintptr_t)ptr,bytes); }
static uint32_t u16(const uint8_t* p){return (uint32_t)p[0]|((uint32_t)p[1]<<8);}
static uint32_t u32(const uint8_t* p){return u16(p)|(u16(p+2)<<16);}
static void put16(uint8_t* p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t* p,uint32_t v){put16(p,v);put16(p+2,v>>16);}
static int read_length(const uint8_t* bytes,uint32_t* offset,uint32_t end,uint32_t* length){
    uint32_t value=0;
    for(uint32_t shift=0;shift<35;shift+=7){if(*offset>=end)return 0;uint8_t b=bytes[(*offset)++];if(shift==28&&(b&240))return 0;value|=(uint32_t)(b&127)<<shift;if(!(b&128)){*length=value;return value<=end-*offset;}}
    return 0;
}
static int validate_footer(TxWorld* w){
    uint32_t index=w->version>=220?10:w->version>=210?9:w->version>=189?8:w->version>=170?7:w->version>=116?6:5;
    if(index>=w->pointer_count)return fail("TERRAX_BAD_FOOTER","world footer section is missing");
    uint32_t off=w->starts[index],end=w->ends[index],header=w->starts[0],name_len,header_len;
    if(off>=end||w->file[off++]!=1||!read_length(w->file,&off,end,&name_len)||!read_length(w->file,&header,w->ends[0],&header_len)||
       name_len!=header_len||memcmp(w->file+off,w->file+header,name_len)||end-off-name_len<4||u32(w->file+off+name_len)!=(uint32_t)w->worldId)
        return fail("TERRAX_BAD_FOOTER","world footer identity is invalid");
    return 0;
}
static StreamTask* task(uint32_t id){return current&&current->id==id&&current->stage!=CANCELLED?current:NULL;}
static void clear_event(StreamTask* t){memset(&t->event,0,sizeof(t->event));t->event.abi_version=1;}
static void event(StreamTask* t,uint32_t kind,uint32_t offset,uint32_t length,const void* data){
    clear_event(t);t->event.kind=kind;t->event.source_id=t->source_id;t->event.offset=offset;
    t->event.length=length;t->event.data_ptr=(uint32_t)(uintptr_t)data;
    t->event.completed_columns=t->x;t->event.total_columns=t->candidate?(uint32_t)t->candidate->maxTilesX:0;
}
void tx_stream_release_world(TxWorld* w){
    if(!w||!w->stream_owned)return;
    if(w->file)tx_internal_free(w->file);
    if(w->stream_columns)tx_internal_free(w->stream_columns);
    w->file=NULL;w->stream_columns=NULL;w->stream_owned=0;
}
static void discard_candidate(StreamTask* t){
    if(!t->candidate)return;
    TxWorld* w=t->candidate;
    tx_output_clear(w);
    if(w->important_override)tx_internal_free(w->important_override);
    if(w->region_mask)tx_internal_free(w->region_mask);
    for(uint32_t i=0;i<TX_MAX_SECTION_OVERRIDES;i++)if(w->section_overrides[i].active&&w->section_overrides[i].data)tx_internal_free(w->section_overrides[i].data);
    if(w->pixel_art_maps)tx_internal_free(w->pixel_art_maps);
    if(w->pixel_art_chunk_table)tx_internal_free(w->pixel_art_chunk_table);
    tx_stream_release_world(w);tx_internal_free(w);t->candidate=NULL;
}
extern int json_validate_document(const char*,int);
extern int json_find_key(const char*,int,const char*);
extern int json_extract_int(const char*,int,int,int32_t*);
extern int parse_marker_array(const char*,int,const char*,const char*,MapMarkerEntry**,uint32_t*);
extern int op_execute_json(TxWorld*,const char*,const char*,TxBuf*);
extern int tx_stream_parse_tile_rules(TxWorld*,const char*,int,TxTileRule**,uint32_t*);
extern void* tx_region_stream_begin(TxWorld*,TxTileRule*,uint32_t);
extern int tx_region_stream_run(TxWorld*,void*,uint32_t,uint32_t,TxTile*,uint32_t);
extern void tx_region_stream_finish(TxWorld*,void*);
extern void tx_region_stream_free(void*);
extern uint32_t tx_last_ptr,tx_last_len;
static char* copy_bridge_string(const char* p,uint32_t maximum){
    if(!valid_range(p,1))return NULL;
    uint32_t n=0;while(n<maximum&&valid_range(p+n,1)&&p[n])n++;
    if(n==maximum||!valid_range(p+n,1))return NULL;
    char* result=(char*)tx_persistent_alloc(n+1);if(result)memcpy(result,p,n+1);return result;
}
static StreamTask* new_task(uint32_t* out){
    if(out&&valid_range(out,4))*out=0;
    if(!valid_range(out,4)||current){fail("TERRAX_STATE_ERROR","invalid stream output pointer or another task is active");return NULL;}
    StreamTask* t=(StreamTask*)tx_persistent_alloc(sizeof(*t));if(!t)return NULL;
    memset(t,0,sizeof(*t));t->id=generation++;if(!t->id)t->id=generation++;
    t->candidate=(TxWorld*)tx_persistent_alloc(sizeof(TxWorld));t->input=tx_persistent_alloc(WINDOW);
    if(!t->candidate||!t->input){if(t->candidate)tx_internal_free(t->candidate);if(t->input)tx_internal_free(t->input);tx_internal_free(t);return NULL;}
    memset(t->candidate,0,sizeof(TxWorld));clear_event(t);current=t;*out=t->id;return t;
}
static int source_event(StreamTask* t,uint32_t offset,uint32_t limit){
    if(offset>=limit)return fail("TERRAX_BAD_TILE_STREAM","source range exhausted");
    event(t,TX_STREAM_NEED_SOURCE,offset,limit-offset>WINDOW?WINDOW:limit-offset,NULL);return 0;
}
int32_t terra_world_stream_open_begin(uint32_t source_id,uint32_t size,uint32_t* out){
    if(out&&valid_range(out,4))*out=0;
    if(!source_id||size<16)return fail("TERRAX_INVALID_ARGUMENT","invalid stream source");
    StreamTask* t=new_task(out);if(!t)return fail("TERRAX_WASM_OOM","stream open allocation failed");
    t->stage=OPEN_FORMAT;t->source_id=source_id;t->source_size=size;
    event(t,TX_STREAM_NEED_SOURCE,0,size<65536?size:65536,NULL);return 0;
}
static const uint8_t* section(TxWorld* w,uint32_t i,uint32_t* length){
    if(i<TX_MAX_SECTION_OVERRIDES&&w->section_overrides[i].active){*length=w->section_overrides[i].len;return w->section_overrides[i].data;}
    *length=w->ends[i]-w->starts[i];return w->file+w->starts[i];
}
static int compact_copy(TxWorld* dest,TxWorld* source){
    if(source->maxTilesX<=0||source->maxTilesY<=0||(uint32_t)source->maxTilesX>METADATA_LIMIT/4-1)return 0;
    uint32_t count=source->pointer_count,format=(source->version>=135?24:4)+2+4*count+2+source->important_len;
    uint64_t total=format;
    for(uint32_t i=0;i<count;i++){uint32_t n;if(i!=1){section(source,i,&n);total+=n;}}
    if(total>METADATA_LIMIT)return 0;
    dest->file=tx_persistent_alloc((uint32_t)total);if(!dest->file)return 0;
    dest->file_len=(uint32_t)total;dest->stream_owned=1;
    uint8_t* p=dest->file;put32(p,source->version);uint32_t off=4;
    if(source->version>=135){memcpy(p+off,source->magic,7);off+=7;p[off++]=source->file_type;put32(p+off,source->revision);off+=4;for(uint32_t b=0;b<8;b++)p[off++]=(uint8_t)(source->favorite>>(8*b));}
    put16(p+off,count);off+=2;uint32_t table=off;off+=4*count;put16(p+off,source->tile_type_count);off+=2;
    memcpy(p+off,source->important,source->important_len);off+=source->important_len;
    for(uint32_t i=0;i<count;i++){put32(p+table+i*4,off);if(i!=1){uint32_t n;const uint8_t* bytes=section(source,i,&n);memcpy(p+off,bytes,n);off+=n;}}
    return parse_format(dest)&&parse_header(dest);
}
int32_t terra_world_stream_pixel_begin(uint32_t handle,const TxStreamPixelSpec* spec,uint32_t* out){
    if(out&&valid_range(out,4))*out=0;
    TxWorld* source=tx_get_world(handle);
    if(!source||!valid_range(spec,sizeof(*spec))||spec->abi_version!=1||spec->reserved||!spec->width||!spec->height||source->legacy_wld||
       !spec->resolved_maps_count||spec->resolved_maps_count>65536||spec->default_palette_index>=spec->resolved_maps_count||
       !tx_bridge_range_is_valid(spec->resolved_maps_ptr,spec->resolved_maps_count*sizeof(TxPixelMap)))
        return fail("TERRAX_INVALID_ARGUMENT","invalid stream pixel specification");
    const TxPixelMap* maps=(const TxPixelMap*)(uintptr_t)spec->resolved_maps_ptr;
    for(uint32_t i=0;i<spec->resolved_maps_count;i++)if(maps[i].active_mode>4||maps[i].block_inactive>1)return fail("TERRAX_INVALID_ARGUMENT","invalid resolved pixel map");
    uint64_t cols=((uint64_t)spec->width+63)/64,rows=((uint64_t)spec->height+63)/64;
    if(cols>65536||rows>65536||cols*rows>WINDOW/sizeof(TxPixelArtChunk*))return fail("TERRAX_INVALID_ARGUMENT","pixel stripe table exceeds bound");
    StreamTask* t=new_task(out);if(!t)return fail("TERRAX_WASM_OOM","pixel task allocation failed");
    t->is_write=1;t->old_handle=handle;t->source=source;
    TxWorld* w=t->candidate;
    if(!compact_copy(w,source))goto oom;
    t->source_id=source->stream_source_id;t->source_size=source->stream_source_size?source->stream_source_size:source->file_len;
    t->original_start=source->stream_source_id?source->stream_tile_start:source->starts[1];
    t->original_end=source->stream_source_id?source->stream_tile_end:source->ends[1];
    if(source->section_overrides[1].active){t->original_start=0;t->original_end=source->section_overrides[1].len;t->source_id=0;}
    t->cursor=t->original_start;
    w->pixel_art_maps=tx_persistent_alloc(spec->resolved_maps_count*sizeof(TxPixelMap));
    w->pixel_art_chunk_table=(TxPixelArtChunk**)tx_persistent_alloc((uint32_t)(cols*rows)*sizeof(TxPixelArtChunk*));
    w->stream_columns=(uint32_t*)tx_persistent_alloc(((uint32_t)w->maxTilesX+1)*4);
    t->stripe_capacity=(uint32_t)rows*2;
    t->stripe=tx_persistent_alloc(t->stripe_capacity*8192);
    t->stripe_nodes=(TxPixelArtChunk*)tx_persistent_alloc(t->stripe_capacity*sizeof(TxPixelArtChunk));
    t->output.data=tx_persistent_alloc(WINDOW);t->output.cap=WINDOW;t->output.ok=t->output.data!=NULL;
    if(!w->pixel_art_maps||!w->pixel_art_chunk_table||!w->stream_columns||!t->stripe||!t->stripe_nodes||!t->output.ok)goto oom;
    memcpy(w->pixel_art_maps,maps,spec->resolved_maps_count*sizeof(TxPixelMap));
    memset(w->pixel_art_chunk_table,0,(uint32_t)(cols*rows)*sizeof(TxPixelArtChunk*));
    w->pixel_art_map_count=spec->resolved_maps_count;w->pixel_art_indexed=1;w->pixel_art_start_x=spec->start_x;w->pixel_art_start_y=spec->start_y;
    w->pixel_art_width=spec->width;w->pixel_art_height=spec->height;w->pixel_art_chunk_cols=(uint32_t)cols;w->pixel_art_chunk_rows=(uint32_t)rows;
    w->pixel_art_default_index=spec->default_palette_index;w->pixel_art_skip_transparent=1;
    if(!tx_stream_metadata_begin(w,&t->metadata))goto oom;
    if(spec->preview_width&&!tx_output_begin(w,NULL,0,0,spec->preview_width))goto oom;
    t->prefix_length=w->starts[1];t->stage=WRITE_PREFIX;return 0;
oom:
    terra_world_stream_cancel(t->id);terra_world_stream_close(t->id);*out=0;
    return fail("TERRAX_WASM_OOM","pixel task metadata/stripe allocation failed");
}
static int finish_compact_open(StreamTask* t){
    TxWorld* w=t->candidate;uint32_t table=u32(w->file)>=135?26:6,count=u16(w->file+table-2);
    for(uint32_t i=2;i<count;i++)put32(w->file+table+i*4,u32(w->file+table+i*4)-(t->original_end-t->original_start));
    if(!parse_format(w)||!parse_header(w)||w->maxTilesX<=0||w->maxTilesY<=0||(uint32_t)w->maxTilesX>METADATA_LIMIT/4-1||validate_footer(w)<0)return -1;
    w->stream_source_id=t->source_id;w->stream_source_size=t->source_size;w->stream_tile_start=t->original_start;w->stream_tile_end=t->original_end;
    w->stream_columns=(uint32_t*)tx_persistent_alloc(((uint32_t)w->maxTilesX+1)*4);
    if(!w->stream_columns)return fail("TERRAX_WASM_OOM","column index allocation failed");
    t->cursor=t->original_start;t->stage=OPEN_SCAN;return 0;
}
int32_t terra_world_stream_operation_begin(uint32_t handle,const char* name,const char* request,uint32_t* out){
    if(out&&valid_range(out,4))*out=0;
    TxWorld* source=tx_get_world(handle);
    if(!source||source->legacy_wld)return fail("TERRAX_INVALID_HANDLE","stream operation needs a modern world");
    StreamTask* t=new_task(out);if(!t)return fail("TERRAX_WASM_OOM","operation task allocation failed");
    t->operation=copy_bridge_string(name,128);t->request=copy_bridge_string(request,WINDOW);
    if(!t->operation||!t->request||!json_validate_document(t->request,(int)strlen(t->request)))goto invalid;
    t->source=source;t->old_handle=handle;t->source_id=source->stream_source_id;
    t->source_size=source->stream_source_size?source->stream_source_size:source->file_len;
    t->original_start=t->source_id?source->stream_tile_start:source->starts[1];
    t->original_end=t->source_id?source->stream_tile_end:source->ends[1];
    if(source->section_overrides[1].active){t->source_id=0;t->original_start=0;t->original_end=source->section_overrides[1].len;}
    TxWorld* w=t->candidate;if(!compact_copy(w,source))goto invalid;
    w->marker_color_index=source->marker_color_index;w->icon_atlas=source->icon_atlas;
    t->cursor=t->original_start;
    w->stream_columns=(uint32_t*)tx_persistent_alloc(((uint32_t)w->maxTilesX+1)*4);
    if(!w->stream_columns)goto invalid;
    if(source->stream_columns)memcpy(w->stream_columns,source->stream_columns,((uint32_t)w->maxTilesX+1)*4);
    int jlen=(int)strlen(t->request);
    if(!strcmp(t->operation,"batch_update_tiles")||!strcmp(t->operation,"save")||!strcmp(t->operation,"header_patch")||!strcmp(t->operation,"replace_chests")||!strcmp(t->operation,"replace_bestiary")){
        t->is_write=1;t->output.data=tx_persistent_alloc(WINDOW);t->output.cap=WINDOW;t->output.ok=t->output.data!=NULL;if(!t->output.ok)goto invalid;
        if(!strcmp(t->operation,"batch_update_tiles")){
            if(tx_stream_parse_tile_rules(w,t->request,jlen,&t->rules,&t->rule_count)<0)goto invalid;
            t->region_scan=tx_region_stream_begin(w,t->rules,t->rule_count);if(!t->region_scan)goto invalid;
            t->stage=OP_REGION;
        }else{
            if(strcmp(t->operation,"save")){
                TxBuf response={0};buf_init(&response,256);int ok=op_execute_json(w,t->operation,t->request,&response);if(response.data)tx_internal_free(response.data);if(ok<0)goto invalid;
                TxWorld compact={0};if(!compact_copy(&compact,w))goto invalid;
                tx_internal_free(w->file);w->file=compact.file;w->file_len=compact.file_len;
                for(uint32_t i=0;i<TX_MAX_SECTION_OVERRIDES;i++){if(w->section_overrides[i].data)tx_internal_free(w->section_overrides[i].data);memset(&w->section_overrides[i],0,sizeof(w->section_overrides[i]));}
                if(w->important_override){tx_internal_free(w->important_override);w->important_override=NULL;}
                if(!parse_format(w)||!parse_header(w))goto invalid;
                if(w->maxTilesX!=source->maxTilesX||w->maxTilesY!=source->maxTilesY){fail("TERRAX_INVALID_ARGUMENT","streaming header patch cannot resize world dimensions");goto invalid;}
            }
            t->prefix_length=w->starts[1];t->stage=WRITE_PREFIX;
        }
    }else{
        int map=!strcmp(t->operation,"render_lit_map")||!strcmp(t->operation,"mark_tiles_and_chests_map");
        int png=!strcmp(t->operation,"render_thumbnail_png")||!strcmp(t->operation,"render_preview_png")||!strcmp(t->operation,"mark_tiles_and_chests_preview");
        if(!map&&!png)goto invalid;
        int32_t width=!strcmp(t->operation,"render_thumbnail_png")?1920:0;
        int pos=json_find_key(t->request,jlen,"max_w");if(pos>=0&&!json_extract_int(t->request,jlen,pos,&width))goto invalid;
        if(width<=0||width>(int32_t)w->maxTilesX)width=(int32_t)w->maxTilesX;
        MapMarkerEntry* markers=NULL;uint32_t count=0;
        if(!parse_marker_array(t->request,jlen,"tile_markers","tile_type",&markers,&count))goto invalid;
        t->full_png=png&&width==(int32_t)w->maxTilesX;
        int ok=tx_output_begin(w,markers,count,0,(uint32_t)(map||t->full_png?256:width));if(markers)tx_internal_free(markers);if(!ok)goto invalid;
        if(!parse_marker_array(t->request,jlen,"chest_markers","item_id",&t->chest_markers,&t->chest_count))goto invalid;
        t->marker_scan=tx_marker_stream_begin(w,w->prepared_output->markers,count);if(!t->marker_scan)goto invalid;
        t->result_kind=map?2:1;t->stage=OP_SCAN;
    }
    return 0;
invalid:
    terra_world_stream_cancel(t->id);terra_world_stream_close(t->id);if(out&&valid_range(out,4))*out=0;
    return fail("TERRAX_STREAM_OPERATION_FAILED","invalid operation or bounded preparation failed");
}
int32_t terra_world_stream_supply_source(uint32_t id,uint32_t source_id,uint32_t offset,const uint8_t* bytes,uint32_t length){
    StreamTask* t=task(id);
    if(!t||t->event.kind!=TX_STREAM_NEED_SOURCE||source_id!=t->event.source_id||offset!=t->event.offset||length!=t->event.length||!valid_range(bytes,length))
        return fail("TERRAX_INVALID_ARGUMENT","source event/range mismatch");
    if(t->stage==OPEN_FORMAT){
        if(length<16)return fail("TERRAX_TRUNCATED_FORMAT","format truncated");
        uint32_t version=u32(bytes),table=version>=135?26:6;
        if(version<88||table>length||u16(bytes+table-2)<3||u16(bytes+table-2)>TX_MAX_SECTIONS)return fail("TERRAX_BAD_POINTERS","stream requires a modern WLD");
        uint32_t count=u16(bytes+table-2);if(table+count*4+2>length)return fail("TERRAX_TRUNCATED_FORMAT","format pointer table truncated");
        uint32_t prior=table+count*4+2+(u16(bytes+table+count*4)+7)/8;
        if(prior>length)return fail("TERRAX_TRUNCATED_FORMAT","important bitmap truncated");
        for(uint32_t i=0;i<count;i++){uint32_t pos=u32(bytes+table+4*i);if(pos<prior||pos>t->source_size)return fail("TERRAX_BAD_POINTERS","invalid source pointers");prior=pos;}
        t->original_start=u32(bytes+table+4);t->original_end=u32(bytes+table+8);
        t->prefix_length=t->original_start;t->suffix_length=t->source_size-t->original_end;
        uint64_t compact=(uint64_t)t->prefix_length+t->suffix_length;
        if(compact>METADATA_LIMIT)return fail("TERRAX_INVALID_ARGUMENT","non-tile metadata exceeds bounded image");
        t->candidate->file=tx_persistent_alloc((uint32_t)compact);if(!t->candidate->file)return fail("TERRAX_WASM_OOM","metadata image allocation failed");
        t->candidate->stream_owned=1;t->candidate->file_len=(uint32_t)compact;
        t->load_offset=0;t->stage=OPEN_PREFIX;
    }else if(t->stage==OPEN_PREFIX){
        memcpy(t->candidate->file+offset,bytes,length);t->load_offset+=length;
    }else if(t->stage==OPEN_SUFFIX){
        memcpy(t->candidate->file+t->prefix_length+offset-t->original_end,bytes,length);t->load_offset+=length;
    }else {memcpy(t->input,bytes,length);t->input_offset=offset;t->input_length=length;}
    clear_event(t);return 0;
}
static int read_next(StreamTask* t,TxTile* tile,uint8_t* raw,uint32_t* raw_length){
    TxWorld* w=t->candidate;uint8_t* saved=w->file;uint32_t saved_len=w->file_len,off,end;
    if(!t->source_id&&t->source){
        w->file=t->source->section_overrides[1].active?t->source->section_overrides[1].data:t->source->file;
        w->file_len=t->source->section_overrides[1].active?t->source->section_overrides[1].len:t->source->file_len;
        off=t->cursor;end=t->original_end;
    }else {
        if(t->cursor<t->input_offset||t->cursor>=t->input_offset+t->input_length||
           (t->input_length-(t->cursor-t->input_offset)<TILE_MAX_BYTES&&t->input_offset+t->input_length<t->original_end))return source_event(t,t->cursor,t->original_end)==0?0:-1;
        w->file=t->input;w->file_len=t->input_length;off=t->cursor-t->input_offset;end=t->input_length;
    }
    uint8_t* saved_important=w->important;uint32_t saved_important_len=w->important_len;uint16_t saved_types=w->tile_type_count;
    if(t->source){w->important=t->source->important;w->important_len=t->source->important_len;w->tile_type_count=t->source->tile_type_count;}
    uint32_t before=off;int ok=read_tile_at(w,&off,end,tile);
    w->important=saved_important;w->important_len=saved_important_len;w->tile_type_count=saved_types;
    if(ok&&raw){*raw_length=off-before;memcpy(raw,w->file+before,*raw_length);}
    w->file=saved;w->file_len=saved_len;
    if(!ok||off<=before||(uint32_t)tile->same+1>(uint32_t)w->maxTilesY-t->y)return fail("TERRAX_BAD_TILE_STREAM","truncated record or RLE exceeds column");
    t->cursor+=off-before;return 1;
}
static int flush_output(StreamTask* t){
    if(!t->output.len)return 0;
    if(t->output.len>UINT32_MAX-t->output_offset)return fail("TERRAX_INVALID_ARGUMENT","output size overflow");
    event(t,TX_STREAM_OUTPUT,t->output_offset,t->output.len,t->output.data);return 1;
}
static int flush_merge(StreamTask* t){
    if(t->merged_valid){
        write_tile(t->candidate,&t->output,&t->merged,t->merged_count);
        if(t->candidate->prepared_output&&!tx_output_stream_run(t->candidate,t->x,t->y-t->merged_count-1,&t->merged,t->merged_count+1))return 0;
        t->merged_valid=0;
    }
    return t->output.ok;
}
static int want_stripe(StreamTask* t){
    TxWorld* w=t->candidate;int64_t px=(int64_t)t->x-w->pixel_art_start_x;
    if(px<0||(uint64_t)px>=w->pixel_art_width)return 0;
    uint32_t first=((uint32_t)px/64)*64;
    if(t->stripe_ready&&first==t->stripe_first)return 0;
    memset(w->pixel_art_chunk_table,0,w->pixel_art_chunk_cols*w->pixel_art_chunk_rows*sizeof(TxPixelArtChunk*));
    t->stripe_ready=0;t->stripe_first=first;t->stripe_count=w->pixel_art_width-first<64?w->pixel_art_width-first:64;
    event(t,TX_STREAM_NEED_PIXELS,0,0,NULL);t->event.first_column=first;t->event.column_count=t->stripe_count;return 1;
}
int32_t terra_world_stream_supply_pixels(uint32_t id,const uint8_t* records,uint32_t bytes,uint32_t count){
    StreamTask* t=task(id);
    if(!t||t->event.kind!=TX_STREAM_NEED_PIXELS||count>t->stripe_capacity||(uint64_t)count*RECORD_BYTES!=bytes||(bytes&&!valid_range(records,bytes)))
        return fail("TERRAX_INVALID_ARGUMENT","invalid pixel stripe payload");
    TxWorld* w=t->candidate;uint32_t cx=t->stripe_first/64;
    /* Validate entire stripe before attaching any node. */
    for(uint32_t i=0;i<count;i++){
        const uint8_t* p=records+i*RECORD_BYTES;uint32_t x=u16(p),y=u16(p+2),used=u16(p+4),actual=0;
        if(x!=cx||y>=w->pixel_art_chunk_rows||!used||used>4096||u16(p+6))return fail("TERRAX_INVALID_ARGUMENT","invalid stripe chunk coordinate/header");
        for(uint32_t j=0;j<i;j++)if(u16(records+j*RECORD_BYTES+2)==y)return fail("TERRAX_INVALID_ARGUMENT","duplicate stripe chunk");
        for(uint32_t cell=0;cell<4096;cell++){
            uint32_t index=u16(p+8+2*cell);
            if(index>=w->pixel_art_map_count||(index&&((uint64_t)x*64+(cell%64)>=w->pixel_art_width||(uint64_t)y*64+cell/64>=w->pixel_art_height)))return fail("TERRAX_INVALID_ARGUMENT","stripe index outside palette/canvas");
            actual+=index!=0;
        }
        if(actual!=used)return fail("TERRAX_INVALID_ARGUMENT","stripe used count mismatch");
    }
    for(uint32_t i=0;i<count;i++){
        const uint8_t* p=records+i*RECORD_BYTES;uint32_t y=u16(p+2);
        memcpy(t->stripe+i*8192,p+8,8192);TxPixelArtChunk* node=&t->stripe_nodes[i];memset(node,0,sizeof(*node));
        node->indices=(uint16_t*)(t->stripe+i*8192);w->pixel_art_chunk_table[y*w->pixel_art_chunk_cols+cx]=node;
    }
    t->stripe_ready=1;tx_stream_metadata_pixels(w,&t->metadata,t->stripe_first,t->stripe_count);clear_event(t);return 0;
}
int32_t terra_world_stream_ack_output(uint32_t id){
    StreamTask* t=task(id);if(!t||t->event.kind!=TX_STREAM_OUTPUT)return fail("TERRAX_STATE_ERROR","no output awaits acknowledgement");
    if(t->stage==OP_MEDIA){
        if(t->map_encoder){if(!tx_stream_map_ack(t->map_encoder))return -1;}
        else if(t->png_encoder){if(!tx_stream_png_ack(t->png_encoder))return -1;}
        t->result_length=t->event.offset+t->event.length>t->result_length?t->event.offset+t->event.length:t->result_length;
    }else if(t->stage==OP_RESULT)t->result_offset+=t->event.length;
    else if(t->stage!=WRITE_PATCH){t->output_offset+=t->event.length;t->output.len=0;}
    else t->stage=DONE;
    clear_event(t);return 0;
}
static int finish_write_tiles(StreamTask* t){
    TxWorld* w=t->candidate;
    if(t->cursor!=t->original_end)return fail("TERRAX_BAD_TILE_STREAM","tile section contains trailing bytes");
    w->stream_columns[t->x]=t->output_offset+t->output.len;
    w->stream_tile_start=t->prefix_length;w->stream_tile_end=t->output_offset+t->output.len;
    if(w->prepared_output)tx_output_stream_finish(w);
    if((w->pixel_art_indexed&&!tx_stream_metadata_finish(w,&t->metadata))||!parse_format(w)||!parse_header(w)||validate_footer(w)<0)return -1;
    t->suffix_offset=w->starts[2];t->stage=WRITE_SUFFIX;return 0;
}
int32_t terra_world_stream_step(uint32_t id,uint32_t units,TxStreamEvent* out){
    StreamTask* t=task(id);if(!t||t->stage==ADOPTED||!valid_range(out,sizeof(*out)))return fail("TERRAX_STATE_ERROR","invalid stream task/event pointer");
    if(t->old_handle&&tx_get_world(t->old_handle)!=t->source)return fail("TERRAX_INVALID_HANDLE","source world changed during stream task");
    if(t->event.kind){*out=t->event;return 0;}
    uint32_t budget=units?units:1;if(budget>4096)budget=4096;budget*=256;
    TxWorld* w=t->candidate;
    while(budget--&&!t->event.kind){
        if(t->stage==OPEN_PREFIX){
            if(t->load_offset<t->prefix_length){event(t,TX_STREAM_NEED_SOURCE,t->load_offset,t->prefix_length-t->load_offset>WINDOW?WINDOW:t->prefix_length-t->load_offset,NULL);break;}
            t->stage=OPEN_SUFFIX;t->load_offset=t->original_end;
        }else if(t->stage==OPEN_SUFFIX){
            if(t->load_offset<t->source_size){source_event(t,t->load_offset,t->source_size);break;}
            if(finish_compact_open(t)<0)return -1;
        }else if(t->stage==OPEN_SCAN){
            if(t->x==(uint32_t)w->maxTilesX){if(t->cursor!=t->original_end)return fail("TERRAX_BAD_TILE_STREAM","tile section trailing bytes");w->stream_columns[t->x]=t->cursor;t->stage=DONE;continue;}
            if(!t->y)w->stream_columns[t->x]=t->cursor;
            TxTile tile;int r=read_next(t,&tile,NULL,NULL);if(r<0)return -1;if(!r)break;
            t->y+=(uint32_t)tile.same+1;if(t->y==(uint32_t)w->maxTilesY){t->x++;t->y=0;}
        }else if(t->stage==OP_REGION||t->stage==OP_SCAN){
            if(t->x==(uint32_t)w->maxTilesX){
                if(t->cursor!=t->original_end)return fail("TERRAX_BAD_TILE_STREAM","operation tile section trailing bytes");
                if(t->stage==OP_REGION){
                    tx_region_stream_finish(w,t->region_scan);tx_region_stream_free(t->region_scan);t->region_scan=NULL;
                    t->x=t->y=0;t->cursor=t->original_start;t->input_length=0;t->prefix_length=w->starts[1];t->stage=WRITE_PREFIX;continue;
                }
                TxBuf points={0};if(!tx_marker_stream_finish(t->marker_scan,&points))return -1;
                if(points.len){w->prepared_output->points.data=tx_persistent_alloc(points.len);if(!w->prepared_output->points.data){tx_internal_free(points.data);return -1;}memcpy(w->prepared_output->points.data,points.data,points.len);}
                w->prepared_output->points.len=points.len;if(points.data)tx_internal_free(points.data);
                tx_marker_stream_free(t->marker_scan);t->marker_scan=NULL;
                tx_output_stream_finish(w);
                if(t->result_kind==2||t->full_png){
                    if(t->result_kind==2)t->map_encoder=tx_stream_map_begin(w,t->chest_markers,t->chest_count,w->prepared_output->markers,w->prepared_output->marker_count);
                    else t->png_encoder=tx_stream_png_begin(w,0,0,t->chest_markers,t->chest_count,w->prepared_output->markers,w->prepared_output->marker_count);
                    if(!t->map_encoder&&!t->png_encoder)return fail("TERRAX_WASM_OOM","bounded media encoder allocation failed");
                    t->stage=OP_MEDIA;continue;
                }
                TxBuf response={0};buf_init(&response,256);int ok=op_execute_json(w,t->operation,t->request,&response);
                if(ok<0){if(response.data)tx_internal_free(response.data);return -1;}
                t->result=(uint8_t*)(uintptr_t)tx_last_ptr;t->result_length=tx_last_len;
                if(response.data&&response.data!=t->result)tx_internal_free(response.data);
                t->stage=OP_RESULT;continue;
            }
            if(!t->y)w->stream_columns[t->x]=t->cursor;
            TxTile tile;int r=read_next(t,&tile,NULL,NULL);if(r<0)return -1;if(!r)break;
            uint32_t run=(uint32_t)tile.same+1;
            if(t->stage==OP_REGION){if(!tx_region_stream_run(w,t->region_scan,t->x,t->y,&tile,run))return -1;}
            else if(!tx_output_stream_run(w,t->x,t->y,&tile,run)||!tx_marker_stream_run(t->marker_scan,t->x,t->y,&tile,run))return -1;
            t->y+=run;if(t->y==(uint32_t)w->maxTilesY){if(t->marker_scan&&!tx_marker_stream_column(t->marker_scan))return -1;t->x++;t->y=0;}
        }else if(t->stage==OP_MEDIA){
            uint32_t offset,length;const uint8_t* bytes;
            int ready=t->map_encoder?tx_stream_map_pull(t->map_encoder,&offset,&bytes,&length):tx_stream_png_pull(t->png_encoder,&offset,&bytes,&length);
            if(ready<0)return -1;if(ready){event(t,TX_STREAM_OUTPUT,offset,length,bytes);break;}
            uint32_t first,count;
            int range=t->map_encoder?tx_stream_map_range(t->map_encoder,&first,&count):tx_stream_png_range(t->png_encoder,&first,&count);
            if(range<0)continue;
            if(!range){t->stage=DONE;if(t->map_encoder)t->result_length=tx_stream_map_size(t->map_encoder);continue;}
            t->x=t->map_encoder?first:0;t->scan_end=t->map_encoder?first+count:(uint32_t)w->maxTilesX;t->y=0;
            t->cursor=w->stream_columns[t->x];t->input_length=0;t->stage=OP_MEDIA_SCAN;
        }else if(t->stage==OP_MEDIA_SCAN){
            if(t->x==t->scan_end){
                int ok=t->map_encoder?tx_stream_map_finish_strip(t->map_encoder):tx_stream_png_finish_strip(t->png_encoder);
                if(!ok)return -1;t->stage=OP_MEDIA;continue;
            }
            TxTile tile;int r=read_next(t,&tile,NULL,NULL);if(r<0)return -1;if(!r)break;
            uint32_t run=(uint32_t)tile.same+1;
            int ok=t->map_encoder?tx_stream_map_run(t->map_encoder,t->x,t->y,&tile,run):tx_stream_png_run(t->png_encoder,t->x,t->y,&tile,run);
            if(!ok)return -1;t->y+=run;if(t->y==(uint32_t)w->maxTilesY){t->x++;t->y=0;}
        }else if(t->stage==OP_RESULT){
            if(t->result_offset<t->result_length){uint32_t n=t->result_length-t->result_offset;if(n>WINDOW)n=WINDOW;event(t,TX_STREAM_OUTPUT,t->result_offset,n,t->result+t->result_offset);break;}
            t->stage=DONE;
        }else if(t->stage==WRITE_PREFIX){
            if(t->output_offset<t->prefix_length){uint32_t n=t->prefix_length-t->output_offset;if(n>WINDOW)n=WINDOW;event(t,TX_STREAM_OUTPUT,t->output_offset,n,w->file+t->output_offset);break;}
            t->stage=WRITE_SCAN;
        }else if(t->stage==WRITE_SCAN){
            if(t->output.len>WINDOW-2*TILE_MAX_BYTES){flush_output(t);break;}
            if(t->x==(uint32_t)w->maxTilesX){if(!flush_merge(t)||finish_write_tiles(t)<0)return -1;if(flush_output(t))break;continue;}
            if(!t->remaining){
                if(!t->y)w->stream_columns[t->x]=t->output_offset+t->output.len;
                if(want_stripe(t))break;
                uint8_t raw[TILE_MAX_BYTES];uint32_t n;int r=read_next(t,&t->source_tile,raw,&n);if(r<0)return -1;if(!r)break;
                t->remaining=(uint32_t)t->source_tile.same+1;t->source_run_end=t->y+t->remaining;
                if(w->pixel_art_indexed)tx_stream_metadata_source(&t->metadata,t->x,t->y,&t->source_tile,t->remaining);
                if(!w->pixel_art_indexed){
                    uint32_t run=tx_region_run(w,t->x,t->y,t->remaining);
                    TxTile tile=t->source_tile;
                    tx_apply_tile_rules(&tile,t->rules,t->rule_count,run,tx_region_at(w,t->x,t->y),t->y,w->worldSurface);
                    write_tile(w,&t->output,&tile,run-1);t->remaining-=run;t->y+=run;
                }
                int64_t px=(int64_t)t->x-w->pixel_art_start_x;
                if(w->pixel_art_indexed&&(px<0||(uint64_t)px>=w->pixel_art_width)){
                    if(w->prepared_output&&!tx_output_stream_run(w,t->x,t->y,&t->source_tile,t->remaining))return -1;
                    memcpy(t->output.data+t->output.len,raw,n);t->output.len+=n;t->y+=t->remaining;t->remaining=0;
                }
            }
            if(t->remaining&&!w->pixel_art_indexed){
                uint32_t run=tx_region_run(w,t->x,t->y,t->remaining);TxTile tile=t->source_tile;
                tx_apply_tile_rules(&tile,t->rules,t->rule_count,run,tx_region_at(w,t->x,t->y),t->y,w->worldSurface);
                write_tile(w,&t->output,&tile,run-1);t->remaining-=run;t->y+=run;
            }else if(t->remaining){
                int64_t py=(int64_t)t->y-w->pixel_art_start_y;
                if(py<0||(uint64_t)py>=w->pixel_art_height){
                    if(!flush_merge(t))return -1;
                    uint32_t run=t->remaining;if(py<0&&(uint64_t)(-py)<run)run=(uint32_t)(-py);
                    if(w->prepared_output&&!tx_output_stream_run(w,t->x,t->y,&t->source_tile,run))return -1;
                    write_tile(w,&t->output,&t->source_tile,run-1);t->remaining-=run;t->y+=run;
                }else{
                    TxTile tile=t->source_tile;tile.same=0;apply_pixel_art_at(w,t->x,t->y,&tile);
                    if(t->merged_valid&&same_tile(&t->merged,&tile)&&t->merged_count<32767)t->merged_count++;
                    else {if(!flush_merge(t))return -1;t->merged=tile;t->merged_valid=1;t->merged_count=0;}
                    t->remaining--;t->y++;
                }
                if(!t->remaining&&!flush_merge(t))return -1;
            }
            if(t->y==(uint32_t)w->maxTilesY){t->x++;t->y=0;}
        }else if(t->stage==WRITE_SUFFIX){
            if(t->suffix_offset<w->file_len){uint32_t n=w->file_len-t->suffix_offset;if(n>WINDOW)n=WINDOW;memcpy(t->output.data,w->file+t->suffix_offset,n);t->output.len=n;t->suffix_offset+=n;flush_output(t);break;}
            w->stream_source_size=t->output_offset;
            memcpy(t->patch,w->file,w->format_len);uint32_t table=w->version>=135?26:6;
            for(uint32_t i=0;i<w->pointer_count;i++){uint32_t pos=w->starts[i];if(i>=2)pos+=w->stream_tile_end-w->stream_tile_start;put32(t->patch+table+4*i,pos);}
            t->stage=WRITE_PATCH;event(t,TX_STREAM_OUTPUT,0,w->format_len,t->patch);break;
        }else if(t->stage==DONE){
            event(t,TX_STREAM_READY,0,0,NULL);t->event.result_size=t->is_write?w->stream_source_size:t->source_size;
            if(t->result_kind){t->event.reserved=t->result_kind;t->event.result_size=t->result_length;}
            t->event.completed_columns=t->event.total_columns;break;
        }else return fail("TERRAX_STATE_ERROR","unknown stream stage");
    }
    *out=t->event;return 0;
}
int32_t terra_world_stream_adopt(uint32_t id,uint32_t source_id,uint32_t* out){
    StreamTask* t=task(id);if(out&&valid_range(out,4))*out=0;
    if(!t||t->stage!=DONE||t->result_kind||t->event.kind!=TX_STREAM_READY||!source_id||!valid_range(out,4))return fail("TERRAX_STATE_ERROR","candidate is not ready for adoption");
    TxWorld* w=t->candidate;
    memset(&w->marker_color_index,0,sizeof(w->marker_color_index));memset(&w->icon_atlas,0,sizeof(w->icon_atlas));
    if(w->region_mask){tx_internal_free(w->region_mask);w->region_mask=NULL;}w->surface_sand_split=0;
    if(t->rules){tx_internal_free(t->rules);t->rules=NULL;}
    if(w->pixel_art_maps){tx_internal_free(w->pixel_art_maps);w->pixel_art_maps=NULL;}
    if(w->pixel_art_chunk_table){tx_internal_free(w->pixel_art_chunk_table);w->pixel_art_chunk_table=NULL;}
    w->pixel_art_indexed=0;w->pixel_art_map_count=0;w->stream_source_id=source_id;
    tx_stream_activate_world(w,out);tx_internal_free(w);t->candidate=NULL;t->stage=ADOPTED;t->old_handle=0;tx_clear_error();return 0;
}
int32_t terra_world_stream_cancel(uint32_t id){
    StreamTask* t=current&&current->id==id?current:NULL;if(!t||t->stage==ADOPTED)return fail("TERRAX_STATE_ERROR","task cannot be cancelled");
    discard_candidate(t);t->stage=CANCELLED;clear_event(t);return 0;
}
int32_t terra_world_stream_close(uint32_t id){
    StreamTask* t=current&&current->id==id?current:NULL;if(!t)return fail("TERRAX_STATE_ERROR","invalid stream task");
    if(t->stage!=ADOPTED)discard_candidate(t);
    tx_stream_metadata_discard(&t->metadata);
    if(t->marker_scan)tx_marker_stream_free(t->marker_scan);
    if(t->region_scan)tx_region_stream_free(t->region_scan);
    if(t->rules)tx_internal_free(t->rules);
    if(t->operation)tx_internal_free(t->operation);if(t->request)tx_internal_free(t->request);
    if(t->result)tx_internal_free(t->result);
    if(t->chest_markers)tx_internal_free(t->chest_markers);
    if(t->map_encoder)tx_stream_map_free(t->map_encoder);if(t->png_encoder)tx_stream_png_free(t->png_encoder);
    if(t->input)tx_internal_free(t->input);if(t->output.data)tx_internal_free(t->output.data);
    if(t->stripe)tx_internal_free(t->stripe);if(t->stripe_nodes)tx_internal_free(t->stripe_nodes);
    tx_internal_free(t);current=NULL;return 0;
}
