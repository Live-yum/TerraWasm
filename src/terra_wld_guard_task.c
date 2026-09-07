/*
 * terra_wld_guard_task.c -- Resumable validation for variable-length WLD sections.
 *
 * The legacy header decoder remains authoritative for fixed header fields. This
 * companion path keeps the external chest/sign/NPC/bestiary validation cursor
 * between world-open steps so large or adversarial section counts cannot force
 * one open_step() call to walk the whole section.
 */
#include "terra_wld_guard_task.h"
#include "terra_reader.h"

#include <stdint.h>

extern int terra_parse_header_unchecked(TxWorld* world);
extern void tx_set_error(const char* code, const char* message);

#define TX_GUARD_STAGE_CHESTS_INIT 0u
#define TX_GUARD_STAGE_CHEST_HEADER 1u
#define TX_GUARD_STAGE_CHEST_ITEMS 2u
#define TX_GUARD_STAGE_SIGNS_INIT 3u
#define TX_GUARD_STAGE_SIGNS 4u
#define TX_GUARD_STAGE_NPCS_INIT 5u
#define TX_GUARD_STAGE_NPCS 6u
#define TX_GUARD_STAGE_BESTIARY_KILLS_INIT 7u
#define TX_GUARD_STAGE_BESTIARY_KILLS 8u
#define TX_GUARD_STAGE_BESTIARY_SIGHTINGS_INIT 9u
#define TX_GUARD_STAGE_BESTIARY_SIGHTINGS 10u
#define TX_GUARD_STAGE_BESTIARY_CHATS_INIT 11u
#define TX_GUARD_STAGE_BESTIARY_CHATS 12u
#define TX_GUARD_STAGE_DONE 13u

