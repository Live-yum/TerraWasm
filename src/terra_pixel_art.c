/*
 * terra_pixel_art.c -- Pixel art queue implementation.
 *
 * Accepts RGBA pixel buffer + TxPixelMap array from JS.
 * Queues the operation on the world; applied during tile section rebuild
 * in terra_update.c (rebuild_tile_section).
 *
 * No stb_image, no file I/O. All data arrives as memory buffers.
 */
#include "terra_types.h"
#include "terra_txci.h"

#define TX_PIXEL_ART_CHUNK_BITS 6u
#define TX_PIXEL_ART_CHUNK_SIZE (1u << TX_PIXEL_ART_CHUNK_BITS)
#define TX_PIXEL_ART_CHUNK_MASK (TX_PIXEL_ART_CHUNK_SIZE - 1u)
#define TX_PIXEL_ART_CHUNK_CELLS (TX_PIXEL_ART_CHUNK_SIZE * TX_PIXEL_ART_CHUNK_SIZE)
#define TX_PIXEL_ART_BULK_HEADER_BYTES 8u
#define TX_PIXEL_ART_BULK_RECORD_BYTES \
    (TX_PIXEL_ART_BULK_HEADER_BYTES + TX_PIXEL_ART_CHUNK_CELLS * sizeof(uint16_t))
#define TX_PIXEL_ART_BULK_MAX_RECORDS 63u

extern void* memset(void* dst, int value, unsigned long n);
extern void* memcpy(void* dst, const void* src, unsigned long n);
extern uint8_t* tx_alloc(uint32_t size);
extern void tx_internal_free(void* ptr);
extern uint32_t tx_bridge_allocation_size(uint32_t ptr);
extern uint32_t tx_mark(void);
extern void tx_rewind(uint32_t mark);
extern TxWorld* tx_get_world(uint32_t handle);
extern void tx_clear_error(void);
extern void tx_set_error(const char* code, const char* message);

static void protect_pixel_art_allocations(TxWorld* w) {
    if (w) w->heap_mark = tx_mark();
}

void txw_clear_pixel_art_state(TxWorld* w) {
    if (!w) return;
    TxPixelArtChunk* chunk = w->pixel_art_chunks;
    while (chunk) {
        TxPixelArtChunk* next = chunk->next;
        if (chunk->indices) tx_internal_free((void*)chunk->indices);
        tx_internal_free(chunk);
        chunk = next;
    }
    if (w->pixel_art_chunk_table) tx_internal_free(w->pixel_art_chunk_table);
    if (w->pixel_art_pixels) tx_internal_free(w->pixel_art_pixels);
    if (w->pixel_art_maps) tx_internal_free(w->pixel_art_maps);
    w->pixel_art_pixels = NULL;
    w->pixel_art_pixels_len = 0u;
    w->pixel_art_maps = NULL;
    w->pixel_art_map_count = 0u;
    w->pixel_art_indexed = 0u;
    w->pixel_art_chunk_cols = 0u;
    w->pixel_art_chunk_rows = 0u;
    w->pixel_art_chunk_table = NULL;
    w->pixel_art_chunks = NULL;
}

static void apply_map_to_tile(const TxPixelMap* map, TxTile* t) {
    uint8_t saved_same = t->same;
    memset(t, 0, sizeof(TxTile));
    t->same = saved_same;

    if (map->active_mode == 0u) {
        return;
    }
    if (map->active_mode == 3u) {
        return;
    }
    if (map->active_mode == 2u) {
        if (map->wall_type) {
            t->wall = map->wall_type;
            t->wall_color = map->wall_color;
        }
        return;
    }
    if (map->tile_type) {
        t->active = 1;
        t->type = map->tile_type;
        t->tile_color = map->tile_color;
        if (map->block_inactive <= 1u)
            t->inactive = map->block_inactive;
    }
}

static int find_override(const TxPixelMap* overrides, uint32_t override_count,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                         TxPixelMap* out) {
    if (!overrides || !out) return 0;
    for (uint32_t i = 0; i < override_count; i++) {
        if (overrides[i].r == r && overrides[i].g == g &&
            overrides[i].b == b && overrides[i].a == a) {
            *out = overrides[i];
            return 1;
        }
    }
    return 0;
}

