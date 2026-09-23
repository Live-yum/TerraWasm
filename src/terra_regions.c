#include "terra_regions.h"
#include "terra_output.h"
#include <string.h>

extern uint8_t* tx_alloc(uint32_t);
extern void tx_internal_free(void*);
extern void tx_set_error(const char*, const char*);
#include "terra_region_cells.inc"

/* Keep only the 169 columns needed by SceneMetrics, not a full classified
 * world. Small masks use 1/2/4/8 bits; larger combinations use exact bits. */
typedef struct {
    uint32_t* columns;
    uint16_t* rows;
    uint16_t* packing;
    uint8_t *mask, *low_mask;
    uint16_t previous_wall;
    uint8_t metrics[METRIC_COUNT], metric_count;
    uint16_t desert_threshold;
    uint32_t active_tiles, needed_counts, next_column;
} RegionScan;

static uint16_t read_region(const uint8_t* mask, uint32_t i, uint8_t bits) {
    if (!bits) return 0;
    if (bits > 8) {
        uint32_t bit = (i % 8u) * bits, at = (i / 8u) * bits + bit / 8u;
        uint32_t value = mask[at] | (uint32_t)mask[at+1] << 8 | (uint32_t)mask[at+2] << 16;
        return (value >> (bit % 8u)) & ((1u << bits) - 1u);
    }
    uint32_t cells = 8u / bits;
    return (mask[i / cells] >> ((i % cells) * bits)) & ((1u << bits) - 1u);
}

static void write_region(uint8_t* mask, uint32_t i, uint8_t bits, uint16_t value) {
    if (!bits) return;
    if (bits > 8) {
        uint32_t bit = (i % 8u) * bits, at = (i / 8u) * bits + bit / 8u;
        uint32_t shifted = (uint32_t)value << (bit % 8u);
        mask[at] |= shifted; mask[at+1] |= shifted >> 8; mask[at+2] |= shifted >> 16;
        return;
    }
    uint32_t cells = 8u / bits;
    mask[i / cells] |= value << ((i % cells) * bits);
}

static uint32_t needed_metrics(uint16_t regions) {
    if (regions & R_FOREST) return (1u << METRIC_COUNT) - 1u;
    uint32_t metrics = 0;
    if (regions & R_DUNGEON) metrics |= C(DUNGEON);
    if (regions & R_JUNGLE) metrics |= C(JUNGLE);
    if (regions & R_DESERT) metrics |= C(DESERT);
    if (regions & R_SNOW) metrics |= C(SNOW);
    if (regions & (R_CORRUPT|R_CRIMSON|R_HALLOW)) metrics |= C(CORRUPT)|C(CRIMSON)|C(HALLOW)|C(FLOWERS);
    if (regions & R_MUSHROOM) metrics |= C(MUSHROOM);
    return metrics;
}

static uint16_t normal_regions(const TxWorld* w, const int* n, uint32_t cell, uint32_t x, uint32_t y, int threshold, int desert_threshold) {
    int flowers = n[FLOWERS] * (w->infectedSeed ? 30 : 10);
    int evil = n[CORRUPT] > flowers ? n[CORRUPT] - flowers : 0;
    int blood = n[CRIMSON] > flowers ? n[CRIMSON] - flowers : 0;
    uint16_t region = 0;
    if (n[DUNGEON] >= 250 && (cell & (C(DUNGEON)|DUNGEON_WALL))) region |= R_DUNGEON;
    if ((n[JUNGLE] >= 140 && (int32_t)y <= w->maxTilesY - 200) || (cell & TEMPLE)) region |= R_JUNGLE;
    if (n[DESERT] >= desert_threshold) region |= R_DESERT;
    if (n[SNOW] >= threshold) region |= R_SNOW;
    if (evil - n[HALLOW] >= 300) region |= R_CORRUPT;
    if (blood - n[HALLOW] >= 300) region |= R_CRIMSON;
    if (n[HALLOW] - evil - blood >= 125) region |= R_HALLOW;
    if (n[MUSHROOM] >= 100) region |= R_MUSHROOM;
    if (ocean_at(w,x,y)) region |= R_OCEAN;
    if ((int32_t)y > w->maxTilesY - 200) region |= R_HELL;
    if (y <= w->worldSurface * 0.3499999940395355) region |= R_SPACE;
    if (y > w->worldSurface && y <= w->rockLayer) region |= R_UNDERGROUND;
    if (y > w->rockLayer && (int32_t)y <= w->maxTilesY - 200) region |= R_CAVERN;
    /* Forest is the neutral overworld, excluding other biomes, meteorites
     * and graveyards. Layers deliberately overlap material-based biomes. */
    if (y <= w->worldSurface && !region && n[METEOR] < 75 && n[GRAVES] - n[FLOWERS]/2 < 28) region |= R_FOREST;
    return region;
}

