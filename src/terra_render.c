/*
 * terra_render.c -- Rendering entry points and scaled-preview fast path.
 *
 * Keep the established renderer implementation in terra_render_core.inc and
 * wrap only the marked-preview entry point here. Scaled previews taller than
 * one PNG strip used to restart the WLD tile stream for every 128 output rows.
 * When the complete scaled RGBA surface fits a bounded cache, render it once,
 * draw markers once, then keep the existing 128-row PNG compression cadence.
 */
#define txw_render_marked_preview_png txw_render_marked_preview_png_striped_fallback
#include "terra_render_core.inc"
#undef txw_render_marked_preview_png

#define SCALED_PREVIEW_CACHE_BUDGET (16u * 1024u * 1024u)
#define SCALED_PREVIEW_CACHE_UNAVAILABLE (-2)

static int encode_cached_scaled_preview_png(
    TxWorld* w, uint32_t pw, uint32_t ph, uint32_t stride,
    const MapMarkerEntry* chest_markers, uint32_t chest_count,
    const MapMarkerEntry* tile_markers, uint32_t tile_count,
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

  /* Native-size previews already have a dedicated single-scan RGB path. */
  if (max_w == 0u && max_h == 0u) {
    return encode_full_preview_rgb_png(
        w, pw, ph, chest_markers, chest_count, tile_markers, tile_count,
        matched_chest_count, matched_tile_count);
  }

  /* Only take the cache path when it removes at least one repeated tile scan.
     Larger scaled surfaces keep the established bounded strip implementation. */
  if (ph > MARKED_PREVIEW_STRIP_ROWS &&
      (uint64_t)stride * ph <= SCALED_PREVIEW_CACHE_BUDGET) {
    int cached = encode_cached_scaled_preview_png(
        w, pw, ph, stride,
        chest_markers, chest_count, tile_markers, tile_count,
        matched_chest_count, matched_tile_count);
    if (cached != SCALED_PREVIEW_CACHE_UNAVAILABLE) return cached;
    tx_clear_error();
  }

  return encode_marked_preview_png_stream(
      w, pw, ph, stride,
      chest_markers, chest_count, tile_markers, tile_count,
      matched_chest_count, matched_tile_count);
}
