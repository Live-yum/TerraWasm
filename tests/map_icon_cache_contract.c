/* Differential oracle: the existing buffered MAP strip writer and the former
 * uncached stream path must emit exactly the same complete MAP bytes. Include
 * the implementation to inspect optional-cache ownership without new ABI. */
#include "terra_types.h"
#include <zlib.h>
static uint8_t* observe_map_alloc(uint32_t);
static void observe_map_buf_init(TxBuf*,uint32_t);
static int observe_map_uncompress(unsigned char*,unsigned long*,const unsigned char*,unsigned long);
#define tx_alloc observe_map_alloc
#define buf_init observe_map_buf_init
#define uncompress observe_map_uncompress
#include "../src/terra_map.c"
#undef tx_alloc
#undef buf_init
#undef uncompress
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

extern uint8_t* tx_alloc(uint32_t);
extern void buf_init(TxBuf*,uint32_t);
extern uint32_t tx_native_heap_used(void);
extern uint32_t tx_heap_used(void);
extern void txw_test_allocation_limit(uint32_t);
extern void txw_test_live_allocation_limit(uint64_t);
extern void* tx_internal_realloc(void*,uint32_t);
extern char tx_last_error[256];
extern void write_tile(TxWorld*,TxBuf*,const TxTile*,uint32_t);

static TxWorld world;
static TxPreparedOutput prepared;
static TxMarkerPoint locations[24];
static uint8_t important[1];
static TxBuf tiles;
static const MapMarkerEntry *chest_markers;
static uint32_t chest_marker_count;
static uint32_t observed_icon_pixels;
static struct {
    int enabled;
    uint32_t cache_allocations,cache_live,compression_ok,compression_failed;
    uint32_t window_ok,window_failed,chunk_output_ok,chunk_output_failed;
} allocation_trace;
static uint32_t decode_calls,decode_oom_count;

/* Observe real allocations made by the included encoder. These wrappers do
 * not change allocation results or the aggregate budget implementation. */
static uint8_t* observe_map_alloc(uint32_t size) {
    uint8_t *p=tx_alloc(size);
    if(allocation_trace.enabled&&p&&size==4u*1024u*1024u){
        allocation_trace.cache_allocations++;allocation_trace.cache_live=tx_heap_used();
    }
    return p;
}

static void observe_map_buf_init(TxBuf *b,uint32_t size) {
    buf_init(b,size);
    if(!allocation_trace.enabled)return;
    if(size==17408u){
        if(b->ok)allocation_trace.compression_ok++;else allocation_trace.compression_failed++;
    }else if(size==1048576u){
        if(b->ok)allocation_trace.window_ok++;else allocation_trace.window_failed++;
    }else if(size!=4096u&&size!=65536u){
        if(b->ok)allocation_trace.chunk_output_ok++;else allocation_trace.chunk_output_failed++;
    }
}

static int observe_map_uncompress(unsigned char *out,unsigned long *size,
                                 const unsigned char *data,unsigned long length) {
    decode_calls++;
    if(decode_oom_count){decode_oom_count--;*size=1;return Z_MEM_ERROR;}
    return uncompress(out,size,data,length);
}

static void observe_icon_pixels(TxStreamMap *encoder) {
    if(!encoder->icon_values)return;
    uint32_t resolved=0;
    for(uint32_t i=0;i<world.icon_atlas.atlas_width*world.icon_atlas.atlas_height;i++){
        if(!world.icon_atlas.rgba[i*4+3])assert(!encoder->icon_values[i]);
        resolved+=encoder->icon_values[i]!=0;
    }
    if(resolved>observed_icon_pixels)observed_icon_pixels=resolved;
}

static void begin_allocation_trace(void) {
    memset(&allocation_trace,0,sizeof allocation_trace);allocation_trace.enabled=1;
}

static TxStreamMap* begin_map(void) {
    return tx_stream_map_begin(&world,chest_markers,chest_marker_count,
        prepared.markers,prepared.marker_count);
}

static TxTile column(uint32_t x) {
    TxTile tile={0};tile.active=x%3!=0;tile.type=1;tile.frame_x=tile.frame_y=-1;
    return tile;
}