static void build_map_from_match(TxPixelMap* map, const TxciIndex* txci,
                                 uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                                 int32_t prefer_wall, int32_t block_inactive) {
    memset(map, 0, sizeof(TxPixelMap));
    map->r = r;
    map->g = g;
    map->b = b;
    map->a = a;

    if (a == 0u) {
        map->active_mode = 0u;
        return;
    }

    TxciItem item;
    if (txci && txci_choose_tile(txci, r, g, b, prefer_wall, &item)) {
        if (item.is_wall) {
            map->wall_type = item.type_id;
            map->wall_color = item.paint_id;
            map->active_mode = 2u;
        } else {
            map->tile_type = item.type_id;
            map->tile_color = item.paint_id;
            map->active_mode = 1u;
            map->block_inactive = (uint8_t)block_inactive;
        }
    } else {
        map->tile_type = 1u;
        map->active_mode = 1u;
        map->block_inactive = (uint8_t)block_inactive;
    }
}

/*
 * txw_queue_pixel_art -- Queue a pixel art operation on a world.
 *
 * The RGBA pixels and TxPixelMap array are copied into bump-allocated memory
 * and stored on the world struct. When terra_world_save rebuilds the tile
 * section, the streaming pass applies the pixel art at (start_x, start_y).
 *
 * Returns 0 on success, -1 on error.
 */
int txw_queue_pixel_art(
    uint32_t handle,
    int32_t start_x,
    int32_t start_y,
    uint32_t width,
    uint32_t height,
    uint32_t pixels_ptr,
    uint32_t pixels_len,
    uint32_t map_ptr,
    uint32_t map_count,
    int32_t skip_transparent)
{
    TxWorld* w = tx_get_world(handle);
    if (!w) {
        tx_set_error("TERRAX_INVALID_HANDLE", "world handle is stale or invalid");
        return -1;
    }
    uint64_t expected_len64 = (uint64_t)width * height * 4u;
    if (!pixels_ptr || !map_ptr || expected_len64 > UINT32_MAX ||
        pixels_len < (uint32_t)expected_len64 ||
        map_count == 0u || map_count > UINT32_MAX / sizeof(TxPixelMap)) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "invalid pixel art payload");
        return -1;
    }

    /* Copy RGBA pixels to bump allocator */
    uint8_t* pix = tx_alloc(pixels_len);
    if (!pix) {
        tx_set_error("TERRAX_WASM_OOM", "pixel art pixels allocation failed");
        return -1;
    }
    memcpy(pix, (const void*)(uintptr_t)pixels_ptr, pixels_len);

    /* Copy TxPixelMap array to bump allocator */
    uint32_t maps_size = map_count * sizeof(TxPixelMap);
    uint8_t* maps = tx_alloc(maps_size);
    if (!maps) {
        tx_internal_free(pix);
        tx_set_error("TERRAX_WASM_OOM", "pixel art maps allocation failed");
        return -1;
    }
    memcpy(maps, (const void*)(uintptr_t)map_ptr, maps_size);

    /* Replace the prior queued operation only after all new roots exist. */
    txw_clear_pixel_art_state(w);
    w->pixel_art_pixels = pix;
    w->pixel_art_pixels_len = pixels_len;
    w->pixel_art_maps = maps;
    w->pixel_art_map_count = map_count;
    w->pixel_art_start_x = start_x;
    w->pixel_art_start_y = start_y;
    w->pixel_art_width = width;
    w->pixel_art_height = height;
    w->pixel_art_skip_transparent = skip_transparent;
    w->pixel_art_indexed = 0;
    w->pixel_art_default_index = 0u;
    w->pixel_art_chunk_cols = 0;
    w->pixel_art_chunk_rows = 0;
    w->pixel_art_chunk_table = NULL;
    w->pixel_art_chunks = NULL;
    protect_pixel_art_allocations(w);

    tx_clear_error();
    return 0;
}

/*
 * txw_begin_pixel_art_indexed -- Queue a low-memory indexed pixel-art write.
 *
 * The palette is an RGBA table indexed by upcoming 64x64 Uint16 chunks.
 * Only non-empty chunks are supplied by txw_add_pixel_art_chunk, so JS never
 * has to allocate a full width*height*4 RGBA image for large canvases.
 */
