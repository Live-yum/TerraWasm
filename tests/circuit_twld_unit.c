#include "terra_circuit_twld.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

typedef union Allocation { size_t bytes; max_align_t alignment; } Allocation;
static size_t live_bytes, live_allocations, allocations, fail_after = SIZE_MAX;
uint8_t* tx_persistent_alloc(uint32_t bytes) {
    if (allocations++ == fail_after) return NULL;
    Allocation* a = malloc(sizeof(Allocation) + bytes); if (!a) return NULL;
    a->bytes = bytes; live_bytes += bytes; ++live_allocations;
    return (uint8_t*)(a + 1);
}
void tx_persistent_free(void* data) {
    if (!data) return;
    Allocation* a = (Allocation*)data - 1;
    live_bytes -= a->bytes; --live_allocations; free(a);
}
typedef struct Buffer { uint8_t bytes[8192]; uint32_t count; } Buffer;
static void add(Buffer* b, const void* p, uint32_t n) { assert(b->count + n <= sizeof(b->bytes)); memcpy(b->bytes + b->count, p, n); b->count += n; }
static void byte(Buffer* b, uint8_t x) { add(b, &x, 1); }
static void be32(Buffer* b, uint32_t x) { byte(b, x >> 24); byte(b, x >> 16); byte(b, x >> 8); byte(b, x); }
static void string(Buffer* b, const char* s) { size_t n = strlen(s); byte(b, n >> 8); byte(b, n); add(b, s, (uint32_t)n); }
static void tag(Buffer* b, uint8_t type, const char* name) { byte(b, type); string(b, name); }
static void strtag(Buffer* b, const char* name, const char* value) { tag(b, 8, name); string(b, value); }
static void inttag(Buffer* b, const char* name, uint32_t value) { tag(b, 3, name); be32(b, value); }
static void map_entry(Buffer* b, uint32_t id, const char* mod, const char* name, uint8_t framed) {
    inttag(b, "value", id); strtag(b, "mod", mod); strtag(b, "name", name);
    tag(b, 1, "framed"); byte(b, framed); strtag(b, "unknown-map-data", "retained verbatim"); byte(b, 0);
}
static Buffer fixture(uint32_t* frame_offset, uint32_t* array_length) {
    Buffer b = {0}; tag(&b, 10, "");
    strtag(&b, "future-header", "unknown metadata is retained");
    tag(&b, 10, "tiles");
    tag(&b, 7, "tileData"); *array_length = b.count; be32(&b, 14);
    uint8_t data[] = {0,0, 0xbb,0x02,0,18,0,36,0, 0xbc,0x02,7, 0,0};
    *frame_offset = b.count + 5; add(&b, data, sizeof(data));
    tag(&b, 9, "tileMap"); byte(&b, 10); be32(&b, 2);
    map_entry(&b, 699, "WireHead", "ColorPixelBox", 1);
    map_entry(&b, 700, "UnknownMod", "NonFramedTile", 0);
    tag(&b, 7, "wallData"); be32(&b, 8); for (uint32_t i=0;i<8;++i) byte(&b, 0);
    tag(&b, 9, "wallMap"); byte(&b, 10); be32(&b, 0); byte(&b, 0);
    tag(&b, 9, "tileEntities"); byte(&b, 10); be32(&b, 1);
    strtag(&b, "mod", "Terraria"); strtag(&b, "name", "TETrainingDummy");
    inttag(&b, "X", 1); inttag(&b, "Y", 1); byte(&b, 0);
    tag(&b, 11, "future-int-array"); be32(&b, 2); be32(&b, 0xdeadbeef); be32(&b, 0x10203040);
    tag(&b, 12, "future-long-array"); be32(&b, 1); be32(&b, 0xabcdef00); be32(&b, 0x76543210);
    tag(&b, 9, "nested-list"); byte(&b, 9); be32(&b, 1); byte(&b, 8); be32(&b, 2); string(&b, "a"); string(&b, "b");
    byte(&b, 0); return b;
}
static Buffer gzip_bytes(const Buffer* raw) {
    Buffer out = {0}; z_stream z = {0};
    assert(deflateInit2(&z, 6, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    z.next_in = (Bytef*)raw->bytes; z.avail_in = raw->count;
    z.next_out = out.bytes; z.avail_out = sizeof(out.bytes);
    assert(deflate(&z, Z_FINISH) == Z_STREAM_END); out.count = sizeof(out.bytes) - z.avail_out;
    assert(deflateEnd(&z) == Z_OK); return out;
}
static Buffer gunzip_bytes(const Buffer* gzip) {
    Buffer out = {0}; z_stream z = {0}; assert(inflateInit2(&z, 31) == Z_OK);
    z.next_in = (Bytef*)gzip->bytes; z.avail_in = gzip->count;
    z.next_out = out.bytes; z.avail_out = sizeof(out.bytes);
    assert(inflate(&z, Z_FINISH) == Z_STREAM_END); out.count = sizeof(out.bytes) - z.avail_out;
    assert(inflateEnd(&z) == Z_OK); return out;
}
typedef struct Check { uint32_t tiles, colors, entities, patches, fail_patch; Buffer output; } Check;
static int32_t visit(void* context, const TerraTwldTile* tile) {
    Check* c = context; ++c->tiles;
    if (tile->color_pixel_box) {
        ++c->colors; assert(tile->x == 0 && tile->y == 1 && tile->saved_type == 699);
        assert(tile->frame_x == 18 && tile->frame_y == 36 && tile->framed);
    } else { assert(tile->saved_type == 700 && !tile->framed && tile->paint == 7); }
    return 0;
}
static int32_t entity(void* context, const TerraTwldEntity* e) {
    Check* c = context; ++c->entities;
    assert(e->x == 1 && e->y == 1 && e->kind == TERRA_TWLD_TRAINING_DUMMY); return 0;
}
static int32_t patch(void* context, TerraTwldTile* tile) {
    Check* c = context; ++c->patches;
    if (c->fail_patch) return -1;
    if (tile->color_pixel_box) { tile->frame_x = 36; tile->frame_y = 54; } return 0;
}
static int32_t output(void* context, const uint8_t* bytes, uint32_t count) { add(&((Check*)context)->output, bytes, count); return 0; }
static const TerraTwldCallbacks callbacks = {.tile = visit, .patch = patch, .output = output, .entity = entity};
static int32_t feed(TerraTwld* t, const Buffer* b, uint32_t quantum) {
    for (uint32_t i = 0; i < b->count;) {
        uint32_t n = b->count - i; if (n > quantum) n = quantum;
        int32_t status = terra_twld_feed(t, b->bytes + i, n, 0); if (status < 0) return status;
        status = terra_twld_feed(t, NULL, 0, 0); if (status < 0) return status;
        i += n;
    }
    return terra_twld_feed(t, NULL, 0, 1);
}
static void destroy(TerraTwld* t) { terra_twld_destroy(t); assert(!live_bytes && !live_allocations); }

static void assert_output(const Check* c, const Buffer* expected) {
    Buffer actual = gunzip_bytes(&c->output);
    assert(actual.count == expected->count && !memcmp(actual.bytes, expected->bytes, actual.count));
}

static void abort_and_restart(const Buffer* compressed, const Buffer* expected) {
    Check c = {0}; TerraTwld* t = NULL; TerraTwldStats s;
    assert(terra_twld_abort_replay(NULL) == TERRA_TWLD_INVALID);
    assert(terra_twld_create(2, 2, 0, &callbacks, &c, &t) == 0);
    assert(terra_twld_abort_replay(t) == TERRA_TWLD_STATE);
    assert(feed(t, compressed, 7) == 0);
    assert(terra_twld_abort_replay(t) == 0); /* Successful metadata is unchanged. */
    terra_twld_stats(t, &s); assert(s.complete && s.pass == 1 && s.map_entries == 2);
    assert(terra_twld_rewind(t, 1) == 0);
    uint32_t cursor = 0;
    while (!c.patches && cursor < compressed->count) {
        assert(terra_twld_feed(t, compressed->bytes + cursor++, 1, 0) == TERRA_TWLD_MORE);
    }
    assert(c.patches && cursor < compressed->count && c.output.count);
    size_t allocation_count = allocations;
    fail_after = allocations;
    assert(terra_twld_abort_replay(t) == 0 && allocations == allocation_count);
    fail_after = SIZE_MAX;
    terra_twld_stats(t, &s); assert(!s.complete && s.pass == 2 && s.map_entries == 2);
    assert(terra_twld_feed(t, compressed->bytes, 1, 0) == TERRA_TWLD_STATE);
    memset(&c, 0, sizeof(c)); /* The owner discards staged output/partial visits. */
    assert(terra_twld_rewind(t, 1) == 0 && feed(t, compressed, 3) == 0);
    assert(c.tiles == 2 && c.patches == 2); assert_output(&c, expected);

    /* Neither abort nor a failed replay may adopt a replacement source CRC. */
    assert(terra_twld_rewind(t, 1) == 0); memset(&c, 0, sizeof(c));
    Buffer changed = *compressed; changed.bytes[4] ^= 1;
    assert(feed(t, &changed, 1) == TERRA_TWLD_CHANGED);
    assert(terra_twld_abort_replay(t) == 0); memset(&c, 0, sizeof(c));
    assert(terra_twld_rewind(t, 1) == 0 && feed(t, compressed, 5) == 0);
    assert_output(&c, expected);

    assert(terra_twld_rewind(t, 1) == 0); memset(&c, 0, sizeof(c)); c.fail_patch = 1;
    assert(feed(t, compressed, 1) == TERRA_TWLD_CALLBACK);
    assert(terra_twld_abort_replay(t) == 0); memset(&c, 0, sizeof(c));
    assert(terra_twld_rewind(t, 1) == 0 && feed(t, compressed, 5) == 0);
    assert_output(&c, expected); destroy(t);
}

static void replay_admission_is_retryable(const Buffer* compressed, const Buffer* expected) {
    Check c = {0}; TerraTwld* t = NULL; TerraTwldStats s;
    assert(terra_twld_create(2, 2, 0, &callbacks, &c, &t) == 0);
    assert(feed(t, compressed, 7) == 0); terra_twld_stats(t, &s);
    assert(terra_twld_set_budget(t, s.allocated_bytes) == 0);
    assert(terra_twld_rewind(t, 1) == TERRA_TWLD_LIMIT);
    assert(terra_twld_feed(t, compressed->bytes, 1, 0) == TERRA_TWLD_STATE);
    assert(terra_twld_set_budget(t, 0) == 0);
    assert(terra_twld_rewind(t, 1) == 0 && feed(t, compressed, 1) == 0);
    assert_output(&c, expected);
    assert(terra_twld_rewind(t, 1) == 0); memset(&c, 0, sizeof(c));
    assert(terra_twld_set_budget(t, 1) == TERRA_TWLD_LIMIT);
    assert(terra_twld_feed(t, compressed->bytes, 1, 0) == TERRA_TWLD_LIMIT);
    assert(terra_twld_abort_replay(t) == 0);
    assert(terra_twld_set_budget(t, 0) == 0);
    assert(terra_twld_rewind(t, 1) == 0 && feed(t, compressed, 7) == 0);
    assert_output(&c, expected); destroy(t);

    for (uint32_t n = 0; n < 9; ++n) {
        memset(&c, 0, sizeof(c)); t = NULL;
        assert(terra_twld_create(2, 2, 0, &callbacks, &c, &t) == 0);
        assert(feed(t, compressed, 7) == 0);
        fail_after = allocations + n;
        int32_t status = terra_twld_rewind(t, 1);
        fail_after = SIZE_MAX;
        assert(status == 0 || status == TERRA_TWLD_OOM);
        if (status == TERRA_TWLD_OOM) assert(terra_twld_rewind(t, 1) == 0);
        assert(feed(t, compressed, 3) == 0); assert_output(&c, expected);
        destroy(t);
    }
}

int main(void) {
    uint32_t frame_offset, array_length;
    Buffer raw = fixture(&frame_offset, &array_length), compressed = gzip_bytes(&raw);
    Check c = {0}; TerraTwld* t = NULL;
    assert(terra_twld_create(2, 2, 0, &callbacks, &c, &t) == 0);
    assert(feed(t, &compressed, 1) == 0 && c.tiles == 0);
    assert(terra_twld_rewind(t, 0) == 0); assert(feed(t, &compressed, 3) == 0);
    assert(c.tiles == 2 && c.colors == 1 && c.entities == 1);
    assert(terra_twld_rewind(t, 1) == 0); assert(feed(t, &compressed, 7) == 0);
    Buffer patched = gunzip_bytes(&c.output);
    Buffer expected = raw; expected.bytes[frame_offset] = 36; expected.bytes[frame_offset + 2] = 54;
    assert(patched.count == expected.count && !memcmp(patched.bytes, expected.bytes, expected.count));
    destroy(t);
    abort_and_restart(&compressed, &expected);
    replay_admission_is_retryable(&compressed, &expected);

    assert(terra_twld_create(2, 2, 0, NULL, NULL, &t) == 0);
    assert(terra_twld_feed(t, compressed.bytes, compressed.count - 1, 1) == TERRA_TWLD_TRUNCATED); destroy(t);
    Buffer invalid = raw; invalid.bytes[array_length] = 0x80; invalid = gzip_bytes(&invalid);
    assert(terra_twld_create(2, 2, 0, NULL, NULL, &t) == 0);
    assert(feed(t, &invalid, 19) == TERRA_TWLD_FORMAT); destroy(t);
    assert(terra_twld_create(2, 3, 0, NULL, NULL, &t) == 0);
    assert(feed(t, &compressed, 17) == 0 && terra_twld_rewind(t, 0) == 0);
    assert(feed(t, &compressed, 17) == TERRA_TWLD_FORMAT); destroy(t);

    assert(terra_twld_create(2, 2, 0, NULL, NULL, &t) == 0);
    assert(feed(t, &compressed, 1) == 0 && terra_twld_rewind(t, 0) == 0);
    Buffer changed = compressed; changed.bytes[4] ^= 1; /* valid different gzip timestamp */
    assert(feed(t, &changed, 1) == TERRA_TWLD_CHANGED); destroy(t);
    assert(terra_twld_create(2, 2, 1024, NULL, NULL, &t) == TERRA_TWLD_LIMIT);
    assert(!live_allocations);

    for (uint32_t n = 0; n < 18; ++n) {
        memset(&c, 0, sizeof(c)); fail_after = allocations + n;
        int32_t status = terra_twld_create(2, 2, 0, &callbacks, &c, &t);
        if (!status) status = feed(t, &compressed, 13);
        if (!status) status = terra_twld_rewind(t, 1);
        if (!status) status = feed(t, &compressed, 13);
        assert(status == 0 || status == TERRA_TWLD_OOM);
        fail_after = SIZE_MAX; destroy(t); t = NULL;
    }
    puts("TWLD: fragmented metadata-after-data, unknown records, exact NBT patch, abort/restart, unchanged source CRC, budgets, 18 lifetime and 9 replay OOM points passed");
    return 0;
}
