/* The PNG task includes its implementation so this contract can disable the
 * optional cache without adding a public ABI switch. The oracle selects from
 * semantic chest fixtures and rasterizes independently of the chest decoder. */
#include "../src/terra_stream_png.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

extern void buf_init(TxBuf*, uint32_t);
extern void buf_u16le(TxBuf*, uint16_t);
extern void buf_u32le(TxBuf*, uint32_t);
extern void tx_internal_free(void*);
extern uint32_t tx_native_heap_used(void);
extern void txw_test_allocation_limit(uint32_t);

typedef struct Item { int32_t id; int16_t stack; uint8_t prefix; } Item;
typedef struct Chest { int32_t x, y; uint32_t count; Item items[5]; } Chest;
typedef struct Bytes { uint8_t* data; uint32_t size; } Bytes;
static TxWorld world;
static Chest chests[7];
static MapMarkerEntry markers[5];
static uint8_t atlas[12 * 4 * 4];
static TxBuf section_bytes;
static uint8_t backing_file[17];

static void setup_atlas(uint32_t variant) {
    memset(&world.icon_atlas, 0, sizeof(world.icon_atlas));
    world.icon_atlas.rgba = atlas; world.icon_atlas.icon_size = 4;
    world.icon_atlas.icon_count = 3; world.icon_atlas.atlas_width = 12; world.icon_atlas.atlas_height = 4;
    for (uint32_t i = 0; i < 3; i++) {
        world.icon_atlas.item_ids[i] = (i + 1) * 101; world.icon_atlas.x_offsets[i] = i * 4;
    }
    for (uint32_t i = 0; i < 48; i++) {
        atlas[i * 4] = (uint8_t)(i * 29 + variant * 71);
        atlas[i * 4 + 1] = (uint8_t)(i * 13 + variant * 37);
        atlas[i * 4 + 2] = (uint8_t)(i * 61 + variant * 17);
        atlas[i * 4 + 3] = (uint8_t[]){0, 1, 73, 128, 255}[i % 5];
    }
}

static void encode_chests(uint32_t version, int override, int reverse_slots) {
    if (section_bytes.data) tx_internal_free(section_bytes.data);
    memset(&section_bytes, 0, sizeof(section_bytes)); buf_init(&section_bytes, 512);
    buf_u16le(&section_bytes, 7); if (version < 294) buf_u16le(&section_bytes, 5);
    for (uint32_t i = 0; i < 7; i++) {
        Chest* chest = &chests[i];
        buf_u32le(&section_bytes, (uint32_t)chest->x); buf_u32le(&section_bytes, (uint32_t)chest->y);
        buf_u8(&section_bytes, 3); buf_bytes(&section_bytes, "box", 3);
        if (version >= 294) buf_u32le(&section_bytes, chest->count);
        uint32_t count = version < 294 ? 5 : chest->count;
        for (uint32_t slot = 0; slot < count; slot++) {
            Item item = chest->items[reverse_slots && i < 2 && slot < 2 ? 1 - slot : slot];
            buf_u16le(&section_bytes, (uint16_t)item.stack);
            if (item.stack) { buf_u32le(&section_bytes, (uint32_t)item.id); buf_u8(&section_bytes, item.prefix); }
        }
    }
    assert(section_bytes.ok); world.version = version;
    world.file = section_bytes.data; world.file_len = section_bytes.len;
    world.pointer_count = 3; world.starts[2] = 0; world.ends[2] = section_bytes.len;
    world.section_overrides[2].active = override;
    world.section_overrides[2].data = override ? section_bytes.data : NULL;
    world.section_overrides[2].len = override ? section_bytes.len : 0;
    if (override) {
        /* The original file has an empty chest section at an unrelated offset.
         * Ignoring the populated override must fail the independent oracle. */
        memset(backing_file, 0xa5, sizeof(backing_file));
        backing_file[13] = backing_file[14] = 0;
        backing_file[15] = 5; backing_file[16] = 0;
        world.file = backing_file; world.file_len = version < 294 ? 17 : 15;
        world.starts[2] = 13; world.ends[2] = world.file_len;
    }
}