int txw_begin_pixel_art_indexed(
    uint32_t handle,
    int32_t start_x,
    int32_t start_y,
    uint32_t width,
    uint32_t height,
    uint32_t palette_ptr,
    uint32_t palette_count,
    uint32_t txci_ptr,
    uint32_t txci_len,
    int32_t prefer_wall,
    int32_t block_inactive,
    uint32_t overrides_ptr,
    uint32_t overrides_count,
    uint32_t default_palette_index)
{
    TxWorld* w = tx_get_world(handle);
    if (!w) {
        tx_set_error("TERRAX_INVALID_HANDLE", "world handle is stale or invalid");
        return -1;
    }
    if (width == 0u || height == 0u) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "image dimensions cannot be zero");
        return -1;
    }
    if (!palette_ptr || palette_count == 0u) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "palette is empty");
        return -1;
    }
    if (!txci_ptr || txci_len < 44u) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "TXCI data too small or null");
        return -1;
    }

    if (palette_count > UINT32_MAX / 4u ||
        palette_count > UINT32_MAX / sizeof(TxPixelMap) ||
        (overrides_count > 0u && (!overrides_ptr ||
         overrides_count > UINT32_MAX / sizeof(TxPixelMap)))) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "indexed palette size overflow");
        return -1;
    }
    uint32_t maps_size = palette_count * sizeof(TxPixelMap);
    uint8_t* maps_buf = tx_alloc(maps_size);
    if (!maps_buf) {
        tx_set_error("TERRAX_WASM_OOM", "indexed palette maps allocation failed");
        return -1;
    }
    TxPixelMap* maps = (TxPixelMap*)maps_buf;

    uint32_t txci_mark = tx_mark();
    TxciIndex txci;
    if (!txci_load_from_memory(&txci, (const uint8_t*)(uintptr_t)txci_ptr, txci_len)) {
        tx_rewind(txci_mark);
        tx_internal_free(maps_buf);
        return -1;
    }

    const uint8_t* palette = (const uint8_t*)(uintptr_t)palette_ptr;
    const TxPixelMap* overrides = overrides_ptr ? (const TxPixelMap*)(uintptr_t)overrides_ptr : NULL;

    for (uint32_t i = 0; i < palette_count; i++) {
        uint32_t off = i * 4u;
        uint8_t r = palette[off];
        uint8_t g = palette[off + 1u];
        uint8_t b = palette[off + 2u];
        uint8_t a = palette[off + 3u];
        if (a == 0u) {
            memset(&maps[i], 0, sizeof(TxPixelMap));
            maps[i].r = r;
            maps[i].g = g;
            maps[i].b = b;
            maps[i].a = a;
            maps[i].active_mode = 0u;
        } else if (!find_override(overrides, overrides_count, r, g, b, a, &maps[i])) {
            build_map_from_match(&maps[i], &txci, r, g, b, a, prefer_wall, block_inactive);
        }
    }

    txci_unload(&txci);
    tx_rewind(txci_mark);

    uint64_t cols64 = ((uint64_t)width + TX_PIXEL_ART_CHUNK_SIZE - 1u) >> TX_PIXEL_ART_CHUNK_BITS;
    uint64_t rows64 = ((uint64_t)height + TX_PIXEL_ART_CHUNK_SIZE - 1u) >> TX_PIXEL_ART_CHUNK_BITS;
    uint64_t table_count64 = cols64 * rows64;
    if (cols64 == 0u || rows64 == 0u || cols64 > UINT32_MAX || rows64 > UINT32_MAX ||
        table_count64 > UINT32_MAX / sizeof(TxPixelArtChunk*)) {
        tx_internal_free(maps_buf);
        tx_set_error("TERRAX_INVALID_ARGUMENT", "chunk table dimensions overflow");
        return -1;
    }
    uint32_t cols = (uint32_t)cols64;
    uint32_t rows = (uint32_t)rows64;
    uint32_t table_count = (uint32_t)table_count64;

    TxPixelArtChunk** table = (TxPixelArtChunk**)tx_alloc(table_count * sizeof(TxPixelArtChunk*));
    if (!table) {
        tx_internal_free(maps_buf);
        tx_set_error("TERRAX_WASM_OOM", "indexed chunk table allocation failed");
        return -1;
    }
    memset(table, 0, table_count * sizeof(TxPixelArtChunk*));

    txw_clear_pixel_art_state(w);
    w->pixel_art_pixels = NULL;
    w->pixel_art_pixels_len = 0u;
    w->pixel_art_maps = maps_buf;
    w->pixel_art_map_count = palette_count;
    w->pixel_art_start_x = start_x;
    w->pixel_art_start_y = start_y;
    w->pixel_art_width = width;
    w->pixel_art_height = height;
    w->pixel_art_skip_transparent = 1;
    w->pixel_art_indexed = 1;
    w->pixel_art_default_index = default_palette_index < palette_count ? default_palette_index : 0u;
    w->pixel_art_chunk_cols = cols;
    w->pixel_art_chunk_rows = rows;
    w->pixel_art_chunk_table = table;
    w->pixel_art_chunks = NULL;
    protect_pixel_art_allocations(w);

    tx_clear_error();
    return 0;
}

