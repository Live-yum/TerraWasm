/*
 * Fast bulk attachment path for indexed pixel art.
 *
 * The legacy bulk ABI validates the whole bridge payload, then calls the
 * public single-chunk entry point for every record. This companion ABI keeps
 * the same binary record layout and validation contract, but attaches the
 * already-validated records directly and advances the persistent heap mark
 * once per batch instead of once per chunk.
 */
#include "terra_types.h"

#define TX_PIXEL_ART_CHUNK_BITS 6u
#define TX_PIXEL_ART_CHUNK_SIZE (1u << TX_PIXEL_ART_CHUNK_BITS)
#define TX_PIXEL_ART_CHUNK_MASK (TX_PIXEL_ART_CHUNK_SIZE - 1u)
#define TX_PIXEL_ART_CHUNK_CELLS (TX_PIXEL_ART_CHUNK_SIZE * TX_PIXEL_ART_CHUNK_SIZE)
#define TX_PIXEL_ART_BULK_HEADER_BYTES 8u
#define TX_PIXEL_ART_BULK_RECORD_BYTES \
    (TX_PIXEL_ART_BULK_HEADER_BYTES + TX_PIXEL_ART_CHUNK_CELLS * sizeof(uint16_t))
#define TX_PIXEL_ART_BULK_MAX_RECORDS 63u

extern void* memcpy(void* dst, const void* src, unsigned long n);
extern uint8_t* tx_alloc(uint32_t size);
extern void tx_internal_free(void* ptr);
extern uint32_t tx_bridge_allocation_size(uint32_t ptr);
extern uint32_t tx_mark(void);
extern TxWorld* tx_get_world(uint32_t handle);
extern void tx_clear_error(void);
extern void tx_set_error(const char* code, const char* message);

static uint16_t pixel_bulk_fast_u16le(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8u));
}

static void protect_bulk_pixel_art_allocations(TxWorld* w) {
    if (w) w->heap_mark = tx_mark();
}

static int validate_bulk_fast_record(
    TxWorld* w,
    const uint8_t* records,
    uint32_t record_index)
{
    const uint8_t* record = records + record_index * TX_PIXEL_ART_BULK_RECORD_BYTES;
    uint32_t chunk_x = pixel_bulk_fast_u16le(record);
    uint32_t chunk_y = pixel_bulk_fast_u16le(record + 2u);
    uint32_t used = pixel_bulk_fast_u16le(record + 4u);
    uint32_t reserved = pixel_bulk_fast_u16le(record + 6u);

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
        if (pixel_bulk_fast_u16le(prior_record) == chunk_x &&
            pixel_bulk_fast_u16le(prior_record + 2u) == chunk_y) {
            tx_set_error("TERRAX_INVALID_ARGUMENT", "indexed bulk contains a duplicate chunk");
            return 0;
        }
    }

    const uint8_t* indices = record + TX_PIXEL_ART_BULK_HEADER_BYTES;
    uint64_t chunk_origin_x = (uint64_t)chunk_x * TX_PIXEL_ART_CHUNK_SIZE;
    uint64_t chunk_origin_y = (uint64_t)chunk_y * TX_PIXEL_ART_CHUNK_SIZE;
    int needs_canvas_bounds =
        chunk_origin_x + TX_PIXEL_ART_CHUNK_SIZE > w->pixel_art_width ||
        chunk_origin_y + TX_PIXEL_ART_CHUNK_SIZE > w->pixel_art_height;
    uint32_t actual_used = 0u;
    for (uint32_t cell = 0u; cell < TX_PIXEL_ART_CHUNK_CELLS; cell++) {
        uint32_t palette_index = pixel_bulk_fast_u16le(indices + cell * sizeof(uint16_t));
        if (palette_index >= w->pixel_art_map_count) {
            tx_set_error("TERRAX_INVALID_ARGUMENT", "indexed bulk palette index out of range");
            return 0;
        }
        if (palette_index == 0u) continue;
        if (needs_canvas_bounds) {
            uint64_t pixel_x = chunk_origin_x + (cell & TX_PIXEL_ART_CHUNK_MASK);
            uint64_t pixel_y = chunk_origin_y + (cell >> TX_PIXEL_ART_CHUNK_BITS);
            if (pixel_x >= w->pixel_art_width || pixel_y >= w->pixel_art_height) {
                tx_set_error("TERRAX_INVALID_ARGUMENT", "indexed bulk contains pixels outside the canvas");
                return 0;
            }
        }
        actual_used++;
    }
    if (actual_used != used) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "indexed bulk used count mismatch");
        return 0;
    }
    return 1;
}

static int attach_validated_bulk_record(TxWorld* w, const uint8_t* record) {
    uint32_t chunk_x = pixel_bulk_fast_u16le(record);
    uint32_t chunk_y = pixel_bulk_fast_u16le(record + 2u);
    uint32_t used = pixel_bulk_fast_u16le(record + 4u);
    const uint8_t* source = record + TX_PIXEL_ART_BULK_HEADER_BYTES;
    uint32_t bytes = TX_PIXEL_ART_CHUNK_CELLS * sizeof(uint16_t);

    uint16_t* indices = (uint16_t*)tx_alloc(bytes);
    if (!indices) {
        tx_set_error("TERRAX_WASM_OOM", "indexed chunk allocation failed");
        return 0;
    }
    memcpy(indices, source, bytes);

    TxPixelArtChunk* chunk = (TxPixelArtChunk*)tx_alloc(sizeof(TxPixelArtChunk));
    if (!chunk) {
        tx_internal_free(indices);
        tx_set_error("TERRAX_WASM_OOM", "indexed chunk node allocation failed");
        return 0;
    }

    chunk->cx = (int32_t)chunk_x;
    chunk->cy = (int32_t)chunk_y;
    chunk->used = used;
    chunk->indices = indices;
    chunk->next = w->pixel_art_chunks;
    w->pixel_art_chunks = chunk;
    w->pixel_art_chunk_table[chunk_y * w->pixel_art_chunk_cols + chunk_x] = chunk;
    return 1;
}

int txw_add_pixel_art_chunks_bulk_fast(
    uint32_t handle,
    uint32_t records_ptr,
    uint32_t records_len,
    uint32_t record_count)
{
    TxWorld* w = tx_get_world(handle);
    if (w && !tx_world_require_writable(w)) return -1;
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
        if (!validate_bulk_fast_record(w, records, index)) return -1;
    }

    for (uint32_t index = 0u; index < record_count; index++) {
        const uint8_t* record = records + index * TX_PIXEL_ART_BULK_RECORD_BYTES;
        if (!attach_validated_bulk_record(w, record)) {
            /* Keep successfully attached earlier records rooted if allocation
             * fails midway, matching the legacy bulk API's retry-safe state. */
            protect_bulk_pixel_art_allocations(w);
            return -1;
        }
    }

    protect_bulk_pixel_art_allocations(w);
    tx_clear_error();
    return 0;
}

