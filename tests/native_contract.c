#include "terra_abi.h"
#include "terra_world.h"
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
    if (!test_string_reader_bounds()) return 4;
    if (!test_failed_save_preserves_destination()) return 5;
    if (!test_header_patch_bytes(318u, 0u, 0u) || !test_header_patch_bytes(322u, 0u, 0u) ||
        !test_header_patch_bytes(323u, 1u, 0u) || !test_header_patch_bytes(323u, 0u, 1u) ||
        !test_header_patch_bytes(326u, 0u, 0u) || !test_header_patch_bytes(326u, 1u, 1u)) return 6;
    return 0;
}