static void setup(uint32_t version, int override) {
    world.maxTilesX = 137; world.maxTilesY = 41; world.worldSurface = 12; world.rockLayer = 27;
    memset(chests, 0, sizeof(chests));
    chests[0] = (Chest){0, 0, 2, {{202, 2, 1}, {101, 3, 2}}};
    chests[1] = (Chest){0, 0, 2, {{101, 4, 3}, {202, 5, 4}}};
    chests[2] = (Chest){63, 6, 5, {{999, 1, 0}, {202, 0, 0}, {0, 0, 0}, {101, 2, 1}, {202, 8, 0}}};
    chests[3] = (Chest){136, 40, 1, {{303, 1, 0}}};
    chests[4] = (Chest){-3, 52, 1, {{101, 1, 0}}};
    chests[5] = (Chest){68, 14, 1, {{999, 1, 0}}};
    chests[6] = (Chest){90, 9, 0, {{0}}};
    memset(markers, 0, sizeof(markers));
    for (uint32_t i = 0; i < 5; i++) {
        markers[i].id = i == 1 ? 202 : i == 4 ? 303 : 101;
        markers[i].icon_id = 9999; /* Chest icons use the actual matched item. */
        markers[i].locate = i == 0 ? 1 : 0;
        markers[i].radius = (uint8_t)(15 + i); markers[i].line_width = 2;
        markers[i].rgba[0] = (uint8_t)(40 + i * 43); markers[i].rgba[1] = (uint8_t)(230 - i * 27);
        markers[i].rgba[2] = (uint8_t)(31 + i * 41); markers[i].rgba[3] = (uint8_t[]){255, 73, 128, 255, 0}[i];
    }
    setup_atlas(0); encode_chests(version, override, 0);
}

static uint32_t coord(int32_t n, uint32_t source, uint32_t target) {
    if (n <= 0) return 0; if ((uint32_t)n >= source) return target - 1;
    return (uint32_t)((uint64_t)(uint32_t)n * target / source);
}
static uint32_t measure(uint32_t n, uint32_t width, uint32_t height) {
    if (!n) return 0;
    uint32_t a = (uint32_t)((uint64_t)n * width / world.maxTilesX);
    uint32_t b = (uint32_t)((uint64_t)n * height / world.maxTilesY);
    return a > b ? (a ? a : 1) : (b ? b : 1);
}
static void blend(uint8_t* pixel, const uint8_t* color) {
    if (!color[3]) return;
    for (uint32_t c = 0; c < 3; c++)
        pixel[c] = (uint8_t)((color[c] * color[3] + pixel[c] * (255 - color[3]) + 127) / 255);
    pixel[3] = 255;
}
static void fill(uint8_t* pixels, uint32_t width, uint32_t height, uint32_t channels) {
    for (uint32_t y = 0; y < height; y++) for (uint32_t x = 0; x < width; x++) {
        for (uint32_t c = 0; c < 3; c++) pixels[(y * width + x) * channels + c] = (uint8_t)(x * 11 + y * 17 + c * 39);
        if (channels == 4) pixels[(y * width + x) * channels + 3] = 255;
    }
}
static void reference(uint8_t* rgba, uint32_t width, uint32_t height, int reverse_slots) {
    for (uint32_t box = 0; box < 7; box++) {
        const Chest* chest = &chests[box]; const MapMarkerEntry* marker = NULL; int32_t item_id = 0;
        for (uint32_t slot = 0; slot < chest->count && !marker; slot++) {
            Item item = chest->items[reverse_slots && box < 2 && slot < 2 ? 1 - slot : slot];
            if (!item.stack) continue;
            for (uint32_t m = 0; m < 5; m++) if (!markers[m].locate && markers[m].id == item.id) {
                marker = &markers[m]; item_id = item.id; break;
            }
        }
        if (!marker) continue;
        int32_t cx = (int32_t)coord(chest->x, world.maxTilesX, width), cy = (int32_t)coord(chest->y, world.maxTilesY, height);
        uint32_t scaled_radius = measure(marker->radius, width, height), scaled_line = measure(marker->line_width, width, height);
        uint32_t radius = scaled_radius ? scaled_radius : 1, line = scaled_line ? scaled_line : 1;
        if (line > radius) line = radius; uint32_t inner = radius - line;
        for (uint32_t y = 0; y < height; y++) for (uint32_t x = 0; x < width; x++) {
            int64_t dx = (int64_t)x - cx, dy = (int64_t)y - cy;
            uint64_t d = (uint64_t)(dx * dx + dy * dy);
            if (d <= (uint64_t)radius * radius && d >= (uint64_t)inner * inner) blend(rgba + (y * width + x) * 4, marker->rgba);
        }
        int index = -1;
        for (uint32_t i = 0; world.icon_atlas.rgba && i < world.icon_atlas.icon_count; i++)
            if (world.icon_atlas.item_ids[i] == (uint32_t)item_id) { index = (int)i; break; }
        if (index < 0) continue;
        inner = scaled_radius > scaled_line ? scaled_radius - scaled_line : scaled_radius;
        uint32_t side = inner >= 2 ? inner * 707 / 1000 : 0;
        while (side && (uint64_t)side * side * 2 > (uint64_t)inner * inner) side--;
        for (uint32_t y = 0; y < side; y++) for (uint32_t x = 0; x < side; x++) {
            int32_t px = cx - (int32_t)(side / 2) + (int32_t)x, py = cy - (int32_t)(side / 2) + (int32_t)y;
            if (px < 0 || py < 0 || (uint32_t)px >= width || (uint32_t)py >= height) continue;
            uint32_t sx = world.icon_atlas.x_offsets[index] + x * world.icon_atlas.icon_size / side;
            uint32_t sy = world.icon_atlas.y_offsets[index] + y * world.icon_atlas.icon_size / side;
            blend(rgba + ((uint32_t)py * width + (uint32_t)px) * 4, world.icon_atlas.rgba + (sy * world.icon_atlas.atlas_width + sx) * 4);
        }
    }
}

