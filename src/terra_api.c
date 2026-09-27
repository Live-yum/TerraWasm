#include "terra_output.h"
/*
 * terra_api.c -- V2 API implementation.
 *
 * Handles the two-call pattern, error propagation, and dispatches
 * to internal functions for section serialization and operations.
 */
#include "terra_types.h"
#include "terra_map.h"
#include "terra_world.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

extern void* memset(void* dst, int value, size_t n);
extern void* memcpy(void* dst, const void* src, size_t n);

#ifdef TERRAX_TESTING
static uint32_t g_tx_test_save_write_limit = UINT32_MAX;

void terrax_test_fail_save_after_bytes(uint32_t bytes) {
    g_tx_test_save_write_limit = bytes;
}

void terrax_test_reset_fail_save(void) {
    g_tx_test_save_write_limit = UINT32_MAX;
}
#endif

/* ---------- External declarations from terra_mem.c ---------- */
extern uint8_t* tx_alloc(uint32_t size);
extern void tx_internal_free(void* ptr);
extern void tx_clear_error(void);
extern void tx_set_error(const char* code, const char* message);
extern int set_result_buf(TxBuf* b);
extern void buf_init(TxBuf* b, uint32_t cap);
extern void buf_u8(TxBuf* b, uint8_t v);
extern void buf_cstr(TxBuf* b, const char* s);
extern void buf_bytes(TxBuf* b, const void* p, uint32_t n);
extern void buf_u16le(TxBuf* b, uint32_t v);
extern void buf_u32le(TxBuf* b, uint32_t v);
extern void buf_u64le(TxBuf* b, uint64_t v);
extern void json_string(TxBuf* b, const char* s);
extern void json_u32(TxBuf* b, uint32_t v);
extern void json_i32(TxBuf* b, int32_t v);
extern void json_u64(TxBuf* b, uint64_t v);
extern uint32_t tx_strlen(const char* s);
extern int tx_streq_c(const char* a, const char* b);

extern uint32_t tx_last_ptr;
extern uint32_t tx_last_len;
extern uint32_t tx_last_width;
extern uint32_t tx_last_height;
extern int32_t tx_last_status;
extern char tx_last_error[256];

extern void tx_set_world_open_count(uint32_t count);
extern uint32_t tx_get_world_open_count(void);
extern uint32_t tx_mark(void);
extern void tx_rewind(uint32_t mark);
extern void tx_reset_heap(void);

/* ---------- External from terra_wld.c ---------- */
extern int parse_format(TxWorld* w);
extern int parse_header(TxWorld* w);
extern int section_index_by_name(const char* name, uint32_t len);
extern int serialize_section_json(TxWorld* w, int section_idx, TxBuf* out);

/* ---------- External from terra_ops.c ---------- */
extern int op_execute_json(TxWorld* w, const char* op_name, const char* request, TxBuf* response);

/* ---------- External from terra_update.c ---------- */
extern int rebuild_tile_section(TxWorld* w, TxBuf* out,
                                TxTileRule* rules, uint32_t rule_count);
extern int rebuild_tile_section_pixel_art(TxWorld* w, TxBuf* out);
extern void txw_clear_pixel_art_state(TxWorld* w);

/* ---------- World slot management ---------- */

static TxWorld g_worlds[TX_MAX_WORLDS];
extern void tx_stream_release_world(TxWorld* world);
static uint32_t g_next_generation = 1;

#define TX_HANDLE_SLOT_BITS 8u
#define TX_HANDLE_SLOT_MASK ((1u << TX_HANDLE_SLOT_BITS) - 1u)
#define TX_HANDLE_GENERATION_MASK 0x00ffffffu

static uint32_t tx_next_handle(uint32_t slot_index) {
    uint32_t generation = g_next_generation++ & TX_HANDLE_GENERATION_MASK;
    if (generation == 0u) {
        generation = 1u;
        g_next_generation = 2u;
    }
    return (generation << TX_HANDLE_SLOT_BITS) | (slot_index + 1u);
}

TxWorld* tx_get_world(uint32_t handle) {
    uint32_t encoded_slot = handle & TX_HANDLE_SLOT_MASK;
    if (!handle || encoded_slot == 0u || encoded_slot > TX_MAX_WORLDS) return NULL;
    TxWorld* world = &g_worlds[encoded_slot - 1u];
    return world->active && world->handle == handle ? world : NULL;
}

static TxWorld* tx_claim_world_slot(void) {
    if (tx_get_world_open_count() != 0u) {
        tx_set_error("TERRAX_STATE_ERROR", "only one world may be open at a time");
        return NULL;
    }
    return &g_worlds[0];
}

static terrax_world_status tx_invalid_handle(void) {
    tx_set_error("TERRAX_INVALID_HANDLE", "world handle is stale or invalid");
    return TERRAX_WORLD_STATUS_STATE_ERROR;
}

/* ---------- Heap mark management ---------- */

static uint32_t tx_active_heap_mark(void) {
    uint32_t mark = 0;
    for (uint32_t i = 0; i < TX_MAX_WORLDS; i++) {
        if (g_worlds[i].active) {
            uint32_t hm = g_worlds[i].heap_mark;
            if (hm > mark) mark = hm;
        }
    }
    return mark;
}

/* ---------- Global transient reclamation ---------- */

void tx_reclaim_transients(void) {
    /* Require at least one open world to have a meaningful heap_mark
       to avoid destroying Emscripten runtime allocations. */
    if (tx_get_world_open_count() == 0u) return;
    uint32_t mark = tx_active_heap_mark();
    for (uint32_t i = 0; i < TX_MAX_WORLDS; i++) {
        TxWorld* world = &g_worlds[i];
        if (!world->active) continue;
        world->op_response_ptr = 0;
        world->op_response_len = 0;
        world->op_request_key = NULL;
        world->op_request_key_len = 0;
        world->section_response = NULL;
        world->section_response_len = 0;
        world->section_response_index = -1;
        world->media_result = NULL;
        world->media_result_len = 0;
        world->media_result_width = 0;
        world->media_result_height = 0;
        world->media_result_kind = 0;
        world->last_op_heap_end = world->heap_mark;
    }
    uint32_t current = tx_mark();
    if (current > mark) {
        tx_rewind(mark);
    }
    tx_last_ptr = tx_last_len = tx_last_width = tx_last_height = 0;
}

