/* Persistent executor for compiled Terraria wiring networks.
 * Ordering reference: Terraria/Wiring.cs, TripWire / LogicGatePass /
 * CheckLogicGate at 8255d34616c780af12079425ac92a0a7aed87d71.
 * No RISC-V opcodes, registers, addresses, or Computerraria coordinates occur in
 * this executor. A CPU is an ordinary arrangement of its physical wiring rules.
 */
#include "terra_circuit_vm.h"
#include <limits.h>
#include <stddef.h>
#include <string.h>

extern uint8_t* tx_persistent_alloc(uint32_t size);
extern void tx_persistent_free(void* pointer);

#define VM_ALLOC_OVERHEAD 128u

typedef union VmAllocation {
    struct { uint32_t charged; } value;
    max_align_t alignment;
} VmAllocation;

typedef struct VmDoneSlot {
    TerraVmGateRef key;
    uint32_t epoch;
} VmDoneSlot;

enum VmPhase {
    VM_IDLE, VM_WAVE_BEGIN, VM_TRIP_BEGIN, VM_NET_SELECT, VM_NET_CANDIDATES,
    VM_TRIP_END, VM_TRIP_CLEAR, VM_EVALUATE, VM_GATES, VM_WAVE_END, VM_FINISH
};

struct TerraCircuitVm {
    TerraVmCallbacks callbacks;
    void* context;
    uint32_t net_count, bit_bytes, maximum, allocated, peak;
    uint8_t *parity, *checkpoint, *trip_seen, *batch_checkpoint;
    uint32_t *seeds, seed_capacity;
    uint32_t *seen_nets, seen_count, seen_capacity, seen_clear_index;
    TerraVmGateRef* candidates;
    uint32_t candidate_count, candidate_capacity, candidate_index;
    TerraVmGateResult *current, *next;
    uint32_t current_count, current_capacity, current_index;
    uint32_t next_count, next_capacity;
    VmDoneSlot* done;
    uint32_t done_capacity, done_count, epoch;
    const uint32_t* trip_nets;
    uint32_t trip_count, trip_index, net, net_cursor;
    uint32_t active, phase, after_trip, batch_active, after_wave;
    uint32_t trip_is_gate;
    TerraVmGateRef trip_gate;
    TerraVmStats counters;
    int32_t allocation_error;
};

static void* vm_allocate(TerraCircuitVm* vm, uint32_t bytes) {
    uint64_t requested = (uint64_t)bytes + sizeof(VmAllocation);
    uint64_t charged = requested + VM_ALLOC_OVERHEAD;
    if (requested > UINT32_MAX || vm->allocated > vm->maximum ||
        charged > (uint64_t)vm->maximum - vm->allocated) {
        vm->allocation_error = TERRA_VM_LIMIT;
        return NULL;
    }
    VmAllocation* block = (VmAllocation*)tx_persistent_alloc((uint32_t)requested);
    if (!block) {
        vm->allocation_error = TERRA_VM_OOM;
        return NULL;
    }
    block->value.charged = (uint32_t)charged;
    vm->allocated += (uint32_t)charged;
    if (vm->allocated > vm->peak) vm->peak = vm->allocated;
    return block + 1;
}

static void vm_release(TerraCircuitVm* vm, void* pointer) {
    if (!pointer) return;
    VmAllocation* block = (VmAllocation*)pointer - 1;
    vm->allocated -= block->value.charged;
    tx_persistent_free(block);
}

static int32_t vm_reserve(TerraCircuitVm* vm, void** data, uint32_t* capacity,
                          uint32_t count, uint32_t needed, uint32_t item_size) {
    if (needed <= *capacity) return TERRA_VM_OK;
    uint32_t target = *capacity ? *capacity : 64u;
    while (target < needed) {
        if (target > UINT32_MAX / 2u) return TERRA_VM_LIMIT;
        target *= 2u;
    }
    if (target > UINT32_MAX / item_size) return TERRA_VM_LIMIT;
    void* grown = vm_allocate(vm, target * item_size);
    if (!grown) return vm->allocation_error;
    if (count) memcpy(grown, *data, (size_t)count * item_size);
    vm_release(vm, *data);
    *data = grown;
    *capacity = target;
    return TERRA_VM_OK;
}

static uint32_t vm_hash(TerraVmGateRef key) {
    uint32_t h = key.group ^ (key.offset + 0x9e3779b9u + (key.group << 6) + (key.group >> 2));
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    return h ^ (h >> 16);
}

