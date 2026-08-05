/*
 * terra_txci.c -- TXCI v3 color index loader and O(1) lookup.
 *
 * Loads a .txci binary file (TerraX Color Index v3) and provides
 * brick-based color lookups. Each RGB color maps to a group_id,
 * which indexes into an array of tile/wall/paint candidates.
 */
#include "terra_txci.h"
#include <limits.h>
#include <stdio.h>

/* External dependencies from terra_mem.c */
extern void* memset(void* dst, int value, unsigned long n);
extern void* memcpy(void* dst, const void* src, unsigned long n);
extern uint8_t* tx_alloc(uint32_t size);
extern void tx_internal_free(void* ptr);
extern void tx_set_error(const char* code, const char* message);
extern uint32_t tx_strlen(const char* s);

/* ---------- File I/O helpers ---------- */

static uint32_t read_u32le(const uint8_t* p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint16_t read_u16le(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int txci_parse_header(TxciIndex* idx, uint8_t* buf, uint32_t buf_len);

/* ---------- TXCI loader (with gzip support) ---------- */

/* zlib inflate for gzip decompression (available via -sUSE_ZLIB=1) */
typedef struct z_stream_s {
    uint8_t* next_in;
    uint32_t avail_in;
    uint32_t total_in;
    uint8_t* next_out;
    uint32_t avail_out;
    uint32_t total_out;
    void* msg;
    void* state;
    void* zalloc;
    void* zfree;
    void* opaque;
    int32_t data_type;
    uint32_t adler;
    uint32_t reserved;
} z_stream;

extern int inflateInit2_(z_stream* strm, int windowBits, const char* version, int stream_size);
extern int inflate(z_stream* strm, int flush);
extern int inflateEnd(z_stream* strm);
#define inflateInit2(strm, windowBits) \
    inflateInit2_(strm, windowBits, "1.3.1.1-motley", (int)sizeof(z_stream))
#define Z_OK 0
#define Z_STREAM_END 1
#define Z_FINISH 4

static uint32_t read_u32le_at(const uint8_t* p, uint32_t off) {
    return (uint32_t)p[off]
         | ((uint32_t)p[off+1] << 8)
         | ((uint32_t)p[off+2] << 16)
         | ((uint32_t)p[off+3] << 24);
}

int txci_load(TxciIndex* idx, const char* path) {
    #define TXCI_SEEK_SET 0
    #define TXCI_SEEK_END 2

    memset(idx, 0, sizeof(TxciIndex));

    FILE* fp = fopen(path, "rb");
    if (!fp) {
        tx_set_error("TERRAX_IO_ERROR", "failed to open TXCI file");
        return 0;
    }

    /* Get file size */
    fseek(fp, 0, TXCI_SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, TXCI_SEEK_SET);
    if (file_size < TXCI_HEADER_SIZE || (unsigned long)file_size > UINT32_MAX) {
        fclose(fp);
        tx_set_error("TERRAX_PARSE_ERROR", "TXCI file too small");
        return 0;
    }

    /* Read entire file */
    uint32_t sz = (uint32_t)file_size;
    uint8_t* compressed = tx_alloc(sz);
    if (!compressed) {
        fclose(fp);
        tx_set_error("TERRAX_WASM_OOM", "TXCI allocation failed");
        return 0;
    }
    size_t nread = fread(compressed, 1, sz, fp);
    fclose(fp);
    if (nread != sz) {
        tx_internal_free(compressed);
        tx_set_error("TERRAX_IO_ERROR", "TXCI read incomplete");
        return 0;
    }

    /* Detect gzip (magic bytes 0x1f 0x8b) and decompress */
    uint8_t* buf;
    uint32_t buf_len;
    if (sz > 18 && compressed[0] == 0x1f && compressed[1] == 0x8b) {
        /* Uncompressed size is stored in the last 4 bytes (ISIZE) */
        uint32_t uncompressed_size = read_u32le_at(compressed, sz - 4);
        if (uncompressed_size < TXCI_HEADER_SIZE || uncompressed_size > 128u * 1024u * 1024u) {
            tx_internal_free(compressed);
            tx_set_error("TERRAX_PARSE_ERROR", "gzip ISIZE invalid");
            return 0;
        }
        uint8_t* decomp = tx_alloc(uncompressed_size);
        if (!decomp) {
            tx_internal_free(compressed);
            tx_set_error("TERRAX_WASM_OOM", "TXCI decompression buffer allocation failed");
            return 0;
        }

        /* Use inflate with windowBits=15+16=31 for gzip format */
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        zs.next_in = compressed;
        zs.avail_in = sz;
        zs.next_out = decomp;
        zs.avail_out = uncompressed_size;

        int zret = inflateInit2(&zs, 31);
        if (zret != Z_OK) {
            tx_internal_free(decomp);
            tx_internal_free(compressed);
            tx_set_error("TERRAX_PARSE_ERROR", "gzip inflateInit failed");
            return 0;
        }
        zret = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        if (zret != Z_STREAM_END) {
            tx_internal_free(decomp);
            tx_internal_free(compressed);
            tx_set_error("TERRAX_PARSE_ERROR", "gzip inflate failed");
            return 0;
        }
        buf = decomp;
        buf_len = (uint32_t)zs.total_out;
        tx_internal_free(compressed);
    } else {
        buf = compressed;
        buf_len = sz;
    }

    if (!txci_parse_header(idx, buf, buf_len)) {
        tx_internal_free(buf);
        memset(idx, 0, sizeof(TxciIndex));
        return 0;
    }
    return 1;
}

/* ---------- Brick-based color lookup ---------- */

int txci_lookup_group(const TxciIndex* idx, uint8_t r, uint8_t g, uint8_t b) {
    if (!idx || !idx->data) return -1;

    uint16_t shift = idx->brick_shift;
    uint16_t mask = idx->brick_mask;
    uint16_t bs = idx->brick_size;
    uint16_t grid = idx->grid;

    /* Compute brick index */
    uint32_t br = r >> shift;
    uint32_t bg = g >> shift;
    uint32_t bb = b >> shift;
    uint32_t brick_id = (br * grid + bg) * grid + bb;

    /* Compute local index within brick */
    uint32_t lr = r & mask;
    uint32_t lg = g & mask;
    uint32_t lb = b & mask;
    uint32_t local = (lr * bs + lg) * bs + lb;

    /* Read directory entry */
    if (brick_id >= idx->brick_count) return -1;
    const uint8_t* dir = idx->directory + brick_id * TXCI_DIR_SIZE;
    uint8_t block_type = dir[0];
    uint32_t payload_rel = read_u32le(dir + 4);

    if (payload_rel >= idx->payload_len) return -1;
    const uint8_t* p = idx->payload + payload_rel;
    uint32_t remaining = idx->payload_len - payload_rel;
    uint32_t local_count = (uint32_t)bs * bs * bs;

    switch (block_type) {
    case TXCI_BLOCK_UNIFORM:
        if (remaining < 2u) return -1;
        return (int)read_u16le(p);

    case TXCI_BLOCK_PAL4: {
        if (remaining < 1u) return -1;
        uint8_t k = p[0];
        uint64_t needed = 1u + (uint64_t)k * 2u + (local_count + 1u) / 2u;
        if (k == 0u || k > 16u || needed > remaining) return -1;
        const uint8_t* pal = p + 1;
        const uint8_t* indices = p + 1 + k * 2;
        uint8_t byte_val = indices[local >> 1];
        uint8_t pal_idx = (local & 1) ? (byte_val >> 4) : (byte_val & 0x0F);
        if (pal_idx >= k) return -1;
        return (int)read_u16le(pal + pal_idx * 2);
    }

    case TXCI_BLOCK_PAL8: {
        if (remaining < 2u) return -1;
        uint16_t k = read_u16le(p);
        uint64_t needed = 2u + (uint64_t)k * 2u + local_count;
        if (k == 0u || k > 256u || needed > remaining) return -1;
        const uint8_t* pal = p + 2;
        const uint8_t* indices = p + 2 + k * 2;
        uint8_t pal_idx = indices[local];
        if (pal_idx >= k) return -1;
        return (int)read_u16le(pal + pal_idx * 2);
    }

    case TXCI_BLOCK_RAW16:
        if ((uint64_t)local_count * 2u > remaining) return -1;
        return (int)read_u16le(p + local * 2);

    default:
        return -1;
    }
}

/* ---------- Item enumeration ---------- */

static void txci_read_item(const TxciIndex* idx, uint32_t index, TxciItem* out) {
    const uint8_t* item = idx->items + index * TXCI_ITEM_SIZE;
    uint16_t kind_and_id = read_u16le(item);
    out->is_wall = (kind_and_id & TXCI_KIND_WALL) ? 1 : 0;
    out->type_id = kind_and_id & TXCI_KIND_ID_MASK;
    out->variant = read_u16le(item + 2);
    out->paint_id = item[4];
}

int txci_get_items(const TxciIndex* idx, uint32_t group_id,
                   TxciItem* out, int max_out) {
    if (!idx || !idx->data || !out || max_out <= 0) return 0;
    if (group_id >= idx->color_count) return 0;

    uint32_t start = idx->group_offsets[group_id];
    uint32_t end = idx->group_offsets[group_id + 1];
    int count = 0;

    for (uint32_t i = start; i < end && count < max_out; i++) {
        txci_read_item(idx, i, &out[count++]);
    }
    return count;
}

/* ---------- Best-match selection ---------- */

int txci_choose_tile(const TxciIndex* idx, uint8_t r, uint8_t g, uint8_t b,
                     int prefer_wall, TxciItem* out) {
    if (!idx || !idx->data || !out) return 0;

    int group_id = txci_lookup_group(idx, r, g, b);
    if (group_id < 0) return 0;

    uint32_t start = idx->group_offsets[(uint32_t)group_id];
    uint32_t end = idx->group_offsets[(uint32_t)group_id + 1u];
    TxciItem first;
    int has_first = 0;

    /* Scan the complete bounded group. A preferred candidate may appear after
     * the first 32 entries, so selection must not depend on a fixed stack copy. */
    for (uint32_t index = start; index < end; index++) {
        TxciItem candidate;
        txci_read_item(idx, index, &candidate);
        if (!has_first) {
            first = candidate;
            has_first = 1;
        }
        if ((prefer_wall && candidate.is_wall) ||
            (!prefer_wall && !candidate.is_wall)) {
            *out = candidate;
            return 1;
        }
    }

    if (!has_first) return 0;
    *out = first;
    return 1;
}

/* ---------- TXCI load from memory (gzip or raw) ---------- */

static int txci_parse_header(TxciIndex* idx, uint8_t* buf, uint32_t buf_len) {
    if (!idx || !buf || buf_len < TXCI_HEADER_SIZE) {
        tx_set_error("TERRAX_PARSE_ERROR", "TXCI header is truncated");
        return 0;
    }
    idx->data = buf;
    idx->data_len = buf_len;

    const uint8_t* h = buf;
    uint32_t magic = read_u32le(h + 0);
    uint16_t version = read_u16le(h + 4);
    uint16_t brick_size = read_u16le(h + 6);

    if (magic != TXCI_MAGIC) {
        tx_set_error("TERRAX_PARSE_ERROR", "TXCI magic mismatch");
        return 0;
    }
    if (version != TXCI_VERSION) {
        tx_set_error("TERRAX_PARSE_ERROR", "TXCI version mismatch");
        return 0;
    }
    if (brick_size != 4 && brick_size != 8 && brick_size != 16 && brick_size != 32) {
        tx_set_error("TERRAX_PARSE_ERROR", "TXCI invalid brick_size");
        return 0;
    }

    idx->brick_size = brick_size;
    idx->color_count = read_u32le(h + 8);
    idx->item_count = read_u32le(h + 12);
    idx->brick_count = read_u32le(h + 16);

    /* Compute derived values */
    uint16_t shift = 0;
    uint16_t bs = brick_size;
    while (bs > 1) { bs >>= 1; shift++; }
    idx->brick_shift = shift;
    idx->brick_mask = brick_size - 1;
    idx->grid = 256 / brick_size;
    if (idx->brick_count != (uint32_t)idx->grid * idx->grid * idx->grid) {
        tx_set_error("TERRAX_PARSE_ERROR", "TXCI brick count does not match brick size");
        return 0;
    }

    /* Map section pointers */
    uint32_t colors_off = read_u32le(h + 20);
    uint32_t groups_off = read_u32le(h + 24);
    uint32_t items_off  = read_u32le(h + 28);
    uint32_t dir_off    = read_u32le(h + 32);
    uint32_t payload_off = read_u32le(h + 36);

    /* Validate ordered sections and all fixed-size tables before exposing pointers. */
    uint64_t colors_end = (uint64_t)colors_off + (uint64_t)idx->color_count * 3u;
    uint64_t groups_end = (uint64_t)groups_off + ((uint64_t)idx->color_count + 1u) * 4u;
    uint64_t items_end = (uint64_t)items_off + (uint64_t)idx->item_count * TXCI_ITEM_SIZE;
    uint64_t directory_end = (uint64_t)dir_off + (uint64_t)idx->brick_count * TXCI_DIR_SIZE;
    if (colors_off < TXCI_HEADER_SIZE || (groups_off & 3u) != 0u ||
        colors_off > groups_off || groups_off > items_off ||
        items_off > dir_off || dir_off > payload_off || payload_off > buf_len ||
        colors_end > groups_off || groups_end > items_off || items_end > dir_off ||
        directory_end > payload_off) {
        tx_set_error("TERRAX_PARSE_ERROR", "TXCI section offset out of range");
        return 0;
    }

    idx->colors = buf + colors_off;
    idx->group_offsets = (const uint32_t*)(buf + groups_off);
    idx->items = buf + items_off;
    idx->directory = buf + dir_off;
    idx->payload = buf + payload_off;
    idx->payload_len = buf_len - payload_off;

    uint32_t prior = 0u;
    for (uint32_t i = 0; i <= idx->color_count; i++) {
        uint32_t offset = idx->group_offsets[i];
        if (offset < prior || offset > idx->item_count) {
            tx_set_error("TERRAX_PARSE_ERROR", "TXCI group offsets are invalid");
            return 0;
        }
        if (offset - prior > TXCI_MAX_GROUP_OPTIONS) {
            tx_set_error("TERRAX_PARSE_ERROR", "TXCI group contains too many options");
            return 0;
        }
        prior = offset;
    }

    return 1;
}

int txci_load_from_memory(TxciIndex* idx, const uint8_t* data, uint32_t len) {
    memset(idx, 0, sizeof(TxciIndex));

    if (!data || len < TXCI_HEADER_SIZE) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "TXCI data too small or null");
        return 0;
    }

    /* Detect gzip (magic bytes 0x1f 0x8b) and decompress */
    uint8_t* buf;
    uint32_t buf_len;
    if (len > 18 && data[0] == 0x1f && data[1] == 0x8b) {
        /* Uncompressed size is stored in the last 4 bytes (ISIZE) */
        uint32_t uncompressed_size = read_u32le_at(data, len - 4);
        if (uncompressed_size < TXCI_HEADER_SIZE || uncompressed_size > 128u * 1024u * 1024u) {
            tx_set_error("TERRAX_PARSE_ERROR", "gzip ISIZE invalid");
            return 0;
        }
        uint8_t* decomp = tx_alloc(uncompressed_size);
        if (!decomp) {
            tx_set_error("TERRAX_WASM_OOM", "TXCI decompression buffer allocation failed");
            return 0;
        }

        /* Use inflate with windowBits=15+16=31 for gzip format */
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        zs.next_in = (uint8_t*)data;
        zs.avail_in = len;
        zs.next_out = decomp;
        zs.avail_out = uncompressed_size;

        int zret = inflateInit2(&zs, 31);
        if (zret != Z_OK) {
            tx_internal_free(decomp);
            tx_set_error("TERRAX_PARSE_ERROR", "gzip inflateInit failed");
            return 0;
        }
        zret = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        if (zret != Z_STREAM_END) {
            tx_internal_free(decomp);
            tx_set_error("TERRAX_PARSE_ERROR", "gzip inflate failed");
            return 0;
        }
        buf = decomp;
        buf_len = (uint32_t)zs.total_out;
    } else {
        /* Raw TXCI data, copy to bump allocator */
        buf = tx_alloc(len);
        if (!buf) {
            tx_set_error("TERRAX_WASM_OOM", "TXCI buffer allocation failed");
            return 0;
        }
        memcpy(buf, data, len);
        buf_len = len;
    }

    if (!txci_parse_header(idx, buf, buf_len)) {
        tx_internal_free(buf);
        memset(idx, 0, sizeof(TxciIndex));
        return 0;
    }
    return 1;
}

