/* Run against the native filesystem: UTF-8 ABI paths must not depend on ACP. */
#include "terra_plr.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <process.h>
#include <wchar.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

extern uint32_t tx_heap_used(void);
extern void terrax_test_plr_write_limit(uint32_t);
extern void terrax_test_plr_corrupt_candidate(int);
extern void terrax_test_plr_fail_alloc_after(int32_t);

#define DIRECTORY_NAME "terra-plr-\xe4\xb8\x96\xe7\x95\x8c-\xf0\x9f\x8c\x8d"
#define PLAYER_NAME "\xe7\x8e\xa9\xe5\xae\xb6-\xf0\x9f\x8e\xae.plr"

#ifdef _WIN32
static void wide_path(const char *path, wchar_t *wide) {
    assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 512) > 0);
}
static FILE *file_open(const char *path, int writing) {
    wchar_t wide[512]; wide_path(path, wide);
    return _wfopen(wide, writing ? L"wb" : L"rb");
}
static void make_directory(const char *path) {
    wchar_t wide[512]; wide_path(path, wide); assert(CreateDirectoryW(wide, NULL));
}
static void remove_directory(const char *path) {
    wchar_t wide[512]; wide_path(path, wide); assert(RemoveDirectoryW(wide));
}
static void remove_file(const char *path) {
    wchar_t wide[512]; wide_path(path, wide); assert(DeleteFileW(wide));
}
#else
static FILE *file_open(const char *path, int writing) { return fopen(path, writing ? "wb" : "rb"); }
static void make_directory(const char *path) { assert(!mkdir(path, 0700)); }
static void remove_directory(const char *path) { assert(!rmdir(path)); }
static void remove_file(const char *path) { assert(!remove(path)); }
#endif

static void write_bytes(const char *path, const uint8_t *bytes, uint32_t length) {
    FILE *file = file_open(path, 1); assert(file);
    assert(fwrite(bytes, 1, length, file) == length);
    assert(!fclose(file));
}
static void exact_file(const char *path, const uint8_t *bytes, uint32_t length) {
    FILE *file = file_open(path, 0); assert(file);
    for (uint32_t i = 0; i < length; ++i) assert(fgetc(file) == bytes[i]);
    assert(fgetc(file) == EOF && !ferror(file));
    assert(!fclose(file));
}
static void no_candidates(const char *directory) {
    unsigned files = 0;
#ifdef _WIN32
    char pattern[512]; snprintf(pattern, sizeof(pattern), "%s/*", directory);
    wchar_t wide[512], name[512]; wide_path(pattern, wide); wide_path(PLAYER_NAME, name);
    WIN32_FIND_DATAW data; HANDLE search = FindFirstFileW(wide, &data);
    assert(search != INVALID_HANDLE_VALUE);
    do {
        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
        assert(!wcscmp(data.cFileName, name)); ++files;
    } while (FindNextFileW(search, &data));
    assert(GetLastError() == ERROR_NO_MORE_FILES); assert(FindClose(search));
#else
    DIR *dir = opendir(directory); assert(dir); struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        assert(!strcmp(entry->d_name, PLAYER_NAME)); ++files;
    }
    assert(!closedir(dir));
#endif
    assert(files == 1);
}
static uint8_t *encoded(uint32_t handle, uint32_t *length) {
    assert(!terra_plr_save_to_buffer(handle, NULL, 0, length) && *length);
    uint8_t *bytes = malloc(*length); assert(bytes);
    assert(!terra_plr_save_to_buffer(handle, bytes, *length, length)); return bytes;
}