static void make_world(void) {
    world.version=world.original_version=326;world.maxTilesX=193;world.maxTilesY=128;
    world.worldSurface=42;world.rockLayer=86;world.worldId=7;
    strcpy(world.worldName,"Icon cache contract");strcpy(world.magic,"relogic");
    world.tile_type_count=2;world.important=important;world.important_len=1;
    buf_init(&tiles,4096);
    for(uint32_t x=0;x<(uint32_t)world.maxTilesX;x++){TxTile tile=column(x);write_tile(&world,&tiles,&tile,127);}
    assert(tiles.ok);world.file=tiles.data;world.file_len=tiles.len;world.ends[1]=tiles.len;
    prepared.ready=1;prepared.marker_count=4;
    for(uint32_t i=0;i<4;i++){
        MapMarkerEntry *marker=&prepared.markers[i];marker->id=1;marker->locate=1;
        marker->frame_x=marker->frame_y=-1;marker->icon_id=1001+(int32_t)i;
        marker->radius=(uint8_t)(24+i*2);marker->line_width=3;
        marker->rgba[0]=(uint8_t)(230-i*31);marker->rgba[1]=(uint8_t)(20+i*47);marker->rgba[2]=50;marker->rgba[3]=255;
    }
    for(uint32_t i=0;i<24;i++)locations[i]=(TxMarkerPoint){(int32_t)((i*29u)%193u),(int32_t)((i*37u)%128u),i%4};
    /* Centers and icon circles straddle both 64-column and world edges. */
    locations[0]=(TxMarkerPoint){0,0,0};locations[1]=(TxMarkerPoint){63,64,1};
    locations[2]=(TxMarkerPoint){64,127,2};locations[3]=(TxMarkerPoint){192,1,3};
    prepared.points=(TxBuf){(uint8_t*)locations,sizeof locations,sizeof locations,1};world.prepared_output=&prepared;
}

static void set_atlas(uint32_t variant,uint32_t width,uint32_t height) {
    free(world.icon_atlas.rgba);memset(&world.icon_atlas,0,sizeof world.icon_atlas);
    world.icon_atlas.atlas_width=width;world.icon_atlas.atlas_height=height;
    world.icon_atlas.icon_size=32;world.icon_atlas.icon_count=4;
    world.icon_atlas.rgba=calloc((size_t)width*height,4);assert(world.icon_atlas.rgba);
    for(uint32_t icon=0;icon<4;icon++){
        world.icon_atlas.item_ids[icon]=1001+icon;world.icon_atlas.x_offsets[icon]=icon*32;
        for(uint32_t y=0;y<32;y++)for(uint32_t x=0;x<32;x++){
            uint32_t n=y*32+x;uint8_t *pixel=world.icon_atlas.rgba+(y*width+icon*32+x)*4;
            pixel[0]=(uint8_t)(n+variant*71);pixel[1]=(uint8_t)((n>>8)*61+icon*13+variant*29);pixel[2]=(uint8_t)(x*y+icon*53);
            if(!(n%17)){pixel[0]=31;pixel[1]=67;pixel[2]=113;} /* repeated RGB across icons */
            pixel[3]=n%11==0?0:n%7==0?1:n%5==0?127:255;
        }
    }
}

typedef struct RecordingSink {
    MapChunkStagingContext staging;
    uint32_t baseline,peak;
} RecordingSink;

static int record_map_chunk(uint32_t index,const uint8_t *data,uint32_t size,void *context) {
    RecordingSink *sink=context;
    uint32_t extra=tx_native_heap_used()-sink->baseline;
    if(extra>sink->peak)sink->peak=extra;
    return stage_map_chunk(index,data,size,&sink->staging);
}

static TxBuf buffered_map(TxStreamMap *encoder,uint32_t *extra_peak) {
    uint32_t baseline=tx_native_heap_used();
    TxBuf staged={0},result={0};buf_init(&staged,65536);buf_init(&result,65536);
    uint32_t *sizes=calloc(encoder->chunks,4),*offsets=calloc(encoder->chunks,4);assert(sizes&&offsets);
    RecordingSink context={{&staged,offsets,sizes,encoder->chunks,32u*1024u*1024u,0},baseline,0};
    assert(walk_map_chunks(&world,&encoder->request,encoder->points,encoder->point_count,
        encoder->width,encoder->height,encoder->cpr,encoder->cpc,encoder->strip_bytes,NULL,record_map_chunk,&context));
    buf_bytes(&result,encoder->header.data,encoder->header.len);
    for(uint32_t i=0;i<encoder->chunks;i++){buf_u32le(&result,sizes[i]);buf_bytes(&result,staged.data+offsets[i],sizes[i]);}
    if(extra_peak)*extra_peak=context.peak;
    assert(result.ok);free(sizes);free(offsets);tx_internal_free(staged.data);return result;
}

/* The collector is allocated by the caller before imposing any memory budget.
 * A failure in the OOM contract must come from the real encoder, not this sink. */
