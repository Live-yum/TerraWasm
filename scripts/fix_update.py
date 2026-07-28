import os, sys

path = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\depths\Desktop\Tdecoder\TerraWasm\src\terra_update.c'

# Read the broken reformatted file to understand what we need
with open(path, 'r', encoding='utf-8', errors='replace') as f:
    lines = f.readlines()

print(f"Current file has {len(lines)} lines")

# Find problematic areas - the regex reformatter broke string literals by inserting newlines
# The safest approach: rewrite the file completely
content = '''/*
 * terra_update.c -- Streaming tile modifications: biome conversion + batch updates.
 *
 * All tile modifications are applied as a single streaming pass through the
 * raw .wld tile data. No full tile array is ever materialized.
 */
#include "terra_types.h"

extern void* memset(void* dst, int value, unsigned long n);
extern void* memcpy(void* dst, const void* src, unsigned long n);

extern uint8_t* tx_alloc(uint32_t size);
extern void tx_clear_error(void);
extern void tx_set_error(const char* code, const char* message);
extern int set_result_buf(TxBuf* b);
extern void buf_init(TxBuf* b, uint32_t cap);
extern int tx_streq_c(const char* a, const char* b);
extern int32_t tx_last_status;
extern void buf_u8(TxBuf* b, uint8_t v);
extern void buf_u32le(TxBuf* b, uint32_t v);
extern void buf_cstr(TxBuf* b, const char* s);
extern void json_u32(TxBuf* b, uint32_t v);
extern void json_i32(TxBuf* b, int32_t v);
extern void json_string(TxBuf* b, const char* s);

extern int read_tile_at(TxWorld* w, uint32_t* off, uint32_t end, TxTile* t);
extern void write_tile(TxWorld* w, TxBuf* b, const TxTile* t, uint32_t same);
extern int tile_important(TxWorld* w, uint16_t type);

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
        memset(&out_rules[n], 0, sizeof(TxTileRule));
        out_rules[n].type = (int32_t)tile_map[i].old_id;
        out_rules[n].patch_type = (int32_t)tile_map[i].new_id;
    }
    for (uint32_t i = 0; i < wall_count && n < out_size; i++, n++) {
        memset(&out_rules[n], 0, sizeof(TxTileRule));
        out_rules[n].wall = (int32_t)wall_map[i].old_id;
        out_rules[n].patch_wall = (int32_t)wall_map[i].new_id;
    }
    return n;
}

static int tile_matches_where(const TxTile* t, const TxTileRule* rule) {
    if (rule->is_active >= 0 && (int32_t)t->active != rule->is_active) return 0;
    if (rule->type >= 0 && (int32_t)t->type != rule->type) return 0;
    if (rule->wall >= 0 && (int32_t)t->wall != rule->wall) return 0;
    if (rule->liquid_type >= 0 && (int32_t)t->liquid_type != rule->liquid_type) return 0;
    if (rule->wire_red >= 0 && (int32_t)t->wire_red != rule->wire_red) return 0;
    if (rule->wire_blue >= 0 && (int32_t)t->wire_blue != rule->wire_blue) return 0;
    if (rule->wire_green >= 0 && (int32_t)t->wire_green != rule->wire_green) return 0;
    if (rule->wire_yellow >= 0 && (int32_t)t->wire_yellow != rule->wire_yellow) return 0;
    if (rule->actuator >= 0 && (int32_t)t->actuator != rule->actuator) return 0;
    if (rule->inactive >= 0 && (int32_t)t->inactive != rule->inactive) return 0;
    if (rule->invisible_block >= 0 && (int32_t)t->invisible_block != rule->invisible_block) return 0;
    if (rule->invisible_wall >= 0 && (int32_t)t->invisible_wall != rule->invisible_wall) return 0;
    return 1;
}

static void apply_tile_patch(TxTile* t, const TxTileRule* rule) {
    if (rule->patch_type >= 0) { t->type = (uint16_t)rule->patch_type; t->active = 1; }
    if (rule->patch_wall >= 0) t->wall = (uint16_t)rule->patch_wall;
    if (rule->patch_wire_red >= 0) t->wire_red = (uint8_t)rule->patch_wire_red;
    if (rule->patch_wire_blue >= 0) t->wire_blue = (uint8_t)rule->patch_wire_blue;
    if (rule->patch_wire_green >= 0) t->wire_green = (uint8_t)rule->patch_wire_green;
    if (rule->patch_wire_yellow >= 0) t->wire_yellow = (uint8_t)rule->patch_wire_yellow;
    if (rule->patch_invisible_block >= 0) t->invisible_block = (uint8_t)rule->patch_invisible_block;
    if (rule->patch_invisible_wall >= 0) t->invisible_wall = (uint8_t)rule->patch_invisible_wall;
    if (rule->patch_actuator >= 0) t->actuator = (uint8_t)rule->patch_actuator;
    if (rule->patch_inactive >= 0) t->inactive = (uint8_t)rule->patch_inactive;
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
    uint8_t* saved_file = w->file;
    uint32_t saved_len = w->file_len;
    w->file = (uint8_t*)tile_src; w->file_len = tile_src_len;
    while (off < end) {
        TxTile t;
        if (!read_tile_at(w, &off, end, &t)) break;
        uint32_t run = (uint32_t)t.same + 1u;
        for (uint32_t r = 0; r < rule_count; r++) {
            if (tile_matches_where(&t, &rules[r])) {
                rules[r].matched += run;
                if (rules[r].limit == 0 || rules[r].updated < rules[r].limit) {
                    apply_tile_patch(&t, &rules[r]);
                    rules[r].updated += run;
                }
            }
        }
        write_tile(w, out, &t, (uint32_t)t.same);
        tile_count += run;
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

int execute_convert_world_biome(TxWorld* w, const char* request, int jlen,
                                TxBuf* response) {
    int mode = parse_biome_mode(request, jlen);
    if (mode < 0) {
        tx_set_error("TERRAX_VALIDATION_ERROR", "invalid or missing biome mode");
        return -1;
    }
    TxTileRule biome_rules[96];
    int rule_count = generate_biome_rules(mode, biome_rules, 96);
    if (rule_count <= 0) {
        tx_set_error("TERRAX_VALIDATION_ERROR", "no biome rules generated");
        return -1;
    }
    uint32_t biome_cap = w->section_overrides[1].active
        ? w->section_overrides[1].len : (w->ends[1] - w->starts[1]);
    TxBuf tile_buf;
    buf_init(&tile_buf, biome_cap + 65536u);
    if (!rebuild_tile_section(w, &tile_buf, biome_rules, (uint32_t)rule_count)) {
        tx_set_error("TERRAX_INTERNAL_ERROR", "tile section rebuild failed");
        return -1;
    }
    extern int set_section_override_data(TxWorld* w, int idx, uint8_t* data, uint32_t len);
    if (!set_section_override_data(w, 1, tile_buf.data, tile_buf.len)) return -1;
    uint32_t total_matched = 0, total_updated = 0;
    for (int r = 0; r < rule_count; r++) {
        total_matched += biome_rules[r].matched;
        total_updated += biome_rules[r].updated;
    }
    buf_cstr(response, "{\\"status\\":\\"ok\\",\\"mode\\":\\"");
    switch (mode) {
    case 0: buf_cstr(response, "purify"); break;
    case 1: buf_cstr(response, "corruption"); break;
    case 2: buf_cstr(response, "crimson"); break;
    case 3: buf_cstr(response, "hallow"); break;
    }
    buf_cstr(response, "\\",\\"rule_count\\":");
    json_u32(response, (uint32_t)rule_count);
    buf_cstr(response, ",\\"total_matched\\":");
    json_u32(response, total_matched);
    buf_cstr(response, ",\\"total_updated\\":");
    json_u32(response, total_updated);
    buf_cstr(response, "}");
    return set_result_buf(response);
}

int execute_batch_update_tiles(TxWorld* w, const char* request, int jlen,
                               TxBuf* response) {
    extern int json_find_key(const char* json, int jlen, const char* key);
    extern int json_array_count(const char* json, int jlen, int pos);
    extern int json_array_element(const char* json, int jlen, int pos, int index);
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
        memset(rules, 0, (uint32_t)rule_count * sizeof(TxTileRule));
        for (int r = 0; r < rule_count; r++) {
            int elem_pos = json_array_element(request, jlen, rules_pos, r);
            if (elem_pos < 0) continue;
            rules[r].is_active = -1; rules[r].type = -1; rules[r].wall = -1;
            rules[r].liquid_type = -1;
            rules[r].wire_red = -1; rules[r].wire_blue = -1;
            rules[r].wire_green = -1; rules[r].wire_yellow = -1;
            rules[r].actuator = -1; rules[r].inactive = -1;
            rules[r].invisible_block = -1; rules[r].invisible_wall = -1;
            rules[r].patch_wire_red = -1; rules[r].patch_wire_blue = -1;
            rules[r].patch_wire_green = -1; rules[r].patch_wire_yellow = -1;
            rules[r].patch_invisible_block = -1; rules[r].patch_invisible_wall = -1;
            rules[r].patch_actuator = -1; rules[r].patch_inactive = -1;
            rules[r].patch_type = -1; rules[r].patch_wall = -1;

            int where_pos = json_find_key(request + elem_pos, jlen - elem_pos, "where");
            if (where_pos >= 0) {
                where_pos += elem_pos;
                int32_t iv; int bv; int wp;
                #define TRY_MATCH(field, key) \\
                    wp = json_find_key(request + where_pos, jlen - where_pos, key); \\
                    if (wp >= 0 && !json_is_null(request, jlen, wp + where_pos)) { \\
                        if (json_extract_int(request, jlen, wp + where_pos, &iv)) \\
                            rules[r].field = iv; \\
                    }
                TRY_MATCH(type, "type")
                else TRY_MATCH(wall, "wall")
                else TRY_MATCH(liquid_type, "liquid_type")
                #undef TRY_MATCH
                #define TRY_BOOL(field, key) \\
                    wp = json_find_key(request + where_pos, jlen - where_pos, key); \\
                    if (wp >= 0 && !json_is_null(request, jlen, wp + where_pos)) { \\
                        if (json_extract_bool(request, jlen, wp + where_pos, &bv)) \\
                            rules[r].field = bv; \\
                        else if (json_extract_int(request, jlen, wp + where_pos, &iv)) \\
                            rules[r].field = iv; \\
                    }
                TRY_BOOL(is_active, "is_active")
                TRY_BOOL(wire_red, "wire_red")
                TRY_BOOL(wire_blue, "wire_blue")
                TRY_BOOL(wire_green, "wire_green")
                TRY_BOOL(wire_yellow, "wire_yellow")
                TRY_BOOL(actuator, "actuator")
                TRY_BOOL(inactive, "inactive")
                TRY_BOOL(invisible_block, "invisible_block")
                TRY_BOOL(invisible_wall, "invisible_wall")
                #undef TRY_BOOL
            }
            int patch_pos = json_find_key(request + elem_pos, jlen - elem_pos, "patch");
            if (patch_pos >= 0) {
                patch_pos += elem_pos;
                int32_t iv; int bv; int pp;
                #define TRY_PATCH(field, key) \\
                    pp = json_find_key(request + patch_pos, jlen - patch_pos, key); \\
                    if (pp >= 0 && !json_is_null(request, jlen, pp + patch_pos)) { \\
                        if (json_extract_bool(request, jlen, pp + patch_pos, &bv)) \\
                            rules[r].field = bv; \\
                        else if (json_extract_int(request, jlen, pp + patch_pos, &iv)) \\
                            rules[r].field = iv; \\
                    }
                TRY_PATCH(patch_wire_red, "wire_red")
                TRY_PATCH(patch_wire_blue, "wire_blue")
                TRY_PATCH(patch_wire_green, "wire_green")
                TRY_PATCH(patch_wire_yellow, "wire_yellow")
                TRY_PATCH(patch_invisible_block, "invisible_block")
                TRY_PATCH(patch_invisible_wall, "invisible_wall")
                TRY_PATCH(patch_actuator, "actuator")
                TRY_PATCH(patch_inactive, "inactive")
                #undef TRY_PATCH
                pp = json_find_key(request + patch_pos, jlen - patch_pos, "type");
                if (pp >= 0 && !json_is_null(request, jlen, pp + patch_pos)) {
                    if (json_extract_int(request, jlen, pp + patch_pos, &iv))
                        rules[r].patch_type = iv;
                }
                pp = json_find_key(request + patch_pos, jlen - patch_pos, "wall");
                if (pp >= 0 && !json_is_null(request, jlen, pp + patch_pos)) {
                    if (json_extract_int(request, jlen, pp + patch_pos, &iv))
                        rules[r].patch_wall = iv;
                }
            }
            int limit_pos = json_find_key(request + elem_pos, jlen - elem_pos, "limit");
            if (limit_pos >= 0) {
                int32_t lv;
                if (json_extract_int(request, jlen, limit_pos + elem_pos, &lv) && lv >= 0)
                    rules[r].limit = (uint32_t)lv;
            }
        }
    }

    uint32_t batch_cap = w->section_overrides[1].active
        ? w->section_overrides[1].len : (w->ends[1] - w->starts[1]);
    TxBuf tile_buf;
    buf_init(&tile_buf, batch_cap + 65536u);
    if (!tile_buf.ok) {
        tx_set_error("TERRAX_WASM_OOM", "failed to allocate tile buffer");
        return -1;
    }
    if (!rebuild_tile_section(w, &tile_buf, rules, (uint32_t)rule_count)) {
        tx_set_error("TERRAX_INTERNAL_ERROR", "tile section rebuild failed");
        return -1;
    }
    extern int set_section_override_data(TxWorld* w, int idx, uint8_t* data, uint32_t len);
    if (!set_section_override_data(w, 1, tile_buf.data, tile_buf.len)) return -1;

    uint32_t total_matched = 0, total_updated = 0;
    buf_cstr(response, "{\\"status\\":\\"ok\\",\\"rule_count\\":");
    json_u32(response, (uint32_t)rule_count);
    buf_cstr(response, ",\\"rules\\":[");
    for (int r = 0; r < rule_count; r++) {
        if (r > 0) buf_u8(response, ',');
        buf_cstr(response, "{\\"index\\":");
        json_u32(response, (uint32_t)r);
        buf_cstr(response, ",\\"matched\\":");
        json_u32(response, rules[r].matched);
        buf_cstr(response, ",\\"updated\\":");
        json_u32(response, rules[r].updated);
        buf_u8(response, '}');
        total_matched += rules[r].matched;
        total_updated += rules[r].updated;
    }
    buf_u8(response, ']');
    buf_cstr(response, ",\\"total_matched\\":");
    json_u32(response, total_matched);
    buf_cstr(response, ",\\"total_updated\\":");
    json_u32(response, total_updated);
    buf_u8(response, '}');
    return set_result_buf(response);
}

int execute_set_visibility(TxWorld* w, const char* request, int jlen,
                           TxBuf* response) {
    extern int json_find_key(const char* json, int jlen, const char* key);
    extern int json_extract_bool(const char* json, int jlen, int pos, int* out);
    int blocks_visible = 1, walls_visible = 1;
    int p = json_find_key(request, jlen, "blocks_visible");
    if (p >= 0) json_extract_bool(request, jlen, p, &blocks_visible);
    p = json_find_key(request, jlen, "walls_visible");
    if (p >= 0) json_extract_bool(request, jlen, p, &walls_visible);
    TxTileRule rules[2];
    memset(rules, 0, sizeof(rules));
    int rule_count = 0;
    if (!blocks_visible) {
        rules[rule_count].is_active = 1;
        rules[rule_count].type = -1;
        rules[rule_count].patch_invisible_block = 1;
        rule_count++;
    } else {
        rules[rule_count].invisible_block = 1;
        rules[rule_count].patch_invisible_block = 0;
        rule_count++;
    }
    if (!walls_visible) {
        rules[rule_count].wall = -1;
        rules[rule_count].patch_invisible_wall = 1;
        rule_count++;
    } else {
        rules[rule_count].invisible_wall = 1;
        rules[rule_count].patch_invisible_wall = 0;
        rule_count++;
    }
    uint32_t vis_cap = w->section_overrides[1].active
        ? w->section_overrides[1].len : (w->ends[1] - w->starts[1]);
    TxBuf tile_buf;
    buf_init(&tile_buf, vis_cap + 65536u);
    if (!rebuild_tile_section(w, &tile_buf, rules, (uint32_t)rule_count)) {
        tx_set_error("TERRAX_INTERNAL_ERROR", "tile section rebuild failed"); return -1;
    }
    extern int set_section_override_data(TxWorld* w, int idx, uint8_t* data, uint32_t len);
    if (!set_section_override_data(w, 1, tile_buf.data, tile_buf.len)) return -1;
    buf_cstr(response, "{\\"status\\":\\"ok\\",\\"blocks_visible\\":");
    json_u32(response, (uint32_t)blocks_visible);
    buf_cstr(response, ",\\"walls_visible\\":");
    json_u32(response, (uint32_t)walls_visible);
    buf_u8(response, '}');
    return set_result_buf(response);
}

int execute_remove_all_wires(TxWorld* w, const char* request, int jlen,
                             TxBuf* response) {
    (void)request; (void)jlen;
    TxTileRule rules[1];
    memset(rules, 0, sizeof(rules));
    rules[0].wire_red = -1; rules[0].wire_blue = -1;
    rules[0].wire_green = -1; rules[0].wire_yellow = -1;
    rules[0].patch_wire_red = 0; rules[0].patch_wire_blue = 0;
    rules[0].patch_wire_green = 0; rules[0].patch_wire_yellow = 0;
    uint32_t wire_cap = w->section_overrides[1].active
        ? w->section_overrides[1].len : (w->ends[1] - w->starts[1]);
    TxBuf tile_buf;
    buf_init(&tile_buf, wire_cap + 65536u);
    if (!rebuild_tile_section(w, &tile_buf, rules, 1)) {
        tx_set_error("TERRAX_INTERNAL_ERROR", "tile section rebuild failed"); return -1;
    }
    extern int set_section_override_data(TxWorld* w, int idx, uint8_t* data, uint32_t len);
    if (!set_section_override_data(w, 1, tile_buf.data, tile_buf.len)) return -1;
    buf_cstr(response, "{\\"status\\":\\"ok\\",\\"removed\\":\\"all_wires\\",\\"tile_count\\":");
    json_u32(response, rules[0].matched);
    buf_cstr(response, "}");
    return set_result_buf(response);
}
'''

with open(path, 'w', encoding='utf-8', newline='\n') as f:
    f.write(content)
print(f"Written terra_update.c successfully")