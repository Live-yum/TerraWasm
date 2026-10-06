#include "terra_circuit_twld.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

typedef union Allocation { size_t bytes; max_align_t alignment; } Allocation;
static size_t live_bytes, peak_bytes, live_allocations;
uint8_t* tx_persistent_alloc(uint32_t bytes) {
    Allocation* a = malloc(sizeof(Allocation) + bytes); if (!a) return NULL;
    a->bytes = bytes; live_bytes += bytes; ++live_allocations;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    return (uint8_t*)(a + 1);
}
void tx_persistent_free(void* data) {
    if (!data) return;
    Allocation* a = (Allocation*)data - 1;
    live_bytes -= a->bytes; --live_allocations; free(a);
}

typedef struct Check {
    uint32_t count, colors, min_x, min_y, max_x, max_y;
    uint32_t patch_count;
    FILE* output;
} Check;
static int32_t tile(void* context, const TerraTwldTile* tile) {
    Check* c = (Check*)context;
    ++c->count;
    if (!tile->color_pixel_box) return 0;
    ++c->colors;
    if (tile->x < c->min_x) c->min_x = tile->x;
    if (tile->y < c->min_y) c->min_y = tile->y;
    if (tile->x > c->max_x) c->max_x = tile->x;
    if (tile->y > c->max_y) c->max_y = tile->y;
    assert(tile->framed && tile->saved_type == 699);
    assert(tile->frame_x == 0 && tile->frame_y == 0);
    return 0;
}
static int32_t patch(void* context, TerraTwldTile* tile) {
    Check* c = (Check*)context;
    if (tile->color_pixel_box && tile->x == 7371 && tile->y == 1002) {
        tile->frame_x = 18; tile->frame_y = 36; ++c->patch_count;
    }
    return 0;
}
static int32_t output(void* context, const uint8_t* bytes, uint32_t count) {
    Check* c = (Check*)context;
    return fwrite(bytes, 1, count, c->output) == count ? 0 : -1;
}
static void replay(TerraTwld* t, const char* path) {
    FILE* file = fopen(path, "rb"); assert(file);
    uint8_t* buffer = malloc(65536); assert(buffer);
    uint32_t sizes[] = {1, 7, 13, 127, 16383, 65536};
    uint32_t round = 0;
    for (;;) {
        uint32_t requested = sizes[round < 6 ? round : 5]; ++round;
        uint32_t count = (uint32_t)fread(buffer, 1, requested, file);
        int32_t status = terra_twld_feed(t, buffer, count, count < requested);
        if (status < 0) fprintf(stderr, "TWLD feed failed %d at compressed round %u\n", status, round);
        assert(status >= 0);
        if (count < requested) { assert(status == 0); break; }
        assert(status == 1);
    }
    free(buffer); fclose(file);
}
int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "usage: circuit_twld_contract computerraria.twld output.twld\n"); return 2; }
    Check c = {0}; c.min_x = c.min_y = UINT32_MAX;
    TerraTwldCallbacks callbacks = {.tile = tile, .patch = patch, .output = output};
    TerraTwld* t = NULL;
    assert(terra_twld_create(15200, 7200, 2u * 1024u * 1024u, &callbacks, &c, &t) == 0);
    replay(t, argv[1]);
    TerraTwldStats s; terra_twld_stats(t, &s);
    assert(s.pass == 1 && s.map_entries == 1 && s.expanded_bytes == 437852040u);
    assert(c.count == 0 && s.peak_bytes < 1024u * 1024u);
    assert(terra_twld_rewind(t, 0) == 0);
    replay(t, argv[1]);
    terra_twld_stats(t, &s);
    assert(s.pass == 2 && s.complete && s.mod_tiles == 16896 && s.color_pixels == 16896);
    assert(s.training_dummies == 121 && s.logic_sensors == 4);
    assert(c.count == 16896 && c.colors == 16896);
    assert(c.min_x == 7371 && c.max_x == 7546 && c.min_y == 1002 && c.max_y == 1097);
    c.output = fopen(argv[2], "wb"); assert(c.output);
    assert(terra_twld_rewind(t, 1) == 0);
    replay(t, argv[1]); fclose(c.output); c.output = NULL;
    terra_twld_stats(t, &s);
    assert(c.patch_count == 1 && s.peak_bytes < 1024u * 1024u);
    printf("TWLD: %llu raw bytes streamed, %llu mod pixels, bounds 7371,1002..7546,1097; one pixel patched; peak %u bytes\n",
           (unsigned long long)s.expanded_bytes, (unsigned long long)s.color_pixels, s.peak_bytes);
    terra_twld_destroy(t);
    assert(!live_bytes && !live_allocations);
    return 0;
}
