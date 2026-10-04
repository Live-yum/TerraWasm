#include "terra_output.h"
#include "terra_regions.h"
#include "terra_theme.h"
/*
 * terra_update.c -- Streaming tile modifications with batch updates.
 *
 * All tile modifications are applied as a single streaming pass through the
 * raw .wld tile data. No full tile array is ever materialized.
 */
#include "terra_types.h"

extern void* memset(void* dst, int value, unsigned long n);
extern void* memcpy(void* dst, const void* src, unsigned long n);

extern uint8_t* tx_alloc(uint32_t size);
extern void tx_internal_free(void* ptr);
extern void tx_set_error(const char* code, const char* message);
extern int set_result_buf(TxBuf* b);
extern void buf_init(TxBuf* b, uint32_t cap);
extern int tx_streq_c(const char* a, const char* b);
extern void buf_u8(TxBuf* b, uint8_t v);
extern void buf_bytes(TxBuf* b, const void* p, uint32_t n);
extern void buf_u32le(TxBuf* b, uint32_t v);
extern void buf_cstr(TxBuf* b, const char* s);
extern void json_u32(TxBuf* b, uint32_t v);

extern int read_tile_at(TxWorld* w, uint32_t* off, uint32_t end, TxTile* t);
extern void write_tile(TxWorld* w, TxBuf* b, const TxTile* t, uint32_t same);
extern int tile_important(TxWorld* w, uint16_t type);
extern int same_tile(const TxTile* a, const TxTile* b);

static int init_tile_buffer(TxBuf* buffer, uint32_t base_capacity, const char* message) {
    if (base_capacity > UINT32_MAX - 65536u) {
        tx_set_error("TERRAX_WASM_OOM", "tile buffer size overflow");
        return 0;
    }
    buf_init(buffer, base_capacity + 65536u);
    if (!buffer->ok) {
        tx_set_error("TERRAX_WASM_OOM", message);
        return 0;
    }
    return 1;
}

typedef struct { uint16_t old_id; uint16_t new_id; } BiomeMapping;

static const BiomeMapping PURIFY_TILES[] = {
    {23,2},{109,2},{199,2},{352,2},{24,3},{110,3},{201,3},
    {25,1},{117,1},{195,1},{474,1},{32,2},{112,53},{116,53},{234,53},
    {113,73},{115,52},{205,52},{636,52},{163,161},{164,161},{200,161},
    {398,397},{399,397},{402,397},{400,396},{401,396},{403,396},
    {492,477},{661,60},{662,60},{70,60}};
static const uint32_t PURIFY_TILES_COUNT = sizeof(PURIFY_TILES)/sizeof(PURIFY_TILES[0]);

static const BiomeMapping CORRUPTION_TILES[] = {
    {1,25},{117,25},{195,25},{474,25},{2,23},{109,23},{199,23},{447,23},{492,23},
    {3,24},{73,24},{110,24},{113,24},{201,24},{52,636},{115,636},{205,636},
    {53,112},{116,112},{234,112},{60,661},{662,661},{161,163},{164,163},{200,163},
    {203,25},{204,22},{32,32},{352,32},{69,32},{396,400},{403,400},
    {397,398},{399,398},{402,398}};
static const uint32_t CORRUPTION_TILES_COUNT = sizeof(CORRUPTION_TILES)/sizeof(CORRUPTION_TILES[0]);

static const BiomeMapping CRIMSON_TILES[] = {
    {1,203},{25,203},{117,203},{474,203},{2,199},{23,199},{109,199},
    {3,201},{24,201},{73,201},{110,201},{113,201},{22,204},{32,352},{69,352},
    {52,205},{115,205},{636,205},{53,234},{112,234},{116,234},
    {60,662},{661,662},{161,200},{163,200},{164,200},
    {396,401},{400,401},{403,401},{397,399},{398,399},{402,399}};
static const uint32_t CRIMSON_TILES_COUNT = sizeof(CRIMSON_TILES)/sizeof(CRIMSON_TILES[0]);

static const BiomeMapping HALLOW_TILES[] = {
    {1,117},{25,117},{195,117},{203,117},{474,117},
    {2,109},{23,109},{60,109},{69,109},{199,109},{661,109},{662,109},
    {3,110},{24,110},{201,110},{32,109},{352,109},
    {52,115},{205,115},{636,115},{53,116},{112,116},{234,116},
    {73,113},{161,164},{163,164},{200,164},
    {396,403},{400,403},{401,403},{397,402},{398,402},{399,402}};
static const uint32_t HALLOW_TILES_COUNT = sizeof(HALLOW_TILES)/sizeof(HALLOW_TILES[0]);

static const BiomeMapping PURIFY_WALLS[] = {
    {69,63},{70,63},{81,63},{80,15},
    {3,349},{28,349},{83,349},{246,349},{248,349},{269,349},
    {188,212},{192,212},{189,213},{193,213},{190,214},{194,214},{191,215},{195,215},
    {217,216},{218,216},{219,216},{304,216},{305,216},{306,216},{307,216},
    {220,187},{221,187},{222,187},{275,187},{308,187},{309,187},{310,187},
    {292,204},{293,205},{294,206},{295,207}};
static const uint32_t PURIFY_WALLS_COUNT = sizeof(PURIFY_WALLS)/sizeof(PURIFY_WALLS[0]);

static const BiomeMapping CORRUPTION_WALLS[] = {
    {63,69},{64,69},{65,69},{66,69},{67,69},{68,69},{70,69},{81,69},{264,69},{265,69},{268,69},
    {1,3},{28,3},{61,3},{83,3},{185,3},{246,3},{248,3},{262,3},{269,3},{274,3},{349,3},
    {216,217},{218,217},{219,217},{304,217},{306,217},{307,217},
    {187,220},{221,220},{222,220},{275,220},{309,220},{310,220},
    {192,188},{200,188},{204,188},{212,188},{193,189},{201,189},{205,189},{213,189},
    {194,190},{202,190},{206,190},{214,190},{195,191},{203,191},{207,191},{215,191}};
static const uint32_t CORRUPTION_WALLS_COUNT = sizeof(CORRUPTION_WALLS)/sizeof(CORRUPTION_WALLS[0]);

static const BiomeMapping CRIMSON_WALLS[] = {
    {63,81},{64,81},{65,81},{66,81},{67,81},{68,81},{69,81},{70,81},{264,81},{265,81},{268,81},
    {1,83},{3,83},{28,83},{61,83},{185,83},{246,83},{248,83},{262,83},{274,83},{349,83},
    {216,218},{217,218},{219,218},{304,218},{305,218},{307,218},
    {187,221},{220,221},{222,221},{275,221},{308,221},{310,221},
    {188,192},{200,192},{204,192},{212,192},{189,193},{201,193},{205,193},{213,193},
    {190,194},{202,194},{206,194},{214,194},{191,195},{203,195},{207,195},{215,195}};
