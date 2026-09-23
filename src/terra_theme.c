#include "terra_theme.h"

/* Style grids come from Terraria 1.4.5.8 TileObjectData and item placements.
 * A style occupies period_x/period_y pixels; zero preserves that axis (state).
 * Wrapped door/chandelier grids retain their orientation/light-state lane. */
typedef struct {
    uint16_t source, period_x, period_y;
    uint16_t target[3], x[3], y[3];
} ThemeFurniture;
#include "terra_theme_data.inc"

static int protected_furniture(const TxTile* t) {
    int style = t->frame_x / 36;
    if (t->type == 21 && (style == 2 || style == 4 ||
        (style >= 23 && style <= 27) || style == 36 || style == 38 || style == 40)) return 1;
    if (t->type == 467 && style == 13) return 1;
    /* Locked temple doors and one-row Dynasty tables have no compatible skin. */
    if (t->type == 10 && t->frame_y >= 594 && t->frame_y <= 646) return 1;
    return t->type == 14 && t->frame_x / 54 == 25;
}

static void theme_furniture(TxTile* t, int theme) {
    if (t->type >= sizeof(THEME_FURNITURE_INDEX) || !THEME_FURNITURE_INDEX[t->type]) return;
    if (t->frame_x < 0 || t->frame_y < 0 || protected_furniture(t)) return;
    const ThemeFurniture* f = &THEME_FURNITURE[THEME_FURNITURE_INDEX[t->type] - 1];
    t->frame_x = f->x[theme] + (f->period_x ? t->frame_x % f->period_x : t->frame_x);
    t->frame_y = f->y[theme] + (f->period_y ? t->frame_y % f->period_y : t->frame_y);
    t->type = f->target[theme];
}

void tx_apply_theme(TxTile* t, int terrain, int wall, int furniture) {
    static const uint16_t blocks[] = {396, 147, 60};
    static const uint16_t beams[] = {577, 574, 575};
    static const uint16_t safe_walls[] = {34, 31, 42};
    static const uint16_t wild_walls[] = {187, 71, 64};
    if (terrain > 0 && t->active && t->type < sizeof(THEME_TERRAIN) && THEME_TERRAIN[t->type]) {
        t->type = THEME_TERRAIN[t->type] == 2 ? beams[terrain - 1] : blocks[terrain - 1];
        t->frame_x = t->frame_y = 0;
    }
    if (wall > 0 && t->wall && t->wall < sizeof(THEME_HOUSE_WALL))
        t->wall = THEME_HOUSE_WALL[t->wall] ? safe_walls[wall - 1] : wild_walls[wall - 1];
    if (furniture > 0 && t->active) theme_furniture(t, furniture - 1);
}
