/*
 * terra_plr.h -- Stable C ABI for encrypted Terraria player files.
 *
 * The API deliberately follows TerraWasm's existing buffer convention: a
 * NULL/zero output is a successful size probe, while a non-zero output that
 * is too small returns TERRAX_WORLD_STATUS_BUFFER_TOO_SMALL. JSON values are
 * UTF-8 and JSON string results include their terminating NUL in the required
 * size.
 */
#ifndef TERRA_PLR_H
#define TERRA_PLR_H

#include <stdint.h>
#include "terra_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Buffer/document lifecycle. */
terrax_world_status terra_plr_open_from_buffer(
    const uint8_t* buffer,
    uint32_t buffer_len,
    uint32_t* out_handle);

terrax_world_status terra_plr_open(
    const char* path_utf8,
    uint32_t* out_handle);

terrax_world_status terra_plr_open_json(
    const char* json_utf8,
    uint32_t* out_handle);

terrax_world_status terra_plr_close(uint32_t handle);

terrax_world_status terra_plr_save(
    uint32_t handle,
    const char* path_utf8);

terrax_world_status terra_plr_save_to_buffer(
    uint32_t handle,
    uint8_t* output,
    uint32_t capacity,
    uint32_t* out_required);

/* Complete semantic camelCase model. */
terrax_world_status terra_plr_get_json(
    uint32_t handle,
    char* buffer,
    uint64_t buffer_size,
    uint32_t* required_size);

terrax_world_status terra_plr_replace_json(
    uint32_t handle,
    const char* json_utf8);

/* RFC 6901 JSON Pointer field access. */
terrax_world_status terra_plr_get(
    uint32_t handle,
    const char* pointer_utf8,
    char* buffer,
    uint64_t buffer_size,
    uint32_t* required_size);

terrax_world_status terra_plr_set(
    uint32_t handle,
    const char* pointer_utf8,
    const char* value_json_utf8);

terrax_world_status terra_plr_set_many(
    uint32_t handle,
    const char* edits_json_utf8);

/* Workspace ABI v1: atomic subtree journals, bounded field discovery, and
 * explicit release of reconstructible serialization caches. Document ownership
 * lasts until close; releasing caches never discards fields or original bytes. */
uint32_t terra_plr_workspace_abi_version(void);
terrax_world_status terra_plr_get_keys(
    uint32_t handle, const char *pointer_utf8, char *buffer,
    uint64_t buffer_size, uint32_t *required_size);
terrax_world_status terra_plr_release_caches(uint32_t handle);

/* Compatibility aliases used by the TerraR/TerraWasm JS adapter. */
terrax_world_status terra_plr_get_field_json(
    uint32_t handle,
    const char* pointer_utf8,
    char* buffer,
    uint64_t buffer_size,
    uint32_t* required_size);

terrax_world_status terra_plr_set_field_json(
    uint32_t handle,
    const char* pointer_utf8,
    const char* value_json_utf8);

terrax_world_status terra_plr_set_json(
    uint32_t handle,
    const char* json_utf8);

terrax_world_status terra_plr_encode(
    uint32_t handle,
    uint8_t* output,
    uint32_t capacity,
    uint32_t* out_required);

terrax_world_status terra_plr_validate_handle(uint32_t handle);

/* Optional structured compatibility patch accepted by TerraR clients. */
terrax_world_status terra_plr_apply_patch_json(
    uint32_t handle,
    const char* patch_json_utf8);

/* Legacy spelling retained as a thin ABI alias. */
terrax_world_status terra_player_open_from_buffer(
    const uint8_t* buffer,
    uint32_t buffer_len,
    uint32_t* out_handle);
terrax_world_status terra_player_close(uint32_t handle);
terrax_world_status terra_player_get_json(
    uint32_t handle, char* buffer, uint64_t buffer_size, uint32_t* required_size);
terrax_world_status terra_player_replace_json(uint32_t handle, const char* json_utf8);
terrax_world_status terra_player_get_field_json(
    uint32_t handle, const char* pointer_utf8, char* buffer,
    uint64_t buffer_size, uint32_t* required_size);
terrax_world_status terra_player_set_field_json(
    uint32_t handle, const char* pointer_utf8, const char* value_json_utf8);
terrax_world_status terra_player_save_to_buffer(
    uint32_t handle, uint8_t* output, uint32_t capacity, uint32_t* out_required);
terrax_world_status terra_player_set_json(uint32_t handle, const char* json_utf8);
terrax_world_status terra_player_get(
    uint32_t handle, const char* pointer_utf8, char* buffer,
    uint64_t buffer_size, uint32_t* required_size);
terrax_world_status terra_player_set(
    uint32_t handle, const char* pointer_utf8, const char* value_json_utf8);
terrax_world_status terra_player_encode(
    uint32_t handle, uint8_t* output, uint32_t capacity, uint32_t* out_required);
terrax_world_status terra_player_validate_handle(uint32_t handle);
terrax_world_status terra_player_open_json(
    const char* json_utf8, uint32_t* out_handle);
terrax_world_status terra_player_apply_patch_json(
    uint32_t handle, const char* patch_json_utf8);

#ifdef __cplusplus
}
#endif

#endif /* TERRA_PLR_H */
