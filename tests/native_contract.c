#include "terra_abi.h"
#include "terra_world.h"
#include "terra_types.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <sys/stat.h>
#include <windows.h>
#define tx_mkdir(path) _mkdir(path)
#define tx_rmdir(path) _rmdir(path)
#define tx_stat _stat
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define tx_mkdir(path) mkdir(path, 0700)
#define tx_rmdir(path) rmdir(path)
#define tx_stat stat
#endif

#ifndef TERRAX_TEST_FIXTURE_DIR
#define TERRAX_TEST_FIXTURE_DIR "."
#endif

#ifdef TERRAX_TESTING
void terrax_test_fail_save_after_bytes(uint32_t bytes);
void terrax_test_reset_fail_save(void);
#endif

void rd_string_copy(const uint8_t* p, uint32_t len, uint32_t* off, char* out, uint32_t cap);
void rd_skip_string_value(const uint8_t* p, uint32_t len, uint32_t* off);
int parse_format(TxWorld* w);
int parse_header(TxWorld* w);
int read_tile_at(TxWorld* w, uint32_t* off, uint32_t end, TxTile* t);
int tile_important(TxWorld* w, uint16_t type);
void write_tile(TxWorld* w, TxBuf* b, const TxTile* t, uint32_t same);
void buf_init(TxBuf* b, uint32_t cap);
void tx_internal_free(void* ptr);
int tx_stream_parse_tile_rules(TxWorld* w, const char* json, int len, TxTileRule** rules, uint32_t* count);
void tx_apply_tile_rules(TxTile* t, TxTileRule* rules, uint32_t count, uint32_t run, uint16_t region, uint32_t y, double surface);

static int read_file_alloc(const char* path, unsigned char** out_data, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    long size = ftell(f);
    if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    unsigned char* data = (unsigned char*)malloc((size_t)size);
    if (!data) {
        fclose(f);
        return 0;
    }
    size_t read = fread(data, 1, (size_t)size, f);
    fclose(f);
    if (read != (size_t)size) {
        free(data);
        return 0;
    }
    *out_data = data;
    *out_len = (size_t)size;
    return 1;
}

static int write_file_bytes(const char* path, const unsigned char* data, size_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    return written == len;
}

static int ensure_dir_exists(const char* path) {
    struct tx_stat info;
    if (tx_stat(path, &info) == 0) return 1;
    return tx_mkdir(path) == 0;
}

static long file_size_or_missing(const char* path) {
    struct tx_stat info;
    if (tx_stat(path, &info) != 0) return -1;
    return (long)info.st_size;
}

static int remove_if_exists_checked(const char* path) {
    if (remove(path) == 0) return 1;
    return errno == ENOENT;
}

static int count_files_with_prefix(const char* dir_path, const char* prefix) {
    int count = 0;
    size_t prefix_len = strlen(prefix);
#ifdef _WIN32
    char pattern[512];
    WIN32_FIND_DATAA entry;
    HANDLE handle;
    if (snprintf(pattern, sizeof(pattern), "%s\\*", dir_path) <= 0) return -1;
    handle = FindFirstFileA(pattern, &entry);
    if (handle == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    }
    do {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
            strncmp(entry.cFileName, prefix, prefix_len) == 0) {
            count++;
        }
    } while (FindNextFileA(handle, &entry) != 0);
    FindClose(handle);
#else
    DIR* dir = opendir(dir_path);
    if (!dir) return errno == ENOENT ? 0 : -1;
    for (;;) {
        struct dirent* entry = readdir(dir);
        if (!entry) break;
        if (strncmp(entry->d_name, prefix, prefix_len) == 0) count++;
    }
    closedir(dir);
#endif
    return count;
}

static int expect(int condition, const char* message) {
    if (condition) return 1;
    fprintf(stderr, "%s\n", message);
    return 0;
}

static void write_u16le(unsigned char* data, size_t offset, uint16_t value) {
    data[offset] = (unsigned char)value;
    data[offset + 1u] = (unsigned char)(value >> 8u);
}

static void write_u32le(unsigned char* data, size_t offset, uint32_t value) {
    data[offset] = (unsigned char)value;
    data[offset + 1u] = (unsigned char)(value >> 8u);
    data[offset + 2u] = (unsigned char)(value >> 16u);
    data[offset + 3u] = (unsigned char)(value >> 24u);
}

static int test_header_section_bounds(void) {
    unsigned char candidate[32] = {0};
    char error[256] = {0};
    uint32_t handle = 0;
    uint64_t required = 0;

    write_u32le(candidate, 0u, 88u);
    write_u16le(candidate, 4u, 2u);
    write_u32le(candidate, 6u, 16u);
    write_u32le(candidate, 10u, 18u);
    write_u16le(candidate, 14u, 0u);
    candidate[16] = 2u;
    candidate[17] = 'A';
    candidate[18] = 'B';

    if (!expect(
        terra_world_open_from_buffer(candidate, sizeof(candidate), &handle) == TERRAX_WORLD_STATUS_PARSE_ERROR,
        "native header contract: cross-section string was accepted")) return 0;
    if (!expect(handle == 0u, "native header contract: failed open returned a handle")) return 0;
    if (!expect(
        terra_info_get_last_error_json(error, sizeof(error), &required) == TERRAX_WORLD_STATUS_OK,
        "native header contract: failed to read parser error")) return 0;
    if (!expect(
        strstr(error, "\"code\":\"TERRAX_TRUNCATED_HEADER\"") != NULL &&
        strstr(error, "world name exceeds section bounds") != NULL,
        "native header contract: wrong parser error")) return 0;

    puts("native header contract: section bounds enforced");
    return 1;
}