static const uint32_t CRIMSON_WALLS_COUNT = sizeof(CRIMSON_WALLS)/sizeof(CRIMSON_WALLS[0]);

static const BiomeMapping HALLOW_WALLS[] = {
    {63,70},{64,70},{65,70},{66,70},{67,70},{68,70},{69,70},{81,70},{264,70},{268,70},
    {1,28},{3,28},{61,28},{83,28},{185,28},{246,28},{262,28},{269,28},{274,28},{349,28},
    {216,219},{217,219},{218,219},{304,219},{305,219},{306,219},
    {187,222},{220,222},{221,222},{275,222},{308,222},{309,222},
    {188,200},{192,200},{204,200},{212,200},{189,201},{193,201},{205,201},{213,201},
    {190,202},{194,202},{206,202},{214,202},{191,203},{195,203},{207,203},{215,203}};
static const uint32_t HALLOW_WALLS_COUNT = sizeof(HALLOW_WALLS)/sizeof(HALLOW_WALLS[0]);

static void init_tile_rule(TxTileRule* rule) {
    memset(rule, 0, sizeof(TxTileRule));
    rule->is_active = -1;
    rule->has_wall = -1;
    rule->biome_region = rule->exclude_biome_region = -1;
    rule->type = -1;
    rule->platform_style = -1;
    rule->frame_x = rule->frame_y = -1;
    rule->wall = -1;
    rule->liquid_amount = -1;
    rule->liquid_type = -1;
    rule->brick_style = -1;
    rule->tile_color = -1;
    rule->wall_color = -1;
    rule->wire_red = -1;
    rule->wire_blue = -1;
    rule->wire_green = -1;
    rule->wire_yellow = -1;
    rule->actuator = -1;
    rule->inactive = -1;
    rule->invisible_block = -1;
    rule->invisible_wall = -1;
    rule->fullbright_block = -1;
    rule->fullbright_wall = -1;

    rule->patch_is_active = -1;
    rule->terrain_theme = rule->wall_theme = rule->furniture_theme = -1;
    rule->patch_liquid_amount = -1;
    rule->patch_liquid_type = -1;
    rule->patch_brick_style = -1;
    rule->patch_tile_color = -1;
    rule->patch_wall_color = -1;
    rule->patch_wire_red = -1;
    rule->patch_wire_blue = -1;
    rule->patch_wire_green = -1;
    rule->patch_wire_yellow = -1;
    rule->patch_invisible_block = -1;
    rule->patch_invisible_wall = -1;
    rule->patch_fullbright_block = -1;
    rule->patch_fullbright_wall = -1;
    rule->patch_actuator = -1;
    rule->patch_inactive = -1;
    rule->patch_type = -1;
    rule->patch_platform_style = -1;
    rule->patch_frame_x = rule->patch_frame_y = -1;
    rule->patch_wall = -1;
}

static int generate_biome_rules(int mode, TxTileRule* out_rules, int out_size) {
    const BiomeMapping* tile_map = NULL; uint32_t tile_count = 0;
    const BiomeMapping* wall_map = NULL; uint32_t wall_count = 0;
    switch (mode) {
    case 0: tile_map = PURIFY_TILES;     tile_count = PURIFY_TILES_COUNT;
            wall_map = PURIFY_WALLS;     wall_count = PURIFY_WALLS_COUNT; break;
    case 1: tile_map = CORRUPTION_TILES; tile_count = CORRUPTION_TILES_COUNT;
            wall_map = CORRUPTION_WALLS; wall_count = CORRUPTION_WALLS_COUNT; break;
    case 2: tile_map = CRIMSON_TILES;    tile_count = CRIMSON_TILES_COUNT;
            wall_map = CRIMSON_WALLS;    wall_count = CRIMSON_WALLS_COUNT; break;
    case 3: tile_map = HALLOW_TILES;     tile_count = HALLOW_TILES_COUNT;
            wall_map = HALLOW_WALLS;     wall_count = HALLOW_WALLS_COUNT; break;
    default: return 0;
    }
    int n = 0;
    for (uint32_t i = 0; i < tile_count && n < out_size; i++, n++) {
        init_tile_rule(&out_rules[n]);
        out_rules[n].type = (int32_t)tile_map[i].old_id;
        out_rules[n].patch_type = (int32_t)tile_map[i].new_id;
    }
    for (uint32_t i = 0; i < wall_count && n < out_size; i++, n++) {
        init_tile_rule(&out_rules[n]);
        out_rules[n].wall = (int32_t)wall_map[i].old_id;
        out_rules[n].patch_wall = (int32_t)wall_map[i].new_id;
    }
    return n;
}

static int tile_matches_where(const TxTile* t, const TxTileRule* rule) {
    if (rule->is_active >= 0 && (int32_t)t->active != rule->is_active) return 0;
    if (rule->has_wall >= 0 && (t->wall != 0) != rule->has_wall) return 0;
    if (rule->type >= 0 && (int32_t)t->type != rule->type) return 0;
    if (rule->platform_style >= 0 && (!t->active || t->type != 19u || t->frame_y != rule->platform_style * 18)) return 0;
    if ((rule->frame_x >= 0 || rule->frame_y >= 0) && !t->active) return 0;
    if (rule->frame_x >= 0 && t->frame_x != rule->frame_x) return 0;
    if (rule->frame_y >= 0 && t->frame_y != rule->frame_y) return 0;
    if (rule->material.present) {
        int found = 0;
        const TxMaterialFrame* m = &rule->material;
        for (int row = 0, fy = m->frame_y; row < m->height; fy += m->coordinate_heights[row++] + m->padding) {
            if (t->frame_y != fy) continue;
            int dx = t->frame_x - m->frame_x;
            found = dx >= 0 && dx % (m->coordinate_width + m->padding) == 0 &&
                dx / (m->coordinate_width + m->padding) < m->width;
            break;
        }
        if (!t->active || !found) return 0;
    }
    if (rule->wall >= 0 && (int32_t)t->wall != rule->wall) return 0;
    if (rule->liquid_amount >= 0 && (int32_t)t->liquid_amount != rule->liquid_amount) return 0;
    if (rule->liquid_type >= 0 && (int32_t)t->liquid_type != rule->liquid_type) return 0;
    if (rule->brick_style >= 0 && (int32_t)t->brick_style != rule->brick_style) return 0;
    if (rule->tile_color >= 0 && (int32_t)t->tile_color != rule->tile_color) return 0;
    if (rule->wall_color >= 0 && (int32_t)t->wall_color != rule->wall_color) return 0;
    if (rule->wire_red >= 0 && (int32_t)t->wire_red != rule->wire_red) return 0;
    if (rule->wire_blue >= 0 && (int32_t)t->wire_blue != rule->wire_blue) return 0;
    if (rule->wire_green >= 0 && (int32_t)t->wire_green != rule->wire_green) return 0;
    if (rule->wire_yellow >= 0 && (int32_t)t->wire_yellow != rule->wire_yellow) return 0;
    if (rule->actuator >= 0 && (int32_t)t->actuator != rule->actuator) return 0;
    if (rule->inactive >= 0 && (int32_t)t->inactive != rule->inactive) return 0;
    if (rule->invisible_block >= 0 && (int32_t)t->invisible_block != rule->invisible_block) return 0;
    if (rule->invisible_wall >= 0 && (int32_t)t->invisible_wall != rule->invisible_wall) return 0;
    if (rule->fullbright_block >= 0 && (int32_t)t->fullbright_block != rule->fullbright_block) return 0;
    if (rule->fullbright_wall >= 0 && (int32_t)t->fullbright_wall != rule->fullbright_wall) return 0;
    return 1;
}

