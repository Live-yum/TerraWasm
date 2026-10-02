/*
 * terra_task.c -- Cooperative, resumable world-open task ABI.
 */
#include "terra_task.h"
#include "terra_types.h"
#include "terra_world.h"
#include "terra_render_task.h"
#include "terra_wld_guard_task.h"

#include <stddef.h>

/* Keep the resumable guard implementation owned by this task translation unit;
 * it is not part of the normal direct-open parser surface. */
#include "terra_wld_guard_task.c"

#define TX_MAX_OPEN_TASKS 4u
#define TX_TASK_SLOT_BITS 8u
#define TX_TASK_SLOT_MASK ((1u << TX_TASK_SLOT_BITS) - 1u)
#define TX_TASK_GENERATION_MASK 0x00ffffffu

/* One work unit intentionally represents enough useful work to amortize the
 * JS/WASM boundary while remaining short enough for Mini Program yielding. */
#define TX_OPEN_COPY_BYTES_PER_UNIT (256u * 1024u)
#define TX_OPEN_GUARD_RECORDS_PER_UNIT 256u
#define TX_OPEN_PREVIEW_RECORDS_PER_UNIT 16384u

enum TxOpenTaskStage {
    TX_OPEN_STAGE_PREPARE = 0u,
    TX_OPEN_STAGE_COPY = 1u,
    TX_OPEN_STAGE_FORMAT = 2u,
    TX_OPEN_STAGE_HEADER = 3u,
    TX_OPEN_STAGE_STRING_GUARD = 4u,
    TX_OPEN_STAGE_PREVIEW_INIT = 5u,
    TX_OPEN_STAGE_PREVIEW_SCAN = 6u,
    TX_OPEN_STAGE_PREVIEW_ENCODE = 7u,
    TX_OPEN_STAGE_ACTIVATE = 8u,
    TX_OPEN_STAGE_DONE = 9u,
    TX_OPEN_STAGE_CANCELLED = 10u
};

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

static TxOpenTask g_tasks[TX_MAX_OPEN_TASKS];
static uint32_t g_next_task_generation = 1u;

extern void* memset(void* dst, int value, unsigned long n);
extern void* memcpy(void* dst, const void* src, unsigned long n);
extern uint8_t* tx_alloc(uint32_t size);
extern void tx_internal_free(void* ptr);
extern uint32_t tx_mark(void);
extern void tx_rewind(uint32_t mark);
extern uint32_t tx_get_world_open_count(void);
extern void tx_set_error(const char* code, const char* message);
extern int parse_format(TxWorld* world);
extern TxWorld* tx_get_world(uint32_t handle);
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

static int has_active_task(void) {
    for (uint32_t index = 0u; index < TX_MAX_OPEN_TASKS; index++) {
        if (g_tasks[index].active) return 1;
    }
    return 0;
}

/* A staging task can render with TMRT before it publishes a world handle. */
int tx_open_task_pending(void) {
    for (uint32_t index = 0u; index < TX_MAX_OPEN_TASKS; index++) {
        if (g_tasks[index].active &&
            g_tasks[index].status == TERRAX_WORLD_STATUS_IN_PROGRESS) return 1;
    }
    return 0;
}

