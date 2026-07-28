/*
 * terra_render.c -- Rendering pipeline for TerraWasm.
 *
 * Pure C, no libc dependencies. Implements:
 *   - Color system (tile/wall/background)
 *   - Preview RGBA rendering (column-major tile scan)
 *   - CRC32 for PNG chunks
 *   - Fixed-Huffman zlib deflate with LZ77 hash-chain matching
 *   - PNG encoding (RGBA -> filter bytes -> zlib -> PNG chunks)
 */
#include "terra_types.h"
#include "terra_map.h"
#include "terra_color_data.h"

/* ====================================================================
 * Extern declarations from terra_mem.c
 * ==================================================================== */

extern void* memset(void* dst, int value, unsigned long n);
extern void* memcpy(void* dst, const void* src, unsigned long n);

extern void  buf_init(TxBuf* b, uint32_t cap);
extern void  buf_u8(TxBuf* b, uint8_t v);
extern void  buf_bytes(TxBuf* b, const void* p, uint32_t n);
extern void  buf_u32be(TxBuf* b, uint32_t v);

extern uint8_t* tx_alloc(uint32_t size);
extern void tx_internal_free(void* ptr);

extern void  tx_set_error(const char* code, const char* message);
extern void  tx_clear_error(void);
extern int   set_result_buf(TxBuf* b);
extern int   set_result_bytes(uint8_t* p, uint32_t len);

extern const uint8_t* tx_get_tile_colors(void);
extern uint32_t       tx_get_tile_color_count(void);
extern const uint8_t* tx_get_wall_colors(void);
extern uint32_t       tx_get_wall_color_count(void);

extern uint32_t tx_last_ptr;
extern uint32_t tx_last_len;
extern uint32_t tx_last_width;
extern uint32_t tx_last_height;
extern uint32_t tx_last_stride;

/* ====================================================================
 * Extern declarations from terra_wld.c
 * ==================================================================== */

extern int read_tile_at(TxWorld* w, uint32_t* off, uint32_t end, TxTile* t);
extern int tile_important(TxWorld* w, uint16_t type);
extern uint8_t  rd_u8(const uint8_t* p, uint32_t len, uint32_t* off);
extern uint16_t rd_u16le(const uint8_t* p, uint32_t len, uint32_t* off);
extern int32_t  rd_i32le(const uint8_t* p, uint32_t len, uint32_t* off);
extern void     rd_skip_string_value(const uint8_t* p, uint32_t len, uint32_t* off);

/* ====================================================================
 * Color system
 * ==================================================================== */

static int read_color_table(const uint8_t* table, uint32_t count, uint32_t id, uint8_t out[4]) {
  if (!table || id >= count) return 0;
  const uint8_t* c = table + id * 4u;
  if (c[0] == 0u && c[1] == 0u && c[2] == 0u) return 0;
  out[0] = c[0]; out[1] = c[1]; out[2] = c[2]; out[3] = 255u;
  return 1;
}

/* Compute tile variant index from U/V frame values, matching TerraX Tile::get_tile_variant_index(). */
static uint16_t get_tile_variant_index(uint16_t type, int16_t u, int16_t v) {
    uint16_t idx = 0;
    switch (type) {
        case 4:   if (u < 66) idx = 1; break;
        case 15:  if (v / 40 == 1 || v / 40 == 20) idx = 1; break;
        case 21: case 421:
            switch (u / 36) {
                case 1: case 2: case 10: case 13: case 15: idx = 1; break;
                case 3: case 4: idx = 2; break;
                case 6: idx = 3; break;
                case 11: case 17: idx = 4; break;
            }
            break;
        case 26:  if (u >= 54) idx = 1; break;
        case 27:  if (v < 34) idx = 1; break;
        case 28: case 653:
            if (v < 144) idx = 0;
            else if (v < 252) idx = 1;
            else if (v < 360 || (v > 900 && v < 1008)) idx = 2;
            else if (v < 468) idx = 3;
            else if (v < 576) idx = 4;
            else if (v < 684) idx = 5;
            else if (v < 792) idx = 6;
            else if (v < 898) idx = 8;
            else if (v < 1006) idx = 7;
            else if (v < 1114) idx = 0;
            else if (v < 1222) idx = 3;
            else idx = 7;
            break;
        case 31:  if (u >= 36) idx = 1; break;
        case 82: case 83: case 84:
            switch (u) {
                case 0: idx = 0; break; case 18: idx = 1; break;
                case 36: idx = 2; break; case 54: idx = 3; break;
                case 72: idx = 4; break; case 90: idx = 5; break;
                default: idx = 6; break;
            }
            break;
        case 89: {
            uint16_t num = u / 54;
            idx = (num == 0 || num == 21 || num == 23) ? 0 : (num == 43 ? 2 : 1);
            break;
        }
        case 105:
            if (1548 <= u && u <= 1654) idx = 1;
            else if (1656 <= u && u <= 1798) idx = 2;
            break;
        case 129: idx = (u >= 324) ? 1 : 0; break;
        case 160: idx = (u >= 90) ? 1 : 0; break;
        case 171: idx = (u / 18 == 1) ? 1 : 0; break;
        case 172: idx = (u / 18 == 2) ? 1 : 0; break;
        case 186: case 187: case 188: case 189: case 190:
        case 191: case 192: case 193: case 194: case 195:
        case 196: case 197: case 198: case 199:
        case 200: case 201: case 202: case 203: case 204: case 205:
        case 206: case 207: case 208: case 209:
            idx = (u / 18 >= 6) ? 1 : 0; break;
        case 227: idx = (u >= 288) ? 1 : 0; break;
        case 240: idx = (u / 18 == 1) ? 1 : 0; break;
        case 254: idx = (u >= 234) ? 1 : 0; break;
        case 272: idx = (u / 54 >= 6) ? 1 : 0; break;
        case 275: idx = (u / 36 == 1) ? 1 : 0; break;
        case 323: idx = (u / 18 == 1) ? 1 : 0; break;
        case 336: idx = (u >= 252) ? 1 : 0; break;
        case 340: idx = (u >= 288) ? 1 : 0; break;
        case 376: idx = (u / 18 == 1) ? 1 : 0; break;
        case 413: idx = (u / 18 == 1) ? 1 : 0; break;
        case 424: idx = (u >= 36) ? 1 : 0; break;
        case 441: idx = (u / 18 == 1) ? 1 : 0; break;
        case 443: idx = (u >= 36) ? 1 : 0; break;
        case 461: idx = (u / 54 == 1) ? 1 : 0; break;
        case 462: idx = (u / 36 == 1) ? 1 : 0; break;
        case 463: idx = (u / 54 == 1) ? 1 : 0; break;
        case 464: idx = (u / 36 == 1) ? 1 : 0; break;
        case 465: idx = (u / 18 == 1) ? 1 : 0; break;
        case 484: idx = (u / 36 == 1) ? 1 : 0; break;
        case 499: idx = (u / 18 == 1) ? 1 : 0; break;
        case 518:
            switch (u) {
                case 0: idx = 0; break; case 18: idx = 1; break;
                case 36: idx = 2; break; default: idx = 0; break;
            }
            break;
        case 519:
            switch (u) {
                case 0: idx = 0; break; case 18: idx = 1; break;
                case 36: idx = 2; break; case 54: idx = 3; break;
                case 72: idx = 4; break; default: idx = 0; break;
            }
            break;
        case 529:
            switch (u) {
                case 0: idx = 0; break; case 18: idx = 1; break;
                case 36: idx = 2; break; case 54: idx = 3; break;
                case 72: idx = 4; break; default: idx = 0; break;
            }
            break;
    }
    return idx;
}