static int finish_stream(TxStreamMap *encoder,TxBuf *result) {
    observed_icon_pixels=0;
    for(uint32_t iterations=0;iterations<10000;iterations++){
        uint32_t offset=0,length=0,first=0,count=0;const uint8_t *bytes=NULL;
        int pending=tx_stream_map_pull(encoder,&offset,&bytes,&length);
        if(pending<0)return 0;
        if(pending){
            uint32_t again_offset=0,again_length=0;const uint8_t *again=NULL;
            assert(tx_stream_map_pull(encoder,&again_offset,&again,&again_length)==1);
            assert(offset==again_offset&&length==again_length&&bytes==again);
            assert(offset<=UINT32_MAX-length);uint32_t end=offset+length;
            /* No allocations here: complete outputs fit in the preallocated collector. */
            assert(end<=result->cap);memcpy(result->data+offset,bytes,length);
            if(end>result->len)result->len=end;
            if(!tx_stream_map_ack(encoder))return 0;
            continue;
        }
        int range=tx_stream_map_range(encoder,&first,&count);
        if(range<0)continue;
        if(!range)return result->len==tx_stream_map_size(encoder);
        for(uint32_t x=first;x<first+count;x++){
            TxTile tile=column(x);if(!tx_stream_map_run(encoder,x,0,&tile,128))return 0;
        }
        if(!tx_stream_map_finish_strip(encoder))return 0;
        observe_icon_pixels(encoder);
    }
    assert(!"MAP encoder did not finish");return 0;
}

static TxBuf stream_map(int cache,int low_memory,int buffered_oracle) {
    uint32_t baseline=tx_native_heap_used();
    if(low_memory)txw_test_allocation_limit(128u*1024u);
    TxStreamMap *encoder=tx_stream_map_begin(&world,NULL,0,prepared.markers,prepared.marker_count);assert(encoder);
    if(low_memory){assert(!encoder->icon_values);txw_test_allocation_limit(UINT32_MAX);}
    else if(world.icon_atlas.rgba)assert(encoder->icon_values);
    if(!cache&&encoder->icon_values){tx_internal_free(encoder->icon_values);encoder->icon_values=NULL;}
    if(buffered_oracle){
        TxBuf result=buffered_map(encoder,NULL);tx_stream_map_free(encoder);
        assert(tx_native_heap_used()==baseline+result.cap);return result;
    }
    TxBuf result={0};buf_init(&result,65536);assert(result.ok&&finish_stream(encoder,&result));
    if(cache&&!low_memory&&world.icon_atlas.rgba)
        assert(observed_icon_pixels>512); /* Measured before direct-output eviction. */
    /* Every compressed chunk is a valid, complete 64x64 uint32 MAP plane. */
    uint32_t at=encoder->header.len;uint8_t raw[16384];
    for(uint32_t i=0;i<encoder->chunks;i++){
        uint32_t size=(uint32_t)result.data[at]|(uint32_t)result.data[at+1]<<8|(uint32_t)result.data[at+2]<<16|(uint32_t)result.data[at+3]<<24;at+=4;
        uLongf decoded=sizeof raw;assert(size<=result.len-at);assert(uncompress(raw,&decoded,result.data+at,size)==Z_OK&&decoded==sizeof raw);at+=size;
    }
    assert(at==result.len);tx_stream_map_free(encoder);assert(tx_native_heap_used()==baseline+result.cap);return result;
}

static void equal(const TxBuf *a,const TxBuf *b){assert(a->len==b->len&&!memcmp(a->data,b->data,a->len));}

/* Literal case expectations are deliberately independent of the production
 * allocation helper. The oracle always executes the former uncached loop. */
static void gate_case(const char *name,int expect_cache) {
    uint32_t baseline=tx_native_heap_used(),buffered_peak=0;
    TxStreamMap *encoder=begin_map();assert(encoder);
    assert((encoder->icon_values!=NULL)==expect_cache);
    uint32_t begin_bytes=tx_native_heap_used()-baseline;
    TxBuf actual={0};buf_init(&actual,65536);assert(actual.ok&&finish_stream(encoder,&actual));
    if(expect_cache)assert(observed_icon_pixels); /* Really sampled before output eviction. */
    TxBuf buffered=buffered_map(encoder,&buffered_peak);equal(&actual,&buffered);
    tx_stream_map_free(encoder);

    encoder=begin_map();assert(encoder);
    if(encoder->icon_values)tx_internal_free(encoder->icon_values);
    encoder->icon_values=NULL;
    TxBuf oracle={0};buf_init(&oracle,65536);assert(oracle.ok&&finish_stream(encoder,&oracle));
    equal(&actual,&oracle);tx_stream_map_free(encoder);

    if(!expect_cache){
        /* Removing the unused atlas changes neither bytes nor live payloads.
         * The buffered sink observes the cache while walk_map_chunks owns it. */
        uint8_t *rgba=world.icon_atlas.rgba;world.icon_atlas.rgba=NULL;
        uint32_t before=tx_native_heap_used(),without_peak=0;
        encoder=begin_map();assert(encoder&&!encoder->icon_values);
        assert(tx_native_heap_used()-before==begin_bytes);
        TxBuf without=buffered_map(encoder,&without_peak);
        equal(&actual,&without);assert(buffered_peak==without_peak);
        tx_stream_map_free(encoder);tx_internal_free(without.data);world.icon_atlas.rgba=rgba;
    }
    printf("gate %s: cache=%u begin=%u buffered_live_extra=%u map_bytes=%u\n",
        name,expect_cache?4194304u:0u,begin_bytes,buffered_peak,actual.len);
    tx_internal_free(actual.data);tx_internal_free(buffered.data);tx_internal_free(oracle.data);
    assert(tx_native_heap_used()==baseline);
}

