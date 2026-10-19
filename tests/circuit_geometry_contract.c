#include "terra_circuit_geometry.h"
#include "terra_world.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
extern uint32_t tx_native_heap_used(void);
extern TxWorld* tx_get_world(uint32_t);

typedef struct Fixture {
    CxWorld owner; TxWorld world;
    uint8_t important[128],owner_canary[64],*bytes,*expected; uint32_t *columns,size,capacity,width,height;
} Fixture;
static void byte(Fixture* f,uint8_t b){assert(f->size<f->capacity);f->bytes[f->size++]=b;}
static void word(Fixture* f,uint16_t b){byte(f,(uint8_t)b);byte(f,(uint8_t)(b>>8));}
/* Independent model of the existing sparse order.cells representation. */
static uint8_t reference(uint32_t active,uint16_t type,int16_t frame,uint32_t mask){
    if(!mask)return 0;uint32_t route=0;
    if(active&&(type==424u||type==445u)){int r=type==424u?frame/18:0;if(r>=0&&r<=2)route=(uint32_t)r+1u;}
    return (uint8_t)(mask|(route<<4)|(active&&type==419u?64u:0u));
}
static void record(Fixture* f,uint32_t active,uint16_t type,int16_t frame,uint32_t mask,uint32_t run){
    uint8_t a=1u|(active?34u:0u),b=(uint8_t)((mask&7u)<<1),c=(mask&8u)?32u:0u;
    if(c)b|=1u;if(run>1u)a|=(run-1u>255u?128u:64u);
    byte(f,a);byte(f,b);if(c)byte(f,c);
    if(active){word(f,type);word(f,(uint16_t)frame);word(f,0);}
    if(run>1u){if(run-1u>255u)word(f,(uint16_t)(run-1u));else byte(f,(uint8_t)(run-1u));}
}
static void init(Fixture* f,uint32_t width,uint32_t height,int rle){
    memset(f,0,sizeof(*f));f->width=width;f->height=height;f->capacity=16u+width*height*12u;
    f->bytes=calloc(f->capacity,1);f->expected=calloc(width*height,1);f->columns=calloc(width+1u,sizeof(uint32_t));assert(f->bytes&&f->expected&&f->columns);f->size=16;
    f->important[419u>>3]|=1u<<(419u&7u);f->important[424u>>3]|=1u<<(424u&7u);f->important[445u>>3]|=1u<<(445u&7u);
    for(uint32_t x=0;x<width;x++){
        f->columns[x]=f->size;
        for(uint32_t y=0;y<height;){
            uint32_t run=rle?((y+x)%17u+1u):1u;if(run>height-y)run=height-y;
            uint32_t variant=(x+y)%8u,active=variant!=0u;
            uint16_t type=variant==1u||variant==2u?419u:variant==3u?445u:424u;
            int16_t frame=(int16_t)(((int)(x+y)%5-1)*18);
            uint32_t mask=(x*7u+y)%16u;
            record(f,active,type,frame,mask,run);
            memset(f->expected+x*height+y,reference(active,type,frame,mask),run);y+=run;
        }
    }
    f->columns[width]=f->size;
    f->world.version=f->world.original_version=279;f->world.tile_type_count=754;
    f->world.important=f->important;f->world.important_len=sizeof(f->important);f->world.stream_columns=f->columns;
    memset(f->owner_canary,0xa5,sizeof(f->owner_canary));
    f->world.file=f->owner_canary;f->world.file_len=15;f->world.tile_decode_calls=123;
    f->world.section_overrides[1].data=f->owner_canary;f->world.section_overrides[1].len=17;
    f->world.region_mask=f->owner_canary;f->world.allocation_mark=71;f->world.heap_mark=73;f->world.override_heap_mark=79;
    f->owner.maximum=160u*1024u*1024u;f->owner.world=&f->world;f->owner.width=width;f->owner.height=height;
    f->owner.input=f->owner_canary;f->owner.world_source=7;f->owner.tile_start=16;f->owner.tile_end=f->size;
    f->owner.x=17;f->owner.y=19;f->owner.cursor=23;f->owner.phase=CX_IDLE;f->owner.input_source=29;f->owner.input_offset=31;f->owner.input_length=37;
}
static void finish(Fixture* f){cx_geometry_destroy(&f->owner);assert(f->owner.bytes==0);free(f->bytes);free(f->expected);free(f->columns);}
static void guarded(const Fixture* f,const TxWorld* source,const CxWorld* owner){
    assert(!memcmp(&f->world,source,sizeof(*source)));assert(!memcmp(&f->owner,owner,sizeof(*owner)));
    for(uint32_t i=0;i<sizeof(f->owner_canary);i++)assert(f->owner_canary[i]==0xa5);
    for(uint32_t i=0;i<sizeof(f->important);i++){
        uint8_t expected=0;if(i==(419u>>3))expected|=1u<<(419u&7u);if(i==(424u>>3))expected|=1u<<(424u&7u);if(i==(445u>>3))expected|=1u<<(445u&7u);
        assert(f->important[i]==expected);
    }
}
static uint32_t fingerprint(const Fixture* f){uint32_t h=2166136261u;for(uint32_t i=0;i<f->size;i++)h=(h^f->bytes[i])*16777619u;for(uint32_t i=0;i<=f->width;i++)h=(h^f->columns[i])*16777619u;return h;}
static uint32_t fill(Fixture* f,uint32_t x,uint32_t work){
    TxWorld source=f->world;CxWorld owner=f->owner;uint32_t hash=fingerprint(f);
    uint32_t reads=0;int s=cx_geometry_begin(&f->owner,x);assert(s==CX_GEOMETRY_MORE||s==CX_GEOMETRY_READY);
    while(s!=CX_GEOMETRY_READY){CxGeometryRead request={0};s=cx_geometry_step(&f->owner,work,&request);assert(s>=0);guarded(f,&source,&owner);
        if(s==CX_GEOMETRY_READ){assert(request.source==7&&request.length<=CX_GEOMETRY_WINDOW&&request.offset>=f->columns[x]&&request.offset+request.length<=f->columns[x+1]);
            CxGeometryRead repeated;assert(cx_geometry_step(&f->owner,work,&repeated)==CX_GEOMETRY_READ);assert(!memcmp(&request,&repeated,sizeof(request)));
            assert(cx_geometry_supply(&f->owner,&request,f->bytes+request.offset,request.length)==0);guarded(f,&source,&owner);++reads;
        }
    }
    for(uint32_t y=0;y<f->height;y++){uint8_t got=255;assert(cx_geometry_get(&f->owner,x,y,&got)==0);assert(got==f->expected[x*f->height+y]);}
    assert(hash==fingerprint(f));return reads;
}
static void differential(int rle){
    Fixture f;init(&f,9,96,rle);TxWorld snapshot=f.world;assert(cx_geometry_create(&f.owner)==0);
    uint32_t bytes=f.owner.bytes;const uint32_t order[]={0,1,2,0,3,8,7,6,5,4,3,2,1,0};
    for(uint32_t i=0;i<sizeof(order)/sizeof(order[0]);i++){fill(&f,order[i],i%3u+1u);assert(f.owner.bytes==bytes);}
    assert(!memcmp(&snapshot,&f.world,sizeof(snapshot)));assert(f.owner.x==17&&f.owner.y==19&&f.owner.cursor==23&&f.owner.phase==CX_IDLE);
    assert(f.owner.input_source==29&&f.owner.input_offset==31&&f.owner.input_length==37);
    uint8_t value=99;assert(cx_geometry_get(&f.owner,9,0,&value)==TCW_INVALID&&value==99);
    assert(cx_geometry_get(&f.owner,0,96,&value)==TCW_INVALID);
    finish(&f);
}
static void cancellation_and_request_validation(void){
    Fixture f;init(&f,8,128,1);assert(cx_geometry_create(&f.owner)==0);fill(&f,0,7);TxWorld source=f.world;CxWorld owner=f.owner;
    assert(cx_geometry_begin(&f.owner,7)==CX_GEOMETRY_MORE);CxGeometryRead first;assert(cx_geometry_step(&f.owner,1,&first)==CX_GEOMETRY_READ);
    assert(cx_geometry_begin(&f.owner,1)==TCW_STATE);
    CxGeometryRead bad=first;bad.source++;assert(cx_geometry_supply(&f.owner,&bad,f.bytes+first.offset,first.length)==TCW_INVALID);
    bad=first;bad.offset++;assert(cx_geometry_supply(&f.owner,&bad,f.bytes+first.offset,first.length)==TCW_INVALID);
    assert(cx_geometry_supply(&f.owner,&first,f.bytes+first.offset,first.length-1u)==TCW_INVALID);
    cx_geometry_cancel(&f.owner);assert(cx_geometry_supply(&f.owner,&first,f.bytes+first.offset,first.length)==TCW_INVALID);
    uint8_t value;assert(cx_geometry_get(&f.owner,7,0,&value)==TCW_STATE);assert(cx_geometry_get(&f.owner,0,0,&value)==0);
    assert(cx_geometry_begin(&f.owner,7)==CX_GEOMETRY_MORE);CxGeometryRead second;assert(cx_geometry_step(&f.owner,1,&second)==CX_GEOMETRY_READ);assert(first.ticket!=second.ticket);
    assert(cx_geometry_supply(&f.owner,&first,f.bytes+first.offset,first.length)==TCW_INVALID);
    assert(cx_geometry_supply(&f.owner,&second,f.bytes+second.offset,second.length)==0);
    assert(cx_geometry_step(&f.owner,2,&second)==CX_GEOMETRY_MORE);cx_geometry_cancel(&f.owner);assert(cx_geometry_get(&f.owner,7,0,&value)==TCW_STATE);
    guarded(&f,&source,&owner);fill(&f,7,1);finish(&f);
}
static void window_and_budget(void){
    /* This boundary measures mandatory geometry, not an optional high-budget cache. */
    Fixture f;init(&f,1,24000,0);f.owner.maximum=1024u*1024u;assert(cx_geometry_create(&f.owner)==0);uint32_t size=f.owner.bytes;
    assert(fill(&f,0,7)>=2);cx_geometry_destroy(&f.owner);
    f.owner.width=1000000;f.owner.wire_cells=UINT32_MAX;assert(cx_geometry_create(&f.owner)==0);assert(f.owner.bytes==size);cx_geometry_destroy(&f.owner);
    f.owner.maximum=size-1;assert(cx_geometry_create(&f.owner)==TCW_MEMORY);assert(f.owner.bytes==0&&!f.owner.geometry);
    f.owner.maximum=1;assert(cx_geometry_create(&f.owner)==TCW_MEMORY);assert(f.owner.bytes==0&&!f.owner.geometry);
    finish(&f);
}
static void malformed_column(void){
    Fixture f;init(&f,1,8,1);assert(cx_geometry_create(&f.owner)==0);
    /* A bounded record declares more rows than the indexed column owns. */
    f.bytes[16]=65;f.bytes[17]=2;f.bytes[18]=255;f.columns[1]=f.owner.tile_end=19;TxWorld source=f.world;CxWorld owner=f.owner;
    assert(cx_geometry_begin(&f.owner,0)==CX_GEOMETRY_MORE);CxGeometryRead r;assert(cx_geometry_step(&f.owner,1,&r)==CX_GEOMETRY_READ);
    assert(cx_geometry_supply(&f.owner,&r,f.bytes+r.offset,r.length)==0);assert(cx_geometry_step(&f.owner,1,&r)==TCW_FORMAT);
    uint8_t v;assert(cx_geometry_get(&f.owner,0,0,&v)==TCW_STATE);guarded(&f,&source,&owner);finish(&f);
}
static void session_lease_and_destroy(void){
    Fixture f;init(&f,4,8,1);uint32_t handle=0,circuit=0;assert(terra_world_create(&handle)==0);
    TxWorld* source=tx_get_world(handle);TxWorld original=*source;
    source->version=source->original_version=279;source->maxTilesX=4;source->maxTilesY=8;
    source->stream_source_id=7;source->stream_source_size=f.size;source->stream_tile_start=16;source->stream_tile_end=f.size;source->stream_columns=f.columns;
    source->important=f.important;source->important_len=sizeof(f.important);source->tile_type_count=754;
    assert(terra_circuit_world_begin(handle,8,0,0,160u*1024u*1024u,&circuit)==0);CxWorld* w=cx_lookup(circuit);assert(w);
    assert(cx_geometry_create(w)==0);assert(cx_geometry_begin(w,0)==CX_GEOMETRY_MORE);CxGeometryRead r;assert(cx_geometry_step(w,1,&r)==CX_GEOMETRY_READ);
    assert(terra_world_close(handle)==TERRAX_WORLD_STATUS_STATE_ERROR);assert(terra_circuit_world_cancel(circuit)==0);
    assert(cx_geometry_supply(w,&r,f.bytes+r.offset,r.length)==TCW_STATE);assert(terra_world_close(handle)==TERRAX_WORLD_STATUS_STATE_ERROR);
    assert(terra_circuit_world_close(circuit)==0);*source=original;assert(terra_world_close(handle)==0);finish(&f);
}
int main(void){
    uint32_t before=tx_native_heap_used();differential(0);differential(1);cancellation_and_request_validation();window_and_budget();malformed_column();session_lease_and_destroy();
    assert(tx_native_heap_used()==before);puts("bounded geometry: differential RLE/routes, yields, refill, cancellation, budget and lease contracts passed");return 0;
}
