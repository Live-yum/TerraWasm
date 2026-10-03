#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "terra_map_runtime.h"
#include "terra_stream_png.h"
#include "terra_stream.h"
#include "terra_task.h"
#include "terra_types.h"

extern void tx_set_world_open_count(uint32_t count);
extern uint32_t tx_get_world_open_count(void);
extern uint32_t txw_test_stream_begin_for_runtime(void);
extern void tx_reset_heap(void);
extern void tx_internal_free(void* p);
extern void buf_init(TxBuf* b, uint32_t cap);
extern void write_tile(TxWorld* world, TxBuf* out, const TxTile* tile, uint32_t same);
extern void tx_render_stream_color(TxWorld* world, const TxTile* tile, uint32_t y, uint8_t rgba[4]);
extern int tx_render_has_foreground(const TxTile* tile);
extern int txw_test_render_full_preview_rgb_to(TxWorld* world, uint8_t* rgb,
                                                uint32_t width, uint32_t height);
extern void txw_test_render_preview_rows_to(TxWorld* world, uint8_t* rgba,
                                             uint32_t width, uint32_t height);
extern void txw_test_stream_rgb_cache_run(TxWorld* world, uint8_t* rgb, uint32_t x,
                                           uint32_t y, const TxTile* tile, uint32_t run);
extern int txw_test_stream_png_pixel(TxStreamPng* png, uint32_t y, uint8_t rgba[4]);
extern uint32_t txw_test_map_runtime_value_for_tile(const TxTile* tile);
extern void txw_test_map_runtime_write_header(TxBuf* out, TxWorld* world);
extern void txw_test_map_runtime_render_color(const TxTile* tile, uint32_t y,
                                              uint32_t height, double ground, double rock,
                                              uint8_t rgba[4]);
extern uint32_t txw_test_map_runtime_background(uint32_t y, double ground, double rock);

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8u);
    p[2] = (uint8_t)(v >> 16u); p[3] = (uint8_t)(v >> 24u);
}
static uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8u)); }
static void entry(uint8_t* p, uint16_t base, uint8_t count) {
    p[0] = (uint8_t)base; p[1] = (uint8_t)(base >> 8u);
    p[2] = count; p[3] = 0u;
}

static uint8_t* make_runtime(uint32_t* out_len) {
    const uint32_t tile_count = 756u, wall_count = 3u, palette_count = 778u;
    const uint32_t len = 96u + (tile_count + wall_count) * 4u + palette_count * 4u + 90u;
    uint8_t* p = calloc(len, 1u);
    uint8_t* palette;
    assert(p);
    put32(p + 0u, TX_MAP_RUNTIME_MAGIC);
    put32(p + 4u, TX_MAP_RUNTIME_SCHEMA);
    put32(p + 8u, len);
    put32(p + 12u, TX_MAP_RUNTIME_FORMAT);
    put32(p + 16u, 326u);
    put32(p + 20u, tile_count);
    put32(p + 24u, wall_count);
    put32(p + 28u, palette_count);
    put32(p + 32u, 1u);    /* tile */
    put32(p + 36u, 4u);    /* wall */
    put32(p + 40u, 5u);    /* liquid */
    put32(p + 44u, 9u);    /* sky */
    put32(p + 48u, 265u);  /* dirt */
    put32(p + 52u, 521u);  /* rock */
    put32(p + 56u, 777u);  /* hell */
    put32(p + 60u, 30u);
    p[64] = 0x7au; /* release SHA */
    entry(p + 96u, 1u, 1u);
    entry(p + 96u + 755u * 4u, 2u, 2u);
    entry(p + 96u + (tile_count + 1u) * 4u, 4u, 1u);
    palette = p + 96u + (tile_count + wall_count) * 4u;
    for (uint32_t i = 1u; i < palette_count; i++) palette[i * 4u + 3u] = 255u;
    palette[2u * 4u] = 10u; palette[2u * 4u + 1u] = 20u; palette[2u * 4u + 2u] = 30u;
    palette[4u * 4u] = 50u; palette[4u * 4u + 1u] = 60u; palette[4u * 4u + 2u] = 70u;
    palette[5u * 4u] = 70u; palette[5u * 4u + 1u] = 80u; palette[5u * 4u + 2u] = 90u;
    palette[9u * 4u] = 1u; palette[9u * 4u + 1u] = 2u; palette[9u * 4u + 2u] = 3u;
    palette[11u * 4u] = 4u; palette[11u * 4u + 1u] = 5u; palette[11u * 4u + 2u] = 6u;
    palette[16u * 4u] = 7u; palette[16u * 4u + 1u] = 8u; palette[16u * 4u + 2u] = 9u;
    palette[36u * 4u] = 13u; palette[36u * 4u + 1u] = 14u; palette[36u * 4u + 2u] = 15u;
    palette[244u * 4u] = 10u; palette[244u * 4u + 1u] = 11u; palette[244u * 4u + 2u] = 12u;
    palette[265u * 4u] = 16u; palette[265u * 4u + 1u] = 17u; palette[265u * 4u + 2u] = 18u;
    palette[521u * 4u] = 19u; palette[521u * 4u + 1u] = 20u; palette[521u * 4u + 2u] = 21u;
    p[len - 90u] = 200u; /* paint 1 */
    *out_len = len;
    return p;
}

