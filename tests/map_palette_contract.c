/* Run: emcc -O2 -Iinclude tests/map_palette_contract.c -sENVIRONMENT=node
 * -sUSE_ZLIB=1 -o build/map_palette_contract.js && node build/map_palette_contract.js
 * Repeat with -DMAP_CONTRACT=1 to exercise the .map path. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "terra_output.h"
extern void tx_persistent_free(void*);
#ifdef MAP_CONTRACT
#include "../src/terra_map.c"
#else
#include "../src/terra_render_core.inc"
#include "../src/terra_defaults.inc"
#endif
const uint8_t* tx_get_tile_colors(void) { return NULL; }
uint32_t tx_get_tile_color_count(void) { return 0; }
const uint8_t* tx_get_wall_colors(void) { return NULL; }
uint32_t tx_get_wall_color_count(void) { return 0; }

int main(void) {
    /* Independently copied from Terraria 1.4.5.8 MapHelper.Initialize. */
    const uint16_t ids[] = {106,107,145,150,152,317,347,366};
    const uint16_t lookups[] = {1164,1165,1203,1208,1210,1373,1402,1421};
    const uint8_t rgb[][3] = {{148,116,74},{60,60,60},{144,144,144},
        {73,59,50},{102,75,34},{143,117,121},{100,65,130},{60,60,60}};
    const uint8_t paints[] = {0,1,14,25,26,27,28,29,30};
    TxTile tile = {0}; uint8_t out[4];
    for (unsigned i=0;i<sizeof(ids)/sizeof(ids[0]);i++) {
        tile.wall = ids[i];
        assert(TX_MAP_WALL_TYPE_COUNTS[ids[i]] == 1);

        TxciItem item = {0}; item.is_wall=1; item.type_id=ids[i];
        for (unsigned j=0;j<sizeof(paints);j++) {
            const uint8_t paint = paints[j]; tile.wall_color=paint; item.paint_id=paint;
#ifndef MAP_CONTRACT
            color_for_tile(&tile,1,100,35,65,out);
            for(unsigned c=0;c<3;c++) {
                unsigned expected = rgb[i][c];
                unsigned max = rgb[i][0]>rgb[i][1]?rgb[i][0]:rgb[i][1];
                if (rgb[i][2]>max) max=rgb[i][2];
                if (paint==30) expected=(255-expected)/2;
                else if (paint==29) {
                    unsigned rg=rgb[i][0]>rgb[i][1]?rgb[i][0]:rgb[i][1];
                    unsigned shadow=rgb[i][2]<rg?rgb[i][2]:rg;
                    expected=(uint8_t)(25*((float)shadow/255)*0.3f);
                } else if(paint) expected=TX_PAINT_COLORS[(paint-1)*3+c]*max/255;
                assert(out[c] == expected);
            }
            assert(out[3]==255);
#else
            uint32_t value; assert(map_value_for_txci_item(&item,&value));
            assert(value == (lookups[i] | (255u<<16) | ((uint32_t)paint<<24)));
            assert(map_value_for_tile(&tile)==value);
#endif
        }
    }
    memset(&tile,0,sizeof(tile)); tile.active=1; tile.type=1; tile.tile_color=30;
#ifndef MAP_CONTRACT
    color_for_tile(&tile,1,100,35,65,out); assert(out[0]==127 && out[1]==127 && out[2]==127);
#else
    assert((map_value_for_tile(&tile)&65535)==2);
#endif
    tile.type=708; tile.tile_color=28;
#ifndef MAP_CONTRACT
    color_for_tile(&tile,1,100,35,65,out);
    assert(out[0]==244 && out[1]==170 && out[2]==119); /* default portrait face */
#else
    assert((map_value_for_tile(&tile)&65535)==1019);
#endif
    assert(TX_MAP_TILE_COUNT==754 && TX_MAP_MAX_WALL_ID==1421);
    puts("1.4.5.8 wall PNG colors, paints and MAP lookups passed");
    return 0;
}