int txw_add_pixel_art_chunk(
    uint32_t handle,
    int32_t chunk_x,
    int32_t chunk_y,
    uint32_t indices_ptr,
    uint32_t index_count,
    uint32_t used)
{
    TxWorld* w = tx_get_world(handle);
    if (!w || !w->pixel_art_indexed || !w->pixel_art_chunk_table) {
        tx_set_error("TERRAX_STATE_ERROR", "indexed pixel art not initialized");
        return -1;
    }
    if (chunk_x < 0 || chunk_y < 0 ||
        (uint32_t)chunk_x >= w->pixel_art_chunk_cols ||
        (uint32_t)chunk_y >= w->pixel_art_chunk_rows) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "chunk coordinates out of range");
        return -1;
    }
    if (!indices_ptr || index_count != TX_PIXEL_ART_CHUNK_CELLS) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "chunk indices payload must contain 4096 values");
        return -1;
    }
    if (used > TX_PIXEL_ART_CHUNK_CELLS) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "chunk used count exceeds 4096");
        return -1;
    }
    if (used == 0u) {
        tx_clear_error();
        return 0;
    }

    uint32_t bytes = TX_PIXEL_ART_CHUNK_SIZE * TX_PIXEL_ART_CHUNK_SIZE * sizeof(uint16_t);
    uint16_t* indices = (uint16_t*)tx_alloc(bytes);
    if (!indices) {
        tx_set_error("TERRAX_WASM_OOM", "indexed chunk allocation failed");
        return -1;
    }
    memcpy(indices, (const void*)(uintptr_t)indices_ptr, bytes);

    uint32_t table_index = (uint32_t)chunk_y * w->pixel_art_chunk_cols + (uint32_t)chunk_x;
    TxPixelArtChunk* existing = w->pixel_art_chunk_table[table_index];
    if (existing) {
        if (existing->indices) tx_internal_free((void*)existing->indices);
        existing->indices = indices;
        existing->used = used;
        protect_pixel_art_allocations(w);
        tx_clear_error();
        return 0;
    }

    TxPixelArtChunk* chunk = (TxPixelArtChunk*)tx_alloc(sizeof(TxPixelArtChunk));
    if (!chunk) {
        tx_internal_free(indices);
        tx_set_error("TERRAX_WASM_OOM", "indexed chunk node allocation failed");
        return -1;
    }
    chunk->cx = chunk_x;
    chunk->cy = chunk_y;
    chunk->used = used;
    chunk->indices = indices;
    chunk->next = w->pixel_art_chunks;
    w->pixel_art_chunks = chunk;
    w->pixel_art_chunk_table[table_index] = chunk;
    protect_pixel_art_allocations(w);

    tx_clear_error();
    return 0;
}

static uint16_t pixel_bulk_u16le(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8u));
}

static int validate_pixel_art_bulk_record(
    TxWorld* w,
    const uint8_t* records,
    uint32_t record_index)
{
    const uint8_t* record = records + record_index * TX_PIXEL_ART_BULK_RECORD_BYTES;
    uint32_t chunk_x = pixel_bulk_u16le(record);
    uint32_t chunk_y = pixel_bulk_u16le(record + 2u);
    uint32_t used = pixel_bulk_u16le(record + 4u);
    uint32_t reserved = pixel_bulk_u16le(record + 6u);

    if (reserved != 0u) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "indexed bulk record reserved field must be zero");
        return 0;
    }
    if (chunk_x >= w->pixel_art_chunk_cols || chunk_y >= w->pixel_art_chunk_rows) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "indexed bulk chunk coordinates out of range");
        return 0;
    }
    if (used == 0u || used > TX_PIXEL_ART_CHUNK_CELLS) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "indexed bulk used count must be in 1..4096");
        return 0;
    }

    uint32_t table_index = chunk_y * w->pixel_art_chunk_cols + chunk_x;
    if (w->pixel_art_chunk_table[table_index]) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "indexed bulk contains a duplicate chunk");
        return 0;
    }
    for (uint32_t prior = 0u; prior < record_index; prior++) {
        const uint8_t* prior_record = records + prior * TX_PIXEL_ART_BULK_RECORD_BYTES;
        if (pixel_bulk_u16le(prior_record) == chunk_x &&
            pixel_bulk_u16le(prior_record + 2u) == chunk_y) {
            tx_set_error("TERRAX_INVALID_ARGUMENT", "indexed bulk contains a duplicate chunk");
            return 0;
        }
    }

    const uint8_t* indices = record + TX_PIXEL_ART_BULK_HEADER_BYTES;
    uint32_t actual_used = 0u;
    for (uint32_t cell = 0u; cell < TX_PIXEL_ART_CHUNK_CELLS; cell++) {
        uint32_t palette_index = pixel_bulk_u16le(indices + cell * sizeof(uint16_t));
        if (palette_index >= w->pixel_art_map_count) {
            tx_set_error("TERRAX_INVALID_ARGUMENT", "indexed bulk palette index out of range");
            return 0;
        }
        if (palette_index == 0u) continue;
        uint64_t pixel_x = (uint64_t)chunk_x * TX_PIXEL_ART_CHUNK_SIZE +
            (cell & TX_PIXEL_ART_CHUNK_MASK);
        uint64_t pixel_y = (uint64_t)chunk_y * TX_PIXEL_ART_CHUNK_SIZE +
            (cell >> TX_PIXEL_ART_CHUNK_BITS);
        if (pixel_x >= w->pixel_art_width || pixel_y >= w->pixel_art_height) {
            tx_set_error("TERRAX_INVALID_ARGUMENT", "indexed bulk contains pixels outside the canvas");
            return 0;
        }
        actual_used++;
    }
    if (actual_used != used) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "indexed bulk used count mismatch");
        return 0;
    }
    return 1;
}