static int task_guard_read_7bit(
        const uint8_t* data, uint32_t length,
        uint32_t* offset, uint32_t* value) {
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

static int task_guard_skip_string(
        const uint8_t* data, uint32_t length, uint32_t* offset) {
    uint32_t string_length = 0u;
    if (!task_guard_read_7bit(data, length, offset, &string_length)) return 0;
    return terra_reader_take(offset, string_length, length);
}

static int task_guard_read_u8(
        const uint8_t* data, uint32_t length,
        uint32_t* offset, uint8_t* value) {
    if (!data || !offset || !value || !terra_reader_has(*offset, 1u, length)) return 0;
    *value = data[(*offset)++];
    return 1;
}

static int task_guard_read_u16(
        const uint8_t* data, uint32_t length,
        uint32_t* offset, uint16_t* value) {
    uint32_t current;
    if (!data || !offset || !value || !terra_reader_has(*offset, 2u, length)) return 0;
    current = *offset;
    *value = (uint16_t)(data[current] | ((uint32_t)data[current + 1u] << 8));
    *offset = current + 2u;
    return 1;
}

static int task_guard_read_u32(
        const uint8_t* data, uint32_t length,
        uint32_t* offset, uint32_t* value) {
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

static int task_guard_section_view(
        TxWorld* world, uint32_t index,
        const uint8_t** data, uint32_t* offset, uint32_t* length) {
    if (!world || !data || !offset || !length || index >= world->pointer_count) return 0;
    if (index < TX_MAX_SECTION_OVERRIDES && world->section_overrides[index].active) {
        *data = world->section_overrides[index].data;
        *offset = 0u;
        *length = world->section_overrides[index].len;
        return *data != NULL || *length == 0u;
    }
    if (!world->file || world->starts[index] > world->ends[index]
            || world->ends[index] > world->file_len) return 0;
    *data = world->file;
    *offset = world->starts[index];
    *length = world->ends[index];
    return 1;
}

static int task_guard_absolute_section_offset(
        TxWorld* world, uint32_t index,
        uint32_t absolute, uint32_t* offset) {
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

static int task_validate_header_prefix(TxWorld* world) {
    const uint8_t* data;
    uint32_t length;
    uint32_t offset;
    if (!world || world->pointer_count == 0u) {
        tx_set_error("TERRAX_BAD_POINTERS", "world has no header section");
        return 0;
    }
    if (!task_guard_section_view(world, 0u, &data, &offset, &length)) {
        tx_set_error("TERRAX_BAD_POINTERS", "header section bounds are invalid");
        return 0;
    }
    if (!data || offset > length || !task_guard_skip_string(data, length, &offset)) {
        tx_set_error("TERRAX_TRUNCATED_HEADER", "world name exceeds section bounds");
        return 0;
    }
    if (world->version >= 179u) {
        if (world->version == 179u) {
            if (!terra_reader_take(&offset, 4u, length)) {
                tx_set_error("TERRAX_TRUNCATED_HEADER", "numeric world seed exceeds section bounds");
                return 0;
            }
        } else if (!task_guard_skip_string(data, length, &offset)) {
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

static int task_validate_recorded_header_offsets(TxWorld* world) {
    const uint8_t* data;
    uint32_t length;
    uint32_t start;
    uint32_t section_length;
    if (!task_guard_section_view(world, 0u, &data, &start, &length)) return 0;
    (void)data;
    section_length = world->section_overrides[0].active ? length : length - start;
    for (uint32_t index = 0u; index < world->header_bool_field_count; index++) {
        if (world->header_bool_fields[index].section_offset >= section_length) {
            tx_set_error("TERRAX_TRUNCATED_HEADER", "fixed header fields exceed section bounds");
            return 0;
        }
    }
    return 1;
}

/* Header late strings are part of the legacy header byte stream: their ends
 * are needed to locate later fixed fields. Keep that bounded header decode
 * authoritative, then move the independent external sections to the resumable
 * state machine below. */
static int task_validate_header_late_strings(TxWorld* world) {
    const uint8_t* data;
    uint32_t length;
    uint32_t offset;
    if (!task_validate_recorded_header_offsets(world)) return 0;
    if (!task_guard_section_view(world, 0u, &data, &offset, &length)) return 0;
    if (world->anglerFinishedSize > 0u) {
        if (!task_guard_absolute_section_offset(world, 0u, world->anglersOff, &offset)) {
            tx_set_error("TERRAX_TRUNCATED_HEADER", "angler string offset is outside the header section");
            return 0;
        }
        for (uint32_t index = 0u; index < world->anglerFinishedSize; index++) {
            if (!task_guard_skip_string(data, length, &offset)) {
                tx_set_error("TERRAX_TRUNCATED_HEADER", "angler name exceeds section bounds");
                return 0;
            }
        }
    }
    if (world->version >= 299u) {
        if (!task_guard_absolute_section_offset(world, 0u, world->maniFestOff, &offset)
                || !task_guard_skip_string(data, length, &offset)) {
            tx_set_error("TERRAX_TRUNCATED_HEADER", "manifest string exceeds section bounds");
            return 0;
        }
    }
    return 1;
}

static void task_guard_reset_section(TxWldGuardTask* task) {
    task->data = NULL;
    task->length = 0u;
    task->offset = 0u;
    task->index = 0u;
    task->count = 0u;
    task->sub_index = 0u;
    task->sub_count = 0u;
    task->legacy_slots = 0u;
}

int tx_wld_guard_task_begin(TxWorld* world, TxWldGuardTask* task) {
    if (!world || !task) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "null incremental WLD guard state");
        return 0;
    }
    *task = (TxWldGuardTask){0};
    if (!task_validate_header_prefix(world)) return 0;
    if (!terra_parse_header_unchecked(world)) return 0;
    if (!task_validate_header_late_strings(world)) return 0;
    task->stage = TX_GUARD_STAGE_CHESTS_INIT;
    task->initialized = 1u;
    return 1;
}

static int task_guard_init_chests(TxWorld* world, TxWldGuardTask* task) {
    uint16_t chest_count;
    task_guard_reset_section(task);
    if (world->pointer_count <= 2u) {
        task->stage = TX_GUARD_STAGE_SIGNS_INIT;
        return 1;
    }
    if (!task_guard_section_view(world, 2u, &task->data, &task->offset, &task->length)) {
        tx_set_error("TERRAX_TRUNCATED_CHESTS", "chest section bounds are invalid");
        return 0;
    }
    if (task->offset == task->length) {
        task->stage = TX_GUARD_STAGE_SIGNS_INIT;
        return 1;
    }
    if (!task_guard_read_u16(task->data, task->length, &task->offset, &chest_count)) {
        tx_set_error("TERRAX_TRUNCATED_CHESTS", "chest section count is truncated");
        return 0;
    }
    task->count = chest_count;
    if (world->version < 294u
            && !task_guard_read_u16(task->data, task->length, &task->offset, &task->legacy_slots)) {
        tx_set_error("TERRAX_TRUNCATED_CHESTS", "legacy chest slot count is truncated");
        return 0;
    }
    task->stage = TX_GUARD_STAGE_CHEST_HEADER;
    return 1;
}

static int task_guard_chest_header(TxWorld* world, TxWldGuardTask* task) {
    uint32_t slots = task->legacy_slots;
    if (task->index >= task->count) {
        task->stage = TX_GUARD_STAGE_SIGNS_INIT;
        return 1;
    }
    if (!terra_reader_take(&task->offset, 8u, task->length)
            || !task_guard_skip_string(task->data, task->length, &task->offset)) {
        tx_set_error("TERRAX_TRUNCATED_CHESTS", "chest name exceeds section bounds");
        return 0;
    }
    if (world->version >= 294u
            && !task_guard_read_u32(task->data, task->length, &task->offset, &slots)) {
        tx_set_error("TERRAX_TRUNCATED_CHESTS", "chest slot count is truncated");
        return 0;
    }
    if (slots > 504u) slots = 504u;
    task->sub_index = 0u;
    task->sub_count = slots;
    task->stage = TX_GUARD_STAGE_CHEST_ITEMS;
    return 1;
}

static int task_guard_chest_item(TxWldGuardTask* task) {
    uint16_t stack;
    if (task->sub_index >= task->sub_count) {
        task->index++;
        task->stage = TX_GUARD_STAGE_CHEST_HEADER;
        return 1;
    }
    if (!task_guard_read_u16(task->data, task->length, &task->offset, &stack)) {
        tx_set_error("TERRAX_TRUNCATED_CHESTS", "chest item stack is truncated");
        return 0;
    }
    if (stack != 0u && !terra_reader_take(&task->offset, 5u, task->length)) {
        tx_set_error("TERRAX_TRUNCATED_CHESTS", "chest item data is truncated");
        return 0;
    }
    task->sub_index++;
    return 1;
}

static int task_guard_init_signs(TxWorld* world, TxWldGuardTask* task) {
    uint16_t sign_count;
    task_guard_reset_section(task);
    if (world->pointer_count <= 3u) {
        task->stage = TX_GUARD_STAGE_NPCS_INIT;
        return 1;
    }
    if (!task_guard_section_view(world, 3u, &task->data, &task->offset, &task->length)) {
        tx_set_error("TERRAX_TRUNCATED_SIGNS", "sign section bounds are invalid");
        return 0;
    }
    if (task->offset == task->length) {
        task->stage = TX_GUARD_STAGE_NPCS_INIT;
        return 1;
    }
    if (!task_guard_read_u16(task->data, task->length, &task->offset, &sign_count)) {
        tx_set_error("TERRAX_TRUNCATED_SIGNS", "sign section count is truncated");
        return 0;
    }
    task->count = sign_count;
    task->stage = TX_GUARD_STAGE_SIGNS;
    return 1;
}

static int task_guard_sign(TxWldGuardTask* task) {
    if (task->index >= task->count) {
        task->stage = TX_GUARD_STAGE_NPCS_INIT;
        return 1;
    }
    if (!task_guard_skip_string(task->data, task->length, &task->offset)
            || !terra_reader_take(&task->offset, 8u, task->length)) {
        tx_set_error("TERRAX_TRUNCATED_SIGNS", "sign text or coordinates exceed section bounds");
        return 0;
    }
    task->index++;
    return 1;
}

static int task_guard_init_npcs(TxWorld* world, TxWldGuardTask* task) {
    task_guard_reset_section(task);
    if (world->pointer_count <= 4u) {
        task->stage = TX_GUARD_STAGE_BESTIARY_KILLS_INIT;
        return 1;
    }
    if (!task_guard_section_view(world, 4u, &task->data, &task->offset, &task->length)) {
        tx_set_error("TERRAX_TRUNCATED_NPCS", "NPC section bounds are invalid");
        return 0;
    }
    if (task->offset == task->length) {
        task->stage = TX_GUARD_STAGE_BESTIARY_KILLS_INIT;
        return 1;
    }
    if (world->version >= 268u) {
        uint32_t shimmered_count;
        if (!task_guard_read_u32(task->data, task->length, &task->offset, &shimmered_count)
                || !terra_reader_take_count(&task->offset, shimmered_count, 4u, task->length)) {
            tx_set_error("TERRAX_TRUNCATED_NPCS", "shimmered NPC list is truncated");
            return 0;
        }
    }
    task->stage = TX_GUARD_STAGE_NPCS;
    return 1;
}

static int task_guard_npc(TxWorld* world, TxWldGuardTask* task) {
    uint8_t has_npc;
    if (task->offset >= task->length) {
        tx_set_error("TERRAX_TRUNCATED_NPCS", "town NPC list has no terminator");
        return 0;
    }
    if (!task_guard_read_u8(task->data, task->length, &task->offset, &has_npc)) {
        tx_set_error("TERRAX_TRUNCATED_NPCS", "town NPC list has no terminator");
        return 0;
    }
    if (!has_npc) {
        task->stage = TX_GUARD_STAGE_BESTIARY_KILLS_INIT;
        return 1;
    }
    if (!terra_reader_take(&task->offset, 4u, task->length)
            || !task_guard_skip_string(task->data, task->length, &task->offset)
            || !terra_reader_take(&task->offset, 17u, task->length)) {
        tx_set_error("TERRAX_TRUNCATED_NPCS", "town NPC name or fixed fields exceed section bounds");
        return 0;
    }
    if (world->version >= 213u) {
        uint8_t has_variation;
        if (!task_guard_read_u8(task->data, task->length, &task->offset, &has_variation)
                || (has_variation && !terra_reader_take(&task->offset, 4u, task->length))) {
            tx_set_error("TERRAX_TRUNCATED_NPCS", "town NPC variation is truncated");
            return 0;
        }
    }
    if (world->version >= 315u && !terra_reader_take(&task->offset, 1u, task->length)) {
        tx_set_error("TERRAX_TRUNCATED_NPCS", "town NPC despawn flag is truncated");
        return 0;
    }
    task->index++;
    return 1;
}

static int task_guard_init_bestiary(TxWorld* world, TxWldGuardTask* task) {
    task_guard_reset_section(task);
    if (world->pointer_count <= 8u) {
        task->stage = TX_GUARD_STAGE_DONE;
        return 1;
    }
    if (!task_guard_section_view(world, 8u, &task->data, &task->offset, &task->length)) {
        tx_set_error("TERRAX_TRUNCATED_BESTIARY", "bestiary section bounds are invalid");
        return 0;
    }
    if (task->offset == task->length) {
        task->stage = TX_GUARD_STAGE_DONE;
        return 1;
    }
    if (!task_guard_read_u32(task->data, task->length, &task->offset, &task->count)) {
        tx_set_error("TERRAX_TRUNCATED_BESTIARY", "bestiary kill count is truncated");
        return 0;
    }
    task->stage = TX_GUARD_STAGE_BESTIARY_KILLS;
    return 1;
}

static int task_guard_bestiary_kill(TxWldGuardTask* task) {
    if (task->index >= task->count) {
        task->stage = TX_GUARD_STAGE_BESTIARY_SIGHTINGS_INIT;
        return 1;
    }
    if (!task_guard_skip_string(task->data, task->length, &task->offset)
            || !terra_reader_take(&task->offset, 4u, task->length)) {
        tx_set_error("TERRAX_TRUNCATED_BESTIARY", "bestiary kill entry exceeds section bounds");
        return 0;
    }
    task->index++;
    return 1;
}

static int task_guard_bestiary_count(
        TxWldGuardTask* task, uint8_t next_stage, const char* error_message) {
    task->index = 0u;
    if (!task_guard_read_u32(task->data, task->length, &task->offset, &task->count)) {
        tx_set_error("TERRAX_TRUNCATED_BESTIARY", error_message);
        return 0;
    }
    task->stage = next_stage;
    return 1;
}

static int task_guard_bestiary_string(
        TxWldGuardTask* task, uint8_t next_stage, const char* error_message) {
    if (task->index >= task->count) {
        task->stage = next_stage;
        return 1;
    }
    if (!task_guard_skip_string(task->data, task->length, &task->offset)) {
        tx_set_error("TERRAX_TRUNCATED_BESTIARY", error_message);
        return 0;
    }
    task->index++;
    return 1;
}

int tx_wld_guard_task_step(TxWorld* world, TxWldGuardTask* task, uint32_t record_budget) {
    if (!world || !task || !task->initialized) {
        tx_set_error("TERRAX_STATE_ERROR", "incremental WLD guard is not initialized");
        return -1;
    }
    if (task->finished) return 1;
    if (record_budget == 0u) return 0;

    while (record_budget > 0u && !task->finished) {
        uint8_t stage_before = task->stage;
        int ok = 1;
        int consumed_record = 0;
        switch (task->stage) {
            case TX_GUARD_STAGE_CHESTS_INIT:
                ok = task_guard_init_chests(world, task);
                break;
            case TX_GUARD_STAGE_CHEST_HEADER:
                if (task->index < task->count) consumed_record = 1;
                ok = task_guard_chest_header(world, task);
                break;
            case TX_GUARD_STAGE_CHEST_ITEMS:
                if (task->sub_index < task->sub_count) consumed_record = 1;
                ok = task_guard_chest_item(task);
                break;
            case TX_GUARD_STAGE_SIGNS_INIT:
                ok = task_guard_init_signs(world, task);
                break;
            case TX_GUARD_STAGE_SIGNS:
                if (task->index < task->count) consumed_record = 1;
                ok = task_guard_sign(task);
                break;
            case TX_GUARD_STAGE_NPCS_INIT:
                ok = task_guard_init_npcs(world, task);
                break;
            case TX_GUARD_STAGE_NPCS:
                consumed_record = 1;
                ok = task_guard_npc(world, task);
                break;
            case TX_GUARD_STAGE_BESTIARY_KILLS_INIT:
                ok = task_guard_init_bestiary(world, task);
                break;
            case TX_GUARD_STAGE_BESTIARY_KILLS:
                if (task->index < task->count) consumed_record = 1;
                ok = task_guard_bestiary_kill(task);
                break;
            case TX_GUARD_STAGE_BESTIARY_SIGHTINGS_INIT:
                ok = task_guard_bestiary_count(
                    task, TX_GUARD_STAGE_BESTIARY_SIGHTINGS,
                    "bestiary sighting count is truncated");
                break;
            case TX_GUARD_STAGE_BESTIARY_SIGHTINGS:
                if (task->index < task->count) consumed_record = 1;
                ok = task_guard_bestiary_string(
                    task, TX_GUARD_STAGE_BESTIARY_CHATS_INIT,
                    "bestiary sighting entry exceeds section bounds");
                break;
            case TX_GUARD_STAGE_BESTIARY_CHATS_INIT:
                ok = task_guard_bestiary_count(
                    task, TX_GUARD_STAGE_BESTIARY_CHATS,
                    "bestiary chat count is truncated");
                break;
            case TX_GUARD_STAGE_BESTIARY_CHATS:
                if (task->index < task->count) consumed_record = 1;
                ok = task_guard_bestiary_string(
                    task, TX_GUARD_STAGE_DONE,
                    "bestiary chat entry exceeds section bounds");
                break;
            case TX_GUARD_STAGE_DONE:
                task->finished = 1u;
                return 1;
            default:
                tx_set_error("TERRAX_STATE_ERROR", "invalid incremental WLD guard stage");
                return -1;
        }
        if (!ok) return -1;
        if (consumed_record) record_budget--;
        /* Every non-record transition must advance stage, otherwise malformed
         * state could spin without consuming the caller's budget. */
        if (!consumed_record && task->stage == stage_before) {
            tx_set_error("TERRAX_STATE_ERROR", "incremental WLD guard made no progress");
            return -1;
        }
        if (task->stage == TX_GUARD_STAGE_DONE) {
            task->finished = 1u;
            return 1;
        }
    }
    return task->finished ? 1 : 0;
}

uint32_t tx_wld_guard_task_progress(const TxWldGuardTask* task) {
    if (!task || !task->initialized) return 0u;
    if (task->finished) return 100u;
    {
        uint32_t stage = task->stage;
        if (stage >= TX_GUARD_STAGE_DONE) return 99u;
        return (stage * 100u) / TX_GUARD_STAGE_DONE;
    }
}
