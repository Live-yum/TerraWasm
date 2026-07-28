/*
 * terra_task.h -- Cooperative world-open task ABI.
 *
 * The task keeps the caller-owned input buffer alive by contract until
 * terra_world_task_close. It performs parsing and first-thumbnail rendering
 * in bounded cooperative steps while preserving the synchronous V2 API.
 */
#ifndef TERRA_TASK_H
#define TERRA_TASK_H

#include <stdint.h>
#include "terra_status.h"

uint32_t terra_world_open_begin(const uint8_t* buffer, uint32_t buffer_len);
terrax_world_status terra_world_open_step(uint32_t task, uint32_t work_units);
terrax_world_status terra_world_open_finish(uint32_t task, uint32_t* out_handle);
terrax_world_status terra_world_open_cancel(uint32_t task);
uint32_t terra_world_task_get_progress(uint32_t task);
terrax_world_status terra_world_task_get_status(uint32_t task);
uint32_t terra_world_task_get_world_handle(uint32_t task);
terrax_world_status terra_world_task_close(uint32_t task);

#endif /* TERRA_TASK_H */
