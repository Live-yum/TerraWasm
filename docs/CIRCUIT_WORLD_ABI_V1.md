# File-backed circuit world ABI v1

This additive C/Wasm interface compiles a sectioned Terraria `.wld` into a native
wiring graph, executes its gates, queries visible cells, and writes a new `.wld`.
An optional `.twld` supplies named mod tile metadata and preserves those tiles on
save. Inputs, compiler scratch, and output transactions belong to the host. No
world-sized JavaScript array, base64 encoding, Emscripten filesystem, or external
WireHead process is required by the Web build.

The public declarations are in [`terra_circuit_world.h`](../include/terra_circuit_world.h).
This interface is separate from the earlier bounded traversal API documented in
[`CIRCUIT_ABI_V1.md`](CIRCUIT_ABI_V1.md); whole-world compilation does not use its
cell array or impose a wire-count or component-count policy cutoff.

## Capabilities and lifetime

WLD and combined builds expose the `circuit-world` capability, compiled build
identity field `circuitWorldAbiVersion: 1`, and the following manifest group:

```json
{
  "abi": {
    "circuitWorld": {
      "version": 1,
      "fileBacked": true,
      "streamingWld": true,
      "streamingTwld": true,
      "compiledNetworks": true,
      "compactState": true,
      "atomicCommands": true,
      "wallLayer": true
    }
  }
}
```

PLR-only builds omit this group and export no circuit-world functions. Manifest
generation checks the compiled identity and actual exports, rather than adding
the capability to a binary that does not implement it.

Open the source with the [streaming world ABI](STREAM_ABI_V2.md), finish its
validation, and adopt the resulting world handle before beginning a circuit.
The world and immutable source must remain available until the circuit closes.
Known sectioned WLD layouts, including format 326, can be simulated. Legacy and
unrecognized future layouts remain outside the mutable circuit interface; the
ordinary world reader retains its own compatibility behavior.

```c
uint32_t terra_circuit_world_abi_version(void);
int32_t terra_circuit_world_begin(uint32_t world_handle,
    uint32_t scratch_source_id, uint32_t twld_source_id, uint32_t twld_size,
    uint32_t max_bytes, uint32_t* out_handle);
int32_t terra_circuit_world_step(uint32_t handle, uint32_t work_units,
    TerraCircuitWorldEvent* out);
int32_t terra_circuit_world_supply(uint32_t handle, uint32_t source_id,
    uint32_t offset, const uint8_t* data, uint32_t length);
int32_t terra_circuit_world_ack(uint32_t handle);
int32_t terra_circuit_world_command(uint32_t handle,
    const TerraCircuitWorldCommand* command);
int32_t terra_circuit_world_stats(uint32_t handle, TerraCircuitWorldStats* out);
int32_t terra_circuit_world_cancel(uint32_t handle);
int32_t terra_circuit_world_close(uint32_t handle);
```

The new scratch source must be empty, readable, writable, and distinct from both
immutable inputs. The compiler first appends zero-filled bounded chunks before
overwriting its allocated ranges; the host need not support sparse holes.
Scratch is no longer read after compilation reaches READY and may then be
removed. Original WLD/TWLD sources are still needed for viewport reconstruction
and save. Source IDs identify files within the host's operation context; they
are not file descriptors or application-visible paths.

Use zero for both TWLD arguments when there is no sidecar. A source ID is never
inferred from a filename. Close the circuit before its adopted world. Native
allocations are released by `close`, including a cancelled or failed compiler.
Cancelling a command after compilation returns to an idle usable session;
cancelling initial compilation requires closing that partial session.

## Bounded event protocol

All structures use little-endian 32-bit words in Wasm memory. Allocate the entire
structure and any input records in the current module's memory. Reacquire heap
views after native calls that may grow memory; a previous typed-array view may
refer to the detached earlier memory buffer.

`step` requires positive `work_units`. Return value `0` means OK, and `1` means
CONTINUE. **Both are successful statuses**; the event determines the host's next
action. The work quantum bounds internal progress, not total circuit work. The
host should yield to its event loop periodically and may cancel long commands.

The event occupies exactly 48 bytes:

| Word | Field | Meaning |
| --- | --- | --- |
| 0 | `abi_version` | Always 1 |
| 1 | `kind` | MORE 0, READ 1, WRITE 2, RESULT 3, READY 4 |
| 2 | `source_id` | Requested input or staging output source |
| 3 | `offset` | Absolute source byte offset |
| 4 | `length` | Bytes to supply, write, or copy |
| 5 | `data_ptr` | Borrowed Wasm bytes for WRITE/RESULT; zero for READ |
| 6 | `phase` | Internal progress phase; treat as an opaque identifier |
| 7–8 | `completed`, `total` | Progress units, independent of the byte range |
| 9 | `result_kind` | Command producing the result |
| 10 | `result_count` | RESULT record count; final SAVE WLD byte count |
| 11 | `reserved` | Rule flags on non-SAVE READY; final SAVE TWLD byte count |