static void rgba_contract(void) {
    const uint32_t dimensions[][2] = {{137, 41}, {49, 15}, {53, 23}, {1, 1}};
    const uint32_t strips[] = {1, 7, 32, 41};
    uint32_t baseline = tx_native_heap_used();
    const uint8_t* original_file = world.file; uint32_t original_length = world.file_len;
    TxPngChestCache* cache = tx_render_stream_chest_cache(&world, markers, 5); assert(cache);
    assert(world.file == original_file && world.file_len == original_length);
    assert(tx_native_heap_used() - baseline <= 128 * 1024);
    for (uint32_t d = 0; d < 4; d++) for (uint32_t a = 0; a < 3; a++) {
        setup_atlas(a); if (a == 2) world.icon_atlas.rgba = NULL;
        uint32_t width = dimensions[d][0], height = dimensions[d][1], bytes = width * height * 4;
        uint8_t *expected = malloc(bytes), *actual = malloc(bytes); assert(expected && actual);
        fill(expected, width, height, 4); reference(expected, width, height, 0);
        for (uint32_t s = 0; s < 4; s++) {
            fill(actual, width, height, 4);
            for (uint32_t start = 0; start < height; start += strips[s]) {
                uint32_t rows = height - start; if (rows > strips[s]) rows = strips[s];
                tx_render_stream_chest_cache_rows(&world, cache, actual + start * width * 4, width, height, start, rows, markers);
            }
            assert(!memcmp(expected, actual, bytes));
        }
        free(actual); free(expected);
    }
    tx_render_stream_chest_cache_free(cache); assert(tx_native_heap_used() == baseline);
    assert(world.file == original_file && world.file_len == original_length); setup_atlas(0);
}

