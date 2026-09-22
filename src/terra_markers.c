#include "terra_map.h"
#include "terra_output.h"
#include <string.h>

extern uint8_t* tx_alloc(uint32_t);
extern void tx_internal_free(void*);
extern void buf_init(TxBuf*, uint32_t);
extern void buf_bytes(TxBuf*, const void*, uint32_t);
extern int read_tile_at(TxWorld*, uint32_t*, uint32_t, TxTile*);
extern void tx_set_error(const char*, const char*);
extern uint32_t tx_mark(void);

typedef struct MarkerRun { uint32_t start, end, label; } MarkerRun;
typedef struct MarkerNode { uint32_t parent, marker; int32_t x, y; } MarkerNode;

static uint32_t marker_root(MarkerNode* nodes, uint32_t label) {
    while (nodes[label].parent != label) {
        nodes[label].parent = nodes[nodes[label].parent].parent;
        label = nodes[label].parent;
    }
    return label;
}

static int marker_frame_matches(int16_t frame, int32_t expected, int32_t modulo) {
    return expected < 0 || (frame >= 0 && (modulo ? frame % modulo : frame) == expected);
}

static int append_marker_point(TxBuf* points, int32_t x, int32_t y, uint32_t marker) {
    TxMarkerPoint point = { x, y, marker };
    buf_bytes(points, &point, sizeof(point));
    return points->ok;
}

/* WLD is column-major RLE. Components are joined against the previous column
 * (including diagonal contacts), then retired as soon as they leave the frontier.
 * Memory is O(world height + resulting points), never a full tile grid. */
int tx_scan_tile_markers(TxWorld* w, const MapMarkerEntry* markers,
                          uint32_t count, TxBuf* points, TxTileVisitor visit, void* context) {
    uint32_t enabled = 0u, clustered = 0u;
    memset(points, 0, sizeof(*points));
    for (uint32_t i = 0; i < count; i++) {
        enabled |= markers[i].locate != 0;
        clustered |= markers[i].locate == 2;
    }
    if (!enabled && !visit) return 1;
    uint32_t height = (uint32_t)w->maxTilesY, width = (uint32_t)w->maxTilesX;
    if (!height || !width || height > UINT32_MAX / (2u * sizeof(MarkerNode))) return 0;
    MarkerRun *prev = NULL, *curr = NULL;
    MarkerNode *nodes = NULL, *next = NULL;
    uint32_t* remap = NULL;
    uint16_t* by_type = NULL;
    uint16_t next_marker[256];
    uint32_t type_count = 0u;
    uint32_t prev_count = 0u, node_count = 0u;
    uint8_t* saved_file = w->file;
    uint32_t saved_len = w->file_len, off = w->starts[1], end = w->ends[1];
    int ok = 0;
    buf_init(points, 1024u);
    if (!points->ok) goto cleanup;
    for (uint32_t m = 0; m < count; m++)
        if (markers[m].locate && (uint32_t)markers[m].id >= type_count) type_count = (uint32_t)markers[m].id + 1u;
    by_type = (uint16_t*)tx_alloc((type_count ? type_count : 1u) * sizeof(uint16_t));
    if (!by_type) goto cleanup;
    memset(by_type, 0xff, type_count * sizeof(uint16_t));
    for (uint32_t m = count; m-- > 0u;) {
        if (!markers[m].locate) continue;
        next_marker[m] = by_type[markers[m].id];
        by_type[markers[m].id] = (uint16_t)m;
    }
    if (clustered) {
        prev = (MarkerRun*)tx_alloc(height * sizeof(MarkerRun));
        curr = (MarkerRun*)tx_alloc(height * sizeof(MarkerRun));
        nodes = (MarkerNode*)tx_alloc(2u * height * sizeof(MarkerNode));
        next = (MarkerNode*)tx_alloc(height * sizeof(MarkerNode));
        remap = (uint32_t*)tx_alloc(2u * height * sizeof(uint32_t));
        if (!prev || !curr || !nodes || !next || !remap) goto cleanup;
    }
    if (w->section_overrides[1].active) {
        w->file = w->section_overrides[1].data;
        w->file_len = w->section_overrides[1].len;
        off = 0u;
        end = w->file_len;
    }
    for (uint32_t x = 0; x < width; x++) {
        uint32_t curr_count = 0u;
        for (uint32_t y = 0; y < height;) {
            TxTile tile;
            if (!read_tile_at(w, &off, end, &tile) || (uint32_t)tile.same + 1u > height - y) {
                tx_set_error("TERRAX_BAD_TILE_STREAM", "invalid run during entity location");
                goto cleanup;
            }
            uint32_t run = (uint32_t)tile.same + 1u;
            if (visit && !visit(w, x, y, &tile, run, context)) goto cleanup;
            for (uint32_t m = tile.active && tile.type < type_count ? by_type[tile.type] : UINT16_MAX;
                 m != UINT16_MAX; m = next_marker[m]) {
                const MapMarkerEntry* marker = &markers[m];
                if (!marker->locate || marker->id != tile.type ||
                    !marker_frame_matches(tile.frame_x, marker->frame_x, marker->frame_x_mod) ||
                    !marker_frame_matches(tile.frame_y, marker->frame_y, marker->frame_y_mod)) continue;
                if (marker->locate == 1) {
                    for (uint32_t dy = 0; dy < run; dy++)
                        if (!append_marker_point(points, x, y + dy, m)) goto cleanup;
                } else {
                    /* One matching cluster selector per tile; the API validates
                     * duplicate cluster types before scanning. */
                    if (curr_count && curr[curr_count - 1u].end + 1u == y &&
                        nodes[curr[curr_count - 1u].label].marker == m) {
                        curr[curr_count - 1u].end += run;
                    } else {
                        nodes[node_count] = (MarkerNode){node_count, m, (int32_t)x, (int32_t)y};
                        curr[curr_count++] = (MarkerRun){y, y + run - 1u, node_count++};
                    }
                }
            }
            y += run;
        }
        if (!clustered) continue;
        uint32_t start = 0u;
        for (uint32_t c = 0; c < curr_count; c++) {
            while (start < prev_count && prev[start].end + 1u < curr[c].start) start++;
            for (uint32_t p = start; p < prev_count && prev[p].start <= curr[c].end + 1u; p++) {
                uint32_t a = marker_root(nodes, curr[c].label), b = marker_root(nodes, prev[p].label);
                if (a == b || nodes[a].marker != nodes[b].marker) continue;
                /* Deterministic anchor: first actual ore cell in scan order. */
                if (nodes[b].x < nodes[a].x || (nodes[b].x == nodes[a].x && nodes[b].y < nodes[a].y)) {
                    nodes[a].x = nodes[b].x; nodes[a].y = nodes[b].y;
                }
                nodes[b].parent = a;
            }
        }
        memset(remap, 0xff, node_count * sizeof(uint32_t));
        uint32_t next_count = 0u;
        for (uint32_t c = 0; c < curr_count; c++) {
            uint32_t root = marker_root(nodes, curr[c].label);
            if (remap[root] == UINT32_MAX) {
                remap[root] = next_count;
                next[next_count] = nodes[root];
                next[next_count].parent = next_count;
                next_count++;
            }
            curr[c].label = remap[root];
        }
        for (uint32_t n = 0; n < node_count; n++) {
            if (nodes[n].parent == n && remap[n] == UINT32_MAX &&
                !append_marker_point(points, nodes[n].x, nodes[n].y, nodes[n].marker)) goto cleanup;
        }
        memcpy(nodes, next, next_count * sizeof(MarkerNode));
        MarkerRun* swap = prev; prev = curr; curr = swap;
        prev_count = curr_count; node_count = next_count;
    }
    for (uint32_t n = 0; n < node_count; n++)
        if (!append_marker_point(points, nodes[n].x, nodes[n].y, nodes[n].marker)) goto cleanup;
    ok = 1;
cleanup:
    w->file = saved_file; w->file_len = saved_len;
    if (prev) tx_internal_free(prev);
    if (curr) tx_internal_free(curr);
    if (nodes) tx_internal_free(nodes);
    if (next) tx_internal_free(next);
    if (remap) tx_internal_free(remap);
    if (by_type) tx_internal_free(by_type);
    if (!ok) {
        if (points->data) tx_internal_free(points->data);
        memset(points, 0, sizeof(*points));
        tx_set_error("TERRAX_ENTITY_LOCATION_FAILED", "entity stream is invalid or location memory exhausted");
    }
    return ok;
}

