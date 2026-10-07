#include "terra_types.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

extern int read_tile_at(TxWorld *, uint32_t *, uint32_t, TxTile *);
extern void write_tile(TxWorld *, TxBuf *, const TxTile *, uint32_t);
extern void buf_init(TxBuf *, uint32_t);
extern void tx_internal_free(void *);

static void byte(TxBuf* b,uint32_t value){assert(b->len<b->cap);b->data[b->len++]=(uint8_t)value;}
static void word(TxBuf* b,uint32_t value){byte(b,value);byte(b,value>>8);}
static void legacy_packets(void){
    /* Independent LoadWorld_Version1 field order; never use the writer to
     * manufacture its expected bytes. Include all historical frame exceptions. */
    const uint16_t types[]={3,4,19,35,49,144,171,321};
    uint8_t important[88]={0};for(unsigned i=0;i<sizeof(types)/sizeof(types[0]);i++)important[types[i]/8]|=1u<<(types[i]%8);
    for(uint32_t v=1;v<88;v++)for(unsigned k=0;k<sizeof(types)/sizeof(types[0]);k++){
        uint32_t type=types[k];if(v<=77&&type>255)continue;
        uint8_t bytes[64];TxBuf expected={bytes,0,sizeof(bytes),1};
        int frame=!(v<28&&type==4)&&!(v<40&&type==19)&&type!=49;
        byte(&expected,1);if(v<=77)byte(&expected,type);else word(&expected,type);
        if(frame){word(&expected,18);word(&expected,type==144?0:36);}
        if(v>=48){byte(&expected,1);byte(&expected,7);}
        if(v<=25)byte(&expected,0);
        byte(&expected,1);byte(&expected,8);if(v>=48){byte(&expected,1);byte(&expected,9);}
        byte(&expected,1);byte(&expected,77);byte(&expected,0);if(v>=51)byte(&expected,1);
        if(v>=33)byte(&expected,1);if(v>=43){byte(&expected,1);byte(&expected,0);}
        if(v>=41){byte(&expected,v<49);if(v>=49)byte(&expected,3);}
        if(v>=42){byte(&expected,1);byte(&expected,1);}if(v>=25)word(&expected,7);
        TxWorld w={0};w.version=v;w.legacy_wld=1;w.file=bytes;w.file_len=expected.len;
        w.important=important;w.important_len=sizeof(important);w.tile_type_count=700;
        TxTile tile;uint32_t off=0;assert(read_tile_at(&w,&off,w.file_len,&tile)&&off==w.file_len);
        assert(tile.type==type&&tile.wall==8&&tile.liquid_amount==77);
        assert(tile.liquid_type==(v>=51?3:1)&&tile.same==(v>=25?7:0));
        TxBuf encoded;buf_init(&encoded,512);write_tile(&w,&encoded,&tile,tile.same);
        assert(encoded.ok&&encoded.len==expected.len&&!memcmp(encoded.data,bytes,expected.len));
        if(v<25){encoded.len=0;write_tile(&w,&encoded,&tile,7);assert(encoded.ok&&encoded.len==expected.len*8);for(unsigned i=0;i<8;i++)assert(!memcmp(encoded.data+i*expected.len,bytes,expected.len));}
        tile.wire_yellow=1;encoded.len=0;write_tile(&w,&encoded,&tile,0);assert(!encoded.ok&&encoded.len==0);
        tx_internal_free(encoded.data);
    }
}

int main(void) {
    legacy_packets();
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
