/* Native Wiring.HitWire traversal. See docs/CIRCUIT_ABI_V1.md for source pin,
 * interleaving contract, limits, and the host-owned gate/device responsibilities. */
#include "terra_circuit.h"
#include <limits.h>
#include <stddef.h>
#include <string.h>

extern uint8_t* tx_persistent_alloc(uint32_t size);
extern void tx_persistent_free(void* pointer);

#define CW_HANDLES 16u
#define CW_OVERHEAD 128u
#define CW_MISSING UINT32_MAX
typedef struct CwCell {
    uint32_t key, neighbors[4], process_epoch, seed_epoch;
    uint8_t wires, routing, counter, reserved;
} CwCell;
typedef union CwAllocation {
    struct { uint32_t bytes; } value;
    max_align_t alignment;
} CwAllocation;
typedef struct CwWorkspace {
    uint32_t handle, width, height, capacity, count, hash_capacity;
    uint32_t maximum, bytes, peak, compiled, sealed, ready, active;
    uint32_t epoch, colour, flags, limit, processed;
    uint32_t queue_capacity, queue_head, queue_count;
    uint32_t pending, pending_index, pending_direction, pending_routing;
    int32_t allocation_error;
    CwCell* cells;
    uint32_t* table;
    uint32_t* queue;
} CwWorkspace;
static CwWorkspace* cw_workspaces[CW_HANDLES];
static uint32_t cw_next_handle = 1u;

_Static_assert(sizeof(CwCell) == 32u, "compact native wiring cell");
_Static_assert(sizeof(TerraCircuitCell) == 16u, "circuit cell ABI");
_Static_assert(sizeof(TerraCircuitPoint) == 8u, "circuit seed ABI");
_Static_assert(sizeof(TerraCircuitEvent) == 16u, "circuit event ABI");
_Static_assert(sizeof(TerraCircuitStep) == 16u, "circuit step ABI");
_Static_assert(sizeof(TerraCircuitStats) == 64u, "circuit stats ABI");

