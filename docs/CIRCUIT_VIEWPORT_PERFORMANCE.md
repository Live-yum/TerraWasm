# Circuit viewport performance

The circuit laboratory already calls `terra_circuit_world_command(VIEWPORT)`
for foreground and wall samples. These changes accelerate that existing ABI;
consumers need no new export, command, capability flag, or record decoder.
Both the `all` and `wld` feature sets include the implementation. A consumer
that also needs player APIs must continue using `all`.

## Work removed

The previous foreground reconstruction starts at a 64-column wiring checkpoint.
For every traversed column it expands source RLE records and visits every row
for all four wire colours, including long runs without that colour, followed by
a per-row gate scan. This is much more work than the visible rectangle alone.
The old wall query repeats exactly the same wiring reconstruction. Every seek
also discards the existing immutable-source input window, causing another read
of up to 1 MiB even when a repeated or nearby rectangle uses the same bytes.

The revised implementation makes three internal changes:

1. **Skip unwired RLE runs during routing.** Clear the already allocated column
   output arrays once, keep the previous column's frontier while routing, then
   publish the new frontier together. A colour absent from an entire RLE run
   leaves zero output and breaks its vertical connection without visiting every
   cell. Wired runs still visit every cell in the original colour/row order.
   Junction ports, label allocation, union order, and subsequent FIFO rules are
   unchanged. Gate scanning skips only source runs without gates; with TWLD
   replacements it retains the original per-cell scan.
2. **Reuse the existing source window.** Seeking no longer invalidates the
   session's existing input bytes. The window remains bounded at 1 MiB and is
   tagged with its source ID, so a TWLD read at an equal offset cannot masquerade
   as WLD data. An out-of-window request replaces the window normally.
3. **Read static walls through the validated column index.** File-backed wall
   viewports seek directly to each sampled source column and decode RLE only as
   far as its last requested sample. They do not reconstruct wiring, visit
   intermediate unsampled columns, or expand full-height tile columns. The old
   path remains available for non-streaming worlds.

No world-sized decoded cache, additional viewport cache, worker, thread, SIMD
requirement, or shared memory is introduced. The measured native session
allocation increases by 48 bytes for the source tag and resumable wall reader;
retained buffers and configured Web memory limits do not change.

## State and rendering contract

The reused bytes belong to the immutable input file, not the mutable simulation.
Foreground queries still reconstruct connectivity and apply current device,
lamp, pixel, actuator, and gate state. Pulses, timer ticks, lamp writes, save,
and cancellation retain their existing transaction behavior. Closing the world
releases its input buffer; changing source content requires a new session as
documented in the [circuit world ABI](CIRCUIT_WORLD_ABI_V1.md).

Every result remains column-major (x outer loop, y inner loop), four little-endian
32-bit words per sample, at the caller's original stride. Foreground preserves
tile type/frame, wires, actuator/inactive bits and wall presence. Wall records
preserve ID, paint, invisible-wall and fullbright-wall flags and have no wire
mask. Sampling and the 65,536-sample transport bound are unchanged.

This optimization preserves the information supported by that ABI. Foreground
records do not carry complete paint, slope, liquid, coating or lighting state;
this change does not expand the circuit laboratory into Terraria's entire
renderer. Resource selection, blending and canvas interaction still belong to
the consumer.

## Reproduce and interpret the measurements

Build the base revision and the candidate with the same Emscripten version,
feature set, flags, and memory settings. The recorded comparison uses Emscripten
5.0.7, `all`, its default `-Oz` plus LTO, and the real Web module instantiated in
Node. Web initial/max linear memory remain 16/160 MiB. Follow the normal build
and manifest instructions in [README](../README.md).

Run the same caller-owned map against both build directories:

```sh
node scripts/benchmark-circuit-viewports.mjs MAP.wld \
  --build-dir ../TerraWasm-base/build --output baseline.json --repeat 20
node scripts/benchmark-circuit-viewports.mjs MAP.wld \
  --build-dir build --output candidate.json --repeat 20
```

The script records the exact input hash and dimensions, compiled artifact
identity and SHA-256, result hashes, source read counts/bytes, native step calls,
timings and tracked memory. It writes no map bytes, source filename or extracted
cell records. Each command uses 4,096 native work units. Each result buffer is
hashed before acknowledgement; a fresh heap view is used after native calls
that may grow memory. The workload includes first and repeated foreground
viewports near spawn, horizontal/vertical one-tile pans, repeated and panning
walls, and full-world foreground/wall overviews sampled at stride 64.

Check every corresponding `resultSha256` and network/device/gate count before
comparing speed. Differences in those values invalidate the performance
comparison. Source reads and step counts describe removed work directly; elapsed
times also include the host and JavaScript/Wasm runtime. Repeated queries run in
one session, while a new script process measures another cold open.

