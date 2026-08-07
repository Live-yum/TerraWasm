/*
 * terra_ops.c -- Operation dispatcher for terra_op_execute_json.
 *
 * Dispatches named operations to their implementations.
 * Operations are: txw_render_preview_png, txw_render_preview_rgba, render_lit_map,
 * mark_chest_items_preview, mark_chest_items_map,
 * mark_tiles_and_chests_preview, mark_tiles_and_chests_map,
 * convert_world_biome, batch_update_tiles, unlock_bestiary,
 * apply_pixel_art_mapping.
 *
 * set_visibility and remove_all_wires are merged into batch_update_tiles.
 */
#include "terra_types.h"
#include "terra_map.h"
#include <stdio.h>

/* External declarations */
extern uint8_t* tx_alloc(uint32_t size);
extern void tx_internal_free(void* ptr);
extern void tx_clear_error(void);
extern void tx_set_error(const char* code, const char* message);
extern int set_result_buf(TxBuf* b);
extern void buf_init(TxBuf* b, uint32_t cap);
extern void buf_u8(TxBuf* b, uint8_t v);
extern void buf_cstr(TxBuf* b, const char* s);
extern void json_u32(TxBuf* b, uint32_t v);
extern void json_i32(TxBuf* b, int32_t v);
extern void json_string(TxBuf* b, const char* s);
extern int tx_streq_c(const char* a, const char* b);
extern uint32_t tx_strlen(const char* s);
extern int tx_mutate_header_patch(TxWorld* w, const char* request, uint32_t request_len, TxBuf* response);
extern int tx_mutate_replace_chests(TxWorld* w, const char* request, uint32_t request_len, TxBuf* response);
extern int tx_mutate_replace_bestiary(TxWorld* w, const char* request, uint32_t request_len, TxBuf* response);

extern uint32_t tx_last_ptr;
extern uint32_t tx_last_len;
extern uint32_t tx_last_width;
extern uint32_t tx_last_height;
extern uint32_t tx_last_stride;

/* From terra_json.c */
extern int json_validate_document(const char* json, int jlen);
extern int json_find_key(const char* json, int jlen, const char* key);
extern int json_extract_str(const char* json, int jlen, int pos, char* out, int ocap);
extern int json_extract_int(const char* json, int jlen, int pos, int32_t* out);
extern int json_skip_value(const char* json, int jlen, int pos);

/* From terra_render.c */
extern int txw_render_preview_png(TxWorld* w, uint32_t max_w, uint32_t max_h);
extern int txw_render_preview_rgba(TxWorld* w, uint32_t max_w, uint32_t max_h);
extern int txw_render_marked_preview_png(TxWorld* w, uint32_t max_w, uint32_t max_h,
                                         const MapMarkerEntry* chest_markers, uint32_t chest_count,
                                         const MapMarkerEntry* tile_markers, uint32_t tile_count,
                                         uint32_t* matched_chest_count,
                                         uint32_t* matched_tile_count);

/* From terra_update.c */
extern int execute_batch_update_tiles(TxWorld* w, const char* request, int jlen, TxBuf* response);
extern int execute_set_visibility(TxWorld* w, const char* request, int jlen, TxBuf* response);
extern int execute_remove_all_wires(TxWorld* w, const char* request, int jlen, TxBuf* response);

/* From terra_pixel_art.c */
extern int txw_queue_pixel_art(uint32_t handle, int32_t start_x, int32_t start_y,
                                uint32_t width, uint32_t height,
                                uint32_t pixels_ptr, uint32_t pixels_len,
                                uint32_t map_ptr, uint32_t map_count,
                                int32_t skip_transparent);

/* From terra_api.c */
extern int write_file_from_heap(const char* path, const uint8_t* data, uint32_t len);

/* From terra_json.c */
extern int json_extract_bool(const char* json, int jlen, int pos, int* out);
extern int json_array_count(const char* json, int jlen, int pos);
extern int json_array_element(const char* json, int jlen, int pos, int index);

static int ascii_lower(int value) {
    if (value >= 'A' && value <= 'Z') return value + ('a' - 'A');
    return value;
}

static int path_has_suffix_case_insensitive(const char* path, const char* suffix) {
    uint32_t path_len = tx_strlen(path);
    uint32_t suffix_len = tx_strlen(suffix);
    if (path_len < suffix_len) return 0;
    for (uint32_t index = 0u; index < suffix_len; index++) {
        int left = ascii_lower((unsigned char)path[path_len - suffix_len + index]);
        int right = ascii_lower((unsigned char)suffix[index]);
        if (left != right) return 0;
    }
    return 1;
}

/* Generic operation JSON can be influenced by a less-trusted caller than the
 * direct path-based Node API. Node builds use NODERAWFS, so operation output
 * paths must stay relative to the process working directory and may not walk
 * through parent segments. Preview writes are additionally limited to PNGs. */
