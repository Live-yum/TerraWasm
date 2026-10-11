/* Independent brute-force rasterization: strip culling must preserve every
 * byte for overlapping markers, clipping, alpha and scaled previews. */
#include "terra_types.h"
#include "terra_map.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void txw_test_render_entity_rows(TxWorld*, uint8_t*, uint32_t, uint32_t,
    uint32_t, uint32_t, const MapMarkerEntry*, const TxBuf*);

static uint32_t coord(int32_t n, uint32_t source, uint32_t target) {
    if (n <= 0) return 0;
    if ((uint32_t)n >= source) return target - 1;
    return (uint32_t)((uint64_t)(uint32_t)n * target / source);
}
static uint32_t measure(uint32_t n, uint32_t ww, uint32_t wh, uint32_t pw, uint32_t ph) {
    if (!n) return 0;
    uint32_t x = (uint32_t)((uint64_t)n * pw / ww), y = (uint32_t)((uint64_t)n * ph / wh);
    return x > y ? (x ? x : 1) : (y ? y : 1);
}
static void blend(uint8_t* pixel, const uint8_t* color) {
    for (unsigned channel = 0; channel < 3; channel++)
        pixel[channel] = (uint8_t)((color[channel] * color[3] + pixel[channel] * (255 - color[3]) + 127) / 255);
    pixel[3] = 255;
}
static void reference(TxWorld* world, uint8_t* image, uint32_t width, uint32_t height,
                      const MapMarkerEntry* markers, const TxMarkerPoint* points, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        const MapMarkerEntry* marker = &markers[points[i].marker_index];
        int32_t cx = (int32_t)coord(points[i].x, world->maxTilesX, width);
        int32_t cy = (int32_t)coord(points[i].y, world->maxTilesY, height);
        uint32_t scaled_radius = measure(marker->radius, world->maxTilesX, world->maxTilesY, width, height);
        uint32_t scaled_line = measure(marker->line_width, world->maxTilesX, world->maxTilesY, width, height);
        uint32_t radius = scaled_radius ? scaled_radius : 1, line = scaled_line ? scaled_line : 1;
        if (line > radius) line = radius;
        uint32_t inner = radius - line;
        for (uint32_t y = 0; y < height; y++) for (uint32_t x = 0; x < width; x++) {
            int64_t dx = (int64_t)x - cx, dy = (int64_t)y - cy;
            uint64_t distance = (uint64_t)(dx * dx + dy * dy);
            if (distance <= (uint64_t)radius * radius && distance >= (uint64_t)inner * inner)
                blend(image + (y * width + x) * 4, marker->rgba);
        }
        int index = -1;
        for (uint32_t j = 0; world->icon_atlas.rgba && j < world->icon_atlas.icon_count; j++)
            if (world->icon_atlas.item_ids[j] == (uint32_t)marker->icon_id) { index = (int)j; break; }
        if (index < 0) continue;
        inner = scaled_radius > scaled_line ? scaled_radius - scaled_line : scaled_radius;
        uint32_t side = inner >= 2 ? inner * 707 / 1000 : 0;
        while (side && (uint64_t)side * side * 2 > (uint64_t)inner * inner) side--;
        for (uint32_t y = 0; y < side; y++) for (uint32_t x = 0; x < side; x++) {
            int32_t px = cx - (int32_t)(side / 2) + (int32_t)x, py = cy - (int32_t)(side / 2) + (int32_t)y;
            if (px < 0 || py < 0 || (uint32_t)px >= width || (uint32_t)py >= height) continue;
            uint32_t sx = world->icon_atlas.x_offsets[index] + x * world->icon_atlas.icon_size / side;
            uint32_t sy = world->icon_atlas.y_offsets[index] + y * world->icon_atlas.icon_size / side;
            const uint8_t* color = world->icon_atlas.rgba + (sy * world->icon_atlas.atlas_width + sx) * 4;
            if (color[3]) blend(image + ((uint32_t)py * width + (uint32_t)px) * 4, color);
        }
    }
}

int main(void) {
    TxWorld world = {0}; world.maxTilesX = 137; world.maxTilesY = 113;
    uint8_t atlas[8 * 4 * 4];
    for (uint32_t i = 0; i < sizeof(atlas); i++) atlas[i] = (uint8_t)(i * 37 + 11);
    for (uint32_t i = 0; i < 32; i++) atlas[i * 4 + 3] = (uint8_t[]){0, 73, 255}[i % 3];
    world.icon_atlas.icon_size = 4; world.icon_atlas.icon_count = 2;
    world.icon_atlas.atlas_width = 8; world.icon_atlas.atlas_height = 4;
    world.icon_atlas.item_ids[0] = 1001; world.icon_atlas.item_ids[1] = 1002;
    world.icon_atlas.x_offsets[1] = 4;
    MapMarkerEntry markers[32] = {0}; TxMarkerPoint entries[96];
    const uint8_t radii[] = {0, 1, 2, 7, 30, 60, 127, 255}, lines[] = {0, 1, 3, 15};
    for (uint32_t i = 0; i < 32; i++) {
        markers[i].radius = radii[i % 8]; markers[i].line_width = lines[i % 4];
        markers[i].icon_id = i % 3 == 0 ? -1 : 1001 + i % 2;
        markers[i].rgba[0] = (uint8_t)(i * 17); markers[i].rgba[1] = (uint8_t)(i * 53);
        markers[i].rgba[2] = (uint8_t)(i * 71); markers[i].rgba[3] = (uint8_t[]){0, 73, 255}[i % 3];
    }
    for (uint32_t i = 0; i < 96; i++) entries[i] = (TxMarkerPoint){(int32_t)(i * 19 % 151) - 7, (int32_t)(i * 31 % 127) - 7, i % 32};
    TxBuf points = {(uint8_t*)entries, sizeof(entries), sizeof(entries), 1};
    const uint32_t sizes[][2] = {{137, 113}, {53, 43}, {53, 77}, {1, 1}};
    uint32_t cases = 0;
    for (uint32_t icons = 0; icons < 2; icons++) for (uint32_t size = 0; size < 4; size++) {
        world.icon_atlas.rgba = icons ? atlas : NULL;
        uint32_t width = sizes[size][0], height = sizes[size][1], bytes = width * height * 4;
        uint8_t* expected = malloc(bytes), *actual = malloc(bytes); assert(expected && actual);
        for (uint32_t i = 0; i < bytes; i++) expected[i] = (uint8_t)(i * 11 + 29);
        reference(&world, expected, width, height, markers, entries, 96);
        const uint32_t strips[] = {1, 7, 32, 113};
        for (uint32_t s = 0; s < 4; s++) {
            for (uint32_t i = 0; i < bytes; i++) actual[i] = (uint8_t)(i * 11 + 29);
            for (uint32_t start = 0; start < height; start += strips[s]) {
                uint32_t rows = height - start; if (rows > strips[s]) rows = strips[s];
                txw_test_render_entity_rows(&world, actual + start * width * 4, width, height, start, rows, markers, &points);
            }
            assert(!memcmp(expected, actual, bytes)); cases++;
        }
        free(actual); free(expected);
    }
    printf("Marker strip rasterization matches independent RGBA reference in %u overlap/alpha/clip/scale cases\n", cases);
    return 0;
}
