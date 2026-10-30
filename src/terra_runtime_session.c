/*
 * terra_runtime_session.c -- Shared runtime task and lease identity owner.
 *
 * Task implementations keep their state machines local, but the registries,
 * active stream-task slot, and generation sequences are centralized here.
 * Per-world caches and source lease fields are embedded in TxRuntimeSession.
 */
#include "terra_runtime_session.h"

static TxOpenTask open_tasks[TX_RUNTIME_MAX_OPEN_TASKS];
static uint32_t open_task_generation = 1u;
static struct StreamTask* stream_task;
static uint32_t stream_generation = 1u;
static uint32_t stream_lease_generation = 1u;

TxOpenTask* tx_runtime_session_open_task_slots(void) {
    return open_tasks;
}

uint32_t tx_runtime_session_next_open_task_generation(void) {
    uint32_t generation = open_task_generation++ & TX_TASK_GENERATION_MASK;
    if (generation == 0u) {
        generation = 1u;
        open_task_generation = 2u;
    }
    return generation;
}

struct StreamTask** tx_runtime_session_stream_task_slot(void) {
    return &stream_task;
}

uint32_t tx_runtime_session_next_stream_generation(void) {
    uint32_t value = stream_generation++;
    if (value == 0u) value = stream_generation++;
    return value;
}

uint32_t tx_runtime_session_next_stream_lease_generation(void) {
    uint32_t value = stream_lease_generation++;
    if (value == 0u) value = stream_lease_generation++;
    return value;
}
