/*
 * terra_render.c -- Rendering entry points and scaled-preview fast path.
 *
 * Keep the established renderer implementation in terra_render_core.inc and
 * wrap only the marked-preview entry point here. Scaled previews taller than
 * one PNG strip used to restart the WLD tile stream for every 128 output rows.
 * When the complete scaled RGBA surface fits a bounded cache, render it once,
 * draw markers once, then keep the existing 128-row PNG compression cadence.
 *
 * The renderer core historically queried four color-table getter functions
 * for every non-empty decoded tile. The getters live in terra_mem.c and normal
 * release builds do not enable LTO, so those cross-translation-unit calls sit
 * directly in the world-render hot path. Snapshot the immutable table metadata
 * once at the public render entry and let the core read the cached values.
 */
#include "terra_types.h"
#include "terra_output.h"
#include "terra_render_task.h"

extern const uint8_t* tx_get_tile_colors(void);
extern uint32_t tx_get_tile_color_count(void);
extern const uint8_t* tx_get_wall_colors(void);
extern uint32_t tx_get_wall_color_count(void);

const uint8_t* tx_render_cached_tile_colors = NULL;
uint32_t tx_render_cached_tile_color_count = 0u;
const uint8_t* tx_render_cached_wall_colors = NULL;
uint32_t tx_render_cached_wall_color_count = 0u;

static void tx_render_refresh_color_tables(void) {
  tx_render_cached_tile_colors = tx_get_tile_colors();
  tx_render_cached_tile_color_count = tx_get_tile_color_count();
  tx_render_cached_wall_colors = tx_get_wall_colors();
  tx_render_cached_wall_color_count = tx_get_wall_color_count();
}

/* Function-like variadic macros intentionally also rewrite the core's extern
 * getter declarations into extern declarations for these cached variables.
 * Calls such as tx_get_tile_colors() then compile to direct variable reads.
 */
#define tx_get_tile_colors(...) tx_render_cached_tile_colors
#define tx_get_tile_color_count(...) tx_render_cached_tile_color_count
#define tx_get_wall_colors(...) tx_render_cached_wall_colors
#define tx_get_wall_color_count(...) tx_render_cached_wall_color_count
#define txw_render_marked_preview_png txw_render_marked_preview_png_striped_fallback
#include "terra_render_core.inc"
#undef txw_render_marked_preview_png
#undef tx_get_wall_color_count
#undef tx_get_wall_colors
#undef tx_get_tile_color_count
#undef tx_get_tile_colors

#define SCALED_PREVIEW_CACHE_BUDGET (16u * 1024u * 1024u)
#define SCALED_PREVIEW_CACHE_UNAVAILABLE (-2)
#define OPEN_PREVIEW_MAX_RGBA_BYTES (16u * 1024u * 1024u)

/* The initial 384px thumbnail is part of world opening. Unlike the public
 * preview renderer, this state machine keeps the tile-stream cursor between
 * open_step calls so a caller-selected work budget actually limits each call.
 */