static CwWorkspace* cw_lookup(uint32_t handle) {
    if (!handle) return NULL;
    for (uint32_t i = 0; i < CW_HANDLES; ++i)
        if (cw_workspaces[i] && cw_workspaces[i]->handle == handle) return cw_workspaces[i];
    return NULL;
}
static void* cw_alloc(CwWorkspace* w, uint32_t bytes) {
    uint64_t total = (uint64_t)bytes + sizeof(CwAllocation) + CW_OVERHEAD;
    if (total > w->maximum - w->bytes) {
        w->allocation_error = TERRA_CIRCUIT_LIMIT; return NULL;
    }
    CwAllocation* p = (CwAllocation*)tx_persistent_alloc(bytes + (uint32_t)sizeof(CwAllocation));
    if (!p) { w->allocation_error = TERRA_CIRCUIT_OOM; return NULL; }
    p->value.bytes = (uint32_t)total;
    w->bytes += (uint32_t)total;
    if (w->bytes > w->peak) w->peak = w->bytes;
    return p + 1;
}
static void cw_free(CwWorkspace* w, void* pointer) {
    if (!pointer) return;
    CwAllocation* p = (CwAllocation*)pointer - 1;
    w->bytes -= p->value.bytes;
    tx_persistent_free(p);
}
static void cw_destroy(CwWorkspace* w) {
    cw_free(w, w->cells); cw_free(w, w->table); cw_free(w, w->queue);
    tx_persistent_free(w);
}
static uint32_t cw_hash(uint32_t value) {
    value ^= value >> 16; value *= 0x7feb352du;
    value ^= value >> 15; value *= 0x846ca68bu;
    return value ^ (value >> 16);
}
static uint32_t cw_slot(const CwWorkspace* w, uint32_t key) {
    uint32_t slot = cw_hash(key) & (w->hash_capacity - 1u);
    while (w->table[slot] && w->cells[w->table[slot] - 1u].key != key)
        slot = (slot + 1u) & (w->hash_capacity - 1u);
    return slot;
}
static uint32_t cw_index(const CwWorkspace* w, uint32_t x, uint32_t y) {
    if (x >= w->width || y >= w->height) return CW_MISSING;
    uint32_t value = w->table[cw_slot(w, (y << 16) | x)];
    return value ? value - 1u : CW_MISSING;
}
static int32_t cw_validate_cell(const CwWorkspace* w, const TerraCircuitCell* cell, int allow_empty) {
    if (cell->x >= w->width || cell->y >= w->height) return TERRA_CIRCUIT_BOUNDS;
    if (cell->wires > 15u || (!cell->wires && !allow_empty) || cell->routing > TERRA_CIRCUIT_PIXEL)
        return TERRA_CIRCUIT_INVALID;
    return TERRA_CIRCUIT_OK;
}
static int32_t cw_reserve_queue(CwWorkspace* w, uint32_t needed) {
    if (needed <= w->queue_capacity) return TERRA_CIRCUIT_OK;
    uint32_t capacity = w->queue_capacity ? w->queue_capacity : 256u;
    while (capacity < needed) {
        if (capacity >= TERRA_CIRCUIT_MAX_BYTES / 8u) return TERRA_CIRCUIT_LIMIT;
        capacity *= 2u;
    }
    uint32_t* queue = (uint32_t*)cw_alloc(w, capacity * 4u);
    if (!queue) return w->allocation_error;
    for (uint32_t i = 0; i < w->queue_count; ++i)
        queue[i] = w->queue[(w->queue_head + i) & (w->queue_capacity - 1u)];
    cw_free(w, w->queue);
    w->queue = queue; w->queue_capacity = capacity; w->queue_head = 0;
    return TERRA_CIRCUIT_OK;
}
static int32_t cw_push(CwWorkspace* w, uint32_t index, uint32_t direction) {
    int32_t status = cw_reserve_queue(w, w->queue_count + 1u);
    if (status != TERRA_CIRCUIT_OK) return status;
    w->queue[(w->queue_head + w->queue_count) & (w->queue_capacity - 1u)] = (index << 2) | direction;
    ++w->queue_count;
    return TERRA_CIRCUIT_OK;
}
static void cw_cancel(CwWorkspace* w) {
    w->active = 0; w->pending = 0; w->queue_count = 0; w->queue_head = 0;
}

