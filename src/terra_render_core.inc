/*
 * terra_render.c -- Rendering pipeline for TerraWasm.
 *
 * Pure C, no libc dependencies. Implements:
 *   - Color system (tile/wall/background)
 *   - Preview rendering (column-major tile scan)
 *   - CRC32 for PNG chunks
 *   - Fixed-Huffman zlib deflate with LZ77 hash-chain matching
 *   - PNG encoding (RGBA -> filter bytes -> zlib -> PNG chunks)
 */
#include "terra_types.h"
#include "terra_map.h"
#include "terra_icon.h"
#include "terra_color_data.h"
#include "terra_map_runtime.h"
#include <string.h>

/* ====================================================================
 * Extern declarations from terra_mem.c
 * ==================================================================== */

extern void  buf_init(TxBuf* b, uint32_t cap);
extern void  buf_u8(TxBuf* b, uint8_t v);
extern void  buf_bytes(TxBuf* b, const void* p, uint32_t n);
extern void  buf_u32be(TxBuf* b, uint32_t v);

extern uint8_t* tx_alloc(uint32_t size);
extern void tx_internal_free(void* ptr);

extern void  tx_set_error(const char* code, const char* message);
extern void  tx_clear_error(void);
extern int   set_result_buf(TxBuf* b);

extern const uint8_t* tx_get_tile_colors(void);
extern uint32_t       tx_get_tile_color_count(void);
extern const uint8_t* tx_get_wall_colors(void);
extern uint32_t       tx_get_wall_color_count(void);

extern uint32_t tx_last_ptr;
extern uint32_t tx_last_len;
extern uint32_t tx_last_width;
extern uint32_t tx_last_height;

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

/* Get paint RGB from TX_PAINT_COLORS table. Paint IDs are 1-30. */
static void get_paint_color(uint8_t paint_id, uint8_t* r, uint8_t* g, uint8_t* b) {
  if (tx_map_runtime_is_set()) {
    uint8_t rgb[3];
    if (tx_map_runtime_paint(paint_id, rgb)) {
      *r = rgb[0]; *g = rgb[1]; *b = rgb[2];
    } else { *r = 255; *g = 255; *b = 255; }
    return;
  }
  if (paint_id == 0 || paint_id > 30) { *r = 255; *g = 255; *b = 255; return; }
  uint32_t idx = (uint32_t)(paint_id - 1) * 3u;
  *r = TX_PAINT_COLORS[idx];
  *g = TX_PAINT_COLORS[idx + 1u];
  *b = TX_PAINT_COLORS[idx + 2u];
}

/*
 * Blend a base color with a paint color, matching Terraria 1.4.5.8 MapColor.
 *
 * paint_id 0 = no change
 * paint_id 29 (shadow): multiply paint color by min(b,max(r,g)) / 255 * 0.3
 * paint_id 30 (negative): invert for tiles, half-invert (50%) for walls
 * Other paints: multiply paint color by max(r,g,b) / 255
 */