static void apply_tile_patch(TxTile* t, const TxTile* source, const TxTileRule* rule, uint32_t y, double world_surface) {
    int material_col = -1, material_row = -1;
    if (rule->patch_material.present) {
        const TxMaterialFrame* m = &rule->material;
        material_col = (source->frame_x - m->frame_x) / (m->coordinate_width + m->padding);
        for (int row = 0, fy = m->frame_y; row < m->height; fy += m->coordinate_heights[row++] + m->padding)
            if (source->frame_y == fy) { material_row = row; break; }
    }
    if (rule->terrain_theme > 0 || rule->wall_theme > 0 || rule->furniture_theme > 0)
        tx_apply_theme(t, rule->terrain_theme, rule->wall_theme, rule->furniture_theme, y, world_surface);
    if (rule->patch_is_active >= 0) t->active = (uint8_t)rule->patch_is_active;
    if (rule->patch_type >= 0 || rule->patch_platform_style >= 0) {
        uint16_t target = rule->patch_platform_style >= 0 ? 19u : (uint16_t)rule->patch_type;
        int changing_type = !t->active || t->type != target;
        if ((target == 19u || target == 427u || (target >= 435u && target <= 439u)) &&
            changing_type) {
            /* Terraria's standalone platform frame; the game reframes neighbors on load/draw. */
            t->frame_x = 90;
            t->frame_y = 0;
            t->brick_style = 0;
        } else if (changing_type && (rule->patch_frame_x >= 0 || rule->patch_frame_y >= 0)) {
            if (rule->patch_frame_x < 0) t->frame_x = 0;
            if (rule->patch_frame_y < 0) t->frame_y = 0;
        }
        t->type = target;
        t->active = 1;
    }
    if (rule->patch_platform_style >= 0) t->frame_y = (int16_t)(rule->patch_platform_style * 18);
    if (rule->patch_frame_x >= 0) t->frame_x = (int16_t)rule->patch_frame_x;
    if (rule->patch_frame_y >= 0) t->frame_y = (int16_t)rule->patch_frame_y;
    if (rule->patch_material.present && material_row >= 0) {
        const TxMaterialFrame* m = &rule->patch_material;
        int fy = m->frame_y;
        for (int row = 0; row < material_row; row++) fy += m->coordinate_heights[row] + m->padding;
        t->frame_x = (int16_t)(m->frame_x + material_col * (m->coordinate_width + m->padding));
        t->frame_y = (int16_t)fy;
    }
    if (rule->patch_wall >= 0) t->wall = (uint16_t)rule->patch_wall;
    if (rule->patch_liquid_amount >= 0) t->liquid_amount = (uint8_t)rule->patch_liquid_amount;
    if (rule->patch_liquid_type >= 0) t->liquid_type = (uint8_t)rule->patch_liquid_type;
    if (rule->patch_brick_style >= 0) t->brick_style = (uint8_t)rule->patch_brick_style;
    if (rule->patch_tile_color >= 0) t->tile_color = (uint8_t)rule->patch_tile_color;
    if (rule->patch_wall_color >= 0) t->wall_color = (uint8_t)rule->patch_wall_color;
    if (rule->patch_wire_red >= 0) t->wire_red = (uint8_t)rule->patch_wire_red;
    if (rule->patch_wire_blue >= 0) t->wire_blue = (uint8_t)rule->patch_wire_blue;
    if (rule->patch_wire_green >= 0) t->wire_green = (uint8_t)rule->patch_wire_green;
    if (rule->patch_wire_yellow >= 0) t->wire_yellow = (uint8_t)rule->patch_wire_yellow;
    if (rule->patch_invisible_block >= 0) t->invisible_block = (uint8_t)rule->patch_invisible_block;
    if (rule->patch_invisible_wall >= 0) t->invisible_wall = (uint8_t)rule->patch_invisible_wall;
    if (rule->patch_fullbright_block >= 0) t->fullbright_block = (uint8_t)rule->patch_fullbright_block;
    if (rule->patch_fullbright_wall >= 0) t->fullbright_wall = (uint8_t)rule->patch_fullbright_wall;
    if (rule->patch_actuator >= 0) t->actuator = (uint8_t)rule->patch_actuator;
    if (rule->patch_inactive >= 0) t->inactive = (uint8_t)rule->patch_inactive;
}

/* External: apply queued pixel art at (x, y). Returns 1 if modified. */
extern int apply_pixel_art_at(TxWorld* w, uint32_t x, uint32_t y, TxTile* t);

static int world_has_pixel_art(TxWorld* w) {
    return w && w->pixel_art_maps && w->pixel_art_map_count &&
        (w->pixel_art_pixels || w->pixel_art_indexed);
}

static int pixel_art_column_overlaps(TxWorld* w, uint32_t x) {
    if (!world_has_pixel_art(w)) return 0;
    int32_t sx = w->pixel_art_start_x;
    int32_t ex = sx + (int32_t)w->pixel_art_width;
    return (int32_t)x >= sx && (int32_t)x < ex;
}