/*
 * txw_add_pixel_art_chunks_bulk -- Add one persisted attachment per bridge call.
 *
 * Records are fixed-size little-endian blocks: cx:u16, cy:u16, used:u16,
 * reserved:u16, then 4096 palette indices as u16. The bridge buffer is fully
 * validated before any world state is mutated. txw_add_pixel_art_chunk copies
 * every accepted record into native-owned memory; no JS pointer is retained.
 */
int txw_add_pixel_art_chunks_bulk(
    uint32_t handle,
    uint32_t records_ptr,
    uint32_t records_len,
    uint32_t record_count)
{
    TxWorld* w = tx_get_world(handle);
    if (!w || !w->pixel_art_indexed || !w->pixel_art_chunk_table) {
        tx_set_error("TERRAX_STATE_ERROR", "indexed pixel art not initialized");
        return -1;
    }
    if (!records_ptr || record_count == 0u || record_count > TX_PIXEL_ART_BULK_MAX_RECORDS) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "indexed bulk record count must be in 1..63");
        return -1;
    }
    uint64_t expected_len = (uint64_t)record_count * TX_PIXEL_ART_BULK_RECORD_BYTES;
    if (expected_len > UINT32_MAX || records_len != (uint32_t)expected_len ||
        records_ptr > UINT32_MAX - records_len) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "indexed bulk payload length mismatch");
        return -1;
    }
    if (tx_bridge_allocation_size(records_ptr) < records_len) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "indexed bulk payload is not a valid bridge allocation");
        return -1;
    }

    const uint8_t* records = (const uint8_t*)(uintptr_t)records_ptr;
    for (uint32_t index = 0u; index < record_count; index++) {
        if (!validate_pixel_art_bulk_record(w, records, index)) return -1;
    }
    for (uint32_t index = 0u; index < record_count; index++) {
        const uint8_t* record = records + index * TX_PIXEL_ART_BULK_RECORD_BYTES;
        uint32_t chunk_x = pixel_bulk_u16le(record);
        uint32_t chunk_y = pixel_bulk_u16le(record + 2u);
        uint32_t used = pixel_bulk_u16le(record + 4u);
        uint32_t indices_ptr = (uint32_t)(uintptr_t)(record + TX_PIXEL_ART_BULK_HEADER_BYTES);
        if (txw_add_pixel_art_chunk(
            handle,
            (int32_t)chunk_x,
            (int32_t)chunk_y,
            indices_ptr,
            TX_PIXEL_ART_CHUNK_CELLS,
            used) < 0) return -1;
    }
    tx_clear_error();
    return 0;
}

/*
 * apply_pixel_art_at -- Check if position (x, y) falls within the queued
 * pixel art region. If so, look up the pixel color in the TxPixelMap array
 * and apply the tile/wall mapping to the tile.
 *
 * Returns 1 if the tile was modified, 0 otherwise.
 * Called from rebuild_tile_section in terra_update.c.
 */
