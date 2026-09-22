#include "terra_output.h"
/*
 * terra_commit.c -- Save and reopen a world through one caller-owned buffer.
 *
 * The normal JS transaction path saves the WLD into WASM, copies it into JS,
 * closes the world, then copies the same bytes back into WASM to reopen it.
 * This ABI preserves the established two-call save contract, but on the fill
 * call closes and reopens directly from the caller's bridge allocation. The
 * output remains available for JS persistence while the new world owns its
 * normal native copy.
 */
#include "terra_world.h"
#include "terra_types.h"

extern TxWorld* tx_get_world(uint32_t handle);
extern void tx_set_error(const char* code, const char* message);

terrax_world_status terra_world_commit_to_buffer(
    uint32_t handle,
    uint8_t* output,
    uint32_t capacity,
    uint32_t* out_required,
    uint32_t* out_new_handle)
{
    if (!out_required || !out_new_handle) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "commit output pointers are required");
        return TERRAX_WORLD_STATUS_INVALID_ARGUMENT;
    }
    *out_new_handle = 0u;

    terrax_world_status status = terra_world_save_to_buffer(
        handle,
        output,
        capacity,
        out_required);
    if (status != TERRAX_WORLD_STATUS_OK) return status;

    /* Probe call: save_to_buffer has prepared any queued pixel-art override
     * and reported the exact size, but the current world remains open. */
    if (!output && capacity == 0u) return TERRAX_WORLD_STATUS_OK;

    /* save_to_buffer already rejected a null/undersized output. The bridge
     * allocation is independent from the world's native allocation mark, so
     * it remains valid while close rewinds the old world and open copies the
     * serialized bytes into the replacement session. */
    TxWorld* old_world = tx_get_world(handle);
    TxPreparedOutput* prepared = old_world->prepared_output;
    uint32_t decodes = old_world->tile_decode_calls;
    old_world->prepared_output = NULL;
    status = terra_world_close(handle);
    if (status != TERRAX_WORLD_STATUS_OK) { old_world->prepared_output = prepared; return status; }

    status = terra_world_open_from_buffer(
        output,
        *out_required,
        out_new_handle);
    if (status != TERRAX_WORLD_STATUS_OK) {
        tx_output_free(prepared);
        *out_new_handle = 0u;
        return status;
    }

    TxWorld* reopened = tx_get_world(*out_new_handle);
    reopened->prepared_output = prepared;
    reopened->tile_decode_calls = decodes;
    return TERRAX_WORLD_STATUS_OK;
}
