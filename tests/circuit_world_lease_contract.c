#include "terra_world.h"
#include "terra_types.h"
#include "terra_circuit_world.h"
#include "terra_owner_guards.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
extern TxWorld* tx_get_world(uint32_t);
extern int txw_begin_pixel_art_indexed(uint32_t,int32_t,int32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,int32_t,int32_t,uint32_t,uint32_t,uint32_t);
extern int txw_add_pixel_art_chunks_bulk_fast(uint32_t,uint32_t,uint32_t,uint32_t);
extern char tx_last_error[256];
extern uint32_t tx_native_heap_used(void);
extern int tx_stream_activate_world(TxWorld*,uint32_t*);
int main(void) {
    uint32_t world=0, first=0, second=0, size=99, token=99, out=99;
    assert(terra_world_create(&world)==0);
    TxWorld* w=tx_get_world(world);
    w->maxTilesX=8;w->maxTilesY=8;w->version=w->original_version=269;
    assert(terra_circuit_world_begin(world,77,0,0,8u*1024u*1024u,&first)==0);
    assert(terra_circuit_world_begin(world,78,0,0,8u*1024u*1024u,&second)==0);
    uint32_t baseline=tx_native_heap_used();
    TxWorld snapshot=*w, candidate;memset(&candidate,0,sizeof(candidate));
    assert(tx_circuit_world_owns_world(world)&&tx_circuit_world_has_sessions());
    assert(terra_world_close(world)==TERRAX_WORLD_STATUS_STATE_ERROR);
    assert(terra_world_workspace_checkpoint_size(world,&size)==TERRAX_WORLD_STATUS_STATE_ERROR&&size==0);
    assert(terra_world_workspace_begin(world,UINT32_MAX,&token)==TERRAX_WORLD_STATUS_STATE_ERROR&&token==0);
    assert(terra_section_set_json(world,"header","{}")==TERRAX_WORLD_STATUS_STATE_ERROR);
    assert(terra_world_save_to_buffer(world,NULL,0,&out)==TERRAX_WORLD_STATUS_STATE_ERROR&&out==0);
    assert(txw_begin_pixel_art_indexed(world,0,0,8,8,0,0,0,0,0,0,0,0,0)==-1);
    assert(strstr(tx_last_error,"TERRAX_STATE_ERROR")&&strstr(tx_last_error,"circuit"));
    assert(txw_add_pixel_art_chunks_bulk_fast(world,0,0,0)==-1);
    assert(strstr(tx_last_error,"TERRAX_STATE_ERROR")&&strstr(tx_last_error,"circuit"));
    uint64_t response_size=99;
    assert(terra_op_execute_json(world,"header_patch","{}",NULL,0,&response_size)==TERRAX_WORLD_STATUS_STATE_ERROR);
    assert(response_size==0);
    out=99;assert(!tx_stream_activate_world(&candidate,&out)&&out==0);
    assert(tx_get_world(world)==w&&!memcmp(w,&snapshot,sizeof(snapshot)));
    assert(tx_native_heap_used()==baseline);
    TerraCircuitWorldStats stats;assert(terra_circuit_world_stats(first,&stats)==0);
    assert(terra_circuit_world_cancel(first)==0);
    assert(terra_world_close(world)==TERRAX_WORLD_STATUS_STATE_ERROR);
    assert(terra_circuit_world_close(first)==0);
    assert(terra_world_close(world)==TERRAX_WORLD_STATUS_STATE_ERROR);
    assert(terra_circuit_world_close(second)==0);
    assert(!tx_circuit_world_has_sessions());
    assert(terra_world_workspace_begin(world,UINT32_MAX,&token)==0);
    baseline=tx_native_heap_used();first=99;
    assert(terra_circuit_world_begin(world,77,0,0,8u*1024u*1024u,&first)==-3&&first==0);
    assert(tx_native_heap_used()==baseline);
    assert(terra_world_workspace_rollback(world,token)==0);
    assert(terra_circuit_world_begin(world,77,0,0,8u*1024u*1024u,&first)==0);
    assert(terra_circuit_world_close(first)==0);
    assert(terra_world_close(world)==0);
    assert(tx_native_heap_used()==0);
    return 0;
}