static int vm_same_gate(TerraVmGateRef a, TerraVmGateRef b) {
    return a.group == b.group && a.offset == b.offset;
}

static uint32_t vm_done_slot(const TerraCircuitVm* vm, TerraVmGateRef key) {
    uint32_t slot = vm_hash(key) & (vm->done_capacity - 1u);
    while (vm->done[slot].epoch == vm->epoch && !vm_same_gate(vm->done[slot].key, key))
        slot = (slot + 1u) & (vm->done_capacity - 1u);
    return slot;
}

static int vm_was_done(const TerraCircuitVm* vm, TerraVmGateRef key) {
    return vm->done_capacity && vm->done[vm_done_slot(vm, key)].epoch == vm->epoch;
}

static int32_t vm_mark_done(TerraCircuitVm* vm, TerraVmGateRef key) {
    if (!vm->done_capacity || (uint64_t)(vm->done_count + 1u) * 4u >= (uint64_t)vm->done_capacity * 3u) {
        if (vm->done_capacity > UINT32_MAX / 2u / sizeof(VmDoneSlot)) return TERRA_VM_LIMIT;
        uint32_t capacity = vm->done_capacity ? vm->done_capacity * 2u : 64u;
        VmDoneSlot* grown = (VmDoneSlot*)vm_allocate(vm, capacity * (uint32_t)sizeof(VmDoneSlot));
        if (!grown) return vm->allocation_error;
        memset(grown, 0, (size_t)capacity * sizeof(VmDoneSlot));
        VmDoneSlot* previous = vm->done;
        uint32_t previous_capacity = vm->done_capacity;
        vm->done = grown; vm->done_capacity = capacity;
        for (uint32_t i = 0; i < previous_capacity; ++i) {
            if (previous[i].epoch != vm->epoch) continue;
            vm->done[vm_done_slot(vm, previous[i].key)] = previous[i];
        }
        vm_release(vm, previous);
    }
    uint32_t slot = vm_done_slot(vm, key);
    if (vm->done[slot].epoch != vm->epoch) {
        vm->done[slot].key = key;
        vm->done[slot].epoch = vm->epoch;
        ++vm->done_count;
    }
    return TERRA_VM_OK;
}

static void vm_reset_queues(TerraCircuitVm* vm) {
    vm->candidate_count = vm->candidate_index = 0;
    vm->current_count = vm->current_index = vm->next_count = 0;
    vm->trip_count = vm->trip_index = vm->net_cursor = 0;
    vm->seen_count = vm->seen_clear_index = 0;
    vm->done_count = 0;
    vm->trip_nets = NULL;
    vm->trip_is_gate = 0;
    vm->phase = VM_IDLE;
}

int32_t terra_vm_cancel(TerraCircuitVm* vm) {
    if (!vm) return TERRA_VM_INVALID;
    if (vm->active) {
        memcpy(vm->parity, vm->checkpoint, vm->bit_bytes);
        for (uint32_t i = 0; i < vm->seen_count; ++i) {
            uint32_t net = vm->seen_nets[i];
            vm->trip_seen[net >> 3] &= (uint8_t)~(1u << (net & 7));
        }
        if (vm->callbacks.transaction_rollback) vm->callbacks.transaction_rollback(vm->context);
    }
    vm->active = 0;
    vm_reset_queues(vm);
    return TERRA_VM_OK;
}

static int32_t vm_fail(TerraCircuitVm* vm, int32_t status) {
    terra_vm_cancel(vm);
    return status;
}

void terra_vm_destroy(TerraCircuitVm* vm) {
    if (!vm) return;
    terra_vm_cancel(vm);
    vm_release(vm, vm->parity); vm_release(vm, vm->checkpoint); vm_release(vm, vm->trip_seen);
    vm_release(vm, vm->batch_checkpoint);
    vm_release(vm, vm->seeds); vm_release(vm, vm->seen_nets); vm_release(vm, vm->candidates);
    vm_release(vm, vm->current); vm_release(vm, vm->next); vm_release(vm, vm->done);
    tx_persistent_free(vm);
}