static void blend_with_paint(uint8_t base_r, uint8_t base_g, uint8_t base_b,
                             uint8_t paint_id, int is_wall,
                             uint8_t* out_r, uint8_t* out_g, uint8_t* out_b) {
  if (paint_id == 0) { *out_r = base_r; *out_g = base_g; *out_b = base_b; return; }

  uint8_t pr, pg, pb;
  get_paint_color(paint_id, &pr, &pg, &pb);

  uint32_t oldR = (uint32_t)base_r;
  uint32_t oldG = (uint32_t)base_g;
  uint32_t oldB = (uint32_t)base_b;

  if (paint_id == 29) {
    /* MapColor swaps G with R, then B with R; B becomes min(B,max(R,G)). */
    uint32_t max_rg = oldR > oldG ? oldR : oldG;
    uint32_t shadow = oldB < max_rg ? oldB : max_rg;
    float ratio = ((float)shadow / 255.0f) * 0.3f;
    *out_r = (uint8_t)((float)pr * ratio);
    *out_g = (uint8_t)((float)pg * ratio);
    *out_b = (uint8_t)((float)pb * ratio);
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
    *out_r = (uint8_t)((255u - oldR) / 2u);
    *out_g = (uint8_t)((255u - oldG) / 2u);
    *out_b = (uint8_t)((255u - oldB) / 2u);
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

static void background_color(uint32_t y, uint32_t world_h, double ground, double rock, uint8_t out[4]) {
  const TxMapRuntimeLayout* layout = tx_map_runtime_layout();
  if (!layout) {
    ground = (uint32_t)ground;
    rock = (uint32_t)rock;
  }
  if (ground == 0u) ground = world_h > 3u ? (world_h * 35u) / 100u : 1u;
  if (rock == 0u) rock = world_h > 2u ? (world_h * 65u) / 100u : ground + 1u;
  if (ground == 0u) ground = 1u;
  if (rock <= ground) rock = ground + 1u;
  if (layout) {
    uint32_t index;
    if (y < ground) {
      index = layout->sky_pos + (uint32_t)(255.0 * ((double)y / ground));
    } else if (y < rock) {
      /* These variants represent scene snowiness, not vertical depth. The
       * offline MAP and PNG renderer use the game's no-snow background. */
      index = layout->dirt_pos;
    } else if (y + 200u < world_h) {
      index = layout->rock_pos;
    } else index = layout->hell_pos;
    tx_map_runtime_color(index, out);
    return;
  }
  if (y < ground) { lerp_rgb(out, 50, 40, 255, 145, 185, 255, y, ground); return; }
  if (y < rock) { lerp_rgb(out, 88, 61, 46, 37, 78, 123, y - ground, rock - ground); return; }
  if (y + 200u < world_h) { lerp_rgb(out, 74, 67, 60, 53, 70, 97, y - rock, (world_h - 200u > rock) ? (world_h - 200u - rock) : 1u); return; }
  out[0] = 50; out[1] = 44; out[2] = 38; out[3] = 255u;
}

static int tile_is_non_empty(const TxTile* t) {
  const TxMapRuntimeLayout* layout = tx_map_runtime_layout();
  if (layout) {
    uint16_t base;
    uint8_t options;
    if (t->active && !t->invisible_block &&
        tx_map_runtime_lookup(0, t->type, &base, &options)) return 1;
    if (t->liquid_amount && t->liquid_type &&
        t->liquid_type <= layout->sky_pos - layout->liquid_pos) return 1;
    if (t->wall && !t->invisible_wall &&
        tx_map_runtime_lookup(1, t->wall, &base, &options)) return 1;
    return 0;
  }
  if (t->active && !t->invisible_block) return 1;
  if (t->liquid_amount && t->liquid_type) return 1;
  if (t->wall && !t->invisible_wall) return 1;
  return 0;
}

int tx_render_has_foreground(const TxTile* tile) {
  return tile && tile_is_non_empty(tile);
}

static void color_for_tile(const TxTile* t, uint32_t y, uint32_t world_h, double ground, double rock, uint8_t out[4]) {
  const TxMapRuntimeLayout* layout = tx_map_runtime_layout();
  if (layout) {
    uint16_t base;
    uint8_t count;
    if (t->active && !t->invisible_block &&
        tx_map_runtime_lookup(0, t->type, &base, &count)) {
      uint32_t variant = get_tile_variant_index(t->type, t->frame_x, t->frame_y);
      if (variant >= count) variant = 0u;
      tx_map_runtime_color((uint32_t)base + variant, out);
      if (t->tile_color) blend_with_paint(out[0], out[1], out[2], t->tile_color, 0,
                                           &out[0], &out[1], &out[2]);
      return;
    }
    if (t->liquid_amount && t->liquid_type && t->liquid_type <= layout->sky_pos - layout->liquid_pos) {
      tx_map_runtime_color(layout->liquid_pos + t->liquid_type - 1u, out);
      return;
    }
    if (t->wall && !t->invisible_wall &&
        tx_map_runtime_lookup(1, t->wall, &base, &count)) {
      tx_map_runtime_color(base, out);
      if (t->wall_color) blend_with_paint(out[0], out[1], out[2], t->wall_color, 1,
                                           &out[0], &out[1], &out[2]);
      return;
    }
    background_color(y, world_h, ground, rock, out);
    return;
  }
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

#ifdef TERRAX_TESTING
void txw_test_map_runtime_render_color(const TxTile* tile, uint32_t y,
                                       uint32_t height, double ground, double rock,
                                       uint8_t rgba[4]) {
  color_for_tile(tile, y, height, ground, rock, rgba);
}
#endif

/* ====================================================================
 * Adler-32 checksum
 * ==================================================================== */

static uint32_t adler32_bytes(const uint8_t* data, uint32_t length) {
  extern unsigned long adler32(unsigned long, const uint8_t*, unsigned int);
  return (uint32_t)adler32(1u, data, length);
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

static void write_fixed_block(TxBuf* out, uint32_t* bitbuf, uint32_t* bitcnt,
                              const uint8_t* data, uint32_t len, uint32_t final_block) {
  for (uint32_t i = 0; i < 32768u; i++) z_head[i] = -1;
  bw_bit(out, bitbuf, bitcnt, final_block ? 1u : 0u, 1u);
  bw_bit(out, bitbuf, bitcnt, 1u, 2u);
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
      fixed_match(out, bitbuf, bitcnt, best_len, best_dist);
      uint32_t end = pos + best_len;
      if (end > len - 2u) end = len - 2u;
      for (uint32_t p = pos + 1u; p < end; p += 16u)
        z_head[fast_hash3(data, p)] = (int32_t)p;
      pos += best_len;
    } else {
      fixed_literal(out, bitbuf, bitcnt, data[pos]);
      pos++;
    }
  }
  /* end-of-block symbol */
  fixed_literal(out, bitbuf, bitcnt, 256u);
}

static void write_zlib_fixed(TxBuf* out, const uint8_t* data, uint32_t len) {
  if (!len) {
    static const uint8_t empty[7] = {0x78,0x01,0x03,0x00,0x00,0x00,0x01};
    buf_bytes(out, empty, 7u);
    return;
  }
  /* zlib header: CMF=0x78 (deflate, window=32k), FLG=0x01 (fastest) */
  buf_u8(out, 0x78); buf_u8(out, 0x01);
  uint32_t bitbuf = 0u, bitcnt = 0u;
  write_fixed_block(out, &bitbuf, &bitcnt, data, len, 1u);
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

static void render_preview_rows_to(TxWorld* w, uint8_t* rgba, uint32_t pw, uint32_t ph,
                                   uint32_t row_start, uint32_t row_count) {
  if (tx_output_copy_rows(w, rgba, pw, ph, row_start, row_count)) return;
  double ground = w->worldSurface;
  double rock = w->rockLayer;
  uint32_t row_end = row_start + row_count;
  if (row_start >= ph || row_count == 0u) return;
  if (row_end < row_start || row_end > ph) row_end = ph;
  row_count = row_end - row_start;

  /* Fill background colors row by row. Alpha temporarily stores the number
     of non-empty source samples accumulated into each preview pixel. */
  for (uint32_t local_py = 0u; local_py < row_count; local_py++) {
    uint32_t py = row_start + local_py;
    uint8_t bg[4];
    uint32_t wy = (uint32_t)(((uint64_t)py * (uint64_t)w->maxTilesY) / ph);
    background_color(wy, (uint32_t)w->maxTilesY, ground, rock, bg);
    for (uint32_t px = 0u; px < pw; px++) {
      uint32_t o = (local_py * pw + px) * 4u;
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
      if (px < pw && py0 < row_end && py1 > row_start) {
        uint32_t local_py0 = py0 > row_start ? py0 - row_start : 0u;
        uint32_t local_py1 = py1 < row_end ? py1 - row_start : row_count;
        if (py1 > ph) py1 = ph;
        for (uint32_t local_py = local_py0; local_py < local_py1; local_py++) {
          uint32_t o = (local_py * pw + px) * 4u;
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
  for (uint32_t p = 0u; p < pw * row_count; p++) {
    rgba[p * 4u + 3u] = 255u;
  }
}

static void render_preview_to(TxWorld* w, uint8_t* rgba, uint32_t pw, uint32_t ph) {
  render_preview_rows_to(w, rgba, pw, ph, 0u, ph);
}

/*
 * Full-size marked previews use one compact RGB surface. At native dimensions
 * every preview pixel maps to exactly one world tile, so the downsampling
 * counters used by render_preview_rows_to are unnecessary. Keeping RGB rather
 * than RGBA saves 25% of the large surface while preserving the final PNG's
 * visual data. The world tile stream is decoded exactly once.
 */
static int render_full_preview_rgb_to(TxWorld* w, uint8_t* rgb,
                                      uint32_t pw, uint32_t ph) {
  uint32_t width;
  uint32_t height;
  double ground;
  double rock;
  uint8_t* saved_file;
  uint32_t saved_len;
  uint32_t off;
  uint32_t end;

  if (!w || !rgb || w->maxTilesX <= 0 || w->maxTilesY <= 0) return 0;
  width = (uint32_t)w->maxTilesX;
  height = (uint32_t)w->maxTilesY;
  if (pw != width || ph != height) return 0;
  ground = w->worldSurface;
  rock = w->rockLayer;

  for (uint32_t y = 0u; y < height; y++) {
    uint8_t bg[4];
    background_color(y, height, ground, rock, bg);
    for (uint32_t x = 0u; x < width; x++) {
      uint8_t* dst = rgb + (((uint64_t)y * width + x) * 3u);
      dst[0] = bg[0];
      dst[1] = bg[1];
      dst[2] = bg[2];
    }
  }

  saved_file = w->file;
  saved_len = w->file_len;
  if (w->section_overrides[1].active) {
    w->file = w->section_overrides[1].data;
    w->file_len = w->section_overrides[1].len;
    off = 0u;
    end = w->section_overrides[1].len;
  } else {
    off = w->starts[1];
    end = w->ends[1];
  }

  for (uint32_t x = 0u; x < width; x++) {
    for (uint32_t y = 0u; y < height;) {
      TxTile tile;
      if (!read_tile_at(w, &off, end, &tile)) {
        w->file = saved_file;
        w->file_len = saved_len;
        return 0;
      }

      uint32_t run = (uint32_t)tile.same + 1u;
      uint32_t y_end = y + run;
      if (y_end < y || y_end > height) y_end = height;
      if (tile_is_non_empty(&tile)) {
        uint8_t color[4];
        color_for_tile(&tile, y, height, ground, rock, color);
        for (uint32_t yy = y; yy < y_end; yy++) {
          uint8_t* dst = rgb + (((uint64_t)yy * width + x) * 3u);
          dst[0] = color[0];
          dst[1] = color[1];
          dst[2] = color[2];
        }
      }
      y = y_end;
    }
  }

  w->file = saved_file;
  w->file_len = saved_len;
  return 1;
}

#ifdef TERRAX_TESTING
int txw_test_render_full_preview_rgb_to(TxWorld* world, uint8_t* rgb,
                                         uint32_t width, uint32_t height) {
  return render_full_preview_rgb_to(world, rgb, width, height);
}
void txw_test_render_preview_rows_to(TxWorld* world, uint8_t* rgba,
                                      uint32_t width, uint32_t height) {
  render_preview_rows_to(world, rgba, width, height, 0u, height);
}
#endif

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
  if (alpha == 255u) {
    dst[0] = rgba[0]; dst[1] = rgba[1]; dst[2] = rgba[2]; dst[3] = 255u;
    return;
  }
  if (alpha == 0u) { dst[3] = 255u; return; }
  uint32_t inv = 255u - alpha;
  dst[0] = (uint8_t)((rgba[0] * alpha + dst[0] * inv + 127u) / 255u);
  dst[1] = (uint8_t)((rgba[1] * alpha + dst[1] * inv + 127u) / 255u);
  dst[2] = (uint8_t)((rgba[2] * alpha + dst[2] * inv + 127u) / 255u);
  dst[3] = 255u;
}

static void draw_marker_ring_at_preview_rows(uint8_t* rgba, uint32_t preview_w, uint32_t preview_h,
                                             uint32_t row_start, uint32_t row_count,
                                             uint32_t center_x, uint32_t center_y,
                                             uint32_t radius, uint32_t line_width,
                                             const uint8_t marker_rgba[4]) {
  if (!rgba || !marker_rgba || preview_w == 0u || preview_h == 0u ||
      row_start >= preview_h || row_count == 0u) return;
  uint32_t row_end = row_start + row_count;
  if (row_end < row_start || row_end > preview_h) row_end = preview_h;
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
    if (min_y < (int32_t)row_start) min_y = (int32_t)row_start;
    if (max_x >= (int32_t)preview_w) max_x = (int32_t)preview_w - 1;
    if (max_y >= (int32_t)row_end) max_y = (int32_t)row_end - 1;

    for (int32_t py = min_y; py <= max_y; py++) {
      int64_t dy = (int64_t)py - (int64_t)center_y;
      for (int32_t px = min_x; px <= max_x; px++) {
        int64_t dx = (int64_t)px - (int64_t)center_x;
        uint64_t dist2 = (uint64_t)(dx * dx + dy * dy);
        if (dist2 > outer2 || dist2 < inner2) continue;
        blend_marker_pixel(
            rgba + ((((uint32_t)py - row_start) * preview_w + (uint32_t)px) * 4u),
            marker_rgba);
      }
    }
  }
}

static void draw_marker_ring_at_preview(uint8_t* rgba, uint32_t preview_w, uint32_t preview_h,
                                        uint32_t center_x, uint32_t center_y,
                                        uint32_t radius, uint32_t line_width,
                                        const uint8_t marker_rgba[4]) {
  draw_marker_ring_at_preview_rows(
      rgba, preview_w, preview_h, 0u, preview_h,
      center_x, center_y, radius, line_width, marker_rgba);
}

static void draw_marker_span_at_preview_rows(uint8_t* rgba, uint32_t preview_w, uint32_t preview_h,
                                             uint32_t row_start, uint32_t row_count,
                                             uint32_t center_x, uint32_t py0_inclusive,
                                             uint32_t py1_exclusive, uint32_t radius,
                                             uint32_t line_width, const uint8_t marker_rgba[4]) {
  if (!rgba || !marker_rgba || preview_w == 0u || preview_h == 0u ||
      row_start >= preview_h || row_count == 0u || py1_exclusive <= py0_inclusive) {
    return;
  }
  uint32_t row_end = row_start + row_count;
  if (row_end < row_start || row_end > preview_h) row_end = preview_h;
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
    if (min_y < (int32_t)row_start) min_y = (int32_t)row_start;
    if (max_x >= (int32_t)preview_w) max_x = (int32_t)preview_w - 1;
    if (max_y >= (int32_t)row_end) max_y = (int32_t)row_end - 1;

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
          blend_marker_pixel(
              rgba + ((((uint32_t)py - row_start) * preview_w + (uint32_t)px) * 4u),
              marker_rgba);
        }
      }
    }
  }
}

static void draw_marker_span_at_preview(uint8_t* rgba, uint32_t preview_w, uint32_t preview_h,
                                        uint32_t center_x, uint32_t py0_inclusive,
                                        uint32_t py1_exclusive, uint32_t radius,
                                        uint32_t line_width, const uint8_t marker_rgba[4]) {
  draw_marker_span_at_preview_rows(
      rgba, preview_w, preview_h, 0u, preview_h,
      center_x, py0_inclusive, py1_exclusive, radius, line_width, marker_rgba);
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

static void draw_marker_ring_rows(uint8_t* rgba, uint32_t preview_w, uint32_t preview_h,
                                  uint32_t row_start, uint32_t row_count,
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
    draw_marker_ring_at_preview_rows(
        rgba, preview_w, preview_h, row_start, row_count,
        center_x, center_y, radius, line_width, marker->rgba);
  }
}

static void draw_marker_icon_rows(TxWorld* w, uint8_t* rgba,
                                  uint32_t preview_w, uint32_t preview_h,
                                  uint32_t row_start, uint32_t row_count,
                                  uint32_t world_w, uint32_t world_h,
                                  int32_t world_x, int32_t world_y,
                                  int32_t item_id, const MapMarkerEntry* marker) {
  const TxIconAtlas* atlas;
  int icon_index;
  uint32_t center_x;
  uint32_t center_y;
  uint32_t radius;
  uint32_t line_width;
  uint32_t inner_radius;
  uint32_t side;
  uint32_t icon_size;
  uint32_t source_x0;
  uint32_t source_y0;
  uint32_t row_end;
  int32_t start_x;
  int32_t start_y;

  if (!w || !rgba || !marker || !world_w || !world_h ||
      !preview_w || !preview_h || row_start >= preview_h || !row_count) return;
  center_x = clamp_preview_coord(world_x, world_w, preview_w);
  center_y = clamp_preview_coord(world_y, world_h, preview_h);
  radius = scale_marker_measure(marker->radius, world_w, world_h, preview_w, preview_h);
  line_width = scale_marker_measure(marker->line_width, world_w, world_h, preview_w, preview_h);
  inner_radius = radius > line_width ? radius - line_width : radius;
  side = terra_icon_side_for_radius(inner_radius);
  if (!side) return;

  start_x = (int32_t)center_x - (int32_t)(side / 2u);
  start_y = (int32_t)center_y - (int32_t)(side / 2u);
  row_end = row_start + row_count;
  if (row_end < row_start || row_end > preview_h) row_end = preview_h;
  int32_t first_y = (int32_t)row_start - start_y;
  int32_t last_y = (int32_t)row_end - start_y;
  int32_t first_x = -start_x;
  int32_t last_x = (int32_t)preview_w - start_x;
  if (first_y < 0) first_y = 0;
  if (last_y > (int32_t)side) last_y = (int32_t)side;
  if (first_x < 0) first_x = 0;
  if (last_x > (int32_t)side) last_x = (int32_t)side;
  if (first_y >= last_y || first_x >= last_x) return;
  // Most entity icons are outside this streaming strip. Reject them before
  // walking the atlas ID table or testing every off-strip source row.
  atlas = &w->icon_atlas;
  icon_index = terra_icon_index_for_item(atlas, item_id);
  if (icon_index < 0) return;
  icon_size = atlas->icon_size;
  source_x0 = atlas->x_offsets[icon_index];
  source_y0 = atlas->y_offsets[icon_index];

  for (uint32_t local_y = (uint32_t)first_y; local_y < (uint32_t)last_y; local_y++) {
    int32_t py = start_y + (int32_t)local_y;
    {
      uint32_t source_y = (local_y * icon_size) / side;
      for (uint32_t local_x = (uint32_t)first_x; local_x < (uint32_t)last_x; local_x++) {
        int32_t px = start_x + (int32_t)local_x;
        {
          uint32_t source_x = (local_x * icon_size) / side;
          const uint8_t* source = (const uint8_t*)(uintptr_t)atlas->rgba +
              ((source_y0 + source_y) * atlas->atlas_width + source_x0 + source_x) * 4u;
          if (!source[3]) continue;
          blend_marker_pixel(
              rgba + ((((uint32_t)py - row_start) * preview_w + (uint32_t)px) * 4u),
              source);
        }
      }
    }
  }
}

static void blend_marker_pixel_rgb(uint8_t* dst, const uint8_t rgba[4]) {
  uint32_t alpha = rgba[3];
  if (alpha == 255u) {
    dst[0] = rgba[0]; dst[1] = rgba[1]; dst[2] = rgba[2];
    return;
  }
  if (alpha == 0u) return;
  uint32_t inv = 255u - alpha;
  dst[0] = (uint8_t)((rgba[0] * alpha + dst[0] * inv + 127u) / 255u);
  dst[1] = (uint8_t)((rgba[1] * alpha + dst[1] * inv + 127u) / 255u);
  dst[2] = (uint8_t)((rgba[2] * alpha + dst[2] * inv + 127u) / 255u);
}

static void draw_marker_ring_at_rgb(uint8_t* rgb, uint32_t preview_w,
                                    uint32_t preview_h, uint32_t center_x,
                                    uint32_t center_y, uint32_t radius,
                                    uint32_t line_width,
                                    const uint8_t marker_rgba[4]) {
  if (!rgb || !marker_rgba || preview_w == 0u || preview_h == 0u) return;
  if (radius == 0u) radius = 1u;
  if (line_width == 0u) line_width = 1u;
  if (line_width > radius) line_width = radius;

  {
    uint32_t inner_radius = radius > line_width ? radius - line_width : 0u;
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
        uint64_t distance = (uint64_t)(dx * dx + dy * dy);
        if (distance > outer2 || distance < inner2) continue;
        blend_marker_pixel_rgb(
            rgb + (((uint64_t)(uint32_t)py * preview_w + (uint32_t)px) * 3u),
            marker_rgba);
      }
    }
  }
}