static int read_builtin_tile_color(uint16_t id, uint8_t variant, uint8_t out[4]) {
  if (id >= TX_COLOR_MAX_IDS) return 0;
  if (variant >= TX_COLOR_MAX_VARIANTS) variant = 0;
  const uint8_t* c = TX_BUILTIN_TILE_COLORS + ((uint32_t)id * TX_COLOR_MAX_VARIANTS + variant) * 4u;
  if (c[3] == 0u) {
    /* Fallback to variant 0 if requested variant is undefined */
    c = TX_BUILTIN_TILE_COLORS + ((uint32_t)id * TX_COLOR_MAX_VARIANTS) * 4u;
    if (c[3] == 0u) return 0;
  }
  out[0] = c[0]; out[1] = c[1]; out[2] = c[2]; out[3] = 255u;
  return 1;
}

static int read_builtin_wall_color(uint16_t id, uint8_t variant, uint8_t out[4]) {
  if (id >= TX_COLOR_MAX_IDS) return 0;
  if (variant >= TX_COLOR_MAX_VARIANTS) variant = 0;
  const uint8_t* c = TX_BUILTIN_WALL_COLORS + ((uint32_t)id * TX_COLOR_MAX_VARIANTS + variant) * 4u;
  if (c[3] == 0u) {
    c = TX_BUILTIN_WALL_COLORS + ((uint32_t)id * TX_COLOR_MAX_VARIANTS) * 4u;
    if (c[3] == 0u) return 0;
  }
  out[0] = c[0]; out[1] = c[1]; out[2] = c[2]; out[3] = 255u;
  return 1;
}

static int is_bad_tile(uint16_t id) {
  for (int i = 0; i < TX_BAD_TILE_COUNT; i++) {
    if (TX_BAD_TILE_IDS[i] == id) return 1;
  }
  return 0;
}

static int is_bad_wall(uint16_t id) {
  for (int i = 0; i < TX_BAD_WALL_COUNT; i++) {
    if (TX_BAD_WALL_IDS[i] == id) return 1;
  }
  return 0;
}

static uint8_t scale_u8(uint8_t v, uint32_t num, uint32_t den) {
  return (uint8_t)(((uint32_t)v * num) / den);
}

/* Get paint RGB from TX_PAINT_COLORS table. Paint IDs are 1-30. */
static void get_paint_color(uint8_t paint_id, uint8_t* r, uint8_t* g, uint8_t* b) {
  if (paint_id == 0 || paint_id > 30) { *r = 255; *g = 255; *b = 255; return; }
  uint32_t idx = (uint32_t)(paint_id - 1) * 3u;
  *r = TX_PAINT_COLORS[idx];
  *g = TX_PAINT_COLORS[idx + 1u];
  *b = TX_PAINT_COLORS[idx + 2u];
}

/*
 * Blend a base color with a paint color, matching TerraX's blendWithPaint logic.
 *
 * paint_id 0 = no change
 * paint_id 29 (shadow): multiply paint color by (blue * 0.3)
 * paint_id 30 (negative): invert for tiles, half-invert (50%) for walls
 * Other paints: multiply paint color by max(r,g,b) / 255
 */