int32_t terra_vm_create(uint32_t net_count, uint32_t max_bytes,
                        const TerraVmCallbacks* callbacks, void* context,
                        TerraCircuitVm** out) {
    if (!out || !callbacks || !callbacks->next_candidate || !callbacks->evaluate) return TERRA_VM_INVALID;
    *out = NULL;
    if (!max_bytes) max_bytes = UINT32_MAX;
    uint32_t base = (uint32_t)sizeof(TerraCircuitVm) + VM_ALLOC_OVERHEAD;
    if (base > max_bytes) return TERRA_VM_LIMIT;
    TerraCircuitVm* vm = (TerraCircuitVm*)tx_persistent_alloc((uint32_t)sizeof(TerraCircuitVm));
    if (!vm) return TERRA_VM_OOM;
    memset(vm, 0, sizeof(*vm));
    vm->callbacks = *callbacks; vm->context = context;
    vm->net_count = net_count; vm->maximum = max_bytes;
    vm->allocated = vm->peak = base;
    vm->bit_bytes = net_count ? (uint32_t)(((uint64_t)net_count + 7u) / 8u) : 1u;
    vm->parity = (uint8_t*)vm_allocate(vm, vm->bit_bytes);
    vm->checkpoint = (uint8_t*)vm_allocate(vm, vm->bit_bytes);
    vm->trip_seen = (uint8_t*)vm_allocate(vm, vm->bit_bytes);
    if (!vm->parity || !vm->checkpoint || !vm->trip_seen) {
        int32_t status = vm->allocation_error;
        terra_vm_destroy(vm);
        return status;
    }
    memset(vm->parity, 0, vm->bit_bytes);
    memset(vm->trip_seen, 0, vm->bit_bytes);
    *out = vm;
    return TERRA_VM_OK;
}

int32_t terra_vm_parity(const TerraCircuitVm* vm, uint32_t net) {
    if (!vm || net >= vm->net_count) return TERRA_VM_INVALID;
    return (vm->parity[net >> 3] >> (net & 7)) & 1;
}

int32_t terra_vm_current_gate(const TerraCircuitVm* vm, TerraVmGateRef* out) {
    if (!vm || !out) return TERRA_VM_INVALID;
    if (vm->active && vm->trip_is_gate) { *out = vm->trip_gate; return 1; }
    *out = (TerraVmGateRef){0, 0};
    return 0;
}

int32_t terra_vm_set_parity(TerraCircuitVm* vm, uint32_t net, uint32_t value) {
    if (!vm || net >= vm->net_count || value > 1u) return TERRA_VM_INVALID;
    if (vm->active) return TERRA_VM_STATE;
    uint8_t bit = (uint8_t)(1u << (net & 7));
    if (value) vm->parity[net >> 3] |= bit;
    else vm->parity[net >> 3] &= (uint8_t)~bit;
    return TERRA_VM_OK;
}

int32_t terra_vm_batch_begin(TerraCircuitVm* vm) {
    if (!vm) return TERRA_VM_INVALID;
    if (vm->active || vm->batch_active) return TERRA_VM_STATE;
    if (vm->allocated > vm->maximum) return TERRA_VM_LIMIT;
    if (!vm->batch_checkpoint) {
        uint8_t* snapshot = (uint8_t*)vm_allocate(vm, vm->bit_bytes);
        if (!snapshot) return vm->allocation_error;
        vm->batch_checkpoint = snapshot;
    }
    memcpy(vm->batch_checkpoint, vm->parity, vm->bit_bytes);
    vm->batch_active = 1;
    return TERRA_VM_OK;
}

int32_t terra_vm_batch_commit(TerraCircuitVm* vm) {
    if (!vm) return TERRA_VM_INVALID;
    if (vm->active || !vm->batch_active) return TERRA_VM_STATE;
    vm->batch_active = 0;
    return TERRA_VM_OK;
}

int32_t terra_vm_batch_cancel(TerraCircuitVm* vm) {
    if (!vm) return TERRA_VM_INVALID;
    if (!vm->batch_active) return TERRA_VM_STATE;
    terra_vm_cancel(vm);
    memcpy(vm->parity, vm->batch_checkpoint, vm->bit_bytes);
    vm->batch_active = 0;
    return TERRA_VM_OK;
}