static void draw_marker_span_at_rgb(uint8_t* rgb, uint32_t preview_w,
                                    uint32_t preview_h, uint32_t center_x,
                                    uint32_t py0_inclusive, uint32_t py1_exclusive,
                                    uint32_t radius, uint32_t line_width,
                                    const uint8_t marker_rgba[4]) {
  if (!rgb || !marker_rgba || preview_w == 0u || preview_h == 0u ||
      py1_exclusive <= py0_inclusive) return;
  if (radius == 0u) radius = 1u;
  if (line_width == 0u) line_width = 1u;
  if (line_width > radius) line_width = radius;

  {
    uint32_t inner_radius = line_width >= radius ? 0u : radius - line_width;
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
          uint64_t distance = (uint64_t)(dx * dx + dy * dy);
          if (distance > outer2 || (inner_radius > 0u && distance < inner2)) continue;
          blend_marker_pixel_rgb(
              rgb + (((uint64_t)(uint32_t)py * preview_w + (uint32_t)px) * 3u),
              marker_rgba);
        }
      }
    }
  }
}

static void draw_marker_icon_rgb(TxWorld* w, uint8_t* rgb,
                                 uint32_t preview_w, uint32_t preview_h,
                                 uint32_t world_w, uint32_t world_h,
                                 int32_t world_x, int32_t world_y,
                                 int32_t item_id, const MapMarkerEntry* marker) {
  const TxIconAtlas* atlas;
  int icon_index;
  uint32_t center_x;
  uint32_t center_y;
  uint32_t radius;
  uint32_t line_width;
  uint32_t inner_radius;
  uint32_t side;
  uint32_t icon_size;
  uint32_t source_x0;
  uint32_t source_y0;
  int32_t start_x;
  int32_t start_y;

  if (!w || !rgb || !marker || !world_w || !world_h || !preview_w || !preview_h) return;
  atlas = &w->icon_atlas;
  icon_index = terra_icon_index_for_item(atlas, item_id);
  if (icon_index < 0) return;

  center_x = clamp_preview_coord(world_x, world_w, preview_w);
  center_y = clamp_preview_coord(world_y, world_h, preview_h);
  radius = scale_marker_measure(marker->radius, world_w, world_h, preview_w, preview_h);
  line_width = scale_marker_measure(marker->line_width, world_w, world_h, preview_w, preview_h);
  inner_radius = radius > line_width ? radius - line_width : radius;
  side = terra_icon_side_for_radius(inner_radius);
  if (!side) return;

  icon_size = atlas->icon_size;
  source_x0 = atlas->x_offsets[icon_index];
  source_y0 = atlas->y_offsets[icon_index];
  start_x = (int32_t)center_x - (int32_t)(side / 2u);
  start_y = (int32_t)center_y - (int32_t)(side / 2u);
  for (uint32_t local_y = 0u; local_y < side; local_y++) {
    int32_t py = start_y + (int32_t)local_y;
    if (py < 0 || py >= (int32_t)preview_h) continue;
    {
      uint32_t source_y = (local_y * icon_size) / side;
      for (uint32_t local_x = 0u; local_x < side; local_x++) {
        int32_t px = start_x + (int32_t)local_x;
        if (px < 0 || px >= (int32_t)preview_w) continue;
        {
          uint32_t source_x = (local_x * icon_size) / side;
          const uint8_t* source = (const uint8_t*)(uintptr_t)atlas->rgba +
              ((source_y0 + source_y) * atlas->atlas_width + source_x0 + source_x) * 4u;
          if (!source[3]) continue;
          blend_marker_pixel_rgb(
              rgb + (((uint64_t)(uint32_t)py * preview_w + (uint32_t)px) * 3u),
              source);
        }
      }
    }
  }
}

