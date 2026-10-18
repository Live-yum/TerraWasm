#ifndef TERRA_OUTCOME_H
#define TERRA_OUTCOME_H
#include <stdint.h>
#include "terra_status.h"
#ifdef __cplusplus
extern "C" {
#endif
#define TERRA_OUTCOME_VERSION 2u
#define TERRA_OUTCOME_JOIN_CALLER 1u
enum TerraOutcomeEffect { TERRA_EFFECT_NONE, TERRA_EFFECT_APPLIED, TERRA_EFFECT_ROLLED_BACK, TERRA_EFFECT_UNKNOWN };
enum TerraOutcomeTransaction { TERRA_TX_IDLE, TERRA_TX_OPEN, TERRA_TX_COMMITTED, TERRA_TX_ROLLED_BACK, TERRA_TX_INVALID };
/* Caller-owned fixed control record. Native layout is not a wire protocol.
 * The record requires no allocation and is valid after a normal API return.
 * Revisions count successful workspace commits for this world handle, not the
 * WLD header revision or host persistence. Legacy out-of-checkpoint mutation
 * must not be fenced using this counter as a universal document revision.
 * Bind the record to the caller's operation ID outside this native structure. */
typedef struct TerraOutcomeV2 {
    uint32_t struct_size, version;
    int32_t status;
    uint32_t effect, transaction_state, reserved;
    uint64_t revision_before, revision_after;
} TerraOutcomeV2;
/* Pointer-native additive interfaces. Wasm callers use the checked wire adapters.
 * Without JOIN_CALLER, mutations reject an existing checkpoint without changing
 * it. With JOIN_CALLER, success leaves it OPEN; mutation failure rolls back the
 * ENTIRE caller checkpoint, including earlier provisional edits. Query failures
 * and response-cache fills do not roll back caller edits. UNKNOWN is reserved
 * for a violated internal finish/rollback invariant, never guessed as rollback. */
uint32_t terra_world_outcome_abi_version(void);
terrax_world_status terra_op_execute_json_outcome(uint32_t handle, const char* operation,
    const char* request, char* response, uint64_t capacity, uint64_t* required,
    uint32_t options, TerraOutcomeV2* outcome);
terrax_world_status terra_world_workspace_commit_outcome(uint32_t handle, uint32_t token, TerraOutcomeV2* outcome);
terrax_world_status terra_world_workspace_rollback_outcome(uint32_t handle, uint32_t token, TerraOutcomeV2* outcome);
/* Little-endian wire record: 40 bytes, explicitly encoded (not native memcpy).
 * All wire buffers must be live bridge allocations. Strings must start at an
 * allocation root and contain a terminator. Invalid control storage rejects
 * before execution; it cannot promise a writable receipt. */
#define TERRA_OUTCOME_WIRE_BYTES 40u
terrax_world_status terra_op_execute_json_receipt(uint32_t handle, const char* operation,
    const char* request, char* response, uint64_t capacity, uint64_t* required,
    uint32_t options, uint8_t* receipt, uint32_t receipt_capacity);
terrax_world_status terra_world_workspace_commit_receipt(uint32_t handle, uint32_t token,
    uint8_t* receipt, uint32_t receipt_capacity);
terrax_world_status terra_world_workspace_rollback_receipt(uint32_t handle, uint32_t token,
    uint8_t* receipt, uint32_t receipt_capacity);
#ifdef __cplusplus
}
#endif
#endif