static int test_native_terraria_header_layout(void) {
    static const char* fixture_path = TERRAX_TEST_FIXTURE_DIR "/fixtures/native-terraria-header.wld";
    unsigned char* fixture = NULL;
    size_t fixture_len = 0u;
    char* header = NULL;
    const size_t header_capacity = 20000u;
    uint64_t required = 0u;
    uint32_t handle = 0u;
    terrax_world_status status;
    int ok = 0;

    header = (char*)calloc(header_capacity, 1u);
    if (!expect(header != NULL, "native WLD layout contract: failed to allocate header output")) return 0;
    if (!expect(
        read_file_alloc(fixture_path, &fixture, &fixture_len),
        "native WLD layout contract: failed to read fixture")) goto cleanup;
    status = terra_world_open_from_buffer(fixture, (uint32_t)fixture_len, &handle);
    if (!expect(
        status == TERRAX_WORLD_STATUS_OK && handle != 0u,
        "native WLD layout contract: native Terraria header was rejected")) goto cleanup;
    status = terra_section_get_json(handle, "header", header, header_capacity, &required);
    if (!expect(
        status == TERRAX_WORLD_STATUS_OK,
        "native WLD layout contract: failed to read parsed header")) goto cleanup;
    if (!expect(
        strstr(header, "\"treeTopVariationCount\":13") != NULL &&
        strstr(header, "\"oreTierCopper\":166") != NULL,
        "native WLD layout contract: header fields were shifted")) goto cleanup;

    terra_world_close(handle); handle = 0u;
    unsigned char* extended = (unsigned char*)realloc(fixture, fixture_len + 1u);
    if (!expect(extended != NULL, "native WLD layout contract: allocation failed")) goto cleanup;
    fixture = extended;
    fixture[fixture_len++] = 0u;
    /* Header-only fixture: all later section pointers originally equal EOF. */
    for (unsigned i = 1u; i < 11u; i++) write_u32le(fixture, 26u + i * 4u, (uint32_t)fixture_len);
    if (!expect(terra_world_open_from_buffer(fixture, (uint32_t)fixture_len, &handle) == TERRAX_WORLD_STATUS_PARSE_ERROR,
                "native WLD layout contract: unconsumed header bytes accepted")) goto cleanup;

    puts("native WLD layout contract: native Terraria header accepted");
    ok = 1;

cleanup:
    if (handle != 0u) terra_world_close(handle);
    free(header);
    free(fixture);
    return ok;
}

static int test_native_abi_contract(void) {
    const char* capabilities = terra_capabilities();
    const char* build = terra_build_info_json();
    if (!expect(terra_abi_version() == 1u && capabilities && build, "native ABI contract: missing core identity")) return 1;
    if (!expect(strstr(capabilities, "world-buffer-io") != NULL, "native ABI contract: missing world-buffer-io capability")) return 1;
    if (!expect(strstr(build, "compiler") != NULL, "native ABI contract: missing compiler metadata")) return 1;
    puts("native ABI contract: ok");
    return 1;
}

/* Independent byte offsets from WorldFile.LoadWorldFlags in the checked-in
 * Terraria fixtures, not from TerraWasm's parser or header encoder:
 * gameMode=254, spawnX/Y=352/356, dualDungeons=2450,
 * v323+ lightning flags=2451/2452, manifest prefix=2453 (2451 before 323).
 * Comparing the entire file also pins all section pointers and the footer. */