static void drawable_gate_contract(void) {
    MapMarkerEntry saved[4];memcpy(saved,prepared.markers,sizeof saved);
    set_atlas(0,1024,1024);
    /* A one-pixel icon samples the origin. Make that pixel visible for the
     * small-radius positive cases; earlier cases retain transparent origins. */
    for(uint32_t i=0;i<4;i++)world.icon_atlas.rgba[i*32u*4u+3u]=255;
    for(uint32_t i=0;i<4;i++)prepared.markers[i].icon_id=-1;
    gate_case("ring-only",0);
    memcpy(prepared.markers,saved,sizeof saved);
    for(uint32_t i=0;i<4;i++)prepared.markers[i].radius=0;
    gate_case("radius-zero",0);
    memcpy(prepared.markers,saved,sizeof saved);
    for(uint32_t i=0;i<4;i++)prepared.markers[i].icon_id=9000+(int32_t)i;
    gate_case("unknown-item",0);
    memcpy(prepared.markers,saved,sizeof saved);
    for(uint32_t i=0;i<4;i++){prepared.markers[i].radius=1;prepared.markers[i].line_width=0;}
    gate_case("radius-one",0);
    for(uint32_t i=0;i<4;i++){prepared.markers[i].radius=2;prepared.markers[i].line_width=1;}
    gate_case("inner-side-zero",0);
    for(uint32_t i=0;i<4;i++)prepared.markers[i].line_width=2;
    gate_case("line-width-equals-radius",1);
    for(uint32_t i=0;i<4;i++)prepared.markers[i].line_width=255;
    gate_case("line-width-exceeds-radius",1);
    for(uint32_t i=0;i<4;i++)prepared.markers[i].line_width=0;
    gate_case("line-width-zero",1);
    memcpy(prepared.markers,saved,sizeof saved);
    prepared.markers[0].icon_id=-1;prepared.markers[1].radius=0;prepared.markers[2].icon_id=9000;
    gate_case("mixed-valid-later-point",1);
    memcpy(prepared.markers,saved,sizeof saved);
    prepared.points.len=0;gate_case("no-points",0);prepared.points.len=sizeof locations;
    world.icon_atlas.icon_size=0;gate_case("zero-icon-size",0);world.icon_atlas.icon_size=32;
    world.icon_atlas.icon_count=0;gate_case("empty-atlas-index",0);world.icon_atlas.icon_count=4;

    /* The chest selector has no icon_id, but the matched inventory item does.
     * This raw 326 chest section contains one slot holding atlas item 1001. */
    TxBuf chests={0};buf_init(&chests,64);buf_u16le(&chests,1);
    buf_u32le(&chests,63);buf_u32le(&chests,64);buf_u8(&chests,0);
    buf_u32le(&chests,1);buf_u16le(&chests,1);buf_u32le(&chests,1001);buf_u8(&chests,0);assert(chests.ok);
    MapMarkerEntry chest=saved[0];chest.id=1001;chest.icon_id=-1;
    chest_markers=&chest;chest_marker_count=1;prepared.points.len=0;
    world.pointer_count=3;world.ends[2]=chests.len;
    world.section_overrides[2].active=1;world.section_overrides[2].data=chests.data;world.section_overrides[2].len=chests.len;
    TxStreamMap *encoder=begin_map();assert(encoder&&encoder->point_count==1&&encoder->points[0].item_id==1001);
    tx_stream_map_free(encoder);gate_case("chest-actual-item",1);
    chest_markers=NULL;chest_marker_count=0;prepared.points.len=sizeof locations;
    world.pointer_count=0;world.ends[2]=0;memset(&world.section_overrides[2],0,sizeof world.section_overrides[2]);
    tx_internal_free(chests.data);memcpy(prepared.markers,saved,sizeof saved);
}

static void aggregate_budget_contract(void) {
    uint32_t baseline=tx_heap_used();uint8_t *p=tx_alloc(64);assert(p);memset(p,37,64);
    txw_test_live_allocation_limit((uint64_t)baseline+64);
    assert(!tx_alloc(1)&&!tx_persistent_alloc(1));
    assert(!tx_internal_realloc(p,65));for(uint32_t i=0;i<64;i++)assert(p[i]==37);
    p=tx_internal_realloc(p,32);assert(p);
    uint8_t *persistent=tx_persistent_alloc(32);assert(persistent);persistent[0]=91;
    assert(tx_heap_used()==baseline+64&&!tx_internal_realloc(persistent,33)&&persistent[0]==91);
    tx_internal_free(p);tx_internal_free(persistent);txw_test_live_allocation_limit(UINT64_MAX);
    assert(tx_heap_used()==baseline);
}