static void test_rle_colors(TxWorld* world, const TxTile* tile, int foreground) {
    enum { HEIGHT = 300 };
    TxBuf encoded;
    uint8_t full[HEIGHT * 3u], rows[HEIGHT * 4u], stream[HEIGHT * 3u];
    uint8_t expected[4], got[4];
    uint32_t start, count;
    TxStreamPng* png;
    memset(full, 0, sizeof(full));
    memset(rows, 0, sizeof(rows));
    memset(stream, 0, sizeof(stream));
    buf_init(&encoded, 32u);
    write_tile(world, &encoded, tile, HEIGHT - 1u);
    assert(encoded.ok);
    world->file = encoded.data;
    world->file_len = encoded.len;
    world->starts[1] = 0u;
    world->ends[1] = encoded.len;
    assert(tx_render_has_foreground(tile) == foreground);
    assert(txw_test_render_full_preview_rgb_to(world, full, 1u, HEIGHT));
    txw_test_render_preview_rows_to(world, rows, 1u, HEIGHT);
    txw_test_stream_rgb_cache_run(world, stream, 0u, 0u, tile, HEIGHT);
    png = tx_stream_png_begin(world, 0u, 0u, NULL, 0u, NULL, 0u);
    assert(png && tx_stream_png_range(png, &start, &count) == 1 && start == 0u && count > 1u);
    assert(tx_stream_png_run(png, 0u, 0u, tile, HEIGHT));
    for (uint32_t sample = 0u; sample < 3u; sample++) {
        uint32_t y = sample < 2u ? sample : 11u;
        tx_render_stream_color(world, foreground ? tile : NULL, y, expected);
        assert(memcmp(full + y * 3u, expected, 3u) == 0);
        assert(memcmp(rows + y * 4u, expected, 4u) == 0);
        assert(memcmp(stream + y * 3u, expected, 3u) == 0);
        assert(txw_test_stream_png_pixel(png, y, got) && memcmp(got, expected, 4u) == 0);
        if (!foreground && y == 11u) {
            assert(expected[0] == 13u && expected[1] == 14u && expected[2] == 15u);
        }
    }
    if (!foreground) assert(memcmp(full, full + 3u, 3u) != 0);
    tx_stream_png_free(png);
    tx_internal_free(encoded.data);
    world->file = NULL;
    world->file_len = 0u;
}