/* ---------- Helper: copy string to caller buffer (two-call pattern) ---------- */

static terrax_world_status write_string_to_caller(
    const char* src,
    char* buffer,
    uint64_t buffer_size,
    uint64_t* required_size) {
    uint32_t len = tx_strlen(src);
    uint64_t needed = (uint64_t)len + 1u;
    if (required_size) *required_size = needed;
    /* Probe call: buffer is NULL or size is 0 -> return OK with required_size */
    if (!buffer || buffer_size == 0) {
        return TERRAX_WORLD_STATUS_OK;
    }
    if (buffer_size < needed) {
        return TERRAX_WORLD_STATUS_BUFFER_TOO_SMALL;
    }
    for (uint32_t i = 0; i <= len; i++) buffer[i] = src[i];
    return TERRAX_WORLD_STATUS_OK;
}

/* ---------- Helper: read file into heap ---------- */

static uint8_t* read_file_to_heap(const char* path, uint32_t* out_len) {
    #ifndef SEEK_SET
    #define SEEK_SET 0
    #endif
    #ifndef SEEK_END
    #define SEEK_END 2
    #endif

    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long file_size = ftell(f);
    if (file_size <= 0 || (uint64_t)file_size > UINT32_MAX ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    uint8_t* data = tx_alloc((uint32_t)file_size);
    if (!data) { fclose(f); return NULL; }
    size_t read = fread(data, 1, (size_t)file_size, f);
    fclose(f);
    if (read != (size_t)file_size) {
        tx_internal_free(data);
        return NULL;
    }
    *out_len = (uint32_t)file_size;
    return data;
}

/* ---------- Helper: write buffer to file ---------- */

static int tx_write_stream(FILE* f, const uint8_t* data, uint32_t len) {
    if (!f) return 0;
    size_t requested = (size_t)len;
#ifdef TERRAX_TESTING
    if (g_tx_test_save_write_limit < len) {
        requested = (size_t)g_tx_test_save_write_limit;
    }
#endif
    size_t written = fwrite(data, 1, requested, f);
    if (written != requested) return 0;
#ifdef TERRAX_TESTING
    if (requested != (size_t)len) return 0;
#endif
    return written == (size_t)len;
}

int write_file_from_heap(const char* path, const uint8_t* data, uint32_t len) {

    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    int ok = tx_write_stream(f, data, len);
    fclose(f);
    return ok;
}

static int tx_replace_file(const char* temp_path, const char* path) {
#ifdef _WIN32
    return MoveFileExA(
        temp_path,
        path,
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(temp_path, path) == 0;
#endif
}

static int tx_remove_if_exists_checked(const char* path) {
    if (remove(path) == 0) return 1;
    return errno == ENOENT;
}

static char* tx_build_temp_save_path(
    const char* path,
    unsigned long pid,
    uint32_t counter) {
    char suffix[64];
    int suffix_len = snprintf(
        suffix,
        sizeof(suffix),
        ".tmp.%lu.%lu",
        pid,
        (unsigned long)counter);
    if (suffix_len <= 0 || suffix_len >= (int)sizeof(suffix)) return NULL;

    uint32_t path_len = tx_strlen(path);
    if (path_len > UINT32_MAX - (uint32_t)suffix_len - 1u) return NULL;
    char* temp_path = (char*)tx_alloc(path_len + (uint32_t)suffix_len + 1u);
    if (!temp_path) return NULL;
    for (uint32_t i = 0; i < path_len; i++) temp_path[i] = path[i];
    for (int i = 0; i < suffix_len; i++) temp_path[path_len + (uint32_t)i] = suffix[i];
    temp_path[path_len + (uint32_t)suffix_len] = 0;
    return temp_path;
}

static int tx_open_unique_temp_save_file(
    const char* path,
    char** out_temp_path,
    FILE** out_file) {
    static uint32_t g_tx_save_temp_counter = 0u;
    unsigned long pid = 0u;
    if (!out_temp_path || !out_file) return 0;
    *out_temp_path = NULL;
    *out_file = NULL;
#ifdef _WIN32
    pid = (unsigned long)_getpid();
#else
    pid = (unsigned long)getpid();
#endif

    for (uint32_t attempt = 0; attempt < 1024u; attempt++) {
        char* temp_path = tx_build_temp_save_path(path, pid, ++g_tx_save_temp_counter);
        if (!temp_path) return 0;
#ifdef _WIN32
        int fd = _open(
            temp_path,
            _O_CREAT | _O_EXCL | _O_BINARY | _O_WRONLY,
            _S_IREAD | _S_IWRITE);
#else
        int fd = open(temp_path, O_CREAT | O_EXCL | O_WRONLY, 0600);
#endif
        if (fd >= 0) {
#ifdef _WIN32
            FILE* f = _fdopen(fd, "wb");
#else
            FILE* f = fdopen(fd, "wb");
#endif
            if (!f) {
#ifdef _WIN32
                _close(fd);
#else
                close(fd);
#endif
                if (!tx_remove_if_exists_checked(temp_path)) {
                    tx_internal_free(temp_path);
                    return 0;
                }
                tx_internal_free(temp_path);
                return 0;
            }
            *out_temp_path = temp_path;
            *out_file = f;
            return 1;
        }
        if (errno != EEXIST) {
            tx_internal_free(temp_path);
            return 0;
        }
        tx_internal_free(temp_path);
    }
    return 0;
}

static int tx_write_file_atomic_from_heap(
    const char* path,
    const uint8_t* data,
    uint32_t len) {
    char* temp_path = NULL;
    FILE* f = NULL;
    if (!tx_open_unique_temp_save_file(path, &temp_path, &f)) return 0;
    int ok = tx_write_stream(f, data, len);
    if (ok && fflush(f) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        if (!tx_remove_if_exists_checked(temp_path)) {
            tx_internal_free(temp_path);
            return 0;
        }
        tx_internal_free(temp_path);
        return 0;
    }

    ok = tx_replace_file(temp_path, path);
    if (!ok) {
        /* Replace is atomic only on success; cleanup still checks whether the
         * temp artifact can be removed on this host after the failed rename. */
        if (!tx_remove_if_exists_checked(temp_path)) {
            tx_internal_free(temp_path);
            return 0;
        }
    }
    tx_internal_free(temp_path);
    return ok;
}

static TxWorld* tx_begin_world_open(uint32_t* allocation_mark) {
    TxWorld* slot = tx_claim_world_slot();
    if (!slot) return NULL;
    *allocation_mark = tx_mark();
    memset(slot, 0, sizeof(TxWorld));
    slot->allocation_mark = *allocation_mark;
    return slot;
}

static void tx_abort_world_open(TxWorld* slot, uint32_t allocation_mark) {
    memset(slot, 0, sizeof(TxWorld));
    tx_rewind(allocation_mark);
}

/* Both path and buffer entry points transfer an owned native buffer here.
 * Keeping parse/activation in one function prevents their validation and
 * lifecycle behavior from drifting apart. */
static terrax_world_status tx_activate_owned_world(
    TxWorld* slot,
    uint8_t* file_data,
    uint32_t file_len,
    uint32_t allocation_mark,
    uint32_t* out_handle) {
    slot->file = file_data;
    slot->file_len = file_len;
    if (!parse_format(slot) || !parse_header(slot)) {
        tx_abort_world_open(slot, allocation_mark);
        return TERRAX_WORLD_STATUS_PARSE_ERROR;
    }

    slot->handle = tx_next_handle(0u);
    slot->active = 1;
    slot->heap_mark = tx_mark();
    tx_set_world_open_count(1u);
    tx_clear_error();
    *out_handle = slot->handle;
    return TERRAX_WORLD_STATUS_OK;
}

/* ====================================================================
 * V2 API: Lifecycle
 * ==================================================================== */

terrax_world_status terra_world_open(
    const char* path_utf8,
    uint32_t* out_handle) {
    if (out_handle) *out_handle = 0;
    if (!path_utf8 || !out_handle) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "null path or output pointer");
        return TERRAX_WORLD_STATUS_INVALID_ARGUMENT;
    }

    uint32_t allocation_mark = 0;
    TxWorld* slot = tx_begin_world_open(&allocation_mark);
    if (!slot) return TERRAX_WORLD_STATUS_STATE_ERROR;

    /* Read file */
    uint32_t file_len = 0;
    uint8_t* file_data = read_file_to_heap(path_utf8, &file_len);
    if (!file_data || file_len < 16) {
        tx_abort_world_open(slot, allocation_mark);
        tx_set_error("TERRAX_IO_ERROR", "failed to read world file");
        return TERRAX_WORLD_STATUS_IO_ERROR;
    }
    return tx_activate_owned_world(
        slot, file_data, file_len, allocation_mark, out_handle);
}