/* Run this same contract against the previous terra_map.c to reproduce the
 * review failure. That encoder successfully reserves 4 MiB, then the actual
 * compression buffer fails. The corrected encoder finishes under this cap.
 * The no-atlas golden cannot allocate the cache under either implementation. */
static int later_compression_budget_contract(void) {
    const uint32_t cache_bytes=4u*1024u*1024u;
    uint32_t baseline=tx_heap_used();MapMarkerEntry saved[4];memcpy(saved,prepared.markers,sizeof saved);
    for(uint32_t i=0;i<4;i++)prepared.markers[i].icon_id=-1;
    set_atlas(0,1024,1024);uint8_t *rgba=world.icon_atlas.rgba;world.icon_atlas.rgba=NULL;
    TxStreamMap *encoder=begin_map();assert(encoder&&!encoder->icon_values);
    TxBuf golden={0};buf_init(&golden,65536);assert(golden.ok&&finish_stream(encoder,&golden));
    tx_stream_map_free(encoder);
    TxBuf actual={0};buf_init(&actual,golden.cap);assert(actual.ok);
    uint32_t before=tx_heap_used();encoder=begin_map();assert(encoder&&!encoder->icon_values);
    uint32_t mandatory=tx_heap_used()-before;tx_stream_map_free(encoder);world.icon_atlas.rgba=rgba;

    uint64_t budget=(uint64_t)before+mandatory+cache_bytes;
    txw_test_live_allocation_limit(budget);tx_clear_error();
    encoder=begin_map();assert(encoder);
    uint32_t begin_live=tx_heap_used(),allocated=encoder->icon_values?cache_bytes:0;
    int complete=finish_stream(encoder,&actual);
    char error[256];memcpy(error,tx_last_error,sizeof error);
    assert(tx_heap_used()<=budget);
    if(allocated){
        assert(begin_live==budget&&!complete&&actual.len==0);
        assert(strstr(error,"map chunk compression buffer allocation failed"));
    }else{
        assert(begin_live==before+mandatory&&complete);equal(&actual,&golden);
    }
    printf("compression budget: cap=%llu baseline=%u mandatory_begin=%u cache=%u begin_live=%u complete=%d map_bytes=%u error=%s\n",
        (unsigned long long)budget,before,mandatory,allocated,begin_live,complete,actual.len,error[0]?error:"none");
    tx_stream_map_free(encoder);txw_test_live_allocation_limit(UINT64_MAX);tx_clear_error();
    tx_internal_free(actual.data);tx_internal_free(golden.data);memcpy(prepared.markers,saved,sizeof saved);
    assert(tx_heap_used()==baseline);return !allocated&&complete;
}

static void draw_next_strip(TxStreamMap *encoder) {
    uint32_t first=0,count=0;assert(tx_stream_map_range(encoder,&first,&count)==1);
    for(uint32_t x=first;x<first+count;x++){
        TxTile tile=column(x);assert(tx_stream_map_run(encoder,x,0,&tile,128));
    }
    assert(tx_stream_map_finish_strip(encoder));observe_icon_pixels(encoder);
}

/* All goldens keep the same atlas and visible icons, explicitly disabling
 * only the colour cache. Modes target distinct real encoder allocations. */
