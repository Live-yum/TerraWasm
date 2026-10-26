#include "terra_checkpoint.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

extern uint8_t* tx_alloc(uint32_t);
extern uint8_t* tx_persistent_alloc(uint32_t);
extern void* tx_internal_realloc(void*, uint32_t);
extern void tx_internal_free(void*);
extern uint32_t tx_native_heap_used(void);
extern void txw_test_allocation_limit(uint32_t);

static void readonly_root_contract(int persistent) {
    const uint32_t size = 4u * 1024u * 1024u;
    uint8_t* baseline = persistent ? tx_persistent_alloc(size) : tx_alloc(size);
    uint8_t* edit = tx_alloc(32);
    assert(baseline && edit);
    memset(baseline, 43, size);
    memset(edit, 11, 32);
    void* roots[] = {persistent ? baseline : NULL};
    uint32_t full = tx_checkpoint_bytes(roots, 1);
    uint32_t reduced = tx_checkpoint_bytes_readonly(roots, 1, baseline);
    assert(full - reduced == size && reduced < 1024);
    uint32_t live = tx_native_heap_used();
    assert(!tx_checkpoint_begin_readonly(roots, 1, reduced - 1, baseline));
    assert(tx_checkpoint_begin_readonly(roots, 1, reduced, baseline));
    assert(tx_native_heap_used() == live + reduced);
    edit[0] = 99;
    /* Even if an operation replaces the read-only root, the old allocation
     * remains pinned for rollback. The original bytes are never overwritten. */
    uint8_t* replacement = tx_internal_realloc(baseline, size + 16);
    assert(replacement && replacement != baseline && replacement[0] == 43);
    replacement[0] = 71;
    txw_test_allocation_limit(0);
    assert(tx_checkpoint_finish(1));
    txw_test_allocation_limit(UINT32_MAX);
    assert(tx_native_heap_used() == live);
    assert(baseline[0] == 43 && baseline[size - 1] == 43 && edit[0] == 11);
    assert(tx_checkpoint_begin_readonly(roots, 1, reduced, baseline));
    tx_internal_free(baseline);
    txw_test_allocation_limit(0);
    assert(tx_checkpoint_finish(1));
    txw_test_allocation_limit(UINT32_MAX);
    assert(tx_native_heap_used() == live && baseline[0] == 43);
    assert(tx_checkpoint_begin_readonly(roots, 1, reduced, baseline));
    tx_internal_free(baseline);
    assert(tx_checkpoint_finish(0));
    assert(tx_native_heap_used() == 32);
    tx_internal_free(edit);
    assert(tx_native_heap_used() == 0);
}

int main(void) {
    readonly_root_contract(0);
    readonly_root_contract(1);
    puts("read-only checkpoint: 4 MiB byte-copy removed, pin/realloc/deferred-free/zero-allocation rollback passed");
    return 0;
}