static int test_header_patch_bytes(uint32_t version, uint8_t more, uint8_t none) {
    const char* path = version >= 323u
        ? TERRAX_TEST_FIXTURE_DIR "/files/1.wld"
        : TERRAX_TEST_FIXTURE_DIR "/pixel_art_output.wld";
    unsigned char *fixture = NULL, *saved = NULL;
    size_t length = 0u;
    uint32_t handle = 0u, reopened = 0u, required = 0u;
    uint64_t json_required = 0u;
    char response[512], header[20000], request[128];
    int ok = 0;

    if (!expect(read_file_alloc(path, &fixture, &length), "header patch bytes: missing game fixture")) goto cleanup;
    write_u32le(fixture, 0u, version);
    if (version >= 323u) { fixture[2451] = more; fixture[2452] = none; }
    /* The 7-bit manifest length is 9571; its JSON ends at the tile pointer. */
    size_t manifest = version >= 323u ? 2453u : 2451u;
    if (!expect(fixture[manifest] == 0xe3u && fixture[manifest + 1u] == 0x4au &&
                fixture[manifest + 2u] == '{', "header patch bytes: unexpected game fixture layout")) goto cleanup;
    if (!expect(terra_world_open_from_buffer(fixture, (uint32_t)length, &handle) == TERRAX_WORLD_STATUS_OK,
                "header patch bytes: game fixture rejected")) goto cleanup;
    if (!expect(terra_section_get_json(handle, "header", header, sizeof(header), &json_required) == TERRAX_WORLD_STATUS_OK,
                "header patch bytes: header JSON failed")) goto cleanup;
    if (version >= 323u) {
        snprintf(request, sizeof(request), "\"moreLightningSeed\":%s", more ? "true" : "false");
        if (!expect(strstr(header, request) != NULL, "header patch bytes: moreLightningSeed misread")) goto cleanup;
        snprintf(request, sizeof(request), "\"noLightningSeed\":%s", none ? "true" : "false");
        if (!expect(strstr(header, request) != NULL, "header patch bytes: noLightningSeed misread")) goto cleanup;
    } else if (!expect(strstr(header, "LightningSeed") == NULL, "header patch bytes: old version exposes new flags")) goto cleanup;

    if (!expect(terra_op_execute_json(handle, "header_patch",
                "{\"patch\":{\"spawnTileX\":4201,\"spawnTileY\":2255,\"gameMode\":3,\"revision\":1234}}",
                response, sizeof(response), &json_required) == TERRAX_WORLD_STATUS_OK,
                "header patch bytes: metadata patch failed")) goto cleanup;
    write_u32le(fixture, 12u, 1234u);
    write_u32le(fixture, 254u, 3u);
    write_u32le(fixture, 352u, 4201u);
    write_u32le(fixture, 356u, 2255u);
    saved = (unsigned char*)malloc(length);
    if (!expect(saved != NULL, "header patch bytes: output allocation failed")) goto cleanup;
    if (!expect(terra_world_save_to_buffer(handle, saved, (uint32_t)length, &required) == TERRAX_WORLD_STATUS_OK &&
                required == length && memcmp(saved, fixture, length) == 0,
                "header patch bytes: metadata patch changed unrelated bytes, pointers or manifest")) goto cleanup;

    snprintf(request, sizeof(request), "{\"patch\":{\"version\":%u}}", version >= 323u ? 322u : 323u);
    if (!expect(terra_op_execute_json(handle, "header_patch", request, response, sizeof(response), &json_required) == TERRAX_WORLD_STATUS_NOT_SUPPORTED,
                "header patch bytes: crossed lightning layout boundary")) goto cleanup;
    if (version >= 323u) {
        if (!expect(terra_op_execute_json(handle, "header_patch", "{\"patch\":{\"moreLightningSeed\":1}}",
                    response, sizeof(response), &json_required) != TERRAX_WORLD_STATUS_OK,
                    "header patch bytes: non-boolean flag accepted")) goto cleanup;
        snprintf(request, sizeof(request), "{\"patch\":{\"moreLightningSeed\":%s,\"noLightningSeed\":%s}}",
                 more ? "false" : "true", none ? "false" : "true");
        if (!expect(terra_op_execute_json(handle, "header_patch", request, response, sizeof(response), &json_required) == TERRAX_WORLD_STATUS_OK,
                    "header patch bytes: lightning patch failed")) goto cleanup;
        fixture[2451] = !more; fixture[2452] = !none;
    } else if (!expect(terra_op_execute_json(handle, "header_patch", "{\"patch\":{\"moreLightningSeed\":true}}",
                          response, sizeof(response), &json_required) == TERRAX_WORLD_STATUS_NOT_SUPPORTED,
                          "header patch bytes: old version writes lightning flags")) goto cleanup;
    if (!expect(terra_world_save_to_buffer(handle, saved, (uint32_t)length, &required) == TERRAX_WORLD_STATUS_OK &&
                required == length && memcmp(saved, fixture, length) == 0,
                "header patch bytes: flag patch or rejected patch changed unrelated bytes")) goto cleanup;
    terra_world_close(handle); handle = 0u;
    if (!expect(terra_world_open_from_buffer(saved, required, &reopened) == TERRAX_WORLD_STATUS_OK,
                "header patch bytes: saved game fixture cannot reopen")) goto cleanup;
    printf("header patch bytes: v%u flags %u/%u metadata, manifest and section bytes preserved\n", version, more, none);
    ok = 1;
cleanup:
    if (reopened) terra_world_close(reopened);
    if (handle) terra_world_close(handle);
    free(saved); free(fixture);
    return ok;
}