terrax_world_status terra_world_create(
    uint32_t* out_handle) {
    if (out_handle) *out_handle = 0;
    if (!out_handle) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "null output pointer");
        return TERRAX_WORLD_STATUS_INVALID_ARGUMENT;
    }

    uint32_t allocation_mark = 0;
    TxWorld* slot = tx_begin_world_open(&allocation_mark);
    if (!slot) return TERRAX_WORLD_STATUS_STATE_ERROR;
    slot->handle = tx_next_handle(0u);
    slot->active = 1;
    slot->heap_mark = tx_mark();
    tx_set_world_open_count(tx_get_world_open_count() + 1);
    tx_clear_error();
    *out_handle = slot->handle;
    return TERRAX_WORLD_STATUS_OK;
}

terrax_world_status terra_world_close(
    uint32_t handle) {
    TxWorld* world = tx_get_world(handle);
    if (!world) return tx_invalid_handle();

    uint32_t allocation_mark = world->allocation_mark;
    tx_output_clear(world);
    if (world->icon_atlas.rgba) tx_internal_free(world->icon_atlas.rgba);
    if (world->entity_marker_cache.data) tx_internal_free(world->entity_marker_cache.data);
    txw_clear_marker_color_index(world);
    tx_stream_release_world(world);
    memset(world, 0, sizeof(TxWorld));
    uint32_t count = tx_get_world_open_count();
    if (count > 0) tx_set_world_open_count(count - 1);
    tx_last_ptr = tx_last_len = tx_last_width = tx_last_height = 0;
    tx_rewind(allocation_mark);
    tx_clear_error();
    return TERRAX_WORLD_STATUS_OK;
}

/* Candidate roots belong to the persistent domain, so closing the old arena
 * cannot invalidate them. This final ownership swap does not allocate. */
int tx_stream_activate_world(TxWorld* candidate, uint32_t* out_handle) {
    if (g_worlds[0].active) terra_world_close(g_worlds[0].handle);
    g_worlds[0] = *candidate;
    g_worlds[0].handle = tx_next_handle(0u);
    g_worlds[0].active = 1u;
    g_worlds[0].allocation_mark = tx_mark();
    g_worlds[0].heap_mark = tx_mark();
    tx_set_world_open_count(1u);
    *out_handle = g_worlds[0].handle;
    return 1;
}

static terrax_world_status tx_prepare_world_for_save(TxWorld* world) {
    if (!world->file || world->file_len == 0) {
        tx_set_error("TERRAX_STATE_ERROR", "world has no file data");
        return TERRAX_WORLD_STATUS_STATE_ERROR;
    }
    if (world->legacy_wld) {
        if (world->format_dirty || world->section_overrides[0].active ||
            world->section_overrides[1].active || world->pixel_art_maps) {
            tx_set_error("TERRAX_NOT_SUPPORTED", "pre-88 worlds cannot be saved with modern binary edits");
            return TERRAX_WORLD_STATUS_NOT_SUPPORTED;
        }
        return TERRAX_WORLD_STATUS_OK;
    }

    /* If pixel art is queued but tile section not yet overridden, rebuild it now */
    if (world->pixel_art_maps && world->pixel_art_map_count > 0 &&
        (world->pixel_art_pixels || world->pixel_art_indexed)) {
        uint32_t tile_cap = world->section_overrides[1].active
            ? world->section_overrides[1].len
            : (world->ends[1] - world->starts[1]);
        if (tile_cap > UINT32_MAX - 65536u) {
            tx_set_error("TERRAX_WASM_OOM", "pixel-art tile buffer size overflow");
            return TERRAX_WORLD_STATUS_INTERNAL_ERROR;
        }
        TxBuf tile_buf;
        buf_init(&tile_buf, tile_cap + 65536u);
        if (!tile_buf.ok) {
            tx_set_error("TERRAX_WASM_OOM", "failed to allocate tile buffer for pixel art");
            return TERRAX_WORLD_STATUS_INTERNAL_ERROR;
        }
        if (!rebuild_tile_section_pixel_art(world, &tile_buf)) {
            if (tile_buf.data) tx_internal_free(tile_buf.data);
            return TERRAX_WORLD_STATUS_INTERNAL_ERROR;
        }
        extern int txw_filter_pixel_art_metadata(TxWorld* w);
        if (!txw_filter_pixel_art_metadata(world)) {
            tx_internal_free(tile_buf.data);
            return TERRAX_WORLD_STATUS_INTERNAL_ERROR;
        }
        extern int set_section_override_data(TxWorld* w, int idx, uint8_t* data, uint32_t len);
        if (!set_section_override_data(world, 1, tile_buf.data, tile_buf.len)) {
            if (tile_buf.data) tx_internal_free(tile_buf.data);
            return TERRAX_WORLD_STATUS_INTERNAL_ERROR;
        }
        txw_clear_pixel_art_state(world);
    }
    return TERRAX_WORLD_STATUS_OK;
}