static void blend_with_paint(uint8_t base_r, uint8_t base_g, uint8_t base_b,
                             uint8_t paint_id, int is_wall,
                             uint8_t* out_r, uint8_t* out_g, uint8_t* out_b) {
  if (paint_id == 0) { *out_r = base_r; *out_g = base_g; *out_b = base_b; return; }

  uint8_t pr, pg, pb;
  get_paint_color(paint_id, &pr, &pg, &pb);

  /* Convert to float-like integer math. Use fixed-point with *256 scaling. */
  uint32_t oldR = (uint32_t)base_r;
  uint32_t oldG = (uint32_t)base_g;
  uint32_t oldB = (uint32_t)base_b;

  if (paint_id == 29) {
    /* Shadow paint: ratio = oldBlue/255 * 0.3 */
    /* Use fixed-point: ratio_fixed = oldB * 77 / 255 (0.3 * 255 = 77) */
    /* Then result = paint * ratio = paint * oldB * 77 / (255 * 255) */
    uint32_t ratio_fp = oldB * 77u; /* 0.3 * 255 = 76.5 ~ 77 */
    *out_r = (uint8_t)((pr * ratio_fp) / 65025u); /* 65025 = 255*255 */
    *out_g = (uint8_t)((pg * ratio_fp) / 65025u);
    *out_b = (uint8_t)((pb * ratio_fp) / 65025u);
    return;
  }

  if (paint_id != 30) {
    /* Normal paint: ratio = max(r,g,b) / 255 */
    uint32_t maxVal = oldR;
    if (oldG > maxVal) maxVal = oldG;
    if (oldB > maxVal) maxVal = oldB;
    /* result = paint * maxVal / 255 */
    *out_r = (uint8_t)((pr * maxVal) / 255u);
    *out_g = (uint8_t)((pg * maxVal) / 255u);
    *out_b = (uint8_t)((pb * maxVal) / 255u);
    return;
  }

  /* Negative paint (id 30) */
  if (is_wall) {
    /* Wall: half-invert (50%) */
    *out_r = (uint8_t)(((255u - oldR) * 128u) / 255u); /* 0.5 * 255 = 127.5 ~ 128 */
    *out_g = (uint8_t)(((255u - oldG) * 128u) / 255u);
    *out_b = (uint8_t)(((255u - oldB) * 128u) / 255u);
  } else {
    /* Tile: full invert */
    *out_r = (uint8_t)(255u - oldR);
    *out_g = (uint8_t)(255u - oldG);
    *out_b = (uint8_t)(255u - oldB);
  }
}

static void lerp_rgb(uint8_t out[4], int32_t r0, int32_t g0, int32_t b0, int32_t r1, int32_t g1, int32_t b1, uint32_t num, uint32_t den) {
  if (!den) den = 1u;
  out[0] = (uint8_t)(r0 + ((r1 - r0) * (int32_t)num) / (int32_t)den);
  out[1] = (uint8_t)(g0 + ((g1 - g0) * (int32_t)num) / (int32_t)den);
  out[2] = (uint8_t)(b0 + ((b1 - b0) * (int32_t)num) / (int32_t)den);
  out[3] = 255u;
}

static void background_color(uint32_t y, uint32_t world_h, uint32_t ground, uint32_t rock, uint8_t out[4]) {
  if (ground == 0u) ground = world_h > 3u ? (world_h * 35u) / 100u : 1u;
  if (rock == 0u) rock = world_h > 2u ? (world_h * 65u) / 100u : ground + 1u;
  if (ground == 0u) ground = 1u;
  if (rock <= ground) rock = ground + 1u;
  if (y < ground) { lerp_rgb(out, 50, 40, 255, 145, 185, 255, y, ground); return; }
  if (y < rock) { lerp_rgb(out, 88, 61, 46, 37, 78, 123, y - ground, rock - ground); return; }
  if (y + 200u < world_h) { lerp_rgb(out, 74, 67, 60, 53, 70, 97, y - rock, (world_h - 200u > rock) ? (world_h - 200u - rock) : 1u); return; }
  out[0] = 50; out[1] = 44; out[2] = 38; out[3] = 255u;
}

static int tile_is_non_empty(const TxTile* t) {
  if (t->active && !t->invisible_block) return 1;
  if (t->liquid_amount && t->liquid_type) return 1;
  if (t->wall && !t->invisible_wall) return 1;
  return 0;
}

static void color_for_tile(const TxTile* t, uint32_t y, uint32_t world_h, uint32_t ground, uint32_t rock, uint8_t out[4]) {
  /* External color table references (fallback) */
  const uint8_t* tile_colors = tx_get_tile_colors();
  uint32_t tile_color_count = tx_get_tile_color_count();
  const uint8_t* wall_colors = tx_get_wall_colors();
  uint32_t wall_color_count = tx_get_wall_color_count();

  if (t->active && !t->invisible_block) {
    /* Skip bad tile IDs */
    if (is_bad_tile(t->type)) {
      /* Bad tile: fall through to wall/liquid/background */
    } else {
      /* Try builtin tile color first */
      if (read_builtin_tile_color(t->type, get_tile_variant_index(t->type, t->frame_x, t->frame_y), out)) {
        /* Apply paint blending for tiles */
        if (t->tile_color > 0) {
          uint8_t pr, pg, pb;
          blend_with_paint(out[0], out[1], out[2], t->tile_color, 0, &pr, &pg, &pb);
          out[0] = pr; out[1] = pg; out[2] = pb;
        }
        return;
      }
      /* Fallback to external color table */
      if (read_color_table(tile_colors, tile_color_count, t->type, out)) {
        if (t->tile_color > 0) {
          uint8_t pr, pg, pb;
          blend_with_paint(out[0], out[1], out[2], t->tile_color, 0, &pr, &pg, &pb);
          out[0] = pr; out[1] = pg; out[2] = pb;
        }
        return;
      }
      /* Grayscale fallback */
      uint32_t v = 60u + ((uint32_t)t->type * 37u) % 120u;
      out[0] = (uint8_t)v; out[1] = (uint8_t)v; out[2] = (uint8_t)v; out[3] = 255u;
      return;
    }
  }
  if (t->liquid_amount && t->liquid_type) {
    /* Liquid colors matching TerraX exactly */
    if (t->liquid_type == 1u) { out[0]=9;   out[1]=61;  out[2]=191; out[3]=255; return; }  /* Water */
    if (t->liquid_type == 2u) { out[0]=253; out[1]=32;  out[2]=3;   out[3]=255; return; }  /* Lava */
    if (t->liquid_type == 3u) { out[0]=254; out[1]=194; out[2]=20;  out[3]=255; return; }  /* Honey */
    if (t->liquid_type == 4u) { out[0]=161; out[1]=127; out[2]=255; out[3]=255; return; }  /* Shimmer */
    out[0]=60; out[1]=120; out[2]=230; out[3]=255; return;  /* Default water fallback */
  }
  if (t->wall && !t->invisible_wall) {
    /* Skip bad wall IDs */
    if (is_bad_wall(t->wall)) {
      background_color(y, world_h, ground, rock, out);
      return;
    }
    /* Try builtin wall color first */
    if (read_builtin_wall_color(t->wall, 0, out)) {
      /* Darken walls (62% like before) */
      out[0] = scale_u8(out[0], 62u, 100u);
      out[1] = scale_u8(out[1], 62u, 100u);
      out[2] = scale_u8(out[2], 62u, 100u);
      /* Apply paint blending for walls */
      if (t->wall_color > 0) {
        uint8_t pr, pg, pb;
        blend_with_paint(out[0], out[1], out[2], t->wall_color, 1, &pr, &pg, &pb);
        out[0] = pr; out[1] = pg; out[2] = pb;
      }
      return;
    }
    /* Fallback to external color table */
    if (read_color_table(wall_colors, wall_color_count, t->wall, out)) {
      out[0] = scale_u8(out[0], 62u, 100u);
      out[1] = scale_u8(out[1], 62u, 100u);
      out[2] = scale_u8(out[2], 62u, 100u);
      if (t->wall_color > 0) {
        uint8_t pr, pg, pb;
        blend_with_paint(out[0], out[1], out[2], t->wall_color, 1, &pr, &pg, &pb);
        out[0] = pr; out[1] = pg; out[2] = pb;
      }
      return;
    }
    out[0]=45; out[1]=45; out[2]=55; out[3]=255; return;
  }
  background_color(y, world_h, ground, rock, out);
}