int main(void) {
    const uint32_t baseline = tx_heap_used();
    FILE *fixture = fopen(TERRAWASM_PLR_JSON_FIXTURE_PATH, "rb"); assert(fixture);
    assert(!fseek(fixture, 0, SEEK_END)); long size = ftell(fixture);
    assert(size > 0 && size < 1024 * 1024); rewind(fixture);
    char *json = malloc((size_t)size + 1); assert(json);
    assert(fread(json, 1, (size_t)size, fixture) == (size_t)size); json[size] = 0;
    assert(!fclose(fixture)); uint32_t player = 0;
    assert(!terra_plr_open_json(json, &player)); free(json);
    uint32_t old_length = 0, new_length = 0;
    uint8_t *old_bytes = encoded(player, &old_length);
    assert(!terra_plr_set(player, "/name", "\"Unicode publication\""));
    uint8_t *new_bytes = encoded(player, &new_length);
    const uint32_t retained = tx_heap_used();

    char directory[128], path[512], blocked[512];
#ifdef _WIN32
    unsigned long pid = (unsigned long)_getpid();
#else
    unsigned long pid = (unsigned long)getpid();
#endif
    snprintf(directory, sizeof(directory), "%s-%lu", DIRECTORY_NAME, pid);
    snprintf(path, sizeof(path), "%s/%s", directory, PLAYER_NAME);
    snprintf(blocked, sizeof(blocked), "%s/blocked", directory);
    make_directory(directory); write_bytes(path, old_bytes, old_length);
    /* One process, one document, and the same directory throughout every cycle. */
    for (unsigned cycle = 0; cycle < 16; ++cycle) {
        write_bytes(path, old_bytes, old_length);
        terrax_test_plr_write_limit(8);
        assert(terra_plr_save(player, path) == TERRAX_WORLD_STATUS_IO_ERROR);
        terrax_test_plr_write_limit(UINT32_MAX);
        exact_file(path, old_bytes, old_length); no_candidates(directory);
        for (int mode = 1; mode <= 3; ++mode) {
            terrax_test_plr_corrupt_candidate(mode);
            assert(terra_plr_save(player, path) == TERRAX_WORLD_STATUS_IO_ERROR);
            terrax_test_plr_corrupt_candidate(0);
            exact_file(path, old_bytes, old_length); no_candidates(directory);
        }
        terrax_test_plr_fail_alloc_after(0);
        assert(terra_plr_save(player, path) != TERRAX_WORLD_STATUS_OK);
        terrax_test_plr_fail_alloc_after(-1);
        exact_file(path, old_bytes, old_length); no_candidates(directory);
        make_directory(blocked);
        assert(terra_plr_save(player, blocked) == TERRAX_WORLD_STATUS_IO_ERROR);
        remove_directory(blocked); no_candidates(directory);
#ifdef _WIN32
        wchar_t wide[512]; wide_path(path, wide);
        /* Real sharing violation at the final MoveFileExW boundary. */
        HANDLE lock = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        assert(lock != INVALID_HANDLE_VALUE);
        assert(terra_plr_save(player, path) == TERRAX_WORLD_STATUS_IO_ERROR);
        assert(CloseHandle(lock)); exact_file(path, old_bytes, old_length); no_candidates(directory);
        char invalid[512]; snprintf(invalid, sizeof(invalid), "%s/bad-\xc3\x28.plr", directory);
        assert(terra_plr_save(player, invalid) == TERRAX_WORLD_STATUS_IO_ERROR);
        uint32_t invalid_player = 99;
        assert(terra_plr_open(invalid, &invalid_player) == TERRAX_WORLD_STATUS_IO_ERROR && !invalid_player);
        no_candidates(directory);
#endif
        assert(tx_heap_used() == retained);
        assert(!terra_plr_save(player, path)); exact_file(path, new_bytes, new_length);
        no_candidates(directory);
        uint32_t reopened = 0; assert(!terra_plr_open(path, &reopened));
        uint32_t reopened_length = 0; uint8_t *reopened_bytes = encoded(reopened, &reopened_length);
        assert(reopened_length == new_length && !memcmp(reopened_bytes, new_bytes, new_length));
        free(reopened_bytes); assert(!terra_plr_close(reopened)); assert(tx_heap_used() == retained);
    }
    remove_file(path); remove_directory(directory);
    free(old_bytes); free(new_bytes); assert(!terra_plr_close(player));
    assert(tx_heap_used() == baseline);
    puts("PLR UTF-8 filesystem: 16 continuous Unicode/emoji publication cycles; short/corrupt/truncated/OOM and replacement failures preserve originals without candidates or owner leaks");
#ifdef _WIN32
    puts("Windows: strict UTF-8 paths and actual sharing-denied replacement verified");
#endif
    return 0;
}