int tx_locate_tile_markers(TxWorld* w, const MapMarkerEntry* markers,
                          uint32_t count, TxBuf* points) {
    uint32_t key_bytes = count * sizeof(MapMarkerEntry);
    TxBuf* cache = &w->entity_marker_cache;
    memset(points, 0, sizeof(*points));
    if (!count) return 1;
    const TxPreparedOutput* prepared = w->prepared_output;
    if (prepared && prepared->ready && prepared->marker_count == count &&
        memcmp(prepared->markers, markers, key_bytes) == 0) {
        buf_init(points, prepared->points.len);
        if (prepared->points.len) buf_bytes(points, prepared->points.data, prepared->points.len);
        return points->ok;
    }
    if (cache->data && w->entity_marker_key_bytes == key_bytes &&
        memcmp(cache->data, markers, key_bytes) == 0) {
        buf_init(points, cache->len - key_bytes);
        buf_bytes(points, cache->data + key_bytes, cache->len - key_bytes);
        if (points->ok) return 1;
        if (points->data) tx_internal_free(points->data);
        memset(points, 0, sizeof(*points));
        tx_set_error("TERRAX_WASM_OOM", "entity point cache copy failed");
        return 0;
    }
    if (cache->data) tx_internal_free(cache->data);
    memset(cache, 0, sizeof(*cache));
    w->entity_marker_key_bytes = 0u;
    if (!tx_scan_tile_markers(w, markers, count, points, NULL, NULL)) return 0;
    /* ponytail: bound retained points to 2 MiB; larger selections rescan.
     * Tile overrides and world close invalidate this cache. */
    if (points->len <= 2u * 1024u * 1024u - key_bytes) {
        cache->data = tx_alloc(key_bytes + points->len);
        if (cache->data) {
            memcpy(cache->data, markers, key_bytes);
            if (points->len) memcpy(cache->data + key_bytes, points->data, points->len);
            cache->len = cache->cap = key_bytes + points->len;
            cache->ok = 1;
            w->entity_marker_key_bytes = key_bytes;
            w->heap_mark = tx_mark();
        }
    }
    return 1;
}