static int pixel_art_run_overlap(TxWorld* w, uint32_t y, uint32_t run,
                                 uint32_t* out_start, uint32_t* out_end) {
    int32_t sy = w->pixel_art_start_y;
    int32_t ey = sy + (int32_t)w->pixel_art_height;
    int32_t run_start = (int32_t)y;
    int32_t run_end = (int32_t)(y + run);
    int32_t overlap_start = run_start < sy ? sy : run_start;
    int32_t overlap_end = run_end < ey ? run_end : ey;
    if (overlap_start >= overlap_end) return 0;
    *out_start = (uint32_t)overlap_start;
    *out_end = (uint32_t)overlap_end;
    return 1;
}

static void write_pixel_art_segment(TxWorld* w, TxBuf* out, TxTile* base,
                                    uint32_t x, uint32_t start_y, uint32_t end_y) {
    TxTile run_tile = *base;
    run_tile.same = 0;
    apply_pixel_art_at(w, x, start_y, &run_tile);
    uint32_t run_count = 0;

    for (uint32_t py = start_y + 1u; py < end_y; py++) {
        TxTile next = *base;
        next.same = 0;
        apply_pixel_art_at(w, x, py, &next);
        if (same_tile(&next, &run_tile) && run_count < 65535u) {
            run_count++;
        } else {
            write_tile(w, out, &run_tile, run_count);
            run_tile = next;
            run_count = 0;
        }
    }
    write_tile(w, out, &run_tile, run_count);
}

int rebuild_tile_section_pixel_art(TxWorld* w, TxBuf* out) {
    if (!w || !w->file || w->file_len == 0 || !world_has_pixel_art(w)) {
        tx_set_error("TERRAX_INTERNAL_ERROR", "invalid pixel art rebuild state");
        return 0;
    }
    if (w->starts[1] >= w->ends[1] || w->ends[1] > w->file_len) {
        tx_set_error("TERRAX_INTERNAL_ERROR", "invalid tile section range");
        return 0;
    }
    if (!out || !out->ok) {
        tx_set_error("TERRAX_INTERNAL_ERROR", "output buffer not initialized");
        return 0;
    }

    const uint8_t* tile_src;
    uint32_t tile_src_len;
    uint32_t off, end;
    if (w->section_overrides[1].active) {
        tile_src = w->section_overrides[1].data;
        tile_src_len = w->section_overrides[1].len;
        off = 0; end = tile_src_len;
    } else {
        tile_src = w->file; tile_src_len = w->file_len;
        off = w->starts[1]; end = w->ends[1];
    }

    uint32_t world_w = (uint32_t)w->maxTilesX;
    uint32_t world_h = (uint32_t)w->maxTilesY;
    uint8_t* saved_file = w->file;
    uint32_t saved_len = w->file_len;
    w->file = (uint8_t*)tile_src;
    w->file_len = tile_src_len;

    for (uint32_t x = 0; x < world_w; x++) {
        uint32_t column_start = off;
        uint32_t y = 0;
        int column_dirty = pixel_art_column_overlaps(w, x);

        if (!column_dirty) {
            while (y < world_h) {
                TxTile t;
                if (!read_tile_at(w, &off, end, &t)) break;
                y += (uint32_t)t.same + 1u;
            }
            if (off > column_start)
                buf_bytes(out, tile_src + column_start, off - column_start);
            continue;
        }

        while (y < world_h) {
            TxTile t;
            if (!read_tile_at(w, &off, end, &t)) break;
            uint32_t run = (uint32_t)t.same + 1u;
            uint32_t overlap_start = 0, overlap_end = 0;

            if (pixel_art_run_overlap(w, y, run, &overlap_start, &overlap_end)) {
                if (overlap_start > y) {
                    uint32_t before_run = overlap_start - y;
                    write_tile(w, out, &t, before_run - 1u);
                }
                write_pixel_art_segment(w, out, &t, x, overlap_start, overlap_end);
                if (overlap_end < y + run) {
                    uint32_t after_run = (y + run) - overlap_end;
                    write_tile(w, out, &t, after_run - 1u);
                }
            } else {
                write_tile(w, out, &t, (uint32_t)t.same);
            }
            y += run;
        }
    }

    w->file = saved_file;
    w->file_len = saved_len;
    return out->ok;
}

void tx_apply_tile_rules(TxTile* t, TxTileRule* rules, uint32_t rule_count, uint32_t run, uint16_t region, uint32_t y, double world_surface) {
    TxTile original = *t;
    for (uint32_t r = 0; r < rule_count; r++) {
        if (rules[r].biome_region > 0 && !(region & rules[r].biome_region_bit)) continue;
        if (rules[r].exclude_biome_region > 0 && (region & rules[r].exclude_biome_region_bit)) continue;
        if (!tile_matches_where(rules[r].material.present ? &original : t, &rules[r])) continue;
        rules[r].matched += run;
        if (rules[r].limit == 0 || rules[r].updated < rules[r].limit) {
            apply_tile_patch(t, &original, &rules[r], y, world_surface);
            rules[r].updated += run;
        }
    }
}