static const MapMarkerEntry* find_marker_by_id(const MapMarkerEntry* markers,
                                               uint32_t marker_count,
                                               int32_t id) {
  for (uint32_t i = 0u; i < marker_count; i++) {
    if (!markers[i].locate && markers[i].id == id) return &markers[i];
  }
  return NULL;
}

static void draw_entity_points_rows(TxWorld* w, uint8_t* rgba, uint32_t pw, uint32_t ph,
                                    uint32_t row_start, uint32_t row_count,
                                    const MapMarkerEntry* markers, const TxBuf* points) {
  if (!points || !row_count || row_start >= ph) return;
  uint32_t row_end = row_start + row_count;
  if (row_end < row_start || row_end > ph) row_end = ph;
  int full_size = pw == (uint32_t)w->maxTilesX && ph == (uint32_t)w->maxTilesY;
  const TxMarkerPoint* entries = (const TxMarkerPoint*)points->data;
  for (uint32_t i = 0; i < points->len / sizeof(TxMarkerPoint); i++) {
    const TxMarkerPoint* point = &entries[i];
    const MapMarkerEntry* marker = &markers[point->marker_index];
    uint32_t center_y = full_size
        ? (point->y <= 0 ? 0u : (uint32_t)point->y >= ph ? ph - 1u : (uint32_t)point->y)
        : clamp_preview_coord(point->y, w->maxTilesY, ph);
    uint32_t radius = full_size ? marker->radius
        : scale_marker_measure(marker->radius, w->maxTilesX, w->maxTilesY, pw, ph);
    if (!radius) radius = 1u;
    // Ring and centered square icon both fit inside this vertical bound. Keep
    // the original point order for overlaps and partially transparent colors.
    if ((uint64_t)center_y + radius < row_start || (int64_t)center_y - radius >= row_end) continue;
    draw_marker_ring_rows(rgba, pw, ph, row_start, row_count,
        w->maxTilesX, w->maxTilesY, point->x, point->y, &markers[point->marker_index]);
    draw_marker_icon_rows(w, rgba, pw, ph, row_start, row_count,
        w->maxTilesX, w->maxTilesY, point->x, point->y,
        markers[point->marker_index].icon_id, &markers[point->marker_index]);
  }
}

