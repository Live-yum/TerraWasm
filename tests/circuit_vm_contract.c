#include "terra_circuit_vm.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/* Standalone allocator with deterministic failures; no production allocator or
 * world parser is replaced in the full Computerraria acceptance test. */
typedef union Allocation { size_t bytes; max_align_t alignment; } Allocation;
static size_t live_bytes, live_allocations, allocations;
static size_t fail_after = SIZE_MAX;
uint8_t* tx_persistent_alloc(uint32_t bytes) {
    if (allocations++ == fail_after) return NULL;
    Allocation* a = (Allocation*)malloc(sizeof(Allocation) + bytes);
    if (!a) return NULL;
    a->bytes = bytes; live_bytes += bytes; ++live_allocations;
    return (uint8_t*)(a + 1);
}
void tx_persistent_free(void* data) {
    if (!data) return;
    Allocation* a = (Allocation*)data - 1;
    live_bytes -= a->bytes; --live_allocations; free(a);
}

enum { NETS = 16, GATES = 4096 };
typedef struct Gate {
    uint32_t identity, condition[4], condition_count, bias, outputs[4], output_count;
    uint32_t sample_limit;
    uint32_t ordinary, frame, and_inputs;
} Gate;
typedef struct Model {
    TerraCircuitVm* vm;
    Gate gates[GATES];
    uint32_t events[NETS][GATES], counts[NETS];
    uint32_t pixel, pixel_checkpoint, intersection;
    uint32_t wave_pixel, wave_pixel_checkpoint, wave_intersection, wave_starts, wave_ends;
    uint32_t wave_open, trip_open;
    uint32_t transaction_begins, commits, rollbacks, trips, smokes;
    uint32_t hit_count, fail_hit;
    uint32_t random_samples[4], random_count;
    uint32_t inspect_gate_origin, external_trips, gate_trips, expected_gate[4];
} Model;