int32_t terra_vm_begin(TerraCircuitVm* vm, const uint32_t* nets, uint32_t count) {
    if (!vm || (count && !nets)) return TERRA_VM_INVALID;
    if (vm->active) return TERRA_VM_STATE;
    if (vm->allocated > vm->maximum) return TERRA_VM_LIMIT;
    for (uint32_t i = 0; i < count; ++i) if (nets[i] >= vm->net_count) return TERRA_VM_INVALID;
    int32_t status = vm_reserve(vm, (void**)&vm->seeds, &vm->seed_capacity, 0, count, sizeof(uint32_t));
    if (status < 0) return status;
    if (count) memcpy(vm->seeds, nets, (size_t)count * sizeof(uint32_t));
    memcpy(vm->checkpoint, vm->parity, vm->bit_bytes);
    vm_reset_queues(vm);
    if (++vm->epoch == 0) {
        if (vm->done) memset(vm->done, 0, (size_t)vm->done_capacity * sizeof(VmDoneSlot));
        vm->epoch = 1;
    }
    vm->active = 1;
    vm->trip_nets = vm->seeds; vm->trip_count = count;
    vm->after_trip = VM_WAVE_END; vm->after_wave = VM_TRIP_BEGIN; vm->phase = VM_WAVE_BEGIN;
    if (vm->callbacks.transaction_begin && vm->callbacks.transaction_begin(vm->context) < 0)
        return vm_fail(vm, TERRA_VM_CALLBACK);
    return TERRA_VM_MORE;
}