The saved [measurement report](benchmarks/circuit-viewports.json) contains three
sequential base/candidate rounds on the supplied 8,400 × 2,400 map. Candidate
artifacts in this diagnostic comparison have an explicit dirty identity at the
base commit. The report identifies the patch separately; release artifacts must
be rebuilt with the final published commit and a clean identity. The diagnostic
artifact must not be deployed.

All eight groups produced identical result SHA-256 values in all three rounds.
The map contains 186,514 wired cells, 11,563 devices, zero gates and 3,667
networks in both builds. Gate behavior is consequently covered by the separate
synthetic regression rather than inferred from this map.

| Workload | Base median (ms) | Candidate median (ms) | Native step calls, base → candidate |
| --- | ---: | ---: | ---: |
| First 64 × 64 foreground | 14.308 | 5.006 | 267 → 37 |
| Repeated foreground | 12.091 | 3.088 | 5,340 → 720 |
| One-tile horizontal foreground pans | 11.185 | 3.467 | 5,910 → 799 |
| One-tile vertical foreground pans | 11.305 | 3.159 | 5,340 → 720 |
| Repeated walls | 14.134 | 0.246 | 5,340 → 80 |
| One-tile horizontal wall pans | 20.243 | 0.227 | 5,910 → 80 |
| Whole-world foreground, stride 64 | 1,297.609 | 484.319 | 25,292 → 4,061 |
| Whole-world walls, stride 64 | 1,147.054 | 8.158 | 25,292 → 18 |

Each displayed time is the median of the three round medians. Step counts cover
the whole group: 20 commands for repeated/panning groups, one command for first
and overview groups. Each repeated/panning group previously read 20 MiB from
the host; all five groups now need zero additional source reads in this input
window. The first foreground still reads 1 MiB. Peak tracked native allocation
is 25,345,260 → 25,345,308 bytes, and observed linear memory stays 32,899,072 bytes
in both builds after compilation.

The shared test host was noisy: the candidate's three single cold foreground
requests took 5.006, 4.420 and 80.283 ms. The report retains all measurements,
including this outlier. These medians are evidence of the algorithm's effect in
this workload, not a worst-case latency guarantee. Open plus compile medians
were 8,301.553 → 1,857.040 ms; that broader measurement also includes source
validation, host scratch copies, JavaScript garbage collection and Wasm runtime
effects, so the deterministic work/read counts are the more portable result.

These are Node-hosted Web Wasm measurements with synchronous in-memory source
reads. They are not measurements of WeChat filesystem latency, worker messaging,
canvas upload/drawing, touch responsiveness, device FPS, or process RSS. The
existing 1 MiB read window can still miss on a large pan, and foreground queries
still replay all intervening columns from the nearest 64-column checkpoint.
Dense all-colour wiring offers less RLE skipping than this ordinary-world map.

## Regression coverage

```sh
node tests/test_circuit_viewport.js
node --test tests/test_circuit_world.js tests/test_circuit_fragments_supports.js
```

The independent fixture encoder emits both compressed and uncompressed versions
of the same world. The new regression covers all four wire colours, long wired
and unwired runs, junctions, ordinary/faulty gate arrangements, live frames after
pulses and 180 timer ticks, lamp writes, and atomic cancellation. It also checks
wall ID 300, wall paint, invisible/fullbright walls, column checkpoints, bottom
edges, non-unit/maximum strides, work quantum 1 versus 4,096, cancelled/retried
queries, source reuse without additional retained allocation, and foreground and
wall coherence after TWLD save. Existing world/fragment contracts continue to
exercise save, routing, devices and supported object semantics.

`build.ps1 -Features all -Test` includes the new regression in the normal WLD
suite. Native CTest and the existing actual-Wasm suites remain the broader
compatibility gates; this optimization does not weaken them or change their
budgets.

The all-profile verification also corrects four stale test files already failing
against base commit `0a9e6dcb3c31148441b22d631c878ea7cf9d5fd2`:
`test_feature_set.js`, `test_plr.js`, `test_plr_real_fixture.js` and
`test_stream_world.js`. The base CMake build already selects `exports.viewer.txt`
and a 16 MiB, filesystem-free Web module for `all`; the tests still expected the
older full Web export surface and 64 MiB initial memory. Tests now verify the
complete declared `all` export list, use exported canonical PLR buffer/field
APIs, count both tracked allocation domains, and compare streaming growth with
the actual manifest initial memory. The Node filesystem roundtrip and standalone
PLR Web filesystem assertions remain. No production export, memory limit, or
size budget is changed to accommodate these test corrections.
