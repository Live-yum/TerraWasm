/* New runtime block geometry, independent grid oracle and full source guards. */
#define main geometry_existing_contract_main
#include "circuit_geometry_contract.c"
#undef main
static void* block_test_alloc(CxWorld*,uint32_t);
#define cx_alloc block_test_alloc
#include "../src/terra_circuit_geometry.c"
#undef cx_alloc
static uint32_t attempt,fail_at;
static void* block_test_alloc(CxWorld* w,uint32_t n){if(++attempt==fail_at){cx_fail(w,TCW_MEMORY,"injected block allocator failure");return NULL;}return cx_alloc(w,n);}
static void clear_hot(CxOrderGeometry* g){memset(g->valid,0,sizeof(g->valid));}
static void point(Fixture* f,uint32_t x,uint32_t y){int s=cx_geometry_point_begin(&f->owner,x,y);for(uint32_t n=0;s!=CX_GEOMETRY_READY;n++){
    assert(n<1000000);CxGeometryRead r;s=cx_geometry_step(&f->owner,1,&r);assert(s>=0);if(s==CX_GEOMETRY_READ)assert(cx_geometry_supply(&f->owner,&r,f->bytes+r.offset,r.length)==0);
}uint8_t word;assert(cx_geometry_point_get(&f->owner,x,y,&word)==0&&word==f->expected[x*f->height+y]);}
static void demand_and_full(int rle){Fixture f;init(&f,8,259,rle);assert(cx_geometry_create(&f.owner)==0);TxWorld source=f.world;uint32_t hash=fingerprint(&f);CxOrderGeometry* g=f.owner.geometry;
    point(&f,0,129);assert(block_find(g,0,1)&&!block_find(g,0,0)&&!block_find(g,0,2));
    CxGeometryStats before=g->stats;point(&f,0,1);assert(g->stats.misses==before.misses&&g->stats.read_requests==before.read_requests&&block_find(g,0,0));
    point(&f,0,258);clear_hot(g);for(uint32_t y=0;y<f.height;y++){uint8_t word;assert(cx_geometry_point_get(&f.owner,0,y,&word)==0&&word==f.expected[y]);}
    assert(cx_geometry_get(&f.owner,0,0,&(uint8_t){0})==TCW_STATE); /* block never claims fullREADY */
    uint64_t records=g->stats.decoded_records;fill(&f,0,7);assert(g->stats.decoded_records>records);assert(!memcmp(&source,&f.world,sizeof(source))&&hash==fingerprint(&f));
    CxBlock* e=block_find(g,0,2);assert(e);uint8_t* last=g->cold_bytes+e->slot*CX_BLOCK_BYTES+3;uint8_t keep=*last;*last|=128;clear_hot(g);assert(cx_geometry_point_get(&f.owner,0,258,&(uint8_t){0})==TCW_FORMAT);*last=keep;
    uint32_t owner=g->slot_owner[e->slot];g->slot_owner[e->slot]=0;assert(cx_geometry_point_get(&f.owner,0,258,&(uint8_t){0})==TCW_FORMAT);g->slot_owner[e->slot]=owner;
    finish(&f);assert(cx_geometry_point_get(&f.owner,0,0,&(uint8_t){0})==TCW_STATE);
}
static void parsed_axis(void){Fixture f;init(&f,5,2,0);f.size=16;
    for(uint32_t x=0;x<5;x++){f.columns[x]=f.size;byte(&f,35);byte(&f,x?0:2);word(&f,65534u);record(&f,0,0,0,0,1);f.expected[x*2]=x?0:1;f.expected[x*2+1]=0;}
    f.columns[5]=f.owner.tile_end=f.size;assert(cx_geometry_create(&f.owner)==0);for(uint32_t x=0;x<5;x++)point(&f,x,0);clear_hot(f.owner.geometry);
    uint8_t word,axis;assert(block_get(&f.owner,0,0,&word,&axis)==0&&word==1&&axis==1);assert(block_get(&f.owner,1,0,&word,&axis)==0&&word==0&&axis==1);finish(&f);
}
static void direct_block(Fixture* f,uint32_t x,uint32_t block){CxOrderGeometry* g=f->owner.geometry;g->slot=0;g->target_column=x;g->point_row=block*128;g->source_end[0]=f->owner.tile_start+1;
    for(uint32_t y=0;y<f->height;y++){uint16_t pair=(uint16_t)((y*73u+256u)&511u);g->cells[y]=(uint8_t)pair;g->compiler_axis[y]=(uint8_t)(pair>>8);}
    start_block(&f->owner,g);uint32_t steps=0;do{uint32_t work=1;assert(block_step(&f->owner,&work)>=0);assert(++steps<=131);}while(g->block_phase);clear_hot(g);
}
static void codec_slots_lru(void){Fixture f;init(&f,4,259,0);assert(cx_geometry_create(&f.owner)==0);CxOrderGeometry* g=f.owner.geometry;
    g->block_head=CX_BLOCK_SLOTS-1;direct_block(&f,0,0);assert(block_find(g,0,0)->slot==CX_BLOCK_SLOTS-1);direct_block(&f,0,1);direct_block(&f,0,2);
    for(uint32_t y=0;y<f.height;y++){uint8_t word,axis;uint16_t pair=(uint16_t)((y*73u+256u)&511u);assert(block_get(&f.owner,0,y,&word,&axis)==0&&word==(uint8_t)pair&&axis==pair>>8);}
    /* Same ring slot invalidates previous key, stale owner cannot alias it. */
    g->block_head=CX_BLOCK_SLOTS-1;direct_block(&f,1,0);assert(!block_find(g,0,0)&&block_find(g,1,0));
    uint32_t keys[5],n=0;for(uint32_t x=10;n<5;x++)if(block_set(x,0)==20)keys[n++]=x;
    uint64_t conflicts=g->stats.cold_conflict_evictions;for(uint32_t i=0;i<5;i++)direct_block(&f,keys[i],0);assert(g->stats.cold_conflict_evictions==conflicts+1);
    uint32_t kept=0;for(uint32_t i=0;i<5;i++)kept+=block_find(g,keys[i],0)!=NULL;assert(kept==4);
    /* Explicit trueLRU policy trace, not old saturated-age heuristic. */
    memset(g->valid,1,sizeof(g->valid));g->age[0]=0;g->age[1]=2;g->age[2]=1;touch(g,1);uint32_t ranks[3];memcpy(ranks,g->age,sizeof(ranks));touch(g,1);touch(g,1);assert(!memcmp(ranks,g->age,sizeof(ranks))&&g->age[2]>g->age[0]);finish(&f);
}
static void mixed_buffered(void){Fixture f;init(&f,64,32,0);f.owner.world_source=0;f.world.file=f.bytes;f.world.file_len=f.size;CxCheckpoint cp={0};cp.source_offset=f.columns[0];f.owner.checkpoint_index=&cp;f.owner.checkpoint_count=1;
    assert(cx_geometry_create(&f.owner)==0);for(uint32_t x=0;x<64;x+=2)point(&f,x,3);cx_geometry_cancel(&f.owner);clear_hot(f.owner.geometry);CxGeometryStats before=f.owner.geometry->stats;
    for(uint32_t x=0;x<64;x++){point(&f,x,3);for(uint32_t y=0;y<f.height;y++){uint8_t word;assert(cx_geometry_point_get(&f.owner,x,y,&word)==0&&word==f.expected[x*f.height+y]);}}
    CxGeometryStats after=f.owner.geometry->stats;assert(after.decoded_records-before.decoded_records==32u*f.height&&after.misses-before.misses==32u&&after.read_requests==before.read_requests);
    /* Tagged hint missing is safe fallback; matching block still retains end. */
    CxOrderGeometry* g=f.owner.geometry;clear_hot(g);g->sequential_valid=0;CxBlock* previous=block_find(g,0,0);CxBlock* target=block_find(g,1,0);assert(previous&&target);block_invalidate(g,(uint32_t)(target-g->blocks));g->ends[0].valid=0;
    uint32_t end=previous->source_end;previous->source_end=f.owner.tile_end;assert(cx_geometry_point_begin(&f.owner,1,3)==TCW_FORMAT);previous->source_end=end;point(&f,1,3);finish(&f);
}
static void cancellation(void){Fixture f;init(&f,4,259,0);assert(cx_geometry_create(&f.owner)==0);CxOrderGeometry* g=f.owner.geometry;point(&f,0,0);g->block_head=block_find(g,0,0)->slot;
    assert(cx_geometry_point_begin(&f.owner,3,129)==CX_GEOMETRY_MORE);CxGeometryRead stale={0};TxWorld source=f.world;
    for(uint32_t n=0;;n++){assert(n<100000);CxGeometryRead r;int s=cx_geometry_step(&f.owner,1,&r);assert(s>=0);if(s==CX_GEOMETRY_READ){stale=r;assert(cx_geometry_supply(&f.owner,&r,f.bytes+r.offset,r.length)==0);}if(g->block_phase==2&&g->block_row>3)break;}
    assert(!block_find(g,0,0)&&!block_find(g,3,1));cx_geometry_cancel(&f.owner);assert(!g->active&&!g->block_phase&&!block_find(g,3,1));assert(cx_geometry_supply(&f.owner,&stale,f.bytes+stale.offset,stale.length)==TCW_INVALID);assert(!memcmp(&source,&f.world,sizeof(source)));point(&f,3,129);finish(&f);
}
static void assert_lru(CxOrderGeometry* g){uint32_t count=0,mask=0;for(uint32_t i=0;i<3;i++)if(g->valid[i]){assert(g->age[i]<3);++count;assert(!(mask&(1u<<g->age[i])));mask|=1u<<g->age[i];}assert(mask==(1u<<count)-1u);}
static void cancelled_lru_trace(void){Fixture f;init(&f,8,259,0);assert(cx_geometry_create(&f.owner)==0);fill(&f,0,7);fill(&f,1,7);fill(&f,2,7);CxOrderGeometry* g=f.owner.geometry;assert_lru(g);
    /* Partial source: victim is already removed, new slot is not yet valid. */
    assert(cx_geometry_point_begin(&f.owner,3,129)==CX_GEOMETRY_MORE);CxGeometryRead pending;assert(cx_geometry_step(&f.owner,1,&pending)==CX_GEOMETRY_READ);assert_lru(g);cx_geometry_cancel(&f.owner);assert_lru(g);
    assert(cx_geometry_point_begin(&f.owner,3,129)==CX_GEOMETRY_MORE);assert(cx_geometry_step(&f.owner,1,&pending)==CX_GEOMETRY_READ);
    assert(cx_geometry_supply(&f.owner,&pending,f.bytes+pending.offset,pending.length)==0);CxGeometryRead partial;
    assert(cx_geometry_step(&f.owner,2,&partial)==CX_GEOMETRY_MORE&&g->row>0&&g->row<f.height&&!g->valid[g->slot]);assert_lru(g);cx_geometry_cancel(&f.owner);assert_lru(g);
    /* A completely validated existing hot column gets only a new block fill. */
    fill(&f,3,7);assert_lru(g);uint64_t reads=g->stats.read_requests;assert(cx_geometry_point_begin(&f.owner,3,129)==CX_GEOMETRY_MORE);
    CxGeometryRead none;assert(cx_geometry_step(&f.owner,2,&none)==CX_GEOMETRY_MORE&&g->block_phase==2&&g->block_row==1&&g->stats.read_requests==reads);cx_geometry_cancel(&f.owner);assert_lru(g);
    /* Reload, repeated MRU hit, and subsequent real eviction keep unique ranks. */
    fill(&f,3,7);uint8_t word;for(uint32_t i=0;i<5;i++){assert(cx_geometry_get(&f.owner,3,i,&word)==0);assert_lru(g);}fill(&f,0,7);fill(&f,1,7);assert_lru(g);
    for(uint32_t x=3;x<8;x++){
        /* x3 may be hot; forcing a different block still covers hot admission. */
        assert(cx_geometry_point_begin(&f.owner,x,129)==CX_GEOMETRY_MORE);
        for(uint32_t n=0;;n++){assert(n<100000);CxGeometryRead r;int s=cx_geometry_step(&f.owner,1,&r);assert(s>=0);if(s==CX_GEOMETRY_READ)assert(cx_geometry_supply(&f.owner,&r,f.bytes+r.offset,r.length)==0);if(g->block_phase==2&&g->block_row==1)break;}
        cx_geometry_cancel(&f.owner);assert_lru(g);
    }
    finish(&f);
}
static void budget_owners(void){Fixture f;init(&f,4,32,0);f.owner.maximum=1024*1024;assert(cx_geometry_create(&f.owner)==0&&!f.owner.geometry->blocks);uint32_t required=f.owner.bytes;point(&f,0,0);cx_geometry_destroy(&f.owner);
    f.owner.maximum=required;assert(cx_geometry_create(&f.owner)==0);cx_geometry_destroy(&f.owner);f.owner.maximum=required-1;assert(cx_geometry_create(&f.owner)==TCW_MEMORY&&!f.owner.geometry&&!f.owner.bytes);
    f.owner.maximum=160u*1024u*1024u;for(uint32_t n=1;n<=8;n++){attempt=0;fail_at=n;assert(cx_geometry_create(&f.owner)==TCW_MEMORY&&!f.owner.geometry&&!f.owner.bytes);}fail_at=0;assert(cx_geometry_create(&f.owner)==0);
    Fixture b;init(&b,4,32,1);assert(cx_geometry_create(&b.owner)==0);point(&f,0,1);point(&b,0,1);clear_hot(f.owner.geometry);clear_hot(b.owner.geometry);point(&f,0,1);finish(&f);point(&b,0,1);finish(&b);
}
int main(void){uint32_t baseline=tx_native_heap_used();demand_and_full(0);demand_and_full(1);parsed_axis();codec_slots_lru();mixed_buffered();cancellation();cancelled_lru_trace();budget_owners();assert(tx_native_heap_used()==baseline);puts("new production block geometry contract passed");return 0;}