static Bytes png(int mode, uint32_t strip_rows, int change_atlas) {
    uint32_t baseline = tx_native_heap_used(), first, rows, steps = 0;
    TxStreamPng* p = tx_stream_png_begin(&world, 0, 0, markers, 5, NULL, 0); assert(p);
    if (strip_rows && strip_rows < p->max_rows) p->max_rows = strip_rows;
    if (mode == 1) p->chest_cache_attempted = 1; /* Former uncached path. */
    uint8_t* rgb = malloc((size_t)world.maxTilesX * world.maxTilesY * 3); assert(rgb);
    fill(rgb, world.maxTilesX, world.maxTilesY, 3); Bytes result = {0}; setup_atlas(0);
    while (tx_stream_png_range(p, &first, &rows) == 1) {
        if (change_atlas && first >= 7) setup_atlas(1);
        assert(tx_stream_png_rgb(p, rgb) == 1);
        if (mode == 2 && !steps) txw_test_allocation_limit(0);
        assert(tx_stream_png_finish_strip(p));
        txw_test_allocation_limit(UINT32_MAX);
        assert(p->chest_cache_attempted);
        assert(mode ? !p->chest_cache : p->chest_cache != NULL);
        uint32_t offset, length; const uint8_t* bytes;
        assert(tx_stream_png_pull(p, &offset, &bytes, &length) && offset == result.size && length <= OUTPUT_CAP);
        result.data = realloc(result.data, result.size + length); assert(result.data);
        memcpy(result.data + result.size, bytes, length); result.size += length;
        assert(tx_stream_png_ack(p)); steps++;
    }
    assert(steps > 0); free(rgb); tx_stream_png_free(p); assert(tx_native_heap_used() == baseline);
    return result;
}
static void equal(const Bytes* a, const Bytes* b) { assert(a->size == b->size && !memcmp(a->data, b->data, a->size)); }
static uint32_t be(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void verify_png(const Bytes* image, int reverse_slots) {
    assert(be(image->data + 16) == (uint32_t)world.maxTilesX && be(image->data + 20) == (uint32_t)world.maxTilesY);
    assert(image->data[24] == 8 && image->data[25] == 2);
    uint32_t compressed_size = 0, width = world.maxTilesX, height = world.maxTilesY, stride = width * 3 + 1;
    uint8_t *compressed = malloc(image->size), *raw = malloc((size_t)stride * height), *expected = malloc((size_t)width * height * 4);
    assert(compressed && raw && expected);
    for (uint32_t offset = 8; offset < image->size;) {
        uint32_t length = be(image->data + offset); assert(length <= image->size - offset - 12);
        assert(be(image->data + offset + length + 8) == (uint32_t)crc32(0, image->data + offset + 4, length + 4));
        if (!memcmp(image->data + offset + 4, "IDAT", 4)) { memcpy(compressed + compressed_size, image->data + offset + 8, length); compressed_size += length; }
        offset += length + 12;
    }
    uLongf decoded = (uLongf)stride * height;
    assert(uncompress(raw, &decoded, compressed, compressed_size) == Z_OK && decoded == (uLongf)stride * height);
    fill(expected, width, height, 4); reference(expected, width, height, reverse_slots);
    for (uint32_t y = 0; y < height; y++) {
        assert(!raw[y * stride]);
        for (uint32_t x = 0; x < width; x++) assert(!memcmp(raw + y * stride + 1 + x * 3, expected + (y * width + x) * 4, 3));
    }
    free(expected); free(raw); free(compressed);
}

static void cancellation_contract(void) {
    uint32_t baseline = tx_native_heap_used();
    TxStreamPng* p = tx_stream_png_begin(&world, 0, 0, markers, 5, NULL, 0); assert(p);
    tx_stream_png_free(p); assert(tx_native_heap_used() == baseline);
    p = tx_stream_png_begin(&world, 0, 0, markers, 5, NULL, 0); assert(p); p->max_rows = 1;
    uint32_t first, rows; assert(tx_stream_png_range(p, &first, &rows) == 1); assert(tx_stream_png_finish_strip(p)); assert(p->chest_cache);
    tx_stream_png_free(p); assert(tx_native_heap_used() == baseline);
    txw_test_allocation_limit(0); assert(!tx_render_stream_chest_cache(&world, markers, 5));
    txw_test_allocation_limit(UINT32_MAX); assert(tx_native_heap_used() == baseline);
}

int main(void) {
    for (uint32_t version = 0; version < 2; version++) for (uint32_t override = 0; override < 2; override++) {
        setup(version ? 326 : 293, override); rgba_contract(); cancellation_contract();
        for (uint32_t strip = 1; strip <= 7; strip += 6) {
            Bytes cached = png(0, strip, 0), uncached = png(1, strip, 0), fallback = png(2, strip, 0);
            equal(&cached, &uncached); equal(&cached, &fallback); verify_png(&cached, 0);
            free(cached.data); free(uncached.data); free(fallback.data);
        }
        Bytes changed = png(0, 1, 1), changed_reference = png(1, 1, 1); equal(&changed, &changed_reference);
        free(changed.data); free(changed_reference.data);
        Bytes before = png(0, 7, 0); encode_chests(world.version, override, 1);
        Bytes reversed = png(0, 7, 0), reversed_reference = png(1, 7, 0); equal(&reversed, &reversed_reference); verify_png(&reversed, 1);
        assert(before.size != reversed.size || memcmp(before.data, reversed.data, before.size));
        free(before.data); free(reversed.data); free(reversed_reference.data);
    }
    setup(326, 0); for (uint32_t i = 0; i < 5; i++) markers[i].id = 4000 + i;
    Bytes no_match = png(0, 1, 0), no_match_reference = png(1, 1, 0); equal(&no_match, &no_match_reference); verify_png(&no_match, 0);
    free(no_match.data); free(no_match_reference.data);
    setup(326, 0); world.maxTilesX = 8400; world.maxTilesY = 29;
    Bytes wide = png(0, 0, 0), wide_reference = png(1, 0, 0); equal(&wide, &wide_reference); verify_png(&wide, 0);
    free(wide.data); free(wide_reference.data);
    /* Oversized declared container counts only disable the optional cache. */
    section_bytes.data[0] = 0x28; section_bytes.data[1] = 0x23; /* 9000 */
    uint32_t baseline = tx_native_heap_used(); assert(!tx_render_stream_chest_cache(&world, markers, 5)); assert(tx_native_heap_used() == baseline);
    /* Even an empty match list is a successful cache, avoiding later rescans. */
    section_bytes.data[0] = section_bytes.data[1] = 0;
    TxPngChestCache* empty = tx_render_stream_chest_cache(&world, markers, 5); assert(empty);
    tx_render_stream_chest_cache_free(empty); assert(tx_native_heap_used() == baseline);
    tx_internal_free(section_bytes.data); memset(&section_bytes, 0, sizeof(section_bytes)); assert(!tx_native_heap_used());
    puts("PNG chest cache: independent RGBA, exact full PNG bytes, first occupied match/order, alpha/clipping/scaling, legacy/modern/override metadata, live atlas replacement, 128 KiB cap, allocation fallback and cancellation passed");
    return 0;
}
