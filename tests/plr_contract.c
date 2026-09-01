#include "terra_plr.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef TERRAWASM_PLR_JSON_FIXTURE_PATH
#error "TERRAWASM_PLR_JSON_FIXTURE_PATH must point to a semantic player JSON fixture"
#endif

extern uint32_t tx_heap_used(void);
extern void tx_rewind(uint32_t mark);
extern void tx_reset_heap(void);

static int bytes_equal(const uint8_t *left, const uint8_t *right, uint32_t length) {
    if (!left || !right) return length == 0u;
    for (uint32_t i = 0u; i < length; i++) {
        if (left[i] != right[i]) return 0;
    }
    return 1;
}

static int text_equal(const char *left, const char *right) {
    if (!left || !right) return 0;
    uint32_t i = 0u;
    while (left[i] || right[i]) {
        if (left[i] != right[i]) return 0;
        i++;
    }
    return 1;
}

static char *read_json_fixture(void) {
    FILE *file = fopen(TERRAWASM_PLR_JSON_FIXTURE_PATH, "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0) {
        if (file) fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size <= 0 || (uint64_t)size >= UINT32_MAX ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *data = (char *)malloc((size_t)size + 1u);
    if (!data) {
        fclose(file);
        return NULL;
    }
    size_t read = fread(data, 1u, (size_t)size, file);
    fclose(file);
    if (read != (size_t)size) {
        free(data);
        return NULL;
    }
    data[size] = 0;
    return data;
}

static int get_field_text(uint32_t handle, const char *pointer, char **out_text) {
    uint32_t required = 0u;
    if (terra_plr_get(handle, pointer, NULL, 0u, &required) != TERRAX_WORLD_STATUS_OK ||
        required == 0u) return 0;
    char *text = (char *)malloc(required);
    if (!text) return 0;
    if (terra_plr_get(handle, pointer, text, required, &required) !=
        TERRAX_WORLD_STATUS_OK) {
        free(text);
        return 0;
    }
    *out_text = text;
    return 1;
}

static int get_document_json(
    uint32_t handle, char **out_text, uint32_t *out_length) {
    uint32_t required = 0u;
    if (terra_plr_get_json(handle, NULL, 0u, &required) != TERRAX_WORLD_STATUS_OK ||
        required < 2u) return 0;
    char *text = (char *)malloc(required);
    if (!text) return 0;
    if (terra_plr_get_json(handle, text, required, &required) !=
        TERRAX_WORLD_STATUS_OK) {
        free(text);
        return 0;
    }
    *out_text = text;
    *out_length = required;
    return 1;
}

static int save_document(
    uint32_t handle, uint8_t **out_bytes, uint32_t *out_length) {
    uint32_t required = 0u;
    if (terra_plr_save_to_buffer(handle, NULL, 0u, &required) !=
        TERRAX_WORLD_STATUS_OK || required == 0u) return 0;
    uint8_t *bytes = (uint8_t *)malloc(required);
    if (!bytes) return 0;
    if (terra_plr_save_to_buffer(handle, bytes, required, &required) !=
        TERRAX_WORLD_STATUS_OK) {
        free(bytes);
        return 0;
    }
    *out_bytes = bytes;
    *out_length = required;
    return 1;
}

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "PLR contract failure: %s\n", (message)); \
        return 1; \
    } \
} while (0)