static int test_failed_save_preserves_destination(void) {
#ifndef TERRAX_TESTING
    fputs("native save contract: TERRAX_TESTING is required\n", stderr);
    return 0;
#else
    static const char* fixture_path = TERRAX_TEST_FIXTURE_DIR "/pixel_art_output.wld";
    static const char* temp_dir = "native_contract_tmp";
    static const char* destination_path = "native_contract_tmp/save-destination.wld";
    static const char* temp_path = "native_contract_tmp/save-destination.wld.tmp";
    static const char* temp_prefix = "save-destination.wld.tmp.";
    static const unsigned char sentinel_bytes[] = "sentinel temp sibling";
    unsigned char* fixture = NULL;
    unsigned char* saved = NULL;
    unsigned char* temp_saved = NULL;
    size_t fixture_len = 0;
    size_t saved_len = 0;
    size_t temp_saved_len = 0;
    uint32_t handle = 0;
    terrax_world_status status;
    int ok = 0;

    if (!expect(remove_if_exists_checked(temp_path), "native save contract: pre-clean temp removal failed")) goto cleanup;
    if (!expect(remove_if_exists_checked(destination_path), "native save contract: pre-clean destination removal failed")) goto cleanup;
    tx_rmdir(temp_dir);

    if (!expect(read_file_alloc(fixture_path, &fixture, &fixture_len), "native save contract: failed to read fixture")) goto cleanup;
    if (!expect(ensure_dir_exists(temp_dir), "native save contract: failed to create temp directory")) goto cleanup;
    if (!expect(write_file_bytes(destination_path, fixture, fixture_len), "native save contract: failed to seed destination")) goto cleanup;
    if (!expect(
        write_file_bytes(temp_path, sentinel_bytes, sizeof(sentinel_bytes) - 1u),
        "native save contract: failed to seed sibling temp sentinel")) goto cleanup;
    status = terra_world_open_from_buffer(fixture, (uint32_t)fixture_len, &handle);
    if (!expect(status == TERRAX_WORLD_STATUS_OK && handle != 0u, "native save contract: failed to open fixture from buffer")) goto cleanup;

    terrax_test_fail_save_after_bytes(0u);
    status = terra_world_save(handle, destination_path);
    terrax_test_reset_fail_save();
    if (!expect(status == TERRAX_WORLD_STATUS_IO_ERROR, "native save contract: save should surface IO_ERROR")) goto cleanup;
    if (!expect(read_file_alloc(destination_path, &saved, &saved_len), "native save contract: failed to re-read destination after failed save")) {
        fprintf(
            stderr,
            "native save contract: destination size=%ld temp size=%ld\n",
            file_size_or_missing(destination_path),
            file_size_or_missing(temp_path));
        goto cleanup;
    }
    if (!expect(saved_len == fixture_len, "native save contract: failed save changed destination length")) goto cleanup;
    if (!expect(memcmp(saved, fixture, fixture_len) == 0, "native save contract: failed save changed destination bytes")) goto cleanup;
    if (!expect(read_file_alloc(temp_path, &temp_saved, &temp_saved_len), "native save contract: sibling .tmp sentinel should survive failed save")) {
        fprintf(
            stderr,
            "native save contract: sentinel size=%ld unique-temp count=%d\n",
            file_size_or_missing(temp_path),
            count_files_with_prefix(temp_dir, temp_prefix));
        goto cleanup;
    }
    if (!expect(
        temp_saved_len == sizeof(sentinel_bytes) - 1u &&
        memcmp(temp_saved, sentinel_bytes, sizeof(sentinel_bytes) - 1u) == 0,
        "native save contract: failed save changed sibling .tmp sentinel")) goto cleanup;
    if (!expect(
        count_files_with_prefix(temp_dir, temp_prefix) == 0,
        "native save contract: failed save left an operation temp artifact")) goto cleanup;

    ok = 1;
    puts("native save contract: failed save preserves destination");

cleanup:
    terrax_test_reset_fail_save();
    if (handle != 0u) terra_world_close(handle);
    free(temp_saved);
    free(saved);
    free(fixture);
    if (!expect(remove_if_exists_checked(temp_path), "native save contract: cleanup temp removal failed")) ok = 0;
    if (!expect(remove_if_exists_checked(destination_path), "native save contract: cleanup destination removal failed")) ok = 0;
    if (!expect(tx_rmdir(temp_dir) == 0, "native save contract: temporary directory cleanup failed")) ok = 0;
    return ok;
#endif
}

