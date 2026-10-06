#include "terra_circuit_world_internal.h"
#include <limits.h>
#include <string.h>

static int32_t collect_tile(void* context,const TerraTwldTile* tile){
    CxWorld* w=(CxWorld*)context;if(w->twld_mode!=1u||!tile->color_pixel_box)return 0;
    if(w->mod_count==w->mod_capacity){uint32_t n=w->mod_capacity?w->mod_capacity*2u:256u;if(n<w->mod_capacity||(uint64_t)n*sizeof(CxModTile)>UINT32_MAX)return -1;CxModTile* p=(CxModTile*)cx_alloc(w,n*sizeof(CxModTile));if(!p)return -1;if(w->mod_count)memcpy(p,w->mods,w->mod_count*sizeof(CxModTile));cx_free(w,w->mods);w->mods=p;w->mod_capacity=n;}
    CxModTile* m=w->mods+w->mod_count++;memset(m,0,sizeof(*m));m->tile=*tile;w->twld_state|=1u;return 0;
}
static int32_t patch_tile(void* context,TerraTwldTile* tile){
    CxWorld* w=(CxWorld*)context;if(!tile->color_pixel_box)return 0;uint32_t p=cx_pixel_find(w,tile->x,tile->y);if(p==CX_NONE)return 0;uint8_t state=w->pixels[p].state;tile->frame_x=(int16_t)(18*(state&3u));tile->frame_y=(int16_t)(18*((state>>2)&3u));return 0;
}
static int32_t output_bytes(void* context,const uint8_t* bytes,uint32_t count){
    CxWorld* w=(CxWorld*)context;if(count>w->output_capacity-w->output_length)return -1;memcpy(w->output+w->output_length,bytes,count);w->output_length+=count;return 0;
}
int cx_twld_begin(CxWorld* w){
    if(!w->twld_source)return TCW_OK;
    TerraTwldCallbacks cb;memset(&cb,0,sizeof(cb));cb.tile=collect_tile;cb.patch=patch_tile;cb.output=output_bytes;
    uint32_t available=w->maximum>w->bytes?w->maximum-w->bytes:1u;
    int s=terra_twld_create(w->width,w->height,available,&cb,w,&w->twld);if(s<0)return cx_fail(w,TCW_MEMORY,"TWLD streaming decoder allocation failed");
    w->twld_mode=0;w->twld_cursor=0;w->phase=CX_TWLD;w->input_length=0;return TCW_OK;
}
int cx_twld_save_begin(CxWorld* w){
    if(!w->twld)return TCW_OK;if(!w->command.aux_source_id)return cx_fail(w,TCW_INVALID,"saving a WLD with mod pixels requires a TWLD output source");
    TerraVmStats vm;memset(&vm,0,sizeof(vm));if(w->vm)terra_vm_stats(w->vm,&vm);uint64_t used=(uint64_t)w->bytes+vm.allocated_bytes;
    if(used>=w->maximum||terra_twld_set_budget(w->twld,w->maximum-(uint32_t)used)<0)return cx_fail(w,TCW_MEMORY,"TWLD output does not fit the remaining session memory budget");
    int s=terra_twld_rewind(w->twld,1);if(s<0)return cx_fail(w,s==TERRA_TWLD_OOM||s==TERRA_TWLD_LIMIT?TCW_MEMORY:TCW_FORMAT,"TWLD cannot begin a lossless output replay");
    w->twld_mode=2;w->twld_cursor=0;w->twld_output_offset=0;w->output_length=0;w->phase=CX_TWLD;w->input_length=0;return TCW_OK;
}
void cx_twld_cancel_save(CxWorld* w){
    if(w->twld&&w->phase==CX_TWLD&&w->twld_mode==2u){terra_twld_abort_replay(w->twld);w->twld_cursor=0;w->twld_output_offset=0;w->twld_saved_size=0;w->output_length=0;w->input_length=0;}
}
int cx_twld_step(CxWorld* w,uint32_t* work){
    if(w->twld_mode==2u&&w->output_length)return cx_emit(w,TCW_WRITE,w->command.aux_source_id,w->twld_output_offset,w->output_length,w->output);
    while(*work&&w->twld_cursor<w->twld_size){
        /* 4 KiB compressed input also bounds the worst-case inflate work and the
         * fixed-size frame rewrite's pending gzip output. */
        uint32_t n=w->twld_size-w->twld_cursor;if(n>4096u)n=4096u;
        if(w->input_offset!=w->twld_cursor||w->input_length!=n){w->read_target=2;return cx_emit(w,TCW_READ,w->twld_source,w->twld_cursor,n,NULL);}
        uint32_t final=w->twld_cursor+n==w->twld_size;int s=terra_twld_feed(w->twld,w->input,n,final);w->input_length=0;--*work;
        if(s<0)return cx_fail(w,s==TERRA_TWLD_OOM||s==TERRA_TWLD_LIMIT?TCW_MEMORY:TCW_FORMAT,"TWLD gzip/NBT replay failed validation");w->twld_cursor+=n;
        if(w->twld_mode==2u&&w->output_length)return cx_emit(w,TCW_WRITE,w->command.aux_source_id,w->twld_output_offset,w->output_length,w->output);
    }
    if(w->twld_cursor<w->twld_size)return TCW_CONTINUE;
    if(w->twld_mode==0u){int s=terra_twld_rewind(w->twld,0);if(s<0)return cx_fail(w,TCW_FORMAT,"TWLD metadata replay cannot be initialized");w->twld_mode=1;w->twld_cursor=0;w->input_length=0;return TCW_OK;}
    if(w->twld_mode==1u){w->phase=CX_TOPOLOGY;cx_scan_reset(w);return TCW_OK;}
    w->twld_saved_size=w->twld_output_offset;w->phase=CX_IDLE;return TCW_OK;
}
int cx_mod_column(CxWorld* w){
    if(!w->mod_count)return TCW_OK;
    uint32_t lo=0,hi=w->mod_count;while(lo<hi){uint32_t m=lo+(hi-lo)/2;if(w->mods[m].tile.x<w->x)lo=m+1;else hi=m;}
    while(lo<w->mod_count&&w->mods[lo].tile.x==w->x){CxModTile* m=w->mods+lo++;uint32_t y=m->tile.y;if(y>=w->height)return TCW_FORMAT;
        if(w->phase==CX_TOPOLOGY){m->vanilla=w->column[y];if(!m->vanilla.active)++w->devices_count;}
        w->column[y].active=1;w->column[y].type=0xfffeu;w->column[y].frame_x=m->tile.frame_x;w->column[y].frame_y=m->tile.frame_y;w->column[y].tile_color=m->tile.paint;
    }return TCW_OK;
}
int cx_mod_vanilla(CxWorld* w,uint32_t x,uint32_t y,TxTile* out){
    uint32_t lo=0,hi=w->mod_count;while(lo<hi){uint32_t m=lo+(hi-lo)/2;TerraTwldTile* t=&w->mods[m].tile;if(t->x<x||(t->x==x&&t->y<y))lo=m+1;else hi=m;}
    if(lo==w->mod_count||w->mods[lo].tile.x!=x||w->mods[lo].tile.y!=y)return 0;*out=w->mods[lo].vanilla;return 1;
}
