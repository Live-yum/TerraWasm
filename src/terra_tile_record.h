#ifndef TERRA_TILE_RECORD_H
#define TERRA_TILE_RECORD_H
#include "terra_types.h"
#include <string.h>
/* Internal interchange shared by extraction and sparse streaming overlay. No
 * native structure padding, pointers, RLE counts or JS objects cross the bridge. */
static uint32_t tx_record_wires(const TxTile* t){return t->wire_red|(t->wire_blue<<1)|(t->wire_green<<2)|(t->wire_yellow<<3);}
static void tx_tile_record(uint32_t* r,uint32_t x,uint32_t y,const TxTile* t,uint32_t wires){
    uint32_t flags=t->active|(t->actuator<<1)|(t->inactive<<2)|(t->invisible_block<<3)|(t->invisible_wall<<4)|(t->fullbright_block<<5)|(t->fullbright_wall<<6);
    r[0]=x;r[1]=y;r[2]=t->type|(flags<<16);r[3]=(uint16_t)t->frame_x|((uint32_t)(uint16_t)t->frame_y<<16);
    r[4]=t->wall|((uint32_t)t->tile_color<<16)|((uint32_t)t->wall_color<<24);
    r[5]=t->liquid_amount|((uint32_t)t->liquid_type<<8)|((uint32_t)t->brick_style<<16)|(wires<<24);r[6]=r[7]=0;
}
static int tx_tile_unrecord(const uint32_t* r,TxTile* t){
    uint32_t flags=r[2]>>16,wires=r[5]>>24,liquid=(r[5]>>8)&255u,brick=(r[5]>>16)&255u;
    if((flags&~127u)||wires>15u||liquid>4u||brick>7u||r[6]||r[7]||(!(r[5]&255u)&&liquid)||((r[5]&255u)&&!liquid))return 0;
    memset(t,0,sizeof(*t));t->type=(uint16_t)r[2];t->active=flags&1;t->actuator=(flags>>1)&1;t->inactive=(flags>>2)&1;
    t->invisible_block=(flags>>3)&1;t->invisible_wall=(flags>>4)&1;t->fullbright_block=(flags>>5)&1;t->fullbright_wall=(flags>>6)&1;
    t->frame_x=(int16_t)r[3];t->frame_y=(int16_t)(r[3]>>16);t->wall=(uint16_t)r[4];t->tile_color=(uint8_t)(r[4]>>16);t->wall_color=(uint8_t)(r[4]>>24);
    t->liquid_amount=(uint8_t)r[5];t->liquid_type=(uint8_t)liquid;t->brick_style=(uint8_t)brick;
    t->wire_red=wires&1;t->wire_blue=(wires>>1)&1;t->wire_green=(wires>>2)&1;t->wire_yellow=(wires>>3)&1;return 1;
}
/* These objects require accompanying section records. Until those records have
 * an explicit transfer protocol, callers must never silently manufacture empty
 * inventories, lose sign text, or leave an orphaned tile entity. */
static int tx_tile_needs_section(uint32_t type){
    switch(type){case 21:case 55:case 85:case 88:case 378:case 395:case 423:case 425:case 467:case 470:case 471:case 475:case 520:case 573:case 597:return 1;default:return 0;}
}
static int tx_tile_framed(const TxWorld* w,uint32_t type){return (type>>3)<w->important_len&&(w->important[type>>3]&(1u<<(type&7)));}
#endif
