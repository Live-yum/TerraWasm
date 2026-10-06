#ifndef TERRA_CIRCUIT_H
#define TERRA_CIRCUIT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define TERRA_CIRCUIT_ABI 1u
#define TERRA_CIRCUIT_BATCH 65536u
#define TERRA_CIRCUIT_MAX_CELLS 1048576u
#define TERRA_CIRCUIT_MAX_DIMENSION 65536u
#define TERRA_CIRCUIT_MAX_BYTES (128u * 1024u * 1024u)

enum TerraCircuitStatus {
    TERRA_CIRCUIT_OK = 0, TERRA_CIRCUIT_MORE = 1,
    TERRA_CIRCUIT_INVALID = -1, TERRA_CIRCUIT_HANDLE = -2,
    TERRA_CIRCUIT_STATE = -3, TERRA_CIRCUIT_LIMIT = -4,
    TERRA_CIRCUIT_OOM = -5, TERRA_CIRCUIT_BOUNDS = -6,
    TERRA_CIRCUIT_DUPLICATE = -7, TERRA_CIRCUIT_EXHAUSTED = -8
};
enum TerraCircuitRouting {
    TERRA_CIRCUIT_WIRE = 0, TERRA_CIRCUIT_TILE = 1,
    TERRA_CIRCUIT_JUNCTION_STRAIGHT = 2,
    TERRA_CIRCUIT_JUNCTION_LEFT = 3, TERRA_CIRCUIT_JUNCTION_RIGHT = 4,
    TERRA_CIRCUIT_PIXEL = 5
};
enum TerraCircuitEventFlags {
    TERRA_CIRCUIT_EVENT_TILE = 1u, TERRA_CIRCUIT_EVENT_SEED = 2u,
    TERRA_CIRCUIT_EVENT_PIXEL_HORIZONTAL = 4u,
    TERRA_CIRCUIT_EVENT_PIXEL_VERTICAL = 8u
};
#define TERRA_CIRCUIT_TRACE 1u

/* Wire cells and hit events are 16 bytes; seeds are 8 bytes. */
typedef struct TerraCircuitCell { uint32_t x, y, wires, routing; } TerraCircuitCell;
typedef struct TerraCircuitPoint { uint32_t x, y; } TerraCircuitPoint;
typedef struct TerraCircuitEvent { uint32_t x, y, direction, flags; } TerraCircuitEvent;
typedef struct TerraCircuitStep { uint32_t processed, emitted, remaining, total_processed; } TerraCircuitStep;
typedef struct TerraCircuitStats {
    uint32_t abi_version, width, height, cell_count, cell_capacity, compiled, ready, active;
    uint32_t colour, processed, queued, active_bytes, peak_bytes, max_bytes, queue_capacity, reserved;
} TerraCircuitStats;

uint32_t terra_circuit_abi_version(void);
int32_t terra_circuit_create(uint32_t width, uint32_t height, uint32_t max_cells,
    uint32_t max_bytes, uint32_t* out_handle);
int32_t terra_circuit_close(uint32_t handle);
int32_t terra_circuit_stats(uint32_t handle, TerraCircuitStats* out);
/* Load accepts unique coordinates with wires 1..15 before compilation starts.
 * Validation errors leave the graph unchanged. Compile advances <=65536 cells;
 * returns MORE until complete. Loads after the first compile return STATE. */
int32_t terra_circuit_load(uint32_t handle, const TerraCircuitCell* cells, uint32_t count);
int32_t terra_circuit_compile(uint32_t handle, uint32_t max_cells, uint32_t* out_compiled);
/* Patch existing coordinates only, wires 0..15. Allowed after compilation,
 * including between traversal steps. No adjacency changes are necessary because
 * erased cells retain their stable index. Validation is atomic. */
int32_t terra_circuit_patch(uint32_t handle, const TerraCircuitCell* cells, uint32_t count);
/* One independent colour pass. Host provides seeds in native x-then-y order and
 * applies SkipWire to its own device hit set. Duplicated seeds are ignored. */
int32_t terra_circuit_begin(uint32_t handle, const TerraCircuitPoint* seeds, uint32_t count,
    uint32_t colour, uint32_t flags, uint32_t work_limit);
/* Pauses after each TILE event, BEFORE expanding its outgoing edges. Host applies
 * HitWireSingle and patches changed topology before calling again. Pure wires run
 * in bounded batches. A negative execution result cancels the pass; the host must
 * roll back its activation. Invalid call arguments do not mutate a valid pass. */
int32_t terra_circuit_step(uint32_t handle, uint32_t max_nodes, TerraCircuitEvent* events,
    uint32_t event_capacity, TerraCircuitStep* out_step);
int32_t terra_circuit_cancel(uint32_t handle);

#ifdef __cplusplus
}
#endif
#endif
