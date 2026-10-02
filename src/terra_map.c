/*
 * terra_map.c -- Map generation for TerraWasm.
 *
 * Generates 64x64 chunked tile maps from parsed .wld data.
 * Each tile is a uint32: type in bits 0-15, light=255 in bits 16-23,
 * extra/tile_color in bits 24-31. Chunks are compressed with fixed-Huffman zlib.
 *
 * The map format header matches the TerraX C++ highlight_from_world output:
 *   version, magic, revision, favorite, world_name, world_id, dimensions,
 *   tile/wall/liquid counts, gradient counts, options, type counts.
 */
#include <stddef.h>

#include "terra_types.h"
#include "terra_output.h"
#include "terra_stream_map.h"
#include <string.h>
#include "terra_map.h"
#include "terra_icon.h"
#include "terra_color_data.h"
#include "terra_defaults.inc"

/* ---------- Extern declarations from terra_mem.c ---------- */

extern uint8_t* tx_alloc(uint32_t size);
extern void     tx_internal_free(void* ptr);
extern void     tx_set_error(const char* code, const char* message);
extern void     tx_clear_error(void);

extern uint32_t tx_last_ptr;
extern uint32_t tx_last_len;
extern uint32_t tx_last_width;
extern uint32_t tx_last_height;

extern void     buf_init(TxBuf* b, uint32_t cap);
extern int      buf_reserve(TxBuf* b, uint32_t extra);
extern void     buf_u8(TxBuf* b, uint8_t v);
extern void     buf_bytes(TxBuf* b, const void* p, uint32_t n);
extern void     buf_u16le(TxBuf* b, uint32_t v);
extern void     buf_u32le(TxBuf* b, uint32_t v);
extern void     buf_u64le(TxBuf* b, uint64_t v);
extern void     buf_u16be(TxBuf* b, uint32_t v);
extern void     buf_u32be(TxBuf* b, uint32_t v);
extern void     buf_cstr(TxBuf* b, const char* s);

extern int      set_result_buf(TxBuf* b);
extern int      set_result_bytes(uint8_t* p, uint32_t len);

extern TxWorld* tx_get_world(uint32_t handle);
extern uint32_t tx_mark(void);
extern int      tx_bridge_range_is_valid(uint32_t ptr, uint32_t length);
extern void     tx_clear_error(void);

extern const uint8_t* tx_get_tile_colors(void);
extern uint32_t       tx_get_tile_color_count(void);
extern const uint8_t* tx_get_wall_colors(void);
extern uint32_t       tx_get_wall_color_count(void);

/* ---------- Extern declarations from terra_wld.c ---------- */

extern uint8_t  rd_u8(const uint8_t* p, uint32_t len, uint32_t* off);
extern uint16_t rd_u16le(const uint8_t* p, uint32_t len, uint32_t* off);
extern int32_t  rd_i32le(const uint8_t* p, uint32_t len, uint32_t* off);
extern uint32_t rd_7bit(const uint8_t* p, uint32_t len, uint32_t* off, int* ok);

extern int  read_tile_at(TxWorld* w, uint32_t* off, uint32_t end, TxTile* t);

extern uint32_t tx_strlen(const char* s);

/* ================================================================ */
/*  CRC32 / Adler32                                                  */
/* ================================================================ */

static uint32_t crc_table[256];
static int crc_ready = 0;

static void init_crc(void) {
    if (crc_ready) return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (uint32_t k = 0; k < 8; k++)
            c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
        crc_table[n] = c;
    }
    crc_ready = 1;
}