int rebuild_tile_section(TxWorld* w, TxBuf* out,
                         TxTileRule* rules, uint32_t rule_count) {
    if (!w || !w->file || w->file_len == 0) {
        tx_set_error("TERRAX_INTERNAL_ERROR", "invalid world state"); return 0;
    }
    if (w->starts[1] >= w->ends[1] || w->ends[1] > w->file_len) {
        tx_set_error("TERRAX_INTERNAL_ERROR", "invalid tile section range"); return 0;
    }
    if (!out || !out->ok) {
        tx_set_error("TERRAX_INTERNAL_ERROR", "output buffer not initialized"); return 0;
    }
    if (w->prepared_output && !w->prepared_output->ready && !world_has_pixel_art(w))
        return tx_output_scan(w, rules, rule_count, out);
    const uint8_t* tile_src;
    uint32_t tile_src_len;
    uint32_t off, end;
    if (w->section_overrides[1].active) {
        tile_src = w->section_overrides[1].data;
        tile_src_len = w->section_overrides[1].len;
        off = 0; end = tile_src_len;
    } else {
        tile_src = w->file; tile_src_len = w->file_len;
        off = w->starts[1]; end = w->ends[1];
    }
    uint32_t tile_count = 0;
    uint32_t world_w = (uint32_t)w->maxTilesX;
    uint32_t world_h = (uint32_t)w->maxTilesY;
    uint8_t* saved_file = w->file;
    uint32_t saved_len = w->file_len;
    w->file = (uint8_t*)tile_src; w->file_len = tile_src_len;

    /* Column-major streaming pass (matching Terraria's tile order) */
    for (uint32_t x = 0; x < world_w; x++) {
        uint32_t y = 0, remaining = 0;
        TxTile source;
        while (y < world_h) {
            if (!remaining) {
                if (!read_tile_at(w, &off, end, &source) || (uint32_t)source.same + 1u > world_h - y) {
                    out->ok = 0;
                    break;
                }
                remaining = (uint32_t)source.same + 1u;
            }
            TxTile t = source;
            uint32_t run = tx_region_run(w, x, y, remaining);
            remaining -= run;
            t.same = run - 1u;

            tx_apply_tile_rules(&t, rules, rule_count, run, tx_region_at(w, x, y), y, w->worldSurface);

            /* Check if this tile falls in the pixel art region.
             * If the run spans the pixel art area, we need to split it. */
            int32_t pa_x0 = w->pixel_art_start_x;
            int32_t pa_y0 = w->pixel_art_start_y;
            uint32_t pa_w = w->pixel_art_width;
            uint32_t pa_h = w->pixel_art_height;
            int has_pixel_art = (w->pixel_art_pixels && w->pixel_art_maps && w->pixel_art_map_count);

            if (has_pixel_art &&
                (int32_t)x >= pa_x0 && (int32_t)x < pa_x0 + (int32_t)pa_w) {
                int32_t run_start = (int32_t)y;
                int32_t run_end = (int32_t)(y + run);
                int32_t overlap_start = run_start < pa_y0 ? pa_y0 : run_start;
                int32_t overlap_end = run_end < pa_y0 + (int32_t)pa_h ? run_end : pa_y0 + (int32_t)pa_h;

                if (overlap_start < overlap_end) {
                    /* Segment before pixel art */
                    if (overlap_start > run_start) {
                        uint32_t before_run = (uint32_t)(overlap_start - run_start);
                        write_tile(w, out, &t, before_run - 1u);
                        tile_count += before_run;
                    }

                    /* Pixel art segment with RLE merging */
                    {
                        int32_t py = overlap_start;
                        TxTile pa_tile = t;
                        pa_tile.same = 0;
                        apply_pixel_art_at(w, x, (uint32_t)py, &pa_tile);
                        uint32_t run_count = 0;
                        TxTile run_tile = pa_tile;

                        for (py = overlap_start + 1; py < overlap_end; py++) {
                            TxTile next = t;
                            next.same = 0;
                            apply_pixel_art_at(w, x, (uint32_t)py, &next);

                            /* Compare tiles for RLE eligibility */
                            if (next.type == run_tile.type &&
                                next.active == run_tile.active &&
                                next.wall == run_tile.wall &&
                                next.wall_color == run_tile.wall_color &&
                                next.tile_color == run_tile.tile_color &&
                                next.inactive == run_tile.inactive &&
                                next.liquid_type == run_tile.liquid_type &&
                                next.liquid_amount == run_tile.liquid_amount &&
                                next.wire_red == run_tile.wire_red &&
                                next.wire_blue == run_tile.wire_blue &&
                                next.wire_green == run_tile.wire_green &&
                                next.wire_yellow == run_tile.wire_yellow &&
                                next.brick_style == run_tile.brick_style &&
                                next.actuator == run_tile.actuator &&
                                next.invisible_block == run_tile.invisible_block &&
                                next.invisible_wall == run_tile.invisible_wall &&
                                run_count < 65535u) {
                                run_count++;
                            } else {
                                write_tile(w, out, &run_tile, run_count);
                                tile_count += run_count + 1u;
                                run_tile = next;
                                run_count = 0;
                            }
                        }
                        write_tile(w, out, &run_tile, run_count);
                        tile_count += run_count + 1u;
                    }

                    /* Segment after pixel art */
                    if (overlap_end < run_end) {
                        uint32_t after_run = (uint32_t)(run_end - overlap_end);
                        write_tile(w, out, &t, after_run - 1u);
                        tile_count += after_run;
                    }

                    y += run;
                    continue;
                }
            }

            /* No pixel art overlap: write original tile */
            write_tile(w, out, &t, (uint32_t)t.same);
            tile_count += run;
            y += run;
        }
    }
    w->file = saved_file; w->file_len = saved_len;
    return out->ok;
}

static int parse_biome_mode(const char* request, int jlen) {
    extern int json_find_key(const char* json, int jlen, const char* key);
    extern int json_extract_str(const char* json, int jlen, int pos, char* out, int ocap);
    int p = json_find_key(request, jlen, "mode");
    if (p < 0) p = json_find_key(request, jlen, "biome_mode");
    if (p >= 0) {
        char mode_str[32];
        if (json_extract_str(request, jlen, p, mode_str, 32)) {
            if (tx_streq_c(mode_str, "purify")) return 0;
            if (tx_streq_c(mode_str, "corruption")) return 1;
            if (tx_streq_c(mode_str, "crimson")) return 2;
            if (tx_streq_c(mode_str, "hallow")) return 3;
        }
    }
    return -1;
}

static int parse_material_frame(const char* json, int jlen, int pos, TxMaterialFrame* m) {
    extern int json_find_key(const char*, int, const char*);
    extern int json_array_count(const char*, int, int);
    extern int json_array_element(const char*, int, int, int);
    extern int json_skip_value(const char*, int, int);
    extern int json_extract_int(const char*, int, int, int32_t*);
    static const char* keys[] = {"frame_x", "frame_y", "width", "height", "coordinate_width", "padding"};
    int32_t* fields[] = {&m->frame_x, &m->frame_y, &m->width, &m->height, &m->coordinate_width, &m->padding};
    int end = json_skip_value(json, jlen, pos);
    if (end <= pos) return 0;
    for (int i = 0; i < 6; i++) {
        int p = json_find_key(json + pos, end - pos, keys[i]);
        if (p < 0 || !json_extract_int(json, jlen, pos + p, fields[i])) return 0;
    }
    if (m->frame_x < 0 || m->frame_y < 0 || m->width < 1 || m->height < 1 ||
        m->width > TX_MATERIAL_MAX_CELLS || m->height > TX_MATERIAL_MAX_CELLS ||
        m->coordinate_width < 1 || m->coordinate_width > 32767 ||
        m->padding < 0 || m->padding > 32767 ||
        (int64_t)m->coordinate_width + m->padding > 32767) return 0;
    int p = json_find_key(json + pos, end - pos, "coordinate_heights");
    if (p < 0 || json_array_count(json, jlen, pos + p) != m->height) return 0;
    int64_t fy = m->frame_y;
    for (int row = 0; row < m->height; row++) {
        int element = json_array_element(json, jlen, pos + p, row);
        if (element < 0 || !json_extract_int(json, jlen, element, &m->coordinate_heights[row]) ||
            m->coordinate_heights[row] < 1 || m->coordinate_heights[row] > 32767 || fy > 32767) return 0;
        fy += m->coordinate_heights[row] + m->padding;
    }
    if ((int64_t)m->frame_x + (int64_t)(m->width - 1) *
        (m->coordinate_width + (int64_t)m->padding) > 32767) return 0;
    m->present = 1;
    return 1;
}