static void event(Model* m, uint32_t net, uint32_t gate) {
    m->events[net][m->counts[net]++] = gate;
}
static void gate(Model* m, uint32_t index, uint32_t bias, uint32_t out) {
    m->gates[index].identity = index;
    m->gates[index].bias = bias;
    if (out < NETS) { m->gates[index].outputs[0] = out; m->gates[index].output_count = 1; }
}
static int32_t next(void* context, uint32_t net, uint32_t* cursor, TerraVmGateRef* out) {
    Model* m = (Model*)context;
    if (*cursor >= m->counts[net]) return 0;
    out->group = net; out->offset = m->events[net][(*cursor)++];
    return 1;
}
static int32_t evaluate(void* context, TerraVmGateRef event_ref,
                         const TerraCircuitVm* vm, TerraVmGateResult* out) {
    Model* m = (Model*)context;
    assert(!m->wave_open && !m->trip_open);
    assert(event_ref.offset < GATES);
    Gate* g = &m->gates[event_ref.offset];
    uint32_t state = g->and_inputs ? 1u : g->bias;
    for (uint32_t i = 0; i < g->condition_count; ++i) {
        uint32_t on = (uint32_t)terra_vm_parity(vm, g->condition[i]);
        if (g->and_inputs) state &= on; else state ^= on;
    }
    if (g->sample_limit) {
        assert(m->random_count < 4);
        /* Source CheckLogicGate: Next(numLamps) < numOn. The independent
         * deterministic sample tape exposes FIFO assignment between gates. */
        uint32_t sample = m->random_samples[m->random_count++];
        assert(sample < g->sample_limit); state = sample < state;
    }
    out->identity.group = 0; out->identity.offset = g->identity;
    out->count = g->output_count;
    memcpy(out->nets, g->outputs, sizeof(out->nets));
    if (g->ordinary) {
        uint32_t changed = state != g->frame;
        g->frame = state;
        return (int32_t)changed;
    }
    return (int32_t)state;
}
static int32_t begin_trip(void* context) {
    Model* m = (Model*)context; assert(m->wave_open && !m->trip_open);
    if (m->inspect_gate_origin) {
        TerraVmGateRef ref = {UINT32_MAX, UINT32_MAX};
        int origin = terra_vm_current_gate(m->vm, &ref);
        if (origin == 0) {
            assert(!m->external_trips && ref.group == 0 && ref.offset == 0);
            ++m->external_trips;
        } else {
            assert(origin == 1 && ref.group == 0 && m->gate_trips < 4);
            assert(ref.offset == m->expected_gate[m->gate_trips++]);
        }
    }
    m->trip_open = 1; m->intersection = 0; return 0;
}
static int32_t hit(void* context, uint32_t net) {
    Model* m = (Model*)context;
    if (net == 10) m->intersection |= 1;
    if (net == 11) m->intersection |= 2;
    if (net == 10) m->wave_intersection ^= 1;
    if (net == 11) m->wave_intersection ^= 2;
    if (++m->hit_count == m->fail_hit) return -1;
    return 0;
}
static int32_t end_trip(void* context) {
    Model* m = (Model*)context;
    assert(m->wave_open && m->trip_open); m->trip_open = 0;
    if (m->intersection == 3) m->pixel ^= 1;
    ++m->trips; return 0;
}
static void smoke(void* context, TerraVmGateRef identity) { (void)identity; ++((Model*)context)->smokes; }
static int32_t begin_transaction(void* context) {
    Model* m = (Model*)context; m->pixel_checkpoint = m->pixel; m->wave_pixel_checkpoint = m->wave_pixel;
    ++m->transaction_begins; return 0;
}
static void commit(void* context) { ++((Model*)context)->commits; }
static void rollback(void* context) {
    Model* m = (Model*)context; m->pixel = m->pixel_checkpoint; m->wave_pixel = m->wave_pixel_checkpoint;
    m->wave_open = m->trip_open = 0; ++m->rollbacks;
}
static int32_t begin_wave(void* context) {
    Model* m = (Model*)context; assert(!m->wave_open && !m->trip_open);
    m->wave_open = 1; m->wave_intersection = 0; ++m->wave_starts; return 0;
}
static int32_t end_wave(void* context) {
    Model* m = (Model*)context; assert(m->wave_open && !m->trip_open);
    if (m->wave_intersection == 3) m->wave_pixel ^= 1;
    m->wave_open = 0; ++m->wave_ends; return 0;
}
static const TerraVmCallbacks callbacks = {
    .next_candidate = next, .evaluate = evaluate, .trip_begin = begin_trip,
    .net_hit = hit, .trip_end = end_trip, .smoke = smoke,
    .transaction_begin = begin_transaction, .transaction_commit = commit,
    .transaction_rollback = rollback, .wave_begin = begin_wave, .wave_end = end_wave
};
static TerraCircuitVm* create(Model* m, uint32_t budget) {
    TerraCircuitVm* vm = NULL;
    assert(terra_vm_create(NETS, budget, &callbacks, m, &vm) == 0 && vm);
    m->vm = vm;
    return vm;
}
static void run(TerraCircuitVm* vm, const uint32_t* seeds, uint32_t count, uint32_t quantum) {
    assert(terra_vm_begin(vm, seeds, count) == TERRA_VM_MORE);
    int32_t status;
    do { status = terra_vm_step(vm, quantum); } while (status == TERRA_VM_MORE);
    assert(status == 0);
}
static void clean(TerraCircuitVm* vm) {
    terra_vm_destroy(vm); assert(live_bytes == 0 && live_allocations == 0);
}

static void smoke_and_repeat(void) {
    Model* m = calloc(1, sizeof(Model)); assert(m);
    gate(m, 0, 1, 0); event(m, 0, 0);
    TerraCircuitVm* vm = create(m, 0);
    uint32_t seed = 0; run(vm, &seed, 1, UINT32_MAX);
    TerraVmStats s; terra_vm_stats(vm, &s);
    assert(s.net_pulses == 2 && s.gates_evaluated == 2 && s.gates_fired == 1 && s.smoke_events == 1);
    assert(terra_vm_parity(vm, 0) == 0 && m->smokes == 1);
    run(vm, &seed, 1, 1);
    terra_vm_stats(vm, &s);
    assert(s.gates_fired == 2 && s.completed_pulses == 2 && m->smokes == 2);
    clean(vm); free(m);
}

