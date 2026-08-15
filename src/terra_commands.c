/*
 * terra_commands.c -- Versioned binary command buffer ABI.
 */
#include "terra_commands.h"
#include "terra_reader.h"
#include "terra_types.h"

extern TxWorld* tx_get_world(uint32_t handle);
extern void tx_clear_error(void);
extern void tx_set_error(const char* code, const char* message);
extern uint8_t* tx_alloc(uint32_t size);
extern void tx_internal_free(void* ptr);
extern void* memcpy(void* dst, const void* src, unsigned long n);
extern void buf_init(TxBuf* b, uint32_t cap);
extern int execute_batch_update_tiles(TxWorld* world, const char* request, int jlen, TxBuf* response);
extern uint32_t tx_last_ptr;
extern uint32_t tx_last_len;

static uint16_t read_u16(const uint8_t* bytes) {
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t read_u32(const uint8_t* bytes) {
    return (uint32_t)bytes[0]
        | ((uint32_t)bytes[1] << 8)
        | ((uint32_t)bytes[2] << 16)
        | ((uint32_t)bytes[3] << 24);
}

static int valid_json_payload(const uint8_t* payload, uint32_t payload_len) {
    if (!payload || payload_len < 2u) return 0;
    uint32_t first = 0u;
    while (first < payload_len &&
           (payload[first] == ' ' || payload[first] == '\n' || payload[first] == '\r' || payload[first] == '\t')) {
        first++;
    }
    uint32_t last = payload_len;
    while (last > first &&
           (payload[last - 1u] == ' ' || payload[last - 1u] == '\n' || payload[last - 1u] == '\r' || payload[last - 1u] == '\t')) {
        last--;
    }
    return first < last && payload[first] == '{' && payload[last - 1u] == '}';
}

static terrax_world_status malformed(const char* message) {
    tx_set_error("TERRAX_COMMAND_INVALID", message);
    return TERRAX_WORLD_STATUS_INVALID_ARGUMENT;
}

static terrax_world_status validate_commands(
    const uint8_t* commands,
    uint32_t command_len,
    uint32_t* out_count) {
    if (!commands || command_len < TERRAX_COMMAND_HEADER_BYTES) {
        return malformed("command buffer header is truncated");
    }
    if (read_u32(commands) != TERRAX_COMMAND_MAGIC) {
        return malformed("command buffer magic is invalid");
    }
    if (read_u16(commands + 4u) != TERRAX_COMMAND_VERSION) {
        return malformed("command buffer version is unsupported");
    }
    uint32_t count = read_u32(commands + 8u);
    uint32_t payload_bytes = read_u32(commands + 12u);
    if (count == 0u || count > TERRAX_COMMAND_MAX_COUNT) {
        return malformed("command count is outside the supported range");
    }
    if (payload_bytes > TERRAX_COMMAND_MAX_PAYLOAD ||
        payload_bytes != command_len - TERRAX_COMMAND_HEADER_BYTES) {
        return malformed("command payload length is invalid");
    }

    uint32_t offset = TERRAX_COMMAND_HEADER_BYTES;
    for (uint32_t index = 0u; index < count; index++) {
        if (!terra_reader_has(offset, TERRAX_COMMAND_RECORD_BYTES, command_len)) {
            return malformed("command record header is truncated");
        }
        uint16_t opcode = read_u16(commands + offset);
        uint32_t record_len = read_u32(commands + offset + 4u);
        offset += TERRAX_COMMAND_RECORD_BYTES;
        if (opcode != TERRAX_COMMAND_BATCH_UPDATE_TILES_JSON) {
            return malformed("command opcode is unsupported");
        }
        if (record_len > TERRAX_COMMAND_MAX_PAYLOAD ||
            !terra_reader_has(offset, record_len, command_len)) {
            return malformed("command record payload is truncated");
        }
        if (!valid_json_payload(commands + offset, record_len)) {
            return malformed("command JSON payload is invalid");
        }
        offset += record_len;
    }
    if (offset != command_len) return malformed("command buffer has trailing bytes");
    if (out_count) *out_count = count;
    return TERRAX_WORLD_STATUS_OK;
}

terrax_world_status terra_world_apply_commands(
    uint32_t handle,
    const uint8_t* commands,
    uint32_t command_len) {
    TxWorld* world = tx_get_world(handle);
    if (!world) {
        tx_set_error("TERRAX_INVALID_HANDLE", "world handle is stale or invalid");
        return TERRAX_WORLD_STATUS_STATE_ERROR;
    }

    uint32_t count = 0u;
    terrax_world_status status = validate_commands(commands, command_len, &count);
    if (status != TERRAX_WORLD_STATUS_OK) return status;

    uint32_t offset = TERRAX_COMMAND_HEADER_BYTES;
    for (uint32_t index = 0u; index < count; index++) {
        uint16_t opcode = read_u16(commands + offset);
        uint32_t record_len = read_u32(commands + offset + 4u);
        offset += TERRAX_COMMAND_RECORD_BYTES;
        if (opcode == TERRAX_COMMAND_BATCH_UPDATE_TILES_JSON) {
            uint8_t* request = tx_alloc(record_len + 1u);
            if (!request) {
                tx_set_error("TERRAX_WASM_OOM", "command request allocation failed");
                return TERRAX_WORLD_STATUS_INTERNAL_ERROR;
            }
            memcpy(request, commands + offset, record_len);
            request[record_len] = 0u;
            TxBuf response;
            buf_init(&response, 256u);
            int result = execute_batch_update_tiles(world, (const char*)request, (int)record_len, &response);
            tx_internal_free(request);
            if (response.data) tx_internal_free(response.data);
            tx_last_ptr = 0u;
            tx_last_len = 0u;
            if (result < 0) return TERRAX_WORLD_STATUS_VALIDATION_ERROR;
        }
        offset += record_len;
    }
    tx_clear_error();
    return TERRAX_WORLD_STATUS_OK;
}
