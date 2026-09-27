#include "terra_stream_png.h"
#include "terra_output.h"
extern void* memset(void*, int, unsigned long);
extern void* memcpy(void*, const void*, unsigned long);
extern void buf_u8(TxBuf*, uint8_t);
extern void buf_bytes(TxBuf*, const void*, uint32_t);
extern void buf_u32be(TxBuf*, uint32_t);
extern unsigned long crc32(unsigned long, const uint8_t*, unsigned int);
extern unsigned long adler32(unsigned long, const uint8_t*, unsigned int);
extern void tx_set_error(const char*, const char*);
extern void tx_render_stream_color(TxWorld*, const TxTile*, uint32_t, uint8_t*);
extern void tx_render_stream_markers(TxWorld*, uint8_t*, uint32_t, uint32_t,
    uint32_t, uint32_t, const MapMarkerEntry*, uint32_t,
    const MapMarkerEntry*, uint32_t, uint32_t);
extern void tx_render_stream_tile_marker(TxWorld*, uint8_t*, uint32_t, uint32_t,
    uint32_t, uint32_t, uint32_t, uint32_t, const TxTile*, uint32_t,
    const MapMarkerEntry*, uint32_t);
extern void tx_render_stream_fixed_block(TxBuf*, uint32_t*, uint32_t*,
    const uint8_t*, uint32_t, uint32_t);
extern void tx_render_stream_finish_bits(TxBuf*, uint32_t*, uint32_t*);

