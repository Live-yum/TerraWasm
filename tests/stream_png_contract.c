/* Standalone bounded Web-memory check (run resulting JS with Node):
 * emcc -O2 -Iinclude tests/stream_png_contract.c src/terra_stream_png.c
 * -sUSE_ZLIB=1 -sINITIAL_MEMORY=67108864 -sMAXIMUM_MEMORY=167772160
 * -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=node -o build/stream_png_contract.js */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "terra_stream_png.h"
#include "terra_output.h"
#include "../src/terra_render_core.inc"
extern int memcmp(const void*, const void*, unsigned long);
extern unsigned long crc32(unsigned long, const uint8_t*, unsigned int);
static size_t live_bytes, peak_bytes;
uint8_t* tx_persistent_alloc(uint32_t n) {
    size_t* p = malloc(n + sizeof(size_t)); if (!p) return NULL;
    *p = n; live_bytes += n; if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    return (uint8_t*)(p + 1);
}
void tx_persistent_free(void* p) {
    if (p) { size_t* q = (size_t*)p - 1; live_bytes -= *q; free(q); }
}
void buf_bytes(TxBuf* b, const void* p, uint32_t n) {
    assert(n <= b->cap - b->len); if (n) memcpy(b->data + b->len, p, n); b->len += n;
}
void buf_u8(TxBuf* b, uint8_t c) { buf_bytes(b, &c, 1); }
void buf_u32be(TxBuf* b, uint32_t n) {
    uint8_t c[4] = {n >> 24, n >> 16, n >> 8, n}; buf_bytes(b, c, 4);
}
void tx_set_error(const char* c, const char* m) { (void)c; (void)m; }
const uint8_t* tx_get_tile_colors(void) { return NULL; }
const uint8_t* tx_get_wall_colors(void) { return NULL; }
uint32_t tx_get_tile_color_count(void) { return 0; }
uint32_t tx_get_wall_color_count(void) { return 0; }
void tx_render_stream_color(TxWorld* w, const TxTile* t, uint32_t y, uint8_t* c) {
    if (t) color_for_tile(t, y, w->maxTilesY, w->worldSurface, w->rockLayer, c);
    else background_color(y, w->maxTilesY, w->worldSurface, w->rockLayer, c);
}
void tx_render_stream_fixed_block(TxBuf* b, uint32_t* bits, uint32_t* count,
    const uint8_t* data, uint32_t n, uint32_t last) { write_fixed_block(b,bits,count,data,n,last); }