static int tx_world_has_overrides(TxWorld* world) {
    /* Legacy section views are for reading; export retains the contiguous source. */
    if (world->legacy_wld) return 0;
    if (world->format_dirty) return 1;
    for (uint32_t i = 0; i < TX_MAX_SECTION_OVERRIDES; i++) {
        if (world->section_overrides[i].active) return 1;
    }
    return 0;
}

static int tx_world_output_size(TxWorld* world, uint32_t* out_size) {
    if (!out_size) return 0;
    if (!tx_world_has_overrides(world)) {
        *out_size = world->file_len;
        return 1;
    }
    uint32_t ptr_table_start = (world->version >= 135u) ? 24u : 4u;
    uint64_t total = (uint64_t)ptr_table_start + 2u +
                     (uint64_t)world->pointer_count * 4u + 2u + world->important_len;
    for (uint32_t i = 0; i < world->pointer_count; i++) {
        if (i < TX_MAX_SECTION_OVERRIDES && world->section_overrides[i].active) {
            total += world->section_overrides[i].len;
        } else {
            uint32_t start = world->starts[i];
            uint32_t end = world->ends[i];
            if (start < end && end <= world->file_len) total += end - start;
        }
    }
    if (total > UINT32_MAX) return 0;
    *out_size = (uint32_t)total;
    return 1;
}

static int tx_world_output_bytes(
    uint8_t* output,
    uint32_t capacity,
    uint32_t* offset,
    const uint8_t* source,
    uint32_t length) {
    if (!offset || (!source && length) || *offset > capacity ||
        length > capacity - *offset) return 0;
    if (output && length) memcpy(output + *offset, source, length);
    *offset += length;
    return 1;
}

static int tx_world_output_u16le(
    uint8_t* output,
    uint32_t capacity,
    uint32_t* offset,
    uint32_t value) {
    uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8u)};
    return tx_world_output_bytes(output, capacity, offset, bytes, 2u);
}

static int tx_world_output_u32le(
    uint8_t* output,
    uint32_t capacity,
    uint32_t* offset,
    uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t)value,
        (uint8_t)(value >> 8u),
        (uint8_t)(value >> 16u),
        (uint8_t)(value >> 24u),
    };
    return tx_world_output_bytes(output, capacity, offset, bytes, 4u);
}

static terrax_world_status tx_serialize_world_into(
    TxWorld* world,
    uint8_t* output,
    uint32_t output_size) {
    /* Rebuild directly into the caller-owned destination. The pointer table
     * must be recalculated because overridden sections may differ in size. */
    uint32_t offset = 0u;
    /* Calculate new section sizes for pointer recalculation */
    uint32_t new_sizes[TX_MAX_SECTIONS];
    for (uint32_t i = 0; i < world->pointer_count && i < TX_MAX_SECTIONS; i++) {
        if (i < TX_MAX_SECTION_OVERRIDES && world->section_overrides[i].active) {
            new_sizes[i] = world->section_overrides[i].len;
        } else {
            uint32_t start = world->starts[i];
            uint32_t end = world->ends[i];
            if (start < end && end <= world->file_len)
                new_sizes[i] = end - start;
            else
                new_sizes[i] = 0u;
        }
    }

    /* Format layout: ver(4) [+ magic(7)+type(1)+rev(4)+fav(8) if >=135]
     * then pointer_count(u16) + pointer_count*u32 + tile_type_count(u16) + bitmap */
    uint32_t ptr_table_start = (world->version >= 135u) ? 24u : 4u;
    uint32_t ptr_table_size  = 2u + world->pointer_count * 4u;
    uint32_t new_format_len  = ptr_table_start + ptr_table_size + 2u + world->important_len;

    /* 1. Format header (before pointer table). Rebuild this metadata instead
     * of copying it so a format-only patch is independent from section edits. */
    if (!tx_world_output_u32le(
        output, output_size, &offset, world->version)) goto write_failed;
    if (world->version >= 135u) {
        uint8_t file_type = world->file_type;
        uint8_t favorite[8];
        for (uint32_t i = 0; i < 8u; i++)
            favorite[i] = (uint8_t)(world->favorite >> (i * 8u));
        if (!tx_world_output_bytes(
            output, output_size, &offset, (const uint8_t*)world->magic, 7u) ||
            !tx_world_output_bytes(
                output, output_size, &offset, &file_type, 1u) ||
            !tx_world_output_u32le(
                output, output_size, &offset, world->revision) ||
            !tx_world_output_bytes(
                output, output_size, &offset, favorite, 8u)) goto write_failed;
    }

    /* 2. Recalculated pointer table */
    if (!tx_world_output_u16le(
        output, output_size, &offset, world->pointer_count)) goto write_failed;
    uint32_t pos = new_format_len;
    for (uint32_t i = 0; i < world->pointer_count; i++) {
        if (!tx_world_output_u32le(
            output, output_size, &offset, pos)) goto write_failed;
        pos += new_sizes[i];
    }

    /* 3. Tile type count + importance bitmap */
    if (!tx_world_output_u16le(
        output, output_size, &offset, world->tile_type_count) ||
        !tx_world_output_bytes(
            output, output_size, &offset, world->important, world->important_len)) goto write_failed;

    /* 4. Section data */
    for (uint32_t i = 0; i < world->pointer_count && i < TX_MAX_SECTIONS; i++) {
        if (i < TX_MAX_SECTION_OVERRIDES && world->section_overrides[i].active) {
            if (!tx_world_output_bytes(
                output,
                output_size,
                &offset,
                world->section_overrides[i].data,
                world->section_overrides[i].len)) goto write_failed;
        } else {
            uint32_t start = world->starts[i];
            uint32_t end = world->ends[i];
            if (start < end && end <= world->file_len && !tx_world_output_bytes(
                output,
                output_size,
                &offset,
                world->file + start,
                end - start)) goto write_failed;
        }
    }
    if (offset != output_size) goto write_failed;
    return TERRAX_WORLD_STATUS_OK;

write_failed:
    tx_set_error("TERRAX_WASM_OOM", "failed to build output file");
    return TERRAX_WORLD_STATUS_INTERNAL_ERROR;
}