static uint32_t crc32_bytes(const uint8_t* data, uint32_t len) {
    init_crc();
    uint32_t c = 0xffffffffu;
    for (uint32_t i = 0; i < len; i++)
        c = crc_table[(c ^ data[i]) & 255u] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

static uint32_t tx_adler32(const uint8_t* data, uint32_t length) {
    extern unsigned long adler32(unsigned long, const uint8_t*, unsigned int);
    return (uint32_t)adler32(1u, data, length);
}

/* ================================================================ */
/*  Network string helper                                            */
/* ================================================================ */

static void buf_net_string(TxBuf* b, const char* s) {
    uint32_t len = tx_strlen(s);
    /* 7-bit encoded length */
    uint32_t v = len;
    while (v >= 0x80u) { buf_u8(b, (uint8_t)(v | 0x80u)); v >>= 7; }
    buf_u8(b, (uint8_t)v);
    buf_bytes(b, s, len);
}

/* ================================================================ */
/*  Fixed-Huffman zlib deflate                                       */
/* ================================================================ */

static uint32_t reverse_bits(uint32_t value, uint32_t bit_count) {
    uint32_t out = 0u;
    for (uint32_t i = 0; i < bit_count; i++)
        out = (out << 1u) | ((value >> i) & 1u);
    return out;
}

static void bw_bit(TxBuf* out, uint32_t* bitbuf, uint32_t* bitcnt, uint32_t value, uint32_t count) {
    *bitbuf |= value << *bitcnt;
    *bitcnt += count;
    while (*bitcnt >= 8u) {
        buf_u8(out, (uint8_t)(*bitbuf & 255u));
        *bitbuf >>= 8u;
        *bitcnt -= 8u;
    }
}

static void bw_finish(TxBuf* out, uint32_t* bitbuf, uint32_t* bitcnt) {
    if (*bitcnt) {
        buf_u8(out, (uint8_t)(*bitbuf & 255u));
        *bitbuf = 0u;
        *bitcnt = 0u;
    }
}

static void fixed_literal(TxBuf* out, uint32_t* bitbuf, uint32_t* bitcnt, uint32_t sym) {
    if (sym <= 143u)
        bw_bit(out, bitbuf, bitcnt, reverse_bits(0x30u + sym, 8u), 8u);
    else if (sym <= 255u)
        bw_bit(out, bitbuf, bitcnt, reverse_bits(0x190u + (sym - 144u), 9u), 9u);
    else if (sym <= 279u)
        bw_bit(out, bitbuf, bitcnt, reverse_bits(sym - 256u, 7u), 7u);
    else
        bw_bit(out, bitbuf, bitcnt, reverse_bits(0xC0u + (sym - 280u), 8u), 8u);
}

static const uint16_t LEN_BASE[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t LEN_EXTRA[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t DIST_BASE[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint8_t DIST_EXTRA[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

static int32_t z_head[32768];

static void fixed_match(TxBuf* out, uint32_t* bitbuf, uint32_t* bitcnt, uint32_t len, uint32_t dist) {
    uint32_t li = 0u;
    while (li < 28u && LEN_BASE[li + 1u] <= len) li++;
    fixed_literal(out, bitbuf, bitcnt, 257u + li);
    if (LEN_EXTRA[li]) bw_bit(out, bitbuf, bitcnt, len - LEN_BASE[li], LEN_EXTRA[li]);
    uint32_t di = 0u;
    while (di < 29u && DIST_BASE[di + 1u] <= dist) di++;
    bw_bit(out, bitbuf, bitcnt, reverse_bits(di, 5u), 5u);
    if (DIST_EXTRA[di]) bw_bit(out, bitbuf, bitcnt, dist - DIST_BASE[di], DIST_EXTRA[di]);
}

static uint32_t fast_hash3(const uint8_t* data, uint32_t pos) {
    return ((uint32_t)data[pos] * 251u ^ (uint32_t)data[pos + 1u] * 47u ^ (uint32_t)data[pos + 2u] * 13u) & 32767u;
}

static void write_zlib_fixed(TxBuf* out, const uint8_t* data, uint32_t len) {
    if (!len) {
        static const uint8_t empty[7] = {0x78,0x01,0x03,0x00,0x00,0x00,0x01};
        buf_bytes(out, empty, 7u);
        return;
    }
    for (uint32_t i = 0; i < 32768u; i++) z_head[i] = -1;
    buf_u8(out, 0x78); buf_u8(out, 0x01);
    uint32_t bitbuf = 0u, bitcnt = 0u;
    /* BFINAL=1, BTYPE=01 (fixed Huffman) */
    bw_bit(out, &bitbuf, &bitcnt, 1u, 1u);
    bw_bit(out, &bitbuf, &bitcnt, 1u, 2u);
    uint32_t pos = 0u;
    while (pos < len) {
        uint32_t best_len = 0u, best_dist = 0u;
        if (pos + 3u < len) {
            uint32_t h = fast_hash3(data, pos);
            int32_t m = z_head[h];
            z_head[h] = (int32_t)pos;
            if (m >= 0) {
                uint32_t mu = (uint32_t)m;
                uint32_t dist = pos - mu;
                if (dist > 0u && dist <= 32768u &&
                    data[mu] == data[pos] && data[mu + 1u] == data[pos + 1u] && data[mu + 2u] == data[pos + 2u]) {
                    uint32_t max = len - pos;
                    if (max > 258u) max = 258u;
                    uint32_t l = 3u;
                    while (l < max && data[mu + l] == data[pos + l]) l++;
                    if (l >= 4u || (l == 3u && dist <= 256u)) { best_len = l; best_dist = dist; }
                }
            }
        }
        if (best_len) {
            fixed_match(out, &bitbuf, &bitcnt, best_len, best_dist);
            uint32_t end = pos + best_len;
            if (end > len - 2u) end = len - 2u;
            for (uint32_t p = pos + 1u; p < end; p += 16u)
                z_head[fast_hash3(data, p)] = (int32_t)p;
            pos += best_len;
        } else {
            fixed_literal(out, &bitbuf, &bitcnt, data[pos]);
            pos++;
        }
    }
    fixed_literal(out, &bitbuf, &bitcnt, 256u);
    bw_finish(out, &bitbuf, &bitcnt);
    uint32_t ad = tx_adler32(data, len);
    buf_u8(out, (uint8_t)(ad >> 24));
    buf_u8(out, (uint8_t)(ad >> 16));
    buf_u8(out, (uint8_t)(ad >> 8));
    buf_u8(out, (uint8_t)ad);
}

/* ================================================================ */
/*  get_tile_variant_index -- exact C port of TerraX Tile::get_tile_variant_index */
/* ================================================================ */

static uint16_t get_tile_variant_index(uint16_t type, int16_t frame_x, int16_t frame_y) {
    uint16_t index = 0;
    switch (type) {
    case 4:
        if (frame_x < 66) index = 1;
        break;
    case 15:
        if (frame_y / 40 == 1 || frame_y / 40 == 20) index = 1;
        break;
    case 21: case 421:
        switch (frame_x / 36) {
        case 1: case 2: case 10: case 13: case 15: index = 1; break;
        case 3: case 4: index = 2; break;
        case 6: index = 3; break;
        case 11: case 17: index = 4; break;
        }
        break;
    case 26:
        if (frame_x >= 54) index = 1;
        break;
    case 27:
        if (frame_y < 34) index = 1;
        break;
    case 28: case 653:
        if (frame_y < 144) index = 0;
        else if (frame_y < 252) index = 1;
        else if (frame_y < 360 || (frame_y > 900 && frame_y < 1008)) index = 2;
        else if (frame_y < 468) index = 3;
        else if (frame_y < 576) index = 4;
        else if (frame_y < 684) index = 5;
        else if (frame_y < 792) index = 6;
        else if (frame_y < 898) index = 8;
        else if (frame_y < 1006) index = 7;
        else if (frame_y < 1114) index = 0;
        else if (frame_y < 1222) index = 3;
        else index = 7;
        break;
    case 31:
        if (frame_x >= 36) index = 1;
        break;
    case 82: case 83: case 84:
        switch (frame_x) {
        case 0: index = 0; break;
        case 18: index = 1; break;
        case 36: index = 2; break;
        case 54: index = 3; break;
        case 72: index = 4; break;
        case 90: index = 5; break;
        default: index = 6; break;
        }
        break;
    case 89: {
        uint16_t num = (uint16_t)(frame_x / 54);
        index = (num == 0 || num == 21 || num == 23) ? 0 : (num == 43 ? 2 : 1);
        break;
    }
    case 105:
        if (1548 <= frame_x && frame_x <= 1654) index = 1;
        else if (1656 <= frame_x && frame_x <= 1798) index = 2;
        break;
    case 129:
        index = (frame_x >= 324) ? 1 : 0;
        break;
    case 133:
        if (frame_x >= 52) index = 1;
        break;
    case 134:
        if (frame_x >= 28) index = 1;
        break;
    case 137: {
        uint16_t num = (uint16_t)(frame_y / 18);
        index = (num >= 1 && num <= 4) ? 1 : (num == 5 ? 2 : 0);
        break;
    }
    case 149:
        if (frame_x < 8) index = 2;
        else if (frame_x < 26) index = 0;
        else if (frame_x < 44) index = 1;
        else if (frame_x < 62) index = 2;
        else if (frame_x < 80) index = 0;
        else if (frame_x < 98) index = 1;
        break;
    case 165:
        if (frame_x < 54) index = 0;
        else if (frame_x < 106) index = 1;
        else if (frame_x >= 218) index = 1;
        else if (frame_x < 162) index = 2;
        else index = 3;
        break;
    case 178:
        index = (uint16_t)(frame_x / 18);
        if (index > 6) index = 6;
        break;
    case 184:
        index = (uint16_t)(frame_x / 22);
        if (index > 10) index = 10;
        break;
    case 185:
        if (frame_y < 18) {
            uint16_t num = (uint16_t)(frame_x / 18);
            if (num < 6 || (num >= 28 && num <= 32) || (num >= 54 && num < 72)) index = 0;
            else if ((num >= 6 && num < 12) || (num >= 33 && num <= 35) || num == 72) index = 1;
            else if (num < 28) index = 2;
            else if (num < 48) index = 3;
            else if (num < 54) index = 4;
        } else {
            uint16_t num = (uint16_t)(frame_x / 36);
            int num7 = frame_y / 18 - 1;
            num = (uint16_t)(num + num7 * 18);
            if (num < 6 || (num >= 19 && num <= 24) || (num >= 33 && num <= 40)) index = 0;
            else if ((num >= 6 && num < 16) || (num >= 59 && num < 62)) index = 2;
            else if ((num >= 16 && num < 19) || num == 31 || num == 32) index = 1;
            else if (num < 31) index = 3;
            else if (num < 38) index = 4;
        }
        break;
    case 186: case 647: {
        uint16_t temp = (uint16_t)(frame_x / 54);
        if (temp < 7) index = 2;
        else if (temp < 22 || temp == 33 || temp == 34 || temp == 35) index = 0;
        else if (temp < 25) index = 1;
        else if (temp == 31) index = 5;
        else if (temp < 32) index = 3;
        break;
    }
    case 187: case 648: {
        uint16_t temp = (uint16_t)(frame_x / 54 + frame_y / 36 * 36);
        if (temp < 3 || (temp >= 14 && temp <= 16)) index = 0;
        else if (temp < 6) index = 6;
        else if (temp < 9) index = 7;
        else if (temp < 14) index = 4;
        else if (temp < 18) index = 4;
        else if (temp < 23) index = 8;
        else if (temp < 25) index = 0;
        else if (temp < 29) index = 1;
        else if (temp < 47) index = 0;
        else if (temp < 50) index = 1;
        else if (temp < 52) index = 10;
        else if (temp < 55) index = 2;
        break;
    }
    case 227:
        index = (uint16_t)(frame_x / 34);
        break;
    case 240: {
        uint16_t num = (uint16_t)(frame_x / 54 + (frame_y / 54) * 36);
        if (num <= 11) index = 0;
        else if (num >= 47 && num <= 53) index = 0;
        else if (num >= 12 && num <= 17) index = 1;
        else if (num >= 18 && num <= 35) index = 1;
        else if (num >= 41 && num <= 45) index = 3;
        else if (num == 46) index = 4;
        else if (num >= 74 && num <= 92) index = 0;
        else index = 0;
        break;
    }
    case 242: {
        uint16_t num = (uint16_t)(frame_y / 72);
        index = (frame_x / 106 == 0 && num >= 22 && num <= 24) ? 1 : 0;
        break;
    }
    case 419: {
        uint16_t temp = (uint16_t)(frame_x / 18);
        index = (temp > 2) ? 2 : temp;
        break;
    }
    case 420: {
        uint16_t temp = (uint16_t)(frame_y / 18);
        index = (temp > 5) ? 5 : temp;
        break;
    }
    case 423: {
        uint16_t temp = (uint16_t)(frame_y / 18);
        index = (temp > 6) ? 6 : temp;
        break;
    }
    case 428: {
        uint16_t temp = (uint16_t)(frame_y / 18);
        index = (temp > 3) ? 3 : temp;
        break;
    }
    case 440: {
        uint16_t temp = (uint16_t)(frame_x / 54);
        index = (temp > 6) ? 6 : temp;
        break;
    }
    case 441:
        switch (frame_x / 36) {
        case 1: case 2: case 10: case 13: case 15: index = 1; break;
        case 3: case 4: index = 2; break;
        case 6: index = 3; break;
        case 11: case 17: index = 4; break;
        }
        break;
    case 453: {
        uint16_t temp = (uint16_t)(frame_x / 36);
        index = (temp > 2) ? 2 : temp;
        break;
    }
    case 457: {
        uint16_t temp = (uint16_t)(frame_x / 36);
        index = (temp > 4) ? 4 : temp;
        break;
    }
    case 467: case 468:
        if (frame_x / 36 >= 0 && frame_x / 36 <= 11) index = (uint16_t)(frame_x / 36);
        else if (frame_x / 36 == 12 || frame_x / 36 == 13) index = 10;
        break;
    case 493:
        if (frame_x < 18) index = 0;
        else if (frame_x < 36) index = 1;
        else if (frame_x < 54) index = 2;
        else if (frame_x < 72) index = 3;
        else if (frame_x < 30) index = 4;
        else index = 5;
        break;
    case 518: case 519:
        index = (uint16_t)(frame_y / 18);
        break;
    case 529:
        index = (uint16_t)(frame_y / 34);
        break;
    case 530: case 572:
        index = (uint16_t)(frame_y / 36);
        break;
    case 548:
        index = (frame_x / 54 < 7) ? 0 : 1;
        break;
    case 560: {
        uint16_t temp = (uint16_t)(frame_x / 36);
        if (temp <= 4) index = temp;
        break;
    }
    case 591:
        index = (uint16_t)(frame_x / 36);
        break;
    case 597: {
        uint16_t temp = (uint16_t)(frame_x / 54);
        if (temp <= 8) index = temp;
        break;
    }
    case 649: {
        uint16_t temp = (uint16_t)(frame_x / 10);
        if (temp < 6 || (temp >= 28 && temp <= 32)) index = 0;
        else if (temp < 12 || (temp >= 33 && temp <= 35)) index = 1;
        else if (temp < 28) index = 2;
        else if (temp < 48) index = 3;
        else if (temp < 54) index = 4;
        else if (temp < 72) index = 0;
        else if (temp != 72) index = 1;
        break;
    }
    case 650: {
        uint16_t temp = (uint16_t)(frame_x / 36 + (frame_y / 18 - 1) * 18);
        if (temp < 6 || (temp >= 19 && temp <= 24) || temp == 33 || (temp >= 38 && temp <= 40)) index = 0;
        else if (temp < 16) index = 2;
        else if (temp == 19 || temp == 31 || temp == 32) index = 1;
        else if (temp < 31) index = 3;
        else if (temp < 39) index = 4;
        else if (temp < 59) index = 0;
        else if (temp >= 62) index = 1;
        break;
    }
    default: break;
    }
    return index;
}


/* ================================================================ */
/*  map_type_for_tile / map_value_for_tile                           */
/* ================================================================ */

static uint32_t map_type_for_tile(const TxTile* t) {
    if (t->active && !t->invisible_block && t->type < TX_MAP_TILE_COUNT && TX_MAP_TILE_ID_LIST[t->type])
        return TX_MAP_TILE_ID_LIST[t->type] + get_tile_variant_index(t->type, t->frame_x, t->frame_y);
    if (t->liquid_type) return (uint32_t)TX_MAP_MAX_WALL_ID + t->liquid_type;
    if (t->wall && !t->invisible_wall && t->wall < TX_MAP_WALL_COUNT && TX_MAP_WALL_ID_LIST[t->wall])
        return TX_MAP_WALL_ID_LIST[t->wall];
    return 0u;
}

static uint32_t map_value_for_type(uint32_t type, uint8_t paint_id) {
    return (type & 65535u) | (255u << 16) | ((uint32_t)(paint_id & 31u) << 24);
}

static uint32_t map_value_for_tile(const TxTile* t) {
    uint32_t type = map_type_for_tile(t);
    uint32_t extra = t->active ? (t->tile_color & 31u) : (t->wall ? (t->wall_color & 31u) : 0u);
    return map_value_for_type(type, (uint8_t)extra);
}

typedef struct TxMapColorCacheEntry {
    uint32_t color_key;
    uint32_t map_value;
} TxMapColorCacheEntry;

static uint32_t map_color_distance_sq(uint8_t r0, uint8_t g0, uint8_t b0,
                                      uint8_t r1, uint8_t g1, uint8_t b1) {
    int32_t dr = (int32_t)r0 - (int32_t)r1;
    int32_t dg = (int32_t)g0 - (int32_t)g1;
    int32_t db = (int32_t)b0 - (int32_t)b1;
    return (uint32_t)(dr * dr + dg * dg + db * db);
}

static int map_read_color(const uint8_t* table, uint32_t count, uint32_t id, uint8_t out[3]) {
    const uint8_t* color;
    if (!table || id >= count) return 0;
    color = table + id * 4u;
    if (color[0] == 0u && color[1] == 0u && color[2] == 0u) return 0;
    out[0] = color[0];
    out[1] = color[1];
    out[2] = color[2];
    return 1;
}

static int map_read_builtin_color(const uint8_t* table, uint32_t id, uint32_t variant,
                                  uint8_t out[3]) {
    const uint8_t* color;
    if (!table || id >= TX_COLOR_MAX_IDS || variant >= TX_COLOR_MAX_VARIANTS) return 0;
    color = table + ((id * TX_COLOR_MAX_VARIANTS + variant) * 4u);
    if (!color[3]) return 0;
    out[0] = color[0];
    out[1] = color[1];
    out[2] = color[2];
    return 1;
}

static int map_value_for_txci_item(const TxciItem* item, uint32_t* out_value) {
    uint32_t base_type;
    uint32_t variant;

    if (!item || !out_value) return 0;
    variant = item->variant;
    if (item->is_wall) {
        if (item->type_id >= TX_MAP_WALL_COUNT ||
            !TX_MAP_WALL_ID_LIST[item->type_id] ||
            variant >= TX_MAP_WALL_TYPE_COUNTS[item->type_id]) return 0;
        base_type = TX_MAP_WALL_ID_LIST[item->type_id];
    } else {
        if (item->type_id >= TX_MAP_TILE_COUNT ||
            !TX_MAP_TILE_ID_LIST[item->type_id] ||
            variant >= TX_MAP_TILE_TYPE_COUNTS[item->type_id]) return 0;
        base_type = TX_MAP_TILE_ID_LIST[item->type_id];
    }

    *out_value = map_value_for_type(base_type + variant, item->paint_id);
    return 1;
}

static int map_value_for_txci_rgb(const TxciIndex* index, uint8_t r, uint8_t g, uint8_t b,
                                  uint32_t* out_value) {
    int group_id;
    TxciItem candidates[TXCI_MAX_GROUP_OPTIONS];
    int candidate_count;

    if (!index || !index->data || !out_value) return 0;
    group_id = txci_lookup_group(index, r, g, b);
    if (group_id < 0) return 0;
    candidate_count = txci_get_items(
        index, (uint32_t)group_id, candidates, (int)TXCI_MAX_GROUP_OPTIONS);

    /* TXCI stores its candidates in preference order. Keep tiles preferred for
     * marker pixels, then accept a wall if no valid map tile is available. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < candidate_count; i++) {
            if ((pass == 0 && candidates[i].is_wall) ||
                (pass == 1 && !candidates[i].is_wall)) continue;
            if (map_value_for_txci_item(&candidates[i], out_value)) return 1;
        }
    }
    return 0;
}

static uint32_t nearest_map_value_for_rgb(
        const TxciIndex* marker_color_index,
        uint8_t r, uint8_t g, uint8_t b, uint32_t fallback,
        TxMapColorCacheEntry* cache, uint32_t* cache_count, uint32_t cache_capacity) {
    uint32_t key = (uint32_t)r | ((uint32_t)g << 8u) | ((uint32_t)b << 16u);
    uint32_t best_value = fallback;
    uint32_t best_distance = UINT32_MAX;
    const uint8_t* tile_colors = tx_get_tile_colors();
    const uint8_t* wall_colors = tx_get_wall_colors();
    uint32_t tile_count = tx_get_tile_color_count();
    uint32_t wall_count = tx_get_wall_color_count();
    uint8_t color[3];
    TxTile tile;

    if (cache && cache_count) {
        for (uint32_t i = 0u; i < *cache_count; i++) {
            if (cache[i].color_key == key) return cache[i].map_value;
        }
    }

    if (map_value_for_txci_rgb(marker_color_index, r, g, b, &best_value)) {
        if (cache && cache_count && *cache_count < cache_capacity) {
            cache[*cache_count].color_key = key;
            cache[*cache_count].map_value = best_value;
            (*cache_count)++;
        }
        return best_value;
    }

    for (uint32_t id = 0u; id < tile_count; id++) {
        if (id >= TX_MAP_TILE_COUNT || !TX_MAP_TILE_ID_LIST[id]) continue;
        if (!map_read_color(tile_colors, tile_count, id, color)) continue;
        {
            uint32_t distance = map_color_distance_sq(r, g, b, color[0], color[1], color[2]);
            if (distance >= best_distance) continue;
            memset(&tile, 0, sizeof(tile));
            tile.active = 1u;
            tile.type = (uint16_t)id;
            best_distance = distance;
            best_value = map_value_for_tile(&tile);
        }
    }
    for (uint32_t id = 0u; id < wall_count; id++) {
        if (id >= TX_MAP_WALL_COUNT || !TX_MAP_WALL_ID_LIST[id]) continue;
        if (!map_read_color(wall_colors, wall_count, id, color)) continue;
        {
            uint32_t distance = map_color_distance_sq(r, g, b, color[0], color[1], color[2]);
            if (distance >= best_distance) continue;
            memset(&tile, 0, sizeof(tile));
            tile.wall = (uint16_t)id;
            best_distance = distance;
            best_value = map_value_for_tile(&tile);
        }
    }

    for (uint32_t id = 0u; id < TX_MAP_TILE_COUNT; id++) {
        uint32_t variant_count;
        if (!TX_MAP_TILE_ID_LIST[id]) continue;
        variant_count = TX_MAP_TILE_TYPE_COUNTS[id];
        if (variant_count > TX_COLOR_MAX_VARIANTS) variant_count = TX_COLOR_MAX_VARIANTS;
        for (uint32_t variant = 0u; variant < variant_count; variant++) {
            if (!map_read_builtin_color(TX_BUILTIN_TILE_COLORS, id, variant, color)) continue;
            {
                uint32_t distance = map_color_distance_sq(r, g, b, color[0], color[1], color[2]);
                if (distance >= best_distance) continue;
                best_distance = distance;
                best_value = map_value_for_type(TX_MAP_TILE_ID_LIST[id] + variant, 0u);
            }
        }
    }
    for (uint32_t id = 0u; id < TX_MAP_WALL_COUNT; id++) {
        uint32_t variant_count;
        if (!TX_MAP_WALL_ID_LIST[id]) continue;
        variant_count = TX_MAP_WALL_TYPE_COUNTS[id];
        if (variant_count > TX_COLOR_MAX_VARIANTS) variant_count = TX_COLOR_MAX_VARIANTS;
        for (uint32_t variant = 0u; variant < variant_count; variant++) {
            if (!map_read_builtin_color(TX_BUILTIN_WALL_COLORS, id, variant, color)) continue;
            {
                uint32_t distance = map_color_distance_sq(r, g, b, color[0], color[1], color[2]);
                if (distance >= best_distance) continue;
                best_distance = distance;
                best_value = map_value_for_type(TX_MAP_WALL_ID_LIST[id] + variant, 0u);
            }
        }
    }

    if (cache && cache_count && *cache_count < cache_capacity) {
        cache[*cache_count].color_key = key;
        cache[*cache_count].map_value = best_value;
        (*cache_count)++;
    }
    return best_value;
}

typedef struct MapChestPoint {
    int32_t x;
    int32_t y;
    uint32_t map_value;
    int32_t item_id;
    uint8_t radius;
    uint8_t line_width;
    uint8_t reserved[2];
    uint8_t rgba[4];
} MapChestPoint;

typedef struct MapBuildRequest {
    const int32_t* item_ids;
    uint32_t item_id_count;
    const int32_t* tile_types;
    uint32_t tile_type_count;
    const MapMarkerEntry* chest_markers;
    uint32_t chest_marker_count;
    const MapMarkerEntry* tile_markers;
    uint32_t tile_marker_count;
    uint32_t legacy_marker_value;
    uint32_t use_legacy_chest_markers;
    uint32_t use_legacy_tile_markers;
    const TxciIndex* marker_color_index;
    uint32_t paint_tile_marker_count;
} MapBuildRequest;

/* ================================================================ */
/*  chunk-column strip helpers                                       */
/* ================================================================ */

static void fill_chunk_strip_run(uint32_t* strip, uint32_t local_x, uint32_t height,
                                 uint32_t y, uint32_t len, uint32_t value) {
    uint32_t y_end = y + len;
    if (local_x >= 64u || y >= height) return;
    if (y_end > height) y_end = height;
    while (y < y_end) {
        uint32_t chunk_y = y >> 6;
        uint32_t local_y = y & 63u;
        uint32_t next_y = ((chunk_y + 1u) << 6);
        if (next_y > y_end) next_y = y_end;
        uint32_t idx = chunk_y * 4096u + local_y * 64u + local_x;
        for (uint32_t yy = y; yy < next_y; yy++) {
            strip[idx] = value;
            idx += 64u;
        }
        y = next_y;
    }
}

static void prefill_chunk_strip_background(uint32_t* strip, uint32_t cpc, uint32_t chunk_x,
                                           uint32_t width, uint32_t height,
                                           int32_t groundLevel, int32_t rockLevel) {
    const uint32_t world_x_base = chunk_x * 64u;
    const uint32_t padding_val = (255u << 16);
    if (groundLevel <= 0) groundLevel = (int32_t)(height > 3u ? (height * 35u) / 100u : 1u);
    if (rockLevel <= groundLevel) {
        rockLevel = (int32_t)(height > 2u ? (height * 65u) / 100u : (uint32_t)groundLevel + 1u);
    }

    for (uint32_t chunk_y = 0; chunk_y < cpc; chunk_y++) {
        uint32_t world_y_base = chunk_y * 64u;
        for (uint32_t ly = 0; ly < 64u; ly++) {
            uint32_t world_y = world_y_base + ly;
            uint32_t base = chunk_y * 4096u + ly * 64u;
            if (world_y >= height) {
                for (uint32_t lx = 0; lx < 64u; lx++) strip[base + lx] = padding_val;
                continue;
            }
            uint32_t bg_type;
            if ((int32_t)world_y < groundLevel) {
                bg_type = TX_MAP_MAX_LIQUID_ID +
                    (uint32_t)(((uint64_t)world_y * (uint64_t)TX_MAP_SKY_GRADIENTS) /
                               (uint64_t)(uint32_t)groundLevel);
            } else if ((int32_t)world_y < rockLevel) {
                bg_type = TX_MAP_DIRT_ID;
            } else if (world_y + 200u < height) {
                bg_type = TX_MAP_ROCK_ID;
            } else {
                bg_type = TX_MAP_HELL_ID;
            }
            {
                uint32_t bg_val = (bg_type & 65535u) | (255u << 16);
                for (uint32_t lx = 0; lx < 64u; lx++) {
                    strip[base + lx] = (world_x_base + lx < width) ? bg_val : padding_val;
                }
            }
        }
    }
}

static void set_chunk_strip_point(uint32_t* strip, uint32_t local_x, uint32_t height,
                                  int32_t y, uint32_t value) {
    if (local_x >= 64u || y < 0 || (uint32_t)y >= height) return;
    {
        uint32_t uy = (uint32_t)y;
        uint32_t idx = ((uy >> 6) * 4096u) + ((uy & 63u) * 64u) + local_x;
        strip[idx] = value;
    }
}

static void draw_map_marker_on_strip(
        TxWorld* world, uint32_t* strip, uint32_t world_x_base,
        uint32_t width, uint32_t height, const MapChestPoint* point,
        TxMapColorCacheEntry* color_cache, uint32_t* color_cache_count,
        uint32_t* icon_values) {
    uint32_t marker_value;
    int32_t radius;
    int32_t thickness;
    int32_t outer_squared;
    int32_t inner_radius;
    int32_t inner_squared;
    int icon_index;

    if (!world || !strip || !point || !width || !height) return;
    if (point->x + (int32_t)point->radius < (int32_t)world_x_base ||
        point->x - (int32_t)point->radius >= (int32_t)(world_x_base + 64u)) return;
    marker_value = point->map_value;
    if (point->reserved[0]) {
        marker_value = nearest_map_value_for_rgb(
            &world->marker_color_index,
            point->rgba[0], point->rgba[1], point->rgba[2], point->map_value,
            color_cache, color_cache_count, 512u);
    }
    if (point->radius == 0u) {
        set_chunk_strip_point(strip, (uint32_t)(point->x - (int32_t)world_x_base),
                              height, point->y, marker_value);
        return;
    }

    radius = (int32_t)point->radius;
    thickness = (int32_t)point->line_width;
    if (thickness > radius) thickness = radius;
    inner_radius = thickness > 0 ? radius - thickness : -1;
    outer_squared = radius * radius;
    inner_squared = inner_radius > 0 ? inner_radius * inner_radius : -1;

    for (int32_t dy = -radius; dy <= radius; dy++) {
        for (int32_t dx = -radius; dx <= radius; dx++) {
            int32_t distance = dx * dx + dy * dy;
            int32_t world_x = point->x + dx;
            if (distance > outer_squared || (inner_squared >= 0 && distance < inner_squared)) continue;
            if (world_x < (int32_t)world_x_base ||
                world_x >= (int32_t)(world_x_base + 64u)) continue;
            set_chunk_strip_point(strip, (uint32_t)(world_x - (int32_t)world_x_base),
                                  height, point->y + dy, marker_value);
        }
    }

    if (point->item_id < 0) return;
    icon_index = terra_icon_index_for_item(&world->icon_atlas, point->item_id);
    if (icon_index < 0) return;

    {
        uint32_t side = terra_icon_side_for_radius(
            (uint32_t)(radius > thickness ? radius - thickness : radius));
        uint32_t icon_size = world->icon_atlas.icon_size;
        uint32_t source_x0 = world->icon_atlas.x_offsets[icon_index];
        uint32_t source_y0 = world->icon_atlas.y_offsets[icon_index];
        const uint8_t* atlas_rgba = world->icon_atlas.rgba;
        int32_t start_x;
        int32_t start_y;

        if (!side || !icon_size || !atlas_rgba) return;
        start_x = point->x - (int32_t)(side / 2u);
        start_y = point->y - (int32_t)(side / 2u);
        for (uint32_t local_y = 0u; local_y < side; local_y++) {
            int32_t world_y = start_y + (int32_t)local_y;
            if (world_y < 0 || world_y >= (int32_t)height) continue;
            {
                uint32_t source_y = (local_y * icon_size) / side;
                for (uint32_t local_x = 0u; local_x < side; local_x++) {
                    int32_t world_x = start_x + (int32_t)local_x;
                    if (world_x < (int32_t)world_x_base ||
                        world_x >= (int32_t)(world_x_base + 64u) ||
                        world_x < 0 || world_x >= (int32_t)width) continue;
                    {
                        uint32_t source_x = (local_x * icon_size) / side;
                        uint32_t source_index = (source_y0 + source_y) * world->icon_atlas.atlas_width + source_x0 + source_x;
                        const uint8_t* source = atlas_rgba + source_index * 4u;
                        if (!source[3]) continue;
                        uint32_t value = icon_values ? icon_values[source_index] : 0u;
                        if (!value) {
                            value = nearest_map_value_for_rgb(
                                &world->marker_color_index,
                                source[0], source[1], source[2], marker_value,
                                color_cache, color_cache_count, 512u);
                            if (icon_values) icon_values[source_index] = value;
                        }
                        set_chunk_strip_point(
                            strip, (uint32_t)(world_x - (int32_t)world_x_base), height,
                            world_y, value);
                    }
                }
            }
        }
    }
}

/* ================================================================ */
/*  Chest marking                                                    */
/* ================================================================ */

static int int32_list_contains(const int32_t* values, uint32_t count, int32_t target) {
    for (uint32_t i = 0; i < count; i++) if (values[i] == target) return 1;
    return 0;
}

static void rd_skip_string_value(const uint8_t* p, uint32_t len, uint32_t* off) {
    int ok = 0;
    uint32_t slen = rd_7bit(p, len, off, &ok);
    if (!ok || *off + slen > len) { *off = len; return; }
    *off += slen;
}

static const MapMarkerEntry* find_chest_marker(const MapMarkerEntry* markers, uint32_t count,
                                               int32_t item_type) {
    for (uint32_t i = 0; i < count; i++) {
        if (markers[i].id == item_type) return &markers[i];
    }
    return NULL;
}

static int collect_matching_chest_points(
        TxWorld* w, const MapBuildRequest* request, uint32_t width, uint32_t height,
        MapChestPoint** out_points, uint32_t* out_count) {
    MapChestPoint* points = NULL;
    uint32_t matched = 0u;
    *out_points = NULL;
    *out_count = 0u;

    if ((!request->use_legacy_chest_markers && request->chest_marker_count == 0u) ||
        w->pointer_count <= 2u || w->starts[2] >= w->ends[2]) {
        return 1;
    }

    {
        const uint8_t* p;
        uint32_t len;
        uint32_t off;
        uint32_t end;
        if (w->section_overrides[2].active) {
            p = w->section_overrides[2].data;
            len = w->section_overrides[2].len;
            off = 0u;
            end = len;
        } else {
            p = w->file;
            len = w->file_len;
            off = w->starts[2];
            end = w->ends[2];
        }
        int32_t chest_count = (int16_t)rd_u16le(p, len, &off);
        int32_t slots_per_chest = 0;
        if (w->version < 294u) slots_per_chest = (int16_t)rd_u16le(p, len, &off);
        if (chest_count <= 0) return 1;

        {
            uint64_t bytes = (uint64_t)(uint32_t)chest_count * (uint64_t)sizeof(MapChestPoint);
            if (bytes > UINT32_MAX) {
                tx_set_error("TERRAX_BAD_DIMENSIONS", "matched chest list exceeds WASM limits");
                return 0;
            }
            points = (MapChestPoint*)tx_alloc((uint32_t)bytes);
            if (!points) {
                tx_set_error("TERRAX_WASM_OOM", "matched chest list allocation failed");
                return 0;
            }
        }

        for (int32_t c = 0; c < chest_count && off < end; c++) {
            int32_t x = rd_i32le(p, len, &off);
            int32_t y = rd_i32le(p, len, &off);
            uint32_t matched_value = 0u;
            int32_t matched_item_id = -1;
            const MapMarkerEntry* matched_marker = NULL;
            int legacy_match = request->use_legacy_chest_markers && request->item_id_count == 0u;
            rd_skip_string_value(p, len, &off);
            {
                int32_t max_items = w->version >= 294u ? rd_i32le(p, len, &off) : slots_per_chest;
                if (max_items < 0) max_items = 0;
                for (int32_t j = 0; j < max_items && off < end; j++) {
                    int16_t stack = (int16_t)rd_u16le(p, len, &off);
                    if (stack != 0) {
                        int32_t item_type = rd_i32le(p, len, &off);
                        rd_u8(p, len, &off);
                        if (matched_marker == NULL && request->chest_marker_count > 0u) {
                            matched_marker = find_chest_marker(
                                request->chest_markers, request->chest_marker_count, item_type);
                            if (matched_marker) {
                                matched_value = matched_marker->map_value;
                                matched_item_id = item_type;
                            }
                        }
                        if (!legacy_match && request->use_legacy_chest_markers &&
                            int32_list_contains(request->item_ids, request->item_id_count, item_type)) {
                            legacy_match = 1;
                        }
                    }
                }
            }

            if ((matched_marker != NULL || legacy_match) &&
                x >= 0 && y >= 0 && (uint32_t)x < width && (uint32_t)y < height) {
                points[matched].x = x;
                points[matched].y = y;
                points[matched].map_value =
                    matched_value != 0u ? matched_value : request->legacy_marker_value;
                points[matched].item_id = matched_item_id;
                points[matched].radius = matched_marker ? matched_marker->radius : 0u;
                points[matched].line_width = matched_marker ? matched_marker->line_width : 0u;
                points[matched].reserved[0] = matched_marker ? 1u : 0u;
                points[matched].reserved[1] = 0u;
                if (matched_marker) {
                    points[matched].rgba[0] = matched_marker->rgba[0];
                    points[matched].rgba[1] = matched_marker->rgba[1];
                    points[matched].rgba[2] = matched_marker->rgba[2];
                    points[matched].rgba[3] = matched_marker->rgba[3];
                } else {
                    points[matched].rgba[0] = 0u;
                    points[matched].rgba[1] = 0u;
                    points[matched].rgba[2] = 0u;
                    points[matched].rgba[3] = 0u;
                }
                matched++;
            }
        }
    }

    if (matched == 0u) {
        tx_internal_free(points);
        return 1;
    }
    *out_points = points;
    *out_count = matched;
    return 1;
}

static const MapMarkerEntry* find_tile_marker(const MapMarkerEntry* markers, uint32_t count,
                                              uint16_t tile_type) {
    for (uint32_t i = 0; i < count; i++) {
        if (!markers[i].locate && markers[i].id == (int32_t)tile_type)
            return &markers[i];
    }
    return NULL;
}

/* ================================================================ */
/*  Map header writing                                               */
/* ================================================================ */

static void write_map_header(TxBuf* out, TxWorld* w) {
    buf_u32le(out, TX_MAP_VERSION);
    buf_bytes(out, w->magic[0] ? w->magic : "relogic", 7);
    buf_u8(out, 1u);
    buf_u32le(out, 0u /* revision: match TerraX C++ highlight_from_world */);
    buf_u64le(out, w->favorite);
    buf_net_string(out, w->worldName[0] ? w->worldName : "World");
    buf_u32le(out, (uint32_t)w->worldId);
    buf_u32le(out, (uint32_t)w->maxTilesY);
    buf_u32le(out, (uint32_t)w->maxTilesX);
    buf_u16le(out, TX_MAP_TILE_COUNT);
    buf_u16le(out, TX_MAP_WALL_COUNT);
    buf_u16le(out, TX_MAP_LIQUID_COUNT);
    buf_u16le(out, TX_MAP_SKY_GRADIENTS);
    buf_u16le(out, TX_MAP_DIRT_GRADIENTS);
    buf_u16le(out, TX_MAP_ROCK_GRADIENTS);
    buf_bytes(out, TX_MAP_TILE_OPTIONS, sizeof(TX_MAP_TILE_OPTIONS));
    buf_bytes(out, TX_MAP_WALL_OPTIONS, sizeof(TX_MAP_WALL_OPTIONS));
    for (uint32_t i = 0; i < TX_MAP_TILE_COUNT; i++)
        if (TX_MAP_TILE_EXISTS[i]) buf_u8(out, TX_MAP_TILE_TYPE_COUNTS[i]);
    for (uint32_t i = 0; i < TX_MAP_WALL_COUNT; i++)
        if (TX_MAP_WALL_EXISTS[i]) buf_u8(out, TX_MAP_WALL_TYPE_COUNTS[i]);
}

/* The TXCI buffer is copied into the active world's native allocation domain,
 * so operation reclamation cannot invalidate the palette while MAP is being
 * generated. It is released explicitly before the world allocation mark is
 * rewound during close. */
void txw_clear_marker_color_index(TxWorld* world) {
    if (!world) return;
    if (world->marker_color_index.data) {
        txci_unload(&world->marker_color_index);
    }
}

int32_t txw_set_marker_color_index_from_buffer(
    TxWorld* world,
    const uint8_t* data,
    uint32_t data_len) {
    if (!world) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "world is null");
        return -1;
    }

    /* Parse into a separate allocation domain root. A failed replacement must
     * not destroy the last known-good index held by the world. */
    TxciIndex replacement;
    memset(&replacement, 0, sizeof(replacement));
    if (!txci_load_from_memory(&replacement, data, data_len)) {
        return -1;
    }

    TxciIndex previous = world->marker_color_index;
    world->marker_color_index = replacement;
    txci_unload(&previous);
    world->heap_mark = tx_mark();
    world->last_op_heap_end = world->heap_mark;
    return 0;
}

int32_t txw_set_marker_color_index(uint32_t handle, uint32_t data_ptr, uint32_t data_len) {
    TxWorld* world = tx_get_world(handle);

    if (!world) {
        tx_set_error("TERRAX_INVALID_HANDLE", "world handle is stale or invalid");
        return -1;
    }

    if (!data_ptr && !data_len) {
        txw_clear_marker_color_index(world);
        world->heap_mark = tx_mark();
        world->last_op_heap_end = world->heap_mark;
        tx_clear_error();
        return 0;
    }
    if (!data_ptr || data_len < TXCI_HEADER_SIZE) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "TXCI data too small or null");
        return -1;
    }
    if (!tx_bridge_range_is_valid(data_ptr, data_len)) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "marker color-index payload exceeds its bridge allocation");
        return -1;
    }
    if (txw_set_marker_color_index_from_buffer(
            world, (const uint8_t*)(uintptr_t)data_ptr, data_len) < 0) {
        return -1;
    }

    tx_clear_error();
    return 0;
}