A READ requests an exact immutable-source or scratch range. Supply exactly its
source ID, offset, and length; partial or mismatched responses are rejected.
Each READ is at most 1 MiB. WRITE borrows its payload until the host completes
that write and acknowledges it. RESULT likewise borrows records until copied
and acknowledged. READ, WRITE, and RESULT repeat unchanged across `step` calls
until fulfilled; calling `step` again does not acknowledge them. MORE requires
another `step`. READY means compilation or the current command has finished.

For non-SAVE READY, `reserved & 1` selects the native Computerraria pixel-wave
compatibility described below. For SAVE, the entire field is the output TWLD
length; it must not be interpreted as rule flags. Cache the rule flag from
compilation or a non-SAVE command.

Status codes are `INVALID -1`, `HANDLE -2`, `STATE -3`, `MEMORY -4`, `FORMAT -5`,
`IO -6`, and `UNSUPPORTED -7`. Host I/O failures require cancelling the native
operation and aborting its staging files. Never acknowledge a write that failed.

## Commands and record layouts

A command occupies exactly 64 bytes:

| Words | Fields |
| --- | --- |
| 0–3 | `abi_version`, `kind`, `x`, `y` |
| 4–7 | `width`, `height`, `stride`, `mask` |
| 8–11 | `count`, `data_ptr`, `data_count`, `source_id` |
| 12–15 | `flags`, `aux_source_id`, `reserved1`, `reserved2` |

Set `abi_version = 1`, reserved words to zero, and unused fields to zero. Only
one command may be active per session. Coordinates are tile positions with an
exclusive upper rectangle edge. Invalid rectangles are rejected before execution.

### 1: VIEWPORT

Supply `x`, `y`, `width`, `height`, and `stride` (zero means one). The result is
a bounded sample, not a full export of world state. Use no more than 65,536
samples in one command, so its result remains at most 1 MiB. This transport
bound does not constrain world dimensions or compiled graph capacity.

Each foreground sample is four words:

```text
[x, y, tileType | flags << 16 | wireMask << 24,
 (frameX & 65535) | (frameY & 65535) << 16]
```

Foreground flags are active `1`, actuator `2`, inactive `4`, named custom color
pixel `8`, and wall present `16`. Wires are red `1`, blue `2`, green `4`, yellow
`8`. Frame halves are signed 16-bit tile frame values. Internal tile type
`0xfffe` represents restored ColorPixelBox only within this API; it is never
written as a vanilla WLD tile ID. The host can render real PNG resources using
the original type and frame, and retain color fallbacks for other world tiles.

Set `flags = 2` to request the separate wall layer. Its sample is
`[x, y, wallId | flags << 16, wallPaint]`, with active `1`, wall layer `32`,
invisible wall `64`, and fullbright wall `128`. The wire mask is zero. Wall
sampling reads the original wall ID and paint, and does not change simulation
state. The host may cache the static wall layer separately from animated wiring.

### 2: TRIGGER

With `flags = 0`, pulse the given rectangle and selected wire `mask`. This is a
physical TripWire input, including an empty tile containing wire. With
`flags = 1`, perform HitSwitch interaction: normalize the clicked part of a
multi-tile switch, update its frame or timer registration, and use the original
object's trigger rectangle. A timer interaction changes its enabled state and
does not immediately emit a wire pulse. Non-interactive tiles are a no-op in
interaction mode; the explicit pulse operation is available separately.

`count = 0` defaults to one pulse. Larger values execute that many **independent**
TripWire transactions inside one atomic command. They are not merged into one
deduplicated network hit. This reduces the host crossings for a clock without
changing gate-wave boundaries.

### 3: TICKS

Advance the mechanical scheduler by `count` game ticks at 60 Hz. Timer styles
use the original 60, 180, 300, 30, and 15 tick periods. Wired timer toggles and
momentary-button releases are scheduled by the native state, including their
registration order and remaining phase. One tick is not one CPU instruction;
the physical CPU may take multiple independent clock inputs per instruction.
This scheduler does not run Terraria's NPC, projectile, liquid, or player loop.

### 4 and 5: READ_LAMPS / WRITE_LAMPS

Supply up to 65,536 four-word records through `data_ptr` and `data_count`:
`[x, y, state, 0]`. Input bytes are copied before the call returns. READ returns
`[x, y, on, tileType]` in the original request order. WRITE requires an ordinary
logic lamp at every coordinate and binary state values. The whole batch is
validated and its storage reserved before the first state mutation.

