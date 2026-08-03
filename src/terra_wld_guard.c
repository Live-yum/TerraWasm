/*
 * terra_wld_guard.c -- Strict validation for the legacy WLD parser.
 *
 * terra_wld.c retains the established version-specific decoder and serializers.
 * Every externally visible parse_header call is routed through this guard so
 * malformed user-provided buffers fail before legacy code can copy a declared
 * string beyond its section boundary.
 */
#include "terra_types.h"
#include "terra_reader.h"

#include <stdint.h>

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

static int guard_read_u8(
        const uint8_t* data,
        uint32_t length,
        uint32_t* offset,
        uint8_t* value) {
    if (!data || !offset || !value || !terra_reader_has(*offset, 1u, length)) return 0;
    *value = data[(*offset)++];
    return 1;
}

static int guard_read_u16(
        const uint8_t* data,
        uint32_t length,
        uint32_t* offset,
        uint16_t* value) {
    uint32_t current;
    if (!data || !offset || !value || !terra_reader_has(*offset, 2u, length)) return 0;
    current = *offset;
    *value = (uint16_t)(data[current] | ((uint32_t)data[current + 1u] << 8));
    *offset = current + 2u;
    return 1;
}

static int guard_read_u32(
        const uint8_t* data,
        uint32_t length,
        uint32_t* offset,
        uint32_t* value) {
    uint32_t current;
    if (!data || !offset || !value || !terra_reader_has(*offset, 4u, length)) return 0;
    current = *offset;
    *value = (uint32_t)data[current]
        | ((uint32_t)data[current + 1u] << 8)
        | ((uint32_t)data[current + 2u] << 16)
        | ((uint32_t)data[current + 3u] << 24);
    *offset = current + 4u;
    return 1;
}

static int guard_section_view(
        TxWorld* world,
        uint32_t index,
        const uint8_t** data,
        uint32_t* offset,
        uint32_t* length) {
    if (!world || !data || !offset || !length || index >= world->pointer_count) return 0;
    if (index < TX_MAX_SECTION_OVERRIDES && world->section_overrides[index].active) {
        *data = world->section_overrides[index].data;
        *offset = 0u;
        *length = world->section_overrides[index].len;
        return *data != NULL || *length == 0u;
    }
    if (!world->file || world->starts[index] > world->ends[index]
            || world->ends[index] > world->file_len) {
        return 0;
    }
    *data = world->file;
    *offset = world->starts[index];
    *length = world->ends[index];
    return 1;
}

static int guard_absolute_section_offset(
        TxWorld* world,
        uint32_t index,
        uint32_t absolute,
        uint32_t* offset) {
    if (!world || !offset || index >= world->pointer_count) return 0;
    if (index < TX_MAX_SECTION_OVERRIDES && world->section_overrides[index].active) {
        if (absolute < world->starts[index]) return 0;
        absolute -= world->starts[index];
        if (absolute > world->section_overrides[index].len) return 0;
    } else if (absolute < world->starts[index] || absolute > world->ends[index]) {
        return 0;
    }
    *offset = absolute;
    return 1;
}

