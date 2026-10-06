# Authoritative pixel workspace ABI 1

`include/terra_pixel_workspace.h` is the normative binary layout and function contract. This ABI is additive, available in `all` and `wld` profiles, and independent of the world writer and world-stream ABI. The source implementation does not imply that previously distributed Wasm binaries support it. A consumer must verify the manifest/build identity capability and `terra_pixel_workspace_abi_version() == 1` before selecting it.

## Ownership and transfers

The native workspace owns sparse 64×64 `uint16_t` blocks, exact RGB24 palette entries, transaction baselines, and undo/redo diffs. The handle is a monotonically allocated opaque integer, not a pointer. Stale handles cannot access a replacement workspace. Sixteen simultaneously live workspaces are allowed.

JS retains only dimensions, palette presentation data, viewport/selection state, bounded render caches, and block version/usage metadata. `read_block` returns one copied 8192-byte block; `read_rect` returns at most 4096 cells. No API lends persistent pointers. Callers must reacquire a heap view after allocation-capable Wasm calls and copy query output before another call or await. Caller-owned bridge buffers must be freed separately.

Dimensions are fixed for a workspace and at most 16384 per axis. Resize/reset creates a replacement owner. For an import, create an unpublished staging owner, import bounded strips in transactions, clear its import history, then atomically swap owners. A failure/cancellation closes only the staging handle; the live edited owner remains intact. A zero history budget explicitly disables undo; it does not disable rollback during the current transaction. Newly created blocks have an implicit zero baseline, avoiding a second full zero image.

## Transactions, history and dirty state

`tx_begin` starts one transaction. Cell commands, interpolated strokes, fill, replace, selection transforms and RGBA strips mutate that transaction. Validation, memory, work-limit or history-budget failure in a mutation/commit automatically rolls the entire open transaction back, including earlier commands and palette additions. Read-query errors, nested begin, undo/redo, checkpoint and clear-history state errors leave the existing transaction intact. Explicit rollback performs no allocation.

An existing block is copied only on its first transaction write. Commit builds compact 8-byte cell records containing global cell offset, previous index and next index. A no-op transaction creates no history and preserves its state identity. Oldest history is evicted only after the replacement diff has been safely allocated. An individual diff larger than the configured history budget is rejected and rolled back; it is never silently accepted without undo. `clear_history` intentionally removes all committed undo/redo without changing pixels, palette, state identity, checkpoint or the future history budget.

Undo/redo preallocates any blocks it must restore before changing data. Failure leaves data, history cursor and identity untouched. `revision` is monotonic, including rollback invalidation. `state_id` follows history transitions; compare with `checkpoint_id` for saved-state dirty behavior. Undoing to the save point is clean. Palette-only additions are display metadata and do not make the canvas dirty; committed palette entries remain immutable and may remain present when unused after undo.

## Bounds and allocation accounting

Each workspace accepts a maximum allocation budget, capped at 128 MiB. Its stats include all owned payloads and a conservative 128-byte charge per allocation in addition to local allocation headers. History byte accounting includes the same charge. Transaction scratch, fill worklists, both sides of a growing allocation, snapshots, new blocks and replacement history are reserved against the budget before allocation. Budgets do not describe process RSS, Canvas/GPU memory or Wasm linear-memory capacity.

Palette colors use an exact hash table, not a full-canvas copy or object map. Imported colors additionally use a fixed 128 KiB cache for 32768 quantized colors. The reconstructable cache can survive rollback and is released on close. All persistent allocations use the tracked persistent allocator domain, so unrelated world-native rewinds do not destroy pixel workspaces.

Input limits:

- Cell and RGBA batches: 65536 cells, explicit RGBA byte length and stride
- Stroke: 4096 points; square brush 1–64; endpoints in [-32768,32768]; at most 16777216 brush-cell visits per call. Outside-board visits are clipped exactly after Bresenham interpolation
- Palette reads, nearest-palette queries, rectangle reads and block-metadata queries: 4096 elements
- Material matching: 65536 candidates and 65536 queries; at most 512 KiB temporary index scratch, freed before return

Large fill/replace/transform operations run synchronously within this ABI. They belong in a Worker computation host where available; this source does not claim preemptive cancellation or measured low-end device latency.

## Exact color semantics and matching

Palette index zero is transparent/missing, never opaque RGB black. Manual palette colors keep all 24 bits. Imported alpha below 16 is skipped without erasing the destination. Each imported channel uses `round(value * 31 / 255)` followed by `floor(level * 255 / 31)`, matching the existing JS import writer. A full palette selects exact squared RGB distance, with earliest palette index winning equal distances; no opaque match means skip.

Material candidates are compact `{rgb, flags}` numeric records. The caller resolves any explicit/special mapping before sending remaining colors. Filters support unpainted/painted and wall/tile requirements. Ties use distance, unpainted first, optional wall preference, then original candidate order. RGB is never reduced for manual queries. A three-pass radix grouping removes duplicate color candidates using the same tie rank; an implicit median KD tree serves the full batch with inclusive distance pruning, preserving tied answers. There are no recursive slice allocations or JS object nodes.

## Rendering and invalidation

`block_versions` returns row-major `{version, used}` records, including version tombstones for erased blocks. `read_block` pads partial edge blocks with zero. `raster_block` returns RGBA for one block at level 0–6 (64² down to 1²). Lower levels currently use deterministic top-left nearest samples, not an area filter; thin features may disappear at low zoom. This deliberate sampling rule must match the fallback renderer. All edits and exports use original indices, never reduced display levels.

## Verification

Run `sh scripts/test-pixel-workspace-native.sh`. Run sanitizer coverage with `SANITIZE=1 sh scripts/test-pixel-workspace-native.sh`; under ptrace-based executors LeakSanitizer cannot attach, so use `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1` and retain the contract's explicit allocation-baseline checks.

The contract covers 240 deterministic differential edit transactions, independent flood/stroke/transform references, manual RGB precision, palette nearest ties, all 32768 import color buckets, alpha 15/16 behavior, strided/strip import, palette-cache rollback, filtered KD results versus exhaustive matching, a 65536-candidate/65536-query duplicate-color batch, bounded block raster levels and edge padding, rollback after mid-transaction/commit OOM, atomic undo OOM, history byte eviction, saved identity, stale handles, native rewind isolation and 20 repeated lifecycles.
