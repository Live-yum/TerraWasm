/* Authoritative sparse pixel workspace, ABI 1. All buffers are caller-owned.
 * Pixel/palette/history data is owned by the handle; no returned borrowed pointers.
 * Coordinates are zero-based; palette index 0 is always transparent/missing.
 * Status 0 is success. On a mutating error, an open transaction is rolled back.
 * Nonmutating query errors do not change the transaction. Close accepts no stale IDs.
 */
#ifndef TERRA_PIXEL_WORKSPACE_H
#define TERRA_PIXEL_WORKSPACE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define TERRA_PIXEL_WORKSPACE_ABI 1u
#define TERRA_PIXEL_BLOCK_SIDE 64u
#define TERRA_PIXEL_QUERY_CELLS 4096u
#define TERRA_PIXEL_BATCH_CELLS 65536u
#define TERRA_PIXEL_BATCH_POINTS 4096u
#define TERRA_PIXEL_MAX_DIMENSION 16384u

enum TerraPixelStatus {
    TERRA_PIXEL_OK = 0, TERRA_PIXEL_INVALID = -1, TERRA_PIXEL_HANDLE = -2,
    TERRA_PIXEL_TRANSACTION = -3, TERRA_PIXEL_LIMIT = -4, TERRA_PIXEL_OOM = -5,
    TERRA_PIXEL_BOUNDS = -6, TERRA_PIXEL_PALETTE = -7, TERRA_PIXEL_HISTORY = -8,
    TERRA_PIXEL_EXHAUSTED = -9
};
enum TerraPixelTransform {
    TERRA_PIXEL_FLIP_X = 1, TERRA_PIXEL_FLIP_Y = 2, TERRA_PIXEL_ROTATE_180 = 3,
    /* 90-degree transforms require a square selection. */
    TERRA_PIXEL_ROTATE_CW = 4, TERRA_PIXEL_ROTATE_CCW = 5
};
/* 24 little-endian u32 fields, 96 bytes, no platform-dependent members.
 * active_bytes and peak_bytes conservatively account payloads + 128 bytes per
 * allocation; this is an allocation budget, not process RSS or Wasm capacity.
 * revision is monotonic and never reused; state_id follows undo/redo, and dirty
 * compares it with checkpoint_id (plus any open changed transaction).
 */
typedef struct TerraPixelWorkspaceStats {
    uint32_t abi_version, width, height, block_columns, block_rows, active_blocks;
    uint32_t palette_count, used_cells, revision, state_id, checkpoint_id, dirty;
    uint32_t undo_count, redo_count, history_bytes, transaction_open, active_bytes, peak_bytes;
    uint32_t max_bytes, history_budget, transaction_bytes, palette_limit, reserved0, reserved1;
} TerraPixelWorkspaceStats;
typedef struct TerraPixelCell { int32_t x, y; uint32_t index; } TerraPixelCell;
typedef struct TerraPixelPoint { int32_t x, y; } TerraPixelPoint;
typedef struct TerraPixelBlockInfo { uint32_t version, used; } TerraPixelBlockInfo;
/* Candidate flags: bit 0 painted, bit 1 wall; all other bits rejected.
 * Matching flags: bit 0 require unpainted, bit 1 require wall, bit 2 prefer wall,
 * bit 3 require tile, bit 4 require painted. Ties: distance, unpainted, preferred
 * wall, then earliest candidate. Missing match is UINT32_MAX. RGB is exact 24-bit.
 * match_colors accepts <=65536 queries and <=65536 candidates; its temporary KD
 * index is bounded to 512 KiB and released before return. Workspace nearest/read
 * queries remain <=4096 items.
 */
typedef struct TerraPixelCandidate { uint32_t rgb, flags; } TerraPixelCandidate;
uint32_t terra_pixel_workspace_abi_version(void);
/* Limits: dimensions 1..16384; 1..65536 palette entries including zero; max_bytes
 * <= 128 MiB. history_bytes <= max_bytes; zero explicitly disables undo storage.
 * Import into a new unpublished workspace in bounded transactions and close the
 * staging handle on cancellation. New zero blocks need no full baseline copy.
 */
int32_t terra_pixel_workspace_create(uint32_t width, uint32_t height, uint32_t max_bytes,
    uint32_t history_bytes, uint32_t palette_limit, uint32_t* out_handle);
