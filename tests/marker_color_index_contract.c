#include "terra_map.h"
#include "terra_txci.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern uint32_t tx_native_heap_used(void);
extern int32_t txw_set_marker_color_index_from_buffer(
    TxWorld* world,
    const uint8_t* data,
    uint32_t data_len);
extern int txw_test_marker_map_value_for_rgb(
    const TxciIndex* index, uint8_t r, uint8_t g, uint8_t b, uint32_t* out_value);

#define TEST_BRICK_COUNT 512u
#define TEST_ITEMS_OFFSET 56u
#define INDEX_DIRECTORY_OFFSET(item_count) \
    (TEST_ITEMS_OFFSET + (item_count) * TXCI_ITEM_SIZE)
#define INDEX_PAYLOAD_OFFSET(item_count) \
    (INDEX_DIRECTORY_OFFSET(item_count) + TEST_BRICK_COUNT * TXCI_DIR_SIZE)
#define INDEX_DATA_SIZE(item_count) (INDEX_PAYLOAD_OFFSET(item_count) + 2u)
#define SINGLE_ITEM_COUNT 1u
#define MANY_ITEM_COUNT 34u
#define LATE_WALL_ITEM_COUNT 306u
#define LATE_TILE_ITEM_COUNT 307u

static void write_u16le(uint8_t* data, uint32_t offset, uint16_t value) {
    data[offset] = (uint8_t)value;
    data[offset + 1u] = (uint8_t)(value >> 8u);
}

static void write_u32le(uint8_t* data, uint32_t offset, uint32_t value) {
    data[offset] = (uint8_t)value;
    data[offset + 1u] = (uint8_t)(value >> 8u);
    data[offset + 2u] = (uint8_t)(value >> 16u);
    data[offset + 3u] = (uint8_t)(value >> 24u);
}

static void make_index(uint8_t* data, uint32_t data_size, uint32_t item_count) {
    uint32_t directory_offset = INDEX_DIRECTORY_OFFSET(item_count);
    uint32_t payload_offset = INDEX_PAYLOAD_OFFSET(item_count);
    assert(data_size == INDEX_DATA_SIZE(item_count));
    memset(data, 0, data_size);

    write_u32le(data, 0u, TXCI_MAGIC);
    write_u16le(data, 4u, TXCI_VERSION);
    write_u16le(data, 6u, 32u);
    write_u32le(data, 8u, 1u);
    write_u32le(data, 12u, item_count);
    write_u32le(data, 16u, TEST_BRICK_COUNT);
    write_u32le(data, 20u, TXCI_HEADER_SIZE);
    write_u32le(data, 24u, 48u);
    write_u32le(data, 28u, TEST_ITEMS_OFFSET);
    write_u32le(data, 32u, directory_offset);
    write_u32le(data, 36u, payload_offset);

    data[44u] = 17u;
    data[45u] = 34u;
    data[46u] = 51u;
    write_u32le(data, 48u, 0u);
    write_u32le(data, 52u, item_count);

    for (uint32_t item = 0u; item < item_count; item++) {
        uint32_t offset = TEST_ITEMS_OFFSET + item * TXCI_ITEM_SIZE;
        write_u16le(data, offset, (uint16_t)(item + 1u));
        write_u16le(data, offset + 2u, (uint16_t)item);
        data[offset + 4u] = (uint8_t)(item & 31u);
    }

    for (uint32_t brick = 0u; brick < TEST_BRICK_COUNT; brick++) {
        uint32_t directory = directory_offset + brick * TXCI_DIR_SIZE;
        data[directory] = TXCI_BLOCK_UNIFORM;
        write_u32le(data, directory + 4u, 0u);
    }
    write_u16le(data, payload_offset, 0u);
}