int apply_pixel_art_at(TxWorld* w, uint32_t x, uint32_t y, TxTile* t) {
    if ((!w->pixel_art_pixels && !w->pixel_art_indexed) ||
        !w->pixel_art_maps || !w->pixel_art_map_count)
        return 0;

    int32_t sx = w->pixel_art_start_x;
    int32_t sy = w->pixel_art_start_y;
    uint32_t pw = w->pixel_art_width;
    uint32_t ph = w->pixel_art_height;

    if ((int32_t)x < sx || (int32_t)y < sy) return 0;
    uint32_t px = (uint32_t)((int32_t)x - sx);
    uint32_t py = (uint32_t)((int32_t)y - sy);
    if (px >= pw || py >= ph) return 0;

    if (w->pixel_art_indexed) {
        uint32_t cx = px >> TX_PIXEL_ART_CHUNK_BITS;
        uint32_t cy = py >> TX_PIXEL_ART_CHUNK_BITS;
        if (cx >= w->pixel_art_chunk_cols || cy >= w->pixel_art_chunk_rows)
            return 0;
        TxPixelArtChunk* chunk = w->pixel_art_chunk_table
            ? w->pixel_art_chunk_table[cy * w->pixel_art_chunk_cols + cx]
            : NULL;
        uint16_t palette_index = (uint16_t)w->pixel_art_default_index;
        if (chunk && chunk->indices) {
            uint32_t local = (py & TX_PIXEL_ART_CHUNK_MASK) * TX_PIXEL_ART_CHUNK_SIZE +
                             (px & TX_PIXEL_ART_CHUNK_MASK);
            palette_index = chunk->indices[local];
            if (palette_index == 0u) palette_index = (uint16_t)w->pixel_art_default_index;
        }
        if (palette_index == 0u) return 0;
        if ((uint32_t)palette_index >= w->pixel_art_map_count) return 0;
        const TxPixelMap* maps = (const TxPixelMap*)w->pixel_art_maps;
        if (maps[palette_index].active_mode == 3u) return 0;
        apply_map_to_tile(&maps[palette_index], t);
        return 1;
    }

    uint32_t offset = (py * pw + px) * 4u;
    if (offset + 3u >= w->pixel_art_pixels_len) return 0;

    uint8_t r = w->pixel_art_pixels[offset];
    uint8_t g = w->pixel_art_pixels[offset + 1];
    uint8_t b = w->pixel_art_pixels[offset + 2];
    uint8_t a = w->pixel_art_pixels[offset + 3];

    /* Skip transparent pixels */
    if (w->pixel_art_skip_transparent && a == 0u) return 0;

    /* Find matching TxPixelMap entry */
    const TxPixelMap* maps = (const TxPixelMap*)w->pixel_art_maps;
    for (uint32_t i = 0; i < w->pixel_art_map_count; i++) {
        if (maps[i].r == r && maps[i].g == g && maps[i].b == b && maps[i].a == a) {
            apply_map_to_tile(&maps[i], t);
            return 1;
        }
    }

    return 0;
}

/*
 * txw_apply_pixel_art -- Integrated pixel art API.
 *
 * Takes RGBA pixel data + TXCI gzip data, internally loads TXCI,
 * matches colors to tiles/walls, and queues the pixel art operation.
 *
 * overrides_ptr / overrides_count: optional array of TxPixelMap entries.
 *   For each unique color in the image, if a matching override exists
 *   (same r,g,b,a), it takes priority over the TXCI lookup.
 *   - active_mode=0 → empty block (clear tile+wall)
 *   - active_mode=1 → tile placement
 *   - active_mode=2 → wall placement
 *   Pass 0/NULL to skip overrides (pure TXCI lookup).
 *
 * Returns 0 on success, -1 on error.
 */
