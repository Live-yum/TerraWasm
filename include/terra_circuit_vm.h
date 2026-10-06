#ifndef TERRA_CIRCUIT_VM_H
#define TERRA_CIRCUIT_VM_H

#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Internal circuit executor. It interprets wiring rules, never CPU instructions.
 * Net IDs are zero-based. Gate references identify physical gates even when their
 * immutable rule bytecode is shared. A compiler must canonicalize references for
 * the same physical gate reached from several wire colours. */
typedef struct TerraCircuitVm TerraCircuitVm;
typedef struct TerraVmGateRef { uint32_t group, offset; } TerraVmGateRef;
typedef struct TerraVmGateResult {
    TerraVmGateRef identity;
    uint32_t nets[4];
    uint32_t count;
} TerraVmGateResult;

enum TerraVmStatus {
    TERRA_VM_OK = 0, TERRA_VM_MORE = 1,
    TERRA_VM_INVALID = -1, TERRA_VM_STATE = -2,
    TERRA_VM_OOM = -3, TERRA_VM_LIMIT = -4,
    TERRA_VM_CALLBACK = -5
};

typedef struct TerraVmCallbacks {
    /* Return 1 with the next lamp event, 0 at end, or a negative status. The
     * compiler owns cursor encoding; zero means the start of a net's rule list.
     * Each call does bounded work and must advance cursor when returning 1. */
    int32_t (*next_candidate)(void* context, uint32_t net, uint32_t* cursor,
                              TerraVmGateRef* out);
    /* Evaluate against current lazy parity AFTER the complete gate wave. Return
     * 1 to fire, 0 for no output, negative for error. identity is the canonical
     * physical gate (not its shared template), nets preserve colour order. */
    int32_t (*evaluate)(void* context, TerraVmGateRef event,
                        const TerraCircuitVm* vm, TerraVmGateResult* out);
    /* One begin/end pair for EACH original TripWire, including every individual
     * gate output. Pixel intersection and colour state must use this boundary. */
    int32_t (*trip_begin)(void* context);
    int32_t (*net_hit)(void* context, uint32_t net);
    int32_t (*trip_end)(void* context);
    void (*smoke)(void* context, TerraVmGateRef identity);
    /* Optional transaction hooks own any state changed by callbacks (gate frame,
     * pixels, RNG, mechanics). VM always restores its own parity when cancelled
     * or a callback/allocation fails. Yielding MORE does not commit or roll back. */
    int32_t (*transaction_begin)(void* context);
    void (*transaction_commit)(void* context);
    void (*transaction_rollback)(void* context);
    /* The initial external TripWire forms one wave; each following batch of
     * queued physical gate outputs forms another. These hooks do not replace
     * per-TripWire hooks. A source-specific compatibility adapter may aggregate
     * auxiliary effects across a wave, while vanilla pixels use trip_end. */
    int32_t (*wave_begin)(void* context);
    int32_t (*wave_end)(void* context);
} TerraVmCallbacks;

typedef struct TerraVmStats {
    uint64_t net_pulses, gates_evaluated, gates_fired, smoke_events;
    uint64_t completed_pulses, operations;
    uint32_t active, queued_candidates, queued_gates, allocated_bytes, peak_bytes;
} TerraVmStats;

/* max_bytes is a caller-provided memory budget; zero means UINT32_MAX. No wire or
 * component-count policy limit is imposed. Memory uses tx_persistent_alloc/free. */
int32_t terra_vm_create(uint32_t net_count, uint32_t max_bytes,
                        const TerraVmCallbacks* callbacks, void* context,
                        TerraCircuitVm** out);
void terra_vm_destroy(TerraCircuitVm* vm);
int32_t terra_vm_parity(const TerraCircuitVm* vm, uint32_t net);
/* During a gate-output TripWire, returns 1 and its canonical physical identity.
 * Returns 0 for the external input or outside a TripWire. This lets source-rule
 * adapters honor the gate's SkipWire rectangle without changing scheduling. */
int32_t terra_vm_current_gate(const TerraCircuitVm* vm, TerraVmGateRef* out);
/* Idle-only state mutation for loading/restoring a compiled session. */
int32_t terra_vm_set_parity(TerraCircuitVm* vm, uint32_t net, uint32_t value);
/* Idle-only budget update. A budget below retained bytes is recorded and returns
 * LIMIT; subsequent begin calls remain blocked until memory is released or the
 * caller restores sufficient budget. Destroy and stats always remain available. */
int32_t terra_vm_set_budget(TerraCircuitVm* vm, uint32_t max_bytes);
/* Optional outer parity transaction for a command containing several complete
 * TripWires. Begin/commit require an idle VM; nesting is rejected. Cancel first
 * rolls back an active pulse, then restores the parity preceding the batch.
 * The owner separately snapshots callback-owned state for this outer boundary.
 * Ordinary begin/step and their per-pulse callback transactions are unchanged.
 * The lazily allocated snapshot is charged to max_bytes and retained for reuse;
 * failed begin changes no parity or transaction state. Work counters continue to
 * include attempted work when a batch is cancelled. */
int32_t terra_vm_batch_begin(TerraCircuitVm* vm);
int32_t terra_vm_batch_commit(TerraCircuitVm* vm);
int32_t terra_vm_batch_cancel(TerraCircuitVm* vm);
int32_t terra_vm_begin(TerraCircuitVm* vm, const uint32_t* nets, uint32_t count);
/* Advances at most max_operations bounded VM transitions. A resumed TripWire is
 * the same pulse: colour, pixel aggregation, gate wave and done set are retained. */
int32_t terra_vm_step(TerraCircuitVm* vm, uint32_t max_operations);
int32_t terra_vm_cancel(TerraCircuitVm* vm);
void terra_vm_stats(const TerraCircuitVm* vm, TerraVmStats* out);

#ifdef __cplusplus
}
#endif
#endif