static terrax_world_status tx_build_world_buffer(TxWorld* world, TxBuf* out) {
    uint32_t output_size = 0;
    if (!tx_world_output_size(world, &output_size)) {
        tx_set_error("TERRAX_WASM_OOM", "output file size exceeds WASM limits");
        return TERRAX_WORLD_STATUS_INTERNAL_ERROR;
    }
    memset(out, 0, sizeof(TxBuf));
    terrax_world_status status = tx_serialize_world_into(world, NULL, output_size);
    if (status != TERRAX_WORLD_STATUS_OK) return status;
    out->data = tx_alloc(output_size);
    if (!out->data) {
        tx_set_error("TERRAX_WASM_OOM", "failed to allocate output file");
        return TERRAX_WORLD_STATUS_INTERNAL_ERROR;
    }
    out->cap = output_size;
    out->ok = 1;
    status = tx_serialize_world_into(world, out->data, output_size);
    if (status != TERRAX_WORLD_STATUS_OK) {
        tx_internal_free(out->data);
        memset(out, 0, sizeof(TxBuf));
        return status;
    }
    out->len = output_size;
    return TERRAX_WORLD_STATUS_OK;
}

terrax_world_status terra_world_save(
    uint32_t handle,
    const char* path_utf8) {
    TxWorld* world = tx_get_world(handle);
    if (!world) return tx_invalid_handle();
    if (!path_utf8) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "null path");
        return TERRAX_WORLD_STATUS_INVALID_ARGUMENT;
    }
    terrax_world_status status = tx_prepare_world_for_save(world);
    if (status != TERRAX_WORLD_STATUS_OK) return status;

    const uint8_t* data = world->file;
    uint32_t len = world->file_len;
    TxBuf out = {0};
    if (tx_world_has_overrides(world)) {
        status = tx_build_world_buffer(world, &out);
        if (status != TERRAX_WORLD_STATUS_OK) return status;
        data = out.data;
        len = out.len;
    }

    if (!tx_write_file_atomic_from_heap(path_utf8, data, len)) {
        if (out.data) tx_internal_free(out.data);
        tx_set_error("TERRAX_IO_ERROR", "failed to write file");
        return TERRAX_WORLD_STATUS_IO_ERROR;
    }
    if (out.data) tx_internal_free(out.data);

    tx_clear_error();
    return TERRAX_WORLD_STATUS_OK;
}

terrax_world_status terra_world_save_to_buffer(
    uint32_t handle,
    uint8_t* output,
    uint32_t capacity,
    uint32_t* out_required) {
    TxWorld* world = tx_get_world(handle);
    if (!world) return tx_invalid_handle();
    if (!out_required) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "null required-size pointer");
        return TERRAX_WORLD_STATUS_INVALID_ARGUMENT;
    }
    *out_required = 0;

    terrax_world_status status = tx_prepare_world_for_save(world);
    if (status != TERRAX_WORLD_STATUS_OK) return status;
    uint32_t required = 0;
    if (!tx_world_output_size(world, &required)) {
        tx_set_error("TERRAX_WASM_OOM", "output file size exceeds WASM limits");
        return TERRAX_WORLD_STATUS_INTERNAL_ERROR;
    }
    *out_required = required;
    if (!output && capacity == 0u) {
        tx_clear_error();
        return TERRAX_WORLD_STATUS_OK;
    }
    if (!output || capacity < required) {
        tx_set_error("TERRAX_BUFFER_TOO_SMALL", "world output buffer is too small");
        return TERRAX_WORLD_STATUS_BUFFER_TOO_SMALL;
    }

    if (!tx_world_has_overrides(world)) {
        memcpy(output, world->file, required);
    } else {
        /* Count and validate through the same serializer before touching the
         * caller buffer, preserving the prior no-partial-output failure contract. */
        status = tx_serialize_world_into(world, NULL, required);
        if (status != TERRAX_WORLD_STATUS_OK) return status;
        status = tx_serialize_world_into(world, output, required);
        if (status != TERRAX_WORLD_STATUS_OK) return status;
    }
    tx_clear_error();
    return TERRAX_WORLD_STATUS_OK;
}


terrax_world_status terra_world_open_from_buffer(
    const uint8_t* buffer,
    uint32_t buffer_len,
    uint32_t* out_handle) {
    if (out_handle) *out_handle = 0;
    if (!buffer || !out_handle || buffer_len < 16) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "null buffer or output pointer");
        return TERRAX_WORLD_STATUS_INVALID_ARGUMENT;
    }

    TxWorld* slot = tx_claim_world_slot();
    if (!slot) {
        return TERRAX_WORLD_STATUS_STATE_ERROR;
    }
    uint32_t allocation_mark = tx_mark();
    memset(slot, 0, sizeof(TxWorld));
    slot->allocation_mark = allocation_mark;

    /* Copy file data to bump allocator */
    uint8_t* file_data = tx_alloc(buffer_len);
    if (!file_data) {
        tx_abort_world_open(slot, allocation_mark);
        tx_set_error("TERRAX_WASM_OOM", "failed to allocate file buffer");
        return TERRAX_WORLD_STATUS_INTERNAL_ERROR;
    }
    memcpy(file_data, buffer, buffer_len);
    return tx_activate_owned_world(
        slot, file_data, buffer_len, allocation_mark, out_handle);
}

/* ====================================================================
 * V2 API: Info
 * ==================================================================== */

terrax_world_status terra_info_list_sections_json(
    char* buffer,
    uint64_t buffer_size,
    uint64_t* required_size) {
    static const char* sections_json =
        "{\"sections\":[\"format\",\"header\",\"chests\",\"signs\",\"npcs\","
        "\"tile_entities\",\"weighted_pressure_plates\",\"town_manager\","
        "\"bestiary\",\"creative_powers\",\"footer\"]}";
    return write_string_to_caller(sections_json, buffer, buffer_size, required_size);
}