static int test_platform_style_rules(void) {
    static const char* path = TERRAX_TEST_FIXTURE_DIR "/files/1.wld";
    static const char* invalid[] = {
        "{\"rules\":[{\"where\":{\"type\":1,\"platform_style\":0},\"patch\":{\"type\":19}}]}",
        "{\"rules\":[{\"where\":{\"type\":19},\"patch\":{\"type\":1,\"platform_style\":0}}]}",
        "{\"rules\":[{\"where\":{\"platform_style\":70},\"patch\":{\"type\":19}}]}",
        "{\"rules\":[{\"where\":{\"type\":19},\"patch\":{\"platform_style\":-1}}]}",
        "{\"rules\":[{\"where\":{\"platform_style\":1.5},\"patch\":{\"type\":19}}]}",
        "{\"rules\":[{\"where\":{\"frame_x\":-1},\"patch\":{\"type\":19}}]}",
        "{\"rules\":[{\"where\":{\"frame_y\":32768},\"patch\":{\"type\":19}}]}",
        "{\"rules\":[{\"where\":{\"frame_x\":1.5},\"patch\":{\"type\":19}}]}",
        "{\"rules\":[{\"where\":{\"frame_y\":\"18\"},\"patch\":{\"type\":19}}]}",
        "{\"rules\":[{\"where\":{\"type\":19},\"patch\":{\"frame_x\":-1}}]}",
        "{\"rules\":[{\"where\":{\"type\":19},\"patch\":{\"frame_y\":32768}}]}",
        "{\"rules\":[{\"where\":{\"type\":19},\"patch\":{\"frame_x\":1.5}}]}",
        "{\"rules\":[{\"where\":{\"type\":19},\"patch\":{\"frame_y\":\"18\"}}]}",
        "{\"rules\":[{\"where\":{\"type\":19},\"patch\":{\"platform_style\":1,\"frame_y\":18}}]}",
        "{\"rules\":[{\"where\":{\"platform_style\":1,\"frame_y\":36},\"patch\":{\"type\":19}}]}"
    };
    unsigned char* bytes = NULL;
    size_t length = 0;
    TxWorld world = {0};
    TxTile platform = {0}, ordinary = {0};
    TxTileRule* rules = NULL;
    uint32_t count = 0;
    int ok = 0;
    if (!expect(read_file_alloc(path, &bytes, &length), "platform contract: real WLD fixture missing")) return 0;
    world.file = bytes; world.file_len = (uint32_t)length;
    if (!expect(parse_format(&world) && parse_header(&world), "platform contract: fixture parse failed")) goto cleanup;
    uint32_t off = world.starts[1];
    for (int32_t x = 0; x < world.maxTilesX && (!platform.active || !ordinary.active); x++) {
        for (int32_t y = 0; y < world.maxTilesY;) {
            TxTile tile;
            if (!expect(read_tile_at(&world, &off, world.ends[1], &tile), "platform contract: fixture tile read failed")) goto cleanup;
            if (tile.active && tile.type == 19 && tile.frame_y >= 0 && tile.frame_y <= 69 * 18 && !platform.active) platform = tile;
            if (tile.active && tile.type == 1 && !ordinary.active) ordinary = tile;
            y += (int32_t)tile.same + 1;
        }
    }
    if (!expect(platform.active && ordinary.active, "platform contract: fixture lacks source platform or ordinary block")) goto cleanup;
    char json[256];
    int source_style = platform.frame_y / 18;
    int target_style = source_style == 49 ? 1 : 49;
    snprintf(json, sizeof(json), "{\"rules\":[{\"where\":{\"platform_style\":%d},\"patch\":{\"platform_style\":%d}}]}", source_style, target_style);
    if (!expect(tx_stream_parse_tile_rules(&world, json, (int)strlen(json), &rules, &count) > 0 && count == 1,
                "platform contract: style rule parse failed")) goto cleanup;
    TxTile changed = platform;
    changed.brick_style = 2;
    tx_apply_tile_rules(&changed, rules, count, 1, 0, 0, world.worldSurface);
    if (!expect(changed.type == 19 && changed.frame_x == platform.frame_x && changed.frame_y == target_style * 18 &&
                changed.brick_style == 2 && rules[0].updated == 1,
                "platform contract: material replacement lost frame X or shape")) goto cleanup;
    TxBuf encoded;
    buf_init(&encoded, 32);
    write_tile(&world, &encoded, &changed, 0);
    TxWorld packet_world = world;
    packet_world.file = encoded.data; packet_world.file_len = encoded.len;
    off = 0;
    TxTile decoded;
    int valid_packet = encoded.ok && read_tile_at(&packet_world, &off, encoded.len, &decoded) &&
        decoded.frame_x == platform.frame_x && decoded.frame_y == target_style * 18 && decoded.brick_style == 2;
    tx_internal_free(encoded.data);
    if (!expect(valid_packet, "platform contract: saved platform frame did not roundtrip")) goto cleanup;
    TxTile unmatched = platform;
    unmatched.frame_y = (int16_t)(target_style * 18);
    tx_apply_tile_rules(&unmatched, rules, count, 1, 0, 0, world.worldSurface);
    if (!expect(unmatched.frame_y == target_style * 18 && rules[0].updated == 1,
                "platform contract: source material match was too broad")) goto cleanup;
    tx_internal_free(rules); rules = NULL;
    snprintf(json, sizeof(json), "{\"rules\":[{\"where\":{\"platform_style\":%d,\"frame_x\":%d,\"frame_y\":%d},\"patch\":{\"frame_x\":126,\"frame_y\":144}}]}",
             source_style, platform.frame_x, platform.frame_y);
    if (!expect(tx_stream_parse_tile_rules(&world, json, (int)strlen(json), &rules, &count) > 0,
                "platform contract: exact source and target frames rejected")) goto cleanup;
    changed = platform;
    tx_apply_tile_rules(&changed, rules, count, 1, 0, 0, world.worldSurface);
    if (!expect(changed.frame_x == 126 && changed.frame_y == 144 && rules[0].updated == 1,
                "platform contract: exact frame patch failed")) goto cleanup;
    unmatched = platform; unmatched.frame_x++;
    tx_apply_tile_rules(&unmatched, rules, count, 1, 0, 0, world.worldSurface);
    if (!expect(unmatched.frame_x == platform.frame_x + 1 && unmatched.frame_y == platform.frame_y && rules[0].updated == 1,
                "platform contract: frame X match was too broad")) goto cleanup;
    unmatched = platform; unmatched.frame_y += 18;
    tx_apply_tile_rules(&unmatched, rules, count, 1, 0, 0, world.worldSurface);
    if (!expect(unmatched.frame_x == platform.frame_x && unmatched.frame_y == platform.frame_y + 18 && rules[0].updated == 1,
                "platform contract: frame Y match was too broad")) goto cleanup;
    tx_internal_free(rules); rules = NULL;
    snprintf(json, sizeof(json), "{\"rules\":[{\"where\":{\"type\":1},\"patch\":{\"platform_style\":%d}}]}", target_style);
    if (!expect(tx_stream_parse_tile_rules(&world, json, (int)strlen(json), &rules, &count) > 0,
                "platform contract: ordinary-to-platform rule parse failed")) goto cleanup;
    ordinary.brick_style = 3;
    tx_apply_tile_rules(&ordinary, rules, count, 1, 0, 0, world.worldSurface);
    if (!expect(ordinary.type == 19 && ordinary.frame_x == 90 && ordinary.frame_y == target_style * 18 && ordinary.brick_style == 0,
                "platform contract: ordinary-to-platform has invalid initial frame or stale slope")) goto cleanup;
    buf_init(&encoded, 32);
    write_tile(&world, &encoded, &ordinary, 0);
    packet_world.file = encoded.data; packet_world.file_len = encoded.len;
    off = 0;
    valid_packet = encoded.ok && read_tile_at(&packet_world, &off, encoded.len, &decoded) &&
        decoded.type == 19 && decoded.frame_x == 90 && decoded.frame_y == target_style * 18;
    tx_internal_free(encoded.data);
    if (!expect(valid_packet, "platform contract: ordinary-to-platform frame did not roundtrip")) goto cleanup;
    tx_internal_free(rules); rules = NULL;
    snprintf(json, sizeof(json), "{\"rules\":[{\"where\":{\"type\":1},\"patch\":{\"platform_style\":%d,\"frame_x\":108}}]}", target_style);
    if (!expect(tx_stream_parse_tile_rules(&world, json, (int)strlen(json), &rules, &count) > 0,
                "platform contract: style plus exact frame X rejected")) goto cleanup;
    ordinary.type = 1; ordinary.frame_x = ordinary.frame_y = -1;
    tx_apply_tile_rules(&ordinary, rules, count, 1, 0, 0, world.worldSurface);
    if (!expect(ordinary.type == 19 && ordinary.frame_x == 108 && ordinary.frame_y == target_style * 18,
                "platform contract: exact frame X did not override initial platform frame")) goto cleanup;
    tx_internal_free(rules); rules = NULL;
    const char* raw_rule = "{\"rules\":[{\"where\":{\"type\":1},\"patch\":{\"type\":4,\"frame_y\":36}}]}";
    if (!expect(tx_stream_parse_tile_rules(&world, raw_rule, (int)strlen(raw_rule), &rules, &count) > 0,
                "platform contract: raw frame with changed type rejected")) goto cleanup;
    ordinary.type = 1; ordinary.frame_x = ordinary.frame_y = -1;
    tx_apply_tile_rules(&ordinary, rules, count, 1, 0, 0, world.worldSurface);
    if (!expect(ordinary.type == 4 && ordinary.frame_x == 0 && ordinary.frame_y == 36,
                "platform contract: missing raw frame axis retained -1")) goto cleanup;
    tx_internal_free(rules); rules = NULL;
    const char* raw_platform = "{\"rules\":[{\"where\":{\"type\":1},\"patch\":{\"type\":19,\"frame_y\":72}}]}";
    if (!expect(tx_stream_parse_tile_rules(&world, raw_platform, (int)strlen(raw_platform), &rules, &count) > 0,
                "platform contract: raw platform frame rejected")) goto cleanup;
    ordinary.type = 1; ordinary.frame_x = ordinary.frame_y = -1;
    tx_apply_tile_rules(&ordinary, rules, count, 1, 0, 0, world.worldSurface);
    if (!expect(ordinary.type == 19 && ordinary.frame_x == 90 && ordinary.frame_y == 72,
                "platform contract: missing platform frame X was not initialized")) goto cleanup;
    tx_internal_free(rules); rules = NULL;
    const char* team_rule = "{\"rules\":[{\"where\":{\"type\":1},\"patch\":{\"type\":427}}]}";
    if (!expect(tx_stream_parse_tile_rules(&world, team_rule, (int)strlen(team_rule), &rules, &count) > 0,
                "platform contract: team platform rule parse failed")) goto cleanup;
    ordinary.type = 1; ordinary.frame_x = ordinary.frame_y = -1;
    tx_apply_tile_rules(&ordinary, rules, count, 1, 0, 0, world.worldSurface);
    if (!expect(ordinary.type == 427 && ordinary.frame_x == 90 && ordinary.frame_y == 0,
                "platform contract: team platform kept source frame")) goto cleanup;
    tx_internal_free(rules); rules = NULL;
    const char* empty_frame_rule = "{\"rules\":[{\"where\":{\"frame_x\":0,\"frame_y\":0},\"patch\":{\"platform_style\":1}}]}";
    if (!expect(tx_stream_parse_tile_rules(&world, empty_frame_rule, (int)strlen(empty_frame_rule), &rules, &count) > 0,
                "platform contract: frame-zero rule rejected")) goto cleanup;
    TxTile empty = {0};
    tx_apply_tile_rules(&empty, rules, count, 1, 0, 0, world.worldSurface);
    if (!expect(!empty.active && rules[0].updated == 0,
                "platform contract: frame zero matched an empty tile")) goto cleanup;
    tx_internal_free(rules); rules = NULL;
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        if (!expect(tx_stream_parse_tile_rules(&world, invalid[i], (int)strlen(invalid[i]), &rules, &count) < 0,
                    "platform contract: invalid style/type combination accepted")) goto cleanup;
    }
    puts("platform contract: real WLD material, frame, and validation passed");
    ok = 1;
