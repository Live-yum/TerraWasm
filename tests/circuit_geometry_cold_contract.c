/* Fixed-budget cold geometry cache: actual implementation, generated sources. */
#define main geometry_existing_contract_main
#include "circuit_geometry_contract.c"
#undef main
static void* cold_test_alloc(CxWorld*,uint32_t);
#define cx_alloc cold_test_alloc
#include "oracles/terra_circuit_geometry_palette_f625.c"
#undef cx_alloc
/* Frozen palette oracle only; adapters satisfy linked ordering symbols.
 * This historical geometry has no borrowed result-cache storage. */
void* cx_geometry_order_cache_arena(CxWorld* w,uint32_t* size){(void)w;if(size)*size=0;return NULL;}
int cx_geometry_point_begin(CxWorld* w,uint32_t x,uint32_t y){(void)y;return cx_geometry_begin(w,x);}
int cx_geometry_point_get(CxWorld* w,uint32_t x,uint32_t y,uint8_t* out){return cx_geometry_get(w,x,y,out);}
static uint32_t allocation_attempt,fail_attempt;
static void* cold_test_alloc(CxWorld* w,uint32_t bytes){if(++allocation_attempt==fail_attempt){cx_fail(w,TCW_MEMORY,"injected persistent allocator failure");return NULL;}return cx_alloc(w,bytes);}
static void clear_hot(CxOrderGeometry* g){memset(g->valid,0,sizeof(g->valid));}
static void complete(Fixture* f,uint32_t x){int status=cx_geometry_begin(&f->owner,x);for(uint32_t n=0;status!=CX_GEOMETRY_READY;n++){
    assert(n<20000000u);CxGeometryRead r;status=cx_geometry_step(&f->owner,17,&r);assert(status>=0);
    if(status==CX_GEOMETRY_READ)assert(cx_geometry_supply(&f->owner,&r,f->bytes+r.offset,r.length)==0);
}}
static CxColdColumn* entry(CxOrderGeometry* g,uint32_t column){uint32_t base=cold_set(column)*CX_GEOMETRY_COLD_WAYS;for(uint32_t i=0;i<CX_GEOMETRY_COLD_WAYS;i++)if(g->cold_entries[base+i].valid&&g->cold_entries[base+i].column==column)return g->cold_entries+base+i;return NULL;}
static void warm_cycle(int rle){Fixture f;init(&f,32,96,rle);assert(cx_geometry_create(&f.owner)==0);uint32_t bytes=f.owner.bytes;
    for(uint32_t x=0;x<32;x++)fill(&f,x,13);CxGeometryStats before;cx_geometry_stats(&f.owner,&before);clear_hot(f.owner.geometry);
    for(uint32_t pass=0;pass<4;pass++)for(uint32_t x=0;x<32;x++)assert(fill(&f,x,1)==0);
    CxGeometryStats after;cx_geometry_stats(&f.owner,&after);assert(after.misses==before.misses&&after.decoded_records==before.decoded_records&&after.read_requests==before.read_requests);
    assert(after.cold_hits>before.cold_hits&&after.cold_fills==32&&f.owner.bytes==bytes);assert(after.cold_raw_fills+after.cold_raw9_fills+after.cold_palette_fills+after.cold_rle_fills==32);finish(&f);
}
/* Use real bounded encoder to exercise exact byte formats independently of IO. */
static void encode_pair_column(Fixture* f,uint32_t column,uint16_t first,uint16_t second){
    CxOrderGeometry* g=f->owner.geometry;g->slot=0;g->target_column=column;g->cold_runs=first==second?1:2;
    for(uint32_t y=0;y<f->height;y++){uint16_t v=y<f->height/2?first:second;g->cells[y]=(uint8_t)v;g->compiler_axis[y]=(uint8_t)(v>>8);}
    g->cold_format=1;g->cold_length=g->cold_runs*6;g->cold_offset=g->cold_head;if(g->cold_offset>CX_GEOMETRY_COLD_BYTES-g->cold_length)g->cold_offset=0;g->cold_scan=0;g->cold_phase=1;
    int status;do{uint32_t work=1;status=cold_step(&f->owner,&work);assert(status>=0&&work<=1);}while(status);clear_hot(g);
}
static void encoding_collision_bounds(void){Fixture f;init(&f,32,64,0);assert(cx_geometry_create(&f.owner)==0);CxOrderGeometry* g=f.owner.geometry;
    encode_pair_column(&f,0,0x100,128);uint8_t word,route;
    assert(cx_geometry_get_connectivity(&f.owner,0,0,&word,&route)==0&&word==0&&route==1); /* unwired raw0xfffe */
    assert(cx_geometry_get_connectivity(&f.owner,0,40,&word,&route)==0&&word==128&&route==0); /* unwired420 */
    encode_pair_column(&f,1,0x101,0x31);assert(cx_geometry_get_connectivity(&f.owner,1,0,&word,&route)==0&&word==1&&route==1);
    assert(cx_geometry_get_connectivity(&f.owner,1,40,&word,&route)==0&&word==0x31&&route==3);
    CxColdColumn* e=entry(g,0);assert(e);uint32_t saved=e->length;e->length=1;assert(cx_geometry_get(&f.owner,0,0,&word)==TCW_FORMAT);e->length=saved;
    uint32_t offset=e->offset;e->offset=CX_GEOMETRY_COLD_BYTES;assert(cx_geometry_get(&f.owner,0,0,&word)==TCW_FORMAT);e->offset=offset;
    uint8_t* last=g->cold_bytes+offset+(e->runs-1u)*6u;uint32_t endpoint=cold_u32(last);cold_put32(last,endpoint-1u);assert(cx_geometry_get(&f.owner,0,0,&word)==TCW_FORMAT);
    cold_put32(last,endpoint);
    g->cold_head=CX_GEOMETRY_COLD_BYTES-5;encode_pair_column(&f,2,7,7);assert(entry(g,2)->offset==0&&!entry(g,0));assert(g->stats.cold_overlap_evictions>0);
    /* Five distinct keys in one four-way set; metadata cap is independent of W. */
    uint32_t keys[5],found=0;for(uint32_t x=100;found<5;x++)if(cold_set(x)==17)keys[found++]=x;
    uint64_t conflicts=g->stats.cold_conflict_evictions;for(uint32_t i=0;i<5;i++)encode_pair_column(&f,keys[i],1,1);
    assert(g->stats.cold_conflict_evictions==conflicts+1);uint32_t retained=0;for(uint32_t i=0;i<5;i++)retained+=entry(g,keys[i])!=NULL;assert(retained==4);finish(&f);
}
static void cancel_fill(uint32_t phase){Fixture f;init(&f,8,64,0);assert(cx_geometry_create(&f.owner)==0);CxOrderGeometry* g=f.owner.geometry;fill(&f,0,7);g->cold_head=0; /* force overlap eviction before cancelling */
    TxWorld owner=f.world;uint32_t source=fingerprint(&f);assert(cx_geometry_begin(&f.owner,7)==CX_GEOMETRY_MORE);CxGeometryRead last={0};
    for(uint32_t n=0;;n++){assert(n<100000);CxGeometryRead r;int s=cx_geometry_step(&f.owner,1,&r);assert(s>=0);
        if(s==CX_GEOMETRY_READ){last=r;assert(cx_geometry_supply(&f.owner,&r,f.bytes+r.offset,r.length)==0);}
        if(g->cold_phase==phase&&(phase==1?g->cold_scan>8:phase==3?g->cold_palette_scan>8:g->cold_row>8))break;
    }
    assert(!entry(g,7)&&!entry(g,0));cx_geometry_cancel(&f.owner);assert(!g->active&&!g->cold_phase&&!entry(g,7));
    assert(cx_geometry_supply(&f.owner,&last,f.bytes+last.offset,last.length)==TCW_INVALID);
    assert(!memcmp(&owner,&f.world,sizeof(owner))&&source==fingerprint(&f));fill(&f,7,1);assert(entry(g,7));finish(&f);
}
static void parsed_raw_axis(void){Fixture f;init(&f,5,2,0);f.size=16;
    for(uint32_t x=0;x<5;x++){f.columns[x]=f.size;byte(&f,35);byte(&f,x?0:2);word(&f,65534u);record(&f,0,0,0,0,1);}
    f.columns[5]=f.owner.tile_end=f.size;assert(cx_geometry_create(&f.owner)==0);
    for(uint32_t x=0;x<5;x++)complete(&f,x);clear_hot(f.owner.geometry);
    uint8_t word,route;assert(cx_geometry_get_connectivity(&f.owner,0,0,&word,&route)==0&&word==1&&route==1);
    assert(cx_geometry_get_connectivity(&f.owner,1,0,&word,&route)==0&&word==0&&route==1);finish(&f);
}
static void owners_and_oversize(void){Fixture a,b;init(&a,4,64,0);init(&b,4,64,1);assert(cx_geometry_create(&a.owner)==0&&cx_geometry_create(&b.owner)==0);
    fill(&a,0,7);fill(&b,0,7);clear_hot(a.owner.geometry);clear_hot(b.owner.geometry);for(uint32_t y=0;y<64;y++){uint8_t x,z;assert(cx_geometry_get(&a.owner,0,y,&x)==0&&x==a.expected[y]);assert(cx_geometry_get(&b.owner,0,y,&z)==0&&z==b.expected[y]);}
    finish(&a);fill(&b,0,1);finish(&b);
    /* Synthetic already-decoded oversized column exercises admission/bypass
     * without retaining a huge source or allocating a width-sized structure. */
    Fixture huge;init(&huge,1,32,0);assert(cx_geometry_create(&huge.owner)==0);CxOrderGeometry* g=huge.owner.geometry;
    uint32_t height=huge.owner.height;huge.owner.height=CX_GEOMETRY_COLD_BYTES;g->cold_count=512;g->cold_runs=huge.owner.height;
    g->active=1;g->slot=0;g->scan_column=g->target_column=0;g->row=huge.owner.height;g->cursor=g->end=huge.owner.tile_end;
    assert(cold_select(&huge.owner,g)>CX_GEOMETRY_COLD_BYTES);CxGeometryRead read;
    assert(cx_geometry_step(&huge.owner,1,&read)==CX_GEOMETRY_READY&&g->stats.cold_oversize==1&&!entry(g,0));huge.owner.height=height;finish(&huge);
}
static void buffered_mixed_cache(void){Fixture f;init(&f,64,32,0);f.owner.world_source=0;f.world.file=f.bytes;f.world.file_len=f.size;
    CxCheckpoint cp={0};cp.column=0;cp.source_offset=f.columns[0];f.owner.checkpoint_index=&cp;f.owner.checkpoint_count=1;
    assert(cx_geometry_create(&f.owner)==0);for(uint32_t x=0;x<64;x+=2)complete(&f,x);
    cx_geometry_cancel(&f.owner);clear_hot(f.owner.geometry);CxGeometryStats before,after;cx_geometry_stats(&f.owner,&before);
    for(uint32_t x=0;x<64;x++){complete(&f,x);for(uint32_t y=0;y<f.height;y++){uint8_t word;assert(cx_geometry_get(&f.owner,x,y,&word)==0&&word==f.expected[x*f.height+y]);}}
    cx_geometry_stats(&f.owner,&after);assert(after.decoded_records-before.decoded_records==32u*f.height&&after.misses-before.misses==32u&&after.read_requests==before.read_requests);
    /* Invalid or absent predecessor endpoints cannot invent a parser cursor. */
    clear_hot(f.owner.geometry);CxColdColumn* prior=entry(f.owner.geometry,0);CxColdColumn* target=entry(f.owner.geometry,1);assert(prior&&target);target->valid=0;
    f.owner.geometry->sequential_valid=0;uint32_t end=prior->source_end;prior->source_end=f.owner.tile_end;assert(cx_geometry_begin(&f.owner,1)==TCW_FORMAT);prior->source_end=end;
    prior->valid=0;complete(&f,1);assert(entry(f.owner.geometry,1));finish(&f);
}
static void optional_budget_contract(void){
    /* Synthetic statistics prove widening before addition without >4GiB alloc. */
    uint64_t wide=cx_resident_total(UINT32_MAX,UINT32_MAX);
    assert(wide==2ull*UINT32_MAX&&!cold_budget_enabled(UINT32_MAX,wide,4096u));
    assert(cx_resident_total(UINT32_MAX,1u)==(uint64_t)UINT32_MAX+1u);
    assert(!cold_budget_enabled(UINT32_MAX,cx_resident_total(UINT32_MAX,1u),4096u));
    assert(!cold_budget_enabled(UINT32_MAX,0,UINT64_MAX));
    assert(cold_budget_enabled(160u*1024u*1024u,0,2u*1024u*1024u));

    Fixture f;init(&f,4,64,0);f.owner.maximum=1024u*1024u;assert(cx_geometry_create(&f.owner)==0);assert(!f.owner.geometry->cold_bytes&&!f.owner.geometry->cold_entries&&!f.owner.geometry->cold_scratch);
    uint64_t required=cx_allocation_charge(sizeof(CxOrderGeometry))+2u*cx_allocation_charge(64u*CX_GEOMETRY_SLOTS)+cx_allocation_charge(CX_GEOMETRY_WINDOW);
    assert(f.owner.bytes==required);fill(&f,0,7);cx_geometry_destroy(&f.owner);assert(!f.owner.bytes);
    f.owner.maximum=(uint32_t)required;assert(cx_geometry_create(&f.owner)==0);fill(&f,0,1);cx_geometry_destroy(&f.owner);
    f.owner.maximum=(uint32_t)required-1u;assert(cx_geometry_create(&f.owner)==TCW_MEMORY&&!f.owner.geometry&&!f.owner.bytes);
    /* Enough total maximum, but not enough remaining resident headroom: decide
     * disabled before attempting either optional allocation. */
    f.owner.maximum=160u*1024u*1024u;uint32_t resident=f.owner.maximum-1024u*1024u;f.owner.bytes=resident;
    allocation_attempt=0;assert(cx_geometry_create(&f.owner)==0&&allocation_attempt==4&&!f.owner.geometry->cold_bytes);cx_geometry_destroy(&f.owner);assert(f.owner.bytes==resident);f.owner.bytes=0;
    /* Enabled admission followed by a real allocation failure remains fatal,
     * on arena, table, and scratch, and unwinds all earlier geometry allocations. */
    for(uint32_t fail=5;fail<=7;fail++){allocation_attempt=0;fail_attempt=fail;f.owner.error=0;assert(cx_geometry_create(&f.owner)==TCW_MEMORY);assert(!f.owner.geometry&&!f.owner.bytes&&f.owner.error==(uint32_t)-TCW_MEMORY);}
    fail_attempt=0;assert(cx_geometry_create(&f.owner)==0&&f.owner.geometry->cold_bytes&&f.owner.geometry->cold_entries);finish(&f);
}
#ifndef COLD_CONTRACT_MAIN
#define COLD_CONTRACT_MAIN main
#endif
int COLD_CONTRACT_MAIN(void){uint32_t baseline=tx_native_heap_used();warm_cycle(0);warm_cycle(1);encoding_collision_bounds();cancel_fill(1);cancel_fill(2);parsed_raw_axis();owners_and_oversize();window_and_budget();buffered_mixed_cache();optional_budget_contract();assert(tx_native_heap_used()==baseline);puts("fixed-byte cold geometry cache contract passed");return 0;}