Writing a lamp changes its stored bias relative to current network parity; it
does not emit a pulse or evaluate gates. This provides a physical ROM/RAM loading
interface without adding a processor interpreter to the runtime. The production
kernel has no Computerraria memory addresses, register file, opcode decoder, or
special implementation of display memory writes.

### 6: SAVE

Provide a new writable staging `source_id` for the WLD. If a TWLD was attached,
also provide a distinct `aux_source_id` for the sidecar. Both must be distinct
from both original inputs and compiler scratch. The host must abort incomplete
files on cancellation or failure, verify the final sizes, and publish the pair
only after both have succeeded. Native success does not itself rename files or
commit a platform transaction.

The writer streams the original prefix, rewrites tiles using the existing WLD
RLE encoder, streams original suffix sections, and patches section offsets.
Chests, entities, unknown non-tile sections, and other unrelated source content
are retained. Tile frames reflect the current committed circuit state. Source
load-time rules, such as disabled timer frames, retain Terraria's interpretation.
The original input is never overwritten and remains the session's immutable
baseline for subsequent saves.

ColorPixelBox uses the vanilla fallback tile in the WLD and its current frame
in TWLD. The sidecar is streamed through inflate, fixed-size frame patching,
and deflate. Unknown NBT tags and unrelated mod data are preserved as raw
uncompressed bytes. Recompression may change gzip bytes and length. An interrupted
save can be abandoned and a new save started from the same committed state.

The final READY has `result_kind = 6`, `result_count = WLD byte length`, and
`reserved = TWLD byte length` (zero without a sidecar). These are real WLD/TWLD
files; neither a gzip stream renamed `.wld` nor the old TL format is produced.

## State, cancellation, and memory

The 96-byte statistics structure contains 24 words in this order:

```text
abi, state, width, height, spawnX, spawnY,
minX, minY, maxX, maxY, wireCells, devices,
gates, networks, compiledColumns, phase, activeBytes, peakBytes,
ticksLow, ticksHigh, netPulsesLow, netPulsesHigh, gatesFiredLow, gatesFiredHigh
```

The three counter pairs are unsigned 64-bit values. Circuit bounds and counts
come from the imported world; the device count is an inventory statistic, not
a promise to simulate every entity's game behavior. Do not mistake network
pulses or gate firings for retired processor instructions.

`max_bytes` is a caller-provided native session memory budget. Zero permits the
available 32-bit address space, but the build and platform can have a tighter
memory ceiling. It is independent of source file length: a 405-MB disk file can
be streamed through a Wasm heap smaller than the input. Allocation checks cover
the compiled graph, VM, codec, command checkpoints, and output buffers. The host
also needs room for linear-memory allocator overhead, JS, decoded textures,
canvas surfaces, source windows, and platform file buffers. Disk quota remains
a real platform constraint, particularly when writing a second complete world.

Before a TRIGGER or TICKS command first changes state, the runtime reserves a
checkpoint. A failed checkpoint allocation has no circuit side effects. Cancel
or failure restores network parity, physical gate frames, pixels, lamp biases,
timer states and deadlines, mechanical registrations, RNG state, tick count,
and counters to the beginning of the entire command, even after several inner
pulses have completed. A successful command commits all of them together.
Read-only sampling and saving do not advance the circuit. Partial save output
is discarded by the host rather than applied to the original world.

There is no 100,000-wire or 20,000-component limit. Real limits are the caller's
memory budget, supported WLD address/layout ranges, available storage, and the
amount of work the user chooses to run before cancelling.

## Compiler and evaluation rules

The compiler sweeps WLD columns with a small connectivity frontier. It resolves
provisional labels, preserves the four wire colors, and handles junction lanes
without retaining a two-dimensional object for every world cell. A delta-coded
label map and column checkpoints let later viewport or save operations replay
only the necessary region. Large intermediate rule and physical-membership
streams use host scratch through bounded random-access windows.

Most Computerraria gates have one ordinary lamp below a faulty lamp. Their
condition is the XOR of their initial lamp bias and a few network parity bits.
The compiler stores compact bytecode and interns identical complete network
rule lists, while retaining each gate's physical identity separately. Other
ordinary/faulty stacks use the general evaluator. A second faulty lamp above
the fast pattern forces that general path, so each source lamp remains effective.
This is graph compression; it does not recognize CPU opcodes or replace RAM
with a host-owned array.

The native VM processes all selected colors before checking the candidate
lamps. It evaluates gate waves after prior wire effects, gives each emitted gate
its own TripWire, and preserves repeated network hits from different gates in
the same wave. A physical gate can emit only once within the outer transaction;
feedback attempts produce the original done/smoke behavior. Pending candidates
are evaluated lazily from the latest parity, rather than retaining stale booleans
from when their wires were first hit.