/* ================================================================ */
/*  generate_map -- full map generation pipeline                     */
/* ================================================================ */

static int map_layout(TxWorld* w, uint32_t* width, uint32_t* height,
                      uint32_t* cpr, uint32_t* cpc, uint32_t* chunks,
                      uint32_t* strip_bytes) {
    if (!w || w->maxTilesX <= 0 || w->maxTilesY <= 0) {
        tx_set_error("TERRAX_BAD_DIMENSIONS", "world dimensions must be positive");
        return 0;
    }
    uint64_t width64 = (uint32_t)w->maxTilesX;
    uint64_t height64 = (uint32_t)w->maxTilesY;
    uint64_t cpr64 = (width64 + 63u) >> 6;
    uint64_t cpc64 = (height64 + 63u) >> 6;
    uint64_t chunks64 = cpr64 * cpc64;
    uint64_t strip_bytes64 = cpc64 * 4096u * sizeof(uint32_t);
    if (width64 * sizeof(uint32_t) > UINT32_MAX || cpr64 > UINT32_MAX ||
        cpc64 > UINT32_MAX || chunks64 > UINT32_MAX || strip_bytes64 > UINT32_MAX) {
        tx_set_error("TERRAX_BAD_DIMENSIONS", "map dimensions exceed WASM limits");
        return 0;
    }
    *width = (uint32_t)width64;
    *height = (uint32_t)height64;
    *cpr = (uint32_t)cpr64;
    *cpc = (uint32_t)cpc64;
    *chunks = (uint32_t)chunks64;
    *strip_bytes = (uint32_t)strip_bytes64;
    return 1;
}