/* ====================================================================
 * Adler-32 checksum
 * ==================================================================== */

static uint32_t adler32_bytes(const uint8_t* data, uint32_t length) {
  uint32_t a = 1u, b = 0u;
  for (uint32_t i = 0; i < length; i++) {
    a += data[i];
    if (a >= 65521u) a -= 65521u;
    b += a;
    if (b >= 65521u) b %= 65521u;
  }
  return ((b << 16) | a) & 0xffffffffu;
}

/* ====================================================================
 * CRC32 for PNG chunks
 * ==================================================================== */

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

static uint32_t crc32_two(const uint8_t* a, uint32_t alen, const uint8_t* b, uint32_t blen) {
  init_crc();
  uint32_t c = 0xffffffffu;
  for (uint32_t i = 0; i < alen; i++)
    c = crc_table[(c ^ a[i]) & 255u] ^ (c >> 8);
  for (uint32_t i = 0; i < blen; i++)
    c = crc_table[(c ^ b[i]) & 255u] ^ (c >> 8);
  return c ^ 0xffffffffu;
}

/* ====================================================================
 * Bit-level output for fixed Huffman
 * ==================================================================== */

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

/* ====================================================================
 * Fixed Huffman literal/length/distance encoding
 * ==================================================================== */

static void fixed_literal(TxBuf* out, uint32_t* bitbuf, uint32_t* bitcnt, uint32_t sym) {
  if (sym <= 143u) bw_bit(out, bitbuf, bitcnt, reverse_bits(0x30u + sym, 8u), 8u);
  else if (sym <= 255u) bw_bit(out, bitbuf, bitcnt, reverse_bits(0x190u + (sym - 144u), 9u), 9u);
  else if (sym <= 279u) bw_bit(out, bitbuf, bitcnt, reverse_bits(sym - 256u, 7u), 7u);
  else bw_bit(out, bitbuf, bitcnt, reverse_bits(0xC0u + (sym - 280u), 8u), 8u);
}

