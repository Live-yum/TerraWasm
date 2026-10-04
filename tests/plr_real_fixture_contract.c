#include "terra_plr.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef TERRAWASM_PLR_REAL_FIXTURE_PATH
#error "TERRAWASM_PLR_REAL_FIXTURE_PATH must point to a real Terraria .plr fixture"
#endif

extern uint32_t tx_heap_used(void);

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "PLR real-fixture contract failure: %s\n", (message)); \
        return 1; \
    } \
} while (0)

static int bytes_equal(const uint8_t *a, const uint8_t *b, uint32_t length) {
    if (!a || !b) return length == 0u;
    for (uint32_t i = 0u; i < length; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int text_equal(const char *a, const char *b) {
    if (!a || !b) return 0;
    uint32_t i = 0u;
    while (a[i] || b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return 1;
}

static uint8_t *read_fixture(uint32_t *out_length) {
    *out_length = 0u;
    FILE *file = fopen(TERRAWASM_PLR_REAL_FIXTURE_PATH, "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0) {
        if (file) fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size <= 0 || (uint64_t)size >= UINT32_MAX || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data) {
        fclose(file);
        return NULL;
    }
    size_t got = fread(data, 1u, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        free(data);
        return NULL;
    }
    *out_length = (uint32_t)size;
    return data;
}

static int get_field(uint32_t handle, const char *pointer, char **out) {
    uint32_t required = 0u;
    if (terra_plr_get(handle, pointer, NULL, 0u, &required) != TERRAX_WORLD_STATUS_OK ||
        required < 2u) return 0;
    char *text = (char *)malloc(required);
    if (!text) return 0;
    if (terra_plr_get(handle, pointer, text, required, &required) != TERRAX_WORLD_STATUS_OK) {
        free(text);
        return 0;
    }
    *out = text;
    return 1;
}

int main(void) {
    uint32_t fixture_length = 0u;
    uint8_t *fixture = read_fixture(&fixture_length);
    CHECK(fixture != NULL, "real fixture could not be read");
    CHECK(fixture_length >= 16u && (fixture_length & 15u) == 0u,
        "real fixture is not AES-CBC framed");

    uint32_t baseline = tx_heap_used();
    uint32_t handle = 0u;
    CHECK(terra_plr_open_from_buffer(fixture, fixture_length, &handle) ==
        TERRAX_WORLD_STATUS_OK, "real Terraria PLR did not open");
    CHECK(handle != 0u, "real fixture returned null handle");

    uint32_t json_required = 0u;
    uint32_t before_json = tx_heap_used();
    CHECK(terra_plr_get_json(handle, NULL, 0u, &json_required) ==
        TERRAX_WORLD_STATUS_OK && json_required > 1u, "get_json probe");
    uint32_t after_json = tx_heap_used();
    CHECK(after_json > before_json, "first JSON probe did not populate cache");
    CHECK(terra_plr_get_json(handle, NULL, 0u, &json_required) ==
        TERRAX_WORLD_STATUS_OK, "second get_json probe");
    CHECK(tx_heap_used() == after_json, "second JSON probe rebuilt the cache");

    char *name = NULL;
    char *version = NULL;
    char *tax_money = NULL;
    char *deaths_pve = NULL;
    char *voice_variant = NULL;
    CHECK(get_field(handle, "/name", &name), "read real player name");
    CHECK(name[0] == '"' && name[1] != '"', "real player name is empty");
    CHECK(get_field(handle, "/version", &version), "read real player version");
    CHECK(text_equal(version, "326"), "real fixture is not the expected v326 player");
    CHECK(get_field(handle, "/taxMoney", &tax_money) && text_equal(tax_money, "113750"),
        "v326 body prefix is misaligned before taxMoney");
    CHECK(get_field(handle, "/numberOfDeathsPve", &deaths_pve) && text_equal(deaths_pve, "11"),
        "v326 death counters are misaligned");
    CHECK(get_field(handle, "/voiceVariant", &voice_variant) && text_equal(voice_variant, "2"),
        "v326 loadout favorite bytes are misaligned before voiceVariant");

    uint32_t required = 0u;
    uint32_t before_clean_probe = tx_heap_used();
    CHECK(terra_plr_save_to_buffer(handle, NULL, 0u, &required) ==
        TERRAX_WORLD_STATUS_OK && required == fixture_length, "clean save probe");
    CHECK(tx_heap_used() == before_clean_probe,
        "clean save probe allocated instead of using original encrypted bytes");
    CHECK(terra_plr_save_to_buffer(handle, NULL, 0u, &required) ==
        TERRAX_WORLD_STATUS_OK, "second clean save probe");
    CHECK(tx_heap_used() == before_clean_probe, "second clean save probe allocated");

    uint8_t *clean = (uint8_t *)malloc(required);
    CHECK(clean != NULL, "clean output allocation");
    CHECK(terra_plr_save_to_buffer(handle, clean, required, &required) ==
        TERRAX_WORLD_STATUS_OK, "clean save fetch");
    CHECK(bytes_equal(clean, fixture, fixture_length),
        "clean real-player save did not preserve exact encrypted bytes");
    free(clean);

    CHECK(terra_plr_set(handle, "/name", "\"real-fixture-edited\"") ==
        TERRAX_WORLD_STATUS_OK, "edit real fixture name");
    uint32_t before_dirty_probe = tx_heap_used();
    CHECK(terra_plr_save_to_buffer(handle, NULL, 0u, &required) ==
        TERRAX_WORLD_STATUS_OK && required > 0u, "dirty save probe");
    uint32_t after_dirty_probe = tx_heap_used();
    CHECK(after_dirty_probe > before_dirty_probe, "dirty save did not populate encoded cache");
    CHECK(terra_plr_save_to_buffer(handle, NULL, 0u, &required) ==
        TERRAX_WORLD_STATUS_OK, "second dirty save probe");
    CHECK(tx_heap_used() == after_dirty_probe, "dirty save probe re-encoded the PLR");

    uint8_t *edited = (uint8_t *)malloc(required);
    CHECK(edited != NULL, "edited output allocation");
    uint32_t edited_length = required;
    CHECK(terra_plr_save_to_buffer(handle, edited, edited_length, &required) ==
        TERRAX_WORLD_STATUS_OK, "dirty save fetch");
    CHECK(tx_heap_used() == after_dirty_probe, "dirty save fetch re-encoded the PLR");

    uint32_t reopened = 0u;
    CHECK(terra_plr_open_from_buffer(edited, edited_length, &reopened) ==
        TERRAX_WORLD_STATUS_OK, "edited real fixture did not reopen");
    char *edited_name = NULL;
    CHECK(get_field(reopened, "/name", &edited_name), "read edited name");
    CHECK(text_equal(edited_name, "\"real-fixture-edited\""),
        "edited name did not survive semantic encoding");

    /* The read contract attempts unchanged future layouts, but never creates
     * a future save by serializing a known schema. */
    CHECK(terra_plr_set(reopened, "/version", "328") != TERRAX_WORLD_STATUS_OK,
        "future creation/conversion unexpectedly allowed");
    extern int terrax_test_plr_fixture_version(const uint8_t*,uint32_t,int32_t,uint8_t*,uint32_t);
    uint8_t *future_bytes = malloc(fixture_length);
    CHECK(future_bytes != NULL, "future fixture allocation");
    uint32_t future_required = (uint32_t)terrax_test_plr_fixture_version(
        fixture, fixture_length, 328, future_bytes, fixture_length);
    CHECK(future_required == fixture_length, "change only encrypted fixture version");
    uint32_t future_handle = 0u;
    CHECK(terra_plr_open_from_buffer(future_bytes, future_required, &future_handle) ==
        TERRAX_WORLD_STATUS_OK, "unchanged future layout rejected");
    char *future_version = NULL;
    CHECK(get_field(future_handle, "/version", &future_version) && text_equal(future_version,"328"),
        "future source version was not preserved");
    CHECK(terra_plr_set(future_handle,"/version","326") == TERRAX_WORLD_STATUS_NOT_SUPPORTED,
        "future read-only bypass through version reset");
    CHECK(terra_plr_set_many(future_handle,"[]") == TERRAX_WORLD_STATUS_NOT_SUPPORTED,
        "future read-only bypass through set_many");
    CHECK(terra_plr_replace_json(future_handle,"{}") == TERRAX_WORLD_STATUS_NOT_SUPPORTED,
        "future read-only bypass through replace_json");
    CHECK(terra_plr_apply_patch_json(future_handle,"{}") == TERRAX_WORLD_STATUS_NOT_SUPPORTED,
        "future read-only bypass through patch");
    uint8_t *future_output=malloc(future_required);
    CHECK(future_output != NULL, "future output allocation");
    uint32_t written=0;
    CHECK(terra_plr_save_to_buffer(future_handle,future_output,future_required,&written)==TERRAX_WORLD_STATUS_OK &&
        written==future_required && bytes_equal(future_output,future_bytes,written),"future original export changed bytes");
    free(future_output);free(future_bytes);free(future_version);

    free(edited_name);
    free(edited);
    free(voice_variant);
    free(deaths_pve);
    free(tax_money);
    free(version);
    free(name);
    CHECK(terra_plr_close(future_handle) == TERRAX_WORLD_STATUS_OK, "close future handle");
    CHECK(terra_plr_close(reopened) == TERRAX_WORLD_STATUS_OK, "close reopened handle");
    CHECK(terra_plr_close(handle) == TERRAX_WORLD_STATUS_OK, "close real fixture handle");
    CHECK(tx_heap_used() == baseline, "real PLR contract leaked tracked allocations/caches");
    free(fixture);
    return 0;
}

