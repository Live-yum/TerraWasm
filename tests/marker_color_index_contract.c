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

#define TEST_BRICK_COUNT 512u
#define TEST_DIRECTORY_OFFSET 64u
#define TEST_PAYLOAD_OFFSET (TEST_DIRECTORY_OFFSET + TEST_BRICK_COUNT * TXCI_DIR_SIZE)
#define TEST_DATA_SIZE (TEST_PAYLOAD_OFFSET + 2u)

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

static void make_valid_index(uint8_t data[TEST_DATA_SIZE]) {
    memset(data, 0, TEST_DATA_SIZE);

    write_u32le(data, 0u, TXCI_MAGIC);
    write_u16le(data, 4u, TXCI_VERSION);
    write_u16le(data, 6u, 32u);
    write_u32le(data, 8u, 1u);
    write_u32le(data, 12u, 1u);
    write_u32le(data, 16u, TEST_BRICK_COUNT);
    write_u32le(data, 20u, TXCI_HEADER_SIZE);
    write_u32le(data, 24u, 48u);
    write_u32le(data, 28u, 56u);
    write_u32le(data, 32u, TEST_DIRECTORY_OFFSET);
    write_u32le(data, 36u, TEST_PAYLOAD_OFFSET);

    data[44u] = 17u;
    data[45u] = 34u;
    data[46u] = 51u;
    write_u32le(data, 48u, 0u);
    write_u32le(data, 52u, 1u);

    write_u16le(data, 56u, 42u);
    write_u16le(data, 58u, 7u);
    data[60u] = 3u;

    for (uint32_t brick = 0u; brick < TEST_BRICK_COUNT; brick++) {
        uint32_t directory = TEST_DIRECTORY_OFFSET + brick * TXCI_DIR_SIZE;
        data[directory] = TXCI_BLOCK_UNIFORM;
        write_u32le(data, directory + 4u, 0u);
    }
    write_u16le(data, TEST_PAYLOAD_OFFSET, 0u);
}

static void assert_known_lookup(const TxWorld* world) {
    TxciItem item;
    assert(txci_lookup_group(&world->marker_color_index, 17u, 34u, 51u) == 0);
    assert(txci_choose_tile(&world->marker_color_index, 17u, 34u, 51u, 0, &item) == 1);
    assert(item.type_id == 42u);
    assert(item.variant == 7u);
    assert(item.paint_id == 3u);
}

int main(void) {
    uint8_t valid[TEST_DATA_SIZE];
    uint8_t invalid_version[TEST_DATA_SIZE];
    uint8_t truncated[TXCI_HEADER_SIZE - 1u];
    TxWorld world = {0};
    uint32_t baseline = tx_native_heap_used();

    make_valid_index(valid);
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

    txw_clear_marker_color_index(&world);
    assert(tx_native_heap_used() == baseline);
    puts("marker color index contract: failed replacements preserve active index");
    return 0;
}
