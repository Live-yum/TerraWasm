#ifndef TERRA_MAP_H
#define TERRA_MAP_H

#include <stdint.h>
#include "terra_types.h"

/* Keep the native MAP result below the Web WASM output contract. */
#define TX_MAP_MAX_OUTPUT_BYTES (128u * 1024u * 1024u)

typedef struct MapMarkerEntry {
    int32_t id;
    uint32_t map_value; /* legacy fallback; marker RGB is resolved at MAP render time */
    uint8_t rgba[4];
    uint8_t radius;
    uint8_t line_width;
    uint8_t reserved[2];
    /* 0: legacy tile paint; 1: frame anchor; 2: eight-connected vein. */
    int32_t locate;
    int32_t frame_x, frame_y, frame_x_mod, frame_y_mod;
} MapMarkerEntry;

typedef struct TxMarkerPoint {
    int32_t x, y;
    uint32_t marker_index;
} TxMarkerPoint;

int tx_locate_tile_markers(TxWorld* world, const MapMarkerEntry* markers,
                          uint32_t count, TxBuf* points);

int32_t txw_set_marker_color_index(
    uint32_t handle,
    uint32_t data_ptr,
    uint32_t data_len);

void txw_clear_marker_color_index(TxWorld* world);

int32_t terra_generate_map(TxWorld* world);

int32_t terra_render_lit_map_marked(
    TxWorld* world,
    const MapMarkerEntry* chest_markers,
    uint32_t chest_count,
    const MapMarkerEntry* tile_markers,
    uint32_t tile_count,
    uint32_t* matched_chest_count,
    uint32_t* matched_tile_count);

#endif /* TERRA_MAP_H */