void tx_render_stream_finish_bits(TxBuf* b, uint32_t* bits, uint32_t* count) { bw_finish(b,bits,count); }
TxPngChestCache* tx_render_stream_chest_cache(TxWorld* w, const MapMarkerEntry* markers, uint32_t count) {
    (void)w; (void)markers; (void)count; return NULL;
}
void tx_render_stream_chest_cache_rows(TxWorld* w, const TxPngChestCache* cache,
    uint8_t* rgba, uint32_t width, uint32_t height, uint32_t start, uint32_t rows, const MapMarkerEntry* markers) {
    (void)w; (void)cache; (void)rgba; (void)width; (void)height; (void)start; (void)rows; (void)markers;
}
void tx_render_stream_chest_cache_free(TxPngChestCache* cache) { (void)cache; }
void tx_render_stream_markers(TxWorld* w, uint8_t* rgba, uint32_t width, uint32_t height,
    uint32_t y, uint32_t rows, const MapMarkerEntry* chests, uint32_t nc,
    const MapMarkerEntry* tiles, uint32_t nt, uint32_t phase) {
    (void)w; (void)chests; (void)nc; (void)tiles; (void)nt;
    if (!phase) { const uint8_t c[4] = {255, 0, 0, 128};
        draw_marker_ring_at_preview_rows(rgba,width,height,y,rows,2,128,2,1,c); }
}
void tx_render_stream_tile_marker(TxWorld* w, uint8_t* rgba, uint32_t width, uint32_t height,
    uint32_t y, uint32_t rows, uint32_t x, uint32_t ty, const TxTile* t, uint32_t run,
    const MapMarkerEntry* markers, uint32_t count) {
    (void)w;
    if (t->active && count) draw_marker_span_at_preview_rows(rgba,width,height,y,rows,
        x,ty,ty+run,markers[0].radius,markers[0].line_width,markers[0].rgba);
}
extern int uncompress(uint8_t*, unsigned long*, const uint8_t*, unsigned long);
static uint32_t be(const uint8_t* p) { return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
int main(void) {
    static TxWorld w; w.maxTilesX = 5; w.maxTilesY = 270; w.worldSurface = 100; w.rockLayer = 200;
    MapMarkerEntry marker = {0}; marker.radius=2; marker.line_width=1; marker.rgba[1]=255; marker.rgba[3]=128;
    TxStreamPng* p = tx_stream_png_begin(&w,0,0,NULL,0,&marker,1); assert(p);
    static uint8_t png[32768], compressed[32768], expected[5*270*4], raw[16*270]; uint32_t png_len=0;
    for (uint32_t y=0;y<270;y++) { uint8_t c[4]; tx_render_stream_color(&w,NULL,y,c);
        for (uint32_t x=0;x<5;x++) memcpy(expected+(y*5+x)*4,c,4); }
    TxTile t = {0}; t.active=1; t.type=1; t.tile_color=30;
    uint8_t c[4]; tx_render_stream_color(&w,&t,120,c);
    for(uint32_t y=120;y<135;y++) memcpy(expected+(y*5+1)*4,c,4);
    const uint8_t ring[4]={255,0,0,128}; draw_marker_ring_at_preview_rows(expected,5,270,0,270,2,128,2,1,ring);
    tx_render_stream_tile_marker(&w,expected,5,270,0,270,1,120,&t,15,&marker,1);
    uint32_t first, rows, scans=0;
    while(tx_stream_png_range(p,&first,&rows)==1) {
        assert(tx_stream_png_run(p,1,120,&t,15)); assert(tx_stream_png_finish_strip(p)); scans++;
        uint32_t offset,n; const uint8_t* data;
        if(tx_stream_png_pull(p,&offset,&data,&n)) {
            assert(n<=1048576 && offset==png_len && n<=sizeof(png)-png_len);
            uint32_t again_n, again_offset; const uint8_t* again;
            assert(tx_stream_png_pull(p,&again_offset,&again,&again_n) && again==data && again_n==n);
            assert(tx_stream_png_range(p,&first,&rows)==-1);
            memcpy(png+png_len,data,n); png_len+=n; assert(tx_stream_png_ack(p)); assert(!tx_stream_png_ack(p));
        }
    }
    assert(scans==6); uint32_t clen=0;
    for(uint32_t off=8;off<png_len;) { uint32_t n=be(png+off); assert(off+12+n<=png_len);
        assert(be(png+off+8+n)==(uint32_t)crc32(0,png+off+4,n+4));
        if(!memcmp(png+off+4,"IDAT",4)) { memcpy(compressed+clen,png+off+8,n); clen+=n; } off+=n+12; }
    unsigned long size=sizeof(raw); assert(!uncompress(raw,&size,compressed,clen) && size==sizeof(raw));
    for(uint32_t y=0;y<270;y++) { assert(raw[y*16]==0);
        for(uint32_t x=0;x<5;x++) { if(memcmp(raw+y*16+1+x*3,expected+(y*5+x)*4,3))
            printf("mismatch %u,%u actual %u,%u,%u expected %u,%u,%u\n",x,y,raw[y*16+1+x*3],raw[y*16+2+x*3],raw[y*16+3+x*3],expected[(y*5+x)*4],expected[(y*5+x)*4+1],expected[(y*5+x)*4+2]);
            assert(!memcmp(raw+y*16+1+x*3,expected+(y*5+x)*4,3)); } }
    tx_stream_png_free(p); assert(!live_bytes);
    w.maxTilesX=8400; w.maxTilesY=2400; peak_bytes=0;
    p=tx_stream_png_begin(&w,0,0,NULL,0,NULL,0); assert(p && peak_bytes<1100000);
    uint32_t total=0, strips=0;
    while(tx_stream_png_range(p,&first,&rows)==1) {
        assert(rows<=7);
        for(uint32_t x=0;x<8400;x++) assert(tx_stream_png_run(p,x,0,&t,2400));
        assert(tx_stream_png_finish_strip(p));
        uint32_t offset,n; const uint8_t* data;
        assert(tx_stream_png_pull(p,&offset,&data,&n) && offset==total && n<=524288);
        assert(peak_bytes<1100000); total+=n; strips++; assert(tx_stream_png_ack(p));
    }
    assert(strips==343 && total>0);
    tx_stream_png_free(p); assert(!live_bytes);
    printf("stream PNG: colors, strip-crossing markers, CRC/zlib, ack and bounded allocation passed (%zu bytes)\n",peak_bytes);
    return 0;
}