static uint16_t dual_regions(uint16_t region, uint32_t cell, uint16_t candidate) {
    region &= ~(R_JUNGLE|R_SNOW|R_CORRUPT|R_CRIMSON|R_HALLOW|R_MUSHROOM);
    if (!(cell & DUNGEON_WALL)) candidate &= ~R_DUNGEON;
    if (!(cell & DESERT_WALL)) candidate &= ~R_DESERT;
    if (cell & TEMPLE) region |= R_JUNGLE;
    return region | (candidate & ~DUAL_STOP);
}

static void count_column(RegionScan* scan, const uint32_t* column, uint32_t height, int delta) {
    for (uint32_t y=0; y<height; y++) {
        uint16_t* row = scan->rows + y * METRIC_COUNT;
        for (uint32_t k=0; k<scan->metric_count; k++) {
            uint8_t m = scan->metrics[k];
            row[m] += delta * ((column[y] & C(m)) != 0);
        }
    }
}

static void emit_column(TxWorld* w, RegionScan* scan, uint32_t x) {
    uint32_t height = (uint32_t)w->maxTilesY;
    const uint32_t* column = scan->columns + (x % 169u) * height;
    int counts[METRIC_COUNT] = {0};
    uint32_t first = height > 63 ? height - 63 : 0, next = height;
    uint16_t candidate = 0;
    for (uint32_t y=first; y<height; y++)
        for (uint32_t k=0; k<scan->metric_count; k++) {
            uint8_t m = scan->metrics[k]; counts[m] += scan->rows[y * METRIC_COUNT + m];
        }
    for (uint32_t y=height; y-- > 0;) {
        if (column[y] >> 16) { candidate = column[y] >> 16; next = y; }
        int dual = w->dualdungeonsSeed && y > w->worldSurface && (int32_t)y <= w->maxTilesY - 200;
        uint16_t region = normal_regions(w,counts,column[y],x,y,1500,scan->desert_threshold);
        if (dual) region = dual_regions(region,column[y],next-y < 300 ? candidate : 0);
        write_region(scan->mask,x*height+y,w->region_mask_bits,scan->packing[region]);
        if (scan->low_mask) {
            region = normal_regions(w,counts,column[y],x,y,300,300);
            if (dual) region = dual_regions(region,column[y],next-y < 300 ? candidate : 0);
            write_region(scan->low_mask,x*height+y,w->region_mask_bits,scan->packing[region]);
        }
        for (uint32_t k=0; k<scan->metric_count; k++) {
            uint8_t m = scan->metrics[k];
            if (y+61 < height) counts[m] -= scan->rows[(y+61)*METRIC_COUNT+m];
            if (y>=63) counts[m] += scan->rows[(y-63)*METRIC_COUNT+m];
        }
    }
    if (x >= 84) count_column(scan,scan->columns+((x-84)%169u)*height,height,-1);
    scan->next_column = x+1;
}