terrax_world_status terra_info_get_section_schema_json(
    const char* section_name,
    char* buffer,
    uint64_t buffer_size,
    uint64_t* required_size) {
    if (!section_name) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "null section name");
        return TERRAX_WORLD_STATUS_INVALID_ARGUMENT;
    }

    /* Build schema JSON */
    TxBuf b;
    buf_init(&b, 512);
    buf_cstr(&b, "{\"name\":");
    json_string(&b, section_name);
    buf_cstr(&b, ",\"readable\":true,");

    if (tx_streq_c(section_name, "tiles")) {
        buf_cstr(&b, "\"writable\":false,\"note\":\"use batch_update_tiles for tile modifications\"");
    } else if (tx_streq_c(section_name, "header")) {
        buf_cstr(&b, "\"writable\":false,\"note\":\"use header_patch for unified header metadata updates\"");
    } else if (tx_streq_c(section_name, "chests")) {
        buf_cstr(&b, "\"writable\":false,\"note\":\"use replace_chests for verified WLD binary encoding\"");
    } else if (tx_streq_c(section_name, "bestiary")) {
        buf_cstr(&b, "\"writable\":false,\"note\":\"use replace_bestiary for verified WLD binary encoding\"");
    } else {
        buf_cstr(&b, "\"writable\":false,\"note\":\"binary encoder not available\"");
    }
    buf_u8(&b, '}');
    buf_u8(&b, 0);

    if (!b.ok) {
        if (b.data) tx_internal_free(b.data);
        tx_set_error("TERRAX_WASM_OOM", "schema serialization failed");
        return TERRAX_WORLD_STATUS_INTERNAL_ERROR;
    }

    terrax_world_status st = write_string_to_caller(
        (const char*)b.data, buffer, buffer_size, required_size);
    tx_internal_free(b.data);
    return st;
}

/* ====================================================================
 * V2 API: Section Read/Write
 * ==================================================================== */

static void tx_clear_section_response(TxWorld* world, int release_root) {
    if (release_root && world->section_response)
        tx_internal_free(world->section_response);
    world->section_response = NULL;
    world->section_response_len = 0;
    world->section_response_index = -1;
}

terrax_world_status terra_section_get_json(
    uint32_t handle,
    const char* section_name,
    char* buffer,
    uint64_t buffer_size,
    uint64_t* required_size) {
    if (required_size) *required_size = 0;
    TxWorld* world = tx_get_world(handle);
    if (!world) return tx_invalid_handle();
    if (!section_name) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "null section name");
        return TERRAX_WORLD_STATUS_INVALID_ARGUMENT;
    }

    int idx = section_index_by_name(section_name, tx_strlen(section_name));
    if (idx == -1) {
        tx_clear_section_response(world, 1);
        tx_set_error("TERRAX_NOT_FOUND", "unknown section");
        return TERRAX_WORLD_STATUS_NOT_FOUND;
    }

    if (world->section_response && world->section_response_index == idx) {
        uint64_t needed = world->section_response_len;
        if (required_size) *required_size = needed;
        if (!buffer || buffer_size == 0) return TERRAX_WORLD_STATUS_OK;
        if (buffer_size < needed) return TERRAX_WORLD_STATUS_BUFFER_TOO_SMALL;
        memcpy(buffer, world->section_response, world->section_response_len);
        tx_clear_section_response(world, 1);
        tx_clear_error();
        return TERRAX_WORLD_STATUS_OK;
    }
    tx_clear_section_response(world, 1);

    /* Serialize section from binary data */
    TxBuf b;
    buf_init(&b, 1024);
    if (!serialize_section_json(world, idx, &b)) {
        if (b.data) tx_internal_free(b.data);
        return TERRAX_WORLD_STATUS_INTERNAL_ERROR;
    }

    /* Null-terminate for the string copy */
    buf_u8(&b, 0);
    if (!b.ok) {
        if (b.data) tx_internal_free(b.data);
        tx_set_error("TERRAX_WASM_OOM", "section serialization failed");
        return TERRAX_WORLD_STATUS_INTERNAL_ERROR;
    }
    uint64_t needed = b.len;
    if (required_size) *required_size = needed;
    if (!buffer || buffer_size == 0) {
        world->section_response = b.data;
        world->section_response_len = b.len;
        world->section_response_index = idx;
        tx_clear_error();
        return TERRAX_WORLD_STATUS_OK;
    }
    if (buffer_size < needed) {
        tx_internal_free(b.data);
        return TERRAX_WORLD_STATUS_BUFFER_TOO_SMALL;
    }
    memcpy(buffer, b.data, b.len);
    tx_internal_free(b.data);
    tx_clear_error();
    return TERRAX_WORLD_STATUS_OK;
}

terrax_world_status terra_section_set_json(
    uint32_t handle,
    const char* section_name,
    const char* json_utf8) {
    TxWorld* world = tx_get_world(handle);
    if (!world) return tx_invalid_handle();
    tx_clear_section_response(world, 1);
    if (!section_name || !json_utf8) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "null parameter");
        return TERRAX_WORLD_STATUS_INVALID_ARGUMENT;
    }

    int idx = section_index_by_name(section_name, tx_strlen(section_name));
    if (idx == -1) {
        tx_set_error("TERRAX_NOT_FOUND", "unknown section");
        return TERRAX_WORLD_STATUS_NOT_FOUND;
    }

    /* Raw JSON is not WLD binary. Only proven binary operations may mutate. */
    if (tx_streq_c(section_name, "tiles")) {
        tx_set_error("TERRAX_NOT_SUPPORTED", "use batch_update_tiles for tile modifications");
    } else {
        tx_set_error("TERRAX_NOT_SUPPORTED", "section has no verified WLD binary encoder");
    }
    return TERRAX_WORLD_STATUS_NOT_SUPPORTED;
}

/* ====================================================================
 * V2 API: Operations
 * ==================================================================== */

static int tx_bytes_equal(const uint8_t* left, const uint8_t* right, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        if (left[i] != right[i]) return 0;
    }
    return 1;
}