int32_t terra_pixel_workspace_close(uint32_t handle);
int32_t terra_pixel_workspace_stats(uint32_t handle, TerraPixelWorkspaceStats* out);
int32_t terra_pixel_workspace_tx_begin(uint32_t handle);
int32_t terra_pixel_workspace_tx_commit(uint32_t handle);
int32_t terra_pixel_workspace_tx_rollback(uint32_t handle);
/* Palette entries are exact, deduplicated, append-only across committed undo/redo.
 * Palette-only additions outside transactions are display metadata and not dirty.
 * Additions inside a failed/cancelled transaction are removed. A full palette uses
 * exact nearest color (ties earliest), or zero when no opaque color exists.
 */
int32_t terra_pixel_workspace_palette_add(uint32_t handle, uint32_t rgb, uint32_t* out_index);
int32_t terra_pixel_workspace_palette_read(uint32_t handle, uint32_t first, uint32_t count, uint32_t* out);
int32_t terra_pixel_workspace_nearest(uint32_t handle, const uint32_t* rgb, uint32_t count, uint16_t* out);
int32_t terra_pixel_workspace_match_colors(const TerraPixelCandidate* candidates, uint32_t candidate_count,
    const uint32_t* rgb, uint32_t count, uint32_t flags, uint32_t* out);
/* All mutations below require an open transaction. Batches are prevalidated.
 * Cells are [x,y,index] triples. Stroke is the page's Bresenham interpolation with
 * a square brush of 1..64 cells, biased left/up for even sizes and clipped at edges.
 * Stroke endpoints may be outside the board, within [-32768,32768]. A call is
 * limited to 16,777,216 brush-cell visits; larger strokes must be split into batches.
 */
int32_t terra_pixel_workspace_cells(uint32_t handle, const TerraPixelCell* cells, uint32_t count);
int32_t terra_pixel_workspace_stroke(uint32_t handle, const TerraPixelPoint* points, uint32_t count,
    uint32_t index, uint32_t brush_size);
int32_t terra_pixel_workspace_fill(uint32_t handle, int32_t x, int32_t y, uint32_t index);
int32_t terra_pixel_workspace_replace(uint32_t handle, uint32_t from_index, uint32_t to_index);
int32_t terra_pixel_workspace_transform(uint32_t handle, int32_t x, int32_t y,
    uint32_t width, uint32_t height, uint32_t kind);
/* <=65536 pixels per call, explicit byte length/row stride; alpha <16 is skipped,
 * import RGB uses round(v*31/255) then floor(level*255/31). Manual colors never do.
 */
int32_t terra_pixel_workspace_import_rgba(uint32_t handle, int32_t x, int32_t y,
    uint32_t width, uint32_t height, const uint8_t* rgba, uint32_t byte_length, uint32_t stride);
/* Read <=4096 cells. Out-of-board rectangles fail; block reads pad edge cells with
 * zero. Raster level 0..6 produces (64>>level)^2 RGBA pixels using deterministic
 * top-left nearest samples; editing always uses full-resolution indices.
 */
int32_t terra_pixel_workspace_read_rect(uint32_t handle, int32_t x, int32_t y,
    uint32_t width, uint32_t height, uint16_t* out, uint32_t capacity_cells);
int32_t terra_pixel_workspace_read_block(uint32_t handle, uint32_t bx, uint32_t by,
    uint16_t* out, uint32_t capacity_cells);
int32_t terra_pixel_workspace_raster_block(uint32_t handle, uint32_t bx, uint32_t by,
    uint32_t level, uint8_t* out, uint32_t capacity_bytes);
/* Row-major grid slots, <=4096 records per call. Empty blocks retain a version so
 * renderers can invalidate cleared content. Missing/unmodified blocks are {0,0}.
 */
int32_t terra_pixel_workspace_block_versions(uint32_t handle, uint32_t first, uint32_t count,
    TerraPixelBlockInfo* out);
int32_t terra_pixel_workspace_undo(uint32_t handle);
int32_t terra_pixel_workspace_redo(uint32_t handle);
int32_t terra_pixel_workspace_checkpoint(uint32_t handle);
/* Clear undo/redo after staging import, retaining budget, pixels and state IDs. */
int32_t terra_pixel_workspace_clear_history(uint32_t handle);
#ifdef __cplusplus
}
#endif
#endif
