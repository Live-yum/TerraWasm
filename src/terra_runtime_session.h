#ifndef TERRA_RUNTIME_SESSION_H
#define TERRA_RUNTIME_SESSION_H

#include <stdint.h>
#include "terra_types.h"
#include "terra_wld_guard_task.h"
#include "terra_render_task.h"

#define TX_RUNTIME_MAX_OPEN_TASKS 4u
#define TX_TASK_SLOT_BITS 8u
#define TX_TASK_SLOT_MASK ((1u << TX_TASK_SLOT_BITS) - 1u)
#define TX_TASK_GENERATION_MASK 0x00ffffffu

/* Cooperative opener state is stored by the runtime-session owner. The task
 * implementation operates on borrowed slots and owns the state transitions. */
typedef struct TxOpenTask {
    uint8_t active;
    uint8_t stage;
    uint8_t allocation_started;
    uint32_t id;

    const uint8_t* buffer;
    uint32_t buffer_len;
    uint8_t* file_data;
    uint32_t copied_bytes;
    uint32_t allocation_mark;

    TxWorld* staging_world;
    TxWldGuardTask guard;
    TxOpenPreviewTask preview;

    uint32_t world_handle;
    uint32_t progress;
    terrax_world_status status;
} TxOpenTask;

TxOpenTask* tx_runtime_session_open_task_slots(void);
uint32_t tx_runtime_session_next_open_task_generation(void);

/* Stream task representation remains private to terra_stream.c. Its pointer
 * slot and generation sequences are nevertheless owned by this module. */
struct StreamTask;
struct StreamTask** tx_runtime_session_stream_task_slot(void);
uint32_t tx_runtime_session_next_stream_generation(void);
uint32_t tx_runtime_session_next_stream_lease_generation(void);

#endif