/* ---------- Cleanup ---------- */

void txci_unload(TxciIndex* idx) {
    if (!idx) return;
    if (idx->data) tx_internal_free((void*)idx->data);
    memset(idx, 0, sizeof(TxciIndex));
}

/* ---------- Gzip decompression utility ---------- */

/*
 * tx_gunzip -- Decompress gzip data using WASM's built-in zlib.
 *
 * @param compressed_ptr   Pointer to gzip-compressed data in WASM memory
 * @param compressed_len   Length of compressed data
 * @param out_ptr          Output buffer pointer (caller allocated)
 * @param out_max_len      Output buffer capacity
 * @return                 Actual decompressed size, or -1 on error
 *
 * The uncompressed size is read from the gzip trailer (last 4 bytes, ISIZE).
 * If out_ptr is 0, returns the required buffer size (from ISIZE).
 */
int32_t tx_gunzip(uint32_t compressed_ptr, uint32_t compressed_len,
                  uint32_t out_ptr, uint32_t out_max_len) {
    if (compressed_ptr == 0 || compressed_len < 18) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "invalid gzip input");
        return -1;
    }

    const uint8_t* src = (const uint8_t*)(uintptr_t)compressed_ptr;

    /* Verify gzip magic bytes */
    if (src[0] != 0x1f || src[1] != 0x8b) {
        tx_set_error("TERRAX_PARSE_ERROR", "not gzip data");
        return -1;
    }

    /* Read uncompressed size from gzip trailer (last 4 bytes, ISIZE) */
    uint32_t uncompressed_size = read_u32le_at(src, compressed_len - 4);

    /* Probe mode: just return the size */
    if (out_ptr == 0) {
        return (int32_t)uncompressed_size;
    }

    if (out_max_len < uncompressed_size) {
        tx_set_error("TERRAX_BUFFER_TOO_SMALL", "output buffer too small for gunzip");
        return -1;
    }

    /* Decompress using zlib inflate with gzip window bits (15+16=31) */
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    zs.next_in = (uint8_t*)(uintptr_t)compressed_ptr;
    zs.avail_in = compressed_len;
    zs.next_out = (uint8_t*)(uintptr_t)out_ptr;
    zs.avail_out = out_max_len;

    int zret = inflateInit2(&zs, 31);
    if (zret != Z_OK) {
        tx_set_error("TERRAX_PARSE_ERROR", "gzip inflateInit failed");
        return -1;
    }

    zret = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);

    if (zret != Z_STREAM_END) {
        tx_set_error("TERRAX_PARSE_ERROR", "gzip inflate failed");
        return -1;
    }

    return (int32_t)zs.total_out;
}