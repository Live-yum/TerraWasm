#include "terra_output.h"
/*
 * terra_commit.c -- Save and reopen a world through one caller-owned buffer.
 *
 * The normal JS transaction path saves the WLD into WASM, copies it into JS,
 * closes the world, then copies the same bytes back into WASM to reopen it.
 * This ABI preserves the established two-call save contract, but the fill
 * validates an independently owned replacement before closing the old
 * world. The final ownership swap allocates nothing; a failed candidate leaves
 * the old handle usable. The caller's output remains independently owned.
 */
#include "terra_world.h"
#include "terra_types.h"
#include "terra_checkpoint.h"
#include <string.h>

extern TxWorld* tx_get_world(uint32_t handle);
extern void tx_set_error(const char* code, const char* message);
extern void tx_clear_error(void);
extern uint8_t* tx_persistent_alloc(uint32_t size);
extern void tx_internal_free(void* pointer);
extern int tx_stream_activate_world(TxWorld* candidate, uint32_t* out_handle);
extern int parse_format(TxWorld* world);
extern int parse_header(TxWorld* world);
extern uint32_t tx_mark(void);
extern void tx_rewind(uint32_t mark);
/* parse_format uses native scratch for ancient frame-important metadata. The
 * replacement retains that bitmap in the tail of its one persistent file root,
 * not in the old world's rewind domain. No new ownership field is required. */
static terrax_world_status tx_commit_prepare_candidate(const uint8_t* bytes,uint32_t length,TxWorld* candidate) {
    if(length<16u||length>TX_MAX_INPUT_BYTES) {
        tx_set_error("TERRAX_INVALID_ARGUMENT","replacement world size is outside supported limits");return TERRAX_WORLD_STATUS_INVALID_ARGUMENT;
    }
    uint32_t version=(uint32_t)bytes[0]|((uint32_t)bytes[1]<<8)|((uint32_t)bytes[2]<<16)|((uint32_t)bytes[3]<<24);
    uint32_t legacy_bytes=version<88u?8192u:0u;
    if(length>UINT32_MAX-legacy_bytes){tx_set_error("TERRAX_WASM_OOM","replacement ownership size overflow");return TERRAX_WORLD_STATUS_INTERNAL_ERROR;}
    candidate->file=tx_persistent_alloc(length+legacy_bytes);
    if(!candidate->file){tx_set_error("TERRAX_WASM_OOM","replacement world allocation failed; old world retained");return TERRAX_WORLD_STATUS_INTERNAL_ERROR;}
    memcpy(candidate->file,bytes,length);candidate->file_len=length;candidate->stream_owned=1u;
    uint32_t mark=tx_mark();
    int valid=parse_format(candidate)&&parse_header(candidate)&&tx_validate_future_tiles(candidate);
    if(valid&&candidate->legacy_wld) {
        valid=candidate->important&&candidate->important_len<=legacy_bytes;
        if(valid){memcpy(candidate->file+length,candidate->important,candidate->important_len);candidate->important=candidate->file+length;}
    }
    tx_rewind(mark);
    if(!valid){tx_internal_free(candidate->file);candidate->file=NULL;return TERRAX_WORLD_STATUS_PARSE_ERROR;}
    return TERRAX_WORLD_STATUS_OK;
}

uint32_t terra_world_commit_abi_version(void) { return 1u; }

terrax_world_status terra_world_commit_to_buffer(
    uint32_t handle,
    uint8_t* output,
    uint32_t capacity,
    uint32_t* out_required,
    uint32_t* out_new_handle)
{
    if (tx_checkpoint_active()) {
        tx_set_error("TERRAX_STATE_ERROR", "destructive commit is forbidden during workspace transaction");
        return TERRAX_WORLD_STATUS_STATE_ERROR;
    }
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

    /* All replacement allocation and parsing precedes the only destructive
     * step. Persistent candidate roots survive the old world's arena rewind. */
    TxWorld candidate={0};
    status=tx_commit_prepare_candidate(output,*out_required,&candidate);
    if(status!=TERRAX_WORLD_STATUS_OK)return status;
    TxWorld* old_world=tx_get_world(handle);
    TxPreparedOutput* prepared=old_world->prepared_output;
    candidate.prepared_output=prepared;
    candidate.tile_decode_calls=old_world->tile_decode_calls;
    old_world->prepared_output=NULL;
    if(!tx_stream_activate_world(&candidate,out_new_handle)) {
        old_world->prepared_output=prepared;
        tx_internal_free(candidate.file);
        return TERRAX_WORLD_STATUS_STATE_ERROR;
    }
    tx_clear_error();
    return TERRAX_WORLD_STATUS_OK;
}

/* Serialize and validate without replacing the active workspace. Call under a
 * checkpoint: even save's deferred pixel materialization can then roll back.
 * The output is caller-owned and remains valid through durable publication. */
terrax_world_status terra_world_workspace_save_to_buffer(uint32_t handle, uint8_t* output,
    uint32_t capacity, uint32_t* out_required) {
    if (!tx_checkpoint_active()) {
        tx_set_error("TERRAX_STATE_ERROR", "workspace save requires a rollback checkpoint");
        return TERRAX_WORLD_STATUS_STATE_ERROR;
    }
    terrax_world_status status = terra_world_save_to_buffer(handle, output, capacity, out_required);
    if (status != TERRAX_WORLD_STATUS_OK || !output || !capacity) return status;
    uint32_t mark = tx_mark();
    TxWorld candidate = {0};
    candidate.file = output; candidate.file_len = *out_required;
    int valid = parse_format(&candidate) && parse_header(&candidate) && tx_validate_future_tiles(&candidate);
    tx_rewind(mark);
    return valid ? TERRAX_WORLD_STATUS_OK : TERRAX_WORLD_STATUS_PARSE_ERROR;
}