static const uint16_t LEN_BASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t  LEN_EXTRA[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t DIST_BASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t  DIST_EXTRA[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

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

/* ====================================================================
 * LZ77 hash chain
 * ==================================================================== */

static int32_t z_head[32768];

static uint32_t fast_hash3(const uint8_t* data, uint32_t pos) {
  return ((uint32_t)data[pos] * 251u ^ (uint32_t)data[pos + 1u] * 47u ^ (uint32_t)data[pos + 2u] * 13u) & 32767u;
}

/* ====================================================================
 * Fixed-Huffman zlib deflate
 * ==================================================================== */

static void write_zlib_fixed(TxBuf* out, const uint8_t* data, uint32_t len) {
  if (!len) {
    static const uint8_t empty[7] = {0x78,0x01,0x03,0x00,0x00,0x00,0x01};
    buf_bytes(out, empty, 7u);
    return;
  }
  for (uint32_t i = 0; i < 32768u; i++) z_head[i] = -1;
  /* zlib header: CMF=0x78 (deflate, window=32k), FLG=0x01 (fastest) */
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
  /* end-of-block symbol */
  fixed_literal(out, &bitbuf, &bitcnt, 256u);
  bw_finish(out, &bitbuf, &bitcnt);
  /* adler32 checksum (big-endian) */
  uint32_t ad = adler32_bytes(data, len);
  buf_u8(out, (uint8_t)(ad >> 24)); buf_u8(out, (uint8_t)(ad >> 16));
  buf_u8(out, (uint8_t)(ad >> 8));  buf_u8(out, (uint8_t)ad);
}

/* ====================================================================
 * PNG chunk helper
 * ==================================================================== */

static void png_chunk(TxBuf* out, const char type[4], const uint8_t* data, uint32_t len) {
  buf_u32be(out, len);
  uint32_t type_start = out->len;
  buf_bytes(out, type, 4);
  if (len) buf_bytes(out, data, len);
  uint32_t crc = crc32_bytes(out->data + type_start, 4u + len);
  buf_u32be(out, crc);
}

/* ====================================================================
 * Preview RGBA rendering (column-major tile scan)
 * ==================================================================== */

static void render_preview_to(TxWorld* w, uint8_t* rgba, uint32_t pw, uint32_t ph) {
  uint32_t ground = (uint32_t)w->worldSurface;
  uint32_t rock   = (uint32_t)w->rockLayer;

  /* Fill background colors row by row. Alpha temporarily stores the number
     of non-empty source samples accumulated into each preview pixel. */
  for (uint32_t py = 0u; py < ph; py++) {
    uint8_t bg[4];
    uint32_t wy = (uint32_t)(((uint64_t)py * (uint64_t)w->maxTilesY) / ph);
    background_color(wy, (uint32_t)w->maxTilesY, ground, rock, bg);
    for (uint32_t px = 0u; px < pw; px++) {
      uint32_t o = (py * pw + px) * 4u;
      rgba[o] = bg[0]; rgba[o + 1u] = bg[1]; rgba[o + 2u] = bg[2]; rgba[o + 3u] = 0u;
    }
  }

  /* Column-major tile scan. Prefer tile override bytes when present so a
     freshly modified world can be previewed before it is reopened. */
  uint8_t* saved_file = w->file;
  uint32_t saved_len = w->file_len;
  uint32_t off;
  uint32_t end;
  if (w->section_overrides[1].active) {
    w->file = w->section_overrides[1].data;
    w->file_len = w->section_overrides[1].len;
    off = 0;
    end = w->section_overrides[1].len;
  } else {
    off = w->starts[1];
    end = w->ends[1];
  }
  uint32_t width  = (uint32_t)w->maxTilesX;
  uint32_t height = (uint32_t)w->maxTilesY;

  for (uint32_t x = 0; x < width; x++) {
    uint32_t px = (uint32_t)(((uint64_t)x * pw) / width);
    for (uint32_t y = 0; y < height;) {
      TxTile t;
      if (!read_tile_at(w, &off, end, &t)) {
        goto render_done;
      }
      uint32_t run = (uint32_t)t.same + 1u;
      if (!tile_is_non_empty(&t)) { y += run; continue; }

      uint8_t c[4];
      color_for_tile(&t, y, height, ground, rock, c);

      /* Aggregate every source column/run covered by this preview pixel.
         Keeping the sample count in alpha avoids a second count buffer. */
      uint32_t py0 = (uint32_t)(((uint64_t)y * ph) / height);
      uint32_t py1 = (uint32_t)((((uint64_t)y + run) * ph) / height);
      if (py1 <= py0) py1 = py0 + 1u;
      if (px < pw && py0 < ph) {
        if (py1 > ph) py1 = ph;
        for (uint32_t py = py0; py < py1; py++) {
          uint32_t o = (py * pw + px) * 4u;
          uint32_t count = rgba[o + 3u];
          if (count == 0u) {
            rgba[o] = c[0]; rgba[o + 1u] = c[1]; rgba[o + 2u] = c[2];
            rgba[o + 3u] = 1u;
          } else if (count < 255u) {
            uint32_t next = count + 1u;
            rgba[o] = (uint8_t)(((uint32_t)rgba[o] * count + c[0]) / next);
            rgba[o + 1u] = (uint8_t)(((uint32_t)rgba[o + 1u] * count + c[1]) / next);
            rgba[o + 2u] = (uint8_t)(((uint32_t)rgba[o + 2u] * count + c[2]) / next);
            rgba[o + 3u] = (uint8_t)next;
          } else {
            rgba[o] = (uint8_t)(((uint32_t)rgba[o] * 255u + c[0]) >> 8);
            rgba[o + 1u] = (uint8_t)(((uint32_t)rgba[o + 1u] * 255u + c[1]) >> 8);
            rgba[o + 2u] = (uint8_t)(((uint32_t)rgba[o + 2u] * 255u + c[2]) >> 8);
          }
        }
      }
      y += run;
    }
  }
render_done:
  w->file = saved_file;
  w->file_len = saved_len;
  for (uint32_t p = 0u; p < pw * ph; p++) {
    rgba[p * 4u + 3u] = 255u;
  }
}

static int compute_preview_size(TxWorld* w, uint32_t max_w, uint32_t max_h,
                                uint32_t* out_pw, uint32_t* out_ph,
                                uint32_t* out_stride) {
  if (!w) {
    tx_set_error("TERRAX_INVALID_HANDLE", "null world handle");
    return 0;
  }
  if (w->maxTilesX <= 0 || w->maxTilesY <= 0) {
    tx_set_error("TERRAX_BAD_PREVIEW_SIZE", "world dimensions must be positive");
    return 0;
  }

  {
    uint32_t ww = (uint32_t)w->maxTilesX;
    uint32_t wh = (uint32_t)w->maxTilesY;
    uint32_t pw = ww;
    uint32_t ph = wh;

    if (max_w || max_h) {
      uint64_t sx_num = max_w ? max_w : ww;
      uint64_t sx_den = ww;
      uint64_t sy_num = max_h ? max_h : wh;
      uint64_t sy_den = wh;
      uint64_t scale_num = sx_num;
      uint64_t scale_den = sx_den;
      if (sy_num * scale_den < scale_num * sy_den) {
        scale_num = sy_num;
        scale_den = sy_den;
      }
      if (scale_num > scale_den) scale_num = scale_den;
      pw = (uint32_t)(((uint64_t)ww * scale_num) / scale_den);
      ph = (uint32_t)(((uint64_t)wh * scale_num) / scale_den);
    }

    if (pw == 0u || ph == 0u) {
      tx_set_error("TERRAX_BAD_PREVIEW_SIZE", "invalid preview size");
      return 0;
    }

    {
      uint64_t rgba_len64 = (uint64_t)pw * ph * 4u;
      uint64_t stride64 = (uint64_t)pw * 4u;
      if (rgba_len64 > UINT32_MAX || stride64 > UINT32_MAX) {
        tx_set_error("TERRAX_BAD_PREVIEW_SIZE", "preview dimensions exceed WASM limits");
        return 0;
      }
      *out_pw = pw;
      *out_ph = ph;
      *out_stride = (uint32_t)stride64;
    }
  }
  return 1;
}

static int encode_png_from_owned_rgba(uint8_t* rgba, uint32_t pw, uint32_t ph) {
  uint64_t raw_len64 = ((uint64_t)pw * 4u + 1u) * ph;
  if (raw_len64 > UINT32_MAX || raw_len64 + 256u > UINT32_MAX) {
    tx_internal_free(rgba);
    tx_set_error("TERRAX_BAD_PREVIEW_SIZE", "PNG scanline buffer exceeds WASM limits");
    return -1;
  }

  {
    uint32_t raw_len = (uint32_t)raw_len64;
    uint8_t* raw = tx_alloc(raw_len);
    if (!raw) {
      tx_internal_free(rgba);
      tx_set_error("TERRAX_WASM_OOM", "png raw allocation failed");
      return -1;
    }

    uint32_t ro = 0u;
    for (uint32_t y = 0u; y < ph; y++) {
      raw[ro++] = 0u;
      memcpy(raw + ro, rgba + y * pw * 4u, pw * 4u);
      ro += pw * 4u;
    }

    {
      TxBuf z;
      buf_init(&z, raw_len + 256u);
      write_zlib_fixed(&z, raw, raw_len);
      if (!z.ok || z.len > UINT32_MAX - 128u) {
        if (z.data) tx_internal_free(z.data);
        tx_internal_free(raw);
        tx_internal_free(rgba);
        tx_set_error("TERRAX_WASM_OOM", "PNG compression failed");
        return -1;
      }

      {
        TxBuf out;
        buf_init(&out, z.len + 128u);
        if (!out.ok) {
          tx_internal_free(z.data);
          tx_internal_free(raw);
          tx_internal_free(rgba);
          tx_set_error("TERRAX_WASM_OOM", "PNG output allocation failed");
          return -1;
        }

        {
          static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
          uint8_t ihdr[13];
          buf_bytes(&out, sig, 8);
          ihdr[0] = (uint8_t)(pw >> 24); ihdr[1] = (uint8_t)(pw >> 16);
          ihdr[2] = (uint8_t)(pw >> 8);  ihdr[3] = (uint8_t)pw;
          ihdr[4] = (uint8_t)(ph >> 24); ihdr[5] = (uint8_t)(ph >> 16);
          ihdr[6] = (uint8_t)(ph >> 8);  ihdr[7] = (uint8_t)ph;
          ihdr[8] = 8;
          ihdr[9] = 6;
          ihdr[10] = 0;
          ihdr[11] = 0;
          ihdr[12] = 0;
          png_chunk(&out, "IHDR", ihdr, 13);
        }

        png_chunk(&out, "IDAT", z.data, z.len);
        png_chunk(&out, "IEND", (const uint8_t*)0, 0u);

        tx_internal_free(z.data);
        tx_internal_free(raw);
        tx_internal_free(rgba);
        if (!out.ok) {
          if (out.data) tx_internal_free(out.data);
          tx_set_error("TERRAX_WASM_OOM", "PNG assembly failed");
          return -1;
        }

        tx_last_width = pw;
        tx_last_height = ph;
        tx_last_stride = pw * 4u;
        return set_result_buf(&out);
      }
    }
  }
}

static uint32_t clamp_preview_coord(int32_t world_coord, uint32_t world_extent,
                                    uint32_t preview_extent) {
  if (preview_extent == 0u || world_extent == 0u) return 0u;
  if (world_coord <= 0) return 0u;
  if ((uint32_t)world_coord >= world_extent) return preview_extent - 1u;
  {
    uint64_t scaled = ((uint64_t)(uint32_t)world_coord * preview_extent) / world_extent;
    if (scaled >= preview_extent) scaled = preview_extent - 1u;
    return (uint32_t)scaled;
  }
}

static uint32_t scale_marker_measure(uint8_t source_value,
                                     uint32_t world_w, uint32_t world_h,
                                     uint32_t preview_w, uint32_t preview_h) {
  uint32_t scaled = 0u;
  if (source_value == 0u) return 0u;
  if (world_w > 0u) scaled = (uint32_t)(((uint64_t)source_value * preview_w) / world_w);
  if (world_h > 0u) {
    uint32_t y_scaled = (uint32_t)(((uint64_t)source_value * preview_h) / world_h);
    if (y_scaled > scaled) scaled = y_scaled;
  }
  if (scaled == 0u) scaled = 1u;
  return scaled;
}

static void blend_marker_pixel(uint8_t* dst, const uint8_t rgba[4]) {
  uint32_t alpha = rgba[3];
  uint32_t inv = 255u - alpha;
  dst[0] = (uint8_t)((rgba[0] * alpha + dst[0] * inv + 127u) / 255u);
  dst[1] = (uint8_t)((rgba[1] * alpha + dst[1] * inv + 127u) / 255u);
  dst[2] = (uint8_t)((rgba[2] * alpha + dst[2] * inv + 127u) / 255u);
  dst[3] = 255u;
}

static void draw_marker_ring_at_preview(uint8_t* rgba, uint32_t preview_w, uint32_t preview_h,
                                        uint32_t center_x, uint32_t center_y,
                                        uint32_t radius, uint32_t line_width,
                                        const uint8_t marker_rgba[4]) {
  if (!rgba || !marker_rgba || preview_w == 0u || preview_h == 0u) return;
  if (radius == 0u) radius = 1u;
  if (line_width == 0u) line_width = 1u;
  if (line_width > radius) line_width = radius;

  {
    uint32_t inner_radius = (radius > line_width) ? (radius - line_width) : 0u;
    uint64_t outer2 = (uint64_t)radius * radius;
    uint64_t inner2 = (uint64_t)inner_radius * inner_radius;
    int32_t min_x = (int32_t)center_x - (int32_t)radius;
    int32_t max_x = (int32_t)center_x + (int32_t)radius;
    int32_t min_y = (int32_t)center_y - (int32_t)radius;
    int32_t max_y = (int32_t)center_y + (int32_t)radius;
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= (int32_t)preview_w) max_x = (int32_t)preview_w - 1;
    if (max_y >= (int32_t)preview_h) max_y = (int32_t)preview_h - 1;

    for (int32_t py = min_y; py <= max_y; py++) {
      int64_t dy = (int64_t)py - (int64_t)center_y;
      for (int32_t px = min_x; px <= max_x; px++) {
        int64_t dx = (int64_t)px - (int64_t)center_x;
        uint64_t dist2 = (uint64_t)(dx * dx + dy * dy);
        if (dist2 > outer2 || dist2 < inner2) continue;
        blend_marker_pixel(rgba + (((uint32_t)py * preview_w + (uint32_t)px) * 4u), marker_rgba);
      }
    }
  }
}

static void draw_marker_span_at_preview(uint8_t* rgba, uint32_t preview_w, uint32_t preview_h,
                                        uint32_t center_x, uint32_t py0_inclusive,
                                        uint32_t py1_exclusive, uint32_t radius,
                                        uint32_t line_width, const uint8_t marker_rgba[4]) {
  if (!rgba || !marker_rgba || preview_w == 0u || preview_h == 0u || py1_exclusive <= py0_inclusive) {
    return;
  }
  if (radius == 0u) radius = 1u;
  if (line_width == 0u) line_width = 1u;
  if (line_width > radius) line_width = radius;

  {
    uint32_t inner_radius = (line_width >= radius) ? 0u : (radius - line_width);
    uint64_t outer2 = (uint64_t)radius * radius;
    uint64_t inner2 = (uint64_t)inner_radius * inner_radius;
    uint32_t segment_y1 = py1_exclusive - 1u;
    int32_t min_x = (int32_t)center_x - (int32_t)radius;
    int32_t max_x = (int32_t)center_x + (int32_t)radius;
    int32_t min_y = (int32_t)py0_inclusive - (int32_t)radius;
    int32_t max_y = (int32_t)segment_y1 + (int32_t)radius;
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= (int32_t)preview_w) max_x = (int32_t)preview_w - 1;
    if (max_y >= (int32_t)preview_h) max_y = (int32_t)preview_h - 1;

    for (int32_t py = min_y; py <= max_y; py++) {
      int32_t nearest_y = py;
      if (nearest_y < (int32_t)py0_inclusive) nearest_y = (int32_t)py0_inclusive;
      if (nearest_y > (int32_t)segment_y1) nearest_y = (int32_t)segment_y1;
      {
        int64_t dy = (int64_t)py - (int64_t)nearest_y;
        for (int32_t px = min_x; px <= max_x; px++) {
          int64_t dx = (int64_t)px - (int64_t)center_x;
          uint64_t dist2 = (uint64_t)(dx * dx + dy * dy);
          if (dist2 > outer2) continue;
          if (inner_radius > 0u && dist2 < inner2) continue;
          blend_marker_pixel(rgba + (((uint32_t)py * preview_w + (uint32_t)px) * 4u), marker_rgba);
        }
      }
    }
  }
}

