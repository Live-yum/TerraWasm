/* Exercise the production insertion/lookup implementation, including rehash. */
#include "../src/terra_circuit_query.c"
#include <assert.h>
#include <stdio.h>
int main(void) {
    CxWorld w={0};w.maximum=64u*1024u*1024u;
    TxTile column[1]={{0}}; uint32_t ids[4]={0};
    w.column=column;w.seed_ids=ids;w.opposite_ids=ids;
    for(uint32_t i=0;i<65536;i++) {
        w.x=i*17u;
        CxBinding* b=bind_point(&w,0);assert(b&&b->x==w.x);
    }
    assert(w.binding_count==65536);
    for(uint32_t i=0;i<65536;i++) {
        CxBinding* b=cx_find_binding(&w,i*17u,0);
        assert(b&&b->x==i*17u&&b->y==0);
        assert(!cx_find_binding(&w,i*17u,1));
    }
    uint64_t probes=0;
    for(uint32_t i=0;i<65536;i++) {
        uint32_t slot=binding_hash(i*17u,0)&(w.binding_index_capacity-1u);
        while(w.binding_index[slot]) {
            ++probes;
            if(w.bindings[w.binding_index[slot]-1u].x==i*17u)break;
            slot=(slot+1u)&(w.binding_index_capacity-1u);
        }
    }
    assert(probes<65536u*8u);
    uint32_t retained=w.bytes;w.maximum=w.bytes;w.x=65536u*17u;
    assert(!bind_point(&w,0));assert(w.bytes==retained&&w.binding_count==65536);
    assert(cx_find_binding(&w,17u,0));w.maximum=64u*1024u*1024u;
    /* Existing keys must not allocate or add another record. */
    for(uint32_t i=0;i<65536;i++){w.x=i*17u;assert(bind_point(&w,0));}
    assert(w.binding_count==65536);
    cx_free(&w,w.bindings);cx_free(&w,w.binding_index);
    assert(w.bytes==0);
    printf("binding index lookup probes: %llu\n",(unsigned long long)probes);
    puts("binding index: 65536 inserts, hits, misses and duplicate inserts passed");
    return 0;
}
