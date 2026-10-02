/*
 * terra_txci.h -- TXCI v3 color index loader and lookup.
 *
 * Loads a .txci binary file (TerraX Color Index v3) and provides O(1)
 * RGB-to-group lookups via brick-based decompression.
 */
#ifndef TERRA_TXCI_H
#define TERRA_TXCI_H

#include <stdint.h>

/* TXCI v3 binary format constants */
#define TXCI_MAGIC             0x49435854  /* "TXCI" little-endian */
#define TXCI_VERSION           3
#define TXCI_HEADER_SIZE       44
#define TXCI_ITEM_SIZE         6
#define TXCI_DIR_SIZE          8

/* Block types in the directory */
#define TXCI_BLOCK_UNIFORM  0
#define TXCI_BLOCK_PAL4     1
#define TXCI_BLOCK_PAL8     2
#define TXCI_BLOCK_RAW16    3

/* Item kind flags */
#define TXCI_KIND_WALL      0x8000
#define TXCI_KIND_ID_MASK   0x7FFF

/* Result of a color lookup */
typedef struct TxciItem {
    uint16_t type_id;
    uint8_t  is_wall;
    uint16_t variant;
    uint8_t  paint_id;
} TxciItem;

/* Loaded TXCI index in memory */
typedef struct TxciIndex {
    const uint8_t* data;       /* full file loaded into memory */
    uint32_t       data_len;
    /* Parsed header */
    uint16_t brick_size;
    uint16_t brick_shift;      /* log2(brick_size) */
    uint16_t brick_mask;       /* brick_size - 1 */
    uint16_t grid;             /* 256 / brick_size */
    uint32_t color_count;
    uint32_t item_count;
    uint32_t brick_count;
    /* Section pointers (into data[]) */
    const uint8_t*  colors;        /* color_count * 3 bytes */
    const uint32_t* group_offsets; /* (color_count+1) * 4 bytes */
    const uint8_t*  items;         /* item_count * 6 bytes */
    const uint8_t*  directory;     /* brick_count * 8 bytes */
    const uint8_t*  payload;       /* variable */
    uint32_t        payload_len;
} TxciIndex;

/*
 * Load a TXCI v3 file from disk into memory.
 * Returns 1 on success, 0 on error (check tx_set_error).
 */
int txci_load(TxciIndex* idx, const char* path);

/*
 * Load a TXCI v3 from memory buffer (gzip or raw).
 * Returns 1 on success, 0 on error (check tx_set_error).
 */
int txci_load_from_memory(TxciIndex* idx, const uint8_t* data, uint32_t len);

/*
 * Lookup an RGB color and return the group_id.
 * Returns >= 0 on success, -1 on error.
 */
int txci_lookup_group(const TxciIndex* idx, uint8_t r, uint8_t g, uint8_t b);

/*
 * Get all item candidates for a given group_id.
 * Returns the number of items written to out (up to max_out).
 */
int txci_get_items(const TxciIndex* idx, uint32_t group_id,
                   TxciItem* out, int max_out);

/* Read one candidate by absolute item index after header validation. */
int txci_get_item(const TxciIndex* idx, uint32_t item_index, TxciItem* out);

/*
 * Choose the best tile/wall match for an RGB color.
 * If prefer_wall is set, wall items are preferred over tile items.
 * Returns 1 on success (out filled), 0 if no match.
 */
int txci_choose_tile(const TxciIndex* idx, uint8_t r, uint8_t g, uint8_t b,
                     int prefer_wall, TxciItem* out);

/*
 * Unload a TXCI index (frees the data buffer).
 */
void txci_unload(TxciIndex* idx);

#endif /* TERRA_TXCI_H */