static void full_wave_before_condition(void) {
    Model* m = calloc(1, sizeof(Model)); assert(m);
    gate(m, 0, 1, 1); gate(m, 1, 1, 1); gate(m, 2, 0, 3);
    m->gates[2].condition[0] = 1; m->gates[2].condition_count = 1;
    event(m, 0, 0); event(m, 0, 1); event(m, 1, 2);
    TerraCircuitVm* vm = create(m, 0);
    uint32_t seed = 0; run(vm, &seed, 1, 1);
    TerraVmStats s; terra_vm_stats(vm, &s);
    assert(s.gates_fired == 2 && s.gates_evaluated == 4 && s.net_pulses == 3);
    assert(terra_vm_parity(vm, 1) == 0 && terra_vm_parity(vm, 3) == 0);
    clean(vm); free(m);
}

static void colours_and_canonical_identity(void) {
    Model* m = calloc(1, sizeof(Model)); assert(m);
    gate(m, 0, 1, 2); gate(m, 1, 1, 2); m->gates[1].identity = 0;
    event(m, 0, 0); event(m, 1, 1);
    TerraCircuitVm* vm = create(m, 0);
    uint32_t seeds[] = {0, 0, 1}; run(vm, seeds, 3, 3);
    TerraVmStats s; terra_vm_stats(vm, &s);
    assert(s.net_pulses == 3 && s.gates_evaluated == 2 && s.gates_fired == 1 && s.smoke_events == 0);
    assert(terra_vm_parity(vm, 2) == 1);
    clean(vm);
    memset(m, 0, sizeof(*m));
    gate(m, 0, 0, 2); event(m, 0, 0);
    m->gates[0].condition[0] = 0; m->gates[0].condition[1] = 1; m->gates[0].condition_count = 2;
    vm = create(m, 0); run(vm, seeds, 3, 1);
    assert(terra_vm_parity(vm, 2) == 0);
    clean(vm); free(m);
}

static void pixel_trip_boundary(void) {
    Model* m = calloc(1, sizeof(Model)); assert(m);
    gate(m, 0, 1, 10); gate(m, 1, 1, 11);
    event(m, 0, 0); event(m, 0, 1);
    TerraCircuitVm* vm = create(m, 0);
    uint32_t seed = 0; run(vm, &seed, 1, 1);
    assert(m->pixel == 0 && m->trips == 3);
    assert(m->wave_pixel == 1 && m->wave_starts == 2 && m->wave_ends == 2);
    uint32_t intersection[] = {10, 11, 10}; run(vm, intersection, 3, 2);
    assert(m->pixel == 1);
    assert(m->wave_pixel == 0 && m->wave_starts == 3 && m->wave_ends == 3);
    /* Repeated axis hits from distinct gates are parity, not an OR extended
     * across the wave. This source-specific adapter leaves vanilla pixels alone. */
    gate(m, 2, 1, 10); event(m, 0, 2);
    run(vm, &seed, 1, 1);
    assert(m->pixel == 1 && m->wave_pixel == 0);
    clean(vm); free(m);
}

static void cancel_and_callback_failure(void) {
    Model* m = calloc(1, sizeof(Model)); assert(m);
    gate(m, 0, 1, 2); event(m, 10, 0);
    TerraCircuitVm* vm = create(m, 0);
    uint32_t seeds[] = {10, 11};
    assert(terra_vm_begin(vm, seeds, 2) == 1);
    while (!m->trips) assert(terra_vm_step(vm, 1) == 1);
    assert(m->pixel == 1);
    assert(terra_vm_cancel(vm) == 0);
    assert(m->pixel == 0 && m->rollbacks == 1 && terra_vm_parity(vm, 10) == 0);
    m->fail_hit = m->hit_count + 3;
    assert(terra_vm_begin(vm, seeds, 2) == 1);
    assert(terra_vm_step(vm, UINT32_MAX) == TERRA_VM_CALLBACK);
    assert(m->pixel == 0 && m->rollbacks == 2 && terra_vm_parity(vm, 2) == 0);
    m->fail_hit = 0;
    run(vm, seeds, 2, 1);
    assert(m->pixel == 1 && m->commits == 1);
    clean(vm); free(m);
}

