# Stream ABI v2: bounded ownership, cursors, and edit plans

This is an additive source/API change. The world ABI, stream event, and pixel
specification remain version 1. Existing `supply_source` callers still work.
The viewer must validate the compiled source identity and generated artifact
manifest before enabling a new path; exported function presence alone is not a
compatibility check.

## Build identity and artifacts

A WLD-capable build reports this object as `build_info.stream`; the manifest
producer copies the probed value to `manifest.abi.stream` and requires it to match
between Node and Web, the native version query, and the export list:

```json
{"version":2,"inputLease":true,"editPlan":true,"pngColumnCursors":true,"stampTiles":true}
```

`terra_world_stream_abi_version()` returns 2. All four WLD export profiles include
this query, acquire/commit/release, and `get_stats`. PLR-only profiles do not.

No compiled `.wasm`, wrapper, or generated artifact manifest is included in this
change. Build from the intended clean commit, run the existing `build.ps1`
workflow, and regenerate `terra.manifest.json` from both resulting modules. Do
not edit source-commit strings or copy this declaration into an older manifest.
The source-only manifest tests use explicit synthetic objects, never publishable
artifacts.

## Input lease (eight little-endian u32 words)

`acquire_input(task, descriptor_ptr)` returns status 0 and writes:

1. `abi_version` = 2
2. `lease_id`
3. `source_id`
4. `offset`
5. `length`
6. `data_ptr`
7. `capacity` (currently exactly `length`, always <= 1 MiB)
8. `memory_bytes` (Wasm linear memory capacity; native host contract uses 0)

Allocate the 32-byte bridge descriptor before acquiring. A lease exists only for
an outstanding NEED_SOURCE event. At most one lease is outstanding. Read source
bytes first, then acquire, obtain a fresh `HEAPU8`, copy exactly the requested
range to `data_ptr`, and immediately call:

`commit_input(task, lease_id, source_id, offset, length)`

Do not invoke unrelated Wasm APIs, allocate, grow memory, or let another task
write the borrowed window between filling it and commit. Do not retain the view
past commit/release/cancel/close. Wasm memory growth invalidates the lease;
commit returns `TERRAX_STREAM_LEASE_GROWTH`, leaves NEED_SOURCE pending, and
requires a new acquisition, fresh view, and refill. It never blesses bytes from
a detached or reused JS view.

Task, lease, source, exact offset, and exact length must all match. A wrong-range
commit leaves the lease pending. Successful commit and explicit release consume
the lease token; repeated/stale commits fail. `release_input(task, lease_id)`
abandons the window, leaving the source event pending. Cancellation/closure
invalidate its lifetime. Caller-owned source IDs continue to designate immutable
source versions; this API does not hash every physical read or prevent a caller
from modifying a file in violation of that contract.

Input is filled directly into the native consumer window, metadata destination,
or the current bounded column block. No bridge-to-native input `memcpy` occurs
in the lease path. Copy-based `supply_source` is rejected while a lease is held
and remains supported otherwise. Output still uses the existing acknowledged
immutable-until-ack window; this change does not authorize asynchronous consumers
to retain a Wasm view across growth/reuse.

## PNG algorithm and bounds

Full-resolution base color PNGs no longer allocate `width * height * 3` RGB.
Each column retains a byte cursor, y position, pending tile/RLE remainder, and a
bounded source read-ahead block. The total block budget is at most 2 MiB (at most
4096 bytes per column); cursor metadata is additional and reported separately in
`cache_bytes`. Short neighboring columns are coalesced through the existing
1 MiB source window and each complete short column is copied once into its
already-budgeted private cache slot, so aggregate short-column data exceeding
1 MiB does not cause window thrashing. This retention copy is reported as
cache_copy_bytes, independently of input_copy_bytes. Long columns retain unread bytes across horizontal strips.

A verified indexed source without tile markers bypasses the old preparation
scan. Each tile record is decoded once regardless of strip count. Buffered
sources and marker-location collection can still require a preliminary scan.
Legacy span markers use a separate monotonically advancing per-column
checkpoint and only replay the strip's radius overlap. Their original full-run
coordinates and painting order are preserved, including transparency. Overlap
work depends on radius/strip height; it is measured rather than claimed to be
one decode per record. Marker-region precomputation is not removed.