static int operation_output_path_is_safe(const char* path, const char* required_suffix) {
    if (!path || !path[0]) return 1;
    if (path[0] == '/' || path[0] == '\\') return 0;
    if (path[1] == ':') return 0;

    uint32_t segment_start = 0u;
    for (uint32_t index = 0u;; index++) {
        unsigned char value = (unsigned char)path[index];
        if (value != 0u && value < 32u) return 0;
        if (value == ':') return 0;
        if (value == '/' || value == '\\' || value == 0u) {
            uint32_t segment_len = index - segment_start;
            if (segment_len == 2u
                && path[segment_start] == '.'
                && path[segment_start + 1u] == '.') {
                return 0;
            }
            if (value == 0u) break;
            segment_start = index + 1u;
        }
    }

    return !required_suffix || path_has_suffix_case_insensitive(path, required_suffix);
}

static int validate_operation_output_path(const char* path, const char* required_suffix) {
    if (operation_output_path_is_safe(path, required_suffix)) return 1;
    tx_set_error(
        "TERRAX_VALIDATION_ERROR",
        required_suffix
            ? "operation output_path must be a relative .png path without parent traversal"
            : "operation output_dir must be relative and must not contain parent traversal");
    return 0;
}

#ifdef TERRAX_TESTING
int terrax_test_operation_output_path_is_safe(const char* path, const char* required_suffix) {
    return operation_output_path_is_safe(path, required_suffix);
}
#endif

/* Hex color parsing helper */
static uint8_t parse_hex_byte(const char* s) {
    uint8_t v = 0;
    for (int k = 0; k < 2; k++) {
        char c = s[k];
        v = (uint8_t)(v << 4);
        if (c >= '0' && c <= '9') v |= (uint8_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint8_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint8_t)(c - 'A' + 10);
    }
    return v;
}