int tx_open_preview_begin(TxWorld* world, TxOpenPreviewTask* task) {
  uint32_t pw = 0u;
  uint32_t ph = 0u;
  uint32_t stride = 0u;
  uint64_t rgba_len64;

  if (!world || !task) {
    tx_set_error("TERRAX_INVALID_ARGUMENT", "null incremental preview state");
    return 0;
  }
  memset(task, 0, sizeof(*task));
  if (!compute_preview_size(world, 384u, 0u, &pw, &ph, &stride)) return 0;
  if (world->pointer_count <= 1u || world->starts[1] > world->ends[1]
      || world->ends[1] > world->file_len) {
    tx_set_error("TERRAX_BAD_POINTERS", "tile section bounds are invalid");
    return 0;
  }

  rgba_len64 = (uint64_t)stride * ph;
  if (rgba_len64 == 0u || rgba_len64 > OPEN_PREVIEW_MAX_RGBA_BYTES
      || rgba_len64 > UINT32_MAX) {
    tx_set_error("TERRAX_BAD_PREVIEW_SIZE", "open thumbnail exceeds incremental preview budget");
    return 0;
  }

  task->rgba = tx_alloc((uint32_t)rgba_len64);
  if (!task->rgba) {
    tx_set_error("TERRAX_WASM_OOM", "open thumbnail allocation failed");
    return 0;
  }
  task->preview_width = pw;
  task->preview_height = ph;
  task->stride = stride;
  task->source_width = (uint32_t)world->maxTilesX;
  task->source_height = (uint32_t)world->maxTilesY;
  task->ground = (uint32_t)world->worldSurface;
  task->rock = (uint32_t)world->rockLayer;
  task->tile_start = world->starts[1];
  task->tile_end = world->ends[1];
  task->offset = task->tile_start;
  task->x = 0u;
  task->y = 0u;
  task->initialized = 1u;

  tx_render_refresh_color_tables();

  /* Match render_preview_rows_to exactly: background RGB is prefilled and the
   * alpha byte temporarily counts non-empty source samples for downsampling. */
  for (uint32_t py = 0u; py < ph; py++) {
    uint8_t bg[4];
    uint32_t wy = (uint32_t)(((uint64_t)py * task->source_height) / ph);
    background_color(wy, task->source_height, task->ground, task->rock, bg);
    for (uint32_t px = 0u; px < pw; px++) {
      uint32_t o = (py * pw + px) * 4u;
      task->rgba[o] = bg[0];
      task->rgba[o + 1u] = bg[1];
      task->rgba[o + 2u] = bg[2];
      task->rgba[o + 3u] = 0u;
    }
  }
  return 1;
}

static void tx_open_preview_finalize_alpha(TxOpenPreviewTask* task) {
  if (!task || !task->rgba) return;
  uint64_t pixels = (uint64_t)task->preview_width * task->preview_height;
  for (uint64_t index = 0u; index < pixels; index++) {
    task->rgba[index * 4u + 3u] = 255u;
  }
}

int tx_open_preview_step(TxWorld* world, TxOpenPreviewTask* task, uint32_t record_budget) {
  if (!world || !task || !task->initialized || !task->rgba) {
    tx_set_error("TERRAX_STATE_ERROR", "incremental preview is not initialized");
    return -1;
  }
  if (task->finished) return 1;
  if (record_budget == 0u) record_budget = 1u;

  while (record_budget-- > 0u && task->x < task->source_width) {
    TxTile tile;
    if (!read_tile_at(world, &task->offset, task->tile_end, &tile)) {
      /* Preserve the established preview behavior: a short tile stream yields
       * the partial image rather than converting open into a new parse error. */
      task->finished = 1u;
      break;
    }

    {
      uint32_t run = (uint32_t)tile.same + 1u;
      if (tile_is_non_empty(&tile)) {
        uint8_t color[4];
        uint32_t px = (uint32_t)(((uint64_t)task->x * task->preview_width)
            / task->source_width);
        uint32_t py0 = (uint32_t)(((uint64_t)task->y * task->preview_height)
            / task->source_height);
        uint32_t py1 = (uint32_t)((((uint64_t)task->y + run) * task->preview_height)
            / task->source_height);
        if (py1 <= py0) py1 = py0 + 1u;
        if (py1 > task->preview_height) py1 = task->preview_height;

        color_for_tile(
            &tile, task->y, task->source_height,
            task->ground, task->rock, color);
        if (px < task->preview_width && py0 < task->preview_height) {
          for (uint32_t py = py0; py < py1; py++) {
            uint32_t o = (py * task->preview_width + px) * 4u;
            uint32_t count = task->rgba[o + 3u];
            if (count == 0u) {
              task->rgba[o] = color[0];
              task->rgba[o + 1u] = color[1];
              task->rgba[o + 2u] = color[2];
              task->rgba[o + 3u] = 1u;
            } else if (count < 255u) {
              uint32_t next = count + 1u;
              task->rgba[o] = (uint8_t)(((uint32_t)task->rgba[o] * count + color[0]) / next);
              task->rgba[o + 1u] = (uint8_t)(((uint32_t)task->rgba[o + 1u] * count + color[1]) / next);
              task->rgba[o + 2u] = (uint8_t)(((uint32_t)task->rgba[o + 2u] * count + color[2]) / next);
              task->rgba[o + 3u] = (uint8_t)next;
            } else {
              task->rgba[o] = (uint8_t)(((uint32_t)task->rgba[o] * 255u + color[0]) >> 8);
              task->rgba[o + 1u] = (uint8_t)(((uint32_t)task->rgba[o + 1u] * 255u + color[1]) >> 8);
              task->rgba[o + 2u] = (uint8_t)(((uint32_t)task->rgba[o + 2u] * 255u + color[2]) >> 8);
            }
          }
        }
      }

      task->y += run;
      if (task->y >= task->source_height) {
        task->x++;
        task->y = 0u;
      }
    }
  }

  if (task->x >= task->source_width) task->finished = 1u;
  if (task->finished) {
    tx_open_preview_finalize_alpha(task);
    return 1;
  }
  return 0;
}

