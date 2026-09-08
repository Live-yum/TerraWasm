#include "terra_types.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

extern int read_tile_at(TxWorld *, uint32_t *, uint32_t, TxTile *);
extern void write_tile(TxWorld *, TxBuf *, const TxTile *, uint32_t);
extern void buf_init(TxBuf *, uint32_t);
extern void tx_internal_free(void *);

int main(void) {
    uint8_t important[88] = {0};
    important[321 / 8] = 1u << (321 % 8);
    /* WorldFile.LoadWorldTiles reads wall-low, wall-paint, liquid, wall-high,
     * then RLE. The high wall byte is not adjacent to the low one. */
    uint8_t wall_packet[] = {0x4d, 1, 0x50, 1, 7, 99, 1, 2};
    uint8_t full_packet[] = {
        0xaf, 0x3f, 0xff, 0x1e,
        0x41, 1, 18, 0, 36, 0, 7, 1, 8, 77, 1, 1, 1
    };
    for (unsigned packet = 0; packet < 2; packet++) {
        uint8_t *data = packet ? full_packet : wall_packet;
        uint32_t size = packet ? sizeof(full_packet) : sizeof(wall_packet);
        TxWorld world = {0};
        world.version = 326;
        world.file = data;
        world.file_len = size;
        world.important = important;
        world.important_len = sizeof(important);
        world.tile_type_count = 700;
        TxTile tile;
        uint32_t offset = 0;
        assert(read_tile_at(&world, &offset, size, &tile));
        assert(offset == size);
        assert(tile.wall == 257);
        assert(tile.wall_color == (packet ? 8 : 7));
        assert(tile.liquid_amount == (packet ? 77 : 99));
        assert(tile.liquid_type == (packet ? 4 : 1));
        assert(tile.same == (packet ? 257 : 2));
        if (packet) {
            assert(tile.type == 321 && tile.frame_x == 18 && tile.frame_y == 36);
            assert(tile.tile_color == 7 && tile.brick_style == 3);
            assert(tile.fullbright_block && tile.invisible_wall && tile.wire_yellow);
        }
        TxBuf encoded;
        buf_init(&encoded, size);
        write_tile(&world, &encoded, &tile, tile.same);
        assert(encoded.ok && encoded.len == size);
        assert(memcmp(encoded.data, data, size) == 0);
        tx_internal_free(encoded.data);
        for (uint32_t prefix = 0; prefix < size; prefix++) {
            offset = 0;
            assert(!read_tile_at(&world, &offset, prefix, &tile));
        }
    }
    return 0;
}
