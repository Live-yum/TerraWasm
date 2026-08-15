/*
 * terra_icon.c -- Marker item thumbnail atlas ownership and sizing helpers.
 */
#include "terra_icon.h"

extern void* memset(void* dst, int value, unsigned long n);
extern void* memcpy(void* dst, const void* src, unsigned long n);

extern TxWorld* tx_get_world(uint32_t handle);
extern uint8_t* tx_alloc(uint32_t size);
extern void tx_internal_free(void* ptr);
extern uint32_t tx_mark(void);
extern int tx_bridge_range_is_valid(uint32_t ptr, uint32_t length);
extern void tx_set_error(const char* code, const char* message);
extern void tx_clear_error(void);

static void clear_icon_atlas(TxWorld* world) {
    if (!world) return;
    if (world->icon_atlas.rgba) tx_internal_free(world->icon_atlas.rgba);
    memset(&world->icon_atlas, 0, sizeof(world->icon_atlas));
}

static int icon_atlas_dimensions_valid(
        uint32_t icon_size, uint32_t icon_count,
        uint32_t atlas_width, uint32_t atlas_height,
        uint32_t* rgba_bytes) {
    uint64_t required;
    if (icon_size == 0u || icon_size > 128u ||
        icon_count == 0u || icon_count > TX_ICON_ATLAS_MAX ||
        atlas_width == 0u || atlas_height == 0u ||
        atlas_width > 4096u || atlas_height > 4096u) {
        return 0;
    }
    required = (uint64_t)atlas_width * (uint64_t)atlas_height * 4u;
    if (required == 0u || required > UINT32_MAX) return 0;
    if (rgba_bytes) *rgba_bytes = (uint32_t)required;
    return 1;
}

int32_t txw_set_icon_atlas(
        uint32_t handle,
        uint32_t rgba_ptr,
        uint32_t icon_size,
        uint32_t icon_count,
        uint32_t atlas_width,
        uint32_t atlas_height,
        uint32_t item_ids_ptr,
        uint32_t x_offsets_ptr,
        uint32_t y_offsets_ptr) {
    TxWorld* world = tx_get_world(handle);
    uint32_t rgba_bytes = 0u;
    uint32_t table_bytes;
    uint8_t* copied_rgba;
    const uint32_t* item_ids;
    const uint32_t* x_offsets;
    const uint32_t* y_offsets;

    if (!world) {
        tx_set_error("TERRAX_INVALID_HANDLE", "world handle is stale or invalid");
        return -1;
    }

    if (!rgba_ptr && !icon_size && !icon_count && !atlas_width && !atlas_height &&
        !item_ids_ptr && !x_offsets_ptr && !y_offsets_ptr) {
        clear_icon_atlas(world);
        world->heap_mark = tx_mark();
        world->last_op_heap_end = world->heap_mark;
        tx_clear_error();
        return 0;
    }

    if (!rgba_ptr || !item_ids_ptr || !x_offsets_ptr || !y_offsets_ptr ||
        !icon_atlas_dimensions_valid(icon_size, icon_count, atlas_width, atlas_height, &rgba_bytes)) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "invalid marker icon atlas dimensions or pointers");
        return -1;
    }

    table_bytes = icon_count * sizeof(uint32_t);
    if (!tx_bridge_range_is_valid(rgba_ptr, rgba_bytes) ||
        !tx_bridge_range_is_valid(item_ids_ptr, table_bytes) ||
        !tx_bridge_range_is_valid(x_offsets_ptr, table_bytes) ||
        !tx_bridge_range_is_valid(y_offsets_ptr, table_bytes)) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "marker icon atlas payload exceeds its bridge allocation");
        return -1;
    }

    item_ids = (const uint32_t*)(uintptr_t)item_ids_ptr;
    x_offsets = (const uint32_t*)(uintptr_t)x_offsets_ptr;
    y_offsets = (const uint32_t*)(uintptr_t)y_offsets_ptr;
    for (uint32_t i = 0u; i < icon_count; i++) {
        if (x_offsets[i] > atlas_width || y_offsets[i] > atlas_height ||
            icon_size > atlas_width - x_offsets[i] ||
            icon_size > atlas_height - y_offsets[i]) {
            tx_set_error("TERRAX_INVALID_ARGUMENT", "marker icon atlas entry exceeds atlas bounds");
            return -1;
        }
    }

    copied_rgba = tx_alloc(rgba_bytes);
    if (!copied_rgba) {
        tx_set_error("TERRAX_WASM_OOM", "marker icon atlas allocation failed");
        return -1;
    }
    memcpy(copied_rgba, (const void*)(uintptr_t)rgba_ptr, rgba_bytes);

    clear_icon_atlas(world);
    world->icon_atlas.rgba = copied_rgba;
    world->icon_atlas.icon_size = icon_size;
    world->icon_atlas.icon_count = icon_count;
    world->icon_atlas.atlas_width = atlas_width;
    world->icon_atlas.atlas_height = atlas_height;
    memcpy(world->icon_atlas.item_ids, item_ids, table_bytes);
    memcpy(world->icon_atlas.x_offsets, x_offsets, table_bytes);
    memcpy(world->icon_atlas.y_offsets, y_offsets, table_bytes);

    /* Keep the copied atlas above all transient operation allocations. */
    world->heap_mark = tx_mark();
    world->last_op_heap_end = world->heap_mark;
    tx_clear_error();
    return (int32_t)icon_count;
}

int32_t txw_clear_icon_atlas(uint32_t handle) {
    TxWorld* world = tx_get_world(handle);
    if (!world) {
        tx_set_error("TERRAX_INVALID_HANDLE", "world handle is stale or invalid");
        return -1;
    }
    clear_icon_atlas(world);
    world->heap_mark = tx_mark();
    world->last_op_heap_end = world->heap_mark;
    tx_clear_error();
    return 0;
}

int terra_icon_index_for_item(const TxIconAtlas* atlas, int32_t item_id) {
    if (!atlas || !atlas->rgba || atlas->icon_count == 0u) return -1;
    for (uint32_t i = 0u; i < atlas->icon_count && i < TX_ICON_ATLAS_MAX; i++) {
        if (atlas->item_ids[i] == (uint32_t)item_id) return (int)i;
    }
    return -1;
}

uint32_t terra_icon_side_for_radius(uint32_t radius) {
    uint32_t side;
    uint64_t radius_squared;
    if (radius < 2u) return 0u;

    /* The icon is square, so side*sqrt(2) <= radius. */
    side = (radius * 707u) / 1000u;
    if (side == 0u) side = 1u;
    radius_squared = (uint64_t)radius * radius;
    while (side > 0u && (uint64_t)side * side * 2u > radius_squared) side--;
    return side;
}