uint32_t tx_open_preview_progress(const TxOpenPreviewTask* task) {
  if (!task || !task->initialized) return 0u;
  if (task->finished) return 100u;
  if (task->tile_end <= task->tile_start || task->offset <= task->tile_start) return 0u;
  {
    uint64_t done = task->offset - task->tile_start;
    uint64_t total = task->tile_end - task->tile_start;
    uint64_t percent = (done * 100u) / total;
    return percent > 99u ? 99u : (uint32_t)percent;
  }
}

int32_t tx_open_preview_finish_png(TxOpenPreviewTask* task) {
  uint8_t* rgba;
  if (!task || !task->initialized || !task->finished || !task->rgba) {
    tx_set_error("TERRAX_STATE_ERROR", "incremental preview is not ready to encode");
    return -1;
  }
  rgba = task->rgba;
  task->rgba = NULL;
  return encode_png_from_owned_rgba(
      rgba, task->preview_width, task->preview_height);
}

void tx_open_preview_discard(TxOpenPreviewTask* task) {
  if (!task) return;
  if (task->rgba) tx_internal_free(task->rgba);
  memset(task, 0, sizeof(*task));
}

static int encode_cached_scaled_preview_png(
    TxWorld* w, uint32_t pw, uint32_t ph, uint32_t stride,
    const MapMarkerEntry* chest_markers, uint32_t chest_count,
    const MapMarkerEntry* tile_markers, uint32_t tile_count,
    const TxBuf* entity_points,
    uint32_t* matched_chest_count, uint32_t* matched_tile_count) {
  uint32_t strip_rows = ph < MARKED_PREVIEW_STRIP_ROWS ? ph : MARKED_PREVIEW_STRIP_ROWS;
  uint64_t rgba_cap64 = (uint64_t)stride * ph;
  uint64_t raw_stride64 = (uint64_t)stride + 1u;
  uint64_t raw_cap64 = raw_stride64 * strip_rows;
  uint8_t* rgba = NULL;
  uint8_t* raw = NULL;
  TxBuf out;
  uint32_t idat_length_pos = 0u;
  uint32_t idat_type_pos = 0u;
  uint32_t idat_data_pos = 0u;
  TxPngDeflateStream stream;

  if (rgba_cap64 == 0u || rgba_cap64 > SCALED_PREVIEW_CACHE_BUDGET ||
      rgba_cap64 > UINT32_MAX || raw_cap64 > UINT32_MAX) {
    return SCALED_PREVIEW_CACHE_UNAVAILABLE;
  }

  rgba = tx_alloc((uint32_t)rgba_cap64);
  if (!rgba) return SCALED_PREVIEW_CACHE_UNAVAILABLE;
  raw = tx_alloc((uint32_t)raw_cap64);
  if (!raw) {
    tx_internal_free(rgba);
    return SCALED_PREVIEW_CACHE_UNAVAILABLE;
  }

  /* One world-tile scan for the complete scaled surface. The old strip path
     below remains the fallback for large scaled outputs that exceed the cache. */
  render_preview_to(w, rgba, pw, ph);

  if (matched_chest_count) *matched_chest_count = 0u;
  if (matched_tile_count) *matched_tile_count = 0u;
  if (chest_count > 0u) {
    uint32_t matched = draw_matching_chest_markers_preview(
        w, rgba, pw, ph, chest_markers, chest_count);
    if (matched_chest_count) *matched_chest_count = matched;
  }
  if (tile_count > 0u) {
    uint32_t matched = draw_matching_tile_markers_preview(
        w, rgba, pw, ph, tile_markers, tile_count);
    if (matched_tile_count) *matched_tile_count = matched;
  }

  draw_entity_points_rows(w, rgba, pw, ph, 0u, ph, tile_markers, entity_points);
  if (matched_tile_count && entity_points) *matched_tile_count += entity_points->len / sizeof(TxMarkerPoint);

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

  /* Preserve the existing 128-row deflate block boundaries so cached and
     fallback scaled previews produce the same PNG encoding cadence. */
  for (uint32_t row_start = 0u; row_start < ph; row_start += strip_rows) {
    uint32_t rows = ph - row_start;
    if (rows > strip_rows) rows = strip_rows;
    uint32_t raw_len = 0u;
    for (uint32_t local_row = 0u; local_row < rows; local_row++) {
      raw[raw_len++] = 0u;
      memcpy(
          raw + raw_len,
          rgba + ((uint64_t)(row_start + local_row) * stride),
          stride);
      raw_len += stride;
    }
    png_deflate_write_block(
        &stream, raw, raw_len, row_start + rows >= ph);
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

static int32_t render_located_preview(TxWorld* w, uint32_t max_w, uint32_t max_h,
                                      const MapMarkerEntry* chest_markers, uint32_t chest_count,
                                      const MapMarkerEntry* tile_markers, uint32_t tile_count,
                                      const TxBuf* entity_points,
                                      uint32_t* matched_chest_count,
                                      uint32_t* matched_tile_count) {
  uint32_t pw = 0u;
  uint32_t ph = 0u;
  uint32_t stride = 0u;

  if (matched_chest_count) *matched_chest_count = 0u;
  if (matched_tile_count) *matched_tile_count = 0u;
  if (!compute_preview_size(w, max_w, max_h, &pw, &ph, &stride)) return -1;

  tx_render_refresh_color_tables();

  /* Native-size previews already have a dedicated single-scan RGB path. */
  if (max_w == 0u && max_h == 0u) {
    return encode_full_preview_rgb_png(
        w, pw, ph, chest_markers, chest_count, tile_markers, tile_count, entity_points,
        matched_chest_count, matched_tile_count);
  }

  /* Only take the cache path when it removes at least one repeated tile scan.
     Larger scaled surfaces keep the established bounded strip implementation. */
  if (ph > MARKED_PREVIEW_STRIP_ROWS &&
      (uint64_t)stride * ph <= SCALED_PREVIEW_CACHE_BUDGET) {
    int cached = encode_cached_scaled_preview_png(
        w, pw, ph, stride,
        chest_markers, chest_count, tile_markers, tile_count, entity_points,
        matched_chest_count, matched_tile_count);
    if (cached != SCALED_PREVIEW_CACHE_UNAVAILABLE) return cached;
    tx_clear_error();
  }

  return encode_marked_preview_png_stream(
      w, pw, ph, stride,
      chest_markers, chest_count, tile_markers, tile_count, entity_points,
      matched_chest_count, matched_tile_count);
}

int32_t txw_render_marked_preview_png(TxWorld* w, uint32_t max_w, uint32_t max_h,
    const MapMarkerEntry* chest_markers, uint32_t chest_count,
    const MapMarkerEntry* tile_markers, uint32_t tile_count,
    uint32_t* matched_chest_count, uint32_t* matched_tile_count) {
  TxBuf points = {0};
  if (!tx_locate_tile_markers(w, tile_markers, tile_count, &points)) return -1;
  int32_t result = render_located_preview(w, max_w, max_h, chest_markers, chest_count,
      tile_markers, tile_count, &points, matched_chest_count, matched_tile_count);
  if (points.data) tx_internal_free(points.data);
  return result;
}

void tx_output_free(TxPreparedOutput* p) {
  if (!p) return;
  tx_persistent_free(p->rgb);
  tx_persistent_free(p->list_rgba);
  tx_persistent_free(p->points.data);
  tx_map_base_free(p->map);
  tx_persistent_free(p);
}

void tx_output_clear(TxWorld* w) {
  tx_output_free(w->prepared_output);
  w->prepared_output = NULL;
}

int tx_output_begin(TxWorld* w, const MapMarkerEntry* markers, uint32_t count, int map) {
  uint32_t pw, ph, stride;
  tx_output_clear(w);
  if (count > 256u || !compute_preview_size(w, 256u, 0u, &pw, &ph, &stride)) return 0;
  uint64_t bytes = (uint64_t)w->maxTilesX * w->maxTilesY * 3u;
  if (!bytes || bytes > MAX_MARKED_PREVIEW_RGB_BYTES) {
    tx_set_error("TERRAX_BAD_PREVIEW_SIZE", "prepared preview exceeds the image budget");
    return 0;
  }
  TxPreparedOutput* p = (TxPreparedOutput*)tx_persistent_alloc(sizeof(*p));
  if (!p) goto oom;
  memset(p, 0, sizeof(*p));
  w->prepared_output = p;
  p->width = (uint32_t)w->maxTilesX; p->height = (uint32_t)w->maxTilesY;
  p->list_width = pw; p->list_height = ph;
  p->marker_count = count;
  if (count) memcpy(p->markers, markers, count * sizeof(*markers));
  p->rgb = tx_persistent_alloc((uint32_t)bytes);
  p->list_rgba = tx_persistent_alloc(stride * ph);
  if (!p->rgb || !p->list_rgba) goto oom;
  if (map && !(p->map = tx_map_base_begin(w))) { tx_output_clear(w); return 0; }
  for (uint32_t y = 0; y < p->height; y++) {
    uint8_t bg[4];
    background_color(y, p->height, (uint32_t)w->worldSurface, (uint32_t)w->rockLayer, bg);
    for (uint32_t x = 0; x < p->width; x++) memcpy(p->rgb + (y * p->width + x) * 3u, bg, 3u);
  }
  for (uint32_t y = 0; y < ph; y++) {
    uint8_t bg[4];
    background_color((uint32_t)((uint64_t)y * p->height / ph), p->height,
                     (uint32_t)w->worldSurface, (uint32_t)w->rockLayer, bg);
    bg[3] = 0;
    for (uint32_t x = 0; x < pw; x++) memcpy(p->list_rgba + (y * pw + x) * 4u, bg, 4u);
  }
  return 1;
oom:
  tx_output_clear(w);
  tx_set_error("TERRAX_WASM_OOM", "output preparation allocation failed");
  return 0;
}

typedef struct OutputScanContext { TxTileRule* rules; uint32_t count; TxBuf* tiles; } OutputScanContext;

/* Match the values a subsequent WLD read would observe after write_tile. */
static void normalize_written_tile(TxWorld* w, TxTile* t) {
  extern int tile_important(TxWorld*, uint16_t);
  if (!t->active) { t->type = 0; t->frame_x = t->frame_y = 0; t->tile_color = 0; }
  else if (!tile_important(w, t->type)) t->frame_x = t->frame_y = -1;
  else if (t->type == 144u) t->frame_y = 0;
  if (!t->wall) t->wall_color = 0;
  if (t->liquid_type != 4u) t->liquid_type &= 3u;
  if (!t->liquid_amount || !t->liquid_type) t->liquid_amount = t->liquid_type = 0;
  t->brick_style &= 7u;
}

static int prepare_output_run(TxWorld* w, uint32_t x, uint32_t y, TxTile* t,
                              uint32_t run, void* context) {
  OutputScanContext* scan = (OutputScanContext*)context;
  TxPreparedOutput* p = w->prepared_output;
  if (scan->tiles) {
    extern void write_tile(TxWorld*, TxBuf*, const TxTile*, uint32_t);
    tx_apply_tile_rules(t, scan->rules, scan->count, run);
    write_tile(w, scan->tiles, t, run - 1u);
    if (!scan->tiles->ok) return 0;
    normalize_written_tile(w, t);
  }
  p->source_runs++;
  if (p->map && !tx_map_base_run(w, p->map, x, y, t, run)) return 0;
  if (!tile_is_non_empty(t)) return 1;
  uint8_t c[4];
  color_for_tile(t, y, p->height, (uint32_t)w->worldSurface, (uint32_t)w->rockLayer, c);
  for (uint32_t yy = y; yy < y + run; yy++) memcpy(p->rgb + (yy * p->width + x) * 3u, c, 3u);
  uint32_t px = (uint32_t)((uint64_t)x * p->list_width / p->width);
  uint32_t py0 = (uint32_t)((uint64_t)y * p->list_height / p->height);
  uint32_t py1 = (uint32_t)((uint64_t)(y + run) * p->list_height / p->height);
  if (py1 <= py0) py1 = py0 + 1u;
  if (py1 > p->list_height) py1 = p->list_height;
  for (uint32_t py = py0; py < py1; py++) {
    uint8_t* dest = p->list_rgba + (py * p->list_width + px) * 4u;
    uint32_t count = dest[3];
    for (uint32_t ch = 0; ch < 3; ch++)
      dest[ch] = count < 255u ? (uint8_t)((dest[ch] * count + c[ch]) / (count + 1u))
                              : (uint8_t)((dest[ch] * 255u + c[ch]) >> 8);
    if (count < 255u) dest[3]++;
  }
  return 1;
}

int tx_output_scan(TxWorld* w, TxTileRule* rules, uint32_t count, TxBuf* tiles) {
  TxPreparedOutput* p = w->prepared_output;
  if (!p || p->ready) return 1;
  TxBuf points = {0};
  OutputScanContext scan = {rules, count, tiles};
  tx_render_refresh_color_tables();
  if (!tx_scan_tile_markers(w, p->markers, p->marker_count, &points, prepare_output_run, &scan)) goto fail;
  if (points.len) {
    p->points.data = tx_persistent_alloc(points.len);
    if (!p->points.data) { tx_set_error("TERRAX_WASM_OOM", "prepared markers allocation failed"); goto fail; }
    memcpy(p->points.data, points.data, points.len);
  }
  p->points.len = points.len;
  if (points.data) tx_internal_free(points.data);
  for (uint32_t i = 0; i < p->list_width * p->list_height; i++) p->list_rgba[i * 4u + 3u] = 255u;
  p->ready = 1;
  return 1;
fail:
  if (points.data) tx_internal_free(points.data);
  tx_output_clear(w);
  return 0;
}

uint8_t* tx_output_take_rgb(TxWorld* w, uint32_t width, uint32_t height) {
  TxPreparedOutput* p = w->prepared_output;
  if (!p || !p->ready || p->width != width || p->height != height) return NULL;
  uint8_t* rgb = p->rgb;
  p->rgb = NULL;
  return rgb;
}

int tx_output_copy_rows(TxWorld* w, uint8_t* out, uint32_t width, uint32_t height,
                        uint32_t start, uint32_t count) {
  TxPreparedOutput* p = w->prepared_output;
  if (!p || !p->ready || width != p->list_width || height != p->list_height ||
      start > height || count > height - start) return 0;
  memcpy(out, p->list_rgba + start * width * 4u, count * width * 4u);
  return 1;
}