static int classify_run(TxWorld* w, uint32_t x, uint32_t y, TxTile* t, uint32_t run, void* context) {
    RegionScan* scan = context;
    uint32_t flags = t->active ? tile_counts(w,t->type) & scan->needed_counts : 0;
    if ((t->wall>=7 && t->wall<=9) || (t->wall>=17 && t->wall<=19) || (t->wall>=94 && t->wall<=105)) flags |= DUNGEON_WALL;
    if (t->wall==87 || (t->active && t->type==226)) flags |= TEMPLE;
    /* Include the safe counterparts for editing, as for dungeon entrances. */
    if (desert_wall(t->wall)) flags |= DESERT_WALL;
    uint32_t height = (uint32_t)w->maxTilesY;
    uint32_t* column = scan->columns + (x%169u)*height;
    if (t->active) scan->active_tiles += run;
    int ocean_sand = t->type==53 || t->type==396 || t->type==397 || t->type==400 || t->type==401 || t->type==403;
    for (uint32_t dy=0; dy<run; dy++) {
        uint32_t row = y+dy, value = flags;
        if (ocean_sand && ocean_at(w,x,row)) value &= ~C(DESERT);
        if (w->dualdungeonsSeed && x>=1 && x<(uint32_t)w->maxTilesX-1u && row>=1 && row<height-1u)
            value |= (uint32_t)dual_candidate(t,dy ? t->wall : (y ? scan->previous_wall : 0),w->rockLayer,row) << 16;
        column[row] = value;
    }
    scan->previous_wall = t->wall;
    if (y+run == height) {
        count_column(scan,column,height,1);
        if (x>=84) emit_column(w,scan,x-84);
    }
    return 1;
}

static uint16_t compile_regions(TxWorld* w, TxTileRule* rules, uint32_t count, uint16_t* packing) {
    uint16_t requested = 0, bits[TX_REGION_COUNT] = {0};
    for (uint32_t r=0; r<count; r++) {
        if (rules[r].biome_region > 0) requested |= 1u << (rules[r].biome_region-1);
        if (rules[r].exclude_biome_region > 0) requested |= 1u << (rules[r].exclude_biome_region-1);
    }
    uint8_t used = 0;
    const uint16_t geometry = R_OCEAN|R_HELL|R_SPACE|R_UNDERGROUND|R_CAVERN;
    for (uint32_t id=0; id<TX_REGION_COUNT; id++)
        if ((requested & (1u<<id)) && !(geometry & (1u<<id))) bits[id] = 1u << used++;
    w->region_mask_bits = used<=2 ? used : used<=4 ? 4 : used<=8 ? 8 : used;
    for (uint32_t id=0; id<TX_REGION_COUNT; id++)
        if (requested & geometry & (1u<<id)) bits[id] = 1u << used++;
    const uint8_t geometry_ids[] = {8,9,11,12,13};
    for (uint32_t k=0; k<5; k++) w->region_geometry[k] = bits[geometry_ids[k]];
    for (uint32_t r=0; r<count; r++) {
        rules[r].biome_region_bit = rules[r].biome_region>0 ? bits[rules[r].biome_region-1] : 0;
        rules[r].exclude_biome_region_bit = rules[r].exclude_biome_region>0 ? bits[rules[r].exclude_biome_region-1] : 0;
    }
    packing[0] = 0;
    for (uint32_t id=0; id<TX_REGION_COUNT; id++)
        for (uint32_t i=0; i<(1u<<id); i++)
            packing[(1u<<id)|i] = packing[i] | ((geometry & (1u<<id)) ? 0 : bits[id]);
    return requested;
}