int txw_apply_pixel_art(
    uint32_t handle,
    uint32_t image_ptr,
    uint32_t image_len,
    uint32_t width,
    uint32_t height,
    uint32_t txci_ptr,
    uint32_t txci_len,
    int32_t start_x,
    int32_t start_y,
    int32_t prefer_wall,
    int32_t block_inactive,
    uint32_t overrides_ptr,
    uint32_t overrides_count)
{
    TxWorld* w = tx_get_world(handle);
    if (!w) {
        tx_set_error("TERRAX_INVALID_HANDLE", "world handle is stale or invalid");
        return -1;
    }

    uint64_t pixel_count64 = (uint64_t)width * height;
    uint64_t expected_len64 = pixel_count64 * 4u;
    if (!image_ptr || pixel_count64 > UINT32_MAX || expected_len64 > UINT32_MAX ||
        image_len < (uint32_t)expected_len64) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "image data too short for given dimensions");
        return -1;
    }
    if (!txci_ptr || txci_len < 44) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "TXCI data too small or null");
        return -1;
    }
    if (width == 0 || height == 0) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "image dimensions cannot be zero");
        return -1;
    }
    if (overrides_count > 0u && (!overrides_ptr ||
        overrides_count > UINT32_MAX / sizeof(TxPixelMap))) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "pixel-art overrides are invalid");
        return -1;
    }
    uint32_t pixel_count = (uint32_t)pixel_count64;

    /* Load TXCI index from memory */
    TxciIndex txci;
    if (!txci_load_from_memory(&txci, (const uint8_t*)(uintptr_t)txci_ptr, txci_len)) {
        /* txci_load_from_memory already set error */
        return -1;
    }

    /* Access overrides array (if provided) */
    const TxPixelMap* overrides = NULL;
    if (overrides_ptr && overrides_count > 0) {
        overrides = (const TxPixelMap*)(uintptr_t)overrides_ptr;
    }

    const uint8_t* pixels = (const uint8_t*)(uintptr_t)image_ptr;

    /*
     * Scan unique colors in the RGBA pixel data.
     * Use a hash table for deduplication, allocated on the bump heap.
     * Key: (r << 24) | (g << 16) | (b << 8) | a
     */
    #define COLOR_HASH_BITS   17
    #define COLOR_HASH_SIZE   (1u << COLOR_HASH_BITS)  /* 131072 */
    #define COLOR_HASH_MASK   (COLOR_HASH_SIZE - 1u)

    /* Allocate hash table and unique colors array on bump heap */
    uint32_t* hash_keys = (uint32_t*)tx_alloc(COLOR_HASH_SIZE * sizeof(uint32_t));
    if (!hash_keys) {
        txci_unload(&txci);
        tx_set_error("TERRAX_WASM_OOM", "hash table allocation failed");
        return -1;
    }
    for (uint32_t i = 0; i < COLOR_HASH_SIZE; i++) {
        hash_keys[i] = 0xFFFFFFFFu;
    }

    /* First pass: count unique colors */
    uint32_t unique_count = 0;
    #define COLOR_HASH(k) (((k) ^ ((k) >> 12) ^ ((k) >> 24)) & COLOR_HASH_MASK)

    for (uint32_t i = 0; i < pixel_count; i++) {
        uint32_t off = i * 4u;
        uint8_t a = pixels[off + 3];
        if (a == 0u) continue;

        uint32_t key = ((uint32_t)pixels[off] << 24) | ((uint32_t)pixels[off + 1] << 16)
                     | ((uint32_t)pixels[off + 2] << 8) | (uint32_t)a;

        uint32_t h = COLOR_HASH(key);
        uint32_t probes = 0u;
        while (hash_keys[h] != 0xFFFFFFFFu && probes < COLOR_HASH_SIZE) {
            if (hash_keys[h] == key) goto next_pixel;
            h = (h + 1u) & COLOR_HASH_MASK;
            probes++;
        }
        if (probes == COLOR_HASH_SIZE) {
            tx_internal_free(hash_keys);
            txci_unload(&txci);
            tx_set_error("TERRAX_INVALID_ARGUMENT", "image has too many unique colors");
            return -1;
        }
        hash_keys[h] = key;
        unique_count++;

        next_pixel:;
    }

    if (unique_count == 0) {
        tx_internal_free(hash_keys);
        txci_unload(&txci);
        tx_clear_error();
        return 0;
    }

    /* Allocate unique colors array on bump heap */
    uint32_t* unique_colors = (uint32_t*)tx_alloc(unique_count * sizeof(uint32_t));
    if (!unique_colors) {
        tx_internal_free(hash_keys);
        txci_unload(&txci);
        tx_set_error("TERRAX_WASM_OOM", "unique colors allocation failed");
        return -1;
    }

    /* Second pass: collect unique colors in order */
    for (uint32_t i = 0; i < COLOR_HASH_SIZE; i++) {
        hash_keys[i] = 0xFFFFFFFFu;
    }
    uint32_t idx = 0;
    for (uint32_t i = 0; i < pixel_count; i++) {
        uint32_t off = i * 4u;
        uint8_t a = pixels[off + 3];
        if (a == 0u) continue;

        uint32_t key = ((uint32_t)pixels[off] << 24) | ((uint32_t)pixels[off + 1] << 16)
                     | ((uint32_t)pixels[off + 2] << 8) | (uint32_t)a;

        uint32_t h = COLOR_HASH(key);
        while (hash_keys[h] != 0xFFFFFFFFu) {
            if (hash_keys[h] == key) goto next_pixel2;
            h = (h + 1u) & COLOR_HASH_MASK;
        }
        hash_keys[h] = key;
        unique_colors[idx++] = key;

        next_pixel2:;
    }

    /* Build TxPixelMap array for each unique color */
    uint32_t map_count = unique_count;
    if (map_count == 0) {
        /* No non-transparent pixels, nothing to do */
        tx_internal_free(unique_colors);
        tx_internal_free(hash_keys);
        txci_unload(&txci);
        return 0;
    }

    if (map_count > UINT32_MAX / sizeof(TxPixelMap)) {
        tx_internal_free(unique_colors);
        tx_internal_free(hash_keys);
        txci_unload(&txci);
        tx_set_error("TERRAX_INVALID_ARGUMENT", "pixel-art map size overflow");
        return -1;
    }
    uint32_t maps_size = map_count * sizeof(TxPixelMap);
    uint8_t* maps_buf = tx_alloc(maps_size);
    if (!maps_buf) {
        tx_internal_free(unique_colors);
        tx_internal_free(hash_keys);
        txci_unload(&txci);
        tx_set_error("TERRAX_WASM_OOM", "pixel art maps allocation failed");
        return -1;
    }
    TxPixelMap* maps = (TxPixelMap*)maps_buf;

    for (uint32_t i = 0; i < unique_count; i++) {
        uint32_t key = unique_colors[i];
        uint8_t r = (uint8_t)(key >> 24);
        uint8_t g = (uint8_t)(key >> 16);
        uint8_t b = (uint8_t)(key >> 8);
        uint8_t a = (uint8_t)(key);

        memset(&maps[i], 0, sizeof(TxPixelMap));
        maps[i].r = r;
        maps[i].g = g;
        maps[i].b = b;
        maps[i].a = a;

        /* Check overrides first */
        int found_override = 0;
        if (overrides) {
            for (uint32_t j = 0; j < overrides_count; j++) {
                if (overrides[j].r == r && overrides[j].g == g &&
                    overrides[j].b == b && overrides[j].a == a) {
                    /* Copy the entire override entry */
                    maps[i].tile_type = overrides[j].tile_type;
                    maps[i].wall_type = overrides[j].wall_type;
                    maps[i].tile_color = overrides[j].tile_color;
                    maps[i].wall_color = overrides[j].wall_color;
                    maps[i].active_mode = overrides[j].active_mode;
                    maps[i].block_inactive = overrides[j].block_inactive;
                    found_override = 1;
                    break;
                }
            }
        }

        if (!found_override) {
            /* No override: look up best tile/wall match via TXCI */
            TxciItem item;
            if (txci_choose_tile(&txci, r, g, b, prefer_wall, &item)) {
                if (item.is_wall) {
                    maps[i].wall_type = item.type_id;
                    maps[i].wall_color = item.paint_id;
                    maps[i].active_mode = 2;
                } else {
                    maps[i].tile_type = item.type_id;
                    maps[i].tile_color = item.paint_id;
                    maps[i].active_mode = 1;
                    maps[i].block_inactive = (uint8_t)block_inactive;
                }
            } else {
                /* No TXCI match, use Stone Block as default */
                maps[i].tile_type = 1;
                maps[i].active_mode = 1;
                maps[i].block_inactive = (uint8_t)block_inactive;
            }
        }
    }

    /* Copy RGBA pixels to bump allocator */
    uint8_t* pix = tx_alloc(image_len);
    if (!pix) {
        tx_internal_free(maps_buf);
        tx_internal_free(unique_colors);
        tx_internal_free(hash_keys);
        txci_unload(&txci);
        tx_set_error("TERRAX_WASM_OOM", "pixel art pixels allocation failed");
        return -1;
    }
    memcpy(pix, (const void*)(uintptr_t)image_ptr, image_len);

    tx_internal_free(unique_colors);
    tx_internal_free(hash_keys);
    txci_unload(&txci);

    /* Store only the final maps/pixels; TXCI and hash roots were released. */
    txw_clear_pixel_art_state(w);
    w->pixel_art_pixels = pix;
    w->pixel_art_pixels_len = image_len;
    w->pixel_art_maps = maps_buf;
    w->pixel_art_map_count = map_count;
    w->pixel_art_start_x = start_x;
    w->pixel_art_start_y = start_y;
    w->pixel_art_width = width;
    w->pixel_art_height = height;
    w->pixel_art_skip_transparent = 1;
    w->pixel_art_indexed = 0;
    w->pixel_art_default_index = 0u;
    w->pixel_art_chunk_cols = 0;
    w->pixel_art_chunk_rows = 0;
    w->pixel_art_chunk_table = NULL;
    w->pixel_art_chunks = NULL;
    protect_pixel_art_allocations(w);

    tx_clear_error();
    return 0;
}