static void make_single_index(uint8_t data[INDEX_DATA_SIZE(SINGLE_ITEM_COUNT)]) {
    make_index(data, INDEX_DATA_SIZE(SINGLE_ITEM_COUNT), SINGLE_ITEM_COUNT);
    write_u16le(data, TEST_ITEMS_OFFSET, 42u);
    write_u16le(data, TEST_ITEMS_OFFSET + 2u, 7u);
    data[TEST_ITEMS_OFFSET + 4u] = 3u;
}

static void make_many_options_index(uint8_t data[INDEX_DATA_SIZE(MANY_ITEM_COUNT)]) {
    make_index(data, INDEX_DATA_SIZE(MANY_ITEM_COUNT), MANY_ITEM_COUNT);
    write_u16le(
        data,
        TEST_ITEMS_OFFSET + (MANY_ITEM_COUNT - 1u) * TXCI_ITEM_SIZE,
        (uint16_t)(TXCI_KIND_WALL | 77u));
}

static void make_late_candidate_index(uint8_t* data, uint32_t count,
                                      uint8_t last_is_wall, uint16_t last_type) {
    make_index(data, INDEX_DATA_SIZE(count), count);
    for (uint32_t item = 0u; item < count; item++) {
        uint32_t offset = TEST_ITEMS_OFFSET + item * TXCI_ITEM_SIZE;
        /* Opposite-kind early candidates keep the preferred valid item last. */
        write_u16le(data, offset,
                    (uint16_t)(8000u | (last_is_wall ? 0u : TXCI_KIND_WALL)));
        write_u16le(data, offset + 2u, 0u);
        data[offset + 4u] = 0u;
    }
    uint32_t last = TEST_ITEMS_OFFSET + (count - 1u) * TXCI_ITEM_SIZE;
    write_u16le(data, last, (uint16_t)(last_type | (last_is_wall ? TXCI_KIND_WALL : 0u)));
    data[last + 4u] = 3u;
}

static void assert_known_lookup(const TxWorld* world) {
    TxciItem item;
    assert(txci_lookup_group(&world->marker_color_index, 17u, 34u, 51u) == 0);
    assert(txci_choose_tile(&world->marker_color_index, 17u, 34u, 51u, 0, &item) == 1);
    assert(item.type_id == 42u);
    assert(item.variant == 7u);
    assert(item.paint_id == 3u);
}

static void assert_complete_group_selection(const TxWorld* world) {
    TxciItem item;
    assert(txci_choose_tile(&world->marker_color_index, 17u, 34u, 51u, 1, &item) == 1);
    assert(item.is_wall == 1u);
    assert(item.type_id == 77u);

    assert(txci_choose_tile(&world->marker_color_index, 17u, 34u, 51u, 0, &item) == 1);
    assert(item.is_wall == 0u);
    assert(item.type_id == 1u);
}

static void assert_late_map_candidate(const TxWorld* world, uint32_t count,
                                      uint8_t is_wall, uint16_t type, uint16_t map_index) {
    TxciItem item;
    uint32_t map_value = 0u;
    assert(world->marker_color_index.item_count == count);
    assert(txci_get_item(&world->marker_color_index, count - 1u, &item) == 1);
    assert(item.type_id == type && item.is_wall == is_wall && item.paint_id == 3u);
    assert(txci_get_item(&world->marker_color_index, count, &item) == 0);
    assert(txci_choose_tile(&world->marker_color_index, 17u, 34u, 51u,
                            is_wall, &item) == 1);
    assert(item.type_id == type && item.is_wall == is_wall);
    assert(txw_test_marker_map_value_for_rgb(
        &world->marker_color_index, 17u, 34u, 51u, &map_value) == 1);
    assert(map_value == ((uint32_t)map_index | (255u << 16u) | (3u << 24u)));
}

static void assert_out_of_range_group_is_rejected(
        uint8_t data[INDEX_DATA_SIZE(SINGLE_ITEM_COUNT)]) {
    TxciIndex index = {0};
    TxciItem item;
    make_single_index(data);
    write_u16le(data, INDEX_PAYLOAD_OFFSET(SINGLE_ITEM_COUNT), 1u);
    assert(txci_load_from_memory(&index, data, INDEX_DATA_SIZE(SINGLE_ITEM_COUNT)) == 1);
    assert(txci_lookup_group(&index, 17u, 34u, 51u) == -1);
    assert(txci_choose_tile(&index, 17u, 34u, 51u, 0, &item) == 0);
    txci_unload(&index);
}