static int tx_response_key_matches(TxWorld* world, const char* operation_name,
                                   const char* request_json) {
    if (!world->op_request_key || !world->op_response_ptr || !world->op_response_len) return 0;
    uint32_t operation_len = tx_strlen(operation_name);
    uint32_t request_len = tx_strlen(request_json);
    if (operation_len > UINT32_MAX - request_len - 1u) return 0;
    uint32_t key_len = operation_len + 1u + request_len;
    if (world->op_request_key_len != key_len) return 0;
    if (!tx_bytes_equal(world->op_request_key, (const uint8_t*)operation_name, operation_len)) return 0;
    if (world->op_request_key[operation_len] != 0u) return 0;
    return tx_bytes_equal(world->op_request_key + operation_len + 1u,
                          (const uint8_t*)request_json, request_len);
}

static int tx_cache_response_key(TxWorld* world, const char* operation_name,
                                 const char* request_json) {
    uint32_t operation_len = tx_strlen(operation_name);
    uint32_t request_len = tx_strlen(request_json);
    if (operation_len > UINT32_MAX - request_len - 1u) return 0;
    uint32_t key_len = operation_len + 1u + request_len;
    uint8_t* key = tx_alloc(key_len ? key_len : 1u);
    if (!key) return 0;
    memcpy(key, operation_name, operation_len);
    key[operation_len] = 0u;
    if (request_len) memcpy(key + operation_len + 1u, request_json, request_len);
    world->op_request_key = key;
    world->op_request_key_len = key_len;
    return 1;
}

static void tx_clear_cached_response(TxWorld* world, int release_roots) {
    if (release_roots) {
        if (world->op_response_ptr)
            tx_internal_free((void*)(uintptr_t)world->op_response_ptr);
        if (world->op_request_key)
            tx_internal_free(world->op_request_key);
    }
    world->op_response_ptr = 0;
    world->op_response_len = 0;
    world->op_request_key = NULL;
    world->op_request_key_len = 0;
}

static void tx_clear_media_result(TxWorld* world, int release_root) {
    uint8_t* media = world->media_result;
    if (media && tx_last_ptr == (uint32_t)(uintptr_t)media) {
        tx_last_ptr = 0u;
        tx_last_len = 0u;
        tx_last_width = 0u;
        tx_last_height = 0u;
    }
    if (release_root && media) tx_internal_free(media);
    world->media_result = NULL;
    world->media_result_len = 0;
    world->media_result_width = 0;
    world->media_result_height = 0;
    world->media_result_kind = 0;
}

terrax_world_status terra_op_execute_json(
    uint32_t handle,
    const char* operation_name,
    const char* request_json,
    char* response_buffer,
    uint64_t response_buffer_size,
    uint64_t* required_size) {
    if (required_size) *required_size = 0;
    TxWorld* world = tx_get_world(handle);
    if (!world) return tx_invalid_handle();
    if (!operation_name || !request_json) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "null operation name or request");
        return TERRAX_WORLD_STATUS_INVALID_ARGUMENT;
    }
    tx_clear_section_response(world, 1);

    /* Serve only an exact operation/request match from the prior probe. */
    if (tx_response_key_matches(world, operation_name, request_json)) {
        uint64_t needed = (uint64_t)world->op_response_len;
        if (required_size) *required_size = needed;
        if (!response_buffer || response_buffer_size == 0) {
            return TERRAX_WORLD_STATUS_OK;
        }
        if (response_buffer_size < needed) {
            return TERRAX_WORLD_STATUS_BUFFER_TOO_SMALL;
        }
        const char* src = (const char*)(uintptr_t)world->op_response_ptr;
        for (uint32_t i = 0; i < world->op_response_len; i++) response_buffer[i] = src[i];
        tx_clear_cached_response(world, 1);
        tx_clear_error();
        return TERRAX_WORLD_STATUS_OK;
    }

    /* Bridge-owned caller strings are outside native rewinds. Keep the short
       operation name on stack, but read the bounded request in place so large
       section payloads do not require a second full-size copy. */
    char on_copy[64];
    const char* request_copy = request_json;
    {
        uint32_t on_len = tx_strlen(operation_name);
        if (on_len >= sizeof(on_copy)) {
            tx_clear_cached_response(world, 1);
            tx_clear_media_result(world, 1);
            tx_set_error("TERRAX_INVALID_ARGUMENT", "operation name exceeds 63 bytes");
            return TERRAX_WORLD_STATUS_INVALID_ARGUMENT;
        }
        uint32_t k;
        for (k = 0; k < on_len; k++) on_copy[k] = operation_name[k];
        on_copy[on_len] = 0;
        uint32_t rj_len = tx_strlen(request_json);
        if (rj_len > 1024u * 1024u) {
            tx_clear_cached_response(world, 1);
            tx_clear_media_result(world, 1);
            tx_set_error("TERRAX_INVALID_ARGUMENT", "operation request exceeds 1 MiB");
            return TERRAX_WORLD_STATUS_INVALID_ARGUMENT;
        }
    }

    /* Release ONLY the previous operation's transient allocations.
       Rewind to max(last_op_heap_end, heap_mark) but never below the
       caller's entry heap position. heap_mark is advanced when a verified
       binary section override is adopted.
       Bridge allocations are a separate domain and cannot be invalidated. */
    {
        uint32_t target = world->last_op_heap_end;
        uint32_t hm = world->heap_mark;
        if (hm > target) target = hm;
        tx_clear_cached_response(world, 0);
        tx_clear_media_result(world, 0);
        if (target > 0) {
            tx_rewind(target);
            tx_last_ptr = 0;
            tx_last_len = 0;
            tx_last_width = 0;
            tx_last_height = 0;
        }
    }

    world->heap_mark = tx_mark();

    /* Execute the operation FIRST, then allocate the response buffer.
       Operations that store section overrides on the bump heap must finish
       before the response allocation so override data is not corrupted when
       the response is copied back to the caller. */
    TxBuf response;
    response.data = NULL;
    response.len = 0;
    response.cap = 0;
    response.ok = 1;

    int result = op_execute_json(world, on_copy, request_copy, &response);
    if (result < 0) {
        world->last_op_heap_end = world->heap_mark;
        tx_clear_cached_response(world, 0);
        tx_clear_media_result(world, 0);
        return (terrax_world_status)(tx_last_status > TERRAX_WORLD_STATUS_OK ?
            tx_last_status : TERRAX_WORLD_STATUS_INTERNAL_ERROR);
    }

    if (tx_streq_c(on_copy, "render_preview_png") ||
               tx_streq_c(on_copy, "render_thumbnail_png")) {
        world->media_result = (uint8_t*)(uintptr_t)tx_last_ptr;
        world->media_result_len = tx_last_len;
        world->media_result_width = tx_last_width;
        world->media_result_height = tx_last_height;
        world->media_result_kind = 2u;
    } else if (tx_streq_c(on_copy, "render_lit_map") ||
               tx_streq_c(on_copy, "mark_tiles_and_chests_map")) {
        if (!tx_last_ptr || tx_last_len == 0u || tx_last_len > TX_MAP_MAX_OUTPUT_BYTES) {
            if (tx_last_ptr) tx_internal_free((void*)(uintptr_t)tx_last_ptr);
            tx_last_ptr = tx_last_len = tx_last_width = tx_last_height = 0u;
            if (response.data) tx_internal_free(response.data);
            world->last_op_heap_end = world->heap_mark;
            tx_set_error("TERRAX_RESULT_TOO_LARGE", "map output is empty or exceeds the 128 MiB budget");
            return TERRAX_WORLD_STATUS_INTERNAL_ERROR;
        }
        world->media_result = (uint8_t*)(uintptr_t)tx_last_ptr;
        world->media_result_len = tx_last_len;
        world->media_result_width = tx_last_width;
        world->media_result_height = tx_last_height;
        world->media_result_kind = 3u;
    }

    /* Save heap position after this operation's allocations. */
    world->last_op_heap_end = world->heap_mark;

    /* Null-terminate for string copy */
    buf_u8(&response, 0);
    if (!response.ok || !response.data) {
        if (response.data) tx_internal_free(response.data);
        tx_clear_media_result(world, 1);
        tx_last_ptr = tx_last_len = tx_last_width = tx_last_height = 0;
        tx_set_error("TERRAX_WASM_OOM", "operation response allocation failed");
        return TERRAX_WORLD_STATUS_INTERNAL_ERROR;
    }
    uint32_t resp_len = response.len > 0 ? response.len - 1 : 0;
    uint64_t needed = (uint64_t)resp_len + 1u;
    if (required_size) *required_size = needed;

    /* Probe and short-buffer calls cache the exact response so retries do not
       execute a mutating operation twice. */
    if (!response_buffer || response_buffer_size == 0 || response_buffer_size < needed) {
        world->op_response_ptr = (uint32_t)(uintptr_t)response.data;
        world->op_response_len = resp_len + 1u;
        if (!tx_cache_response_key(world, on_copy, request_copy)) {
            tx_clear_cached_response(world, 1);
            tx_clear_media_result(world, 1);
            tx_set_error("TERRAX_WASM_OOM", "failed to cache operation response key");
            return TERRAX_WORLD_STATUS_INTERNAL_ERROR;
        }
        return (!response_buffer || response_buffer_size == 0)
            ? TERRAX_WORLD_STATUS_OK : TERRAX_WORLD_STATUS_BUFFER_TOO_SMALL;
    }
    /* Real call: clear any leftover cache (should not happen) */
    world->op_response_ptr = 0;
    world->op_response_len = 0;
    /* Copy response to caller buffer */
    for (uint32_t i = 0; i <= resp_len; i++) response_buffer[i] = (char)response.data[i];
    tx_internal_free(response.data);
    tx_clear_error();
    return TERRAX_WORLD_STATUS_OK;
}
/* ====================================================================
 * Exported: retrieve rendered PNG thumbnail from WASM memory.
 *
 * Must be called after a "render_thumbnail_png" or "render_preview_png"
 * operation via terra_op_execute_json, which leaves the PNG pointer
 * in tx_last_ptr / tx_last_len.
 *
 * Two-call pattern:
 *   1st call: buffer=NULL, buffer_size=0 → writes required_size, width, height
 *   2nd call: buffer=allocated, buffer_size>=required_size → copies PNG bytes
 * ==================================================================== */
