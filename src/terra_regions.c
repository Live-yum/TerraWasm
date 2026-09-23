#include "terra_regions.h"
#include "terra_output.h"
#include <string.h>

extern uint8_t* tx_alloc(uint32_t);
extern void tx_internal_free(void*);
extern void tx_set_error(const char*, const char*);

enum { JUNGLE_TILE=1, DUNGEON_TILE=2, DUNGEON_WALL=4, TEMPLE=8 };
typedef struct { uint8_t* cells; uint16_t previous_wall; } RegionScan;

static int jungle_wall(uint16_t wall) {
    return wall == 15 || wall == 64 || (wall >= 204 && wall <= 207);
}

/* Terraria 1.4.5.8 NPCSpawningFlagsForDualDungeons, filtered through
 * SolidTile3: plants, vines and platforms cannot stop the downward search.
 * 0=continue, 1=other biome, 2=jungle, 3=dungeon, 4=temple. */
static uint8_t dual_candidate(const TxTile* t, uint16_t above, double rock, uint32_t y) {
    if (!t->active || t->inactive) return 0;
    switch (t->type) {
    case 60: case 225: case 383: case 384: return 2;
    case 41: case 43: case 44: case 481: case 482: case 483: return y > rock ? 3 : 1;
    case 226: return 4;
    case 59: case 120: return jungle_wall(above) ? 2 : 0;
    case 1: case 38: return above == 349 ? 1 : 0;
    case 123: return above == 349 ? 1 : jungle_wall(above) ? 2 : above == 87 ? 4 : 0;
    case 22: case 140:
        return above == 69 || above == 217 || above == 220 || above == 3 || above == 233;
    case 204: case 347:
        return above == 81 || above == 218 || above == 221 || above == 83 || above == 77;
    case 53: case 396: case 397:
        return above == 187 || (above >= 216 && above <= 223) || above == 275 ||
               (above >= 304 && above <= 310);
    case 109: case 116: case 117: case 118: case 164: case 385: case 402: case 403: case 492:
    case 147: case 148: case 161: case 162: case 206: case 224: case 191:
    case 23: case 25: case 112: case 398: case 400: case 474: case 661: case 163: case 200:
    case 195: case 199: case 203: case 234: case 399: case 401: case 662: case 70: return 1;
    default: return 0;
    }
}

static int classify_run(TxWorld* w, uint32_t x, uint32_t y, TxTile* t, uint32_t run, void* context) {
    RegionScan* scan = context;
    uint8_t flags = 0;
    if (t->wall == 7 || t->wall == 8 || t->wall == 9 || (t->wall >= 94 && t->wall <= 99)) flags |= DUNGEON_WALL;
    if (t->wall == 87 || (t->active && t->type == 226)) flags |= TEMPLE;
    if (t->active) {
        switch (t->type) {
        case 60: case 61: case 62: case 74: case 225: flags |= JUNGLE_TILE; break;
        case 226: if (!w->remixWorld) flags |= JUNGLE_TILE; break;
        case 41: case 43: case 44: case 481: case 482: case 483: flags |= DUNGEON_TILE; break;
        }
    }
    uint8_t* column = scan->cells + x * (uint32_t)w->maxTilesY;
    memset(column + y, flags, run);
    if (w->dualdungeonsSeed && x >= 1 && x < (uint32_t)w->maxTilesX - 1u) {
        for (uint32_t dy = 0; dy < run; dy++) {
            uint32_t row = y + dy;
            if (row >= 1 && row < (uint32_t)w->maxTilesY - 1u)
                column[row] |= dual_candidate(t, dy ? t->wall : (y ? scan->previous_wall : 0), w->rockLayer, row) << 4;
        }
    }
    scan->previous_wall = t->wall;
    return 1;
}

static void set_region(uint8_t* mask, uint32_t index, uint8_t value) {
    uint32_t shift = (index & 3u) * 2u;
    mask[index >> 2] = (mask[index >> 2] & ~(3u << shift)) | (value << shift);
}

static void count_column(const uint8_t* column, uint16_t* rows, uint32_t height, int delta) {
    for (uint32_t y = 0; y < height; y++) {
        rows[y * 2] += delta * ((column[y] & JUNGLE_TILE) != 0);
        rows[y * 2 + 1] += delta * ((column[y] & DUNGEON_TILE) != 0);
    }
}