int tx_regions_build(TxWorld* w, TxTileRule* rules, uint32_t count) {
    uint32_t needed=0, width=(uint32_t)w->maxTilesX, height=(uint32_t)w->maxTilesY;
    uint8_t surface_sand_split = 0, protect_desert_edges = 0;
    for (uint32_t r=0; r<count; r++) {
        needed |= rules[r].biome_region>0 || rules[r].exclude_biome_region>0;
        surface_sand_split |= rules[r].terrain_theme == 1;
        protect_desert_edges |= rules[r].exclude_biome_region == 3;
    }
    if (!needed) { w->surface_sand_split=surface_sand_split; return 1; }
    if (!width || !height || width>UINT32_MAX/height || height>UINT32_MAX/(169u*4u)) return 0;
    RegionScan scan = {0}; TxBuf points = {0}; int ok = 0;
    scan.desert_threshold = protect_desert_edges ? 300 : 1500;
    scan.packing = (uint16_t*)tx_alloc((1u<<TX_REGION_COUNT)*2u);
    if (!scan.packing) goto cleanup;
    uint16_t requested = compile_regions(w,rules,count,scan.packing);
    /* Two trailing bytes make the unaligned 3-byte reads safe at the end. */
    uint64_t bytes64 = ((uint64_t)width*height*w->region_mask_bits+7u)/8u + 2u;
    if (bytes64>UINT32_MAX) goto cleanup;
    uint32_t bytes = (uint32_t)bytes64;
    scan.mask = tx_alloc(bytes);
    scan.rows = (uint16_t*)tx_alloc(height*METRIC_COUNT*2u);
    scan.columns = (uint32_t*)tx_alloc(169u*height*4u);
    if (!scan.mask || !scan.rows || !scan.columns) goto cleanup;
    memset(scan.mask,0,bytes); memset(scan.rows,0,height*METRIC_COUNT*2u);
    memset(scan.columns,0,169u*height*4u);
    if (w->skyblockWorld && (requested & (R_DESERT|R_SNOW|R_FOREST))) {
        scan.low_mask=tx_alloc(bytes);
        if (!scan.low_mask) goto cleanup;
        memset(scan.low_mask,0,bytes);
    }
    scan.needed_counts = needed_metrics(requested);
    for (uint32_t m=0; m<METRIC_COUNT; m++)
        if (scan.needed_counts & C(m)) scan.metrics[scan.metric_count++]=(uint8_t)m;
    if (scan.metric_count && !tx_scan_tile_markers(w,NULL,0,&points,classify_run,&scan)) goto cleanup;
    while (scan.metric_count && scan.next_column<width) emit_column(w,&scan,scan.next_column);
    if (scan.low_mask && (double)scan.active_tiles/((double)width*height)<0.1) {
        tx_internal_free(scan.mask); scan.mask=scan.low_mask; scan.low_mask=NULL;
    }
    w->region_mask=scan.mask; scan.mask=NULL; ok=1;
cleanup:
    if (scan.columns) tx_internal_free(scan.columns);
    if (scan.rows) tx_internal_free(scan.rows);
    if (scan.packing) tx_internal_free(scan.packing);
    if (scan.mask) tx_internal_free(scan.mask);
    if (scan.low_mask) tx_internal_free(scan.low_mask);
    if (points.data) tx_internal_free(points.data);
    if (!ok) tx_set_error("TERRAX_REGION_FAILED","region classification failed or ran out of memory");
    w->surface_sand_split = ok ? surface_sand_split : 0;
    return ok;
}

uint16_t tx_region_at(const TxWorld* w, uint32_t x, uint32_t y) {
    if (!w->region_mask) return 0;
    uint16_t region = read_region(w->region_mask,x*(uint32_t)w->maxTilesY+y,w->region_mask_bits);
    /* Coordinate-only regions need no per-tile storage. */
    if (w->region_geometry[0] && ocean_at(w,x,y)) region |= w->region_geometry[0];
    if ((int32_t)y > w->maxTilesY - 200) region |= w->region_geometry[1];
    if (y <= w->worldSurface * 0.3499999940395355) region |= w->region_geometry[2];
    if (y > w->worldSurface && y <= w->rockLayer) region |= w->region_geometry[3];
    if (y > w->rockLayer && (int32_t)y <= w->maxTilesY - 200) region |= w->region_geometry[4];
    return region;
}

uint32_t tx_region_run(const TxWorld* w, uint32_t x, uint32_t y, uint32_t run) {
    /* The desert theme uses the surface boundary even without a region mask. */
    if (w->surface_sand_split && y <= w->worldSurface && y + run > (uint32_t)w->worldSurface + 1u)
        run = (uint32_t)w->worldSurface + 1u - y;
    if (!w->region_mask) return run;
    uint16_t region=tx_region_at(w,x,y);
    uint32_t length=1;
    while (length<run && tx_region_at(w,x,y+length)==region) length++;
    return length;
}