static int compress_chunk_exact(const uint32_t* raw_chunk, TxBuf* compressed) {
    TxBuf z;
    buf_init(&z, 17408u);
    if (!z.ok) {
        tx_set_error("TERRAX_WASM_OOM", "map chunk compression buffer allocation failed");
        return 0;
    }
    write_zlib_fixed(&z, (const uint8_t*)raw_chunk, 4096u * 4u);
    if (!z.ok) {
        if (z.data) tx_internal_free(z.data);
        tx_set_error("TERRAX_WASM_OOM", "map chunk compression failed");
        return 0;
    }

    *compressed = z;
    return 1;
}

typedef int (*MapChunkSink)(uint32_t chunk_index, const uint8_t* data, uint32_t size, void* context);

struct TxPreparedMap {
    uint32_t width, height, cpr, cpc, chunks, strip_bytes;
    uint32_t *strip, *offsets, *sizes;
    uint8_t* bytes;
    uint32_t length, capacity;
};

void tx_map_base_free(TxPreparedMap* p) {
    if (!p) return;
    tx_persistent_free(p->strip);
    tx_persistent_free(p->offsets);
    tx_persistent_free(p->sizes);
    tx_persistent_free(p->bytes);
    tx_persistent_free(p);
}

TxPreparedMap* tx_map_base_begin(TxWorld* w) {
    TxPreparedMap* p = (TxPreparedMap*)tx_persistent_alloc(sizeof(*p));
    if (!p) goto oom;
    memset(p, 0, sizeof(*p));
    if (!map_layout(w, &p->width, &p->height, &p->cpr, &p->cpc, &p->chunks, &p->strip_bytes)) {
        tx_map_base_free(p); return NULL;
    }
    p->strip = (uint32_t*)tx_persistent_alloc(p->strip_bytes);
    p->offsets = (uint32_t*)tx_persistent_alloc(p->chunks * 4u);
    p->sizes = (uint32_t*)tx_persistent_alloc(p->chunks * 4u);
    if (p->strip && p->offsets && p->sizes) return p;
oom:
    tx_map_base_free(p);
    tx_set_error("TERRAX_WASM_OOM", "prepared map allocation failed");
    return NULL;
}

