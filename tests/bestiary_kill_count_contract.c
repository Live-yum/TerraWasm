#include "terra_types.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int tx_mutate_replace_bestiary(TxWorld*, const char*, uint32_t, TxBuf*);
extern void serialize_bestiary_json(TxWorld*, TxBuf*);
extern void buf_init(TxBuf*, uint32_t);
extern void buf_u8(TxBuf*, uint8_t);
extern void tx_internal_free(void*);
extern void tx_reset_heap(void);

/* Read independently of TerraWasm's reader: BinaryWriter.Write(Int32) stores
 * four little-endian bytes, with no change to the bestiary section layout. */
static uint32_t read_u32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int replace(TxWorld* world, const char* count, int invalid_second_entry) {
    char request[512];
    int length = snprintf(request, sizeof(request),
        "{\"kills\":[%s{\"persistentNpcId\":\"Terraria.Zombie\",\"killCount\":%s}],"
        "\"sightings\":[],\"chats\":[]}",
        invalid_second_entry ? "{\"persistentNpcId\":\"Terraria.Guide\",\"killCount\":7}," : "",
        count);
    assert(length > 0 && (size_t)length < sizeof(request));
    TxBuf response;
    buf_init(&response, 128u);
    int result = tx_mutate_replace_bestiary(world, request, (uint32_t)length, &response);
    tx_internal_free(response.data);
    return result;
}

int main(void) {
    TxWorld world = {0};
    world.version = world.original_version = 279u;
    world.pointer_count = 11u;
    const uint32_t accepted[] = {0u, 1u, 1000000u, 1000001u, 999999998u, 999999999u};
    const char* npc = "Terraria.Zombie";
    const uint32_t name_length = (uint32_t)strlen(npc);
    for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); i++) {
        char count[32];
        snprintf(count, sizeof(count), "%u", accepted[i]);
        assert(replace(&world, count, 0) >= 0);
        TxSectionOverride* section = &world.section_overrides[8];
        assert(section->active);
        assert(section->len == 4u + 1u + name_length + 4u + 4u + 4u);
        assert(read_u32le(section->data) == 1u);
        assert(section->data[4] == name_length);
        assert(memcmp(section->data + 5u, npc, name_length) == 0);
        uint32_t offset = 5u + name_length;
        assert(read_u32le(section->data + offset) == accepted[i]);
        assert((int32_t)read_u32le(section->data + offset) >= 0);
        assert(read_u32le(section->data + offset + 4u) == 0u);
        assert(read_u32le(section->data + offset + 8u) == 0u);

        /* Parse the actual encoded section through a new read-only world view,
         * without consulting the first world's active section override. */
        TxWorld reopened = {0};
        reopened.version = reopened.original_version = world.version;
        reopened.pointer_count = world.pointer_count;
        reopened.file = section->data;
        reopened.file_len = section->len;
        reopened.ends[8] = section->len;
        TxBuf decoded;
        buf_init(&decoded, 256u);
        serialize_bestiary_json(&reopened, &decoded);
        buf_u8(&decoded, 0u);
        assert(decoded.ok);
        char expected[48];
        snprintf(expected, sizeof(expected), "\"killCount\":%u\n", accepted[i]);
        assert(strstr((const char*)decoded.data, expected));
        assert(strstr((const char*)decoded.data, npc));
        tx_internal_free(decoded.data);
    }

    const char* rejected[] = {
        "1000000000", "2147483647", "2147483648", "4294967295", "4294967296",
        "-1", "0.5", "1.0", "1e2", "\"12\"", "null", "true",
        "999999999999999999999999999999",
    };
    uint8_t snapshot[64];
    TxSectionOverride* section = &world.section_overrides[8];
    uint8_t* previous = section->data;
    uint32_t previous_length = section->len;
    assert(previous_length <= sizeof(snapshot));
    memcpy(snapshot, section->data, previous_length);
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        /* A valid first entry must not be committed when the next entry fails. */
        assert(replace(&world, rejected[i], 1) < 0);
        assert(section->active && section->data == previous);
        assert(section->len == previous_length);
        assert(memcmp(section->data, snapshot, previous_length) == 0);
    }
    tx_internal_free(section->data);
    tx_reset_heap();
    puts("bestiary kill-count bounds and Int32 section round-trip: ok");
    return 0;
}