#ifdef TERRAX_TESTING
void txw_test_render_entity_rows(TxWorld* w, uint8_t* rgba, uint32_t width, uint32_t height,
                                 uint32_t start, uint32_t count,
                                 const MapMarkerEntry* markers, const TxBuf* points) {
  draw_entity_points_rows(w, rgba, width, height, start, count, markers, points);
}
#endif

static void draw_entity_points_rgb(TxWorld* w, uint8_t* rgb, uint32_t pw, uint32_t ph,
                                   const MapMarkerEntry* markers, const TxBuf* points) {
  if (!points) return;
  const TxMarkerPoint* entries = (const TxMarkerPoint*)points->data;
  for (uint32_t i = 0; i < points->len / sizeof(TxMarkerPoint); i++) {
    const TxMarkerPoint* point = &entries[i];
    const MapMarkerEntry* marker = &markers[point->marker_index];
    draw_marker_ring_at_rgb(rgb, pw, ph,
        clamp_preview_coord(point->x, w->maxTilesX, pw),
        clamp_preview_coord(point->y, w->maxTilesY, ph),
        scale_marker_measure(marker->radius, w->maxTilesX, w->maxTilesY, pw, ph),
        scale_marker_measure(marker->line_width, w->maxTilesX, w->maxTilesY, pw, ph), marker->rgba);
    draw_marker_icon_rgb(w, rgb, pw, ph, w->maxTilesX, w->maxTilesY,
        point->x, point->y, marker->icon_id, marker);
  }
}

typedef void (*TxChestMarkerVisitor)(TxWorld*, int32_t, int32_t, int32_t,
                                     const MapMarkerEntry*, void*);

/* Visit exactly the first matching occupied slot in each chest. Both the
 * buffered renderer and the optional streaming cache share this decoder. */
static uint32_t visit_matching_chest_markers(TxWorld* w,
                                            const MapMarkerEntry* chest_markers,
                                            uint32_t chest_marker_count,
                                            TxChestMarkerVisitor visit, void* context) {
  if (!w || chest_marker_count == 0u ||
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
        int32_t matched_item_id = -1;
        if (max_items < 0) max_items = 0;

        for (int32_t item_index = 0; item_index < max_items && off < end; item_index++) {
          int16_t stack = (int16_t)rd_u16le(w->file, w->file_len, &off);
          if (stack != 0) {
            int32_t item_type = rd_i32le(w->file, w->file_len, &off);
            rd_u8(w->file, w->file_len, &off);
            if (!matched_marker) {
              matched_marker = find_marker_by_id(chest_markers, chest_marker_count, item_type);
              if (matched_marker) matched_item_id = item_type;
            }
          }
        }

        if (matched_marker) {
          visit(w, chest_x, chest_y, matched_item_id, matched_marker, context);
          matched++;
        }
      }
    }

    w->file = (uint8_t*)saved_file;
    w->file_len = saved_len;
    return matched;
  }
}

typedef struct TxChestMarkerRows {
  uint8_t* rgba;
  uint32_t width, height, start, count;
} TxChestMarkerRows;

static void draw_chest_marker_rows(TxWorld* w, int32_t x, int32_t y, int32_t item_id,
                                    const MapMarkerEntry* marker, void* context) {
  TxChestMarkerRows* rows = (TxChestMarkerRows*)context;
  draw_marker_ring_rows(rows->rgba, rows->width, rows->height, rows->start, rows->count,
      (uint32_t)w->maxTilesX, (uint32_t)w->maxTilesY, x, y, marker);
  draw_marker_icon_rows(w, rows->rgba, rows->width, rows->height, rows->start, rows->count,
      (uint32_t)w->maxTilesX, (uint32_t)w->maxTilesY, x, y, item_id, marker);
}

static uint32_t draw_matching_chest_markers_preview_rows(TxWorld* w, uint8_t* rgba,
                                                         uint32_t preview_w, uint32_t preview_h,
                                                         uint32_t row_start, uint32_t row_count,
                                                         const MapMarkerEntry* chest_markers,
                                                         uint32_t chest_marker_count,
                                                         uint32_t count_matches) {
  if (!rgba) return 0u;
  TxChestMarkerRows rows = {rgba, preview_w, preview_h, row_start, row_count};
  uint32_t matched = visit_matching_chest_markers(
      w, chest_markers, chest_marker_count, draw_chest_marker_rows, &rows);
  return count_matches ? matched : 0u;
}