static int stream_pressure_contract(uint32_t mode) {
    static const char *names[]={"first-compression","later-compression","staged-output-window",
        "fallback-compression","fallback-chunk-output","staging-growth-fallback","fallback-with-headroom"};
    uint32_t baseline=tx_heap_used();set_atlas(0,1024,1024);
    TxBuf golden=stream_map(0,0,0),actual={0};buf_init(&actual,65536);assert(actual.ok);
    uint64_t budget=UINT64_MAX;
    if(mode==0){
        uint32_t before=tx_heap_used();TxStreamMap *probe=begin_map();assert(probe&&probe->icon_values);
        budget=tx_heap_used();tx_stream_map_free(probe);assert(tx_heap_used()==before);
        txw_test_live_allocation_limit(budget);
    }
    tx_clear_error();begin_allocation_trace();TxStreamMap *encoder=begin_map();
    assert(encoder&&encoder->icon_values&&allocation_trace.cache_allocations==1);
    uint32_t cache_live=allocation_trace.cache_live;
    if(mode==1)draw_next_strip(encoder);
    if(mode==2)while(encoder->cx<encoder->cpr)draw_next_strip(encoder);
    if(mode==3||mode==4||mode==6)encoder->fallback=1;
    if(mode==3||mode==4){
        while(encoder->cx<encoder->cpr)draw_next_strip(encoder);
        uint32_t x=0,n=0;assert(tx_stream_map_range(encoder,&x,&n)<0&&encoder->pass==1&&encoder->pending==1);
        memcpy(actual.data,encoder->header.data,encoder->header.len);actual.len=encoder->header.len;
        assert(tx_stream_map_ack(encoder));draw_next_strip(encoder);
        assert(encoder->pending==2&&encoder->icon_values&&!encoder->output.data);
    }
    if(mode==5){
        tx_internal_free(encoder->staged.data);buf_init(&encoder->staged,64);assert(encoder->staged.ok);
    }
    if(mode!=0&&mode!=6){
        budget=(uint64_t)tx_heap_used()+((mode==4||mode==5)?17408u:0u);
        txw_test_live_allocation_limit(budget);
    }
    uint32_t compression_before=allocation_trace.compression_ok;
    int complete=finish_stream(encoder,&actual),cache_left=encoder->icon_values!=NULL;
    if(complete){equal(&actual,&golden);assert(!tx_last_error[0]);}
    int correct=complete;
    if(mode==0||mode==1||mode==3)
        correct=correct&&!cache_left&&allocation_trace.compression_failed==1&&allocation_trace.compression_ok>compression_before;
    if(mode==2)correct=correct&&!cache_left&&encoder->pass==2&&allocation_trace.window_ok==1&&!allocation_trace.window_failed;
    if(mode==4)correct=correct&&!cache_left&&!allocation_trace.compression_failed&&
        allocation_trace.chunk_output_failed==1&&allocation_trace.chunk_output_ok>0;
    if(mode==5)correct=correct&&encoder->fallback&&!cache_left;
    if(mode==6)correct=correct&&cache_left&&observed_icon_pixels>512;
    printf("drawable stream %s: cap=%llu cache_allocations=%u cache_live=%u cache_left=%d compression_ok=%u compression_failed=%u window_ok=%u window_failed=%u chunk_output_ok=%u chunk_output_failed=%u pass=%u complete=%d bytes=%u error=%s\n",
        names[mode],(unsigned long long)budget,allocation_trace.cache_allocations,cache_live,cache_left,
        allocation_trace.compression_ok,allocation_trace.compression_failed,allocation_trace.window_ok,allocation_trace.window_failed,
        allocation_trace.chunk_output_ok,allocation_trace.chunk_output_failed,encoder->pass,complete,actual.len,
        tx_last_error[0]?tx_last_error:"none");
    allocation_trace.enabled=0;tx_stream_map_free(encoder);txw_test_live_allocation_limit(UINT64_MAX);tx_clear_error();
    tx_internal_free(actual.data);tx_internal_free(golden.data);assert(tx_heap_used()==baseline);return correct;
}

static void append_buffered_chunks(TxStreamMap *encoder,TxBuf *result,TxBuf *staged,
                                   uint32_t *sizes,uint32_t *offsets) {
    buf_bytes(result,encoder->header.data,encoder->header.len);
    for(uint32_t i=0;i<encoder->chunks;i++){
        buf_u32le(result,sizes[i]);buf_bytes(result,staged->data+offsets[i],sizes[i]);
    }
    assert(result->ok);
}

/* Exercise the production staging, measuring and positioned-output sinks.
 * The last mode reproduces staging growth pressure and its original two-pass
 * fallback instead of retrying an arbitrary sink with partially written state. */