static void large_wave_and_oom(void) {
    Model* m = calloc(1, sizeof(Model)); assert(m);
    for (uint32_t i = 0; i < GATES; ++i) { gate(m, i, 1, 2); event(m, 0, i); }
    uint32_t seed = 0;
    for (uint32_t offset = 0; offset < 36; ++offset) {
        fail_after = SIZE_MAX;
        TerraCircuitVm* vm = create(m, 0);
        fail_after = allocations + offset;
        int32_t status = terra_vm_begin(vm, &seed, 1);
        if (status == TERRA_VM_MORE) {
            do { status = terra_vm_step(vm, 7); } while (status == TERRA_VM_MORE);
        }
        assert(status == TERRA_VM_OOM || status == 0);
        TerraVmStats s; terra_vm_stats(vm, &s); assert(!s.active);
        if (status == TERRA_VM_OOM) {
            assert(terra_vm_parity(vm, 0) == 0 && terra_vm_parity(vm, 2) == 0);
        } else {
            assert(s.gates_fired == GATES && terra_vm_parity(vm, 2) == 0);
        }
        fail_after = SIZE_MAX; clean(vm);
    }
    TerraCircuitVm* vm = create(m, 32768);
    assert(terra_vm_begin(vm, &seed, 1) == 1);
    assert(terra_vm_step(vm, UINT32_MAX) == TERRA_VM_LIMIT);
    assert(terra_vm_parity(vm, 0) == 0);
    clean(vm); free(m);
}

static void argument_contract(void) {
    Model* m = calloc(1, sizeof(Model)); assert(m);
    TerraCircuitVm* vm = create(m, 0);
    uint32_t invalid = NETS, seed = 0;
    assert(terra_vm_begin(vm, &invalid, 1) == TERRA_VM_INVALID);
    assert(terra_vm_begin(vm, NULL, 1) == TERRA_VM_INVALID);
    assert(terra_vm_set_parity(vm, 0, 1) == 0);
    assert(terra_vm_begin(vm, &seed, 1) == 1);
    assert(terra_vm_set_parity(vm, 0, 0) == TERRA_VM_STATE);
    assert(terra_vm_set_budget(vm, 1) == TERRA_VM_STATE);
    assert(terra_vm_begin(vm, &seed, 1) == TERRA_VM_STATE);
    assert(terra_vm_step(vm, 0) == 1);
    assert(terra_vm_cancel(vm) == 0 && terra_vm_parity(vm, 0) == 1);
    assert(terra_vm_set_budget(vm, 1) == TERRA_VM_LIMIT);
    assert(terra_vm_begin(vm, &seed, 1) == TERRA_VM_LIMIT);
    assert(terra_vm_set_budget(vm, 0) == 0);
    run(vm, NULL, 0, 1);
    clean(vm); free(m);
    for (uint32_t i = 0; i < 4; ++i) {
        fail_after = allocations + i;
        TerraCircuitVm* failed = NULL;
        assert(terra_vm_create(NETS, 0, &callbacks, NULL, &failed) == TERRA_VM_OOM);
        assert(!failed && !live_allocations);
    }
    fail_after = SIZE_MAX;
}

static void batch_atomicity(void) {
    Model* m = calloc(1, sizeof(Model)); assert(m);
    TerraCircuitVm* vm = create(m, 0);
    uint32_t seed = 0, second = 1;
    assert(terra_vm_set_parity(vm, 4, 1) == 0);
    assert(terra_vm_batch_commit(vm) == TERRA_VM_STATE);
    assert(terra_vm_batch_cancel(vm) == TERRA_VM_STATE);
    assert(terra_vm_batch_begin(vm) == 0);
    assert(terra_vm_batch_begin(vm) == TERRA_VM_STATE);
    run(vm, &seed, 1, 1);
    assert(terra_vm_parity(vm, 0) == 1 && terra_vm_parity(vm, 4) == 1);
    assert(terra_vm_begin(vm, &second, 1) == 1);
    assert(terra_vm_step(vm, 3) == 1 && terra_vm_parity(vm, 1) == 1);
    assert(terra_vm_batch_commit(vm) == TERRA_VM_STATE);
    assert(terra_vm_batch_cancel(vm) == 0);
    assert(terra_vm_parity(vm, 0) == 0 && terra_vm_parity(vm, 1) == 0 && terra_vm_parity(vm, 4) == 1);
    assert(m->commits == 1 && m->rollbacks == 1);
    assert(terra_vm_batch_begin(vm) == 0);
    run(vm, &seed, 1, 3); run(vm, &second, 1, 3);
    assert(terra_vm_batch_commit(vm) == 0);
    assert(terra_vm_parity(vm, 0) == 1 && terra_vm_parity(vm, 1) == 1);
    /* A later batch snapshots the committed state, not the initial zeroes. */
    assert(terra_vm_batch_begin(vm) == 0);
    run(vm, &seed, 1, 3);
    m->fail_hit = m->hit_count + 1;
    assert(terra_vm_begin(vm, &second, 1) == 1);
    assert(terra_vm_step(vm, 100) == TERRA_VM_CALLBACK);
    assert(terra_vm_batch_cancel(vm) == 0);
    assert(terra_vm_parity(vm, 0) == 1 && terra_vm_parity(vm, 1) == 1);
    /* Outer batches do not silently replace per-pulse callback transactions. */
    assert(m->transaction_begins == 6 && m->commits == 4 && m->rollbacks == 2);
    clean(vm); free(m);
}

