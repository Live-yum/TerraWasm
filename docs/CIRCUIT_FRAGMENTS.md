# Circuit fragments and sparse world overlays

The existing circuit command/event structures and exported functions are
unchanged. Check `manifest.abi.circuitWorld.fragments` and
`manifest.abi.stream.stampTiles` before using these commands. Source builds add
these claims only after probing the compiled stream identity; an old artifact
must not be relabelled with these capabilities.

## Enumerating and extracting

Command **7 (FRAGMENTS)** takes `x = page offset`, `count = page size` (1–32768).
Its first invocation can supply at most 65536 geometry records through the
existing `data_ptr`/`data_count` fields. Each geometry record is four u32 words:

`[tileId, uint16(frameX) | uint16(frameY)<<16, dx | dy<<8 | width<<16 | height<<24, 0]`

Expand each verified `TileObjectData` layout to one record per occupied cell.
Coordinates are raw game frame coordinates, never atlas positions. Layouts are
caller supplied because the target game version, alternates, and runtime frame
states determine their meaning. The reference game source is
`Live-yum/TerrariaDecompiledSource`, commit
`8255d34616c780af12079425ac92a0a7aed87d71`,
`Terraria.ObjectData/TileObjectData.cs` (SHA-256
`348b09f93e78a2e704a137fc39e871bb7da5e5ad89519e9f1c0a2705cc5cf345`).
Conflicting layouts for the same type/frame are ambiguous, never guessed.
The copied geometry survives a cancelled enumeration; once indexed, later page
requests omit it. A different geometry set requires a new circuit session.

Two cooperative column replays reuse the existing compiled network identities.
Electrical color/directional networks remain distinct. A second, independent
grouping joins networks touching one complete object, plus logic lamp/gate
stacks. It does not flood adjacent terrain. Retained auxiliary storage scales
with network/object counts and one column; it is charged to the circuit memory
budget. It is not a second world tile grid.

FRAGMENTS emits 32-byte records, eight little-endian u32 words:

`[id, x, y, width, height, cellCount, wireCellCount, flags]`

Flags: **1** incomplete/ambiguous object geometry; **2** an object needs a
chest/sign/tile-entity section; **4** a mod tile needs its companion data. Flags
1 and 4 prevent a complete supported world round trip. Flag 2 requires the
object companion described below; tile records alone cannot preserve it.
RESULT `result_count` counts this page; READY `result_count` is the total number
of descriptors. IDs remain stable for this compiled world, but need not be
consecutive. Empty worlds return a valid empty page.

Command **8 (EXTRACT)** takes `mask = fragment ID`, `count = cell budget`
(1–32768). It rejects oversized fragments before returning any partial data.
The output is sparse, in absolute world x/y order, and includes whole recognized
objects. Unrelated cells inside a fragment's bounding rectangle are absent.
At a shared wire coordinate, only colors belonging to this fragment are copied.
The current simulation state is resolved before encoding each tile.

The 32-byte cell record is shared with the overlay writer:

| Word | Value |
| --- | --- |
| 0–1 | x, y |
| 2 | tile type in low 16 bits; flags in high 16 bits |
| 3 | signed 16-bit frame X and frame Y |
| 4 | wall ID, tile paint at bit 16, wall paint at bit 24 |
| 5 | liquid amount, liquid type at bit 8, brick style at bit 16, wire mask at bit 24 |
| 6–7 | reserved zero |

Tile flag bits 0–6 are active, actuator, inactive, invisible block, invisible
wall, fullbright block, fullbright wall. Wire bits are red, blue, green, yellow.
READY `result_count` is the number of extracted records. Standard borrowed
RESULT acknowledgement, source reads, cancellation, and close ownership apply.

## Writing an overlay

Call `terra_world_stream_operation_begin` with operation **`stamp_tiles`** and:

```json
{"x":100,"y":200,"width":20,"height":10,"recordCount":50,"recordSourceId":9,"mode":"overlay"}
```

