#ifndef TERRA_PLATFORM_FILE_IO_H
#define TERRA_PLATFORM_FILE_IO_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Internal filesystem port. Allocation remains in the calling owner's domain. */
typedef struct TxFileAllocator {
    void *(*allocate)(size_t);
    void (*release)(void *);
} TxFileAllocator;
typedef struct TxFilePublishOptions {
    unsigned posix_mode; /* WLD: 0600; PLR: 0666, both subject to umask. */
    unsigned attempts;
#ifdef TERRAX_TESTING
    uint32_t write_limit;
    int corrupt_candidate;
#endif
} TxFilePublishOptions;
FILE *tx_file_open_utf8(const char *path, int writing, const TxFileAllocator *memory);
/* Completed candidates are reopened for byte verification. If permissions
 * (for example an owner-read-masking umask) prevent verification, publication
 * fails and leaves the destination intact; it never widens candidate modes. */
int tx_file_publish_utf8(const char *path, const uint8_t *data, uint32_t length,
    const TxFileAllocator *memory, const TxFilePublishOptions *options);
#endif