static int buffered_pressure_contract(uint32_t mode) {
    static const char *names[]={"staged-compression","positioned-output","measure-and-output","staging-growth-fallback"};
    uint32_t baseline=tx_heap_used();set_atlas(0,1024,1024);
    TxStreamMap *encoder=begin_map();assert(encoder&&encoder->icon_values);
    tx_internal_free(encoder->icon_values);encoder->icon_values=NULL;
    txw_test_allocation_limit(128u*1024u);TxBuf golden=buffered_map(encoder,NULL);txw_test_allocation_limit(UINT32_MAX);
    TxBuf actual={0},staged={0};buf_init(&actual,65536);buf_init(&staged,mode==3?64:65536);assert(actual.ok&&staged.ok);
    uint32_t *sizes=calloc(encoder->chunks,4),*offsets=calloc(encoder->chunks,4);assert(sizes&&offsets);
    uint32_t at=encoder->header.len;
    for(uint32_t i=0;i<encoder->chunks;i++){
        offsets[i]=at;sizes[i]=(uint32_t)golden.data[at]|(uint32_t)golden.data[at+1]<<8|
            (uint32_t)golden.data[at+2]<<16|(uint32_t)golden.data[at+3]<<24;at+=4+sizes[i];
    }
    assert(at==golden.len);
    MapChunkStagingContext stage={&staged,offsets,sizes,encoder->chunks,32u*1024u*1024u,0};
    MapChunkMeasureContext measure={sizes,encoder->chunks};
    MapChunkWriteContext write={actual.data,golden.len,offsets,sizes,encoder->chunks};
    MapChunkSink sink=mode==1?write_map_chunk:mode==2?measure_map_chunk:stage_map_chunk;
    void *context=mode==1?(void*)&write:mode==2?(void*)&measure:(void*)&stage;
    if(mode==1){memcpy(actual.data,encoder->header.data,encoder->header.len);actual.len=golden.len;}
    uint64_t budget=(uint64_t)tx_heap_used()+encoder->strip_bytes+4u*1024u*1024u+(mode==3?17408u:0u);
    txw_test_live_allocation_limit(budget);tx_clear_error();begin_allocation_trace();
    uint8_t *original=world.file;uint32_t original_len=world.file_len;
    int complete=walk_map_chunks(&world,&encoder->request,encoder->points,encoder->point_count,
        encoder->width,encoder->height,encoder->cpr,encoder->cpc,encoder->strip_bytes,NULL,sink,context);
    assert(world.file==original&&world.file_len==original_len&&allocation_trace.cache_allocations==1);
    if(mode==3){
        assert(!complete&&stage.fallback&&!staged.ok);tx_internal_free(staged.data);staged.data=NULL;
        tx_clear_error();memset(sizes,0,encoder->chunks*4);memset(offsets,0,encoder->chunks*4);
        complete=walk_map_chunks(&world,&encoder->request,encoder->points,encoder->point_count,
            encoder->width,encoder->height,encoder->cpr,encoder->cpc,encoder->strip_bytes,NULL,measure_map_chunk,&measure);
    }
    if(complete&&(mode==2||mode==3)){
        at=encoder->header.len;
        for(uint32_t i=0;i<encoder->chunks;i++){offsets[i]=at;at+=4+sizes[i];}
        assert(at==golden.len);memcpy(actual.data,encoder->header.data,encoder->header.len);actual.len=at;
        complete=walk_map_chunks(&world,&encoder->request,encoder->points,encoder->point_count,
            encoder->width,encoder->height,encoder->cpr,encoder->cpc,encoder->strip_bytes,NULL,write_map_chunk,&write);
    }else if(complete&&mode==0)append_buffered_chunks(encoder,&actual,&staged,sizes,offsets);
    if(complete){equal(&actual,&golden);assert(!tx_last_error[0]);}
    printf("drawable buffered %s: cap=%llu cache_allocations=%u cache_live=%u compression_ok=%u compression_failed=%u complete=%d bytes=%u error=%s\n",
        names[mode],(unsigned long long)budget,allocation_trace.cache_allocations,allocation_trace.cache_live,
        allocation_trace.compression_ok,allocation_trace.compression_failed,complete,actual.len,tx_last_error[0]?tx_last_error:"none");
    int correct=complete&&(mode==3||allocation_trace.compression_failed>0);
    allocation_trace.enabled=0;txw_test_live_allocation_limit(UINT64_MAX);tx_clear_error();
    free(sizes);free(offsets);tx_internal_free(actual.data);tx_internal_free(golden.data);
    if(staged.data)tx_internal_free(staged.data);tx_stream_map_free(encoder);assert(tx_heap_used()==baseline);return correct;
}

static void prepared_decode_contract(void) {
    uint32_t baseline=tx_heap_used();set_atlas(0,1024,1024);
    TxPreparedMap *base=tx_map_base_begin(&world);assert(base);
    for(uint32_t x=0;x<(uint32_t)world.maxTilesX;x++){
        TxTile tile=column(x);assert(tx_map_base_run(&world,base,x,0,&tile,128));
    }
    prepared.map=base;
    TxStreamMap *encoder=begin_map();assert(encoder&&encoder->icon_values);
    tx_internal_free(encoder->icon_values);encoder->icon_values=NULL;
    txw_test_allocation_limit(128u*1024u);TxBuf golden=buffered_map(encoder,NULL);txw_test_allocation_limit(UINT32_MAX);
    for(uint32_t mode=0;mode<3;mode++){
        TxBuf actual={0},staged={0};buf_init(&actual,65536);buf_init(&staged,65536);
        uint32_t *sizes=calloc(encoder->chunks,4),*offsets=calloc(encoder->chunks,4);assert(sizes&&offsets);
        MapChunkStagingContext stage={&staged,offsets,sizes,encoder->chunks,32u*1024u*1024u,0};
        uint8_t original=base->bytes[base->offsets[0]];
        if(mode==2)base->bytes[base->offsets[0]]=0; /* Real malformed zlib header. */
        decode_calls=0;decode_oom_count=mode==0?1:mode==1?2:0;
        tx_clear_error();begin_allocation_trace();uint32_t before=tx_heap_used();
        int complete=walk_map_chunks(&world,&encoder->request,encoder->points,encoder->point_count,
            encoder->width,encoder->height,encoder->cpr,encoder->cpc,encoder->strip_bytes,NULL,stage_map_chunk,&stage);
        assert(tx_heap_used()==before&&allocation_trace.cache_allocations==1);
        if(mode==0){
            assert(complete&&decode_calls==encoder->chunks+1&&!tx_last_error[0]);
            append_buffered_chunks(encoder,&actual,&staged,sizes,offsets);equal(&actual,&golden);
        }else if(mode==1){
            assert(!complete&&decode_calls==2&&strstr(tx_last_error,"TERRAX_WASM_OOM"));
        }else{
            assert(!complete&&decode_calls==1&&strstr(tx_last_error,"TERRAX_STATE_ERROR"));
        }
        printf("prepared decode mode=%u calls=%u complete=%d bytes=%u error=%s\n",mode,decode_calls,complete,actual.len,tx_last_error[0]?tx_last_error:"none");
        allocation_trace.enabled=0;base->bytes[base->offsets[0]]=original;decode_oom_count=0;tx_clear_error();
        free(sizes);free(offsets);tx_internal_free(actual.data);tx_internal_free(staged.data);
    }
    tx_internal_free(golden.data);tx_stream_map_free(encoder);prepared.map=NULL;tx_map_base_free(base);
    assert(tx_heap_used()==baseline);
}