The immutable source named by `recordSourceId` contains exactly `recordCount`
32-byte cell records. Their x/y values are local to the declared rectangle,
strictly ordered by x then y, with no duplicate coordinates. The source ID must
be distinct from the original world. The limit is 1048576 records; native
staging remains 64 KiB rather than retaining that entire payload. Both ordinary
`supply_source` and ABI v2 input leases support this additional source.

The writer validates records before output, then replays bounded windows while
performing one ordinary world tile scan. It rechecks consumed records and
splits source RLE runs only where necessary. Overlay semantics are:

- Merge each supplied wire color with the existing wires.
- An active source tile supplies its full foreground attributes. A wire-only
  source preserves the target foreground. A supplied actuator can attach to it.
- A nonzero source wall replaces wall attributes; absent walls preserve them.
- A nonzero source liquid amount replaces the target liquid; absent liquid
  preserves it. This operation does not erase unspecified cells or layers.

New doors, statues, and other complete framed objects can be placed over empty
space or ordinary terrain. A foreground replacement with a different type or
frame cannot overwrite an existing framed object, even partially. Overlapping
an identical tile/frame or adding wires is safe. Source object completeness is
the verified fragment/editor encoder's responsibility; this layer does not
invent missing object cells from a bounding rectangle.

Without an object companion, these foreground sources are rejected:
21/467 containers, 88 dressers, 55/85/425/573 signs/gravestones/announcement
boxes, and tile entities 378/395/423/470/471/475/520/597/698/723/724. The last
three are the Dead Cells display jar, kite anchor, and critter anchor; their
displayed item and anchored entity state cannot be reconstructed from tiles.
The complete list is checked against `TileID.Sets.IsAContainer`, `Main.tileSign`,
and every registration in [TileEntitiesManager.RegisterAll](https://github.com/Live-yum/TerrariaDecompiledSource/blob/8255d34616c780af12079425ac92a0a7aed87d71/Terraria.DataStructures/TileEntitiesManager.cs)
and its entity-specific tile validation at that pinned game source. The old
frame-encoded weapon rack (334) is not mistaken for the tile-entity weapon rack
(471). The bounded companion protocol below carries the associated records.
The host must not apply this WLD-only writer to a TWLD companion transaction.

All other sections and unselected tile values are preserved. The result remains
an unadopted candidate until the existing host verification/publish protocol
completes. Failure/cancellation discards it and leaves the active world intact.
After adoption, discard any circuit compiled from the previous tile topology
and compile the new immutable world before simulating it.

## Object companions (COB1)

This is an internal transfer payload, not a new world or circuit file format.
It preserves raw section encodings without the length-limited display JSON.
Require numeric `manifest.abi.circuitWorld.fragmentObjects === 1` and
`manifest.abi.stream.stampObjects === 1`. The compiled build identity must agree
through `circuitWorldFragmentObjects` and `stream.stampObjects`; changing a
manifest cannot enable the feature in older binaries. Existing exports,
command/event sizes, and stream ABI versions remain unchanged.

Command 8 with `flags = 1` takes `aux_source_id` as a distinct writable output,
`width` as the byte budget (32–4194304), and `height` as the object budget
(1–32768). The source must differ from world, scratch, and TWLD sources. After
validating the whole selected payload, native emits sequential WRITE events
from offset zero in blocks up to 65536 bytes. The first block includes the
complete 32-byte header; there are no backpatches. The original cell RESULT and
READY follow the companion. The host must only publish both after success.

All words are little-endian u32; there is no padding between item payloads.

| Global header word | Value |
| --- | --- |
| 0–1 | `0x31424f43` (COB1), schema version 1 |
| 2–4 | source WLD version, object count, total bytes including header |
| 5–7 | fragment origin x, fragment origin y, reserved zero |

| Item header word | Value |
| --- | --- |
| 0–1 | section (2 chest, 3 sign, 5 tile entity), entity type (zero for chest/sign) |
| 2–4 | anchor x/y relative to fragment origin, tile type |
| 5–7 | payload byte length, reserved zero, reserved zero |

Each header is followed immediately by its complete payload. Chest payloads
contain the original 7-bit length-prefixed UTF-8 name, u32 slot count, and every
slot (i16 stack; positive stacks also have i32 item type and u8 prefix). Signs
contain their full length-prefixed UTF-8 text. Empty strings are valid; embedded
NUL and long text are copied verbatim. Entity payloads contain only saved extra
data; original IDs and coordinates are replaced at destination.

| Entity type | Tile | Extra payload in WLD 326 |
| --- | --- | --- |
| 0 | 378 training dummy | i16 runtime NPC slot |
| 1 | 395 item frame | i16 item type, u8 prefix, i16 stack |
| 2 | 423 logic sensor | u8 logic check (0–7), u8 saved On (0–1) |
| 3 | 470 display doll | two inventory masks, pose byte, extra mask; populated items |
| 4 | 471 weapon rack | item record (5 bytes) |
| 5 | 475 hat rack | mask byte; populated items |
| 6 | 520 food platter | item record (5 bytes) |
| 7 | 597 teleportation pylon | empty (zero bytes) |
| 8 | 698 Dead Cells display jar | item record (5 bytes) |
| 9 | 723 kite anchor | i16 item type |
| 10 | 724 critter anchor | i16 item type |

All 18 tile kinds use their verified complete root/occupancy geometry. A source
object with missing or duplicate section metadata fails extraction; it never
becomes an empty container implicitly. An editor may explicitly construct a
new empty container, a sign with its text, or a new sensor with a valid payload.
Saved sensor state is preserved rather than inferred from simulation feedback.
Training dummy NPC slots are reset to -1 when written: the source world's
runtime index is not valid in a different world. The destination game recreates
the dummy NPC. Leashed anchors retain their saved item, which Terraria uses to
recreate the kite or critter on loading; runtime motion is not stored in WLD.

Add `objectSourceId`, `objectBytes`, and `objectCount` to `stamp_tiles` together.
The immutable object source must differ from world and tile-record sources.
Native reads 64 KiB windows into one selected-payload buffer capped at 4 MiB,
then validates exact sizes, kinds, anchors, frame offsets, complete foreground
coverage, and unique/nonoverlapping objects. No complete world grid enters JS.
Metadata scans use the existing bounded metadata image. The candidate rebuild
uses the effective sections, including earlier queued overrides, and keeps
untouched original records byte-for-byte. The temporary compact image is freed
before allocating the final image, whose existing limit remains 16 MiB.

Nonempty object bundles currently require **326 → 326**. Older chest and entity
layouts have version-specific differences (including display doll order in
311), so unsupported conversions fail explicitly. An empty bundle is valid
for older tile-only worlds and imposes no new version restriction.

Any existing target object metadata anchor inside the incoming footprint,
including an orphan record, is a conflict. An incoming object cannot replace
an active framed target, even with the same frame, so inventories cannot be
overwritten. Chest/sign capacity and entity ID overflow are checked. Failures
before or during writing leave the original world unchanged; partial output
must be discarded. A failed stamp is terminal and can only be cancelled or
closed, preventing repeated-step allocation leaks or an OOM retry on a released
candidate. Cancellation and close release the selected companion/index.

## Regression coverage

`tests/circuit_fragments_contract.inc` runs in the existing native stream job,
including its UBSan variant; no production test exports are added.
`tests/test_circuit_world.js` runs the same public commands against the real Web
Wasm in the existing combined/WLD builds. Synthetic independent WLD encoders
cover color crossings, complete frames, gate stacks, pagination, malformed
geometry, resource bounds, metadata byte preservation, multiple stamp windows,
lease ownership, rejection, cancellation and complete release. The additional
`tests/circuit_objects_contract.inc` uses independently encoded public 326
fixtures for all 18 objects and 11 entities, 200-slot inventories, long UTF-8/NUL
text, complete raw payload equality, metadata overrides, orphan/duplicate target
conflicts, malformed packets, partial objects, old-version rejection, empty
legacy companions, cancellation during side output/input, repeated failed steps,
and heap return to baseline. The sanitizer job also runs the stream harness
under UBSan; its Wasm32 pointer layout requires low native addresses.
The real-Wasm tests exercise the same public commands without production test
exports or private application sources.
