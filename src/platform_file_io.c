/* Same-directory exclusive publication; not a durability or managed-CAS API. */
#include "platform_file_io.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <fcntl.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <wchar.h>
typedef wchar_t FileChar;
#define FILE_TEXT(s) L##s
#define file_open _wfopen
#define file_remove _wremove
#else
#include <unistd.h>
typedef char FileChar;
#define FILE_TEXT(s) s
#define file_open fopen
#define file_remove remove
#endif

#ifdef _WIN32
static FileChar *windows_path(const char *path, const TxFileAllocator *memory) {
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (count <= 0 || (uint64_t)count * sizeof(FileChar) > UINT32_MAX) return NULL;
    FileChar *wide = memory->allocate((size_t)count * sizeof(FileChar));
    if (!wide) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, count) != count) {
        memory->release(wide);
        return NULL;
    }
    return wide;
}
#endif

FILE *tx_file_open_utf8(const char *path, int writing, const TxFileAllocator *memory) {
    if (!path || !memory || !memory->allocate || !memory->release) return NULL;
#ifdef _WIN32
    FileChar *wide = windows_path(path, memory);
    if (!wide) return NULL;
    FILE *file = file_open(wide, writing ? FILE_TEXT("wb") : FILE_TEXT("rb"));
    memory->release(wide);
    return file;
#else
    return file_open(path, writing ? "wb" : "rb");
#endif
}

static int candidate_matches(const FileChar *path, const uint8_t *data, uint32_t length) {
    FILE *file = file_open(path, FILE_TEXT("rb"));
    if (!file) return 0;
    uint8_t chunk[4096];
    uint32_t offset = 0;
    int ok = 1;
    while (ok && offset < length) {
        uint32_t n = length - offset;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (fread(chunk, 1, n, file) != n || memcmp(chunk, data + offset, n)) ok = 0;
        offset += n;
    }
    if (ok && (fgetc(file) != EOF || ferror(file))) ok = 0;
    if (fclose(file)) ok = 0;
    return ok;
}

int tx_file_publish_utf8(const char *path, const uint8_t *data, uint32_t length,
    const TxFileAllocator *memory, const TxFilePublishOptions *options) {
    if (!path || (!data && length) || !memory || !memory->allocate ||
        !memory->release || !options || !options->attempts) return 0;
#ifdef _WIN32
    FileChar *target = windows_path(path, memory);
    if (!target) return 0;
    size_t n = wcslen(target);
    unsigned long pid = (unsigned long)_getpid();
#else
    const FileChar *target = path;
    size_t n = strlen(path);
    unsigned long pid = (unsigned long)getpid();
#endif
    FileChar *candidate = NULL;
    FILE *file = NULL;
    int ok = 0;
    int created = 0;
    static uint32_t sequence;
    /* Both path buffers exist before creation; cleanup never allocates. */
    if (n > UINT32_MAX / sizeof(FileChar) - 80u) goto cleanup;
    candidate = memory->allocate((n + 80u) * sizeof(FileChar));
    if (!candidate) goto cleanup;
    for (unsigned attempt = 0; attempt < options->attempts; ++attempt) {
#ifdef _WIN32
        int count = swprintf(candidate, n + 80u, L"%ls.tmp.%lu.%lu",
            target, pid, (unsigned long)++sequence);
        if (count < 0 || (size_t)count >= n + 80u) break;
        int fd = _wopen(candidate, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
            _S_IREAD | _S_IWRITE);
#else
        int count = snprintf(candidate, n + 80u, "%s.tmp.%lu.%lu",
            target, pid, (unsigned long)++sequence);
        if (count < 0 || (size_t)count >= n + 80u) break;
        int fd = open(candidate, O_WRONLY | O_CREAT | O_EXCL, options->posix_mode);
#endif
        if (fd >= 0) {
            created = 1;
#ifdef _WIN32
            file = _fdopen(fd, "wb");
            if (!file) _close(fd);
#else
            file = fdopen(fd, "wb");
            if (!file) close(fd);
#endif
            break;
        }
        if (errno != EEXIST) break;
    }
    if (!file) goto cleanup;
    size_t requested = length;
#ifdef TERRAX_TESTING
    if (requested > options->write_limit) requested = options->write_limit;
#endif
    size_t written = fwrite(data, 1, requested, file);
    ok = written == length && fflush(file) == 0;
    if (fclose(file)) ok = 0;
#ifdef TERRAX_TESTING
    if (ok && options->corrupt_candidate) {
        FILE *corrupt = file_open(candidate,
            options->corrupt_candidate == 3 ? FILE_TEXT("wb") : FILE_TEXT("r+b"));
        if (!corrupt) ok = 0;
        else {
            if (options->corrupt_candidate == 1) {
                int first = fgetc(corrupt);
                if (first == EOF || fseek(corrupt, 0, SEEK_SET) || fputc(first ^ 0xff, corrupt) == EOF) ok = 0;
            }
            if (options->corrupt_candidate == 2 &&
                (fseek(corrupt, 0, SEEK_END) || fputc(0, corrupt) == EOF)) ok = 0;
            if (fclose(corrupt)) ok = 0;
        }
    }
#endif
    if (ok) ok = candidate_matches(candidate, data, length);
#ifdef _WIN32
    if (ok) ok = MoveFileExW(candidate, target,
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    if (ok) ok = rename(candidate, target) == 0;
#endif
cleanup:
    /* Never remove a path whose exclusive creation failed (including EEXIST). */
    if (created && !ok && file_remove(candidate) != 0 && errno != ENOENT) ok = 0;
    if (candidate) memory->release(candidate);
#ifdef _WIN32
    memory->release(target);
#endif
    return ok;
}
