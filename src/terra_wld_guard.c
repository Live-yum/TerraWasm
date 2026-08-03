/*
 * terra_wld_guard.c -- Strict prefix validation for the legacy WLD header parser.
 *
 * terra_wld.c retains the established version-specific decoder, but its string
 * helper historically assumed that the declared 7-bit length was in bounds.
 * Every externally visible parse_header call is routed through this guard so
 * malformed user-provided buffers fail before the legacy decoder can copy.
 */
#include "terra_types.h"
#include "terra_reader.h"

extern int terra_parse_header_unchecked(TxWorld* world);
extern void tx_set_error(const char* code, const char* message);

static int guard_read_7bit(
        const uint8_t* data,
        uint32_t length,
        uint32_t* offset,
        uint32_t* value) {
    uint32_t result = 0u;
    uint32_t shift = 0u;
    if (!data || !offset || !value) return 0;

    for (uint32_t index = 0u; index < 5u; index++) {
        if (*offset >= length) return 0;
        uint8_t byte = data[(*offset)++];
        if (index == 4u && (byte & 0x7fu) > 0x0fu) return 0;
        result |= (uint32_t)(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0u) {
            *value = result;
            return 1;
        }
        shift += 7u;
    }
    return 0;
}

static int guard_skip_string(
        const uint8_t* data,
        uint32_t length,
        uint32_t* offset) {
    uint32_t string_length = 0u;
    if (!guard_read_7bit(data, length, offset, &string_length)) return 0;
    return terra_reader_take(offset, string_length, length);
}

static int validate_header_prefix(TxWorld* world) {
    const uint8_t* data;
    uint32_t length;
    uint32_t offset;

    if (!world || !world->file || world->pointer_count == 0u) return 0;
    data = world->file;
    length = world->file_len;
    offset = world->starts[0];

    if (world->section_overrides[0].active) {
        data = world->section_overrides[0].data;
        length = world->section_overrides[0].len;
        offset = 0u;
    }

    if (!data || offset > length || !guard_skip_string(data, length, &offset)) {
        tx_set_error("TERRAX_TRUNCATED_HEADER", "world name exceeds section bounds");
        return 0;
    }

    if (world->version >= 179u) {
        if (world->version == 179u) {
            if (!terra_reader_take(&offset, 4u, length)) {
                tx_set_error("TERRAX_TRUNCATED_HEADER", "numeric world seed exceeds section bounds");
                return 0;
            }
        } else if (!guard_skip_string(data, length, &offset)) {
            tx_set_error("TERRAX_TRUNCATED_HEADER", "world seed exceeds section bounds");
            return 0;
        }

        if (!terra_reader_take(&offset, 8u, length)) {
            tx_set_error("TERRAX_TRUNCATED_HEADER", "world generator version exceeds section bounds");
            return 0;
        }
    }

    if (world->version >= 181u && !terra_reader_take(&offset, 16u, length)) {
        tx_set_error("TERRAX_TRUNCATED_HEADER", "world UUID exceeds section bounds");
        return 0;
    }

    return 1;
}

int parse_header(TxWorld* world) {
    if (!validate_header_prefix(world)) return 0;
    return terra_parse_header_unchecked(world);
}