int main(void) {
    char *fixture_json = read_json_fixture();
    CHECK(fixture_json != NULL, "semantic fixture could not be read");

    uint32_t heap_before = tx_heap_used();
    uint32_t generated_handle = 0u;
    CHECK(terra_plr_open_json(fixture_json, &generated_handle) ==
        TERRAX_WORLD_STATUS_OK, "open_json fixture");
    CHECK(generated_handle != 0u, "open_json returned a null handle");
    CHECK(tx_heap_used() > heap_before,
        "PLR JSON document was not tracked by the allocator");

    char *name = NULL;
    CHECK(get_field_text(generated_handle, "/name", &name), "get generated name");
    CHECK(text_equal(name, "\"fixture-player\""), "generated player name");
    free(name);

    uint8_t *fixture = NULL;
    uint32_t fixture_length = 0u;
    CHECK(save_document(generated_handle, &fixture, &fixture_length),
        "encode semantic fixture");
    CHECK(fixture_length >= 16u && (fixture_length & 15u) == 0u,
        "encrypted fixture framing");
    CHECK(terra_plr_close(generated_handle) == TERRAX_WORLD_STATUS_OK,
        "close generated document");
    CHECK(tx_heap_used() == heap_before,
        "closing generated document leaked tracked allocations");

    uint32_t handle = 0u;
    CHECK(terra_plr_open_from_buffer(fixture, fixture_length, &handle) ==
        TERRAX_WORLD_STATUS_OK, "open_from_buffer");
    CHECK(handle != 0u, "open returned a null handle");
    CHECK(tx_heap_used() > heap_before, "PLR document was not tracked");

    char *json = NULL;
    uint32_t json_length = 0u;
    CHECK(get_document_json(handle, &json, &json_length), "get_json two-call ABI");
    CHECK(json_length > 1u && json[json_length - 1u] == 0,
        "get_json NUL contract");
    CHECK(get_field_text(handle, "/name", &name), "get field");
    CHECK(text_equal(name, "\"fixture-player\""), "decoded player name");
    free(name);

    uint8_t *clean = NULL;
    uint32_t clean_length = 0u;
    CHECK(save_document(handle, &clean, &clean_length), "clean save");
    CHECK(clean_length == fixture_length &&
        bytes_equal(clean, fixture, fixture_length),
        "clean save did not preserve encrypted bytes");
    free(clean);

    /* Rewinding/resetting transient WLD allocations must not invalidate a
     * live PLR document held in the persistent allocator domain. */
    tx_rewind(0u);
    tx_reset_heap();
    CHECK(tx_heap_used() > heap_before, "transient reset destroyed PLR");
    CHECK(get_field_text(handle, "/name", &name),
        "get field after allocator reset");
    CHECK(text_equal(name, "\"fixture-player\""), "name after allocator reset");
    free(name);

    CHECK(terra_plr_set(handle, "/name", "\"contract-edited\"") ==
        TERRAX_WORLD_STATUS_OK, "set field");
    CHECK(get_field_text(handle, "/name", &name), "get edited field");
    CHECK(text_equal(name, "\"contract-edited\""), "edited player name");
    free(name);

    CHECK(terra_plr_set_many(handle,
        "[{\"path\":\"/name\",\"value\":\"must-not-commit\"},"
        "{\"path\":\"/missing\",\"value\":1}]") ==
        TERRAX_WORLD_STATUS_VALIDATION_ERROR, "invalid set_many status");
    CHECK(get_field_text(handle, "/name", &name),
        "get name after failed set_many");
    CHECK(text_equal(name, "\"contract-edited\""),
        "failed set_many was not atomic");
    free(name);

    CHECK(terra_plr_apply_patch_json(handle,
        "{\"items\":[{\"section\":\"inventory\",\"index\":0,"
        "\"itemType\":1234,\"stack\":2,\"prefix\":3,\"favorited\":true}],"
        "\"buffs\":[{\"index\":0,\"buffType\":5,\"buffTime\":60}],"
        "\"loadoutSlots\":[{\"loadoutIndex\":0,\"slotKind\":\"Armor\","
        "\"slotIndex\":0,\"itemType\":4321,\"stack\":1,\"prefix\":2}]}") ==
        TERRAX_WORLD_STATUS_OK, "structured patch");

    uint8_t *edited = NULL;
    uint32_t edited_length = 0u;
    CHECK(save_document(handle, &edited, &edited_length), "edited save");
    CHECK(edited_length != fixture_length ||
        !bytes_equal(edited, fixture, fixture_length),
        "edited save unexpectedly preserved original bytes");

    uint32_t edited_handle = 0u;
    CHECK(terra_plr_open_from_buffer(edited, edited_length, &edited_handle) ==
        TERRAX_WORLD_STATUS_OK, "reopen edited bytes");
    CHECK(get_field_text(edited_handle, "/name", &name), "get reopened name");
    CHECK(text_equal(name, "\"contract-edited\""),
        "edited name did not survive encryption");
    free(name);

    uint32_t json_handle = 0u;
    CHECK(terra_plr_open_json(json, &json_handle) == TERRAX_WORLD_STATUS_OK,
        "open semantic JSON document");
    CHECK(json_handle != 0u, "open_json returned a null handle");
    CHECK(terra_plr_close(json_handle) == TERRAX_WORLD_STATUS_OK,
        "close JSON document");
    CHECK(terra_plr_close(edited_handle) == TERRAX_WORLD_STATUS_OK,
        "close reopened edited document");
    CHECK(terra_plr_close(edited_handle) != TERRAX_WORLD_STATUS_OK,
        "stale handle rejection");

    uint8_t *bad = (uint8_t *)malloc(fixture_length);
    CHECK(bad != NULL, "malformed fixture allocation");
    for (uint32_t i = 0u; i < fixture_length; i++) bad[i] = fixture[i];
    bad[fixture_length - 1u] ^= 0x01u;
    uint32_t bad_handle = 0u;
    CHECK(terra_plr_open_from_buffer(bad, fixture_length, &bad_handle) ==
        TERRAX_WORLD_STATUS_PARSE_ERROR, "malformed PLR status");
    free(bad);

    CHECK(terra_plr_close(handle) == TERRAX_WORLD_STATUS_OK,
        "close original document");
    free(edited);
    free(json);
    free(fixture);
    free(fixture_json);
    CHECK(tx_heap_used() == heap_before, "PLR close leaked allocations");
    return 0;
}