int32_t terra_vm_step(TerraCircuitVm* vm, uint32_t max_operations) {
    if (!vm) return TERRA_VM_INVALID;
    if (!vm->active) return TERRA_VM_OK;
    while (max_operations-- && vm->active) {
        int32_t status;
        ++vm->counters.operations;
        switch (vm->phase) {
        case VM_WAVE_BEGIN:
            if (vm->callbacks.wave_begin && vm->callbacks.wave_begin(vm->context) < 0)
                return vm_fail(vm, TERRA_VM_CALLBACK);
            vm->phase = vm->after_wave;
            break;
        case VM_TRIP_BEGIN:
            if (vm->callbacks.trip_begin && vm->callbacks.trip_begin(vm->context) < 0)
                return vm_fail(vm, TERRA_VM_CALLBACK);
            vm->phase = VM_NET_SELECT;
            break;
        case VM_NET_SELECT: {
            if (vm->trip_index == vm->trip_count) { vm->phase = VM_TRIP_END; break; }
            uint32_t net = vm->trip_nets[vm->trip_index++];
            uint8_t bit = (uint8_t)(1u << (net & 7));
            if (vm->trip_seen[net >> 3] & bit) break;
            if (vm->seen_count == UINT32_MAX) return vm_fail(vm, TERRA_VM_LIMIT);
            status = vm_reserve(vm, (void**)&vm->seen_nets, &vm->seen_capacity,
                                vm->seen_count, vm->seen_count + 1u, sizeof(uint32_t));
            if (status < 0) return vm_fail(vm, status);
            vm->seen_nets[vm->seen_count++] = net;
            vm->trip_seen[net >> 3] |= bit;
            vm->parity[net >> 3] ^= bit;
            ++vm->counters.net_pulses;
            if (vm->callbacks.net_hit && vm->callbacks.net_hit(vm->context, net) < 0)
                return vm_fail(vm, TERRA_VM_CALLBACK);
            vm->net = net; vm->net_cursor = 0;
            vm->phase = VM_NET_CANDIDATES;
            break;
        }
        case VM_NET_CANDIDATES: {
            TerraVmGateRef event;
            uint32_t previous_cursor = vm->net_cursor;
            status = vm->callbacks.next_candidate(vm->context, vm->net, &vm->net_cursor, &event);
            if (status < 0) return vm_fail(vm, TERRA_VM_CALLBACK);
            if (!status) { vm->phase = VM_NET_SELECT; break; }
            if (status != 1 || vm->net_cursor == previous_cursor) return vm_fail(vm, TERRA_VM_CALLBACK);
            if (vm->candidate_count == UINT32_MAX) return vm_fail(vm, TERRA_VM_LIMIT);
            status = vm_reserve(vm, (void**)&vm->candidates, &vm->candidate_capacity,
                                vm->candidate_count, vm->candidate_count + 1u, sizeof(TerraVmGateRef));
            if (status < 0) return vm_fail(vm, status);
            vm->candidates[vm->candidate_count++] = event;
            break;
        }
        case VM_TRIP_END:
            if (vm->callbacks.trip_end && vm->callbacks.trip_end(vm->context) < 0)
                return vm_fail(vm, TERRA_VM_CALLBACK);
            vm->seen_clear_index = 0;
            vm->phase = VM_TRIP_CLEAR;
            break;
        case VM_TRIP_CLEAR:
            if (vm->seen_clear_index < vm->seen_count) {
                uint32_t net = vm->seen_nets[vm->seen_clear_index++];
                vm->trip_seen[net >> 3] &= (uint8_t)~(1u << (net & 7));
            } else {
                vm->seen_count = 0;
                vm->trip_is_gate = 0;
                if (vm->after_trip == VM_WAVE_END) vm->after_wave = VM_EVALUATE;
                vm->phase = vm->after_trip;
            }
            break;
        case VM_EVALUATE: {
            if (vm->candidate_index == vm->candidate_count) {
                vm->candidate_count = vm->candidate_index = 0;
                TerraVmGateResult* items = vm->current;
                uint32_t capacity = vm->current_capacity;
                vm->current = vm->next; vm->current_capacity = vm->next_capacity;
                vm->current_count = vm->next_count; vm->current_index = 0;
                vm->next = items; vm->next_capacity = capacity; vm->next_count = 0;
                vm->after_wave = VM_GATES;
                vm->phase = vm->current_count ? VM_WAVE_BEGIN : VM_FINISH;
                break;
            }
            TerraVmGateRef event = vm->candidates[vm->candidate_index++];
            TerraVmGateResult result;
            memset(&result, 0, sizeof(result)); result.identity = event;
            status = vm->callbacks.evaluate(vm->context, event, vm, &result);
            ++vm->counters.gates_evaluated;
            if (status < 0 || status > 1) return vm_fail(vm, TERRA_VM_CALLBACK);
            if (!status) break;
            if (result.count > 4u) return vm_fail(vm, TERRA_VM_CALLBACK);
            for (uint32_t i = 0; i < result.count; ++i)
                if (result.nets[i] >= vm->net_count) return vm_fail(vm, TERRA_VM_CALLBACK);
            if (vm_was_done(vm, result.identity)) {
                ++vm->counters.smoke_events;
                if (vm->callbacks.smoke) vm->callbacks.smoke(vm->context, result.identity);
                break;
            }
            if (vm->next_count == UINT32_MAX) return vm_fail(vm, TERRA_VM_LIMIT);
            status = vm_reserve(vm, (void**)&vm->next, &vm->next_capacity,
                                vm->next_count, vm->next_count + 1u, sizeof(TerraVmGateResult));
            if (status < 0) return vm_fail(vm, status);
            vm->next[vm->next_count++] = result;
            break;
        }
        case VM_GATES: {
            if (vm->current_index == vm->current_count) {
                vm->current_count = vm->current_index = 0;
                vm->after_wave = vm->candidate_count ? VM_EVALUATE : VM_FINISH;
                vm->phase = VM_WAVE_END;
                break;
            }
            TerraVmGateResult* gate = &vm->current[vm->current_index++];
            if (vm_was_done(vm, gate->identity)) break;
            status = vm_mark_done(vm, gate->identity);
            if (status < 0) return vm_fail(vm, status);
            ++vm->counters.gates_fired;
            vm->trip_is_gate = 1; vm->trip_gate = gate->identity;
            vm->trip_nets = gate->nets; vm->trip_count = gate->count; vm->trip_index = 0;
            vm->after_trip = VM_GATES; vm->phase = VM_TRIP_BEGIN;
            break;
        }
        case VM_WAVE_END:
            if (vm->callbacks.wave_end && vm->callbacks.wave_end(vm->context) < 0)
                return vm_fail(vm, TERRA_VM_CALLBACK);
            vm->phase = vm->after_wave;
            break;
        case VM_FINISH:
            vm->active = 0;
            ++vm->counters.completed_pulses;
            if (vm->callbacks.transaction_commit) vm->callbacks.transaction_commit(vm->context);
            vm_reset_queues(vm);
            break;
        default:
            return vm_fail(vm, TERRA_VM_STATE);
        }
    }
    return vm->active ? TERRA_VM_MORE : TERRA_VM_OK;
}

int32_t terra_vm_set_budget(TerraCircuitVm* vm, uint32_t max_bytes) {
    if (!vm) return TERRA_VM_INVALID;
    if (vm->active) return TERRA_VM_STATE;
    vm->maximum = max_bytes ? max_bytes : UINT32_MAX;
    return vm->allocated <= vm->maximum ? TERRA_VM_OK : TERRA_VM_LIMIT;
}

void terra_vm_stats(const TerraCircuitVm* vm, TerraVmStats* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!vm) return;
    *out = vm->counters;
    out->active = vm->active;
    out->queued_candidates = vm->candidate_count - vm->candidate_index;
    out->queued_gates = vm->current_count - vm->current_index + vm->next_count;
    out->allocated_bytes = vm->allocated;
    out->peak_bytes = vm->peak;
}