Initial seed tiles use SkipWire: an ordinary seed lamp does not toggle, a faulty
seed lamp does not enqueue its gate, and an emitting gate does not actuate
itself. Traversal does not enter the outer two-cell world margin, but a seed in
that margin may emit into an adjacent interior tile. Actuators use source-derived
solid/material, temple progression, protected wall, tree, furniture, boulder,
and teleporter support predicates, with sparse adjacent-column reads where
necessary. An unavailable malformed support neighborhood is an explicit error.

### Vanilla and Computerraria pixel profiles

Original Terraria applies PixelBox intersections after each individual TripWire.
The pinned WireHead implementation used to build Computerraria applies them
after a whole gate wave, uses cross-color pairing for normal monochrome pixels,
and pairs same-color horizontal/vertical networks for ColorPixelBox. Repeated
hits contribute by parity, and identical H/V networks are not a two-net pair.

The native importer selects the compatibility profile only when TWLD metadata
identifies an actual tile as mod `WireHead`, name `ColorPixelBox`. Saved numeric
IDs and filenames do not select it. Worlds without that named tile retain
vanilla per-TripWire behavior. Both behaviors are implemented in C; installing
WireHead, tModLoader, or a JavaScript display emulator is unnecessary.

### Explicit compatibility boundaries

The compact compiler supplies each network's candidates in stable physical
order. It does not reproduce the original breadth-first lamp visit order from
every possible input coordinate. Although faulty gates use the original
probability calculation and UnifiedRandom arithmetic, multiple probabilistic
gates on one network can receive successive samples in a different order.
The game also does not store its global `Main.rand` state in WLD. Therefore this
API does not promise frame-for-frame replay of arbitrary stochastic gameplay;
the deterministic CPU and display acceptance does not remove that boundary.

The implemented behavior covers wiring, gates and lamps, junctions, pixel boxes,
switch interactions, mechanical timers, momentary buttons, selected wired light
and block state changes, and actuation eligibility. World inventory/resources
can be displayed independently. A full player/NPC/projectile simulation,
automatic dummy/hoik motion, liquid transfer, item drops, destructible-support
physics, and arbitrary mod code are outside this circuit engine. In particular,
the physical clock wire can drive Computerraria without pretending that one
mechanical tick implements its original NPC clock generator.

## Source evidence and verification

Rules were checked against the user-pinned
[Terraria Wiring.cs](https://github.com/Live-yum/TerrariaDecompiledSource/blob/8255d34616c780af12079425ac92a0a7aed87d71/Terraria/Wiring.cs),
[WorldGen.cs](https://github.com/Live-yum/TerrariaDecompiledSource/blob/8255d34616c780af12079425ac92a0a7aed87d71/Terraria/WorldGen.cs),
[WorldFile.cs](https://github.com/Live-yum/TerrariaDecompiledSource/blob/8255d34616c780af12079425ac92a0a7aed87d71/Terraria.IO/WorldFile.cs),
and the published WireHead
[WiringWrapper.cs](https://github.com/misprit7/WireHead/blob/e6009d010ca54ff43d04b44697accc7115807b9c/WiringWrapper.cs)
and [Accelerator.cs](https://github.com/misprit7/WireHead/blob/e6009d010ca54ff43d04b44697accc7115807b9c/Accelerator.cs).
The material table is generated from pinned source-derived inputs, whose hashes
are recorded in the generated header.

`tests/test_circuit_world.js` independently encodes small WLD fixtures and uses
the actual Web module to exercise all six ordinary truth tables, faulty-source
identity, seed suppression, five timer periods, multi-pulse/tick rollback,
normal pixel boundaries, direct junction seeds, world margins, material and
wide support rules, multi-tile interactions, walls, and save/reopen.

Native contracts cover VM scheduling and callback rollback, whole-batch
checkpoints, allocation failures, sparse actuation predicates, one-byte gzip
fragments, sidecar metadata ordering, unknown tags, lossless frame patching,
changed/truncated source rejection, and interrupted replay recovery.

The actual full-world integration test is described in
[`tests/computerraria/README.md`](../tests/computerraria/README.md). It checks
the published 405,983,441-byte world and its exact sidecar hashes, compiles
72,939,714 wired cells and 13,641,575 gates, loads a physical ROM program, and
compares 48 physical RAM signatures across 38 RV32I instruction kinds. It also
changes one actual ROM bit as a negative control, executes real display stores,
saves both files, reimports the complete world, checks both displays, and reruns
the program. The production runtime contains no instruction decoder.

The published CPU's ECALL/EBREAK behavior is tested as an architectural boundary;
this acceptance is not full RISC-V certification. Run reports include the tested
Wasm hash, counts, memory, and saved-file hashes. A fixture test reported as
skipped because the full inputs are absent is not a completed CPU acceptance.
