/*
 * terra_world.h -- Public V2 API header for TerraWasm.
 *
 * Declares the exported C functions matching the TERRAX_WORLD_V2_API_SPEC.
 * All buffer sizes use uint64_t per the spec. Internally we use uint32_t
 * (WASM is 32-bit) but the API boundary must match the spec exactly.
 */
#ifndef TERRA_WORLD_H
#define TERRA_WORLD_H

#include <stdint.h>
#include "terra_status.h"
#include "terra_task.h"
#include "terra_commands.h"

/*
 * Lifecycle
 */
terrax_world_status terra_world_open(
    const char*           path_utf8,
    uint32_t* out_handle);

terrax_world_status terra_world_create(
    uint32_t* out_handle);

terrax_world_status terra_world_close(
    uint32_t handle);

terrax_world_status terra_world_save(
    uint32_t handle,
    const char*           path_utf8);

terrax_world_status terra_world_save_to_buffer(
    uint32_t handle,
    uint8_t* output,
    uint32_t capacity,
    uint32_t* out_required);

terrax_world_status terra_world_open_from_buffer(
    const uint8_t* buffer,
    uint32_t buffer_len,
    uint32_t* out_handle);

/* One idle-owner transaction; rollback/commit allocate nothing. The token
 * protects this exact handle, rejects nesting and cannot outlive the world. */
uint32_t terra_world_workspace_abi_version(void);
terrax_world_status terra_world_workspace_checkpoint_size(uint32_t handle, uint32_t* out_bytes);
terrax_world_status terra_world_workspace_begin(uint32_t handle, uint32_t max_bytes, uint32_t* out_token);
terrax_world_status terra_world_workspace_save_to_buffer(uint32_t handle, uint8_t* output, uint32_t capacity, uint32_t* out_required);
terrax_world_status terra_world_workspace_commit(uint32_t handle, uint32_t token);
terrax_world_status terra_world_workspace_rollback(uint32_t handle, uint32_t token);

/*
 * Info
 */
terrax_world_status terra_info_get_last_error_json(
    char*     buffer,
    uint64_t  buffer_size,
    uint64_t* required_size);

terrax_world_status terra_info_list_sections_json(
    char*     buffer,
    uint64_t  buffer_size,
    uint64_t* required_size);

terrax_world_status terra_info_get_section_schema_json(
    const char* section_name,
    char*       buffer,
    uint64_t    buffer_size,
    uint64_t*   required_size);

/*
 * Section read/write
 */
terrax_world_status terra_section_get_json(
    uint32_t handle,
    const char*          section_name,
    char*                buffer,
    uint64_t             buffer_size,
    uint64_t*            required_size);

terrax_world_status terra_section_set_json(
    uint32_t handle,
    const char*          section_name,
    const char*          json_utf8);

/*
 * Operations
 */
terrax_world_status terra_op_execute_json(
    uint32_t handle,
    const char*          operation_name,
    const char*          request_json,
    char*                response_buffer,
    uint64_t             response_buffer_size,
    uint64_t*            required_size);

terrax_world_status terra_op_get_thumbnail_png(
    uint32_t handle,
    uint8_t*             buffer,
    uint64_t             buffer_size,
    uint64_t*            required_size,
    uint32_t*            width,
    uint32_t*            height);

terrax_world_status terra_op_get_map(
    uint32_t handle,
    uint8_t*             buffer,
    uint64_t             buffer_size,
    uint64_t*            required_size,
    uint32_t*            width,
    uint32_t*            height);

#endif /* TERRA_WORLD_H */
