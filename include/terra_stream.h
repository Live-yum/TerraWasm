#ifndef TERRA_STREAM_H
#define TERRA_STREAM_H
#include <stdint.h>
enum TxStreamEventKind { TX_STREAM_MORE, TX_STREAM_NEED_SOURCE, TX_STREAM_NEED_PIXELS, TX_STREAM_OUTPUT, TX_STREAM_READY };
/* SOURCE/OUTPUT ranges are <=1 MiB. An outstanding event repeats until supplied/acked.
 * READY.reserved: 0=adoptable WLD/open, 1=PNG, 2=MAP, 3=JSON; otherwise zero.
 * Read-only tasks cannot adopt. Closing/cancelling them retains the active world. */
typedef struct TxStreamEvent {
    uint32_t abi_version, kind, source_id, offset, length, data_ptr;
    uint32_t first_column, column_count, completed_columns, total_columns, result_size, reserved;
} TxStreamEvent;
typedef struct TxStreamPixelSpec {
    uint32_t abi_version;
    int32_t start_x, start_y;
    uint32_t width, height, resolved_maps_ptr, resolved_maps_count;
    uint32_t default_palette_index, preview_width, reserved;
} TxStreamPixelSpec;
/* Additive input ownership protocol. Event/spec ABI remains version 1.
 * Allocate the descriptor before acquire; no Wasm calls between filling a fresh
 * HEAPU8 view and commit. Memory growth invalidates the lease. A failed commit
 * never consumes the event. Cancel/close invalidate every borrowed pointer. */
typedef struct TxStreamInputLease {
    uint32_t abi_version, lease_id, source_id, offset, length, data_ptr, capacity, memory_bytes;
} TxStreamInputLease;
/* Counters saturate at UINT32_MAX; source_bytes / decoded_tile_bytes is read
 * amplification (metadata bytes are included in source_bytes). No heap metric
 * here describes process RSS or the platform's file-system cache. */
typedef struct TxStreamStats {
    uint32_t abi_version, source_requests, source_bytes, decoded_tile_bytes;
    uint32_t tile_records, tile_scan_passes, input_copy_bytes, tile_copy_bytes;
    uint32_t output_bytes, cache_hits, cache_misses, cache_bytes, candidate_count;
    uint32_t cache_copy_bytes;
} TxStreamStats;
uint32_t terra_world_stream_abi_version(void);
int32_t terra_world_stream_acquire_input(uint32_t task, TxStreamInputLease* out);
int32_t terra_world_stream_commit_input(uint32_t task, uint32_t lease_id,
    uint32_t source_id, uint32_t offset, uint32_t length);
int32_t terra_world_stream_release_input(uint32_t task, uint32_t lease_id);
int32_t terra_world_stream_get_stats(uint32_t task, TxStreamStats* out);
int32_t terra_world_stream_open_begin(uint32_t source_id, uint32_t source_size, uint32_t* out_task);
int32_t terra_world_stream_pixel_begin(uint32_t world, const TxStreamPixelSpec* spec, uint32_t* out_task);
int32_t terra_world_stream_step(uint32_t task, uint32_t work_units, TxStreamEvent* event);
int32_t terra_world_stream_supply_source(uint32_t task,uint32_t source_id,uint32_t offset,const uint8_t* bytes,uint32_t length);
int32_t terra_world_stream_supply_pixels(uint32_t task,const uint8_t* records,uint32_t byte_length,uint32_t record_count);
int32_t terra_world_stream_ack_output(uint32_t task);
int32_t terra_world_stream_adopt(uint32_t task,uint32_t published_source_id,uint32_t* out_world);
int32_t terra_world_stream_cancel(uint32_t task);
int32_t terra_world_stream_close(uint32_t task);
int32_t terra_world_stream_operation_begin(uint32_t world,const char* operation,const char* request,uint32_t* out_task);
#endif