static void draw_marker_ring(uint8_t* rgba, uint32_t preview_w, uint32_t preview_h,
                             uint32_t world_w, uint32_t world_h,
                             int32_t world_x, int32_t world_y,
                             const MapMarkerEntry* marker) {
  if (!rgba || !marker || preview_w == 0u || preview_h == 0u ||
      world_w == 0u || world_h == 0u) {
    return;
  }

  {
    uint32_t center_x = clamp_preview_coord(world_x, world_w, preview_w);
    uint32_t center_y = clamp_preview_coord(world_y, world_h, preview_h);
    uint32_t radius = scale_marker_measure(marker->radius, world_w, world_h, preview_w, preview_h);
    uint32_t line_width = scale_marker_measure(marker->line_width, world_w, world_h, preview_w, preview_h);
    draw_marker_ring_at_preview(
        rgba, preview_w, preview_h, center_x, center_y, radius, line_width, marker->rgba);
  }
}

static const MapMarkerEntry* find_marker_by_id(const MapMarkerEntry* markers,
                                               uint32_t marker_count,
                                               int32_t id) {
  for (uint32_t i = 0u; i < marker_count; i++) {
    if (markers[i].id == id) return &markers[i];
  }
  return NULL;
}

static uint32_t draw_matching_chest_markers_preview(TxWorld* w, uint8_t* rgba,
                                                    uint32_t preview_w, uint32_t preview_h,
                                                    const MapMarkerEntry* chest_markers,
                                                    uint32_t chest_marker_count) {
  if (!w || !rgba || chest_marker_count == 0u ||
      w->pointer_count <= 2u || w->starts[2] >= w->ends[2]) {
    return 0u;
  }

  {
    const uint8_t* saved_file = w->file;
    uint32_t saved_len = w->file_len;
    uint32_t off = 0u;
    uint32_t end = 0u;
    uint32_t matched = 0u;

    if (w->section_overrides[2].active) {
      w->file = w->section_overrides[2].data;
      w->file_len = w->section_overrides[2].len;
      end = w->section_overrides[2].len;
    } else {
      off = w->starts[2];
      end = w->ends[2];
    }

    {
      int32_t chest_count = (int16_t)rd_u16le(w->file, w->file_len, &off);
      int32_t slots_per_chest = 0;
      if (w->version < 294u) slots_per_chest = (int16_t)rd_u16le(w->file, w->file_len, &off);
      if (chest_count < 0) chest_count = 0;

      for (int32_t chest_index = 0; chest_index < chest_count && off < end; chest_index++) {
        int32_t chest_x = rd_i32le(w->file, w->file_len, &off);
        int32_t chest_y = rd_i32le(w->file, w->file_len, &off);
        rd_skip_string_value(w->file, w->file_len, &off);
        int32_t max_items = w->version >= 294u ? rd_i32le(w->file, w->file_len, &off) : slots_per_chest;
        const MapMarkerEntry* matched_marker = NULL;
        if (max_items < 0) max_items = 0;

        for (int32_t item_index = 0; item_index < max_items && off < end; item_index++) {
          int16_t stack = (int16_t)rd_u16le(w->file, w->file_len, &off);
          if (stack != 0) {
            int32_t item_type = rd_i32le(w->file, w->file_len, &off);
            rd_u8(w->file, w->file_len, &off);
            if (!matched_marker) {
              matched_marker = find_marker_by_id(chest_markers, chest_marker_count, item_type);
            }
          }
        }

        if (matched_marker) {
          draw_marker_ring(rgba, preview_w, preview_h,
                           (uint32_t)w->maxTilesX, (uint32_t)w->maxTilesY,
                           chest_x, chest_y, matched_marker);
          matched++;
        }
      }
    }

    w->file = (uint8_t*)saved_file;
    w->file_len = saved_len;
    return matched;
  }
}