static uint32_t draw_matching_chest_markers_preview(TxWorld* w, uint8_t* rgba,
                                                    uint32_t preview_w, uint32_t preview_h,
                                                    const MapMarkerEntry* chest_markers,
                                                    uint32_t chest_marker_count) {
  return draw_matching_chest_markers_preview_rows(
      w, rgba, preview_w, preview_h, 0u, preview_h,
      chest_markers, chest_marker_count, 1u);
}

static uint32_t draw_matching_tile_markers_preview_rows(TxWorld* w, uint8_t* rgba,
                                                        uint32_t preview_w, uint32_t preview_h,
                                                        uint32_t row_start, uint32_t row_count,
                                                        const MapMarkerEntry* tile_markers,
                                                        uint32_t tile_marker_count,
                                                        uint32_t count_matches) {
  if (!w || !rgba || tile_marker_count == 0u) return 0u;
  uint32_t legacy = 0u;
  for (uint32_t i = 0; i < tile_marker_count; i++) legacy |= !tile_markers[i].locate;
  if (!legacy) return 0u;

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
              if (count_matches) {
                if (UINT32_MAX - matched < run) matched = UINT32_MAX;
                else matched += run;
              }
              draw_marker_span_at_preview_rows(
                  rgba, preview_w, preview_h, row_start, row_count,
                  px, py0, py1,
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

static uint32_t draw_matching_tile_markers_preview(TxWorld* w, uint8_t* rgba,
                                                   uint32_t preview_w, uint32_t preview_h,
                                                   const MapMarkerEntry* tile_markers,
                                                   uint32_t tile_marker_count) {
  return draw_matching_tile_markers_preview_rows(
      w, rgba, preview_w, preview_h, 0u, preview_h,
      tile_markers, tile_marker_count, 1u);
}

static uint32_t draw_matching_chest_markers_rgb(
    TxWorld* w, uint8_t* rgb, uint32_t preview_w, uint32_t preview_h,
    const MapMarkerEntry* chest_markers, uint32_t chest_marker_count) {
  if (!w || !rgb || chest_marker_count == 0u ||
      w->pointer_count <= 2u || w->starts[2] >= w->ends[2]) return 0u;

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
      if (w->version < 294u) slots_per_chest = (int16_t)rd_u16le(
          w->file, w->file_len, &off);
      if (chest_count < 0) chest_count = 0;

      for (int32_t chest_index = 0;
           chest_index < chest_count && off < end; chest_index++) {
        int32_t chest_x = rd_i32le(w->file, w->file_len, &off);
        int32_t chest_y = rd_i32le(w->file, w->file_len, &off);
        rd_skip_string_value(w->file, w->file_len, &off);
        int32_t max_items = w->version >= 294u
            ? rd_i32le(w->file, w->file_len, &off)
            : slots_per_chest;
        const MapMarkerEntry* matched_marker = NULL;
        int32_t matched_item_id = -1;
        if (max_items < 0) max_items = 0;

        for (int32_t item_index = 0;
             item_index < max_items && off < end; item_index++) {
          int16_t stack = (int16_t)rd_u16le(w->file, w->file_len, &off);
          if (stack != 0) {
            int32_t item_type = rd_i32le(w->file, w->file_len, &off);
            rd_u8(w->file, w->file_len, &off);
            if (!matched_marker) {
              matched_marker = find_marker_by_id(
                  chest_markers, chest_marker_count, item_type);
              if (matched_marker) matched_item_id = item_type;
            }
          }
        }

        if (matched_marker) {
          uint32_t center_x = clamp_preview_coord(
              chest_x, (uint32_t)w->maxTilesX, preview_w);
          uint32_t center_y = clamp_preview_coord(
              chest_y, (uint32_t)w->maxTilesY, preview_h);
          uint32_t radius = scale_marker_measure(
              matched_marker->radius, (uint32_t)w->maxTilesX,
              (uint32_t)w->maxTilesY, preview_w, preview_h);
          uint32_t line_width = scale_marker_measure(
              matched_marker->line_width, (uint32_t)w->maxTilesX,
              (uint32_t)w->maxTilesY, preview_w, preview_h);
          draw_marker_ring_at_rgb(
              rgb, preview_w, preview_h, center_x, center_y,
              radius, line_width, matched_marker->rgba);
          draw_marker_icon_rgb(
              w, rgb, preview_w, preview_h,
              (uint32_t)w->maxTilesX, (uint32_t)w->maxTilesY,
              chest_x, chest_y, matched_item_id, matched_marker);
          matched++;
        }
      }
    }

    w->file = (uint8_t*)saved_file;
    w->file_len = saved_len;
    return matched;
  }
}

static uint32_t draw_matching_tile_markers_rgb(
    TxWorld* w, uint8_t* rgb, uint32_t preview_w, uint32_t preview_h,
    const MapMarkerEntry* tile_markers, uint32_t tile_marker_count) {
  if (!w || !rgb || tile_marker_count == 0u) return 0u;
  uint32_t legacy = 0u;
  for (uint32_t i = 0; i < tile_marker_count; i++) legacy |= !tile_markers[i].locate;
  if (!legacy) return 0u;

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
        TxTile tile;
        if (!read_tile_at(w, &off, end, &tile)) {
          w->file = saved_file;
          w->file_len = saved_len;
          return matched;
        }
        {
          uint32_t run = (uint32_t)tile.same + 1u;
          uint32_t y_end = y + run;
          if (y_end < y || y_end > height) y_end = height;
          if (tile.active) {
            const MapMarkerEntry* marker = find_marker_by_id(
                tile_markers, tile_marker_count, (int32_t)tile.type);
            if (marker) {
              uint32_t px = clamp_preview_coord((int32_t)x, width, preview_w);
              uint32_t radius = scale_marker_measure(
                  marker->radius, width, height, preview_w, preview_h);
              uint32_t line_width = scale_marker_measure(
                  marker->line_width, width, height, preview_w, preview_h);
              draw_marker_span_at_rgb(
                  rgb, preview_w, preview_h, px, y, y_end,
                  radius, line_width, marker->rgba);
              if (UINT32_MAX - matched < run) matched = UINT32_MAX;
              else matched += run;
            }
          }
          y = y_end;
        }
      }
    }

    w->file = saved_file;
    w->file_len = saved_len;
    return matched;
  }
}

/*
 * Native marked previews keep one compact RGB surface and stream only the PNG
 * scanlines. The world tile stream is never rescanned for each output strip.
 */
#define MARKED_PREVIEW_STRIP_ROWS 128u
#define MAX_MARKED_PREVIEW_RGB_BYTES (64u * 1024u * 1024u)

typedef struct TxPngDeflateStream {
  TxBuf* out;
  uint32_t bitbuf;
  uint32_t bitcnt;
  uint32_t adler_a;
  uint32_t adler_b;
} TxPngDeflateStream;

static void png_deflate_adler_update(TxPngDeflateStream* stream,
                                     const uint8_t* data, uint32_t len) {
  extern unsigned long adler32(unsigned long, const uint8_t*, unsigned int);
  uint32_t checksum = (uint32_t)adler32((stream->adler_b << 16u) | stream->adler_a, data, len);
  stream->adler_a = checksum & 65535u;
  stream->adler_b = checksum >> 16u;
}

