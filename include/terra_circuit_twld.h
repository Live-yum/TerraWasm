#ifndef TERRA_CIRCUIT_TWLD_H
#define TERRA_CIRCUIT_TWLD_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct TerraTwld TerraTwld;
typedef struct TerraTwldTile {
    uint32_t x, y;
    uint16_t saved_type;
    int16_t frame_x, frame_y;
    uint8_t paint, framed, color_pixel_box;
} TerraTwldTile;
enum TerraTwldEntityKind { TERRA_TWLD_TRAINING_DUMMY = 1, TERRA_TWLD_LOGIC_SENSOR = 2 };
typedef struct TerraTwldEntity { uint32_t x, y, kind; } TerraTwldEntity;
typedef struct TerraTwldCallbacks {
    int32_t (*tile)(void* context, const TerraTwldTile* tile);
    /* Optional fixed-size edit during output replay. saved_type, x/y, framed and
     * color_pixel_box cannot change. All unrecognized mod records remain intact. */
    int32_t (*patch)(void* context, TerraTwldTile* tile);
    int32_t (*output)(void* context, const uint8_t* bytes, uint32_t count);
    int32_t (*entity)(void* context, const TerraTwldEntity* entity);
} TerraTwldCallbacks;
typedef struct TerraTwldStats {
    uint64_t compressed_bytes, expanded_bytes, world_cells, mod_tiles, color_pixels;
    uint32_t map_entries, pass, complete, allocated_bytes, peak_bytes;
    uint32_t training_dummies, logic_sensors;
} TerraTwldStats;
enum TerraTwldStatus {
    TERRA_TWLD_OK = 0, TERRA_TWLD_MORE = 1,
    TERRA_TWLD_INVALID = -1, TERRA_TWLD_STATE = -2,
    TERRA_TWLD_OOM = -3, TERRA_TWLD_LIMIT = -4,
    TERRA_TWLD_FORMAT = -5, TERRA_TWLD_TRUNCATED = -6,
    TERRA_TWLD_CALLBACK = -7, TERRA_TWLD_CHANGED = -8
};

/* Streaming gzip/NBT codec. The first pass reads metadata and skips large byte
 * arrays. Rewind then feed the SAME compressed bytes to decode tileData using
 * that map; this is required because tileMap follows tileData in actual tModLoader
 * files. Buffers are bounded independently of the expanded world size. */
int32_t terra_twld_create(uint32_t width, uint32_t height, uint32_t max_bytes,
                          const TerraTwldCallbacks* callbacks, void* context,
                          TerraTwld** out);
int32_t terra_twld_feed(TerraTwld* codec, const uint8_t* bytes, uint32_t count, uint32_t final);
/* emit_output=1 streams a gzip result through callbacks.output. NBT bytes not
 * explicitly patched are preserved exactly, including unknown tags and mods.
 * Output callbacks must write to a staging sink and commit only after final OK. */
int32_t terra_twld_rewind(TerraTwld* codec, uint32_t emit_output);
/* Discard an incomplete or failed second pass after metadata was successfully
 * read. Original map, compressed size and source CRC remain immutable. The next
 * rewind starts again at source offset zero; feed is rejected until then.
 * This performs no allocation and is also valid after a completed replay.
 * Discard any staged output and callback-owned partial results separately.
 * Failed rewind allocation/budget admission also leaves a retryable idle replay. */
int32_t terra_twld_abort_replay(TerraTwld* codec);
int32_t terra_twld_set_budget(TerraTwld* codec, uint32_t max_bytes);
void terra_twld_stats(const TerraTwld* codec, TerraTwldStats* out);
void terra_twld_destroy(TerraTwld* codec);

#ifdef __cplusplus
}
#endif
#endif