int tx_map_base_run(TxWorld* w, TxPreparedMap* p, uint32_t x, uint32_t y,
                    const TxTile* tile, uint32_t run) {
    uint32_t cx = x / 64u;
    if (!(x % 64u) && !y)
        prefill_chunk_strip_background(p->strip, p->cpc, cx, p->width, p->height,
                                       w->worldSurface, w->rockLayer);
    uint32_t value = map_value_for_tile(tile);
    if (value & 65535u) fill_chunk_strip_run(p->strip, x % 64u, p->height, y, run, value);
    if (y + run != p->height || (x % 64u != 63u && x + 1u != p->width)) return 1;
    for (uint32_t cy = 0; cy < p->cpc; cy++) {
        TxBuf compressed = {0};
        if (!compress_chunk_exact(p->strip + cy * 4096u, &compressed)) return 0;
        uint32_t required = p->length + compressed.len;
        if (required > 32u * 1024u * 1024u) {
            tx_internal_free(compressed.data);
            /* ponytail: exceptionally noisy MAP bases retain the existing
             * streaming reader rather than reject a previously supported WLD. */
            w->prepared_output->map = NULL;
            tx_map_base_free(p);
            return 1;
        }
        if (required > p->capacity) {
            uint32_t cap = p->capacity ? p->capacity * 2u : 65536u;
            if (cap < required) cap = required;
            if (cap > 32u * 1024u * 1024u) cap = 32u * 1024u * 1024u;
            void* next = tx_persistent_realloc(p->bytes, cap);
            if (!next) {
                tx_internal_free(compressed.data);
                tx_set_error("TERRAX_WASM_OOM", "prepared MAP growth failed"); return 0;
            }
            p->bytes = next; p->capacity = cap;
        }
        uint32_t i = cx * p->cpc + cy;
        p->offsets[i] = p->length; p->sizes[i] = compressed.len;
        memcpy(p->bytes + p->length, compressed.data, compressed.len);
        p->length = required;
        tx_internal_free(compressed.data);
    }
    if (x + 1u == p->width) { tx_persistent_free(p->strip); p->strip = NULL; }
    return 1;
}

