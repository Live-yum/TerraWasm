/* Run against the native filesystem: UTF-8 ABI paths must not depend on ACP. */
#include "terra_world.h"
#include "platform_file_io.h"
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
extern void terrax_test_fail_save_after_bytes(uint32_t);
extern void terrax_test_reset_fail_save(void);

#define DIRECTORY_NAME "terra-wld-\xe4\xb8\x96\xe7\x95\x8c-\xf0\x9f\x8c\x8d"
#define PLAYER_NAME "\xe7\x8e\xa9\xe5\xae\xb6-\xf0\x9f\x8e\xae.wld"

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

/* Direct port tests run before any successful candidate allocation, so the
 * first exclusive-create attempt has sequence 1 in this fresh process. */
static int allocation_budget = -1;
static unsigned live_allocations;
static void *fault_allocate(size_t size) {
    if (!allocation_budget) return NULL;
    if (allocation_budget > 0) --allocation_budget;
    void *memory = malloc(size);
    if (memory) ++live_allocations;
    return memory;
}
static void fault_release(void *memory) {
    if (memory) { assert(live_allocations); --live_allocations; free(memory); }
}
static void test_file_port(const char *directory, const char *path, unsigned long pid) {
    static const uint8_t original[] = "retained destination";
    static const uint8_t replacement[] = "replacement";
    static const uint8_t collision[] = "unowned exclusive candidate";
    const TxFileAllocator memory = { fault_allocate, fault_release };
    TxFilePublishOptions options = {0};
    options.posix_mode = 0600;
    options.attempts = 1;
    options.write_limit = UINT32_MAX;
    write_bytes(path, original, sizeof(original));
#ifdef _WIN32
    /* Failure of target UTF-16 allocation and then candidate allocation. */
    for (int budget = 0; budget < 2; ++budget) {
#else
    for (int budget = 0; budget < 1; ++budget) {
#endif
        allocation_budget = budget;
        assert(!tx_file_publish_utf8(path, replacement, sizeof(replacement), &memory, &options));
        assert(!live_allocations);
        exact_file(path, original, sizeof(original)); no_candidates(directory);
    }
#ifdef _WIN32
    allocation_budget = 0;
    assert(!tx_file_open_utf8(path, 0, &memory));
    assert(!live_allocations);
#endif
    allocation_budget = -1;
    char candidate[640];
    snprintf(candidate, sizeof(candidate), "%s.tmp.%lu.1", path, pid);
    write_bytes(candidate, collision, sizeof(collision));
    assert(!tx_file_publish_utf8(path, replacement, sizeof(replacement), &memory, &options));
    assert(!live_allocations);
    exact_file(path, original, sizeof(original));
    exact_file(candidate, collision, sizeof(collision));
    remove_file(candidate); no_candidates(directory);
#ifndef _WIN32
    /* Verification must never widen mode bits to bypass an unusual umask.
     * Privileged users can read mode-000 files, so this check is for ordinary
     * filesystem users (the native/sanitizer CI jobs run unprivileged). */
    if (geteuid() != 0) {
        mode_t saved_umask = umask(0777);
        int published = tx_file_publish_utf8(path, replacement, sizeof(replacement), &memory, &options);
        umask(saved_umask);
        assert(!published && !live_allocations);
        exact_file(path, original, sizeof(original)); no_candidates(directory);
    }
#endif
}

int main(void) {
    const uint32_t baseline = tx_heap_used();
#ifndef _WIN32
    mode_t previous_umask = umask(0027);
#endif
    FILE *fixture = fopen(TERRAX_TEST_FIXTURE_DIR "/pixel_art_output.wld", "rb");
    assert(fixture);
    assert(!fseek(fixture, 0, SEEK_END));
    long size = ftell(fixture);
    assert(size >= 16 && (uint64_t)size <= UINT32_MAX);
    rewind(fixture);
    uint8_t *bytes = malloc((size_t)size); assert(bytes);
    assert(fread(bytes, 1, (size_t)size, fixture) == (size_t)size);
    assert(!fclose(fixture));
    static const uint8_t original[] = "original destination must survive";
    char directory[128], path[512], blocked[512];
#ifdef _WIN32
    unsigned long pid = (unsigned long)_getpid();
#else
    unsigned long pid = (unsigned long)getpid();
#endif
    snprintf(directory, sizeof(directory), "%s-%lu", DIRECTORY_NAME, pid);
    snprintf(path, sizeof(path), "%s/%s", directory, PLAYER_NAME);
    snprintf(blocked, sizeof(blocked), "%s/blocked", directory);
    make_directory(directory);
    test_file_port(directory, path, pid);
    for (unsigned cycle = 0; cycle < 16; ++cycle) {
        /* Exercise UTF-8 open as well as publication. WLD permits one owner. */
        write_bytes(path, bytes, (uint32_t)size);
        uint32_t world = 0;
        assert(terra_world_open(path, &world) == TERRAX_WORLD_STATUS_OK && world);
        write_bytes(path, original, sizeof(original));
        terrax_test_fail_save_after_bytes(8);
        assert(terra_world_save(world, path) == TERRAX_WORLD_STATUS_IO_ERROR);
        terrax_test_reset_fail_save();
        exact_file(path, original, sizeof(original)); no_candidates(directory);
        make_directory(blocked);
        assert(terra_world_save(world, blocked) == TERRAX_WORLD_STATUS_IO_ERROR);
        remove_directory(blocked);
        exact_file(path, original, sizeof(original)); no_candidates(directory);
#ifdef _WIN32
        wchar_t wide[512]; wide_path(path, wide);
        HANDLE lock = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        assert(lock != INVALID_HANDLE_VALUE);
        assert(terra_world_save(world, path) == TERRAX_WORLD_STATUS_IO_ERROR);
        assert(CloseHandle(lock));
        exact_file(path, original, sizeof(original)); no_candidates(directory);
        const char *bad_suffixes[] = { "/bad-\xc3\x28.wld", "/bad-\xc0\xaf.wld",
            "/bad-\xed\xa0\x80.wld", "/bad-\xf4\x90\x80\x80.wld" };
        for (unsigned i = 0; i < sizeof(bad_suffixes) / sizeof(bad_suffixes[0]); ++i) {
            char invalid[512]; snprintf(invalid, sizeof(invalid), "%s%s", directory, bad_suffixes[i]);
            assert(terra_world_save(world, invalid) == TERRAX_WORLD_STATUS_IO_ERROR);
            exact_file(path, original, sizeof(original)); no_candidates(directory);
        }
#endif
        assert(terra_world_save(world, path) == TERRAX_WORLD_STATUS_OK);
        exact_file(path, bytes, (uint32_t)size); no_candidates(directory);
#ifndef _WIN32
        struct stat info; assert(!stat(path, &info));
        assert((info.st_mode & 0777) == 0600);
#endif
        assert(terra_world_close(world) == TERRAX_WORLD_STATUS_OK);
#ifdef _WIN32
        for (unsigned i = 0; i < sizeof(bad_suffixes) / sizeof(bad_suffixes[0]); ++i) {
            char invalid[512]; snprintf(invalid, sizeof(invalid), "%s%s", directory, bad_suffixes[i]);
            uint32_t invalid_world = 99;
            assert(terra_world_open(invalid, &invalid_world) == TERRAX_WORLD_STATUS_IO_ERROR);
            assert(!invalid_world); no_candidates(directory);
        }
#endif
        uint32_t reopened = 0;
        assert(terra_world_open(path, &reopened) == TERRAX_WORLD_STATUS_OK && reopened);
        uint32_t required = 0;
        assert(terra_world_save_to_buffer(reopened, NULL, 0, &required) == TERRAX_WORLD_STATUS_OK);
        assert(required == (uint32_t)size);
        uint8_t *saved = malloc(required); assert(saved);
        assert(terra_world_save_to_buffer(reopened, saved, required, &required) == TERRAX_WORLD_STATUS_OK);
        assert(!memcmp(saved, bytes, required)); free(saved);
        assert(terra_world_close(reopened) == TERRAX_WORLD_STATUS_OK);
        assert(tx_heap_used() == baseline);
    }
    remove_file(path); remove_directory(directory); free(bytes);
#ifndef _WIN32
    umask(previous_umask);
#endif
    assert(tx_heap_used() == baseline);
    puts("WLD UTF-8 filesystem: 16 Unicode/emoji open/save cycles; failed writes and replacement preserve original bytes, with no candidates or owner leaks");
    return 0;
}
