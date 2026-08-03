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

static int test_native_abi_contract(void) {
    const char* capabilities = terra_capabilities();
    const char* build = terra_build_info_json();
    if (!expect(terra_abi_version() == 1u && capabilities && build, "native ABI contract: missing core identity")) return 1;
    if (!expect(strstr(capabilities, "world-buffer-io") != NULL, "native ABI contract: missing world-buffer-io capability")) return 1;
    if (!expect(strstr(build, "compiler") != NULL, "native ABI contract: missing compiler metadata")) return 1;
    puts("native ABI contract: ok");
    return 1;
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

int main(void) {
    if (!test_native_abi_contract()) return 1;
    if (!test_header_section_bounds()) return 2;
    if (!test_failed_save_preserves_destination()) return 3;
    return 0;
}