int tx_map_base_strip(TxPreparedMap* p, uint32_t cx, uint32_t* strip) {
    extern int uncompress(unsigned char*, unsigned long*, const unsigned char*, unsigned long);
    for (uint32_t cy = 0; cy < p->cpc; cy++) {
        uint32_t i = cx * p->cpc + cy;
        unsigned long size = 4096u * 4u;
        if (uncompress((unsigned char*)(strip + cy * 4096u), &size,
                       p->bytes + p->offsets[i], p->sizes[i]) != 0 || size != 4096u * 4u) {
            tx_set_error("TERRAX_STATE_ERROR", "prepared MAP chunk is invalid"); return 0;
        }
    }
    return 1;
}

typedef struct MapChunkMeasureContext {
    uint32_t* sizes;
    uint32_t count;
} MapChunkMeasureContext;

typedef struct MapChunkWriteContext {
    uint8_t* output;
    uint32_t output_size;
    const uint32_t* offsets;
    const uint32_t* sizes;
    uint32_t count;
} MapChunkWriteContext;

static int map_value_for_requested_tile(const MapBuildRequest* request, const TxTile* t,
                                        uint32_t run, uint32_t* matched_tiles,
                                        TxMapColorCacheEntry* color_cache,
                                        uint32_t* color_cache_count,
                                        uint32_t* out_value);

static int measure_map_chunk(uint32_t chunk_index, const uint8_t* data, uint32_t size, void* context) {
    MapChunkMeasureContext* measure = (MapChunkMeasureContext*)context;
    (void)data;
    if (!measure || chunk_index >= measure->count) {
        tx_set_error("TERRAX_BAD_DIMENSIONS", "map chunk index is out of range");
        return 0;
    }
    measure->sizes[chunk_index] = size;
    return 1;
}

static void write_map_u32le(uint8_t* destination, uint32_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
    destination[3] = (uint8_t)(value >> 24);
}

static int write_map_chunk(uint32_t chunk_index, const uint8_t* data, uint32_t size, void* context) {
    MapChunkWriteContext* write = (MapChunkWriteContext*)context;
    uint64_t end;
    uint32_t offset;
    if (!write || chunk_index >= write->count || !data || size != write->sizes[chunk_index]) {
        tx_set_error("TERRAX_STATE_ERROR", "map chunk size changed between passes");
        return 0;
    }
    offset = write->offsets[chunk_index];
    end = (uint64_t)offset + 4u + size;
    if (end > write->output_size) {
        tx_set_error("TERRAX_RESULT_TOO_LARGE", "map chunk exceeds the output budget");
        return 0;
    }
    write_map_u32le(write->output + offset, size);
    memcpy(write->output + offset + 4u, data, size);
    return 1;
}

static int walk_map_chunks(TxWorld* w, const MapBuildRequest* request,
                           MapChestPoint* chest_points, uint32_t chest_point_count,
                           uint32_t width, uint32_t height, uint32_t cpr, uint32_t cpc,
                           uint32_t strip_bytes,
                           uint32_t* matched_tile_count,
                           MapChunkSink sink, void* sink_context) {
    uint32_t off;
    uint32_t end;
    uint32_t tile_matches = 0u;
    TxMapColorCacheEntry color_cache[512];
    uint32_t color_cache_count = 0u;
    uint32_t* strip = (uint32_t*)tx_alloc(strip_bytes);
    uint32_t* icon_values = NULL;
    const uint8_t* tile_src = w->file;
    uint32_t tile_src_len = w->file_len;
    uint8_t* saved_file = w->file;
    uint32_t saved_len = w->file_len;
    int ok = 1;

    TxPreparedMap* prepared = w->prepared_output && w->prepared_output->ready &&
        !request->paint_tile_marker_count ? w->prepared_output->map : NULL;
    /* Keep one original strip for an exact comparison after markers. Most sky
     * and distant chunks can reuse their already encoded bytes. Allocation is
     * optional: the existing encoder remains valid without this scratch copy. */
    uint32_t* base_strip = prepared && chest_point_count ? (uint32_t*)tx_alloc(strip_bytes) : NULL;

    if (!strip) {
        if (base_strip) tx_internal_free(base_strip);
        tx_set_error("TERRAX_WASM_OOM", "map chunk strip allocation failed");
        return 0;
    }
    if (!sink) {
        if (base_strip) tx_internal_free(base_strip);
        tx_internal_free(strip);
        tx_set_error("TERRAX_INVALID_ARGUMENT", "map chunk sink is null");
        return 0;
    }
    /* Resolve each used atlas pixel once, regardless of how many chests/veins
     * share that icon. Oversized custom atlases retain the bounded slow path. */
    uint64_t icon_bytes = (uint64_t)w->icon_atlas.atlas_width * w->icon_atlas.atlas_height * 4u;
    if (w->icon_atlas.rgba && icon_bytes && icon_bytes <= 4u * 1024u * 1024u) {
        icon_values = (uint32_t*)tx_alloc((uint32_t)icon_bytes);
        if (icon_values) memset(icon_values, 0, (uint32_t)icon_bytes);
    }

    if (w->section_overrides[1].active) {
        tile_src = w->section_overrides[1].data;
        tile_src_len = w->section_overrides[1].len;
        off = 0u;
        end = tile_src_len;
    } else {
        off = w->starts[1];
        end = w->ends[1];
    }
    w->file = (uint8_t*)tile_src;
    w->file_len = tile_src_len;

    for (uint32_t chunk_x = 0; chunk_x < cpr && ok; chunk_x++) {
        uint32_t world_x_base = chunk_x * 64u;
        uint32_t column_count = width > world_x_base ? width - world_x_base : 0u;
        if (column_count > 64u) column_count = 64u;
        if (prepared) {
            if (chest_point_count) ok = tx_map_base_strip(prepared, chunk_x, strip);
            if (base_strip && ok) memcpy(base_strip, strip, strip_bytes);
        }
        else prefill_chunk_strip_background(strip, cpc, chunk_x, width, height,
                                            w->worldSurface, w->rockLayer);

        for (uint32_t local_x = 0; !prepared && local_x < column_count && ok; local_x++) {
            for (uint32_t y = 0; y < height; ) {
                TxTile t;
                uint32_t run;
                uint32_t value;
                if (!read_tile_at(w, &off, end, &t)) {
                    tx_set_error("TERRAX_BAD_TILE_STREAM",
                                 "tile stream ended early during map generation");
                    ok = 0;
                    break;
                }
                run = (uint32_t)t.same + 1u;
                if (!map_value_for_requested_tile(
                        request, &t, run, matched_tile_count ? &tile_matches : NULL,
                        color_cache, &color_cache_count, &value)) {
                    ok = 0;
                    break;
                }
                if ((value & 65535u) != 0u) {
                    fill_chunk_strip_run(strip, local_x, height, y, run, value);
                }
                y += run;
            }
        }

        for (uint32_t i = 0; i < chest_point_count && ok; i++) {
            if (chest_points[i].x >= 0 && chest_points[i].y >= 0 &&
                (uint32_t)chest_points[i].x < width &&
                (uint32_t)chest_points[i].y < height) {
                draw_map_marker_on_strip(
                    w, strip, world_x_base, width, height, &chest_points[i],
                    color_cache, &color_cache_count, icon_values);
            }
        }

        for (uint32_t chunk_y = 0; chunk_y < cpc && ok; chunk_y++) {
            TxBuf compressed = { 0 };
            uint32_t chunk_index = chunk_y * cpr + chunk_x;
            if (prepared && (!chest_point_count || (base_strip &&
                memcmp(base_strip + chunk_y * 4096u, strip + chunk_y * 4096u, 4096u * 4u) == 0))) {
                uint32_t i = chunk_x * cpc + chunk_y;
                ok = sink(chunk_index, prepared->bytes + prepared->offsets[i], prepared->sizes[i], sink_context);
                continue;
            }
            if (!compress_chunk_exact(strip + chunk_y * 4096u, &compressed)) {
                ok = 0;
                break;
            }
            ok = sink(chunk_index, compressed.data, compressed.len, sink_context);
            tx_internal_free(compressed.data);
        }
    }

    w->file = saved_file;
    w->file_len = saved_len;
    if (matched_tile_count) *matched_tile_count = tile_matches;
    tx_internal_free(strip);
    if (base_strip) tx_internal_free(base_strip);
    if (icon_values) tx_internal_free(icon_values);
    return ok;
}

static int map_value_for_requested_tile(const MapBuildRequest* request, const TxTile* t,
                                        uint32_t run, uint32_t* matched_tiles,
                                        TxMapColorCacheEntry* color_cache,
                                        uint32_t* color_cache_count,
                                        uint32_t* out_value) {
    if (request->paint_tile_marker_count > 0u && t->active) {
        const MapMarkerEntry* marker = find_tile_marker(
            request->tile_markers, request->tile_marker_count, t->type);
        if (marker) {
            if (matched_tiles) {
                if (UINT32_MAX - *matched_tiles < run) {
                    tx_set_error("TERRAX_BAD_DIMENSIONS", "tile match count overflow");
                    return 0;
                }
                *matched_tiles += run;
            }
            *out_value = nearest_map_value_for_rgb(
                request->marker_color_index,
                marker->rgba[0], marker->rgba[1], marker->rgba[2], marker->map_value,
                color_cache, color_cache_count, 512u);
            return 1;
        }
    }
    if (request->use_legacy_tile_markers && t->active &&
        int32_list_contains(request->tile_types, request->tile_type_count, (int32_t)t->type)) {
        *out_value = request->legacy_marker_value;
        return 1;
    }
    *out_value = map_value_for_tile(t);
    return 1;
}

#define TX_MAP_SINGLE_PASS_STAGING_LIMIT_BYTES (32u * 1024u * 1024u)

typedef struct MapChunkStagingContext {
    TxBuf* compressed;
    uint32_t* offsets;
    uint32_t* sizes;
    uint32_t count;
    uint32_t limit;
    int fallback;
} MapChunkStagingContext;

static int stage_map_chunk(uint32_t chunk_index, const uint8_t* data, uint32_t size, void* context) {
    MapChunkStagingContext* stage = (MapChunkStagingContext*)context;
    if (!stage || !stage->compressed || chunk_index >= stage->count || !data || size == 0u) {
        tx_set_error("TERRAX_STATE_ERROR", "single-pass map chunk context is invalid");
        return 0;
    }
    if (size > stage->limit || stage->compressed->len > stage->limit - size) {
        stage->fallback = 1;
        return 0;
    }
    if (!buf_reserve(stage->compressed, size)) {
        stage->fallback = 1;
        return 0;
    }
    stage->offsets[chunk_index] = stage->compressed->len;
    stage->sizes[chunk_index] = size;
    memcpy(stage->compressed->data + stage->compressed->len, data, size);
    stage->compressed->len += size;
    return 1;
}

