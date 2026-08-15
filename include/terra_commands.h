/*
 * terra_commands.h -- Versioned binary command buffer ABI.
 */
#ifndef TERRA_COMMANDS_H
#define TERRA_COMMANDS_H

#include <stdint.h>
#include "terra_status.h"

#define TERRAX_COMMAND_MAGIC 0x31435854u /* little-endian "TXC1" */
#define TERRAX_COMMAND_VERSION 1u
#define TERRAX_COMMAND_HEADER_BYTES 16u
#define TERRAX_COMMAND_RECORD_BYTES 8u
#define TERRAX_COMMAND_BATCH_UPDATE_TILES_JSON 1u
#define TERRAX_COMMAND_MAX_COUNT 128u
#define TERRAX_COMMAND_MAX_PAYLOAD (16u * 1024u * 1024u)

terrax_world_status terra_world_apply_commands(
    uint32_t handle,
    const uint8_t* commands,
    uint32_t command_len);

#endif /* TERRA_COMMANDS_H */
