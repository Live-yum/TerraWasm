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
chest/sign/tile-entity section; **4** a mod tile needs its companion data. A
caller must not advertise a safe complete world round trip for these flags.
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

Objects requiring additional sections are rejected as foreground sources:
21/467 containers, 88 dressers, 55/85/425/573 signs/gravestones/announcement
boxes, and tile entities 378/395/423/470/471/475/520/597. These IDs are pinned
game `TileID` facts; the old frame-encoded weapon rack (334) is not mistaken for
the tile-entity weapon rack (471). A future payload must explicitly carry and
relocate associated section records before lifting this restriction. Likewise,
the host must not apply this WLD-only writer to a TWLD companion transaction.

All other sections and unselected tile values are preserved. The result remains
an unadopted candidate until the existing host verification/publish protocol
completes. Failure/cancellation discards it and leaves the active world intact.
After adoption, discard any circuit compiled from the previous tile topology
and compile the new immutable world before simulating it.

## Regression coverage

`tests/circuit_fragments_contract.inc` runs in the existing native stream job,
including its UBSan variant; no production test exports are added.
`tests/test_circuit_world.js` runs the same public commands against the real Web
Wasm in the existing combined/WLD builds. Synthetic independent WLD encoders
cover color crossings, complete frames, gate stacks, pagination, malformed
geometry, resource bounds, metadata byte preservation, multiple stamp windows,
lease ownership, rejection, cancellation and complete release.