int main(int argc, char** argv) {
    uint32_t len;
    uint8_t* data = make_runtime(&len);
    uint8_t* changed = malloc(len);
    uint8_t rgba[4];
    TxTile tile = {0};
    TxWorld world = {0};
    TxBuf header;
    uint16_t base;
    uint8_t options;
    assert(changed);
    /* A parsed world can outlive the first TMRT install; active tasks cannot. */
    tx_set_world_open_count(1u);
    assert(txw_set_map_runtime_from_buffer(data, len) == 0);
    assert(tx_map_runtime_is_set());
    assert(txw_set_map_runtime_from_buffer(NULL, 0u) < 0);
    tx_set_world_open_count(0u);
    assert(txw_set_map_runtime_from_buffer(NULL, 0u) == 0);
    {
        uint8_t pending_input[16] = {0};
        uint32_t task_id = terra_world_open_begin(pending_input, sizeof(pending_input));
        assert(task_id && tx_get_world_open_count() == 0u);
        assert(txw_set_map_runtime_from_buffer(data, len) < 0);
        assert(!tx_map_runtime_is_set());
        assert(terra_world_open_cancel(task_id) == TERRAX_WORLD_STATUS_CANCELLED);
        assert(terra_world_task_close(task_id) == TERRAX_WORLD_STATUS_OK);
    }
    {
        uint32_t task_id = txw_test_stream_begin_for_runtime();
        assert(task_id && tx_get_world_open_count() == 0u);
        assert(txw_set_map_runtime_from_buffer(data, len) < 0);
        assert(!tx_map_runtime_is_set());
        assert(terra_world_stream_cancel(task_id) == 0);
        assert(terra_world_stream_close(task_id) == 0);
    }
    assert(txw_set_map_runtime_from_buffer(data, len) == 0);
    assert(tx_map_runtime_is_set());
    tx_reset_heap(); /* persistent copy survives ordinary heap reset */
    for (volatile uint32_t i = 0u; i < len; i++) data[i] = 0u;
    /* Source lifetime is independent of the native persistent copy. */
    assert(tx_map_runtime_lookup(0, 755u, &base, &options) && base == 2u && options == 2u);
    tile.active = 1u; tile.type = 755u;
    assert((txw_test_map_runtime_value_for_tile(&tile) & 65535u) == 2u);
    txw_test_map_runtime_render_color(&tile, 1u, 100u, 35u, 65u, rgba);
    assert(rgba[0] == 10u && rgba[1] == 20u && rgba[2] == 30u && rgba[3] == 255u);
    tile.tile_color = 1u;
    txw_test_map_runtime_render_color(&tile, 1u, 100u, 35u, 65u, rgba);
    assert(rgba[0] == 23u && rgba[1] == 0u && rgba[2] == 0u);
    memset(&tile, 0, sizeof(tile)); tile.wall = 1u;
    assert((txw_test_map_runtime_value_for_tile(&tile) & 65535u) == 4u);
    txw_test_map_runtime_render_color(&tile, 1u, 100u, 35u, 65u, rgba);
    assert(rgba[0] == 50u && rgba[1] == 60u && rgba[2] == 70u);
    memset(&tile, 0, sizeof(tile)); tile.liquid_amount = 255u; tile.liquid_type = 1u;
    txw_test_map_runtime_render_color(&tile, 1u, 100u, 35u, 65u, rgba);
    assert(rgba[0] == 70u && rgba[1] == 80u && rgba[2] == 90u);
    assert((txw_test_map_runtime_value_for_tile(&tile) >> 24u) == 0u);
    tile.liquid_type = 5u; tile.wall = 1u; tile.wall_color = 1u;
    assert((txw_test_map_runtime_value_for_tile(&tile) & 65535u) == 4u &&
           (txw_test_map_runtime_value_for_tile(&tile) >> 24u) == 1u);
    txw_test_map_runtime_render_color(&tile, 1u, 100u, 35u, 65u, rgba);
    assert(rgba[0] != 70u || rgba[1] != 80u || rgba[2] != 90u);
    memset(&tile, 0, sizeof(tile));
    txw_test_map_runtime_render_color(&tile, 0u, 100u, 35u, 65u, rgba);
    assert(rgba[0] == 1u && rgba[1] == 2u && rgba[2] == 3u);
    /* Official CalcSkyGradient: skyPosition + floor(255 * y / worldSurface).
     * MAP, buffered PNG and stream PNG keep the original fractional surface. */
    assert((txw_test_map_runtime_background(21u, 671.0, 900.0) & 65535u) == 16u);
    txw_test_map_runtime_render_color(&tile, 21u, 1000u, 671.0, 900.0, rgba);
    assert(rgba[0] == 7u && rgba[1] == 8u && rgba[2] == 9u);
    assert((txw_test_map_runtime_background(3u, 3.25, 900.0) & 65535u) == 244u);
    txw_test_map_runtime_render_color(&tile, 3u, 1000u, 3.25, 900.0, rgba);
    assert(rgba[0] == 10u && rgba[1] == 11u && rgba[2] == 12u);
    txw_test_map_runtime_render_color(&tile, 150u, 1000u, 100.5, 200.5, rgba);
    assert(rgba[0] == 16u && rgba[1] == 17u && rgba[2] == 18u);
    txw_test_map_runtime_render_color(&tile, 300u, 1000u, 100.5, 200.5, rgba);
    assert(rgba[0] == 19u && rgba[1] == 20u && rgba[2] == 21u);
    world.worldSurface = 3.25; world.rockLayer = 900.0; world.maxTilesY = 1000;
    tx_render_stream_color(&world, NULL, 3u, rgba);
    assert(rgba[0] == 10u && rgba[1] == 11u && rgba[2] == 12u);

    world.version = 326u;
    world.maxTilesX = 1;
    world.maxTilesY = 300;
    world.worldSurface = 100.5;
    world.rockLayer = 200;
    memset(&tile, 0, sizeof(tile)); tile.active = 1u; tile.type = 127u;
    test_rle_colors(&world, &tile, 0);
    memset(&tile, 0, sizeof(tile)); tile.wall = 2u; /* in-domain, zero options */
    test_rle_colors(&world, &tile, 0);
    memset(&tile, 0, sizeof(tile)); tile.active = 1u; tile.type = 127u; tile.wall = 1u;
    test_rle_colors(&world, &tile, 1);
    memset(&tile, 0, sizeof(tile)); tile.active = 1u; tile.type = 127u;
    tile.liquid_amount = 255u; tile.liquid_type = 1u;
    test_rle_colors(&world, &tile, 1);

    strcpy(world.worldName, "X");
    buf_init(&header, 256u);
    txw_test_map_runtime_write_header(&header, &world);
    assert(header.ok && get16(header.data + 38u) == 756u &&
           get16(header.data + 40u) == 3u && get16(header.data + 42u) == 4u);
    /* Tile 755 has two options; its high-domain bit and explicit count follow. */
    assert(header.data[50u + 94u] == 15u);
    tx_internal_free(header.data);

    free(data);
    data = make_runtime(&len);
    memcpy(changed, data, len);
    changed[96u + 755u * 4u + 3u] = 1u; /* reserved */
    assert(txw_set_map_runtime_from_buffer(changed, len) < 0);
    assert(tx_map_runtime_lookup(0, 755u, &base, &options) && base == 2u);
    memcpy(changed, data, len);
    put32(changed + 48u, 266u); /* broken contiguous gradient layout */
    assert(txw_set_map_runtime_from_buffer(changed, len) < 0);
    memcpy(changed, data, len);
    changed[96u + 755u * 4u] = 3u; /* lookup hole */
    assert(txw_set_map_runtime_from_buffer(changed, len) < 0);
    assert(txw_set_map_runtime_from_buffer(data, len - 1u) < 0);
    assert(txw_set_map_runtime(1u, len) < 0); /* unowned bridge pointer */
    memcpy(changed, data, len);
    changed[64] = 0x7bu;
    {
        uint8_t pending_input[16] = {0};
        uint32_t task_id = terra_world_open_begin(pending_input, sizeof(pending_input));
        assert(task_id && tx_get_world_open_count() == 0u);
        assert(txw_set_map_runtime_from_buffer(data, len) == 0); /* identical bytes */
        assert(txw_set_map_runtime_from_buffer(changed, len) < 0);
        assert(txw_set_map_runtime_from_buffer(NULL, 0u) < 0);
        assert(terra_world_open_cancel(task_id) == TERRAX_WORLD_STATUS_CANCELLED);
        assert(txw_set_map_runtime_from_buffer(changed, len) == 0);
        assert(txw_set_map_runtime_from_buffer(data, len) == 0);
        assert(terra_world_task_close(task_id) == TERRAX_WORLD_STATUS_OK);
    }
    {
        uint32_t task_id = txw_test_stream_begin_for_runtime();
        assert(task_id && tx_get_world_open_count() == 0u);
        assert(txw_set_map_runtime_from_buffer(data, len) == 0); /* identical bytes */
        assert(txw_set_map_runtime_from_buffer(changed, len) < 0);
        assert(txw_set_map_runtime_from_buffer(NULL, 0u) < 0);
        assert(terra_world_stream_cancel(task_id) == 0);
        assert(txw_set_map_runtime_from_buffer(changed, len) == 0);
        assert(terra_world_stream_close(task_id) == 0);
        assert(txw_set_map_runtime_from_buffer(data, len) == 0);
    }
    tx_set_world_open_count(1u);
    assert(txw_set_map_runtime_from_buffer(data, len) == 0);
    data[64] = 0x7bu;
    assert(txw_set_map_runtime_from_buffer(data, len) < 0);
    assert(txw_set_map_runtime_from_buffer(NULL, 0u) < 0);
    tx_set_world_open_count(0u);
    assert(txw_set_map_runtime_from_buffer(data, len) == 0);
    assert(txw_set_map_runtime_from_buffer(NULL, 0u) == 0);
    assert(!tx_map_runtime_is_set());
    free(data); free(changed);
    if (argc > 1) {
        FILE* file = fopen(argv[1], "rb");
        long size;
        uint8_t* real;
        const TxMapRuntimeLayout* layout;
        TxBuf compiled_header, runtime_header;
        buf_init(&compiled_header, 256u);
        txw_test_map_runtime_write_header(&compiled_header, &world);
        assert(file && fseek(file, 0, SEEK_END) == 0);
        size = ftell(file);
        assert(size > 0 && size <= TX_MAP_RUNTIME_MAX_BYTES && fseek(file, 0, SEEK_SET) == 0);
        real = malloc((size_t)size);
        assert(real && fread(real, 1u, (size_t)size, file) == (size_t)size);
        fclose(file);
        assert(txw_set_map_runtime_from_buffer(real, (uint32_t)size) == 0);
        layout = tx_map_runtime_layout();
        assert(layout && layout->cur_release == 326u && layout->tile_count == 754u &&
               layout->wall_count == 367u && layout->palette_count == 2195u &&
               layout->wall_pos == 1065u && layout->liquid_pos == 1422u &&
               layout->sky_pos == 1426u && layout->dirt_pos == 1682u &&
               layout->rock_pos == 1938u && layout->hell_pos == 2194u);
        assert(tx_map_runtime_lookup(1, 106u, &base, &options) && base == 1164u && options == 1u);
        assert(tx_map_runtime_color(base, rgba) &&
               rgba[0] == 148u && rgba[1] == 116u && rgba[2] == 74u && rgba[3] == 255u);
        buf_init(&runtime_header, 256u);
        txw_test_map_runtime_write_header(&runtime_header, &world);
        assert(compiled_header.ok && runtime_header.ok &&
               compiled_header.len == runtime_header.len &&
               memcmp(compiled_header.data, runtime_header.data, compiled_header.len) == 0);
        tx_internal_free(compiled_header.data);
        tx_internal_free(runtime_header.data);
        assert(txw_set_map_runtime_from_buffer(NULL, 0u) == 0);
        free(real);
        puts("real CDN TMRT fixture passed");
    }
    puts("TMRT layout, MAP header, PNG colors, ownership and replacement passed");
    return 0;
}