static int32_t generate_map_streaming(TxWorld* w, const MapBuildRequest* request,
                                      uint32_t* matched_chest_count,
                                      uint32_t* matched_tile_count) {
    uint32_t width, height, cpr, cpc, chunk_count, strip_bytes;
    uint32_t* chunk_sizes = NULL;
    uint32_t* chunk_offsets = NULL;
    uint32_t tile_matches = 0u;
    MapChestPoint* chest_points = NULL;
    uint32_t chest_point_count = 0u;
    TxBuf entity_points = { 0 };
    uint32_t original_chest_count = 0u, entity_count = 0u;
    TxBuf header = { 0 };
    TxBuf staged = { 0 };
    uint8_t* output = NULL;
    int32_t result = -1;

    if (matched_chest_count) *matched_chest_count = 0u;
    if (matched_tile_count) *matched_tile_count = 0u;
    tx_last_ptr = 0u;
    tx_last_len = 0u;
    tx_last_width = 0u;
    tx_last_height = 0u;

    if (!w) {
        tx_set_error("TERRAX_SESSION_NOT_FOUND", "world handle not found");
        return -1;
    }
    if (!map_layout(w, &width, &height, &cpr, &cpc, &chunk_count, &strip_bytes)) return -1;
    if (!collect_matching_chest_points(w, request, width, height, &chest_points, &chest_point_count)) goto cleanup;
    original_chest_count = chest_point_count;
    if (!tx_locate_tile_markers(w, request->tile_markers, request->tile_marker_count, &entity_points)) goto cleanup;
    entity_count = entity_points.len / sizeof(TxMarkerPoint);
    if (entity_count) {
        uint64_t bytes = ((uint64_t)chest_point_count + entity_count) * sizeof(MapChestPoint);
        if (bytes > UINT32_MAX) {
            tx_set_error("TERRAX_WASM_OOM", "entity point list exceeds WASM limits");
            goto cleanup;
        }
        MapChestPoint* combined = (MapChestPoint*)tx_alloc((uint32_t)bytes);
        if (!combined) {
            tx_set_error("TERRAX_WASM_OOM", "entity map points allocation failed");
            goto cleanup;
        }
        if (chest_point_count) memcpy(combined, chest_points, chest_point_count * sizeof(MapChestPoint));
        if (chest_points) tx_internal_free(chest_points);
        chest_points = combined;
        const TxMarkerPoint* points = (const TxMarkerPoint*)entity_points.data;
        for (uint32_t i = 0; i < entity_count; i++) {
            const MapMarkerEntry* marker = &request->tile_markers[points[i].marker_index];
            MapChestPoint* point = &chest_points[chest_point_count++];
            memset(point, 0, sizeof(*point));
            point->x = points[i].x; point->y = points[i].y; point->item_id = marker->icon_id;
            point->radius = marker->radius; point->line_width = marker->line_width;
            point->reserved[0] = 1u;
            memcpy(point->rgba, marker->rgba, 4u);
        }
        tx_internal_free(entity_points.data);
        entity_points.data = NULL;
    }


    if ((uint64_t)chunk_count * sizeof(uint32_t) > UINT32_MAX) {
        tx_set_error("TERRAX_BAD_DIMENSIONS", "map chunk tables exceed WASM limits");
        goto cleanup;
    }
    chunk_sizes = (uint32_t*)tx_alloc(chunk_count * sizeof(uint32_t));
    chunk_offsets = (uint32_t*)tx_alloc(chunk_count * sizeof(uint32_t));
    if (!chunk_sizes || !chunk_offsets) {
        tx_set_error("TERRAX_WASM_OOM", "map chunk tables allocation failed");
        goto cleanup;
    }
    memset(chunk_sizes, 0, chunk_count * sizeof(uint32_t));
    memset(chunk_offsets, 0, chunk_count * sizeof(uint32_t));

    buf_init(&header, 4096u);
    if (!header.ok) {
        tx_set_error("TERRAX_WASM_OOM", "map header allocation failed");
        goto cleanup;
    }
    write_map_header(&header, w);
    if (!header.ok) {
        tx_set_error("TERRAX_WASM_OOM", "map header generation failed");
        goto cleanup;
    }

    /* Fast path: retain only the bytes that compression actually produced.
     * chunk_offsets maps the x-major WLD scan back to the row-major MAP file.
     * The 32 MiB staging cap keeps staging + final output bounded; if the
     * compressed stream would exceed that cap, the exact low-memory two-pass
     * path below remains available. */
    buf_init(&staged, 64u * 1024u);
    if (staged.ok) {
        MapChunkStagingContext stage = {
            &staged,
            chunk_offsets,
            chunk_sizes,
            chunk_count,
            TX_MAP_SINGLE_PASS_STAGING_LIMIT_BYTES,
            0,
        };
        if (walk_map_chunks(w, request, chest_points, chest_point_count,
                            width, height, cpr, cpc, strip_bytes,
                            matched_tile_count ? &tile_matches : NULL,
                            stage_map_chunk, &stage)) {
            uint64_t final_size = header.len;
            for (uint32_t index = 0u; index < chunk_count; index++) {
                uint32_t size = chunk_sizes[index];
                uint32_t offset = chunk_offsets[index];
                if (!size || offset > staged.len || size > staged.len - offset ||
                    final_size > UINT32_MAX - 4u - size ||
                    final_size + 4u + size > TX_MAP_MAX_OUTPUT_BYTES) {
                    tx_set_error("TERRAX_RESULT_TOO_LARGE", "map output exceeds the 128 MiB budget");
                    goto cleanup;
                }
                final_size += 4u + size;
            }
            output = tx_alloc((uint32_t)final_size);
            if (!output) {
                tx_set_error("TERRAX_WASM_OOM", "map output allocation failed");
                goto cleanup;
            }
            memcpy(output, header.data, header.len);
            {
                uint32_t write_offset = header.len;
                for (uint32_t index = 0u; index < chunk_count; index++) {
                    uint32_t size = chunk_sizes[index];
                    uint32_t offset = chunk_offsets[index];
                    write_map_u32le(output + write_offset, size);
                    memcpy(output + write_offset + 4u, staged.data + offset, size);
                    write_offset += 4u + size;
                }
            }
            {
                TxBuf result_buffer = { output, (uint32_t)final_size, (uint32_t)final_size, 1 };
                result = set_result_buf(&result_buffer);
                if (result < 0) goto cleanup;
                output = NULL;
            }
            goto finalized;
        }
        if (!stage.fallback) goto cleanup;
    }

    /* Fallback: the previous exact two-pass implementation has lower peak
     * staging memory for unusually large compressed MAPs. */
    if (staged.data) {
        tx_internal_free(staged.data);
        staged.data = NULL;
    }
    memset(chunk_sizes, 0, chunk_count * sizeof(uint32_t));
    memset(chunk_offsets, 0, chunk_count * sizeof(uint32_t));
    tile_matches = 0u;
    tx_clear_error();
    {
        MapChunkMeasureContext measure = { chunk_sizes, chunk_count };
        if (!walk_map_chunks(w, request, chest_points, chest_point_count,
                             width, height, cpr, cpc, strip_bytes,
                             matched_tile_count ? &tile_matches : NULL,
                             measure_map_chunk, &measure)) goto cleanup;
    }
    {
        uint64_t final_size = header.len;
        for (uint32_t index = 0; index < chunk_count; index++) {
            if (final_size > UINT32_MAX - 4u - chunk_sizes[index]) {
                tx_set_error("TERRAX_BAD_DIMENSIONS", "map output exceeds WASM limits");
                goto cleanup;
            }
            chunk_offsets[index] = (uint32_t)final_size;
            final_size += 4u + chunk_sizes[index];
            if (final_size > TX_MAP_MAX_OUTPUT_BYTES) {
                tx_set_error("TERRAX_RESULT_TOO_LARGE", "map output exceeds the 128 MiB budget");
                goto cleanup;
            }
        }
        output = tx_alloc((uint32_t)final_size);
        if (!output) {
            tx_set_error("TERRAX_WASM_OOM", "map output allocation failed");
            goto cleanup;
        }
        memcpy(output, header.data, header.len);
        {
            MapChunkWriteContext write = { output, (uint32_t)final_size, chunk_offsets, chunk_sizes, chunk_count };
            if (!walk_map_chunks(w, request, chest_points, chest_point_count,
                                 width, height, cpr, cpc, strip_bytes, NULL,
                                 write_map_chunk, &write)) goto cleanup;
        }
        {
            TxBuf result_buffer = { output, (uint32_t)final_size, (uint32_t)final_size, 1 };
            result = set_result_buf(&result_buffer);
            if (result < 0) goto cleanup;
            output = NULL;
        }
    }

finalized:
    tx_last_width = width;
    tx_last_height = height;
    if (result >= 0) {
        if (matched_chest_count) *matched_chest_count = original_chest_count;
        if (matched_tile_count) *matched_tile_count = tile_matches + entity_count;
    }

cleanup:
    if (entity_points.data) tx_internal_free(entity_points.data);
    if (header.data) tx_internal_free(header.data);
    if (staged.data) tx_internal_free(staged.data);
    if (chunk_sizes) tx_internal_free(chunk_sizes);
    if (chunk_offsets) tx_internal_free(chunk_offsets);
    if (output) tx_internal_free(output);
    if (chest_points) tx_internal_free(chest_points);
    return result;
}

/* ================================================================ */
/*  Multi-color marker support                                       */
/* ================================================================ */
static int32_t generate_map(TxWorld* w, const int32_t* item_ids, uint32_t item_id_count,
                            const int32_t* tile_types, uint32_t tile_type_count,
                            uint32_t mark_chests) {
    uint32_t legacy_marker_value = nearest_map_value_for_rgb(
        &w->marker_color_index, 255u, 35u, 26u, 0u, NULL, NULL, 0u);
    const MapBuildRequest request = {
        item_ids,
        item_id_count,
        tile_types,
        tile_type_count,
        NULL,
        0u,
        NULL,
        0u,
        legacy_marker_value,
        mark_chests ? 1u : 0u,
        tile_type_count ? 1u : 0u,
        &w->marker_color_index
    };
    return generate_map_streaming(w, &request, NULL, NULL);
}