cleanup:
    if (rules) tx_internal_free(rules);
    free(bytes);
    return ok;
}

static int test_material_frame_rules(void) {
    static const char* path = TERRAX_TEST_FIXTURE_DIR "/files/1.wld";
    unsigned char* bytes = NULL;
    size_t length = 0;
    TxWorld world = {0};
    TxTileRule* rules = NULL;
    uint32_t count = 0;
    int ok = 0;
    char json[1600];
    const char* source = "{\"frame_x\":0,\"frame_y\":0,\"width\":2,\"height\":2,\"coordinate_width\":16,\"coordinate_heights\":[16,18],\"padding\":2}";
    const char* target = "{\"frame_x\":72,\"frame_y\":54,\"width\":2,\"height\":2,\"coordinate_width\":16,\"coordinate_heights\":[18,16],\"padding\":2}";
    if (!expect(read_file_alloc(path, &bytes, &length), "material contract: fixture missing")) return 0;
    world.file = bytes; world.file_len = (uint32_t)length;
    if (!expect(parse_format(&world) && parse_header(&world), "material contract: fixture parse failed")) goto cleanup;
    snprintf(json, sizeof(json), "{\"rules\":[{\"where\":{\"type\":21,\"material\":%s},\"patch\":{\"material\":%s}}]}", source, target);
    if (!expect(tx_stream_parse_tile_rules(&world, json, (int)strlen(json), &rules, &count) > 0 && count == 1,
                "material contract: 2x2 layout rejected")) goto cleanup;
    for (int row = 0; row < 2; row++) for (int col = 0; col < 2; col++) {
        TxTile tile = {0}; tile.active = 1; tile.type = 21;
        tile.frame_x = (int16_t)(col * 18); tile.frame_y = (int16_t)(row * 18);
        tx_apply_tile_rules(&tile, rules, count, 1, 0, 0, world.worldSurface);
        if (!expect(tile.type == 21 && tile.frame_x == 72 + col * 18 && tile.frame_y == 54 + row * 20,
                    "material contract: subcell offset lost")) goto cleanup;
    }
    TxTile tile = {0}; tile.active = 1; tile.type = 21; tile.frame_x = 1; tile.frame_y = 0;
    tx_apply_tile_rules(&tile, rules, count, 1, 0, 0, world.worldSurface);
    if (!expect(tile.frame_x == 1 && rules[0].updated == 4,
                "material contract: neighboring frame matched")) goto cleanup;
    tx_internal_free(rules); rules = NULL;
    snprintf(json, sizeof(json), "{\"rules\":[{\"where\":{\"type\":21,\"material\":%s},\"patch\":{\"material\":%s}},{\"where\":{\"type\":21,\"material\":%s},\"patch\":{\"frame_x\":300}}]}", source, target, target);
    if (!expect(tx_stream_parse_tile_rules(&world, json, (int)strlen(json), &rules, &count) > 0,
                "material contract: ordered material rules rejected")) goto cleanup;
    tile.frame_x = tile.frame_y = 0;
    tx_apply_tile_rules(&tile, rules, count, 1, 0, 0, world.worldSurface);
    if (!expect(tile.frame_x == 72 && rules[1].updated == 0,
                "material contract: rule cascade transformed same cell twice")) goto cleanup;
    tx_internal_free(rules); rules = NULL;
    snprintf(json, sizeof(json), "{\"rules\":[{\"where\":{\"type\":21,\"material\":%s},\"patch\":{\"tile_color\":3}},{\"where\":{\"type\":21,\"material\":%s},\"patch\":{\"fullbright_block\":1}}]}", source, source);
    if (!expect(tx_stream_parse_tile_rules(&world, json, (int)strlen(json), &rules, &count) > 0,
                "material contract: independent material rules rejected")) goto cleanup;
    tile.frame_x = tile.frame_y = 0; tile.tile_color = tile.fullbright_block = 0;
    tx_apply_tile_rules(&tile, rules, count, 1, 0, 0, world.worldSurface);
    if (!expect(tile.tile_color == 3 && tile.fullbright_block == 1 &&
                rules[0].updated == 1 && rules[1].updated == 1,
                "material contract: independent material patches did not compose")) goto cleanup;
    tx_internal_free(rules); rules = NULL;
    const char* one_cell = "{\"frame_x\":0,\"frame_y\":0,\"width\":1,\"height\":1,\"coordinate_width\":16,\"coordinate_heights\":[16],\"padding\":2}";
    snprintf(json, sizeof(json), "{\"rules\":[{\"where\":{\"type\":21,\"material\":%s},\"patch\":{\"type\":19,\"platform_style\":2}}]}", one_cell);
    if (!expect(tx_stream_parse_tile_rules(&world, json, (int)strlen(json), &rules, &count) > 0,
                "material contract: 1x1 to platform rejected")) goto cleanup;
    tile.frame_x = tile.frame_y = 0; tile.type = 21;
    tx_apply_tile_rules(&tile, rules, count, 1, 0, 0, world.worldSurface);
    if (!expect(tile.type == 19 && tile.frame_x == 90 && tile.frame_y == 36,
                "material contract: 1x1 to platform frame invalid")) goto cleanup;
    tx_internal_free(rules); rules = NULL;
    snprintf(json, sizeof(json), "{\"rules\":[{\"where\":{\"type\":21,\"material\":%s},\"patch\":{\"type\":4,\"frame_x\":18,\"frame_y\":36}}]}", one_cell);
    if (!expect(tx_stream_parse_tile_rules(&world, json, (int)strlen(json), &rules, &count) > 0,
                "material contract: 1x1 to raw frame rejected")) goto cleanup;
    tile.frame_x = tile.frame_y = 0; tile.type = 21;
    tx_apply_tile_rules(&tile, rules, count, 1, 0, 0, world.worldSurface);
    if (!expect(tile.type == 4 && tile.frame_x == 18 && tile.frame_y == 36,
                "material contract: 1x1 to raw frame failed")) goto cleanup;
    tx_internal_free(rules); rules = NULL;
    snprintf(json, sizeof(json), "{\"rules\":[{\"where\":{\"type\":21,\"material\":%s},\"patch\":{\"type\":1}}]}", one_cell);
    if (!expect(tx_stream_parse_tile_rules(&world, json, (int)strlen(json), &rules, &count) > 0,
                "material contract: 1x1 to unframed tile rejected")) goto cleanup;
    tile.frame_x = tile.frame_y = 0; tile.type = 21;
    tx_apply_tile_rules(&tile, rules, count, 1, 0, 0, world.worldSurface);
    if (!expect(tile.type == 1, "material contract: 1x1 to unframed tile failed")) goto cleanup;
    tx_internal_free(rules); rules = NULL;
    const char* invalid_target = "{\"frame_x\":72,\"frame_y\":54,\"width\":1,\"height\":2,\"coordinate_width\":16,\"coordinate_heights\":[16,16],\"padding\":2}";
    snprintf(json, sizeof(json), "{\"rules\":[{\"where\":{\"type\":21,\"material\":%s},\"patch\":{\"material\":%s}}]}", source, invalid_target);
    if (!expect(tx_stream_parse_tile_rules(&world, json, (int)strlen(json), &rules, &count) < 0,
                "material contract: cross-dimension conversion accepted")) goto cleanup;
    snprintf(json, sizeof(json), "{\"rules\":[{\"where\":{\"type\":21,\"material\":%s},\"patch\":{\"type\":55,\"material\":%s}}]}", source, target);
    if (!expect(tile_important(&world, 55), "material contract: sign target must be frame-important")) goto cleanup;
    if (!expect(tx_stream_parse_tile_rules(&world, json, (int)strlen(json), &rules, &count) < 0,
                "material contract: multi-cell cross-ID conversion accepted")) goto cleanup;
    snprintf(json, sizeof(json), "{\"rules\":[{\"where\":{\"type\":21,\"material\":%s},\"patch\":{\"tile_color\":3},\"limit\":1}]}", source);
    if (!expect(tx_stream_parse_tile_rules(&world, json, (int)strlen(json), &rules, &count) < 0,
                "material contract: partial multi-cell limit accepted")) goto cleanup;
    snprintf(json, sizeof(json), "{\"rules\":[{\"where\":{\"type\":21,\"material\":%s},\"patch\":{\"material\":%s,\"frame_x\":5}}]}", source, target);
    if (!expect(tx_stream_parse_tile_rules(&world, json, (int)strlen(json), &rules, &count) < 0,
                "material contract: conflicting raw frame override accepted")) goto cleanup;
    puts("material contract: 2x2 subcells, matching, cascade, and validation passed");
    ok = 1;
cleanup:
    if (rules) tx_internal_free(rules);
    free(bytes);
    return ok;
}