static int validate_header_prefix(TxWorld* world) {
    const uint8_t* data;
    uint32_t length;
    uint32_t offset;

    if (!world || world->pointer_count == 0u) {
        tx_set_error("TERRAX_BAD_POINTERS", "world has no header section");
        return 0;
    }

    if (!guard_section_view(world, 0u, &data, &offset, &length)) {
        tx_set_error("TERRAX_BAD_POINTERS", "header section bounds are invalid");
        return 0;
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

static int validate_header_late_strings(TxWorld* world) {
    const uint8_t* data;
    uint32_t length;
    uint32_t offset;

    if (!guard_section_view(world, 0u, &data, &offset, &length)) return 0;

    if (world->anglerFinishedSize > 0u) {
        if (!guard_absolute_section_offset(world, 0u, world->anglersOff, &offset)) {
            tx_set_error("TERRAX_TRUNCATED_HEADER", "angler string offset is outside the header section");
            return 0;
        }
        for (uint32_t index = 0u; index < world->anglerFinishedSize; index++) {
            if (!guard_skip_string(data, length, &offset)) {
                tx_set_error("TERRAX_TRUNCATED_HEADER", "angler name exceeds section bounds");
                return 0;
            }
        }
    }

    if (world->version >= 299u) {
        if (!guard_absolute_section_offset(world, 0u, world->maniFestOff, &offset)
                || !guard_skip_string(data, length, &offset)) {
            tx_set_error("TERRAX_TRUNCATED_HEADER", "manifest string exceeds section bounds");
            return 0;
        }
    }
    return 1;
}

static int validate_chest_strings(TxWorld* world) {
    const uint8_t* data;
    uint32_t length;
    uint32_t offset;
    uint16_t chest_count;
    uint16_t legacy_slots = 0u;

    if (world->pointer_count <= 2u) return 1;
    if (!guard_section_view(world, 2u, &data, &offset, &length)) {
        tx_set_error("TERRAX_TRUNCATED_CHESTS", "chest section bounds are invalid");
        return 0;
    }
    if (offset == length) return 1;
    if (!guard_read_u16(data, length, &offset, &chest_count)) {
        tx_set_error("TERRAX_TRUNCATED_CHESTS", "chest section count is truncated");
        return 0;
    }
    if (world->version < 294u
            && !guard_read_u16(data, length, &offset, &legacy_slots)) {
        tx_set_error("TERRAX_TRUNCATED_CHESTS", "legacy chest slot count is truncated");
        return 0;
    }

    for (uint32_t chest = 0u; chest < chest_count; chest++) {
        uint32_t slots = legacy_slots;
        if (!terra_reader_take(&offset, 8u, length)
                || !guard_skip_string(data, length, &offset)) {
            tx_set_error("TERRAX_TRUNCATED_CHESTS", "chest name exceeds section bounds");
            return 0;
        }
        if (world->version >= 294u
                && !guard_read_u32(data, length, &offset, &slots)) {
            tx_set_error("TERRAX_TRUNCATED_CHESTS", "chest slot count is truncated");
            return 0;
        }
        if (slots > 504u) slots = 504u;
        for (uint32_t item = 0u; item < slots; item++) {
            uint16_t stack;
            if (!guard_read_u16(data, length, &offset, &stack)) {
                tx_set_error("TERRAX_TRUNCATED_CHESTS", "chest item stack is truncated");
                return 0;
            }
            if (stack != 0u && !terra_reader_take(&offset, 5u, length)) {
                tx_set_error("TERRAX_TRUNCATED_CHESTS", "chest item data is truncated");
                return 0;
            }
        }
    }
    return 1;
}

static int validate_sign_strings(TxWorld* world) {
    const uint8_t* data;
    uint32_t length;
    uint32_t offset;
    uint16_t sign_count;

    if (world->pointer_count <= 3u) return 1;
    if (!guard_section_view(world, 3u, &data, &offset, &length)) {
        tx_set_error("TERRAX_TRUNCATED_SIGNS", "sign section bounds are invalid");
        return 0;
    }
    if (offset == length) return 1;
    if (!guard_read_u16(data, length, &offset, &sign_count)) {
        tx_set_error("TERRAX_TRUNCATED_SIGNS", "sign section count is truncated");
        return 0;
    }
    for (uint32_t sign = 0u; sign < sign_count; sign++) {
        if (!terra_reader_take(&offset, 8u, length)
                || !guard_skip_string(data, length, &offset)) {
            tx_set_error("TERRAX_TRUNCATED_SIGNS", "sign text exceeds section bounds");
            return 0;
        }
    }
    return 1;
}

static int validate_npc_strings(TxWorld* world) {
    const uint8_t* data;
    uint32_t length;
    uint32_t offset;

    if (world->pointer_count <= 4u) return 1;
    if (!guard_section_view(world, 4u, &data, &offset, &length)) {
        tx_set_error("TERRAX_TRUNCATED_NPCS", "NPC section bounds are invalid");
        return 0;
    }
    if (offset == length) return 1;

    if (world->version >= 268u) {
        uint32_t shimmered_count;
        if (!guard_read_u32(data, length, &offset, &shimmered_count)
                || !terra_reader_take_count(&offset, shimmered_count, 4u, length)) {
            tx_set_error("TERRAX_TRUNCATED_NPCS", "shimmered NPC list is truncated");
            return 0;
        }
    }

    while (offset < length) {
        uint8_t has_npc;
        if (!guard_read_u8(data, length, &offset, &has_npc)) break;
        if (!has_npc) return 1;
        if (!terra_reader_take(&offset, 4u, length)
                || !guard_skip_string(data, length, &offset)
                || !terra_reader_take(&offset, 17u, length)) {
            tx_set_error("TERRAX_TRUNCATED_NPCS", "town NPC name or fixed fields exceed section bounds");
            return 0;
        }
        if (world->version >= 213u) {
            uint8_t has_variation;
            if (!guard_read_u8(data, length, &offset, &has_variation)
                    || (has_variation && !terra_reader_take(&offset, 4u, length))) {
                tx_set_error("TERRAX_TRUNCATED_NPCS", "town NPC variation is truncated");
                return 0;
            }
        }
        if (world->version >= 315u && !terra_reader_take(&offset, 1u, length)) {
            tx_set_error("TERRAX_TRUNCATED_NPCS", "town NPC despawn flag is truncated");
            return 0;
        }
    }

    tx_set_error("TERRAX_TRUNCATED_NPCS", "town NPC list has no terminator");
    return 0;
}

static int validate_bestiary_strings(TxWorld* world) {
    const uint8_t* data;
    uint32_t length;
    uint32_t offset;
    uint32_t count;

    if (world->pointer_count <= 8u) return 1;
    if (!guard_section_view(world, 8u, &data, &offset, &length)) {
        tx_set_error("TERRAX_TRUNCATED_BESTIARY", "bestiary section bounds are invalid");
        return 0;
    }
    if (offset == length) return 1;

    if (!guard_read_u32(data, length, &offset, &count)) {
        tx_set_error("TERRAX_TRUNCATED_BESTIARY", "bestiary kill count is truncated");
        return 0;
    }
    for (uint32_t index = 0u; index < count; index++) {
        if (!guard_skip_string(data, length, &offset)
                || !terra_reader_take(&offset, 4u, length)) {
            tx_set_error("TERRAX_TRUNCATED_BESTIARY", "bestiary kill entry exceeds section bounds");
            return 0;
        }
    }

    if (!guard_read_u32(data, length, &offset, &count)) {
        tx_set_error("TERRAX_TRUNCATED_BESTIARY", "bestiary sighting count is truncated");
        return 0;
    }
    for (uint32_t index = 0u; index < count; index++) {
        if (!guard_skip_string(data, length, &offset)) {
            tx_set_error("TERRAX_TRUNCATED_BESTIARY", "bestiary sighting entry exceeds section bounds");
            return 0;
        }
    }

    if (!guard_read_u32(data, length, &offset, &count)) {
        tx_set_error("TERRAX_TRUNCATED_BESTIARY", "bestiary chat count is truncated");
        return 0;
    }
    for (uint32_t index = 0u; index < count; index++) {
        if (!guard_skip_string(data, length, &offset)) {
            tx_set_error("TERRAX_TRUNCATED_BESTIARY", "bestiary chat entry exceeds section bounds");
            return 0;
        }
    }
    return 1;
}

int terra_validate_string_sections(TxWorld* world) {
    return validate_header_late_strings(world)
        && validate_chest_strings(world)
        && validate_sign_strings(world)
        && validate_npc_strings(world)
        && validate_bestiary_strings(world);
}

int parse_header(TxWorld* world) {
    if (!validate_header_prefix(world)) return 0;
    if (!terra_parse_header_unchecked(world)) return 0;
    return terra_validate_string_sections(world);
}