The PNG encoder still holds bounded 256 KiB raw and 512 KiB output allocations.
Its strip initialization/compression remain bounded synchronous operations;
low-end device P95/max step timing, filesystem latency, and cancellation delay
still require browser/WeChat measurement. A low read-byte ratio does not imply
low read-call latency: the noise fixture has many small reads.

## Verified unchanged tiles and ordered plans

For save/header/chest/bestiary-only edits on an immutable source whose complete
column index was validated during open, the tile range is copied without
re-decoding/re-encoding. The format version, dimensions, tile-type count and
frame-important bitmap must remain identical. Header/footer/section offsets are
rebuilt; unsupported format changes use the existing scanning path.

`operation_begin(world, "edit_plan", request, out_task)` accepts:

```json
{"operations":[
  {"operation":"header_patch","request":{"patch":{"worldName":"Updated"}}},
  {"operation":"batch_update_tiles","request":{"rules":[{"where":{"type":1},"patch":{"type":2}}]}},
  {"operation":"save","request":{}}
]}
```

Bounds: 1 MiB request, 1–128 operations, up to 1024 point-local rules total.
Supported metadata commands are header_patch, replace_chests, replace_bestiary,
and save, subject to their existing version validation. Plans cannot resize or
reinterpret tile format. Future versions remain read-only, including rejecting
edit plans. Original-byte future save retains its existing separate operation.

Point-local tile groups are evaluated in original operation order. Each group
has its own original-tile matching semantics; an encode/decode normalization
between groups preserves fields that a separate saved WLD would discard. A
plan produces one candidate, not an intermediate candidate per operation.
Environment predicates, terrain themes and limited rules can depend on a whole
intermediate world or RLE grouping, and are rejected rather than incorrectly
fused. Use separate isolated transactions for such dependencies. Unknown or
unsupported commands fail during preparation before any output is published.
No arbitrary callback is captured and no query response is fabricated.

## Metrics and reproducible verification

`get_stats(task, descriptor)` writes 14 u32 words (56 bytes): ABI version,
source requests, source bytes, decoded tile bytes, tile records, logical tile
passes, input copy bytes, unchanged tile copy bytes, output bytes, cache hits,
cache misses, cache bytes, candidate count, and internal cache retention-copy bytes. Counters saturate at UINT32_MAX.
Output bytes include the final header patch; source bytes include metadata reads
where applicable. Logical tile passes are traversal starts, not a claim that
marker overlap never decodes a record again; inspect tile_records and decoded
bytes as well. These are application counters, not process/device memory or
platform-cache metrics.

Using already installed GCC, Node and zlib on Linux:

```sh
python3 scripts/test-stream-native.py
python3 scripts/test-stream-native.py --ubsan
node --test tests/test_stream_abi_manifest.mjs
```

The synthetic fixture generator follows an independent v139 WLD header layout;
no game assemblies or third-party binary resources are added. It covers high
RLE, low RLE/noise, stripe-crossing markers, PNG CRC/inflate/pixel correctness,
zero input-copy leases, wrong owner/range/replay/replacement/growth, allocation
ceilings/failures, truncated candidates, metadata byte-copy, ordered mixed
plans versus sequential adopted transactions, unsupported plan rollback, and
native allocation baselines. The compiled Wasm lease test in build.ps1 checks
real linear-memory growth rather than the native test hook.

Observed synthetic GCC counters (not device benchmarks):

- 8400 × 2400 high RLE: 8400 decoded records, 33600 source/tile bytes, 1 request
- 8400 × 257 noise: 2158800 records, 4838400 source bytes for 4317600 tile bytes
  (about 1.12×), 25200 source requests
- 8400 × 2400 short-column aggregate: 504000 records, 1512076 source bytes
  for 1512000 tile bytes, 2 requests; 1512000 internal cache-retention copy bytes
- All three: 2494800 bytes of source cache plus cursor metadata; no full RGB cache;
  all task allocations individually constrained to at most 3 MiB

The larger source fixture only exists in the local generated test directory.
Native source contracts are not a substitute for the existing real-world and
real-Wasm regression suite or Android/iOS testing.