terrax_world_status terra_op_get_thumbnail_png(
    uint32_t handle,
    uint8_t* buffer,
    uint64_t buffer_size,
    uint64_t* required_size,
    uint32_t* width,
    uint32_t* height)
{
    if (required_size) *required_size = 0;
    TxWorld* world = tx_get_world(handle);
    if (!world) return tx_invalid_handle();
    if (world->media_result_kind != 2u || !world->media_result || world->media_result_len == 0) {
        tx_set_error("TERRAX_STATE_ERROR", "no thumbnail rendered yet");
        return TERRAX_WORLD_STATUS_STATE_ERROR;
    }

    uint64_t needed = (uint64_t)world->media_result_len;
    if (required_size) *required_size = needed;
    if (width)         *width  = world->media_result_width;
    if (height)        *height = world->media_result_height;

    if (!buffer || buffer_size == 0) {
        return TERRAX_WORLD_STATUS_BUFFER_TOO_SMALL;
    }
    if (buffer_size < needed) {
        return TERRAX_WORLD_STATUS_BUFFER_TOO_SMALL;
    }

    memcpy(buffer, world->media_result, world->media_result_len);
    tx_clear_media_result(world, 1);
    tx_clear_error();
    return TERRAX_WORLD_STATUS_OK;
}

terrax_world_status terra_op_get_map(
    uint32_t handle,
    uint8_t* buffer,
    uint64_t buffer_size,
    uint64_t* required_size,
    uint32_t* width,
    uint32_t* height)
{
    if (required_size) *required_size = 0;
    TxWorld* world = tx_get_world(handle);
    if (!world) return tx_invalid_handle();
    if (world->media_result_kind != 3u || !world->media_result ||
        world->media_result_len == 0u || world->media_result_len > TX_MAP_MAX_OUTPUT_BYTES) {
        tx_set_error("TERRAX_STATE_ERROR", "no map output available");
        return TERRAX_WORLD_STATUS_STATE_ERROR;
    }

    uint64_t needed = (uint64_t)world->media_result_len;
    if (required_size) *required_size = needed;
    if (width) *width = world->media_result_width;
    if (height) *height = world->media_result_height;

    if (!buffer || buffer_size == 0u || buffer_size < needed)
        return TERRAX_WORLD_STATUS_BUFFER_TOO_SMALL;

    memcpy(buffer, world->media_result, world->media_result_len);
    tx_clear_media_result(world, 1);
    tx_clear_error();
    return TERRAX_WORLD_STATUS_OK;
}