int tx_stream_parse_tile_rules(TxWorld* w, const char* request, int jlen, TxTileRule** out_rules, uint32_t* out_count) {
    if (!tx_world_require_writable(w)) return -1;
    if (w->legacy_wld) {
        tx_set_error("TERRAX_NOT_SUPPORTED", "pre-88 worlds support reading and original-byte export only");
        return -1;
    }
    extern int json_find_key(const char* json, int jlen, const char* key);
    extern int json_array_count(const char* json, int jlen, int pos);
    extern int json_array_element(const char* json, int jlen, int pos, int index);
    extern int json_skip_value(const char* json, int jlen, int pos);
    extern int json_extract_int(const char* json, int jlen, int pos, int32_t* out);
    extern int json_extract_bool(const char* json, int jlen, int pos, int* out);
    extern int json_is_null(const char* json, int jlen, int pos);

    TxTileRule* rules = NULL;
    int rule_count = 0;

    int mode = parse_biome_mode(request, jlen);
    if (mode >= 0) {
        rules = (TxTileRule*)tx_alloc(96 * sizeof(TxTileRule));
        if (!rules) { tx_set_error("TERRAX_WASM_OOM", "alloc failed"); return -1; }
        rule_count = generate_biome_rules(mode, rules, 96);
        if (rule_count <= 0) {
            tx_internal_free(rules);
            tx_set_error("TERRAX_VALIDATION_ERROR", "no biome rules generated");
            return -1;
        }
    }

    if (!rules) {
        int rules_pos = json_find_key(request, jlen, "rules");
        if (rules_pos < 0) {
            tx_set_error("TERRAX_VALIDATION_ERROR", "missing rules or biome_mode");
            return -1;
        }
        rule_count = json_array_count(request, jlen, rules_pos);
        if (rule_count <= 0 || rule_count > 128) {
            tx_set_error("TERRAX_VALIDATION_ERROR", "rules array must have 1-128 elements");
            return -1;
        }
        rules = (TxTileRule*)tx_alloc((uint32_t)rule_count * sizeof(TxTileRule));
        if (!rules) { tx_set_error("TERRAX_WASM_OOM", "failed to allocate rules"); return -1; }
        for (int r = 0; r < rule_count; r++) {
            init_tile_rule(&rules[r]);
            int elem_pos = json_array_element(request, jlen, rules_pos, r);
            if (elem_pos < 0) continue;
            int elem_end = json_skip_value(request, jlen, elem_pos);
            int elem_len = elem_end > elem_pos ? elem_end - elem_pos : 0;

            int where_pos = json_find_key(request + elem_pos, elem_len, "where");
            if (where_pos >= 0) {
                where_pos += elem_pos;
                int where_end = json_skip_value(request, elem_end, where_pos);
                int where_len = where_end > where_pos ? where_end - where_pos : 0;
                int32_t iv; int bv; int wp;
                #define TRY_MATCH_INT(field, key) \
                    wp = json_find_key(request + where_pos, where_len, key); \
                    if (wp >= 0 && !json_is_null(request, jlen, wp + where_pos)) { \
                        if (json_extract_int(request, jlen, wp + where_pos, &iv)) \
                            rules[r].field = iv; \
                    }
                TRY_MATCH_INT(type, "type")
                TRY_MATCH_INT(wall, "wall")
                TRY_MATCH_INT(liquid_amount, "liquid_amount")
                TRY_MATCH_INT(liquid_type, "liquid_type")
                TRY_MATCH_INT(brick_style, "brick_style")
                TRY_MATCH_INT(tile_color, "tile_color")
                TRY_MATCH_INT(wall_color, "wall_color")
                #undef TRY_MATCH_INT
                wp = json_find_key(request + where_pos, where_len, "platform_style");
                if (wp >= 0) {
                    int type_pos = json_find_key(request + where_pos, where_len, "type");
                    if (!json_extract_int(request, jlen, wp + where_pos, &iv) || iv < 0 || iv > 69 ||
                        (type_pos >= 0 && !json_is_null(request, jlen, type_pos + where_pos) && rules[r].type != 19)) {
                        tx_internal_free(rules);
                        tx_set_error("TERRAX_VALIDATION_ERROR", "where.platform_style requires Tile 19 and integer style 0..69");
                        return -1;
                    }
                    rules[r].platform_style = iv;
                }
                #define TRY_MATCH_FRAME(field, key, label) \
                    wp = json_find_key(request + where_pos, where_len, key); \
                    if (wp >= 0) { \
                        if (!json_extract_int(request, jlen, wp + where_pos, &iv) || iv < 0 || iv > 32767) { \
                            tx_internal_free(rules); \
                            tx_set_error("TERRAX_VALIDATION_ERROR", label " must be an integer from 0 to 32767"); \
                            return -1; \
                        } \
                        rules[r].field = iv; \
                    }
                TRY_MATCH_FRAME(frame_x, "frame_x", "where.frame_x")
                TRY_MATCH_FRAME(frame_y, "frame_y", "where.frame_y")
                #undef TRY_MATCH_FRAME
                if (rules[r].platform_style >= 0 && rules[r].frame_y >= 0 &&
                    rules[r].frame_y != rules[r].platform_style * 18) {
                    tx_internal_free(rules);
                    tx_set_error("TERRAX_VALIDATION_ERROR", "where.platform_style conflicts with where.frame_y");
                    return -1;
                }
                #define TRY_MATCH_BOOL(field, key) \
                    wp = json_find_key(request + where_pos, where_len, key); \
                    if (wp >= 0 && !json_is_null(request, jlen, wp + where_pos)) { \
                        if (json_extract_bool(request, jlen, wp + where_pos, &bv)) \
                            rules[r].field = bv; \
                        else if (json_extract_int(request, jlen, wp + where_pos, &iv)) \
                            rules[r].field = iv; \
                    }
                TRY_MATCH_BOOL(is_active, "is_active")
                TRY_MATCH_BOOL(wire_red, "wire_red")
                TRY_MATCH_BOOL(wire_blue, "wire_blue")
                TRY_MATCH_BOOL(wire_green, "wire_green")
                TRY_MATCH_BOOL(wire_yellow, "wire_yellow")
                TRY_MATCH_BOOL(actuator, "actuator")
                TRY_MATCH_BOOL(inactive, "inactive")
                TRY_MATCH_BOOL(invisible_block, "invisible_block")
                TRY_MATCH_BOOL(invisible_wall, "invisible_wall")
                TRY_MATCH_BOOL(fullbright_block, "fullbright_block")
                TRY_MATCH_BOOL(fullbright_wall, "fullbright_wall")
                #undef TRY_MATCH_BOOL
                wp = json_find_key(request + where_pos, where_len, "has_wall");
                if (wp >= 0) {
                    if (json_extract_bool(request, jlen, wp + where_pos, &bv)) iv = bv;
                    else if (!json_extract_int(request, jlen, wp + where_pos, &iv)) iv = -1;
                    if (iv < 0 || iv > 1) {
                        tx_internal_free(rules);
                        tx_set_error("TERRAX_VALIDATION_ERROR", "has_wall must be boolean or 0/1");
                        return -1;
                    }
                    rules[r].has_wall = iv;
                }
                wp = json_find_key(request + where_pos, where_len, "biome_region");
                if (wp >= 0) {
                    if (!json_extract_int(request, jlen, wp + where_pos, &iv) || iv < 1 || iv > TX_REGION_COUNT) {
                        tx_internal_free(rules);
                        tx_set_error("TERRAX_VALIDATION_ERROR", "biome_region must be an integer from 1 to 14");
                        return -1;
                    }
                    rules[r].biome_region = iv;
                }
                wp = json_find_key(request + where_pos, where_len, "exclude_biome_region");
                if (wp >= 0) {
                    if (!json_extract_int(request, jlen, wp + where_pos, &iv) || iv < 1 || iv > TX_REGION_COUNT) {
                        tx_internal_free(rules);
                        tx_set_error("TERRAX_VALIDATION_ERROR", "exclude_biome_region must be an integer from 1 to 14");
                        return -1;
                    }
                    rules[r].exclude_biome_region = iv;
                }
            }
            int patch_pos = json_find_key(request + elem_pos, elem_len, "patch");
            if (patch_pos >= 0) {
                patch_pos += elem_pos;
                int patch_end = json_skip_value(request, elem_end, patch_pos);
                int patch_len = patch_end > patch_pos ? patch_end - patch_pos : 0;
                int32_t iv; int bv; int pp;
                #define TRY_PATCH_INT(field, key) \
                    pp = json_find_key(request + patch_pos, patch_len, key); \
                    if (pp >= 0 && !json_is_null(request, jlen, pp + patch_pos)) { \
                        if (json_extract_int(request, jlen, pp + patch_pos, &iv)) \
                            rules[r].field = iv; \
                    }
                TRY_PATCH_INT(patch_type, "type")
                TRY_PATCH_INT(patch_wall, "wall")
                TRY_PATCH_INT(patch_liquid_amount, "liquid_amount")
                TRY_PATCH_INT(patch_liquid_type, "liquid_type")
                TRY_PATCH_INT(patch_brick_style, "brick_style")
                TRY_PATCH_INT(patch_tile_color, "tile_color")
                TRY_PATCH_INT(patch_wall_color, "wall_color")
                #undef TRY_PATCH_INT
                pp = json_find_key(request + patch_pos, patch_len, "platform_style");
                if (pp >= 0) {
                    int type_pos = json_find_key(request + patch_pos, patch_len, "type");
                    if (!json_extract_int(request, jlen, pp + patch_pos, &iv) || iv < 0 || iv > 69 ||
                        (type_pos >= 0 && !json_is_null(request, jlen, type_pos + patch_pos) && rules[r].patch_type != 19)) {
                        tx_internal_free(rules);
                        tx_set_error("TERRAX_VALIDATION_ERROR", "patch.platform_style requires Tile 19 and integer style 0..69");
                        return -1;
                    }
                    rules[r].patch_platform_style = iv;
                }
                #define TRY_PATCH_FRAME(field, key, label) \
                    pp = json_find_key(request + patch_pos, patch_len, key); \
                    if (pp >= 0) { \
                        if (!json_extract_int(request, jlen, pp + patch_pos, &iv) || iv < 0 || iv > 32767) { \
                            tx_internal_free(rules); \
                            tx_set_error("TERRAX_VALIDATION_ERROR", label " must be an integer from 0 to 32767"); \
                            return -1; \
                        } \
                        rules[r].field = iv; \
                    }
                TRY_PATCH_FRAME(patch_frame_x, "frame_x", "patch.frame_x")
                TRY_PATCH_FRAME(patch_frame_y, "frame_y", "patch.frame_y")
                #undef TRY_PATCH_FRAME
                if (rules[r].patch_platform_style >= 0 && rules[r].patch_frame_y >= 0) {
                    tx_internal_free(rules);
                    tx_set_error("TERRAX_VALIDATION_ERROR", "patch.platform_style conflicts with patch.frame_y");
                    return -1;
                }
                #define TRY_THEME(field) \
                    pp = json_find_key(request + patch_pos, patch_len, #field); \
                    if (pp >= 0) { \
                        if (!json_extract_int(request, jlen, pp + patch_pos, &iv) || iv < 1 || iv > 3) { \
                            tx_internal_free(rules); \
                            tx_set_error("TERRAX_VALIDATION_ERROR", #field " must be 1 (desert), 2 (snow), or 3 (jungle)"); \
                            return -1; \
                        } \
                        rules[r].field = iv; \
                    }
                TRY_THEME(terrain_theme)
                TRY_THEME(wall_theme)
                TRY_THEME(furniture_theme)
                #undef TRY_THEME
                #define TRY_PATCH_BOOL(field, key) \
                    pp = json_find_key(request + patch_pos, patch_len, key); \
                    if (pp >= 0 && !json_is_null(request, jlen, pp + patch_pos)) { \
                        if (json_extract_bool(request, jlen, pp + patch_pos, &bv)) \
                            rules[r].field = bv; \
                        else if (json_extract_int(request, jlen, pp + patch_pos, &iv)) \
                            rules[r].field = iv; \
                    }
                TRY_PATCH_BOOL(patch_is_active, "is_active")
                TRY_PATCH_BOOL(patch_wire_red, "wire_red")
                TRY_PATCH_BOOL(patch_wire_blue, "wire_blue")
                TRY_PATCH_BOOL(patch_wire_green, "wire_green")
                TRY_PATCH_BOOL(patch_wire_yellow, "wire_yellow")
                TRY_PATCH_BOOL(patch_invisible_block, "invisible_block")
                TRY_PATCH_BOOL(patch_invisible_wall, "invisible_wall")
                TRY_PATCH_BOOL(patch_fullbright_block, "fullbright_block")
                TRY_PATCH_BOOL(patch_fullbright_wall, "fullbright_wall")
                TRY_PATCH_BOOL(patch_actuator, "actuator")
                TRY_PATCH_BOOL(patch_inactive, "inactive")
                #undef TRY_PATCH_BOOL
            }
            if (where_pos >= 0) {
                int where_end = json_skip_value(request, jlen, where_pos);
                int mp = json_find_key(request + where_pos, where_end - where_pos, "material");
                if (mp >= 0 && (!parse_material_frame(request, jlen, where_pos + mp, &rules[r].material) ||
                    rules[r].type < 0 || rules[r].type > 65535 ||
                    !tile_important(w, (uint16_t)rules[r].type) ||
                    rules[r].platform_style >= 0 || rules[r].frame_x >= 0 || rules[r].frame_y >= 0)) {
                    tx_internal_free(rules);
                    tx_set_error("TERRAX_VALIDATION_ERROR", "where.material requires a frame-important where.type and valid layout without raw frames or platform_style");
                    return -1;
                }
            }
            if (patch_pos >= 0) {
                int patch_end = json_skip_value(request, jlen, patch_pos);
                int mp = json_find_key(request + patch_pos, patch_end - patch_pos, "material");
                if (mp >= 0 && (!parse_material_frame(request, jlen, patch_pos + mp, &rules[r].patch_material) ||
                    !rules[r].material.present || rules[r].patch_platform_style >= 0 ||
                    rules[r].patch_frame_x >= 0 || rules[r].patch_frame_y >= 0 ||
                    rules[r].patch_type > 65535 ||
                    (rules[r].patch_type >= 0 && !tile_important(w, (uint16_t)rules[r].patch_type)) ||
                    rules[r].material.width != rules[r].patch_material.width ||
                    rules[r].material.height != rules[r].patch_material.height)) {
                    tx_internal_free(rules);
                    tx_set_error("TERRAX_VALIDATION_ERROR", "patch.material requires where.material, frame-important target type, and matching dimensions without raw frames or platform_style");
                    return -1;
                }
            }
            if (rules[r].material.present &&
                (rules[r].patch_platform_style >= 0 ? 19 : rules[r].patch_type) >= 0 &&
                (rules[r].patch_platform_style >= 0 ? 19 : rules[r].patch_type) != rules[r].type &&
                (rules[r].material.width != 1 || rules[r].material.height != 1)) {
                tx_internal_free(rules);
                tx_set_error("TERRAX_VALIDATION_ERROR", "changing a multi-cell material's tile type is unsupported");
                return -1;
            }
            if (rules[r].patch_material.present && (rules[r].patch_type < -1 || rules[r].patch_is_active == 0)) {
                tx_internal_free(rules);
                tx_set_error("TERRAX_VALIDATION_ERROR", "patch.material requires an active tile and valid patch.type");
                return -1;
            }
            int limit_pos = json_find_key(request + elem_pos, elem_len, "limit");
            if (limit_pos >= 0) {
                int32_t lv;
                if (json_extract_int(request, jlen, limit_pos + elem_pos, &lv) && lv >= 0)
                    rules[r].limit = (uint32_t)lv;
            }
            if (rules[r].material.present &&
                (rules[r].material.width != 1 || rules[r].material.height != 1) && rules[r].limit != 0) {
                tx_internal_free(rules);
                tx_set_error("TERRAX_VALIDATION_ERROR", "multi-cell material rules cannot have a limit");
                return -1;
            }
        }
    }

    *out_rules=rules; *out_count=(uint32_t)rule_count; return 1;
}

int execute_batch_update_tiles(TxWorld* w, const char* request, int jlen,
                               TxBuf* response) {
    TxTileRule* rules=NULL; uint32_t parsed_count=0;
    if(tx_stream_parse_tile_rules(w,request,jlen,&rules,&parsed_count)<0)return -1;
    int rule_count=(int)parsed_count;

    if (w->prepared_output && w->prepared_output->ready) tx_output_clear(w);
    if (!tx_regions_build(w, rules, (uint32_t)rule_count)) {
        tx_internal_free(rules);
        return -1;
    }
    uint32_t batch_cap = w->section_overrides[1].active
        ? w->section_overrides[1].len : (w->ends[1] - w->starts[1]);
    TxBuf tile_buf;
    if (!init_tile_buffer(&tile_buf, batch_cap, "failed to allocate tile buffer")) {
        if (w->region_mask) tx_internal_free(w->region_mask);
        w->region_mask = NULL;
        w->surface_sand_split = 0;
        tx_internal_free(rules);
        return -1;
    }
    int rebuilt = rebuild_tile_section(w, &tile_buf, rules, (uint32_t)rule_count);
    if (w->region_mask) tx_internal_free(w->region_mask);
    w->region_mask = NULL;
    w->surface_sand_split = 0;
    if (!rebuilt) {
        tx_internal_free(tile_buf.data);
        tx_internal_free(rules);
        tx_set_error("TERRAX_INTERNAL_ERROR", "tile section rebuild failed");
        return -1;
    }
    w->output_capture = w->prepared_output && w->prepared_output->ready;
    extern int set_section_override_data(TxWorld* w, int idx, uint8_t* data, uint32_t len);
    if (!set_section_override_data(w, 1, tile_buf.data, tile_buf.len)) {
        w->output_capture = 0;
        tx_internal_free(tile_buf.data);
        tx_internal_free(rules);
        return -1;
    }

    w->output_capture = 0;
    uint32_t total_matched = 0, total_updated = 0;
    buf_cstr(response, "{\"status\":\"ok\",\"rule_count\":");
    json_u32(response, (uint32_t)rule_count);
    buf_cstr(response, ",\"rules\":[");
    for (int r = 0; r < rule_count; r++) {
        if (r > 0) buf_u8(response, ',');
        buf_cstr(response, "{\"index\":");
        json_u32(response, (uint32_t)r);
        buf_cstr(response, ",\"matched\":");
        json_u32(response, rules[r].matched);
        buf_cstr(response, ",\"updated\":");
        json_u32(response, rules[r].updated);
        buf_u8(response, '}');
        total_matched += rules[r].matched;
        total_updated += rules[r].updated;
    }
    buf_u8(response, ']');
    buf_cstr(response, ",\"total_matched\":");
    json_u32(response, total_matched);
    buf_cstr(response, ",\"total_updated\":");
    json_u32(response, total_updated);
    buf_u8(response, '}');
    int result = set_result_buf(response);
    tx_internal_free(rules);
    return result;
}