static void batch_budget_and_failed_begin(void) {
    Model* m = calloc(1, sizeof(Model)); assert(m);
    TerraCircuitVm* vm = create(m, 0);
    assert(terra_vm_set_parity(vm, 2, 1) == 0);
    TerraVmStats before, after; terra_vm_stats(vm, &before);
    fail_after = allocations;
    assert(terra_vm_batch_begin(vm) == TERRA_VM_OOM);
    fail_after = SIZE_MAX;
    terra_vm_stats(vm, &after);
    assert(before.allocated_bytes == after.allocated_bytes && !after.active);
    assert(terra_vm_parity(vm, 2) == 1 && terra_vm_batch_cancel(vm) == TERRA_VM_STATE);
    assert(terra_vm_set_budget(vm, before.allocated_bytes) == 0);
    assert(terra_vm_batch_begin(vm) == TERRA_VM_LIMIT);
    assert(terra_vm_parity(vm, 2) == 1 && !m->transaction_begins);
    assert(terra_vm_set_budget(vm, 0) == 0);
    assert(terra_vm_batch_begin(vm) == 0);
    terra_vm_stats(vm, &after); assert(after.allocated_bytes > before.allocated_bytes);
    size_t allocated = allocations;
    assert(terra_vm_batch_commit(vm) == 0);
    assert(terra_vm_batch_begin(vm) == 0 && allocations == allocated);
    /* Destroy also frees an active outer snapshot, with or without an inner pulse. */
    clean(vm); free(m);
}

static void probability_assignment_requires_source_order(void) {
    /* Physical source fixture: a horizontal red wire at y=3, x=3..7. Its
     * branches reach faulty lamps (3,4) and (6,4), each above one on and one off
     * ordinary lamp. Starting at (7,3), Terraria's FIFO (down/up/right/left)
     * reaches the right lamp first. Sorting physical gate columns reverses it. */
    const int dx[] = {0, 0, 1, -1}, dy[] = {1, -1, 0, 0};
    int queue[16][2] = {{7,3}}, visited[12][12] = {{0}}, order[2], found = 0, count = 1;
    visited[7][3] = 1;
    for (int at = 0; at < count; ++at) {
        int x = queue[at][0], y = queue[at][1];
        if (y == 4) order[found++] = x == 3 ? 0 : 1;
        for (int direction = 0; direction < 4; ++direction) {
            int nx = x + dx[direction], ny = y + dy[direction];
            int wired = (ny == 3 && nx >= 3 && nx <= 7) || (ny == 4 && (nx == 3 || nx == 6));
            if (wired && !visited[nx][ny]) {
                visited[nx][ny] = 1; queue[count][0] = nx; queue[count++][1] = ny;
            }
        }
    }
    assert(found == 2 && order[0] == 1 && order[1] == 0);
    for (uint32_t sorted = 0; sorted < 2; ++sorted) {
        Model* m = calloc(1, sizeof(Model)); assert(m);
        gate(m, 0, 1, 4); gate(m, 1, 1, 5);
        m->gates[0].sample_limit = m->gates[1].sample_limit = 2;
        m->random_samples[0] = 0; m->random_samples[1] = 1;
        event(m, 0, sorted ? 0u : (uint32_t)order[0]);
        event(m, 0, sorted ? 1u : (uint32_t)order[1]);
        TerraCircuitVm* vm = create(m, 0); uint32_t seed = 0;
        run(vm, &seed, 1, 1);
        assert(m->random_count == 2);
        assert(terra_vm_parity(vm, 4) == (int)sorted);
        assert(terra_vm_parity(vm, 5) == (int)!sorted);
        clean(vm); free(m);
    }
}