static void png_deflate_init(TxPngDeflateStream* stream, TxBuf* out) {
  stream->out = out;
  stream->bitbuf = 0u;
  stream->bitcnt = 0u;
  stream->adler_a = 1u;
  stream->adler_b = 0u;
  buf_u8(out, 0x78u);
  buf_u8(out, 0x01u);
}

static void png_deflate_write_block(TxPngDeflateStream* stream,
                                    const uint8_t* data, uint32_t len,
                                    uint32_t final_block) {
  write_fixed_block(
      stream->out, &stream->bitbuf, &stream->bitcnt,
      data, len, final_block);
  png_deflate_adler_update(stream, data, len);
}

static void png_deflate_finish(TxPngDeflateStream* stream) {
  bw_finish(stream->out, &stream->bitbuf, &stream->bitcnt);
  {
    uint32_t adler = (stream->adler_b << 16u) | stream->adler_a;
    buf_u8(stream->out, (uint8_t)(adler >> 24u));
    buf_u8(stream->out, (uint8_t)(adler >> 16u));
    buf_u8(stream->out, (uint8_t)(adler >> 8u));
    buf_u8(stream->out, (uint8_t)adler);
  }
}

static void write_u32be_at(uint8_t* p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24u);
  p[1] = (uint8_t)(value >> 16u);
  p[2] = (uint8_t)(value >> 8u);
  p[3] = (uint8_t)value;
}

static int begin_streamed_png(TxBuf* out, uint32_t pw, uint32_t ph,
                              uint8_t color_type,
                              uint32_t* idat_length_pos,
                              uint32_t* idat_type_pos,
                              uint32_t* idat_data_pos) {
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  uint8_t ihdr[13];
  buf_bytes(out, sig, 8u);
  ihdr[0] = (uint8_t)(pw >> 24u); ihdr[1] = (uint8_t)(pw >> 16u);
  ihdr[2] = (uint8_t)(pw >> 8u);  ihdr[3] = (uint8_t)pw;
  ihdr[4] = (uint8_t)(ph >> 24u); ihdr[5] = (uint8_t)(ph >> 16u);
  ihdr[6] = (uint8_t)(ph >> 8u);  ihdr[7] = (uint8_t)ph;
  ihdr[8] = 8u;
  ihdr[9] = color_type;
  ihdr[10] = 0u;
  ihdr[11] = 0u;
  ihdr[12] = 0u;
  png_chunk(out, "IHDR", ihdr, 13u);

  *idat_length_pos = out->len;
  buf_u32be(out, 0u);
  *idat_type_pos = out->len;
  buf_bytes(out, "IDAT", 4u);
  *idat_data_pos = out->len;
  return out->ok;
}

static int encode_marked_preview_png_stream(
    TxWorld* w, uint32_t pw, uint32_t ph, uint32_t stride,
    const MapMarkerEntry* chest_markers, uint32_t chest_count,
    const MapMarkerEntry* tile_markers, uint32_t tile_count,
    const TxBuf* entity_points,
    uint32_t* matched_chest_count, uint32_t* matched_tile_count) {
  uint32_t strip_rows = ph < MARKED_PREVIEW_STRIP_ROWS ? ph : MARKED_PREVIEW_STRIP_ROWS;
  uint64_t rgba_cap64 = (uint64_t)stride * strip_rows;
  uint64_t raw_stride64 = (uint64_t)stride + 1u;
  uint64_t raw_cap64 = raw_stride64 * strip_rows;
  uint8_t* rgba = NULL;
  uint8_t* raw = NULL;
  TxBuf out;
  uint32_t idat_length_pos = 0u;
  uint32_t idat_type_pos = 0u;
  uint32_t idat_data_pos = 0u;
  TxPngDeflateStream stream;

  if (rgba_cap64 > UINT32_MAX || raw_cap64 > UINT32_MAX) {
    tx_set_error("TERRAX_BAD_PREVIEW_SIZE", "preview strip exceeds WASM limits");
    return -1;
  }

  rgba = tx_alloc((uint32_t)rgba_cap64);
  raw = tx_alloc((uint32_t)raw_cap64);
  if (!rgba || !raw) {
    if (raw) tx_internal_free(raw);
    if (rgba) tx_internal_free(rgba);
    tx_set_error("TERRAX_WASM_OOM", "preview strip allocation failed");
    return -1;
  }

  buf_init(&out, 1024u);
  if (!out.ok || !begin_streamed_png(
      &out, pw, ph, 6u, &idat_length_pos, &idat_type_pos, &idat_data_pos)) {
    if (out.data) tx_internal_free(out.data);
    tx_internal_free(raw);
    tx_internal_free(rgba);
    tx_set_error("TERRAX_WASM_OOM", "PNG output allocation failed");
    return -1;
  }
  png_deflate_init(&stream, &out);

  *matched_chest_count = 0u;
  *matched_tile_count = entity_points ? entity_points->len / sizeof(TxMarkerPoint) : 0u;
  for (uint32_t row_start = 0u; row_start < ph; row_start += strip_rows) {
    uint32_t rows = ph - row_start;
    if (rows > strip_rows) rows = strip_rows;
    render_preview_rows_to(w, rgba, pw, ph, row_start, rows);

    if (chest_count > 0u) {
      uint32_t matched = draw_matching_chest_markers_preview_rows(
          w, rgba, pw, ph, row_start, rows,
          chest_markers, chest_count, row_start == 0u);
      if (row_start == 0u) *matched_chest_count = matched;
    }
    if (tile_count > 0u) {
      uint32_t matched = draw_matching_tile_markers_preview_rows(
          w, rgba, pw, ph, row_start, rows,
          tile_markers, tile_count, row_start == 0u);
      if (row_start == 0u) *matched_tile_count += matched;
    }

    draw_entity_points_rows(w, rgba, pw, ph, row_start, rows, tile_markers, entity_points);
    {
      uint32_t raw_len = 0u;
      for (uint32_t local_row = 0u; local_row < rows; local_row++) {
        raw[raw_len++] = 0u;
        memcpy(raw + raw_len, rgba + local_row * stride, stride);
        raw_len += stride;
      }
      png_deflate_write_block(
          &stream, raw, raw_len, row_start + rows >= ph);
    }
    if (!out.ok) {
      tx_internal_free(out.data);
      tx_internal_free(raw);
      tx_internal_free(rgba);
      tx_set_error("TERRAX_WASM_OOM", "PNG compression failed");
      return -1;
    }
  }

  png_deflate_finish(&stream);
  if (!out.ok || out.len < idat_data_pos ||
      out.len - idat_data_pos > UINT32_MAX - 12u) {
    if (out.data) tx_internal_free(out.data);
    tx_internal_free(raw);
    tx_internal_free(rgba);
    tx_set_error("TERRAX_WASM_OOM", "PNG compression failed");
    return -1;
  }

  {
    uint32_t idat_len = out.len - idat_data_pos;
    uint32_t crc = crc32_bytes(out.data + idat_type_pos, idat_len + 4u);
    write_u32be_at(out.data + idat_length_pos, idat_len);
    buf_u32be(&out, crc);
  }
  png_chunk(&out, "IEND", (const uint8_t*)0, 0u);

  tx_internal_free(raw);
  tx_internal_free(rgba);
  if (!out.ok) {
    if (out.data) tx_internal_free(out.data);
    tx_set_error("TERRAX_WASM_OOM", "PNG assembly failed");
    return -1;
  }

  tx_last_width = pw;
  tx_last_height = ph;
  return set_result_buf(&out);
}