static int test_string_reader_bounds(void) {
    static const unsigned char truncated[] = {5u, 'A'};
    static const unsigned char valid[] = {5u, 'h', 'e', 'l', 'l', 'o'};
    static const unsigned char malformed_7bit[] = {0x80u, 0x80u, 0x80u, 0x80u, 0x80u};
    char output[8] = "sentinel";
    uint32_t off = 0u;

    rd_string_copy(truncated, (uint32_t)sizeof(truncated), &off, output, sizeof(output));
    if (!expect(off == sizeof(truncated), "native reader contract: truncated string offset did not clamp")) return 0;
    if (!expect(output[0] == '\0', "native reader contract: truncated string output was not cleared")) return 0;

    off = 0u;
    memset(output, 0x7f, sizeof(output));
    rd_string_copy(valid, (uint32_t)sizeof(valid), &off, output, 4u);
    if (!expect(off == sizeof(valid), "native reader contract: valid string offset did not advance")) return 0;
    if (!expect(strcmp(output, "hel") == 0, "native reader contract: bounded copy did not terminate correctly")) return 0;

    off = 0u;
    rd_string_copy(valid, (uint32_t)sizeof(valid), &off, NULL, 0u);
    if (!expect(off == sizeof(valid), "native reader contract: zero-capacity copy did not consume the string")) return 0;

    off = 0u;
    output[0] = 'X';
    rd_string_copy(malformed_7bit, (uint32_t)sizeof(malformed_7bit), &off, output, sizeof(output));
    if (!expect(off == sizeof(malformed_7bit), "native reader contract: malformed 7-bit length did not clamp")) return 0;
    if (!expect(output[0] == '\0', "native reader contract: malformed 7-bit output was not cleared")) return 0;

    off = 0u;
    rd_skip_string_value(truncated, (uint32_t)sizeof(truncated), &off);
    if (!expect(off == sizeof(truncated), "native reader contract: truncated skipped string did not clamp")) return 0;

    puts("native reader contract: bounded string readers enforced");
    return 1;
}
int main(void) {
    if (!test_native_abi_contract()) return 1;
    if (!test_header_section_bounds()) return 2;
    if (!test_native_terraria_header_layout()) return 3;
    if (!test_platform_style_rules()) return 7;
    if (!test_material_frame_rules()) return 8;
    if (!test_string_reader_bounds()) return 4;
    if (!test_failed_save_preserves_destination()) return 5;
    if (!test_header_patch_bytes(318u, 0u, 0u) || !test_header_patch_bytes(322u, 0u, 0u) ||
        !test_header_patch_bytes(323u, 1u, 0u) || !test_header_patch_bytes(323u, 0u, 1u) ||
        !test_header_patch_bytes(326u, 0u, 0u) || !test_header_patch_bytes(326u, 1u, 1u)) return 6;
    return 0;
}