static TxOpenTask* claim_task_slot(void) {
    /* TerraWasm exposes a one-world runtime contract. Serializing open tasks
     * also makes native allocation rewind ownership unambiguous on cancel. */
    if (has_active_task()) {
        tx_set_error("TERRAX_TASK_LIMIT", "only one world-open task may be active");
        return NULL;
    }
    for (uint32_t index = 0u; index < TX_MAX_OPEN_TASKS; index++) {
        if (!g_tasks[index].active) {
            TxOpenTask* task = &g_tasks[index];
            *task = (TxOpenTask){0};
            task->active = 1u;
            task->id = next_task_id(index);
            task->status = TERRAX_WORLD_STATUS_IN_PROGRESS;
            task->stage = TX_OPEN_STAGE_PREPARE;
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

static void clear_task(TxOpenTask* task) {
    *task = (TxOpenTask){0};
}

static void release_staging(TxOpenTask* task) {
    if (!task) return;
    tx_open_preview_discard(&task->preview);
    if (task->allocation_started) {
        tx_rewind(task->allocation_mark);
    }
    task->allocation_started = 0u;
    task->staging_world = NULL;
    task->file_data = NULL;
    task->copied_bytes = 0u;
    tx_last_ptr = 0u;
    tx_last_len = 0u;
    tx_last_width = 0u;
    tx_last_height = 0u;
}

static terrax_world_status fail_task(TxOpenTask* task, terrax_world_status status) {
    task->status = status;
    task->world_handle = 0u;
    release_staging(task);
    return status;
}

uint32_t terra_world_open_begin(const uint8_t* buffer, uint32_t buffer_len) {
    if (!buffer || buffer_len < 16u) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "null or truncated world buffer");
        return 0u;
    }
    if (tx_get_world_open_count() != 0u) {
        tx_set_error("TERRAX_STATE_ERROR", "close the active world before starting another open task");
        return 0u;
    }
    TxOpenTask* task = claim_task_slot();
    if (!task) return 0u;
    task->buffer = buffer;
    task->buffer_len = buffer_len;
    return task->id;
}

static terrax_world_status prepare_task(TxOpenTask* task) {
    task->allocation_mark = tx_mark();
    task->allocation_started = 1u;
    task->staging_world = (TxWorld*)tx_alloc((uint32_t)sizeof(TxWorld));
    task->file_data = tx_alloc(task->buffer_len);
    if (!task->staging_world || !task->file_data) {
        tx_set_error("TERRAX_WASM_OOM", "failed to allocate incremental world-open state");
        return fail_task(task, TERRAX_WORLD_STATUS_INTERNAL_ERROR);
    }

    memset(task->staging_world, 0, sizeof(TxWorld));
    task->staging_world->allocation_mark = task->allocation_mark;
    task->staging_world->file = task->file_data;
    task->staging_world->file_len = task->buffer_len;
    task->stage = TX_OPEN_STAGE_COPY;
    task->progress = 1u;
    return TERRAX_WORLD_STATUS_IN_PROGRESS;
}

static void copy_task_quantum(TxOpenTask* task) {
    uint32_t remaining = task->buffer_len - task->copied_bytes;
    uint32_t amount = remaining;
    if (amount > TX_OPEN_COPY_BYTES_PER_UNIT) amount = TX_OPEN_COPY_BYTES_PER_UNIT;
    if (amount) {
        memcpy(
            task->file_data + task->copied_bytes,
            task->buffer + task->copied_bytes,
            amount);
        task->copied_bytes += amount;
    }
    if (task->buffer_len) {
        uint64_t scaled = ((uint64_t)task->copied_bytes * 19u) / task->buffer_len;
        task->progress = 1u + (uint32_t)scaled;
    }
    if (task->copied_bytes >= task->buffer_len) {
        task->progress = 20u;
        task->stage = TX_OPEN_STAGE_FORMAT;
    }
}

static terrax_world_status activate_task_world(TxOpenTask* task) {
    uint32_t handle = 0u;
    terrax_world_status status = terra_world_create(&handle);
    if (status != TERRAX_WORLD_STATUS_OK) {
        return fail_task(task, status);
    }

    TxWorld* target = tx_get_world(handle);
    if (!target) {
        (void)terra_world_close(handle);
        tx_set_error("TERRAX_STATE_ERROR", "failed to claim activated world slot");
        return fail_task(task, TERRAX_WORLD_STATUS_STATE_ERROR);
    }

    {
        uint32_t heap_mark = tx_mark();
        *target = *task->staging_world;
        target->active = 1u;
        target->handle = handle;
        target->allocation_mark = task->allocation_mark;
        target->heap_mark = heap_mark;
    }

    tx_internal_free(task->staging_world);
    task->staging_world = NULL;
    task->file_data = NULL;
    task->allocation_started = 0u; /* ownership transferred to target world */
    task->world_handle = handle;
    task->stage = TX_OPEN_STAGE_DONE;
    task->progress = 100u;
    task->status = TERRAX_WORLD_STATUS_OK;
    return task->status;
}

static terrax_world_status advance_one_unit(TxOpenTask* task) {
    switch (task->stage) {
        case TX_OPEN_STAGE_PREPARE:
            return prepare_task(task);

        case TX_OPEN_STAGE_COPY:
            copy_task_quantum(task);
            return TERRAX_WORLD_STATUS_IN_PROGRESS;

        case TX_OPEN_STAGE_FORMAT:
            if (!parse_format(task->staging_world)) {
                return fail_task(task, TERRAX_WORLD_STATUS_PARSE_ERROR);
            }
            task->stage = TX_OPEN_STAGE_HEADER;
            task->progress = 24u;
            return TERRAX_WORLD_STATUS_IN_PROGRESS;

        case TX_OPEN_STAGE_HEADER:
            /* Decode bounded header metadata once, then validate independent
             * variable-length sections with a resumable cursor. */
            if (!tx_wld_guard_task_begin(task->staging_world, &task->guard)) {
                return fail_task(task, TERRAX_WORLD_STATUS_PARSE_ERROR);
            }
            task->stage = TX_OPEN_STAGE_STRING_GUARD;
            task->progress = 28u;
            return TERRAX_WORLD_STATUS_IN_PROGRESS;

        case TX_OPEN_STAGE_STRING_GUARD: {
            int result = tx_wld_guard_task_step(
                task->staging_world,
                &task->guard,
                TX_OPEN_GUARD_RECORDS_PER_UNIT);
            if (result < 0) {
                return fail_task(task, TERRAX_WORLD_STATUS_PARSE_ERROR);
            }
            {
                uint32_t guard_progress = tx_wld_guard_task_progress(&task->guard);
                task->progress = 28u + (guard_progress * 7u) / 100u;
            }
            if (result > 0) {
                task->progress = 35u;
                task->stage = TX_OPEN_STAGE_PREVIEW_INIT;
            }
            return TERRAX_WORLD_STATUS_IN_PROGRESS;
        }

        case TX_OPEN_STAGE_PREVIEW_INIT:
            if (!tx_open_preview_begin(task->staging_world, &task->preview)) {
                return fail_task(task, TERRAX_WORLD_STATUS_INTERNAL_ERROR);
            }
            task->stage = TX_OPEN_STAGE_PREVIEW_SCAN;
            task->progress = 36u;
            return TERRAX_WORLD_STATUS_IN_PROGRESS;

        case TX_OPEN_STAGE_PREVIEW_SCAN: {
            int result = tx_open_preview_step(
                task->staging_world,
                &task->preview,
                TX_OPEN_PREVIEW_RECORDS_PER_UNIT);
            if (result < 0) {
                return fail_task(task, TERRAX_WORLD_STATUS_INTERNAL_ERROR);
            }
            {
                uint32_t preview = tx_open_preview_progress(&task->preview);
                task->progress = 36u + (preview * 54u) / 100u;
            }
            if (result > 0) {
                task->progress = 90u;
                task->stage = TX_OPEN_STAGE_PREVIEW_ENCODE;
            }
            return TERRAX_WORLD_STATUS_IN_PROGRESS;
        }

        case TX_OPEN_STAGE_PREVIEW_ENCODE:
            if (tx_open_preview_finish_png(&task->preview) < 0) {
                return fail_task(task, TERRAX_WORLD_STATUS_INTERNAL_ERROR);
            }
            task->staging_world->media_result = (uint8_t*)(uintptr_t)tx_last_ptr;
            task->staging_world->media_result_len = tx_last_len;
            task->staging_world->media_result_width = tx_last_width;
            task->staging_world->media_result_height = tx_last_height;
            task->staging_world->media_result_kind = 2u;
            task->stage = TX_OPEN_STAGE_ACTIVATE;
            task->progress = 96u;
            return TERRAX_WORLD_STATUS_IN_PROGRESS;

        case TX_OPEN_STAGE_ACTIVATE:
            return activate_task_world(task);

        case TX_OPEN_STAGE_DONE:
            return TERRAX_WORLD_STATUS_OK;

        case TX_OPEN_STAGE_CANCELLED:
            return TERRAX_WORLD_STATUS_CANCELLED;

        default:
            tx_set_error("TERRAX_STATE_ERROR", "invalid incremental world-open stage");
            return fail_task(task, TERRAX_WORLD_STATUS_STATE_ERROR);
    }
}

terrax_world_status terra_world_open_step(uint32_t id, uint32_t work_units) {
    TxOpenTask* task = get_task(id);
    if (!task) return invalid_task();
    if (task->status == TERRAX_WORLD_STATUS_CANCELLED) return task->status;
    if (task->status != TERRAX_WORLD_STATUS_IN_PROGRESS) return task->status;

    /* A zero budget is a real no-op. This makes the parameter a genuine work
     * budget instead of an ignored hint and is useful for scheduler probing. */
    while (work_units-- > 0u && task->status == TERRAX_WORLD_STATUS_IN_PROGRESS) {
        terrax_world_status status = advance_one_unit(task);
        if (status != TERRAX_WORLD_STATUS_IN_PROGRESS) return status;
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
        (void)terra_world_close(task->world_handle);
        task->world_handle = 0u;
    } else {
        release_staging(task);
    }
    task->status = TERRAX_WORLD_STATUS_CANCELLED;
    task->progress = 0u;
    task->stage = TX_OPEN_STAGE_CANCELLED;
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
    if (task->world_handle) {
        (void)terra_world_close(task->world_handle);
        task->world_handle = 0u;
    } else if (task->allocation_started) {
        release_staging(task);
    }
    clear_task(task);
    return TERRAX_WORLD_STATUS_OK;
}
