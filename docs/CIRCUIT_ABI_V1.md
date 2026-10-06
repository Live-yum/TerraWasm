# Circuit traversal ABI v1

Source semantics: Terraria `Wiring.HitWire` at
[`8255d34616c780af12079425ac92a0a7aed87d71`](https://github.com/Live-yum/TerrariaDecompiledSource/blob/8255d34616c780af12079425ac92a0a7aed87d71/Terraria/Wiring.cs).

This module owns persistent sparse wire topology and the expensive FIFO traversal.
The host retains `HitWireSingle`, skip sets, gate waves, mechanics and atomic rollback.
It must run four independent colour passes in red, blue, green, yellow order and
finish pixel boxes / gates at the original boundary. This is not a complete game
simulation or a substitute for the host's device rules.

## Host protocol

All records below use 32-bit little-endian fields, no platform pointers in records.
Pointers passed to functions are caller-owned Wasm buffers. No borrowed outputs.
`create(width,height,max_cells,max_bytes,out_handle)` allocates an unpublished graph.
Dimensions are 1..65536; max_cells is 1..1048576; max_bytes is <=128 MiB.

1. `terra_circuit_create(width,height,max_cells,max_bytes,out_handle)` -> status.
2. `terra_circuit_load(handle,cells,count)` imports <=65536 unique wire cells;
   record `{x,y,wires,routing}` is 16 bytes. `wires` is 1..15. Routing is 0 ordinary
   wire, 1 ordinary tile, 2 straight junction, 3 junction style 1, 4 junction style 2,
   5 pixel box. Include every occupied wire cell, including multi-cell footprints.
3. `terra_circuit_compile(handle,max_cells,out_compiled)` builds <=65536 cells'
   adjacency. Returns 1 while more remain, 0 once ready. Imported cells are immutable
   after compilation, except for `patch` below. Editing normally creates a replacement
   graph, publishes it when ready, then closes the prior graph.
4. `terra_circuit_begin(handle,seeds,count,colour,flags,work_limit)` starts one pass.
   Seeds are `{x,y}` (8 bytes), deduplicated in given order; the host sorts them by
   x then y as in `TripWire`. Colour is 0..3. Flag 1 collects all wire visits as trace;
   flag 0 emits only tile visits. Seeds without this colour are ignored. All matching
   seeds start with incoming direction 0 and counter 4.
5. `terra_circuit_step(handle,max_nodes,events,event_capacity,out_step)` processes at
   most 65536 nodes. Events are `{x,y,direction,flags}` (16 bytes). Flags: bit 0 tile,
   bit 1 seed (skip its device hit), bit 2 pixel horizontal, bit 3 pixel vertical.
   Directions are 0 down, 1 up, 2 right, 3 left. `out_step` is 16 bytes:
   `{processed,emitted,remaining,total_processed}`. Returns 1 if unfinished, 0 if done.
   **Each step pauses at a tile event before expanding that tile's outgoing edges.**
   The host must apply that tile hit and pixel flags, then call step again. Thus
   queued traversal cannot race ahead of device side effects. Pure-wire stretches
   need no per-cell JavaScript calls. Output capacity is 1..65536. `remaining` includes
   a pending tile expansion. The last tile event can therefore return 1 even if its
   following expansion completes the pass. `processed` includes only new visits,
   not this deferred expansion. Add processed to the host's operation counter.
6. `terra_circuit_cancel(handle)` discards the current pass, retaining topology and
   allocated buffers; `terra_circuit_close(handle)` frees all persistent allocations.

`terra_circuit_patch(handle,cells,count)` updates wires (0..15) and routing for
existing loaded coordinates, including between steps. It cannot insert new nodes;
the host must rebuild outside an active pass when the set of wire coordinates grows.
For shape-changing devices the host must synchronize affected cells before resuming.
Use the JS engine when a device can create wires during a pass. Ordinary lamp/gate
state changes do not modify the traversal graph.

## Correctness and bounded execution

The graph preserves every native FIFO visit, rather than converting the algorithm
to a visited-set flood fill. Native toProcess counters are 4 for seeds and 3 for
ordinary destinations; junctions and pixel boxes have no destination counter.
Junction loops are bounded by work_limit, memory budget, and host cancellation.
The counter generation changes per pass, so repeated pulses do not clear or scan
the whole graph. All buffers belong to the persistent allocator and survive unrelated
world-parser scratch resets. Handles are never reused. A limit or allocation failure
during `step` cancels the pass and clears its output. Invalid arguments leave an
existing pass unchanged; the host must cancel it and roll back its outer activation
transaction if it cannot safely retry the call.

Statuses: 0 OK/done, 1 more; -1 invalid argument, -2 stale handle, -3 wrong state,
-4 configured limit, -5 allocation failure, -6 invalid coordinate, -7 duplicate,
-8 exhausted handle/pass generation. Validation failures in load/patch are atomic.
`terra_circuit_stats(handle,out)` reports 16 u32 fields / 64 bytes:
`abi_version,width,height,cell_count,cell_capacity,compiled,ready,active,colour,`
`processed,queued,active_bytes,peak_bytes,max_bytes,queue_capacity,reserved`.

The limits account requested payloads plus conservative allocation overhead, not
process RSS or total Wasm memory. Callers must separately reserve room for their
bridge buffers and other modules.

## Scale and compatibility boundaries

The tests execute a 1000 x 1000 fully wired board in the actual Web artifact,
traverse all four colours (4,000,000 visits), and assert that the module stays below
the 160 MiB Mini Program memory ceiling. No speed threshold is asserted because
host CPUs and devices vary. The test prints build/traversal time and allocated /
linear memory separately. This measures the traversal kernel, not full logic-gate
simulation, rendering, file import, or mobile frame rate.

This ABI is deliberately bounded to 1,048,576 wire coordinates. It cannot load the
complete computerraria world: that world requires tens of millions of wire and gate
cells. Supporting it needs an independently validated compiled network representation,
compact gate/lamp state, and source-compatible lazy updates. A plain gate parity
shortcut is unsafe: Terraria XOR means exactly one lamp on, faulty lamp events keep
their order, and gate waves / repeated activation smoke suppression affect results.
The present ABI does not claim that a full computerraria machine boots or runs.

Board bounds are the host's editable region, with zero-based coordinates; the host
must exclude Terraria's two-tile uneditable world margin when importing world data.
Pixel axis events follow the same board bounds as the existing circuit editor.