#define RAW_CAP (256u * 1024u)
#define OUTPUT_CAP (512u * 1024u)
struct TxStreamPng {
    TxWorld* world;
    const MapMarkerEntry *chests, *tiles;
    uint32_t chest_count, tile_count, width, height, start, rows, max_rows;
    uint32_t offset, bitbuf, bitcnt, adler, phase, legacy, active, done;
    uint8_t *rgba, *raw;
    TxBuf output;
};
static int fail(void) {
    tx_set_error("TERRAX_STREAM_PNG_STATE", "invalid streaming PNG state or range");
    return 0;
}
static void chunk(TxBuf* b, const char* type, const uint8_t* bytes, uint32_t n) {
    buf_u32be(b, n); buf_bytes(b, type, 4u); buf_bytes(b, bytes, n);
    uint32_t crc = (uint32_t)crc32(0u, (const uint8_t*)type, 4u);
    if (n) crc = (uint32_t)crc32(crc, bytes, n);
    buf_u32be(b, crc);
}
void tx_stream_png_free(TxStreamPng* p) {
    if (!p) return;
    tx_persistent_free(p->rgba); tx_persistent_free(p->raw);
    tx_persistent_free(p->output.data); tx_persistent_free(p);
}
TxStreamPng* tx_stream_png_begin(TxWorld* w, uint32_t mw, uint32_t mh,
    const MapMarkerEntry* chests, uint32_t nc, const MapMarkerEntry* tiles, uint32_t nt) {
    if (!w || w->maxTilesX <= 0 || w->maxTilesY <= 0 ||
        (nc && !chests) || (nt && !tiles) ||
        (mw && mw < (uint32_t)w->maxTilesX) || (mh && mh < (uint32_t)w->maxTilesY)) {
        fail(); return NULL;
    }
    uint64_t stride = (uint64_t)(uint32_t)w->maxTilesX * 4u + 1u;
    if (stride > RAW_CAP) { fail(); return NULL; }
    TxStreamPng* p = (TxStreamPng*)tx_persistent_alloc(sizeof(*p));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p)); p->world = w;
    p->width = (uint32_t)w->maxTilesX; p->height = (uint32_t)w->maxTilesY;
    p->max_rows = RAW_CAP / (uint32_t)stride;
    if (p->max_rows > 128u) p->max_rows = 128u;
    p->chests = chests; p->chest_count = nc; p->tiles = tiles; p->tile_count = nt;
    for (uint32_t i = 0; i < nt; i++) p->legacy |= tiles[i].locate == 0;
    p->rgba = tx_persistent_alloc(p->width * 4u * p->max_rows);
    p->raw = tx_persistent_alloc((uint32_t)stride * p->max_rows);
    p->output.data = tx_persistent_alloc(OUTPUT_CAP);
    if (!p->rgba || !p->raw || !p->output.data) { tx_stream_png_free(p); return NULL; }
    p->output.cap = OUTPUT_CAP; p->output.ok = 1; p->adler = 1;
    return p;
}
int tx_stream_png_range(TxStreamPng* p, uint32_t* y, uint32_t* rows) {
    if (!p || !y || !rows || p->output.len) return -1;
    if (p->done) return 0;
    if (!p->active) {
        p->rows = p->height - p->start;
        if (p->rows > p->max_rows) p->rows = p->max_rows;
        for (uint32_t r = 0; r < p->rows; r++) {
            uint8_t c[4]; tx_render_stream_color(p->world, NULL, p->start + r, c);
            for (uint32_t x = 0; x < p->width; x++)
                memcpy(p->rgba + (r * p->width + x) * 4u, c, 4u);
        }
        p->active = 1;
    }
    *y = p->start; *rows = p->rows; return 1;
}
int tx_stream_png_run(TxStreamPng* p, uint32_t x, uint32_t y, const TxTile* t, uint32_t run) {
    if (!p || !p->active || p->output.len || !t || x >= p->width ||
        y >= p->height || !run || run > p->height - y) return fail();
    if (p->phase) {
        tx_render_stream_tile_marker(p->world, p->rgba, p->width, p->height,
            p->start, p->rows, x, y, t, run, p->tiles, p->tile_count);
        return 1;
    }
    if (!(t->active && !t->invisible_block) && !(t->liquid_amount && t->liquid_type)
        && !(t->wall && !t->invisible_wall)) return 1;
    uint32_t first = y > p->start ? y : p->start;
    uint32_t end = y + run < p->start + p->rows ? y + run : p->start + p->rows;
    uint8_t c[4]; tx_render_stream_color(p->world, t, y, c);
    for (uint32_t yy = first; yy < end; yy++)
        memcpy(p->rgba + ((yy - p->start) * p->width + x) * 4u, c, 4u);
    return 1;
}
int tx_stream_png_rgb(TxStreamPng* p, const uint8_t* rgb) {
    if(!p||!rgb||!p->active||p->output.len)return -1;
    if(p->phase)return 0;
    const uint8_t* src=rgb+(uint64_t)p->start*p->width*3;
    for(uint32_t i=0;i<p->rows*p->width;i++){
        memcpy(p->rgba+i*4,src+i*3,3);p->rgba[i*4+3]=255;
    }
    return 1;
}
int tx_stream_png_finish_strip(TxStreamPng* p) {
    if (!p || !p->active || p->output.len) return fail();
    if (!p->phase) {
        tx_render_stream_markers(p->world, p->rgba, p->width, p->height,
            p->start, p->rows, p->chests, p->chest_count, p->tiles, p->tile_count, 0u);
        if (p->legacy) { p->phase = 1; return 1; }
    }
    tx_render_stream_markers(p->world, p->rgba, p->width, p->height,
        p->start, p->rows, p->chests, p->chest_count, p->tiles, p->tile_count, 1u);
    TxBuf* out = &p->output;
    if (!p->start) {
        const uint8_t sig[8] = {137,80,78,71,13,10,26,10};
        uint8_t ihdr[13] = {0};
        ihdr[0] = p->width >> 24; ihdr[1] = p->width >> 16;
        ihdr[2] = p->width >> 8; ihdr[3] = p->width;
        ihdr[4] = p->height >> 24; ihdr[5] = p->height >> 16;
        ihdr[6] = p->height >> 8; ihdr[7] = p->height;
        ihdr[8] = 8; ihdr[9] = 2;
        buf_bytes(out, sig, 8); chunk(out, "IHDR", ihdr, 13);
    }
    uint32_t prefix = out->len;
    buf_u32be(out, 0); buf_bytes(out, "IDAT", 4);
    uint32_t payload = out->len;
    if (!p->start) { buf_u8(out, 0x78); buf_u8(out, 0x01); }
    uint32_t stride = p->width * 3u + 1u, n = stride * p->rows;
    for (uint32_t r = 0; r < p->rows; r++) {
        p->raw[r * stride] = 0;
        for (uint32_t x = 0; x < p->width; x++)
            memcpy(p->raw + r * stride + 1u + x * 3u,
                p->rgba + (r * p->width + x) * 4u, 3u);
    }
    p->adler = (uint32_t)adler32(p->adler, p->raw, n);
    uint32_t last = p->start + p->rows == p->height;
    tx_render_stream_fixed_block(out, &p->bitbuf, &p->bitcnt, p->raw, n, last);
    if (last) { tx_render_stream_finish_bits(out, &p->bitbuf, &p->bitcnt); buf_u32be(out, p->adler); }
    uint32_t len = out->len - payload;
    out->data[prefix] = len >> 24; out->data[prefix+1] = len >> 16;
    out->data[prefix+2] = len >> 8; out->data[prefix+3] = len;
    buf_u32be(out, (uint32_t)crc32(0u, out->data + prefix + 4u, len + 4u));
    if (last) chunk(out, "IEND", NULL, 0);
    if (!out->ok || out->len > OUTPUT_CAP || out->len > UINT32_MAX - p->offset) return fail();
    p->done = last; p->active = 0; p->phase = 0; return 1;
}
int tx_stream_png_pull(TxStreamPng* p, uint32_t* offset, const uint8_t** bytes, uint32_t* n) {
    if (!p || !offset || !bytes || !n) return fail();
    if (!p->output.len) return 0;
    *offset = p->offset; *bytes = p->output.data; *n = p->output.len; return 1;
}
int tx_stream_png_ack(TxStreamPng* p) {
    if (!p || !p->output.len) return fail();
    p->offset += p->output.len; p->output.len = 0;
    p->start += p->rows; return 1;
}
