/* Streaming, bounded-memory WLD circuit session. See docs/CIRCUIT_WORLD_ABI_V1.md. */
#include "terra_circuit_world_internal.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t* tx_persistent_alloc(uint32_t);
extern void tx_persistent_free(void*);
extern TxWorld* tx_get_world(uint32_t);
extern void tx_set_error(const char*,const char*);
extern int read_tile_at(TxWorld*,uint32_t*,uint32_t,TxTile*);
extern void write_tile(TxWorld*,TxBuf*,const TxTile*,uint32_t);
extern void tx_internal_free(void*);
extern int tx_bridge_range_is_valid(uint32_t,uint32_t);

typedef union CxAllocation { struct { uint32_t bytes; } v; max_align_t alignment; } CxAllocation;
static CxWorld* sessions[4];
static uint32_t next_session=1;
CxWorld* cx_lookup(uint32_t id) {
    for(uint32_t i=0;i<4;i++)if(sessions[i]&&sessions[i]->id==id)return sessions[i];
    return NULL;
}
static uint32_t vm_bytes(CxWorld* w) { uint32_t bytes=0;TerraVmStats s;if(w->vm){terra_vm_stats(w->vm,&s);bytes=s.allocated_bytes;}TerraTwldStats t;if(w->twld){terra_twld_stats(w->twld,&t);bytes+=t.allocated_bytes;}return bytes; }
uint32_t cx_vm_available(CxWorld* w){uint64_t used=w->bytes;TerraTwldStats t;if(w->twld){terra_twld_stats(w->twld,&t);used+=t.allocated_bytes;}return used<w->maximum?w->maximum-(uint32_t)used:0u;}
void* cx_alloc(CxWorld* w,uint32_t bytes) {
    uint64_t n=(uint64_t)bytes+sizeof(CxAllocation)+32u;
    if(n>UINT32_MAX||(uint64_t)w->bytes+vm_bytes(w)+n>w->maximum) {
        cx_fail(w,TCW_MEMORY,"circuit allocation exceeds the available process memory budget");return NULL;
    }
    CxAllocation* p=(CxAllocation*)tx_persistent_alloc(bytes+(uint32_t)sizeof(CxAllocation));
    if(!p){cx_fail(w,TCW_MEMORY,"circuit allocation failed");return NULL;}
    p->v.bytes=(uint32_t)n;w->bytes+=(uint32_t)n;
    if(w->bytes+vm_bytes(w)>w->peak)w->peak=w->bytes+vm_bytes(w);
    return p+1;
}
void cx_free(CxWorld* w,void* pointer) {
    if(!pointer)return;CxAllocation* p=(CxAllocation*)pointer-1;w->bytes-=p->v.bytes;tx_persistent_free(p);
}
int cx_fail(CxWorld* w,int status,const char* message) {
    if(w)w->error=(uint32_t)-status;
    tx_set_error(status==TCW_MEMORY?"TERRAX_CIRCUIT_MEMORY":status==TCW_FORMAT?"TERRAX_CIRCUIT_FORMAT":"TERRAX_CIRCUIT_STATE",message);
    return status;
}
static int grow_pages(CxWorld* w,void*** pages,uint32_t* capacity,uint32_t needed) {
    if(needed<=*capacity)return 1;uint32_t n=*capacity?*capacity:16u;
    while(n<needed){if(n>UINT32_MAX/2u)return 0;n*=2u;}
    if((uint64_t)n*sizeof(void*)>UINT32_MAX)return 0;
    void** p=(void**)cx_alloc(w,n*(uint32_t)sizeof(void*));if(!p)return 0;
    memset(p,0,n*sizeof(void*));if(*pages)memcpy(p,*pages,*capacity*sizeof(void*));
    cx_free(w,*pages);*pages=p;*capacity=n;return 1;
}
int cx_words_size(CxWorld* w,CxWords* b,uint32_t size) {
    uint32_t needed=(uint32_t)(((uint64_t)size+CX_WORDS-1)/CX_WORDS);
    if(!grow_pages(w,(void***)&b->pages,&b->pages_capacity,needed))return 0;
    while(b->pages_count<needed){uint32_t* p=(uint32_t*)cx_alloc(w,CX_PAGE_SIZE);if(!p)return 0;memset(p,0,CX_PAGE_SIZE);b->pages[b->pages_count++]=p;}
    if(size>b->length)b->length=size;return 1;
}
uint32_t cx_word(const CxWords* b,uint32_t i){return b->pages[i/(CX_WORDS)][i&(CX_WORDS-1u)];}
void cx_set_word(CxWords* b,uint32_t i,uint32_t v){b->pages[i/CX_WORDS][i&(CX_WORDS-1u)]=v;}
void cx_words_free(CxWorld* w,CxWords* b){for(uint32_t i=0;i<b->pages_count;i++)cx_free(w,b->pages[i]);cx_free(w,b->pages);memset(b,0,sizeof(*b));}
void cx_words_drop_page(CxWorld* w,CxWords* b,uint32_t page){if(page<b->pages_count){cx_free(w,b->pages[page]);b->pages[page]=NULL;}}
int cx_bytes_append(CxWorld* w,CxBytes* b,const void* data,uint32_t size) {
    if(size>UINT32_MAX-b->length)return cx_fail(w,TCW_MEMORY,"circuit byte stream exceeds the address space")==0;
    uint32_t needed=(uint32_t)(((uint64_t)b->length+size+CX_PAGE_SIZE-1u)/CX_PAGE_SIZE);
    if(!grow_pages(w,(void***)&b->pages,&b->pages_capacity,needed))return 0;
    while(b->pages_count<needed){uint8_t* p=(uint8_t*)cx_alloc(w,CX_PAGE_SIZE);if(!p)return 0;b->pages[b->pages_count++]=p;}
    const uint8_t* p=(const uint8_t*)data;
    while(size){uint32_t at=b->length&(CX_PAGE_SIZE-1u),n=CX_PAGE_SIZE-at;if(n>size)n=size;memcpy(b->pages[b->length>>CX_PAGE_SHIFT]+at,p,n);b->length+=n;p+=n;size-=n;}
    return 1;
}
int cx_bytes_byte(CxWorld* w,CxBytes* b,uint8_t value){return cx_bytes_append(w,b,&value,1);}
uint8_t cx_bytes_get(const CxBytes* b,uint32_t at){return b->pages[at>>CX_PAGE_SHIFT][at&(CX_PAGE_SIZE-1u)];}
void cx_bytes_copy(const CxBytes* b,uint32_t at,void* target,uint32_t size){uint8_t* out=(uint8_t*)target;while(size){uint32_t p=at&(CX_PAGE_SIZE-1u),n=CX_PAGE_SIZE-p;if(n>size)n=size;memcpy(out,b->pages[at>>CX_PAGE_SHIFT]+p,n);out+=n;at+=n;size-=n;}}
int cx_bytes_equal(const CxBytes* b,uint32_t at,const uint8_t* data,uint32_t size){while(size){uint32_t p=at&(CX_PAGE_SIZE-1u),n=CX_PAGE_SIZE-p;if(n>size)n=size;if(memcmp(b->pages[at>>CX_PAGE_SHIFT]+p,data,n))return 0;data+=n;at+=n;size-=n;}return 1;}
void cx_bytes_free(CxWorld* w,CxBytes* b){for(uint32_t i=0;i<b->pages_count;i++)cx_free(w,b->pages[i]);cx_free(w,b->pages);memset(b,0,sizeof(*b));}
uint32_t cx_var_size(uint32_t v){uint32_t n=1;while(v>=128u){v>>=7;++n;}return n;}
uint32_t cx_var_put(uint8_t* out,uint32_t v){uint32_t n=0;while(v>=128u){out[n++]=(uint8_t)(v|128u);v>>=7;}out[n++]=(uint8_t)v;return n;}
uint32_t cx_var_read(const CxBytes* b,uint32_t* at){uint32_t v=0;for(uint32_t shift=0;shift<35;shift+=7){uint8_t c=cx_bytes_get(b,(*at)++);v|=(uint32_t)(c&127u)<<shift;if(!(c&128u))break;}return v;}
uint32_t cx_wire_mask(const TxTile* t){return t->wire_red|(t->wire_blue<<1)|(t->wire_green<<2)|(t->wire_yellow<<3);}
int cx_inside_wiring(const CxWorld* w,uint32_t x,uint32_t y){return w->width>4u&&w->height>4u&&x>=2u&&y>=2u&&x<w->width-2u&&y<w->height-2u;}
int cx_emit(CxWorld* w,uint32_t kind,uint32_t source,uint32_t offset,uint32_t length,const void* data){
    memset(&w->event,0,sizeof(w->event));w->event.abi_version=1;w->event.kind=kind;
    w->event.source_id=source;w->event.offset=offset;w->event.length=length;w->event.data_ptr=(uint32_t)(uintptr_t)data;
    w->event.phase=w->phase;w->event.completed=w->x;w->event.total=w->width;return TCW_CONTINUE;
}
int cx_world_tile(CxWorld* w,TxTile* out){
    if(w->cursor>=w->tile_end)return cx_fail(w,TCW_FORMAT,"circuit tile stream ended before the world columns");
    if(!w->world_source){uint32_t p=w->cursor;if(!read_tile_at(w->world,&p,w->tile_end,out))return cx_fail(w,TCW_FORMAT,"invalid circuit tile record");w->cursor=p;return TCW_OK;}
    uint32_t need=w->tile_end-w->cursor;if(need>32u)need=32u;
    if(w->cursor<w->input_offset||w->cursor-w->input_offset>w->input_length||need>w->input_length-(w->cursor-w->input_offset)){
        uint32_t n=w->tile_end-w->cursor;if(n>TERRA_CIRCUIT_WORLD_WINDOW)n=TERRA_CIRCUIT_WORLD_WINDOW;
        w->read_target=0;return cx_emit(w,TCW_READ,w->world_source,w->cursor,n,NULL);
    }
    uint8_t* previous=w->world->file;uint32_t old_len=w->world->file_len;
    w->world->file=w->input;w->world->file_len=w->input_length;
    uint32_t p=w->cursor-w->input_offset;int ok=read_tile_at(w->world,&p,w->input_length,out);
    w->world->file=previous;w->world->file_len=old_len;
    if(!ok)return cx_fail(w,TCW_FORMAT,"invalid circuit tile window");
    w->cursor=w->input_offset+p;return TCW_OK;
}
static int cache_page(CxWorld* w,uint32_t page,uint32_t* out){
    if(page>=w->store_pages)return cx_fail(w,TCW_IO,"compiled circuit scratch offset is outside its file");
    uint32_t found=w->cache_map[page];
    if(found){uint32_t slot=found-1u;w->cache_pages[slot].stamp=++w->cache_clock;*out=slot;return TCW_OK;}
    uint32_t slot=CX_NONE,oldest=UINT32_MAX;
    for(uint32_t i=0;i<w->cache_count;i++)if(!w->cache_pages[i].valid){slot=i;break;}else if(w->cache_pages[i].stamp<oldest){oldest=w->cache_pages[i].stamp;slot=i;}
    if(slot==CX_NONE)return cx_fail(w,TCW_IO,"compiled circuit page cache is unavailable");
    CxCachePage* p=w->cache_pages+slot;
    if(p->valid&&p->dirty){w->read_slot=slot;uint32_t at=p->page*CX_PAGE_SIZE,n=w->store_size-at;if(n>CX_PAGE_SIZE)n=CX_PAGE_SIZE;return cx_emit(w,TCW_WRITE,w->scratch_source,at,n,w->cache+slot*CX_PAGE_SIZE);}
    if(p->valid)w->cache_map[p->page]=0;
    p->page=page;p->valid=1;p->dirty=0;p->stamp=++w->cache_clock;w->cache_map[page]=slot+1u;
    *out=slot;
    if(w->stored_pages[page>>3]&(1u<<(page&7u))){
        w->read_target=1;w->read_slot=slot;uint32_t at=page*CX_PAGE_SIZE,n=w->store_size-at;if(n>CX_PAGE_SIZE)n=CX_PAGE_SIZE;
        return cx_emit(w,TCW_READ,w->scratch_source,at,n,NULL);
    }
    memset(w->cache+slot*CX_PAGE_SIZE,0,CX_PAGE_SIZE);return TCW_OK;
}
int cx_store(CxWorld* w,uint32_t offset,uint8_t* bytes,uint32_t length,int write){
    if(offset>w->store_size||length>w->store_size-offset)return cx_fail(w,TCW_IO,"invalid compiled circuit scratch range");
    if(!length)return TCW_OK;
    if((offset&(CX_PAGE_SIZE-1u))+length>CX_PAGE_SIZE)return cx_fail(w,TCW_IO,"circuit scratch operation crosses an internal cache page");
    uint32_t slot;int status=cache_page(w,offset>>CX_PAGE_SHIFT,&slot);if(status)return status;
    uint8_t* ptr=w->cache+slot*CX_PAGE_SIZE+(offset&(CX_PAGE_SIZE-1u));
    if(write){memcpy(ptr,bytes,length);w->cache_pages[slot].dirty=1;}else memcpy(bytes,ptr,length);
    return TCW_OK;
}
int cx_cache_flush(CxWorld* w){
    while(w->flush_index<w->cache_count){uint32_t slot=w->flush_index++;CxCachePage* p=w->cache_pages+slot;if(p->valid&&p->dirty){w->read_slot=slot;uint32_t at=p->page*CX_PAGE_SIZE,n=w->store_size-at;if(n>CX_PAGE_SIZE)n=CX_PAGE_SIZE;return cx_emit(w,TCW_WRITE,w->scratch_source,at,n,w->cache+slot*CX_PAGE_SIZE);}}
    return TCW_OK;
}
static void destroy(CxWorld* w){
    if(!w)return;terra_vm_destroy(w->vm);w->vm=NULL;terra_twld_destroy(w->twld);w->twld=NULL;
    cx_words_free(w,&w->parents);cx_words_free(w,&w->code_ends);cx_words_free(w,&w->member_ends);cx_words_free(w,&w->previous_gate);
    cx_bytes_free(w,&w->map);cx_bytes_free(w,&w->checkpoints);cx_bytes_free(w,&w->code);cx_bytes_free(w,&w->members);
#define RELEASE(field) cx_free(w,w->field)
    RELEASE(input);RELEASE(column);RELEASE(front);RELEASE(ids);RELEASE(seed_ids);RELEASE(opposite_ids);RELEASE(gate_at_y);RELEASE(pixel_at_y);RELEASE(checkpoint_index);RELEASE(column_gates);
    RELEASE(action_bits);RELEASE(action_rank);RELEASE(actions);RELEASE(cache);RELEASE(cache_pages);RELEASE(cache_map);RELEASE(stored_pages);
    RELEASE(intern_buffer);RELEASE(hashes);RELEASE(general);RELEASE(lamps);RELEASE(devices);RELEASE(pixels);RELEASE(overrides);RELEASE(bindings);
    RELEASE(points);RELEASE(result);RELEASE(output);RELEASE(general_snapshot);RELEASE(mods);RELEASE(ports);RELEASE(pixel_touched);RELEASE(pixel_snapshot);
    RELEASE(mechs);RELEASE(mechs_snapshot);RELEASE(trigger_nets);RELEASE(override_snapshot);
    RELEASE(policies);cx_free(w,w->previous_columns[0]);cx_free(w,w->previous_columns[1]);
#undef RELEASE
    tx_persistent_free(w);
}
uint32_t terra_circuit_world_abi_version(void){return TERRA_CIRCUIT_WORLD_ABI;}
int32_t terra_circuit_world_begin(uint32_t world_handle,uint32_t scratch_source_id,uint32_t twld_source_id,uint32_t twld_size,uint32_t max_bytes,uint32_t* out_handle){
    if(!out_handle)return TCW_INVALID;*out_handle=0;
    TxWorld* world=tx_get_world(world_handle);
    if(!world||!scratch_source_id||scratch_source_id==world->stream_source_id||scratch_source_id==twld_source_id||(!twld_source_id&&twld_size))return TCW_INVALID;
    if(world->legacy_wld)return cx_fail(NULL,TCW_UNSUPPORTED,"circuit simulation requires a sectioned WLD file");
    if(tx_world_is_future(world))return cx_fail(NULL,TCW_UNSUPPORTED,"future-layout worlds are read-only and cannot open a mutable circuit session");
    if(world->maxTilesX<1||world->maxTilesY<1)return TCW_FORMAT;
    uint32_t slot=0;while(slot<4&&sessions[slot])++slot;if(slot==4||!next_session)return TCW_STATE;
    CxWorld* w=(CxWorld*)tx_persistent_alloc(sizeof(*w));if(!w)return TCW_MEMORY;memset(w,0,sizeof(*w));
    w->maximum=max_bytes?max_bytes:UINT32_MAX;w->bytes=w->peak=sizeof(*w)+32u;
    w->id=next_session++;w->world_handle=world_handle;w->world=world;w->scratch_source=scratch_source_id;w->twld_source=twld_source_id;w->twld_size=twld_size;
    w->world_source=world->stream_source_id;w->world_size=w->world_source?world->stream_source_size:world->file_len;
    w->tile_start=w->world_source?world->stream_tile_start:world->starts[1];w->tile_end=w->world_source?world->stream_tile_end:world->ends[1];
    w->width=(uint32_t)world->maxTilesX;w->height=(uint32_t)world->maxTilesY;w->min_x=w->width;w->min_y=w->height;
    if((uint64_t)w->height*sizeof(TxTile)>UINT32_MAX||(uint64_t)w->height*16u>UINT32_MAX||((uint64_t)w->width+1u)*4u>UINT32_MAX){destroy(w);return TCW_MEMORY;}
    w->input=(uint8_t*)cx_alloc(w,TERRA_CIRCUIT_WORLD_WINDOW);
    w->column=(TxTile*)cx_alloc(w,w->height*sizeof(TxTile));w->front=(uint32_t*)cx_alloc(w,w->height*16u);w->ids=(uint32_t*)cx_alloc(w,w->height*16u);w->seed_ids=(uint32_t*)cx_alloc(w,w->height*16u);w->opposite_ids=(uint32_t*)cx_alloc(w,w->height*16u);
    w->gate_at_y=(uint32_t*)cx_alloc(w,w->height*4u);w->pixel_at_y=(uint32_t*)cx_alloc(w,w->height*4u);w->column_gates=(uint32_t*)cx_alloc(w,(w->width+1u)*4u);
    w->checkpoint_count=(w->width+CX_CHECK_COLUMNS-1u)/CX_CHECK_COLUMNS;
    w->checkpoint_index=(CxCheckpoint*)cx_alloc(w,w->checkpoint_count*sizeof(CxCheckpoint));
    if(!w->input||!w->column||!w->front||!w->ids||!w->seed_ids||!w->opposite_ids||!w->gate_at_y||!w->pixel_at_y||!w->column_gates||!w->checkpoint_index||!cx_compile_initialize(w)){int status=w->error?-(int)w->error:TCW_MEMORY;destroy(w);return status;}
    memset(w->checkpoint_index,0,w->checkpoint_count*sizeof(CxCheckpoint));
    if(twld_source_id){int status=cx_twld_begin(w);if(status<0){destroy(w);return status;}}
    sessions[slot]=w;*out_handle=w->id;return TCW_OK;
}
int32_t terra_circuit_world_supply(uint32_t handle,uint32_t source_id,uint32_t offset,const uint8_t* data,uint32_t length){
    CxWorld* w=cx_lookup(handle);if(!w)return TCW_HANDLE;
    if(w->event.kind!=TCW_READ||source_id!=w->event.source_id||offset!=w->event.offset||length!=w->event.length||(!data&&length))return TCW_INVALID;
#ifdef __wasm__
    if(!tx_bridge_range_is_valid((uint32_t)(uintptr_t)data,length))return TCW_INVALID;
#endif
    if(w->read_target==1){memcpy(w->cache+w->read_slot*CX_PAGE_SIZE,data,length);}
    else{memcpy(w->input,data,length);w->input_offset=offset;w->input_length=length;}
    memset(&w->event,0,sizeof(w->event));return TCW_OK;
}
int32_t terra_circuit_world_ack(uint32_t handle){
    CxWorld* w=cx_lookup(handle);if(!w)return TCW_HANDLE;
    if(w->event.kind!=TCW_WRITE&&w->event.kind!=TCW_RESULT)return TCW_STATE;
    if(w->event.kind==TCW_WRITE){
        if(w->phase==CX_ZERO)w->output_offset+=w->event.length;
        else if(w->event.source_id==w->scratch_source){CxCachePage* p=w->cache_pages+w->read_slot;p->dirty=0;w->stored_pages[p->page>>3]|=(uint8_t)(1u<<(p->page&7u));}
        else if(w->phase==CX_SAVE_PATCH){int s=cx_twld_save_begin(w);if(s<0)return s;if(!w->twld)w->phase=CX_IDLE;}
        else if(w->phase==CX_TWLD&&w->twld_mode==2u){w->twld_output_offset+=w->event.length;w->output_length=0;}
        else{w->output_offset+=w->event.length;w->output_length=0;}
    }else{w->result_count=0;w->phase=CX_IDLE;}
    memset(&w->event,0,sizeof(w->event));return TCW_OK;
}
int32_t terra_circuit_world_step(uint32_t handle,uint32_t work_units,TerraCircuitWorldEvent* out){
    CxWorld* w=cx_lookup(handle);if(!w)return TCW_HANDLE;
    if(!out||!work_units)return TCW_INVALID;if(!tx_get_world(w->world_handle))return TCW_HANDLE;
    if(w->phase==CX_FAILED)return -(int)(w->error?w->error:-TCW_STATE);
    if(w->phase==CX_CANCELLED)return TCW_STATE;
    if(w->event.kind){*out=w->event;return TCW_CONTINUE;}
    uint32_t work=work_units;
    while(work&&w->phase!=CX_IDLE){
        int status=TCW_OK;
        if(w->phase<=CX_INTERN)status=cx_compile_step(w,&work);
        else if(w->phase==CX_QUERY)status=cx_query_step(w,&work);
        else if(w->phase==CX_RUN){
            uint32_t n=work;work=0;status=terra_vm_step(w->vm,n);
            if(status<0)status=cx_fail(w,status==TERRA_VM_OOM||status==TERRA_VM_LIMIT?TCW_MEMORY:TCW_STATE,"native circuit execution failed and was rolled back");
            else if(status==TERRA_VM_OK){
                if(w->trigger_remaining>1u){--w->trigger_remaining;status=terra_vm_begin(w->vm,w->trigger_nets,w->trigger_count);if(status<0)status=cx_fail(w,TCW_MEMORY,"native circuit pulse allocation failed");}
                else status=cx_command_complete(w);
            }else status=TCW_CONTINUE;
        }else if(w->phase>=CX_SAVE_PREFIX&&w->phase<=CX_SAVE_PATCH)status=cx_save_step(w,&work);
        else if(w->phase==CX_TWLD)status=cx_twld_step(w,&work);
        else if(w->phase==CX_TICKS)status=cx_ticks_step(w,&work);
        else status=cx_fail(w,TCW_STATE,"unknown circuit session phase");
        if(status<0){if(w->vm){cx_twld_cancel_save(w);cx_operation_rollback(w);w->phase=CX_IDLE;memset(&w->event,0,sizeof(w->event));}else w->phase=CX_FAILED;return status;}
        if(w->event.kind){*out=w->event;return TCW_CONTINUE;}
        if(status==TCW_CONTINUE&&!work)break;
    }
    memset(out,0,sizeof(*out));out->abi_version=1;out->kind=w->phase==CX_IDLE?TCW_READY:TCW_MORE;out->phase=w->phase;out->completed=w->x;out->total=w->width;
    if(w->phase==CX_IDLE){out->result_kind=w->command.kind;out->reserved=w->twld_state&1u;if(w->command.kind==TCW_SAVE){out->source_id=w->output_source;out->result_count=w->save_result_size;out->reserved=w->twld_saved_size;}}
    return w->phase==CX_IDLE?TCW_OK:TCW_CONTINUE;
}
int32_t terra_circuit_world_stats(uint32_t handle,TerraCircuitWorldStats* out){
    CxWorld* w=cx_lookup(handle);if(!w)return TCW_HANDLE;if(!out)return TCW_INVALID;TerraVmStats s;memset(&s,0,sizeof(s));if(w->vm)terra_vm_stats(w->vm,&s);
    uint32_t bytes=w->bytes+vm_bytes(w),peak=w->peak;if(bytes>peak)w->peak=peak=bytes;
    *out=(TerraCircuitWorldStats){1,w->phase,w->width,w->height,(uint32_t)w->world->spawnTileX,(uint32_t)w->world->spawnTileY,
        w->min_x,w->min_y,w->max_x,w->max_y,w->wire_cells,w->devices_count,w->gates,w->networks,w->vm?w->width:w->x,w->phase,bytes,peak,
        (uint32_t)w->ticks,(uint32_t)(w->ticks>>32),(uint32_t)s.net_pulses,(uint32_t)(s.net_pulses>>32),(uint32_t)s.gates_fired,(uint32_t)(s.gates_fired>>32)};return TCW_OK;
}
int32_t terra_circuit_world_cancel(uint32_t handle){CxWorld* w=cx_lookup(handle);if(!w)return TCW_HANDLE;if(w->vm){cx_twld_cancel_save(w);cx_operation_rollback(w);w->phase=CX_IDLE;}else w->phase=CX_CANCELLED;memset(&w->event,0,sizeof(w->event));w->tick_remaining=w->tick_stage=w->trigger_remaining=0;return TCW_OK;}
int32_t terra_circuit_world_close(uint32_t handle){CxWorld* w=cx_lookup(handle);if(!w)return TCW_HANDLE;for(uint32_t i=0;i<4;i++)if(sessions[i]==w){sessions[i]=NULL;break;}destroy(w);return TCW_OK;}
