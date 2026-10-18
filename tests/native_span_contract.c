#include "terra_native_span.h"
#include "terra_stream.h"
#include "terra_circuit_world.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
_Static_assert(sizeof(TxStreamEvent)==48,"stream wire event size");
_Static_assert(offsetof(TxStreamEvent,data_ptr)==20,"stream wire offset position");
_Static_assert(sizeof(TxStreamInputLease)==32,"stream wire lease size");
_Static_assert(offsetof(TxStreamInputLease,data_ptr)==20,"lease wire offset position");
_Static_assert(sizeof(TerraCircuitWorldEvent)==48,"circuit wire event size");
_Static_assert(sizeof(TerraCircuitWorldCommand)==64,"circuit wire command size");
_Static_assert(offsetof(TerraCircuitWorldCommand,data_ptr)==36,"circuit wire offset position");
extern uint32_t tx_bridge_heap_used(void);
extern uint32_t tx_malloc(uint32_t);
extern void tx_free(uint32_t);
int main(void) {
    uint32_t baseline=tx_bridge_heap_used();
    uint8_t* p=tx_bridge_alloc_native(128);assert(p);memset(p,0xa5,128);
    assert(tx_bridge_heap_used()==baseline+128&&tx_bridge_allocation_size_native(p)==128);
    assert(tx_bridge_contains_native(p,128)&&tx_bridge_contains_native(p+127,1));
    assert(tx_bridge_contains_native(p+128,0)&&!tx_bridge_contains_native(p+128,1));
    assert(!tx_bridge_contains_native(p,129)&&!tx_bridge_contains_native(p,SIZE_MAX));
    assert(!tx_bridge_contains_native((void*)((uintptr_t)p-1),1));
    tx_bridge_free_native(p+1);assert(tx_bridge_heap_used()==baseline+128);
    uint32_t offset=99;
    if((uintptr_t)p>UINT32_MAX){assert(!tx_wasm_offset32(p,&offset)&&offset==0);puts("native bridge exercised a pointer above UINT32_MAX");}
    else assert(tx_wasm_offset32(p,&offset)&&offset==(uintptr_t)p);
    assert(tx_wasm_offset32(NULL,&offset)&&offset==0);
    assert(!tx_wasm_span32(NULL,1,&offset)&&offset==0);
    assert(tx_wasm_span32((void*)(uintptr_t)UINT32_MAX,1,&offset)&&offset==UINT32_MAX);
    assert(!tx_wasm_span32((void*)(uintptr_t)UINT32_MAX,2,&offset)&&offset==0);
#if UINTPTR_MAX > UINT32_MAX
    offset=99;assert(!tx_wasm_offset32((void*)((uintptr_t)UINT32_MAX+1u),&offset)&&offset==0);
    assert(!tx_bridge_alloc_native((size_t)UINT32_MAX+1u));
#endif
    tx_bridge_free_native(p);tx_bridge_free_native(p);assert(tx_bridge_heap_used()==baseline);
    /* The old wire allocator either returns a representable pointer or rejects
     * without leaking the newly allocated high-address native bridge root. */
    offset=tx_malloc(64);if(offset)tx_free(offset);assert(tx_bridge_heap_used()==baseline);
    return 0;
}
