/* Candidate G: borrowed order tail never changes geometry ownership or grows
 * F's charged footprint. Run this same contract with cache use both on and off. */
#define main geometry_existing_contract_main
#include "terra_alignment.h"
#include "circuit_geometry_contract.c"
#undef main
static void* partition_alloc(CxWorld*,uint32_t);
#define cx_alloc partition_alloc
#include "../src/terra_circuit_geometry.c"
#undef cx_alloc
static uint32_t partition_attempt,partition_fail_at;
static void* partition_alloc(CxWorld* w,uint32_t n){
    if(++partition_attempt==partition_fail_at){cx_fail(w,TCW_MEMORY,"injected partition allocation failure");return NULL;}
    return cx_alloc(w,n);
}
static uint64_t optional_charge(uint32_t slots){
    return cx_allocation_charge(CX_GEOMETRY_COLD_BYTES)+
        cx_allocation_charge(CX_BLOCK_ENTRIES*sizeof(CxBlock))+
        cx_allocation_charge(slots*sizeof(uint32_t))+
        cx_allocation_charge(64u*sizeof(CxColumnEnd));
}
static void unchanged_bytes(const uint8_t* p,uint32_t n,uint8_t expected){
    for(uint32_t i=0;i<n;i++)assert(p[i]==expected);
}
static void install_test_block(Fixture* f,uint32_t x){
    CxOrderGeometry* g=f->owner.geometry;g->slot=0;g->target_column=x;g->point_row=0;g->source_end[0]=f->owner.tile_start+1u;
    for(uint32_t y=0;y<f->height;y++){uint16_t pair=(uint16_t)((y*73u+256u)&511u);g->cells[y]=(uint8_t)pair;g->compiler_axis[y]=(uint8_t)(pair>>8);}
    start_block(&f->owner,g);uint32_t steps=0;
    do{uint32_t work=1;assert(block_step(&f->owner,&work)>=0);assert(++steps<=131u);}while(g->block_phase);
}
static void tail_and_wrap(void){
    Fixture f;init(&f,4,128,0);assert(cx_geometry_create(&f.owner)==TCW_OK);CxOrderGeometry* g=f.owner.geometry;
    uint32_t size=0;uint8_t* tail=cx_geometry_order_cache_arena(&f.owner,&size);
    assert(tail&&size&&size<=CX_GEOMETRY_ORDER_CACHE_BYTES);
    assert((uintptr_t)tail%_Alignof(TerraMaxAlign)==0);
    assert(tail==g->cold_bytes+CX_ORDER_ARENA_OFFSET&&tail+size==g->cold_bytes+CX_GEOMETRY_COLD_BYTES);
    assert(CX_BLOCK_SLOTS*CX_BLOCK_BYTES<=CX_BLOCK_ARENA_BYTES&&CX_BLOCK_ARENA_BYTES<=CX_ORDER_ARENA_OFFSET);
    assert(CX_BLOCK_SLOTS==(CX_GEOMETRY_COLD_BYTES-CX_GEOMETRY_ORDER_CACHE_BYTES)/CX_BLOCK_BYTES);
    unchanged_bytes(tail,size,0);memset(tail,0xa7,size);
    uint32_t used=CX_BLOCK_SLOTS*CX_BLOCK_BYTES,gap=(uint32_t)CX_ORDER_ARENA_OFFSET-used;
    memset(g->cold_bytes+used,0x5c,gap);
    g->block_head=CX_BLOCK_SLOTS-1u;install_test_block(&f,0);
    CxBlock* last=block_find(g,0,0);assert(last&&last->slot==CX_BLOCK_SLOTS-1u&&g->block_head==0);
    assert(g->slot_owner[CX_BLOCK_SLOTS-1u]==(uint32_t)(last-g->blocks)+1u);
    for(uint32_t y=0;y<f.height;y++){uint8_t word,axis;uint16_t pair=(uint16_t)((y*73u+256u)&511u);assert(block_get(&f.owner,0,y,&word,&axis)==TCW_OK&&word==(uint8_t)pair&&axis==(pair>>8));}
    install_test_block(&f,1);assert(block_find(g,1,0)->slot==0&&g->block_head==1u);
    g->block_head=CX_BLOCK_SLOTS-1u;install_test_block(&f,2);
    assert(!block_find(g,0,0)&&block_find(g,2,0)->slot==CX_BLOCK_SLOTS-1u);
    /* Out-of-range metadata must fail before indexing the smaller owner array. */
    CxBlock* e=block_find(g,2,0);uint32_t slot=e->slot;e->slot=CX_BLOCK_SLOTS;
    assert(block_get(&f.owner,2,0,&(uint8_t){0},&(uint8_t){0})==TCW_FORMAT);e->slot=slot;
    unchanged_bytes(g->cold_bytes+used,gap,0x5c);unchanged_bytes(tail,size,0xa7);
    cx_geometry_cancel(&f.owner);assert(cx_geometry_order_cache_arena(&f.owner,&size)==tail);unchanged_bytes(tail,size,0xa7);
    Fixture b;init(&b,4,128,1);assert(cx_geometry_create(&b.owner)==TCW_OK);uint32_t other_size=0;uint8_t* other=cx_geometry_order_cache_arena(&b.owner,&other_size);
    assert(other&&other!=tail&&other_size==size);unchanged_bytes(other,other_size,0);other[0]=3;assert(tail[0]==0xa7);
    finish(&f);size=123;assert(cx_geometry_order_cache_arena(&f.owner,&size)==NULL&&size==0);assert(other[0]==3);finish(&b);
}
static void footprint_and_failures(void){
    Fixture f;init(&f,4,32,0);f.owner.maximum=1024u*1024u;
    assert(cx_geometry_create(&f.owner)==TCW_OK&&!f.owner.geometry->cold_bytes);uint32_t required=f.owner.bytes,size=123;
    assert(cx_geometry_order_cache_arena(&f.owner,&size)==NULL&&size==0);cx_geometry_destroy(&f.owner);
    f.owner.maximum=required;assert(cx_geometry_create(&f.owner)==TCW_OK&&f.owner.bytes==required);cx_geometry_destroy(&f.owner);
    f.owner.maximum=required-1u;assert(cx_geometry_create(&f.owner)==TCW_MEMORY&&!f.owner.geometry&&!f.owner.bytes);
    uint64_t original_admission=optional_charge(CX_GEOMETRY_COLD_BYTES/CX_BLOCK_BYTES);
    assert(original_admission*16u<=UINT32_MAX&&original_admission*15u>=required);
    /* Immediately below F's ratio threshold, G's smaller actual allocation
     * would fit, but admission must still deliberately omit the cold layer. */
    f.owner.maximum=(uint32_t)(original_admission*16u-1u);
    assert(optional_charge(CX_BLOCK_SLOTS)<=f.owner.maximum/16u);
    assert(cx_geometry_create(&f.owner)==TCW_OK&&!f.owner.geometry->cold_bytes&&f.owner.bytes==required);cx_geometry_destroy(&f.owner);
    f.owner.maximum=(uint32_t)(original_admission*16u);
    assert(cx_geometry_create(&f.owner)==TCW_OK&&f.owner.geometry->cold_bytes);cx_geometry_destroy(&f.owner);
    /* Model other already-accounted owner storage, without allocating it.
     * Restore the synthetic charge after geometry frees its own allocations. */
    uint32_t resident_at=f.owner.maximum-(uint32_t)original_admission-required;
    f.owner.bytes=resident_at+1u;
    assert(cx_geometry_create(&f.owner)==TCW_OK&&!f.owner.geometry->cold_bytes);
    cx_geometry_destroy(&f.owner);assert(f.owner.bytes==resident_at+1u);f.owner.bytes=resident_at;
    assert(cx_geometry_create(&f.owner)==TCW_OK&&f.owner.geometry->cold_bytes);
    cx_geometry_destroy(&f.owner);assert(f.owner.bytes==resident_at);f.owner.bytes=0;
    f.owner.maximum=160u*1024u*1024u;
    for(uint32_t n=1;n<=8;n++){partition_attempt=0;partition_fail_at=n;assert(cx_geometry_create(&f.owner)==TCW_MEMORY&&!f.owner.geometry&&!f.owner.bytes);size=123;assert(cx_geometry_order_cache_arena(&f.owner,&size)==NULL&&size==0);}
    partition_fail_at=0;partition_attempt=0;assert(cx_geometry_create(&f.owner)==TCW_OK&&partition_attempt==8u);
    uint64_t current=optional_charge(CX_BLOCK_SLOTS),original_f=optional_charge(CX_GEOMETRY_COLD_BYTES/CX_BLOCK_BYTES);
    assert(current<original_f&&f.owner.bytes==(uint64_t)required+current);
    assert(f.owner.bytes<=(uint64_t)required+original_f);
    assert(original_f-current==(uint64_t)((CX_GEOMETRY_COLD_BYTES/CX_BLOCK_BYTES)-CX_BLOCK_SLOTS)*sizeof(uint32_t));
    assert(cx_geometry_order_cache_arena(&f.owner,&size)&&size<=CX_GEOMETRY_ORDER_CACHE_BYTES);
    finish(&f);size=123;assert(cx_geometry_order_cache_arena(NULL,&size)==NULL&&size==0);assert(cx_geometry_order_cache_arena(NULL,NULL)==NULL);
}
int main(void){uint32_t baseline=tx_native_heap_used();tail_and_wrap();footprint_and_failures();assert(tx_native_heap_used()==baseline);puts("geometry order-cache partition contract passed");return 0;}