int main(void) {
    uint8_t valid[INDEX_DATA_SIZE(SINGLE_ITEM_COUNT)];
    uint8_t invalid_version[INDEX_DATA_SIZE(SINGLE_ITEM_COUNT)];
    uint8_t invalid_group[INDEX_DATA_SIZE(SINGLE_ITEM_COUNT)];
    uint8_t truncated[TXCI_HEADER_SIZE - 1u];
    uint8_t many_options[INDEX_DATA_SIZE(MANY_ITEM_COUNT)];
    uint8_t late_wall[INDEX_DATA_SIZE(LATE_WALL_ITEM_COUNT)];
    uint8_t late_tile[INDEX_DATA_SIZE(LATE_TILE_ITEM_COUNT)];
    uint8_t invalid_offsets[INDEX_DATA_SIZE(LATE_TILE_ITEM_COUNT)];
    TxWorld world = {0};
    uint32_t baseline = tx_native_heap_used();

    make_single_index(valid);
    assert(txw_set_marker_color_index_from_buffer(&world, valid, sizeof(valid)) == 0);
    assert_known_lookup(&world);

    memcpy(invalid_version, valid, sizeof(invalid_version));
    write_u16le(invalid_version, 4u, TXCI_VERSION - 1u);
    assert(txw_set_marker_color_index_from_buffer(
        &world, invalid_version, sizeof(invalid_version)) == -1);
    assert_known_lookup(&world);

    memset(truncated, 0, sizeof(truncated));
    assert(txw_set_marker_color_index_from_buffer(
        &world, truncated, sizeof(truncated)) == -1);
    assert_known_lookup(&world);

    assert_out_of_range_group_is_rejected(invalid_group);
    assert_known_lookup(&world);

    make_many_options_index(many_options);
    assert(txw_set_marker_color_index_from_buffer(
        &world, many_options, sizeof(many_options)) == 0);
    assert_complete_group_selection(&world);

    make_late_candidate_index(late_wall, LATE_WALL_ITEM_COUNT, 1u, 106u);
    assert(txw_set_marker_color_index_from_buffer(
        &world, late_wall, sizeof(late_wall)) == 0);
    assert_late_map_candidate(&world, LATE_WALL_ITEM_COUNT, 1u, 106u, 1164u);

    make_late_candidate_index(late_tile, LATE_TILE_ITEM_COUNT, 0u, 1u);
    assert(txw_set_marker_color_index_from_buffer(
        &world, late_tile, sizeof(late_tile)) == 0);
    assert_late_map_candidate(&world, LATE_TILE_ITEM_COUNT, 0u, 1u, 2u);

    memcpy(invalid_offsets, late_tile, sizeof(invalid_offsets));
    write_u32le(invalid_offsets, 48u, 1u);
    assert(txw_set_marker_color_index_from_buffer(
        &world, invalid_offsets, sizeof(invalid_offsets)) == -1);
    assert_late_map_candidate(&world, LATE_TILE_ITEM_COUNT, 0u, 1u, 2u);

    memcpy(invalid_offsets, late_tile, sizeof(invalid_offsets));
    write_u32le(invalid_offsets, 52u, LATE_TILE_ITEM_COUNT + 1u);
    assert(txw_set_marker_color_index_from_buffer(
        &world, invalid_offsets, sizeof(invalid_offsets)) == -1);
    assert_late_map_candidate(&world, LATE_TILE_ITEM_COUNT, 0u, 1u, 2u);

    txw_clear_marker_color_index(&world);
    assert(tx_native_heap_used() == baseline);
    puts("marker color index contract: 306/307 options, late MAP candidates and transactional replacement passed");
    return 0;
}