static void cancel_after_eviction_contract(void) {
    uint32_t baseline=tx_heap_used();set_atlas(0,1024,1024);
    TxStreamMap *encoder=begin_map();assert(encoder&&encoder->icon_values);
    txw_test_live_allocation_limit(tx_heap_used());draw_next_strip(encoder);
    assert(!encoder->icon_values&&encoder->cx==1&&!tx_last_error[0]);
    tx_stream_map_free(encoder);txw_test_live_allocation_limit(UINT64_MAX);assert(tx_heap_used()==baseline);
}

int main(int argc,char **argv) {
    aggregate_budget_contract();
    make_world();set_atlas(0,256,256);
    if(argc==2&&!strcmp(argv[1],"--oom-only")){
        int ok=later_compression_budget_contract();free(world.icon_atlas.rgba);tx_internal_free(tiles.data);
        assert(tx_heap_used()==0);return ok?0:1;
    }
    if(argc==2&&(!strcmp(argv[1],"--drawable-oom")||!strcmp(argv[1],"--output-oom")||!strcmp(argv[1],"--buffered-oom"))){
        int ok=!strcmp(argv[1],"--buffered-oom")?buffered_pressure_contract(0):
            stream_pressure_contract(!strcmp(argv[1],"--output-oom")?4:0);
        free(world.icon_atlas.rgba);tx_internal_free(tiles.data);assert(tx_heap_used()==0);return ok?0:1;
    }
    TxBuf first=stream_map(1,0,0),uncached=stream_map(0,0,0),buffered=stream_map(1,0,1),fallback=stream_map(1,1,0);
    equal(&first,&uncached);equal(&first,&buffered);equal(&first,&fallback);
    tx_internal_free(uncached.data);tx_internal_free(buffered.data);tx_internal_free(fallback.data);
    set_atlas(1,256,256);TxBuf changed=stream_map(1,0,0),changed_reference=stream_map(0,0,0);
    equal(&changed,&changed_reference);assert(first.len!=changed.len||memcmp(first.data,changed.data,first.len));
    tx_internal_free(first.data);tx_internal_free(changed.data);tx_internal_free(changed_reference.data);
    /* Cancellation/free before any output releases the optional cache too. */
    uint32_t baseline=tx_native_heap_used();TxStreamMap *cancelled=tx_stream_map_begin(&world,NULL,0,prepared.markers,4);
    assert(cancelled&&cancelled->icon_values);uint32_t x,n;assert(tx_stream_map_range(cancelled,&x,&n)==1);
    tx_stream_map_free(cancelled);assert(tx_native_heap_used()==baseline);
    set_atlas(0,1024,1024);cancelled=tx_stream_map_begin(&world,NULL,0,prepared.markers,4);assert(cancelled&&cancelled->icon_values);tx_stream_map_free(cancelled);assert(tx_native_heap_used()==baseline);
    set_atlas(0,1024,1025);cancelled=tx_stream_map_begin(&world,NULL,0,prepared.markers,4);assert(cancelled&&!cancelled->icon_values);tx_stream_map_free(cancelled);assert(tx_native_heap_used()==baseline);
    free(world.icon_atlas.rgba);world.icon_atlas.rgba=NULL;
    TxBuf without=stream_map(1,0,0),without_reference=stream_map(0,0,1);equal(&without,&without_reference);
    tx_internal_free(without.data);tx_internal_free(without_reference.data);
    drawable_gate_contract();assert(later_compression_budget_contract());
    for(uint32_t mode=0;mode<7;mode++)assert(stream_pressure_contract(mode));
    for(uint32_t mode=0;mode<4;mode++)assert(buffered_pressure_contract(mode));
    prepared_decode_contract();cancel_after_eviction_contract();
    free(world.icon_atlas.rgba);tx_internal_free(tiles.data);
    assert(tx_native_heap_used()==0);
    puts("MAP icon cache: exact buffered/uncached bytes, drawable-point gates, aggregate-budget compression, alpha/repeated RGB, atlas replacement, 4 MiB bound, allocation fallback and cancellation release passed");
    return 0;
}