static void dual_regions(TxWorld* w, const uint8_t* column, uint8_t* mask, uint32_t x) {
    uint32_t height = (uint32_t)w->maxTilesY, next = height;
    uint8_t candidate = 0;
    for (uint32_t y = height; y-- > 0;) {
        if (column[y] >> 4) { candidate = column[y] >> 4; next = y; }
        if (y <= w->worldSurface || (int32_t)y > w->maxTilesY - 200) continue;
        uint8_t region = (column[y] & TEMPLE) ? 2 : 0;
        if (next - y < 300) {
            if (candidate == 2 || candidate == 4) region |= 2;
            if (candidate == 3 && (column[y] & DUNGEON_WALL)) region |= 1;
        }
        set_region(mask, x * height + y, region);
    }
}

/* SceneMetrics uses a clipped 169x124 neighborhood. Rolling column/row
 * sums avoid a full TxTile grid or an integral image. The classification
 * pass is only needed for batches that actually select an environment. */
int tx_regions_build(TxWorld* w, const TxTileRule* rules, uint32_t count) {
    uint32_t needed = 0, width = (uint32_t)w->maxTilesX, height = (uint32_t)w->maxTilesY;
    for (uint32_t r = 0; r < count; r++) needed |= rules[r].biome_region > 0;
    if (!needed) return 1;
    if (!width || !height || width > UINT32_MAX / height || height > UINT32_MAX / 4u) return 0;
    uint32_t cells = width * height, bytes = cells / 4u + (cells % 4u != 0);
    uint8_t* classification = tx_alloc(cells);
    uint8_t* mask = tx_alloc(bytes);
    uint16_t* rows = (uint16_t*)tx_alloc(height * 4u);
    TxBuf points = {0};
    int ok = 0;
    if (!classification || !mask || !rows) goto cleanup;
    RegionScan scan = {classification, 0};
    if (!tx_scan_tile_markers(w, NULL, 0, &points, classify_run, &scan)) goto cleanup;
    memset(mask, 0, bytes);
    memset(rows, 0, height * 4u);
    for (uint32_t x = 0; x < 85 && x < width; x++) count_column(classification + x * height, rows, height, 1);
    double dungeon_depth = w->worldSurface;
    if ((w->drunkWorld || w->worldSurface <= 50.0) && w->dungeonY + 40 > dungeon_depth) dungeon_depth = w->dungeonY + 40;
    for (uint32_t x = 0; x < width; x++) {
        uint32_t jungle = 0, dungeon = 0;
        const uint8_t* column = classification + x * height;
        for (uint32_t y = 0; y < 62 && y < height; y++) { jungle += rows[y * 2]; dungeon += rows[y * 2 + 1]; }
        for (uint32_t y = 0; y < height; y++) {
            uint8_t region = (column[y] & TEMPLE) ? 2 : 0;
            if (jungle >= 140 && (int32_t)y <= w->maxTilesY - 200) region |= 2;
            if (dungeon >= 250 && y > dungeon_depth && (column[y] & DUNGEON_WALL)) region |= 1;
            set_region(mask, x * height + y, region);
            if (y >= 62) { jungle -= rows[(y - 62) * 2]; dungeon -= rows[(y - 62) * 2 + 1]; }
            if (y + 62 < height) { jungle += rows[(y + 62) * 2]; dungeon += rows[(y + 62) * 2 + 1]; }
        }
        if (w->dualdungeonsSeed) dual_regions(w, column, mask, x);
        if (x >= 84) count_column(classification + (x - 84) * height, rows, height, -1);
        if (x + 85 < width) count_column(classification + (x + 85) * height, rows, height, 1);
    }
    w->region_mask = mask; mask = NULL; ok = 1;
cleanup:
    if (classification) tx_internal_free(classification);
    if (mask) tx_internal_free(mask);
    if (rows) tx_internal_free(rows);
    if (points.data) tx_internal_free(points.data);
    if (!ok) tx_set_error("TERRAX_REGION_FAILED", "region classification failed or ran out of memory");
    return ok;
}

uint8_t tx_region_at(const TxWorld* w, uint32_t x, uint32_t y) {
    if (!w->region_mask) return 0;
    uint32_t i = x * (uint32_t)w->maxTilesY + y;
    return (w->region_mask[i >> 2] >> ((i & 3u) * 2u)) & 3u;
}

uint32_t tx_region_run(const TxWorld* w, uint32_t x, uint32_t y, uint32_t run) {
    if (!w->region_mask) return run;
    uint8_t region = tx_region_at(w, x, y);
    uint32_t length = 1;
    while (length < run && tx_region_at(w, x, y + length) == region) length++;
    return length;
}