static void physical_gate_output_origin(void) {
    Model* m = calloc(1, sizeof(Model)); assert(m);
    gate(m, 5, 1, 1); m->gates[5].identity = 500; event(m, 0, 5);
    gate(m, 9, 1, 2); m->gates[9].identity = 900; event(m, 1, 9);
    event(m, 2, 9); /* A later smoke event is not another output TripWire. */
    m->inspect_gate_origin = 1; m->expected_gate[0] = 500; m->expected_gate[1] = 900;
    TerraCircuitVm* vm = create(m, 0);
    TerraVmGateRef ref = {1, 2}; uint32_t seed = 0;
    assert(terra_vm_current_gate(NULL, &ref) == TERRA_VM_INVALID);
    assert(terra_vm_current_gate(vm, NULL) == TERRA_VM_INVALID);
    assert(terra_vm_current_gate(vm, &ref) == 0 && ref.group == 0 && ref.offset == 0);
    assert(terra_vm_begin(vm, &seed, 1) == TERRA_VM_MORE);
    assert(terra_vm_current_gate(vm, &ref) == 0);
    while (!m->gate_trips) assert(terra_vm_step(vm, 1) == TERRA_VM_MORE);
    assert(terra_vm_current_gate(vm, &ref) == 1 && ref.offset == 500);
    assert(terra_vm_cancel(vm) == 0);
    assert(terra_vm_current_gate(vm, &ref) == 0 && ref.offset == 0);
    m->external_trips = m->gate_trips = 0;
    run(vm, &seed, 1, 1);
    assert(m->external_trips == 1 && m->gate_trips == 2 && m->smokes == 1);
    assert(terra_vm_current_gate(vm, &ref) == 0 && ref.group == 0 && ref.offset == 0);
    clean(vm); free(m);
}

static void deterministic_candidate_order(void) {
    /* A and B fire together, toggling the two lamps of ordinary AND gate C.
     * C must observe both lamp changes before it is checked, and its repeated
     * lamp event must not emit again. C drives ordinary gate D, which toggles
     * C's first lamp back off. C updates its frame but emits only source smoke
     * because its physical gate has already fired in this external pulse.
     * Reversing A/B's candidate order and changing the yield quantum preserve
     * all deterministic lamp states, gate frames and smoke counts. */
    for (uint32_t order = 0; order < 2; ++order) {
        for (uint32_t tiny = 0; tiny < 2; ++tiny) {
            Model* m = calloc(1, sizeof(Model)); assert(m);
            gate(m, 0, 1, 1); gate(m, 1, 1, 2);
            event(m, 0, order ? 1 : 0); event(m, 0, order ? 0 : 1);
            gate(m, 2, 0, 3); m->gates[2].ordinary = m->gates[2].and_inputs = 1;
            m->gates[2].condition[0] = 1; m->gates[2].condition[1] = 2;
            m->gates[2].condition_count = 2; event(m, 1, 2); event(m, 2, 2);
            gate(m, 3, 0, 1); m->gates[3].ordinary = 1;
            m->gates[3].condition[0] = 3; m->gates[3].condition_count = 1; event(m, 3, 3);
            TerraCircuitVm* vm = create(m, 0); uint32_t seed = 0;
            run(vm, &seed, 1, tiny ? 1u : UINT32_MAX);
            assert(terra_vm_parity(vm, 1) == 0 && terra_vm_parity(vm, 2) == 1 && terra_vm_parity(vm, 3) == 1);
            assert(m->gates[2].frame == 0 && m->gates[3].frame == 1 && m->smokes == 1);
            TerraVmStats s; terra_vm_stats(vm, &s);
            assert(s.gates_fired == 4 && s.gates_evaluated == 6 && s.net_pulses == 5 && s.smoke_events == 1);
            clean(vm); free(m);
        }
    }
}

int main(void) {
    smoke_and_repeat(); full_wave_before_condition(); colours_and_canonical_identity();
    pixel_trip_boundary(); cancel_and_callback_failure(); large_wave_and_oom(); argument_contract();
    batch_atomicity(); batch_budget_and_failed_begin();
    probability_assignment_requires_source_order();
    physical_gate_output_origin();
    deterministic_candidate_order();
    puts("circuit VM: gate waves, deterministic candidate order, source/gate identity, pixel boundaries, yielding, pulse/batch rollback, 4096-gate waves, 36 OOM points passed");
    return 0;
}