static int is_hex_digit(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static int marker_color_is_valid(const char* hex) {
    uint32_t len = tx_strlen(hex);
    if ((len != 7u && len != 9u) || hex[0] != '#') return 0;
    for (uint32_t i = 1u; i < len; i++) {
        if (!is_hex_digit(hex[i])) return 0;
    }
    return 1;
}

static void default_marker_rgba(uint8_t rgba[4]) {
    rgba[0] = 255u;
    rgba[1] = 35u;
    rgba[2] = 26u;
    rgba[3] = 255u;
}

static void parse_marker_rgba(const char* hex, uint8_t rgba[4]) {
    if (!hex || !marker_color_is_valid(hex)) {
        default_marker_rgba(rgba);
        return;
    }
    {
        const char* h = hex + 1;
        rgba[0] = parse_hex_byte(h);
        rgba[1] = parse_hex_byte(h + 2);
        rgba[2] = parse_hex_byte(h + 4);
        rgba[3] = hex[7] ? parse_hex_byte(h + 6) : 255u;
    }
}

static int parse_marker_size_field(const char* request, int jlen, int elem,
                                   const char* key, int32_t default_value,
                                   int32_t max_value, const char* error_message,
                                   uint8_t* out_value) {
    int elem_end = json_skip_value(request, jlen, elem);
    int elem_len = elem_end > elem ? elem_end - elem : 0;
    int field_pos = elem_len > 0 ? json_find_key(request + elem, elem_len, key) : -1;
    int32_t value = default_value;
    if (field_pos >= 0) {
        if (!json_extract_int(request, jlen, elem + field_pos, &value)) {
            tx_set_error("TERRAX_VALIDATION_ERROR", error_message);
            return 0;
        }
        if (value <= 0) value = default_value;
        if (value > max_value) value = max_value;
    }
    *out_value = (uint8_t)value;
    return 1;
}

static int parse_marker_array(const char* request, int jlen,
                              const char* array_key, const char* id_key,
                              MapMarkerEntry** out_markers, uint32_t* out_count) {
    *out_markers = NULL;
    *out_count = 0u;
    int array_pos = json_find_key(request, jlen, array_key);
    if (array_pos < 0) return 1;

    int count = json_array_count(request, jlen, array_pos);
    if (count < 0) {
        tx_set_error("TERRAX_VALIDATION_ERROR", "marker field must be an array");
        return 0;
    }
    if (count == 0) return 1;
    if (count > 256) {
        tx_set_error("TERRAX_VALIDATION_ERROR", "marker array exceeds 256 entries");
        return 0;
    }

    uint64_t bytes = (uint64_t)(uint32_t)count * (uint64_t)sizeof(MapMarkerEntry);
    if (bytes > UINT32_MAX) {
        tx_set_error("TERRAX_WASM_OOM", "marker array size overflow");
        return 0;
    }
    MapMarkerEntry* markers = (MapMarkerEntry*)tx_alloc((uint32_t)bytes);
    if (!markers) {
        tx_set_error("TERRAX_WASM_OOM", "marker array allocation failed");
        return 0;
    }

    for (int i = 0; i < count; i++) {
        int elem = json_array_element(request, jlen, array_pos, i);
        if (elem < 0) {
            tx_internal_free(markers);
            tx_set_error("TERRAX_VALIDATION_ERROR", "invalid marker entry");
            return 0;
        }
        int elem_end = json_skip_value(request, jlen, elem);
        int elem_len = elem_end > elem ? elem_end - elem : 0;
        int id_pos = elem_len > 0 ? json_find_key(request + elem, elem_len, id_key) : -1;
        int32_t id = 0;
        if (id_pos < 0 || !json_extract_int(request, jlen, elem + id_pos, &id)) {
            tx_internal_free(markers);
            tx_set_error("TERRAX_VALIDATION_ERROR", "marker id must be an integer");
            return 0;
        }

        uint32_t map_value = 0u;
        uint8_t rgba[4];
        default_marker_rgba(rgba);
        int color_pos = elem_len > 0 ? json_find_key(request + elem, elem_len, "color") : -1;
        if (color_pos >= 0) {
            char hex[16] = {0};
            if (!json_extract_str(request, jlen, elem + color_pos, hex, sizeof(hex)) ||
                !marker_color_is_valid(hex)) {
                tx_internal_free(markers);
                tx_set_error("TERRAX_VALIDATION_ERROR", "marker color must be #RRGGBB or #RRGGBBAA");
                return 0;
            }
            parse_marker_rgba(hex, rgba);
        }
        if (!parse_marker_size_field(
                request, jlen, elem, "radius", 30, 60,
                "marker radius must be an integer", &markers[i].radius)) {
            tx_internal_free(markers);
            return 0;
        }
        if (!parse_marker_size_field(
                request, jlen, elem, "line_width", 3, 15,
                "marker line_width must be an integer", &markers[i].line_width)) {
            tx_internal_free(markers);
            return 0;
        }
        markers[i].id = id;
        markers[i].map_value = map_value;
        markers[i].rgba[0] = rgba[0];
        markers[i].rgba[1] = rgba[1];
        markers[i].rgba[2] = rgba[2];
        markers[i].rgba[3] = rgba[3];
        markers[i].reserved[0] = 0u;
        markers[i].reserved[1] = 0u;
    }

    *out_markers = markers;
    *out_count = (uint32_t)count;
    return 1;
}

static int finish_map_response(TxBuf* response, uint8_t* map_data, uint32_t map_len,
                               uint32_t map_width, uint32_t map_height) {
    if (!response->ok || !response->data) {
        if (map_data) tx_internal_free(map_data);
        tx_set_error("TERRAX_WASM_OOM", "map operation response allocation failed");
        return -1;
    }
    int response_len = set_result_buf(response);
    if (response_len < 0) {
        if (map_data) tx_internal_free(map_data);
        return -1;
    }
    tx_last_ptr = (uint32_t)(uintptr_t)map_data;
    tx_last_len = map_len;
    tx_last_width = map_width;
    tx_last_height = map_height;
    tx_last_stride = 0u;
    return response_len;
}

static void load_preview_bounds(const char* request, int jlen, uint32_t* max_w, uint32_t* max_h) {
    int32_t parsed_w = 0;
    int32_t parsed_h = 0;
    int p = json_find_key(request, jlen, "max_w");
    if (p >= 0) json_extract_int(request, jlen, p, &parsed_w);
    p = json_find_key(request, jlen, "max_h");
    if (p >= 0) json_extract_int(request, jlen, p, &parsed_h);
    *max_w = parsed_w > 0 ? (uint32_t)parsed_w : 0u;
    *max_h = parsed_h > 0 ? (uint32_t)parsed_h : 0u;
}

static int finish_preview_png_response(TxWorld* w, TxBuf* response,
                                       uint8_t* png_data, uint32_t png_len,
                                       uint32_t png_width, uint32_t png_height) {
    if (!response->ok || !response->data) {
        if (response->data) tx_internal_free(response->data);
        if (png_data) tx_internal_free(png_data);
        tx_set_error("TERRAX_WASM_OOM", "preview operation response allocation failed");
        return -1;
    }

    {
        int response_len = set_result_buf(response);
        if (response_len < 0) {
            if (png_data) tx_internal_free(png_data);
            return -1;
        }

        w->media_result = png_data;
        w->media_result_len = png_len;
        w->media_result_width = png_width;
        w->media_result_height = png_height;
        w->media_result_stride = 0u;
        w->media_result_kind = 2u;

        tx_last_ptr = (uint32_t)(uintptr_t)png_data;
        tx_last_len = png_len;
        tx_last_width = png_width;
        tx_last_height = png_height;
        tx_last_stride = 0u;
        return response_len;
    }
}

/* ====================================================================
 * Operation: txw_render_preview_png
 * Writes PNG to file at output_path from request JSON.
 * ==================================================================== */

static int execute_txw_render_preview_png(TxWorld* w, const char* request, int jlen,
                                      TxBuf* response) {
    /* Parse optional max_w, max_h from request */
    int32_t max_w = 0, max_h = 0;
    int p = json_find_key(request, jlen, "max_w");
    if (p >= 0) json_extract_int(request, jlen, p, &max_w);
    p = json_find_key(request, jlen, "max_h");
    if (p >= 0) json_extract_int(request, jlen, p, &max_h);

    /* Parse and validate output_path before the expensive render. */
    char output_path[512] = {0};
    p = json_find_key(request, jlen, "output_path");
    if (p >= 0 && !json_extract_str(request, jlen, p, output_path, sizeof(output_path))) {
        tx_set_error("TERRAX_VALIDATION_ERROR", "operation output_path must be a string under 512 bytes");
        return -1;
    }
    if (!validate_operation_output_path(output_path, ".png")) return -1;

    /* Render PNG into WASM memory */
    int result = txw_render_preview_png(w, (uint32_t)max_w, (uint32_t)max_h);
    if (result < 0) return -1;

    /* Save PNG pointer before set_result_buf overwrites it */
    uint32_t png_ptr = tx_last_ptr, png_len = tx_last_len;
    uint32_t png_w = tx_last_width, png_h = tx_last_height;

    if (output_path[0] && !write_file_from_heap(
            output_path, (const uint8_t*)(uintptr_t)png_ptr, png_len)) {
        tx_set_error("TERRAX_IO_ERROR", "preview PNG output write failed");
        return -1;
    }

    /* Build response */
    buf_cstr(response, "{\"status\":\"ok\",\"output_path\":");
    json_string(response, output_path);
    buf_cstr(response, ",\"width\":");
    json_u32(response, png_w);
    buf_cstr(response, ",\"height\":");
    json_u32(response, png_h);
    buf_cstr(response, ",\"size\":");
    json_u32(response, png_len);
    buf_u8(response, '}');
    int rlen = set_result_buf(response);
    /* Restore PNG pointer so it remains accessible */
    tx_last_ptr = png_ptr; tx_last_len = png_len;
    tx_last_width = png_w; tx_last_height = png_h;
    return rlen;
}

/* ====================================================================
 * Operation: render_thumbnail_png
 * Renders PNG thumbnail with proportional scaling (default max_w=1920).
 * PNG bytes remain in WASM memory for retrieval via
 * terra_op_get_thumbnail_png().
 * ==================================================================== */

static int execute_txw_render_thumbnail_png(TxWorld* w, const char* request, int jlen,
                                            TxBuf* response) {
    int32_t max_w = 1920;
    int p = json_find_key(request, jlen, "max_w");
    if (p >= 0) json_extract_int(request, jlen, p, &max_w);
    if (max_w <= 0) max_w = 1920;

    int result = txw_render_preview_png(w, (uint32_t)max_w, 0);
    if (result < 0) return -1;

    /* Save PNG pointer so terra_op_get_thumbnail_png can retrieve it */
    uint32_t png_ptr = tx_last_ptr, png_len = tx_last_len;
    uint32_t png_w = tx_last_width, png_h = tx_last_height;

    /* Build response */
    buf_cstr(response, "{\"status\":\"ok\",\"width\":");
    json_u32(response, png_w);
    buf_cstr(response, ",\"height\":");
    json_u32(response, png_h);
    buf_cstr(response, ",\"size\":");
    json_u32(response, png_len);
    buf_u8(response, '}');
    int rlen = set_result_buf(response);
    /* Restore PNG pointer so terra_op_get_thumbnail_png works */
    tx_last_ptr = png_ptr; tx_last_len = png_len;
    tx_last_width = png_w; tx_last_height = png_h;
    return rlen;
}

/* ====================================================================
 * Operation: txw_render_preview_rgba
 * Renders RGBA buffer into WASM memory.
 * ==================================================================== */

static int execute_txw_render_preview_rgba(TxWorld* w, const char* request, int jlen,
                                       TxBuf* response) {
    int32_t max_w = 0, max_h = 0;
    int p = json_find_key(request, jlen, "max_w");
    if (p >= 0) json_extract_int(request, jlen, p, &max_w);
    p = json_find_key(request, jlen, "max_h");
    if (p >= 0) json_extract_int(request, jlen, p, &max_h);

    int result = txw_render_preview_rgba(w, (uint32_t)max_w, (uint32_t)max_h);
    if (result < 0) return -1;

    /* Save RGBA pointers before set_result_buf overwrites them */
    uint32_t sv_ptr = tx_last_ptr, sv_len = tx_last_len;
    uint32_t sv_w = tx_last_width, sv_h = tx_last_height, sv_s = tx_last_stride;

    buf_cstr(response, "{\"status\":\"ok\",\"pixel_format\":\"rgba8\",\"width\":");
    json_u32(response, sv_w);
    buf_cstr(response, ",\"height\":");
    json_u32(response, sv_h);
    buf_cstr(response, ",\"stride\":");
    json_u32(response, sv_s);
    buf_cstr(response, ",\"buffer_size\":");
    json_u32(response, sv_len);
    buf_u8(response, '}');
    int rlen = set_result_buf(response);
    /* Restore RGBA buffer pointers so terra_op_get_preview_rgba works */
    tx_last_ptr = sv_ptr; tx_last_len = sv_len;
    tx_last_width = sv_w; tx_last_height = sv_h; tx_last_stride = sv_s;
    return rlen;
}

/* ====================================================================
 * Operation: render_lit_map
 * Generates a .map file at output_dir.
 * ==================================================================== */

static int execute_render_lit_map(TxWorld* w, const char* request, int jlen,
                                  TxBuf* response) {
    char dir_buf[512] = {0};
    int dir_pos = json_find_key(request, jlen, "output_dir");
    if (dir_pos >= 0 && !json_extract_str(request, jlen, dir_pos, dir_buf, sizeof(dir_buf))) {
        tx_set_error("TERRAX_VALIDATION_ERROR", "operation output_dir must be a string under 512 bytes");
        return -1;
    }
    if (!validate_operation_output_path(dir_buf, NULL)) return -1;

    int32_t result = terra_generate_map(w);
    if (result < 0) return -1;

    uint8_t* map_data = (uint8_t*)(uintptr_t)tx_last_ptr;
    uint32_t map_len = tx_last_len;
    uint32_t map_w = tx_last_width;
    uint32_t map_h = tx_last_height;

    int wrote_file = 0;
    if (dir_buf[0] && map_data && map_len > 0) {
        char map_path[768];
        uint32_t pos = 0;
        for (uint32_t i = 0; dir_buf[i] && pos < sizeof(map_path) - 32; i++)
            map_path[pos++] = dir_buf[i];
        if (pos > 0 && map_path[pos-1] != '/' && map_path[pos-1] != '\\')
            map_path[pos++] = '/';
        const char* fname = "world.map";
        for (uint32_t i = 0; fname[i] && pos < sizeof(map_path) - 1; i++)
            map_path[pos++] = fname[i];
        map_path[pos] = 0;
        wrote_file = write_file_from_heap(map_path, map_data, map_len);
    }

    buf_cstr(response, "{\"status\":\"ok\",\"width\":");
    json_u32(response, map_w);
    buf_cstr(response, ",\"height\":");
    json_u32(response, map_h);
    buf_cstr(response, ",\"map_bytes\":");
    json_u32(response, map_len);
    buf_cstr(response, ",\"file_written\":");
    buf_cstr(response, wrote_file ? "true" : "false");
    buf_u8(response, '}');
    return finish_map_response(response, map_data, map_len, map_w, map_h);
}
static int execute_unlock_bestiary(TxWorld* w, const char* request, int jlen,
                                   TxBuf* response) {
    (void)w; (void)request; (void)jlen; (void)response;
    tx_set_error("TERRAX_NOT_SUPPORTED",
                 "unlock_bestiary has no verified built-in catalog; use replace_bestiary");
    return -1;
}

/* ====================================================================
 * Operation: mark_chest_items_preview
 * Renders a preview PNG with chest marker rings using the legacy request shape.
 * ==================================================================== */

static int execute_mark_chest_items_preview(TxWorld* w, const char* request, int jlen,
                                            TxBuf* response) {
    MapMarkerEntry* chest_markers = NULL;
    uint32_t chest_count = 0u;
    uint32_t max_w = 0u;
    uint32_t max_h = 0u;
    uint32_t matched_chests = 0u;
    uint32_t matched_tiles = 0u;

    if (!parse_marker_array(
            request, jlen, "markers", "item_id", &chest_markers, &chest_count)) {
        return -1;
    }
    if (chest_count == 0u) {
        tx_set_error("TERRAX_VALIDATION_ERROR", "markers must contain at least one marker");
        return -1;
    }

    load_preview_bounds(request, jlen, &max_w, &max_h);
    if (txw_render_marked_preview_png(
            w, max_w, max_h, chest_markers, chest_count, NULL, 0u,
            &matched_chests, &matched_tiles) < 0) {
        tx_internal_free(chest_markers);
        return -1;
    }
    tx_internal_free(chest_markers);

    {
        uint8_t* png_data = (uint8_t*)(uintptr_t)tx_last_ptr;
        uint32_t png_len = tx_last_len;
        uint32_t png_w = tx_last_width;
        uint32_t png_h = tx_last_height;

        buf_cstr(response, "{\"status\":\"ok\",\"matched_chest_count\":");
        json_u32(response, matched_chests);
        buf_cstr(response, ",\"width\":");
        json_u32(response, png_w);
        buf_cstr(response, ",\"height\":");
        json_u32(response, png_h);
        buf_cstr(response, ",\"marker_count\":");
        json_u32(response, chest_count);
        buf_cstr(response, ",\"thumbnail_png_bytes\":");
        json_u32(response, png_len);
        buf_u8(response, '}');
        return finish_preview_png_response(w, response, png_data, png_len, png_w, png_h);
    }
}

/* ====================================================================
 * Operation: mark_chest_items_map
 * Generates a marked map from the legacy `markers` request shape.
 * ==================================================================== */

static int execute_mark_chest_items_map(TxWorld* w, const char* request, int jlen,
                                        TxBuf* response) {
    char output_dir[512] = {0};
    int output_pos = json_find_key(request, jlen, "output_dir");
    if (output_pos >= 0 && !json_extract_str(
            request, jlen, output_pos, output_dir, sizeof(output_dir))) {
        tx_set_error("TERRAX_VALIDATION_ERROR", "operation output_dir must be a string under 512 bytes");
        return -1;
    }
    if (!validate_operation_output_path(output_dir, NULL)) return -1;

    MapMarkerEntry* chest_markers = NULL;
    uint32_t chest_count = 0u;
    if (!parse_marker_array(
            request, jlen, "markers", "item_id", &chest_markers, &chest_count)) {
        return -1;
    }
    if (chest_count == 0u) {
        tx_set_error("TERRAX_VALIDATION_ERROR", "markers must contain at least one marker");
        return -1;
    }

    uint32_t matched_chests = 0u;
    uint32_t matched_tiles = 0u;
    int32_t result = terra_render_lit_map_marked(
        w, chest_markers, chest_count, NULL, 0u, &matched_chests, &matched_tiles);
    tx_internal_free(chest_markers);
    if (result < 0) return -1;

    uint8_t* map_data = (uint8_t*)(uintptr_t)tx_last_ptr;
    uint32_t map_len = tx_last_len;
    uint32_t map_w = tx_last_width;
    uint32_t map_h = tx_last_height;

    int wrote_file = 0;
    if (output_dir[0] && map_data && map_len > 0) {
        char map_path[768];
        uint32_t pos = 0u;
        for (uint32_t i = 0; output_dir[i] && pos < sizeof(map_path) - 32u; i++)
            map_path[pos++] = output_dir[i];
        if (pos > 0u && map_path[pos - 1u] != '/' && map_path[pos - 1u] != '\\')
            map_path[pos++] = '/';
        const char* filename = "marked_world.map";
        for (uint32_t i = 0; filename[i] && pos < sizeof(map_path) - 1u; i++)
            map_path[pos++] = filename[i];
        map_path[pos] = 0;
        wrote_file = write_file_from_heap(map_path, map_data, map_len);
    }

    buf_cstr(response, "{\"status\":\"ok\",\"matched_chest_count\":");
    json_u32(response, matched_chests);
    buf_cstr(response, ",\"width\":");
    json_u32(response, map_w);
    buf_cstr(response, ",\"height\":");
    json_u32(response, map_h);
    buf_cstr(response, ",\"marker_count\":");
    json_u32(response, chest_count);
    buf_cstr(response, ",\"map_bytes\":");
    json_u32(response, map_len);
    buf_cstr(response, ",\"file_written\":");
    buf_cstr(response, wrote_file ? "true" : "false");
    buf_u8(response, '}');
    return finish_map_response(response, map_data, map_len, map_w, map_h);
}

/* ====================================================================
 * Operation: mark_tiles_and_chests_preview
 * Renders a preview PNG with chest/tile marker rings.
 * ==================================================================== */

static int execute_mark_tiles_and_chests_preview(TxWorld* w, const char* request, int jlen,
                                                 TxBuf* response) {
    MapMarkerEntry* chest_markers = NULL;
    MapMarkerEntry* tile_markers = NULL;
    uint32_t chest_count = 0u;
    uint32_t tile_count = 0u;
    uint32_t max_w = 0u;
    uint32_t max_h = 0u;
    uint32_t matched_chests = 0u;
    uint32_t matched_tiles = 0u;

    if (!parse_marker_array(
            request, jlen, "chest_markers", "item_id", &chest_markers, &chest_count)) {
        return -1;
    }
    if (!parse_marker_array(
            request, jlen, "tile_markers", "tile_type", &tile_markers, &tile_count)) {
        if (chest_markers) tx_internal_free(chest_markers);
        return -1;
    }
    if (chest_count == 0u && tile_count == 0u) {
        if (chest_markers) tx_internal_free(chest_markers);
        if (tile_markers) tx_internal_free(tile_markers);
        tx_set_error("TERRAX_VALIDATION_ERROR",
                     "at least one of chest_markers or tile_markers required");
        return -1;
    }

    load_preview_bounds(request, jlen, &max_w, &max_h);
    if (txw_render_marked_preview_png(
            w, max_w, max_h, chest_markers, chest_count, tile_markers, tile_count,
            &matched_chests, &matched_tiles) < 0) {
        if (chest_markers) tx_internal_free(chest_markers);
        if (tile_markers) tx_internal_free(tile_markers);
        return -1;
    }
    if (chest_markers) tx_internal_free(chest_markers);
    if (tile_markers) tx_internal_free(tile_markers);

    {
        uint8_t* png_data = (uint8_t*)(uintptr_t)tx_last_ptr;
        uint32_t png_len = tx_last_len;
        uint32_t png_w = tx_last_width;
        uint32_t png_h = tx_last_height;

        buf_cstr(response, "{\"status\":\"ok\",\"matched_chest_count\":");
        json_u32(response, matched_chests);
        buf_cstr(response, ",\"matched_tile_count\":");
        json_u32(response, matched_tiles);
        buf_cstr(response, ",\"width\":");
        json_u32(response, png_w);
        buf_cstr(response, ",\"height\":");
        json_u32(response, png_h);
        buf_cstr(response, ",\"thumbnail_png_bytes\":");
        json_u32(response, png_len);
        buf_u8(response, '}');
        return finish_preview_png_response(w, response, png_data, png_len, png_w, png_h);
    }
}

/* ====================================================================
 * Operation: mark_tiles_and_chests_map
 * Generates a marked map and retains its bytes for terra_op_get_map.
 * ==================================================================== */

static int execute_mark_tiles_and_chests_map(TxWorld* w, const char* request, int jlen,
                                             TxBuf* response) {
    /* Parse output_dir */
    char output_dir[512] = {0};
    int p = json_find_key(request, jlen, "output_dir");
    if (p >= 0 && !json_extract_str(request, jlen, p, output_dir, sizeof(output_dir))) {
        tx_set_error("TERRAX_VALIDATION_ERROR", "operation output_dir must be a string under 512 bytes");
        return -1;
    }
    if (!validate_operation_output_path(output_dir, NULL)) return -1;

    MapMarkerEntry* chest_markers = NULL;
    MapMarkerEntry* tile_markers = NULL;
    uint32_t chest_count = 0u;
    uint32_t tile_count = 0u;
    if (!parse_marker_array(
            request, jlen, "chest_markers", "item_id", &chest_markers, &chest_count)) {
        return -1;
    }
    if (!parse_marker_array(
            request, jlen, "tile_markers", "tile_type", &tile_markers, &tile_count)) {
        if (chest_markers) tx_internal_free(chest_markers);
        return -1;
    }

    if (chest_count == 0 && tile_count == 0) {
        if (chest_markers) tx_internal_free(chest_markers);
        if (tile_markers) tx_internal_free(tile_markers);
        tx_set_error("TERRAX_VALIDATION_ERROR",
                     "at least one of chest_markers or tile_markers required");
        return -1;
    }

    /* Generate map with multi-color markers */
    uint32_t matched_chests = 0u;
    uint32_t matched_tiles = 0u;
    int32_t result = terra_render_lit_map_marked(w, chest_markers, chest_count,
                                                  tile_markers, tile_count,
                                                  &matched_chests, &matched_tiles);
    if (chest_markers) tx_internal_free(chest_markers);
    if (tile_markers) tx_internal_free(tile_markers);
    if (result < 0) return -1;

    uint8_t* map_data = (uint8_t*)(uintptr_t)tx_last_ptr;
    uint32_t map_len = tx_last_len;
    uint32_t map_w = tx_last_width;
    uint32_t map_h = tx_last_height;

    /* Write map file to output_dir */
    int wrote_file = 0;
    if (output_dir[0] && map_data && map_len > 0) {
        char map_path[768];
        uint32_t pos = 0;
        for (uint32_t i = 0; output_dir[i] && pos < sizeof(map_path) - 32; i++)
            map_path[pos++] = output_dir[i];
        if (pos > 0 && map_path[pos-1] != '/' && map_path[pos-1] != '\\')
            map_path[pos++] = '/';
        const char* fname = "marked_world.map";
        for (uint32_t i = 0; fname[i] && pos < sizeof(map_path) - 1; i++)
            map_path[pos++] = fname[i];
        map_path[pos] = 0;
        wrote_file = write_file_from_heap(map_path, map_data, map_len);
    }

    buf_cstr(response, "{\"status\":\"ok\",\"matched_chest_count\":");
    json_u32(response, matched_chests);
    buf_cstr(response, ",\"matched_tile_count\":");
    json_u32(response, matched_tiles);
    buf_cstr(response, ",\"width\":");
    json_u32(response, map_w);
    buf_cstr(response, ",\"height\":");
    json_u32(response, map_h);
    buf_cstr(response, ",\"map_bytes\":");
    json_u32(response, map_len);
    buf_cstr(response, ",\"file_written\":");
    buf_cstr(response, wrote_file ? "true" : "false");
    buf_u8(response, '}');
    return finish_map_response(response, map_data, map_len, map_w, map_h);
}

/* ====================================================================
 * Operation: apply_pixel_art_mapping
 * Placeholder.
 * ==================================================================== */

static int execute_apply_pixel_art_mapping(TxWorld* w, const char* request, int jlen,
                                           TxBuf* response) {
    (void)w; (void)request; (void)jlen; (void)response;
    tx_set_error("TERRAX_DEPRECATED",
                 "apply_pixel_art_mapping is removed. Use txw_queue_pixel_art + terra_world_save instead.");
    return -1;
}

/* ====================================================================
 * Main operation dispatcher
 * ==================================================================== */

/* Inline string comparison to avoid cross-file linkage issues */
static int op_streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

int op_execute_json(TxWorld* w, const char* op_name, const char* request,
                    TxBuf* response) {
    /* Debug: report operation name in response for diagnostics */
    if (!op_name || !request || tx_strlen(op_name) == 0) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "empty operation name");
        return -1;
    }

    int jlen = (int)tx_strlen(request);
    if (!json_validate_document(request, jlen)) {
        tx_set_error("TERRAX_VALIDATION_ERROR", "operation request is not valid JSON");
        return -1;
    }

    if (op_streq(op_name, "render_preview_png"))
        return execute_txw_render_preview_png(w, request, jlen, response);
    if (op_streq(op_name, "render_thumbnail_png"))
        return execute_txw_render_thumbnail_png(w, request, jlen, response);
    if (op_streq(op_name, "render_preview_rgba"))
        return execute_txw_render_preview_rgba(w, request, jlen, response);
    if (op_streq(op_name, "render_lit_map"))
        return execute_render_lit_map(w, request, jlen, response);
    if (op_streq(op_name, "mark_chest_items_preview"))
        return execute_mark_chest_items_preview(w, request, jlen, response);
    if (op_streq(op_name, "mark_chest_items_map"))
        return execute_mark_chest_items_map(w, request, jlen, response);
    if (op_streq(op_name, "mark_tiles_and_chests_preview"))
        return execute_mark_tiles_and_chests_preview(w, request, jlen, response);
    if (op_streq(op_name, "mark_tiles_and_chests_map"))
        return execute_mark_tiles_and_chests_map(w, request, jlen, response);
    if (op_streq(op_name, "convert_world_biome"))
        return execute_batch_update_tiles(w, request, jlen, response);
    if (op_streq(op_name, "batch_update_tiles"))
        return execute_batch_update_tiles(w, request, jlen, response);
    if (op_streq(op_name, "set_visibility"))
        return execute_set_visibility(w, request, jlen, response);
    if (op_streq(op_name, "remove_all_wires"))
        return execute_remove_all_wires(w, request, jlen, response);
    if (op_streq(op_name, "header_patch"))
        return tx_mutate_header_patch(w, request, (uint32_t)jlen, response);
    if (op_streq(op_name, "replace_chests"))
        return tx_mutate_replace_chests(w, request, (uint32_t)jlen, response);
    if (op_streq(op_name, "replace_bestiary"))
        return tx_mutate_replace_bestiary(w, request, (uint32_t)jlen, response);
    if (op_streq(op_name, "unlock_bestiary"))
        return execute_unlock_bestiary(w, request, jlen, response);
    if (op_streq(op_name, "apply_pixel_art_mapping"))
        return execute_apply_pixel_art_mapping(w, request, jlen, response);

    /* Include operation name in error for debugging */
    {
        char err_msg[128];
        uint32_t olen = tx_strlen(op_name);
        const char* prefix = "unknown op: ";
        uint32_t i = 0;
        for (i = 0; prefix[i] && i < 127; i++) err_msg[i] = prefix[i];
        for (uint32_t j = 0; op_name[j] && i < 127; j++, i++) err_msg[i] = op_name[j];
        err_msg[i] = 0;
        tx_set_error("TERRAX_UNKNOWN_OPERATION", err_msg);
    }
    return -1;
}