/* generate_map_marked -- map generation with per-marker colors */
static int32_t generate_map_marked(TxWorld* w,
                                   const MapMarkerEntry* chest_markers, uint32_t chest_count,
                                   const MapMarkerEntry* tile_markers, uint32_t tile_count,
                                   uint32_t* matched_chest_count,
                                   uint32_t* matched_tile_count) {
    MapBuildRequest request = {
        NULL,
        0u,
        NULL,
        0u,
        chest_markers,
        chest_count,
        tile_markers,
        tile_count,
        0u,
        0u,
        0u,
        &w->marker_color_index
    };
    for (uint32_t i = 0; i < tile_count; i++) request.paint_tile_marker_count += !tile_markers[i].locate;
    return generate_map_streaming(w, &request, matched_chest_count, matched_tile_count);
}

/* ================================================================ */
/*  Exported entry points                                            */
/* ================================================================ */

/* generate_map -- basic map generation (no markers) */
int32_t terra_generate_map(TxWorld* w) {
    return generate_map(w, NULL, 0u, NULL, 0u, 0u);
}

/* Cooperative MAP encoder: one 64-column strip, two exact compression passes.
 * Pass one measures row-major offsets; pass two emits positioned chunks. */
struct TxStreamMap {
    TxWorld* world;
    MapBuildRequest request;
    MapChestPoint* points;
    uint32_t point_count,width,height,cpr,cpc,chunks,strip_bytes,cx,cy,pass,active,pending,total;
    uint32_t emit_index,emit_offset,fallback;
    uint32_t *strip,*sizes,*offsets,*staged_offsets;
    TxMapColorCacheEntry colors[512];
    uint32_t color_count;
    TxBuf header,output,staged;
};
void tx_stream_map_free(TxStreamMap* p){
    if(!p)return;
    if(p->points)tx_internal_free(p->points);if(p->strip)tx_internal_free(p->strip);
    if(p->sizes)tx_internal_free(p->sizes);if(p->offsets)tx_internal_free(p->offsets);
    if(p->staged_offsets)tx_internal_free(p->staged_offsets);
    if(p->header.data)tx_internal_free(p->header.data);if(p->output.data)tx_internal_free(p->output.data);
    if(p->staged.data)tx_internal_free(p->staged.data);tx_internal_free(p);
}
TxStreamMap* tx_stream_map_begin(TxWorld* w,const MapMarkerEntry* chests,uint32_t chest_count,const MapMarkerEntry* tiles,uint32_t tile_count){
    TxStreamMap* p=(TxStreamMap*)tx_alloc(sizeof(*p));if(!p)return NULL;memset(p,0,sizeof(*p));p->world=w;
    p->request=(MapBuildRequest){NULL,0,NULL,0,chests,chest_count,tiles,tile_count,0,0,0,&w->marker_color_index,0};
    for(uint32_t i=0;i<tile_count;i++)p->request.paint_tile_marker_count+=!tiles[i].locate;
    if(!map_layout(w,&p->width,&p->height,&p->cpr,&p->cpc,&p->chunks,&p->strip_bytes))goto failed;
    p->strip=(uint32_t*)tx_alloc(p->strip_bytes);p->sizes=(uint32_t*)tx_alloc(p->chunks*4);p->offsets=(uint32_t*)tx_alloc(p->chunks*4);
    if(!p->strip||!p->sizes||!p->offsets)goto failed;
    p->staged_offsets=(uint32_t*)tx_alloc(p->chunks*4);
    if(p->staged_offsets)buf_init(&p->staged,64u*1024u);
    if(!p->staged_offsets||!p->staged.ok)p->fallback=1;
    if(!collect_matching_chest_points(w,&p->request,p->width,p->height,&p->points,&p->point_count))goto failed;
    const TxBuf* entity=w->prepared_output?&w->prepared_output->points:NULL;
    uint32_t count=entity?entity->len/sizeof(TxMarkerPoint):0;
    if(count){
        uint64_t bytes=((uint64_t)p->point_count+count)*sizeof(MapChestPoint);if(bytes>UINT32_MAX)goto failed;
        MapChestPoint* all=(MapChestPoint*)tx_alloc((uint32_t)bytes);if(!all)goto failed;
        if(p->point_count)memcpy(all,p->points,p->point_count*sizeof(MapChestPoint));if(p->points)tx_internal_free(p->points);p->points=all;
        const TxMarkerPoint* entries=(const TxMarkerPoint*)entity->data;
        for(uint32_t i=0;i<count;i++){
            const MapMarkerEntry* m=&tiles[entries[i].marker_index];MapChestPoint* point=&all[p->point_count++];memset(point,0,sizeof(*point));
            point->x=entries[i].x;point->y=entries[i].y;point->item_id=m->icon_id;point->radius=m->radius;point->line_width=m->line_width;point->reserved[0]=1;memcpy(point->rgba,m->rgba,4);
        }
    }
    buf_init(&p->header,4096);write_map_header(&p->header,w);if(!p->header.ok)goto failed;
    return p;
failed:tx_stream_map_free(p);return NULL;
}
int tx_stream_map_range(TxStreamMap* p,uint32_t* first,uint32_t* count){
    if(p->pending||p->active)return -1;
    if(p->pass==2){if(p->emit_index==p->chunks)return 0;p->pending=3;return -1;}
    if(p->cx==p->cpr){
        if(p->pass)return 0;
        uint64_t total=p->header.len;
        for(uint32_t i=0;i<p->chunks;i++){if(total+4+p->sizes[i]>UINT32_MAX)return -1;p->offsets[i]=(uint32_t)total;total+=4+p->sizes[i];}
        p->total=(uint32_t)total;
        if(!p->fallback){
            buf_init(&p->output,1048576u);
            if(!p->output.ok)p->fallback=1;
        }
        if(p->fallback){
            if(p->staged.data){tx_internal_free(p->staged.data);p->staged.data=NULL;}
            if(p->staged_offsets){tx_internal_free(p->staged_offsets);p->staged_offsets=NULL;}
            tx_clear_error();p->pass=1;p->cx=0;
        }else{p->pass=2;p->emit_offset=p->header.len;}
        p->pending=1;return -1;
    }
    *first=p->cx*64;*count=p->width-*first;if(*count>64)*count=64;
    prefill_chunk_strip_background(p->strip,p->cpc,p->cx,p->width,p->height,p->world->worldSurface,p->world->rockLayer);
    p->active=1;return 1;
}
int tx_stream_map_run(TxStreamMap* p,uint32_t x,uint32_t y,const TxTile* t,uint32_t run){
    if(!p->active||x/64!=p->cx)return 0;
    uint32_t value;if(!map_value_for_requested_tile(&p->request,t,run,NULL,p->colors,&p->color_count,&value))return 0;
    if(value&65535)fill_chunk_strip_run(p->strip,x%64,p->height,y,run,value);return 1;
}
int tx_stream_map_finish_strip(TxStreamMap* p){
    if(!p->active)return 0;p->active=0;
    for(uint32_t i=0;i<p->point_count;i++)draw_map_marker_on_strip(p->world,p->strip,p->cx*64,p->width,p->height,&p->points[i],p->colors,&p->color_count,NULL);
    if(!p->pass){
        for(uint32_t cy=0;cy<p->cpc;cy++){
            TxBuf bytes={0};if(!compress_chunk_exact(p->strip+cy*4096,&bytes))return 0;
            uint32_t index=cy*p->cpr+p->cx;p->sizes[index]=bytes.len;
            if(!p->fallback){
                MapChunkStagingContext stage={&p->staged,p->staged_offsets,p->sizes,p->chunks,TX_MAP_SINGLE_PASS_STAGING_LIMIT_BYTES,0};
                if(!stage_map_chunk(index,bytes.data,bytes.len,&stage)){
                    p->fallback=1;
                    if(p->staged.data){tx_internal_free(p->staged.data);p->staged.data=NULL;}
                    if(p->staged_offsets){tx_internal_free(p->staged_offsets);p->staged_offsets=NULL;}
                    tx_clear_error();
                }
            }
            tx_internal_free(bytes.data);
        }
        p->cx++;return 1;
    }
    p->cy=0;p->pending=2;return 1;
}
int tx_stream_map_pull(TxStreamMap* p,uint32_t* offset,const uint8_t** bytes,uint32_t* length){
    if(!p->pending)return 0;
    if(p->pending==1){*offset=0;*bytes=p->header.data;*length=p->header.len;return 1;}
    if(p->pending==3){
        if(!p->output.len){
            while(p->emit_index<p->chunks){
                uint32_t index=p->emit_index,size=p->sizes[index],start=p->staged_offsets[index];
                if(!size||start>p->staged.len||size>p->staged.len-start||size>1048576u-4u)return -1;
                if(p->output.len>1048576u-4u-size)break;
                buf_u32le(&p->output,size);buf_bytes(&p->output,p->staged.data+start,size);
                p->emit_index++;
            }
        }
        *offset=p->emit_offset;*bytes=p->output.data;*length=p->output.len;return p->output.ok&&p->output.len?1:-1;
    }
    uint32_t index=p->cy*p->cpr+p->cx;
    if(!p->output.data){TxBuf compressed={0};if(!compress_chunk_exact(p->strip+p->cy*4096,&compressed))return -1;
        if(compressed.len!=p->sizes[index]){tx_internal_free(compressed.data);return -1;}
        buf_init(&p->output,compressed.len+4);buf_u32le(&p->output,compressed.len);buf_bytes(&p->output,compressed.data,compressed.len);tx_internal_free(compressed.data);if(!p->output.ok)return -1;
    }
    *offset=p->offsets[index];*bytes=p->output.data;*length=p->output.len;return 1;
}
int tx_stream_map_ack(TxStreamMap* p){
    if(!p->pending)return 0;
    if(p->pending==1){p->pending=0;return 1;}
    if(p->pending==3){p->emit_offset+=p->output.len;p->output.len=0;p->pending=0;return 1;}
    if(p->output.data)tx_internal_free(p->output.data);memset(&p->output,0,sizeof(p->output));
    if(++p->cy==p->cpc){p->cx++;p->pending=0;}return 1;
}
uint32_t tx_stream_map_size(TxStreamMap* p){return p->total;}

/* render_lit_map_marked -- map generation with per-marker colors */
int32_t terra_render_lit_map_marked(TxWorld* w,
                                     const MapMarkerEntry* chest_markers, uint32_t chest_count,
                                     const MapMarkerEntry* tile_markers, uint32_t tile_count,
                                     uint32_t* matched_chest_count,
                                     uint32_t* matched_tile_count) {
    return generate_map_marked(
        w, chest_markers, chest_count, tile_markers, tile_count,
        matched_chest_count, matched_tile_count);
}
