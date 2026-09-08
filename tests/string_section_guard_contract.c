#include "terra_types.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

extern int terra_validate_string_sections(TxWorld* world);

static void set_section(TxWorld* world, uint32_t index, uint32_t start, uint32_t end) {
    world->starts[index] = start;
    world->ends[index] = end;
}

static TxWorld make_world(uint8_t* data, uint32_t length, uint32_t pointer_count, uint32_t version) {
    TxWorld world = {0};
    world.file = data;
    world.file_len = length;
    world.pointer_count = pointer_count;
    world.version = version;
    set_section(&world, 0u, 0u, 0u);
    return world;
}

static void valid_empty_chests(TxWorld* world, uint32_t start) {
    set_section(world, 2u, start, start + 2u);
}

static void valid_empty_signs(TxWorld* world, uint32_t start) {
    set_section(world, 3u, start, start + 2u);
}

static void valid_empty_npcs(TxWorld* world, uint32_t start) {
    set_section(world, 4u, start, start + 5u);
}

static void accepts_zero_length_string_sections(void) {
    uint8_t placeholder[] = {0u};
    TxWorld world = make_world(placeholder, (uint32_t)sizeof(placeholder), 9u, 279u);
    for (uint32_t index = 1u; index < world.pointer_count; index++) {
        set_section(&world, index, 0u, 0u);
    }
    assert(terra_validate_string_sections(&world));
}

static void rejects_truncated_chest_name(void) {
    uint8_t invalid[] = {
        1u, 0u,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
        5u, 'o', 'k'
    };
    TxWorld world = make_world(invalid, (uint32_t)sizeof(invalid), 3u, 294u);
    set_section(&world, 2u, 0u, (uint32_t)sizeof(invalid));
    assert(!terra_validate_string_sections(&world));
}

static void accepts_bounded_chest_name(void) {
    uint8_t valid[] = {
        1u, 0u,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
        2u, 'o', 'k',
        0u, 0u, 0u, 0u
    };
    TxWorld world = make_world(valid, (uint32_t)sizeof(valid), 3u, 294u);
    set_section(&world, 2u, 0u, (uint32_t)sizeof(valid));
    assert(terra_validate_string_sections(&world));
}

static void rejects_truncated_sign_text(void) {
    uint8_t invalid[] = {
        0u, 0u,
        1u, 0u,
        4u, 'n', 'o',
        0u, 0u, 0u, 0u,
        0u, 0u, 0u, 0u
    };
    TxWorld world = make_world(invalid, (uint32_t)sizeof(invalid), 4u, 294u);
    valid_empty_chests(&world, 0u);
    set_section(&world, 3u, 2u, (uint32_t)sizeof(invalid));
    assert(!terra_validate_string_sections(&world));
}

static void accepts_sign_text_before_coordinates(void) {
    uint8_t valid[] = {
        0u, 0u,
        1u, 0u,
        2u, 'o', 'k',
        0x7au, 0x11u, 0u, 0u,
        0x3fu, 0x02u, 0u, 0u
    };
    TxWorld world = make_world(valid, (uint32_t)sizeof(valid), 4u, 294u);
    valid_empty_chests(&world, 0u);
    set_section(&world, 3u, 2u, (uint32_t)sizeof(valid));
    assert(terra_validate_string_sections(&world));
}

static void rejects_truncated_npc_name(void) {
    uint8_t invalid[] = {
        0u, 0u,
        0u, 0u,
        0u, 0u, 0u, 0u,
        1u,
        0u, 0u, 0u, 0u,
        6u, 'n', 'p'
    };
    TxWorld world = make_world(invalid, (uint32_t)sizeof(invalid), 5u, 268u);
    valid_empty_chests(&world, 0u);
    valid_empty_signs(&world, 2u);
    set_section(&world, 4u, 4u, (uint32_t)sizeof(invalid));
    assert(!terra_validate_string_sections(&world));
}

static void accepts_legacy_npc_string_type_without_persistent_section(void) {
    /* v139 stores both NPC type and given name as 7-bit strings and has no
     * persistent NPC list at all. */
    uint8_t valid[] = {
        1u, 5u, 'G', 'u', 'i', 'd', 'e',
        3u, 'B', 'o', 'b',
        0u, 0u, 32u, 65u, 0u, 0u, 0u, 0u,
        0u,
        10u, 0u, 0u, 0u, 20u, 0u, 0u, 0u,
        0u
    };
    TxWorld world = make_world(valid, (uint32_t)sizeof(valid), 5u, 139u);
    set_section(&world, 2u, 0u, 0u);
    set_section(&world, 3u, 0u, 0u);
    set_section(&world, 4u, 0u, (uint32_t)sizeof(valid));
    assert(terra_validate_string_sections(&world));
}

static void rejects_truncated_persistent_npc_for_v140(void) {
    /* v140 introduces the second NPC loop; a lone active marker must be
     * rejected instead of being mistaken for the footer. */
    uint8_t invalid[] = {
        0u, /* town terminator */
        1u, /* persistent active, but missing type and position */
    };
    TxWorld world = make_world(invalid, (uint32_t)sizeof(invalid), 5u, 140u);
    set_section(&world, 2u, 0u, 0u);
    set_section(&world, 3u, 0u, 0u);
    set_section(&world, 4u, 0u, (uint32_t)sizeof(invalid));
    assert(!terra_validate_string_sections(&world));
}

static void rejects_legacy_npc_without_town_terminator(void) {
    uint8_t invalid[] = { 1u,5u,'G','u','i','d','e',3u,'B','o','b',
        0u,0u,32u,65u,0u,0u,0u,0u,0u,10u,0u,0u,0u,20u,0u,0u,0u };
    TxWorld world = make_world(invalid, (uint32_t)sizeof(invalid), 5u, 139u);
    set_section(&world, 2u, 0u, 0u); set_section(&world, 3u, 0u, 0u);
    set_section(&world, 4u, 0u, (uint32_t)sizeof(invalid));
    assert(!terra_validate_string_sections(&world));
}

static void rejects_truncated_bestiary_name(void) {
    uint8_t invalid[] = {
        0u, 0u,
        0u, 0u,
        0u, 0u, 0u, 0u, 0u,
        1u, 0u, 0u, 0u,
        5u, 'b', 'a'
    };
    TxWorld world = make_world(invalid, (uint32_t)sizeof(invalid), 9u, 268u);
    valid_empty_chests(&world, 0u);
    valid_empty_signs(&world, 2u);
    valid_empty_npcs(&world, 4u);
    set_section(&world, 5u, 9u, 9u);
    set_section(&world, 6u, 9u, 9u);
    set_section(&world, 7u, 9u, 9u);
    set_section(&world, 8u, 9u, (uint32_t)sizeof(invalid));
    assert(!terra_validate_string_sections(&world));
}

int main(void) {
    accepts_zero_length_string_sections();
    rejects_truncated_chest_name();
    accepts_bounded_chest_name();
    rejects_truncated_sign_text();
    accepts_sign_text_before_coordinates();
    rejects_truncated_npc_name();
    accepts_legacy_npc_string_type_without_persistent_section();
    rejects_truncated_persistent_npc_for_v140();
    rejects_legacy_npc_without_town_terminator();
    rejects_truncated_bestiary_name();
    return 0;
}
