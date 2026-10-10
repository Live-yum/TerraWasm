/* Differential oracle: the existing buffered MAP strip writer and the former
 * uncached stream path must emit exactly the same complete MAP bytes. Include
 * the implementation to inspect optional-cache ownership without new ABI. */
#include "../src/terra_map.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>

extern uint32_t tx_native_heap_used(void);
extern void txw_test_allocation_limit(uint32_t);
extern void write_tile(TxWorld*,TxBuf*,const TxTile*,uint32_t);

static TxWorld world;
static TxPreparedOutput prepared;
static TxMarkerPoint locations[24];
static uint8_t important[1];
static TxBuf tiles;

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

static TxBuf buffered_map(TxStreamMap *encoder) {
    TxBuf staged={0},result={0};buf_init(&staged,65536);buf_init(&result,65536);
    uint32_t *sizes=calloc(encoder->chunks,4),*offsets=calloc(encoder->chunks,4);assert(sizes&&offsets);
    MapChunkStagingContext context={&staged,offsets,sizes,encoder->chunks,32u*1024u*1024u,0};
    assert(walk_map_chunks(&world,&encoder->request,encoder->points,encoder->point_count,
        encoder->width,encoder->height,encoder->cpr,encoder->cpc,encoder->strip_bytes,NULL,stage_map_chunk,&context));
    buf_bytes(&result,encoder->header.data,encoder->header.len);
    for(uint32_t i=0;i<encoder->chunks;i++){buf_u32le(&result,sizes[i]);buf_bytes(&result,staged.data+offsets[i],sizes[i]);}
    assert(result.ok);free(sizes);free(offsets);tx_internal_free(staged.data);return result;
}

static TxBuf stream_map(int cache,int low_memory,int buffered_oracle) {
    uint32_t baseline=tx_native_heap_used();
    if(low_memory)txw_test_allocation_limit(128u*1024u);
    TxStreamMap *encoder=tx_stream_map_begin(&world,NULL,0,prepared.markers,prepared.marker_count);assert(encoder);
    if(low_memory){assert(!encoder->icon_values);txw_test_allocation_limit(UINT32_MAX);}
    else if(world.icon_atlas.rgba)assert(encoder->icon_values);
    if(!cache&&encoder->icon_values){tx_internal_free(encoder->icon_values);encoder->icon_values=NULL;}
    if(buffered_oracle){
        TxBuf result=buffered_map(encoder);tx_stream_map_free(encoder);
        assert(tx_native_heap_used()==baseline+result.cap);return result;
    }
    TxBuf result={0};buf_init(&result,65536);uint32_t iterations=0;
    for(;;){
        assert(++iterations<10000);uint32_t offset=0,length=0,first=0,count=0;const uint8_t *bytes=NULL;
        int pending=tx_stream_map_pull(encoder,&offset,&bytes,&length);assert(pending>=0);
        if(pending){
            assert(offset<=UINT32_MAX-length);uint32_t end=offset+length;
            assert(buf_reserve(&result,end>result.len?end-result.len:0));memcpy(result.data+offset,bytes,length);
            if(end>result.len)result.len=end;assert(tx_stream_map_ack(encoder));continue;
        }
        int range=tx_stream_map_range(encoder,&first,&count);if(range<0)continue;if(!range)break;
        for(uint32_t x=first;x<first+count;x++){TxTile tile=column(x);assert(tx_stream_map_run(encoder,x,0,&tile,128));}
        assert(tx_stream_map_finish_strip(encoder));
    }
    assert(result.len==tx_stream_map_size(encoder));
    if(encoder->icon_values){
        uint32_t resolved=0;
        for(uint32_t i=0;i<world.icon_atlas.atlas_width*world.icon_atlas.atlas_height;i++){
            if(!world.icon_atlas.rgba[i*4+3])assert(!encoder->icon_values[i]);
            resolved+=encoder->icon_values[i]!=0;
        }
        assert(resolved>512); /* Exercise saturation of the older RGB cache. */
    }
    /* Every compressed chunk is a valid, complete 64x64 uint32 MAP plane. */
    uint32_t at=encoder->header.len;uint8_t raw[16384];
    for(uint32_t i=0;i<encoder->chunks;i++){
        uint32_t size=(uint32_t)result.data[at]|(uint32_t)result.data[at+1]<<8|(uint32_t)result.data[at+2]<<16|(uint32_t)result.data[at+3]<<24;at+=4;
        uLongf decoded=sizeof raw;assert(size<=result.len-at);assert(uncompress(raw,&decoded,result.data+at,size)==Z_OK&&decoded==sizeof raw);at+=size;
    }
    assert(at==result.len);tx_stream_map_free(encoder);assert(tx_native_heap_used()==baseline+result.cap);return result;
}

static void equal(const TxBuf *a,const TxBuf *b){assert(a->len==b->len&&!memcmp(a->data,b->data,a->len));}

int main(void) {
    make_world();set_atlas(0,256,256);
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
    tx_internal_free(without.data);tx_internal_free(without_reference.data);tx_internal_free(tiles.data);
    assert(tx_native_heap_used()==0);
    puts("stream MAP icon cache: exact buffered/uncached bytes, alpha/repeated RGB, atlas replacement, 4 MiB bound, allocation fallback and cancellation release passed");
    return 0;
}
