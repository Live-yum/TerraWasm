#ifndef TERRA_CIRCUIT_WORLD_H
#define TERRA_CIRCUIT_WORLD_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Additive, file-backed circuit ABI. World and scratch sources are host-owned
 * random-access files. No world-sized JavaScript array or Wasm input buffer is
 * required. A source request and output event repeat until supplied/acked. */
#define TERRA_CIRCUIT_WORLD_ABI 1u
#define TERRA_CIRCUIT_WORLD_WINDOW (1024u * 1024u)
enum TerraCircuitWorldEventKind {
    TCW_MORE = 0, TCW_READ = 1, TCW_WRITE = 2, TCW_RESULT = 3, TCW_READY = 4
};
enum TerraCircuitWorldCommandKind {
    TCW_VIEWPORT = 1, TCW_TRIGGER = 2, TCW_TICKS = 3,
    TCW_READ_LAMPS = 4, TCW_WRITE_LAMPS = 5, TCW_SAVE = 6,
    TCW_FRAGMENTS = 7, TCW_EXTRACT = 8
};
enum TerraCircuitWorldStatus {
    TCW_OK = 0, TCW_CONTINUE = 1,
    TCW_INVALID = -1, TCW_HANDLE = -2, TCW_STATE = -3,
    TCW_MEMORY = -4, TCW_FORMAT = -5, TCW_IO = -6, TCW_UNSUPPORTED = -7
};
/* Exactly twelve little-endian uint32 words. WRITE.data_ptr is borrowed until
 * ack; READ.data_ptr is zero. RESULT holds 16-byte cell records. READY is idle.
 * phase/completed/total are progress, independent of the source byte range.
 * Non-SAVE READY.reserved bit 0 identifies native WireHead pixel-wave rules.
 * They are selected ONLY by a TWLD tile-map entry named WireHead/ColorPixelBox,
 * never by the filename or saved numeric mod ID. Ordinary worlds retain vanilla
 * per-TripWire PixelBox rules; the mod profile reproduces its documented same-
 * colour custom pixel and cross-colour monochrome gate-wave pairing. */
typedef struct TerraCircuitWorldEvent {
    uint32_t abi_version, kind, source_id, offset, length, data_ptr;
    uint32_t phase, completed, total, result_kind, result_count, reserved;
} TerraCircuitWorldEvent;
/* Exactly sixteen words. VIEWPORT uses x/y/width/height/stride. TRIGGER uses the
 * rectangle and mask (red=1, blue=2, green=4, yellow=8). TICKS uses count.
 * READ/WRITE_LAMPS take data_count records of four words [x,y,state,reserved],
 * copied before returning; READ returns [x,y,on,tileType]. TRIGGER.count means
 * that many independent pulses (zero defaults to one), committed atomically.
 * TRIGGER.flags bit 0 invokes HitSwitch (normalizing multi-tile objects); zero
 * directly pulses the rectangle. TICKS advances the 60 Hz mechanical scheduler.
 * Cancelling or failing either command restores all of its circuit state.
 * SAVE writes to a distinct
 * source_id, plus aux_source_id when a TWLD sidecar was attached. Its final READY
 * has result_kind=TCW_SAVE, result_count=WLD bytes, reserved=TWLD bytes.
 * Frame/cell results are [x,y,type|flags<<16|wireMask<<24,frameX|frameY<<16]. */
/* VIEWPORT.flags bit 1 requests the separate wall layer. Its records remain
 * 16 bytes: [x,y,wallId|flags<<16,wallPaint]. Wall flags: active=1, layer=32,
 * invisible=64, fullbright=128. The wire mask is zero for this layer. */
typedef struct TerraCircuitWorldCommand {
    uint32_t abi_version, kind, x, y, width, height, stride, mask;
    uint32_t count, data_ptr, data_count, source_id, flags, aux_source_id, reserved1, reserved2;
} TerraCircuitWorldCommand;
/* FRAGMENTS: x=page offset, count=page size (1..32768). Optional geometry
 * data_count 16-byte records [type,frameX|frameY<<16,dx|dy<<8|width<<16|height<<24,0]
 * is copied on the first request. Exact frame layouts must come from the target
 * game's TileObjectData, never a rendered atlas. Later requests reuse the index.
 * Result records (32 bytes): [id,x,y,width,height,cells,wireCells,flags]. Flags:
 * 1=incomplete/ambiguous object geometry, 2=section-backed object, 4=modded tile.
 * EXTRACT: mask=fragment id, count=maximum cells (1..32768). Whole selected
 * objects and their circuit wires only; unrelated cells in the bounds are absent.
 * Result records (32 bytes): [x,y,type|flags<<16,frameX|frameY<<16,
 * wall|tilePaint<<16|wallPaint<<24,liquidAmount|liquidType<<8|brickStyle<<16|wires<<24,0,0].
 * Tile flags bits 0..6: active, actuator, inactive, invisible block/wall,
 * fullbright block/wall. Frame coordinates are signed 16-bit values.
 * EXTRACT flags bit 0: emit a COB1 object companion to aux_source_id using
 * sequential WRITE events, then the original cell RESULT. width limits bytes
 * (32..4194304); height limits objects (1..32768). Requires the numeric
 * circuitWorld.fragmentObjects=1 capability. No implicit empty inventories.
 * See docs/CIRCUIT_FRAGMENTS.md for the bounded COB1/overlay protocol.
 * READY.result_count is the total fragment count or extracted cell count.
 * Neither command changes electrical connectivity or simulation state. */
/* Exactly twenty-four words. Byte figures count retained native allocations;
 * the host must also account for Wasm heap, JS, images and platform buffers. */
typedef struct TerraCircuitWorldStats {
    uint32_t abi_version, state, width, height, spawn_x, spawn_y;
    uint32_t min_x, min_y, max_x, max_y, wire_cells, devices;
    uint32_t gates, networks, compiled_columns, phase, active_bytes, peak_bytes;
    uint32_t ticks_lo, ticks_hi, net_pulses_lo, net_pulses_hi, gates_fired_lo, gates_fired_hi;
} TerraCircuitWorldStats;

uint32_t terra_circuit_world_abi_version(void);
/* world_handle must remain open until circuit close. scratch_source_id must be
 * a new empty writable file, distinct from the world's immutable source. A zero
 * twld_source_id means absent. Optional TWLD input is gzip/NBT streamed in C. */
int32_t terra_circuit_world_begin(uint32_t world_handle, uint32_t scratch_source_id,
    uint32_t twld_source_id, uint32_t twld_size, uint32_t max_bytes, uint32_t* out_handle);
int32_t terra_circuit_world_step(uint32_t handle, uint32_t work_units, TerraCircuitWorldEvent* out);
int32_t terra_circuit_world_supply(uint32_t handle, uint32_t source_id, uint32_t offset,
    const uint8_t* data, uint32_t length);
int32_t terra_circuit_world_ack(uint32_t handle);
int32_t terra_circuit_world_command(uint32_t handle, const TerraCircuitWorldCommand* command);
int32_t terra_circuit_world_stats(uint32_t handle, TerraCircuitWorldStats* out);
int32_t terra_circuit_world_cancel(uint32_t handle);
int32_t terra_circuit_world_close(uint32_t handle);

#ifdef __cplusplus
}
#endif
#endif
