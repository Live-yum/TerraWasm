/* Reserved sidecar ABI slots reject before allocation, ownership or source IO. */
#include "terra_world.h"
#include "terra_types.h"
#include "terra_circuit_world.h"
#include "terra_owner_guards.h"
#include "terra_abi.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
extern TxWorld* tx_get_world(uint32_t);
extern uint32_t tx_native_heap_used(void);
extern void txw_test_fail_after(uint32_t);
extern char tx_last_error[256];
static void rejected(uint32_t world,uint32_t source,uint32_t size){
    uint32_t handle=99,baseline=tx_native_heap_used();
    assert(terra_circuit_world_begin(world,77,source,size,8u*1024u*1024u,&handle)==TCW_UNSUPPORTED);
    assert(handle==0&&tx_native_heap_used()==baseline);
    assert(!tx_circuit_world_has_sessions()&&!tx_circuit_world_owns_world(world));
    assert(strstr(tx_last_error,"TWLD")&&strstr(tx_last_error,"not supported"));
}
int main(void){
    uint32_t initial=tx_native_heap_used(),world=0,handle=0;
    assert(strstr(terra_build_info_json(),"\"circuitWorldStreamingTwld\":false"));
    assert(terra_world_create(&world)==0);
    TxWorld* w=tx_get_world(world);w->maxTilesX=8;w->maxTilesY=8;w->version=w->original_version=269;
    TxWorld before=*w;
    for(uint32_t n=0;n<2;n++){
        if(n)txw_test_fail_after(0);
        rejected(world,3,0);rejected(world,0,1);rejected(world,3,UINT32_MAX);
        rejected(0,77,1); /* refusal also precedes world/source validation */
        assert(!memcmp(w,&before,sizeof(before)));
        txw_test_fail_after(UINT32_MAX);
    }
    /* Refused opens consumed no slots or source ownership; vanilla still opens. */
    assert(terra_circuit_world_begin(world,77,0,0,8u*1024u*1024u,&handle)==TCW_OK&&handle);
    assert(tx_circuit_world_owns_world(world));
    assert(terra_circuit_world_close(handle)==TCW_OK);
    assert(terra_world_close(world)==0);
    assert(tx_native_heap_used()==initial&&!tx_circuit_world_has_sessions());
    return 0;
}