static uint32_t draw_matching_tile_markers_preview(TxWorld* w, uint8_t* rgba,
                                                   uint32_t preview_w, uint32_t preview_h,
                                                   const MapMarkerEntry* tile_markers,
                                                   uint32_t tile_marker_count) {
  if (!w || !rgba || tile_marker_count == 0u) return 0u;

  {
    uint8_t* saved_file = w->file;
    uint32_t saved_len = w->file_len;
    uint32_t off = 0u;
    uint32_t end = 0u;
    uint32_t matched = 0u;
    uint32_t width = (uint32_t)w->maxTilesX;
    uint32_t height = (uint32_t)w->maxTilesY;

    if (w->section_overrides[1].active) {
      w->file = w->section_overrides[1].data;
      w->file_len = w->section_overrides[1].len;
      end = w->section_overrides[1].len;
    } else {
      off = w->starts[1];
      end = w->ends[1];
    }

    for (uint32_t x = 0u; x < width; x++) {
      for (uint32_t y = 0u; y < height;) {
        TxTile t;
        if (!read_tile_at(w, &off, end, &t)) {
          w->file = saved_file;
          w->file_len = saved_len;
          return matched;
        }
        {
          uint32_t run = (uint32_t)t.same + 1u;
          if (t.active) {
            const MapMarkerEntry* matched_marker =
                find_marker_by_id(tile_markers, tile_marker_count, (int32_t)t.type);
            if (matched_marker) {
              uint32_t px = clamp_preview_coord((int32_t)x, width, preview_w);
              uint32_t py0 = (uint32_t)(((uint64_t)y * preview_h) / height);
              uint32_t py1 = (uint32_t)((((uint64_t)y + run) * preview_h) / height);
              if (py1 <= py0) py1 = py0 + 1u;
              if (py1 > preview_h) py1 = preview_h;
              if (UINT32_MAX - matched < run) matched = UINT32_MAX;
              else matched += run;
              draw_marker_span_at_preview(
                  rgba, preview_w, preview_h, px, py0, py1,
                  scale_marker_measure(matched_marker->radius, width, height, preview_w, preview_h),
                  scale_marker_measure(matched_marker->line_width, width, height, preview_w, preview_h),
                  matched_marker->rgba);
            }
          }
          y += run;
        }
      }
    }

    w->file = saved_file;
    w->file_len = saved_len;
    return matched;
  }
}