static int encode_full_preview_rgb_png(
    TxWorld* w, uint32_t pw, uint32_t ph,
    const MapMarkerEntry* chest_markers, uint32_t chest_count,
    const MapMarkerEntry* tile_markers, uint32_t tile_count,
    const TxBuf* entity_points,
    uint32_t* matched_chest_count, uint32_t* matched_tile_count) {
  uint64_t rgb_len64 = (uint64_t)pw * ph * 3u;
  uint32_t stride = pw * 3u;
  uint32_t strip_rows = ph < MARKED_PREVIEW_STRIP_ROWS ? ph : MARKED_PREVIEW_STRIP_ROWS;
  uint64_t raw_cap64 = ((uint64_t)stride + 1u) * strip_rows;
  uint8_t* rgb = NULL;
  uint8_t* raw = NULL;
  TxBuf out;
  uint32_t idat_length_pos = 0u;
  uint32_t idat_type_pos = 0u;
  uint32_t idat_data_pos = 0u;
  TxPngDeflateStream stream;

  if (rgb_len64 == 0u || rgb_len64 > MAX_MARKED_PREVIEW_RGB_BYTES ||
      rgb_len64 > UINT32_MAX || raw_cap64 > UINT32_MAX) {
    tx_set_error(
        "TERRAX_BAD_PREVIEW_SIZE",
        "full marked preview exceeds the low-memory image budget");
    return -1;
  }

  rgb = tx_output_take_rgb(w, pw, ph);
  int prepared_rgb = rgb != NULL;
  void (*free_rgb)(void*) = prepared_rgb ? tx_persistent_free : tx_internal_free;
  if (!rgb) rgb = tx_alloc((uint32_t)rgb_len64);
  raw = tx_alloc((uint32_t)raw_cap64);
  if (!rgb || !raw) {
    if (raw) tx_internal_free(raw);
    if (rgb) free_rgb(rgb);
    tx_set_error("TERRAX_WASM_OOM", "full marked preview allocation failed");
    return -1;
  }

  if (!prepared_rgb && !render_full_preview_rgb_to(w, rgb, pw, ph)) {
    tx_internal_free(raw);
    free_rgb(rgb);
    tx_set_error("TERRAX_BAD_PREVIEW_SIZE", "full marked preview rendering failed");
    return -1;
  }

  {
    uint32_t matched_chests = draw_matching_chest_markers_rgb(
        w, rgb, pw, ph, chest_markers, chest_count);
    uint32_t matched_tiles = draw_matching_tile_markers_rgb(
        w, rgb, pw, ph, tile_markers, tile_count);
    if (matched_chest_count) *matched_chest_count = matched_chests;
    if (matched_tile_count) *matched_tile_count = matched_tiles + (entity_points ? entity_points->len / sizeof(TxMarkerPoint) : 0u);
    draw_entity_points_rgb(w, rgb, pw, ph, tile_markers, entity_points);
  }

  buf_init(&out, 1024u);
  if (!out.ok || !begin_streamed_png(
      &out, pw, ph, 2u, &idat_length_pos, &idat_type_pos, &idat_data_pos)) {
    if (out.data) tx_internal_free(out.data);
    tx_internal_free(raw);
    free_rgb(rgb);
    tx_set_error("TERRAX_WASM_OOM", "full marked PNG output allocation failed");
    return -1;
  }
  png_deflate_init(&stream, &out);

  for (uint32_t row_start = 0u; row_start < ph; row_start += strip_rows) {
    uint32_t rows = ph - row_start;
    if (rows > strip_rows) rows = strip_rows;
    uint32_t raw_len = 0u;
    for (uint32_t local_row = 0u; local_row < rows; local_row++) {
      raw[raw_len++] = 0u;
      memcpy(
          raw + raw_len,
          rgb + ((uint64_t)(row_start + local_row) * stride),
          stride);
      raw_len += stride;
    }
    png_deflate_write_block(
        &stream, raw, raw_len, row_start + rows >= ph);
    if (!out.ok) {
      tx_internal_free(out.data);
      tx_internal_free(raw);
      free_rgb(rgb);
      tx_set_error("TERRAX_WASM_OOM", "full marked PNG compression failed");
      return -1;
    }
  }

  png_deflate_finish(&stream);
  if (!out.ok || out.len < idat_data_pos ||
      out.len - idat_data_pos > UINT32_MAX - 12u) {
    if (out.data) tx_internal_free(out.data);
    tx_internal_free(raw);
    free_rgb(rgb);
    tx_set_error("TERRAX_WASM_OOM", "full marked PNG compression failed");
    return -1;
  }

  {
    uint32_t idat_len = out.len - idat_data_pos;
    uint32_t crc = crc32_bytes(out.data + idat_type_pos, idat_len + 4u);
    write_u32be_at(out.data + idat_length_pos, idat_len);
    buf_u32be(&out, crc);
  }
  png_chunk(&out, "IEND", (const uint8_t*)0, 0u);

  tx_internal_free(raw);
  free_rgb(rgb);
  if (!out.ok) {
    if (out.data) tx_internal_free(out.data);
    tx_set_error("TERRAX_WASM_OOM", "full marked PNG assembly failed");
    return -1;
  }

  tx_last_width = pw;
  tx_last_height = ph;
  return set_result_buf(&out);
}

/* ====================================================================
 * Exported: render preview as PNG in WASM memory
 *
 * Renders RGBA first, then encodes as PNG with fixed-Huffman zlib.
 * Returns byte count on success, -1 on error.
 * Result pointer available via tx_last_ptr/tx_last_len.
 * ==================================================================== */



int32_t txw_render_marked_preview_png(TxWorld* w, uint32_t max_w, uint32_t max_h,
                                      const MapMarkerEntry* chest_markers, uint32_t chest_count,
                                      const MapMarkerEntry* tile_markers, uint32_t tile_count,
                                      uint32_t* matched_chest_count,
                                      uint32_t* matched_tile_count) {
  uint32_t pw = 0u;
  uint32_t ph = 0u;
  uint32_t stride = 0u;

  if (matched_chest_count) *matched_chest_count = 0u;
  if (matched_tile_count) *matched_tile_count = 0u;
  if (!compute_preview_size(w, max_w, max_h, &pw, &ph, &stride)) return -1;

  if (max_w == 0u && max_h == 0u) {
    return encode_full_preview_rgb_png(
        w, pw, ph, chest_markers, chest_count, tile_markers, tile_count, NULL,
        matched_chest_count, matched_tile_count);
  }

  return encode_marked_preview_png_stream(
      w, pw, ph, stride,
      chest_markers, chest_count, tile_markers, tile_count, NULL,
      matched_chest_count, matched_tile_count);
}