uint32_t terra_circuit_abi_version(void) { return TERRA_CIRCUIT_ABI; }
int32_t terra_circuit_create(uint32_t width, uint32_t height, uint32_t max_cells,
    uint32_t max_bytes, uint32_t* out_handle) {
    if (!out_handle) return TERRA_CIRCUIT_INVALID;
    *out_handle = 0;
    if (!width || !height || width > TERRA_CIRCUIT_MAX_DIMENSION || height > TERRA_CIRCUIT_MAX_DIMENSION
        || !max_cells || max_cells > TERRA_CIRCUIT_MAX_CELLS || !max_bytes || max_bytes > TERRA_CIRCUIT_MAX_BYTES)
        return TERRA_CIRCUIT_INVALID;
    if (max_bytes < sizeof(CwWorkspace) + CW_OVERHEAD) return TERRA_CIRCUIT_LIMIT;
    if (!cw_next_handle) return TERRA_CIRCUIT_EXHAUSTED;
    uint32_t slot = 0;
    while (slot < CW_HANDLES && cw_workspaces[slot]) ++slot;
    if (slot == CW_HANDLES) return TERRA_CIRCUIT_LIMIT;
    CwWorkspace* w = (CwWorkspace*)tx_persistent_alloc((uint32_t)sizeof(CwWorkspace));
    if (!w) return TERRA_CIRCUIT_OOM;
    memset(w, 0, sizeof(*w));
    w->width = width; w->height = height; w->capacity = max_cells; w->maximum = max_bytes;
    w->bytes = w->peak = (uint32_t)sizeof(CwWorkspace) + CW_OVERHEAD;
    w->hash_capacity = 2u;
    while (w->hash_capacity < max_cells * 2u) w->hash_capacity *= 2u;
    w->cells = (CwCell*)cw_alloc(w, max_cells * (uint32_t)sizeof(CwCell));
    if (w->cells) w->table = (uint32_t*)cw_alloc(w, w->hash_capacity * 4u);
    if (!w->cells || !w->table) {
        int32_t status = w->allocation_error; cw_destroy(w); return status;
    }
    memset(w->table, 0, w->hash_capacity * 4u);
    w->handle = cw_next_handle++;
    cw_workspaces[slot] = w; *out_handle = w->handle;
    return TERRA_CIRCUIT_OK;
}
int32_t terra_circuit_close(uint32_t handle) {
    CwWorkspace* w = cw_lookup(handle);
    if (!w) return TERRA_CIRCUIT_HANDLE;
    for (uint32_t i = 0; i < CW_HANDLES; ++i) if (cw_workspaces[i] == w) { cw_workspaces[i] = NULL; break; }
    cw_destroy(w); return TERRA_CIRCUIT_OK;
}
int32_t terra_circuit_stats(uint32_t handle, TerraCircuitStats* out) {
    CwWorkspace* w = cw_lookup(handle);
    if (!w) return TERRA_CIRCUIT_HANDLE;
    if (!out) return TERRA_CIRCUIT_INVALID;
    *out = (TerraCircuitStats){TERRA_CIRCUIT_ABI, w->width, w->height, w->count, w->capacity,
        w->compiled, w->ready, w->active, w->colour, w->processed, w->queue_count + w->pending,
        w->bytes, w->peak, w->maximum, w->queue_capacity, 0u};
    return TERRA_CIRCUIT_OK;
}
int32_t terra_circuit_load(uint32_t handle, const TerraCircuitCell* cells, uint32_t count) {
    CwWorkspace* w = cw_lookup(handle);
    if (!w) return TERRA_CIRCUIT_HANDLE;
    if (w->sealed) return TERRA_CIRCUIT_STATE;
    if ((!cells && count) || count > TERRA_CIRCUIT_BATCH) return TERRA_CIRCUIT_INVALID;
    if (count > w->capacity - w->count) return TERRA_CIRCUIT_LIMIT;
    for (uint32_t i = 0; i < count; ++i) {
        int32_t status = cw_validate_cell(w, cells + i, 0);
        if (status != TERRA_CIRCUIT_OK) return status;
    }
    uint32_t before = w->count;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t key = (cells[i].y << 16) | cells[i].x, slot = cw_slot(w, key);
        if (w->table[slot]) {
            /* Reverse insertion order permits deletion without tombstones: no
             * older probe chain can depend on a newly inserted table slot. */
            while (w->count > before) {
                --w->count; w->table[cw_slot(w, w->cells[w->count].key)] = 0;
            }
            return TERRA_CIRCUIT_DUPLICATE;
        }
        CwCell* cell = w->cells + w->count;
        memset(cell, 0, sizeof(*cell));
        cell->key = key; cell->wires = (uint8_t)cells[i].wires; cell->routing = (uint8_t)cells[i].routing;
        w->table[slot] = ++w->count;
    }
    return TERRA_CIRCUIT_OK;
}
int32_t terra_circuit_compile(uint32_t handle, uint32_t max_cells, uint32_t* out_compiled) {
    CwWorkspace* w = cw_lookup(handle);
    if (!w) return TERRA_CIRCUIT_HANDLE;
    if (!out_compiled || !max_cells || max_cells > TERRA_CIRCUIT_BATCH) return TERRA_CIRCUIT_INVALID;
    if (w->active) return TERRA_CIRCUIT_STATE;
    w->sealed = 1u;
    uint32_t stop = w->compiled + max_cells;
    if (stop > w->count) stop = w->count;
    for (; w->compiled < stop; ++w->compiled) {
        CwCell* cell = w->cells + w->compiled;
        uint32_t x = cell->key & 65535u, y = cell->key >> 16;
        cell->neighbors[0] = cw_index(w, x, y + 1u) + 1u;
        cell->neighbors[1] = cw_index(w, x, y - 1u) + 1u;
        cell->neighbors[2] = cw_index(w, x + 1u, y) + 1u;
        cell->neighbors[3] = cw_index(w, x - 1u, y) + 1u;
    }
    *out_compiled = w->compiled;
    w->ready = w->compiled == w->count;
    return w->ready ? TERRA_CIRCUIT_OK : TERRA_CIRCUIT_MORE;
}
int32_t terra_circuit_patch(uint32_t handle, const TerraCircuitCell* cells, uint32_t count) {
    CwWorkspace* w = cw_lookup(handle);
    if (!w) return TERRA_CIRCUIT_HANDLE;
    if (!w->ready) return TERRA_CIRCUIT_STATE;
    if ((!cells && count) || count > TERRA_CIRCUIT_BATCH) return TERRA_CIRCUIT_INVALID;
    for (uint32_t i = 0; i < count; ++i) {
        int32_t status = cw_validate_cell(w, cells + i, 1);
        if (status != TERRA_CIRCUIT_OK) return status;
        if (cw_index(w, cells[i].x, cells[i].y) == CW_MISSING) return TERRA_CIRCUIT_BOUNDS;
    }
    for (uint32_t i = 0; i < count; ++i) {
        CwCell* cell = w->cells + cw_index(w, cells[i].x, cells[i].y);
        cell->wires = (uint8_t)cells[i].wires; cell->routing = (uint8_t)cells[i].routing;
    }
    return TERRA_CIRCUIT_OK;
}
int32_t terra_circuit_begin(uint32_t handle, const TerraCircuitPoint* seeds, uint32_t count,
    uint32_t colour, uint32_t flags, uint32_t work_limit) {
    CwWorkspace* w = cw_lookup(handle);
    if (!w) return TERRA_CIRCUIT_HANDLE;
    if (!w->ready || w->active) return TERRA_CIRCUIT_STATE;
    if ((!seeds && count) || count > TERRA_CIRCUIT_BATCH || colour > 3u || flags > TERRA_CIRCUIT_TRACE || !work_limit)
        return TERRA_CIRCUIT_INVALID;
    for (uint32_t i = 0; i < count; ++i)
        if (seeds[i].x >= w->width || seeds[i].y >= w->height) return TERRA_CIRCUIT_BOUNDS;
    if (w->epoch == UINT32_MAX) return TERRA_CIRCUIT_EXHAUSTED;
    cw_cancel(w); ++w->epoch;
    w->colour = colour; w->flags = flags; w->limit = work_limit; w->processed = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t index = cw_index(w, seeds[i].x, seeds[i].y);
        if (index == CW_MISSING) continue;
        CwCell* cell = w->cells + index;
        if (!(cell->wires & (1u << colour)) || cell->seed_epoch == w->epoch) continue;
        int32_t status = cw_push(w, index, 0u);
        if (status != TERRA_CIRCUIT_OK) { cw_cancel(w); return status; }
        cell->seed_epoch = cell->process_epoch = w->epoch; cell->counter = 4u;
    }
    w->active = 1u;
    return TERRA_CIRCUIT_OK;
}
static int32_t cw_expand(CwWorkspace* w) {
    static const uint8_t routes[3][4] = {{0,1,2,3}, {3,2,1,0}, {2,3,0,1}};
    const CwCell* current = w->cells + w->pending_index;
    uint32_t routing = w->pending_routing, incoming = w->pending_direction;
    for (uint32_t direction = 0; direction < 4u; ++direction) {
        if (routing >= TERRA_CIRCUIT_JUNCTION_STRAIGHT && routing <= TERRA_CIRCUIT_JUNCTION_RIGHT
            && routes[routing - TERRA_CIRCUIT_JUNCTION_STRAIGHT][incoming] != direction) continue;
        if (routing == TERRA_CIRCUIT_PIXEL && direction != incoming) continue;
        if (!current->neighbors[direction]) continue;
        uint32_t index = current->neighbors[direction] - 1u;
        CwCell* next = w->cells + index;
        if (!(next->wires & (1u << w->colour))) continue;
        if (next->process_epoch == w->epoch && next->counter) { --next->counter; continue; }
        int32_t status = cw_push(w, index, direction);
        if (status != TERRA_CIRCUIT_OK) return status;
        if (next->routing <= TERRA_CIRCUIT_TILE) { next->process_epoch = w->epoch; next->counter = 3u; }
    }
    w->pending = 0u;
    return TERRA_CIRCUIT_OK;
}
int32_t terra_circuit_step(uint32_t handle, uint32_t max_nodes, TerraCircuitEvent* events,
    uint32_t event_capacity, TerraCircuitStep* out_step) {
    CwWorkspace* w = cw_lookup(handle);
    if (!w) return TERRA_CIRCUIT_HANDLE;
    if (!out_step || !events || !event_capacity || event_capacity > TERRA_CIRCUIT_BATCH
        || !max_nodes || max_nodes > TERRA_CIRCUIT_BATCH) return TERRA_CIRCUIT_INVALID;
    if (!w->active) return TERRA_CIRCUIT_STATE;
    *out_step = (TerraCircuitStep){0};
    for (;;) {
        if (w->pending) {
            int32_t status = cw_expand(w);
            if (status != TERRA_CIRCUIT_OK) { cw_cancel(w); *out_step = (TerraCircuitStep){0}; return status; }
        }
        if (!w->queue_count) { w->active = 0u; break; }
        if (out_step->processed == max_nodes || out_step->emitted == event_capacity) break;
        if (w->processed == w->limit) {
            cw_cancel(w); *out_step = (TerraCircuitStep){0}; return TERRA_CIRCUIT_LIMIT;
        }
        uint32_t word = w->queue[w->queue_head];
        w->queue_head = (w->queue_head + 1u) & (w->queue_capacity - 1u); --w->queue_count;
        uint32_t index = word >> 2, direction = word & 3u;
        CwCell* cell = w->cells + index;
        uint32_t x = cell->key & 65535u, y = cell->key >> 16, flags = 0;
        ++w->processed; ++out_step->processed;
        if (cell->routing != TERRA_CIRCUIT_WIRE) flags |= TERRA_CIRCUIT_EVENT_TILE;
        if (cell->seed_epoch == w->epoch) flags |= TERRA_CIRCUIT_EVENT_SEED;
        if (cell->routing == TERRA_CIRCUIT_PIXEL) {
            /* Wiring records the axis after bounds but before the neighboring
             * wire test. A dangling in-bounds pixel exit still records its axis. */
            if ((direction == 0u && y + 1u < w->height) || (direction == 1u && y > 0u))
                flags |= TERRA_CIRCUIT_EVENT_PIXEL_VERTICAL;
            if ((direction == 2u && x + 1u < w->width) || (direction == 3u && x > 0u))
                flags |= TERRA_CIRCUIT_EVENT_PIXEL_HORIZONTAL;
        }
        if ((flags & TERRA_CIRCUIT_EVENT_TILE) || (w->flags & TERRA_CIRCUIT_TRACE))
            events[out_step->emitted++] = (TerraCircuitEvent){x, y, direction, flags};
        w->pending = 1u; w->pending_index = index; w->pending_direction = direction;
        w->pending_routing = cell->routing;
        if (flags & TERRA_CIRCUIT_EVENT_TILE) break;
    }
    out_step->remaining = w->queue_count + w->pending;
    out_step->total_processed = w->processed;
    return w->active ? TERRA_CIRCUIT_MORE : TERRA_CIRCUIT_OK;
}
int32_t terra_circuit_cancel(uint32_t handle) {
    CwWorkspace* w = cw_lookup(handle);
    if (!w) return TERRA_CIRCUIT_HANDLE;
    cw_cancel(w); return TERRA_CIRCUIT_OK;
}
