#ifndef TERRA_ICON_H
#define TERRA_ICON_H

#include <stdint.h>
#include "terra_types.h"

/* Store a copied RGBA atlas in the active world. The bridge pointers remain
 * caller-owned and may be released immediately after this function returns. */
int32_t txw_set_icon_atlas(
    uint32_t handle,
    uint32_t rgba_ptr,
    uint32_t icon_size,
    uint32_t icon_count,
    uint32_t atlas_width,
    uint32_t atlas_height,
    uint32_t item_ids_ptr,
    uint32_t x_offsets_ptr,
    uint32_t y_offsets_ptr);

int32_t txw_clear_icon_atlas(uint32_t handle);

int terra_icon_index_for_item(const TxIconAtlas* atlas, int32_t item_id);
uint32_t terra_icon_side_for_radius(uint32_t radius);

#endif /* TERRA_ICON_H */
