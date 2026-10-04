#ifndef TERRA_MAP_RUNTIME_H
#define TERRA_MAP_RUNTIME_H

#include <stdint.h>

/* TMRT v1 is a little-endian, owned, process-wide MAP palette. See docs/map-runtime.md. */
#define TX_MAP_RUNTIME_MAGIC 0x54524d54u
#define TX_MAP_RUNTIME_SCHEMA 1u
#define TX_MAP_RUNTIME_FORMAT 33083u
#define TX_MAP_RUNTIME_HEADER_SIZE 96u
#define TX_MAP_RUNTIME_MAX_BYTES (1024u * 1024u)

typedef struct TxMapRuntimeLayout {
    uint32_t cur_release;
    uint32_t tile_count;
    uint32_t wall_count;
    uint32_t palette_count;
    uint32_t tile_pos;
    uint32_t wall_pos;
    uint32_t liquid_pos;
    uint32_t sky_pos;
    uint32_t dirt_pos;
    uint32_t rock_pos;
    uint32_t hell_pos;
    uint32_t paint_count;
} TxMapRuntimeLayout;

/* Public WASM bridge setter. (0,0) clears when no world is open. */
int32_t txw_set_map_runtime(uint32_t data_ptr, uint32_t data_len);
/* Native/test setter; copies data and never retains the caller's buffer. */
int32_t txw_set_map_runtime_from_buffer(const uint8_t* data, uint32_t data_len);
/* Select compiled colors without closing edited worlds; requires idle tasks. */
int32_t txw_use_builtin_map_runtime(void);

int tx_map_runtime_is_set(void);
const TxMapRuntimeLayout* tx_map_runtime_layout(void);
int tx_map_runtime_lookup(int wall, uint32_t type, uint16_t* map_index, uint8_t* options);
int tx_map_runtime_color(uint32_t map_index, uint8_t rgba[4]);
int tx_map_runtime_paint(uint32_t paint_id, uint8_t rgb[3]);

#endif
