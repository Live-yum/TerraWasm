/*
 * terra_render_png_lowmem.c -- Low-memory public PNG preview entry point.
 *
 * The retired full-RGBA preview implementation materialized a full RGBA
 * surface, a second full PNG scanline buffer, and a compression/output buffer
 * at the same time. A native 8400x2400 world needs about 76.9 MiB for each of
 * the first two buffers alone, which cannot fit inside the 160 MiB Web/Mini
 * Program linear-memory ceiling once the live world and allocator overhead are
 * included.
 *
 * The marked-preview renderer already owns the equivalent low-memory encoder:
 * native-size output uses one compact RGB surface plus a bounded scanline
 * strip, while scaled output streams bounded RGBA strips. With zero markers it
 * produces the same unmarked world preview, so reuse that implementation here
 * instead of maintaining a second whole-image PNG pipeline.
 */
#include "terra_types.h"
#include "terra_map.h"

extern int32_t txw_render_marked_preview_png(
    TxWorld* w, uint32_t max_w, uint32_t max_h,
    const MapMarkerEntry* chest_markers, uint32_t chest_count,
    const MapMarkerEntry* tile_markers, uint32_t tile_count,
    uint32_t* matched_chest_count,
    uint32_t* matched_tile_count);

int32_t txw_render_preview_png(TxWorld* w, uint32_t max_w, uint32_t max_h) {
  uint32_t matched_chest_count = 0u;
  uint32_t matched_tile_count = 0u;
  return txw_render_marked_preview_png(
      w, max_w, max_h,
      (const MapMarkerEntry*)0, 0u,
      (const MapMarkerEntry*)0, 0u,
      &matched_chest_count, &matched_tile_count);
}
