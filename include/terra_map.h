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
} MapMarkerEntry;

int32_t txw_set_marker_color_index(
    uint32_t handle,
    uint32_t data_ptr,
    uint32_t data_len);

void txw_clear_marker_color_index(TxWorld* world);

int32_t terra_generate_map(TxWorld* world);

int32_t terra_render_lit_map(
    TxWorld* world,
    const int32_t* item_ids,
    uint32_t item_id_count,
    const int32_t* tile_types,
    uint32_t tile_type_count,
    uint32_t mark_chests);

int32_t terra_render_lit_map_marked(
    TxWorld* world,
    const MapMarkerEntry* chest_markers,
    uint32_t chest_count,
    const MapMarkerEntry* tile_markers,
    uint32_t tile_count,
    uint32_t* matched_chest_count,
    uint32_t* matched_tile_count);

#endif /* TERRA_MAP_H */
