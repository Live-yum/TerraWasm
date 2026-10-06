#include "terra_world.h"
#include "terra_types.h"
#include "terra_checkpoint.h"
#include "terra_stream.h"
#include "terra_pixel_workspace.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdlib.h>
extern TxWorld* tx_get_world(uint32_t);
extern uint8_t* tx_alloc(uint32_t);
extern uint8_t* tx_persistent_alloc(uint32_t);
extern void* tx_internal_realloc(void*,uint32_t);
extern void tx_internal_free(void*);
extern uint32_t tx_mark(void);
extern uint32_t tx_native_heap_used(void);
extern void txw_test_allocation_limit(uint32_t);

static uint32_t independent_pixel = 0, independent_color = 0;
static void roundtrip(int streaming) {
    uint32_t handle=0, token=0, size=0, ignored=0;
    assert(terra_world_create(&handle)==0);
    TxWorld* w=tx_get_world(handle);
    w->file=streaming?tx_persistent_alloc(1024):tx_alloc(1024);
    assert(w->file); memset(w->file,7,1024);w->file_len=1024;
    if(streaming){w->stream_owned=1;w->stream_source_id=99;w->stream_columns=(uint32_t*)tx_persistent_alloc(12);memset(w->stream_columns,3,12);}
    w->section_overrides[0].data=tx_alloc(64);w->section_overrides[0].len=64;w->section_overrides[0].active=1;
    memset(w->section_overrides[0].data,11,64);
    w->pixel_art_pixels=tx_alloc(16);w->pixel_art_pixels_len=16;memset(w->pixel_art_pixels,19,16);
    strcpy(w->worldName,"already unsaved");w->revision=23;w->heap_mark=tx_mark();
    uint8_t *file=w->file,*override=w->section_overrides[0].data,*pixels=w->pixel_art_pixels;
    uint32_t baseline=tx_native_heap_used();
    for(unsigned i=0;i<50;i++){
        TerraPixelWorkspaceStats pixel_before, pixel_after;
        assert(terra_pixel_workspace_stats(independent_pixel,&pixel_before)==0);
        assert(terra_world_workspace_checkpoint_size(handle,&size)==0);
        assert(size>1104&&size<10000); /* independent persistent bitmap is excluded */
        assert(terra_world_workspace_begin(handle,size-1,&token)!=0&&token==0);
        assert(terra_world_workspace_begin(handle,size,&token)==0&&token);
        assert(terra_world_workspace_begin(handle,size,&ignored)!=0);
        assert(terra_world_workspace_rollback(handle,token+1)!=0);
        assert(terra_world_close(handle)==TERRAX_WORLD_STATUS_STATE_ERROR);
        assert(terra_world_open_begin(file,1024)==0);
        uint32_t task=0;assert(terra_world_stream_open_begin(1,1000,&task)<0&&!task);
        w->file[20]=55;w->revision++;strcpy(w->worldName,"candidate");
        tx_internal_free(override);w->section_overrides[0].data=tx_alloc(128);assert(w->section_overrides[0].data);
        w->pixel_art_pixels=tx_internal_realloc(pixels,128);assert(w->pixel_art_pixels);memset(w->pixel_art_pixels,1,128);
        void* extra=tx_persistent_alloc(4096);assert(extra);
        uint16_t cell=0;assert(terra_pixel_workspace_read_rect(independent_pixel,64,64,1,1,&cell,1)==0&&cell==independent_color);
        txw_test_allocation_limit(0); /* Rollback must not allocate, even one byte. */
        assert(terra_world_workspace_rollback(handle,token)==0);
        txw_test_allocation_limit(UINT32_MAX);
        assert(tx_get_world(handle)==w&&w->file==file&&w->file[20]==7);
        assert(w->section_overrides[0].data==override&&override[0]==11);
        assert(w->pixel_art_pixels==pixels&&pixels[0]==19);
        assert(w->revision==23&&!strcmp(w->worldName,"already unsaved"));
        assert(!streaming||(w->stream_source_id==99&&((uint8_t*)w->stream_columns)[0]==3));
        assert(tx_native_heap_used()==baseline);assert(!tx_checkpoint_active());
        assert(terra_pixel_workspace_stats(independent_pixel,&pixel_after)==0);
        assert(!memcmp(&pixel_before,&pixel_after,sizeof(pixel_before)));
        assert(terra_world_workspace_commit(handle,token)!=0);
    }
    txw_test_allocation_limit(0);
    assert(terra_world_workspace_begin(handle,size,&token)!=0&&token==0);
    assert(w->file==file&&file[20]==7&&w->revision==23);
    txw_test_allocation_limit(UINT32_MAX);
    assert(terra_world_workspace_begin(handle,size,&token)==0);
    w->file[20]=80;w->revision=24;
    tx_internal_free(override);w->section_overrides[0].data=tx_alloc(32);assert(w->section_overrides[0].data);
    w->heap_mark=tx_mark();txw_test_allocation_limit(0);
    assert(terra_world_workspace_commit(handle,token)==0);
    txw_test_allocation_limit(UINT32_MAX);
    assert(w->file[20]==80&&w->revision==24&&w->section_overrides[0].data!=override);
    assert(terra_world_close(handle)==0);
}
static void real_operations(const char* path) {
    uint32_t handle=0,size=0,token=0,required=0;uint64_t json_size=0;char response[2048];
    assert(terra_world_open(path,&handle)==0);
    assert(terra_op_execute_json(handle,"header_patch","{\"patch\":{\"worldName\":\"unsaved baseline\"}}",response,sizeof(response),&json_size)==0);
    TxWorld* world=tx_get_world(handle);assert(!strcmp(world->worldName,"unsaved baseline"));
    assert(terra_world_workspace_checkpoint_size(handle,&size)==0);
    assert(terra_world_workspace_begin(handle,size,&token)==0);
    assert(terra_op_execute_json(handle,"header_patch","{\"patch\":{\"worldName\":\"candidate\"}}",response,sizeof(response),&json_size)==0);
    txw_test_allocation_limit(0);
    assert(terra_op_execute_json(handle,"header_patch","{\"patch\":{\"worldName\":\"out of memory\"}}",response,sizeof(response),&json_size)!=0);
    assert(terra_world_workspace_rollback(handle,token)==0);
    txw_test_allocation_limit(UINT32_MAX);
    assert(tx_get_world(handle)==world&&!strcmp(world->worldName,"unsaved baseline"));
    assert(terra_world_workspace_checkpoint_size(handle,&size)==0);
    assert(terra_world_workspace_begin(handle,size,&token)==0);
    assert(terra_world_workspace_save_to_buffer(handle,NULL,0,&required)==0&&required);
    uint8_t* output=malloc(required);assert(output);
    assert(terra_world_workspace_save_to_buffer(handle,output,required,&required)==0);
    assert(tx_get_world(handle)==world&&!strcmp(world->worldName,"unsaved baseline"));
    assert(terra_world_workspace_commit(handle,token)==0);
    assert(terra_world_close(handle)==0);
    assert(terra_world_open_from_buffer(output,required,&handle)==0);
    assert(!strcmp(tx_get_world(handle)->worldName,"unsaved baseline"));
    assert(terra_world_close(handle)==0);free(output);
    puts("real WLD workspace checkpoint: prior unsaved header, native OOM rollback, non-destructive save/readback passed");
}
int main(int argc,char** argv){
    uint8_t* separate=tx_persistent_alloc(8*1024*1024);assert(separate);separate[0]=123;
    assert(terra_pixel_workspace_create(65,65,8*1024*1024,1024*1024,16,&independent_pixel)==0);
    assert(terra_pixel_workspace_palette_add(independent_pixel,0x010305,&independent_color)==0);
    TerraPixelCell entry={64,64,independent_color};
    assert(terra_pixel_workspace_tx_begin(independent_pixel)==0);
    assert(terra_pixel_workspace_cells(independent_pixel,&entry,1)==0);
    assert(terra_pixel_workspace_tx_commit(independent_pixel)==0);
    uint32_t baseline=tx_native_heap_used();
    roundtrip(0);assert(tx_native_heap_used()==baseline&&separate[0]==123);
    roundtrip(1);assert(tx_native_heap_used()==baseline&&separate[0]==123);
    assert(terra_pixel_workspace_undo(independent_pixel)==0);
    uint16_t cell=99;assert(terra_pixel_workspace_read_rect(independent_pixel,64,64,1,1,&cell,1)==0&&cell==0);
    assert(terra_pixel_workspace_redo(independent_pixel)==0);
    assert(terra_pixel_workspace_read_rect(independent_pixel,64,64,1,1,&cell,1)==0&&cell==independent_color);
    assert(terra_pixel_workspace_close(independent_pixel)==0);
    tx_internal_free(separate);assert(tx_native_heap_used()==0);
    if(argc>1){real_operations(argv[1]);assert(tx_native_heap_used()==0);}
    puts("world checkpoint contract: 100 allocation-free rollbacks, pinned realloc/free, OOM, isolation and commit passed");
}