/* ====================================================================
 * Exported: render preview to RGBA buffer in WASM memory
 *
 * Returns byte count on success, -1 on error.
 * Result pointer available via tx_last_ptr/tx_last_len.
 * ==================================================================== */

int32_t txw_render_preview_rgba(TxWorld* w, uint32_t max_w, uint32_t max_h) {
  uint32_t pw = 0u;
  uint32_t ph = 0u;
  uint32_t stride = 0u;
  if (!compute_preview_size(w, max_w, max_h, &pw, &ph, &stride)) return -1;
  uint32_t rgba_len = stride * ph;
  uint8_t* rgba = tx_alloc(rgba_len);
  if (!rgba) { tx_set_error("TERRAX_WASM_OOM", "preview allocation failed"); return -1; }

  render_preview_to(w, rgba, pw, ph);

  tx_last_width  = pw;
  tx_last_height = ph;
  tx_last_stride = stride;
  return set_result_bytes(rgba, rgba_len);
}

/* ====================================================================
 * Exported: render preview as PNG in WASM memory
 *
 * Renders RGBA first, then encodes as PNG with fixed-Huffman zlib.
 * Returns byte count on success, -1 on error.
 * Result pointer available via tx_last_ptr/tx_last_len.
 * ==================================================================== */

int32_t txw_render_preview_png(TxWorld* w, uint32_t max_w, uint32_t max_h) {
  /* Render RGBA first */
  int32_t n = txw_render_preview_rgba(w, max_w, max_h);
  if (n < 0) return -1;

  uint8_t* rgba = (uint8_t*)(uintptr_t)tx_last_ptr;
  uint32_t pw = tx_last_width, ph = tx_last_height;
  return encode_png_from_owned_rgba(rgba, pw, ph);
}

int32_t txw_render_marked_preview_png(TxWorld* w, uint32_t max_w, uint32_t max_h,
                                      const MapMarkerEntry* chest_markers, uint32_t chest_count,
                                      const MapMarkerEntry* tile_markers, uint32_t tile_count,
                                      uint32_t* matched_chest_count,
                                      uint32_t* matched_tile_count) {
  uint32_t pw = 0u;
  uint32_t ph = 0u;
  uint32_t stride = 0u;
  uint32_t rgba_len = 0u;
  uint8_t* rgba = NULL;

  if (matched_chest_count) *matched_chest_count = 0u;
  if (matched_tile_count) *matched_tile_count = 0u;
  if (!compute_preview_size(w, max_w, max_h, &pw, &ph, &stride)) return -1;

  rgba_len = stride * ph;
  rgba = tx_alloc(rgba_len);
  if (!rgba) {
    tx_set_error("TERRAX_WASM_OOM", "preview allocation failed");
    return -1;
  }

  render_preview_to(w, rgba, pw, ph);
  if (chest_count > 0u && matched_chest_count) {
    *matched_chest_count = draw_matching_chest_markers_preview(
        w, rgba, pw, ph, chest_markers, chest_count);
  } else if (chest_count > 0u) {
    (void)draw_matching_chest_markers_preview(w, rgba, pw, ph, chest_markers, chest_count);
  }
  if (tile_count > 0u && matched_tile_count) {
    *matched_tile_count = draw_matching_tile_markers_preview(
        w, rgba, pw, ph, tile_markers, tile_count);
  } else if (tile_count > 0u) {
    (void)draw_matching_tile_markers_preview(w, rgba, pw, ph, tile_markers, tile_count);
  }

  return encode_png_from_owned_rgba(rgba, pw, ph);
}
