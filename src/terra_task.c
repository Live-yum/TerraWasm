/*
 * terra_task.c -- Cooperative world-open task ABI.
 */
#include "terra_task.h"
#include "terra_types.h"
#include "terra_world.h"

#include <stddef.h>

#define TX_MAX_OPEN_TASKS 4u
#define TX_TASK_SLOT_BITS 8u
#define TX_TASK_SLOT_MASK ((1u << TX_TASK_SLOT_BITS) - 1u)
#define TX_TASK_GENERATION_MASK 0x00ffffffu

typedef struct TxOpenTask {
    uint8_t active;
    uint8_t stage;
    uint32_t id;
    const uint8_t* buffer;
    uint32_t buffer_len;
    uint32_t world_handle;
    uint32_t progress;
    terrax_world_status status;
} TxOpenTask;

static TxOpenTask g_tasks[TX_MAX_OPEN_TASKS];
static uint32_t g_next_task_generation = 1u;

extern void tx_set_error(const char* code, const char* message);
extern TxWorld* tx_get_world(uint32_t handle);
extern int32_t txw_render_preview_png(TxWorld* world, uint32_t max_w, uint32_t max_h);
extern uint32_t tx_last_ptr;
extern uint32_t tx_last_len;
extern uint32_t tx_last_width;
extern uint32_t tx_last_height;

static uint32_t next_task_id(uint32_t slot_index) {
    uint32_t generation = g_next_task_generation++ & TX_TASK_GENERATION_MASK;
    if (generation == 0u) {
        generation = 1u;
        g_next_task_generation = 2u;
    }
    return (generation << TX_TASK_SLOT_BITS) | (slot_index + 1u);
}

static TxOpenTask* get_task(uint32_t id) {
    uint32_t encoded_slot = id & TX_TASK_SLOT_MASK;
    if (!id || encoded_slot == 0u || encoded_slot > TX_MAX_OPEN_TASKS) return NULL;
    TxOpenTask* task = &g_tasks[encoded_slot - 1u];
    return task->active && task->id == id ? task : NULL;
}

static TxOpenTask* claim_task_slot(void) {
    for (uint32_t index = 0; index < TX_MAX_OPEN_TASKS; index++) {
        if (!g_tasks[index].active) {
            TxOpenTask* task = &g_tasks[index];
            *task = (TxOpenTask){0};
            task->active = 1u;
            task->id = next_task_id(index);
            task->status = TERRAX_WORLD_STATUS_IN_PROGRESS;
            return task;
        }
    }
    tx_set_error("TERRAX_TASK_LIMIT", "too many open world tasks");
    return NULL;
}

static terrax_world_status invalid_task(void) {
    tx_set_error("TERRAX_INVALID_TASK", "task is stale or invalid");
    return TERRAX_WORLD_STATUS_STATE_ERROR;
}

uint32_t terra_world_open_begin(const uint8_t* buffer, uint32_t buffer_len) {
    if (!buffer || buffer_len < 16u) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "null or truncated world buffer");
        return 0u;
    }
    TxOpenTask* task = claim_task_slot();
    if (!task) return 0u;
    task->buffer = buffer;
    task->buffer_len = buffer_len;
    return task->id;
}

static void clear_task(TxOpenTask* task) {
    *task = (TxOpenTask){0};
}

terrax_world_status terra_world_open_step(uint32_t id, uint32_t work_units) {
    (void)work_units;
    TxOpenTask* task = get_task(id);
    if (!task) return invalid_task();
    if (task->status == TERRAX_WORLD_STATUS_CANCELLED) return task->status;
    if (task->status != TERRAX_WORLD_STATUS_IN_PROGRESS) return task->status;

    if (task->stage == 0u) {
        uint32_t handle = 0u;
        terrax_world_status status = terra_world_open_from_buffer(
            task->buffer, task->buffer_len, &handle);
        if (status != TERRAX_WORLD_STATUS_OK) {
            task->status = status;
            task->progress = 0u;
            return status;
        }
        task->world_handle = handle;
        task->stage = 1u;
        task->progress = 70u;
        return TERRAX_WORLD_STATUS_IN_PROGRESS;
    }

    if (task->stage == 1u) {
        TxWorld* world = tx_get_world(task->world_handle);
        if (!world) {
            task->status = TERRAX_WORLD_STATUS_STATE_ERROR;
            task->progress = 0u;
            return task->status;
        }
        if (txw_render_preview_png(world, 384u, 0u) < 0) {
            terra_world_close(task->world_handle);
            task->world_handle = 0u;
            task->status = TERRAX_WORLD_STATUS_INTERNAL_ERROR;
            task->progress = 0u;
            return task->status;
        }
        world->media_result = (uint8_t*)(uintptr_t)tx_last_ptr;
        world->media_result_len = tx_last_len;
        world->media_result_width = tx_last_width;
        world->media_result_height = tx_last_height;
        world->media_result_kind = 2u;
        task->stage = 2u;
        task->progress = 100u;
        task->status = TERRAX_WORLD_STATUS_OK;
        return task->status;
    }

    return task->status;
}

terrax_world_status terra_world_open_finish(uint32_t id, uint32_t* out_handle) {
    if (out_handle) *out_handle = 0u;
    TxOpenTask* task = get_task(id);
    if (!task) return invalid_task();
    if (task->status != TERRAX_WORLD_STATUS_OK || task->world_handle == 0u) {
        tx_set_error("TERRAX_TASK_NOT_READY", "world open task is not complete");
        return task->status == TERRAX_WORLD_STATUS_IN_PROGRESS
            ? TERRAX_WORLD_STATUS_IN_PROGRESS
            : TERRAX_WORLD_STATUS_STATE_ERROR;
    }
    if (!out_handle) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "null world handle output");
        return TERRAX_WORLD_STATUS_INVALID_ARGUMENT;
    }
    *out_handle = task->world_handle;
    task->world_handle = 0u;
    return TERRAX_WORLD_STATUS_OK;
}

terrax_world_status terra_world_open_cancel(uint32_t id) {
    TxOpenTask* task = get_task(id);
    if (!task) return invalid_task();
    if (task->world_handle) {
        terra_world_close(task->world_handle);
        task->world_handle = 0u;
    }
    task->status = TERRAX_WORLD_STATUS_CANCELLED;
    task->progress = 0u;
    task->stage = 3u;
    return task->status;
}

uint32_t terra_world_task_get_progress(uint32_t id) {
    TxOpenTask* task = get_task(id);
    return task ? task->progress : 0u;
}

terrax_world_status terra_world_task_get_status(uint32_t id) {
    TxOpenTask* task = get_task(id);
    return task ? task->status : invalid_task();
}

uint32_t terra_world_task_get_world_handle(uint32_t id) {
    TxOpenTask* task = get_task(id);
    return task ? task->world_handle : 0u;
}

terrax_world_status terra_world_task_close(uint32_t id) {
    TxOpenTask* task = get_task(id);
    if (!task) return invalid_task();
    if (task->world_handle) terra_world_close(task->world_handle);
    clear_task(task);
    return TERRAX_WORLD_STATUS_OK;
}
