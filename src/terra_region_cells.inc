/* Terraria 1.4.5.8 SceneMetrics.AggregateTileCounts / WorldGen.oceanDepths.
 * These flags describe one cell; region membership is determined by its neighborhood. */
enum { JUNGLE, DUNGEON, DESERT, SNOW, CORRUPT, CRIMSON, HALLOW, MUSHROOM,
       FLOWERS, METEOR, GRAVES, METRIC_COUNT };
#define C(n) (1u << (n))
enum { DUNGEON_WALL=1u<<11, TEMPLE=1u<<12, DESERT_WALL=1u<<13 };
enum { R_DUNGEON=1, R_JUNGLE=2, R_DESERT=4, R_SNOW=8, R_CORRUPT=16,
       R_CRIMSON=32, R_HALLOW=64, R_MUSHROOM=128, R_OCEAN=256, R_HELL=512,
       R_FOREST=1024, R_SPACE=2048, R_UNDERGROUND=4096, R_CAVERN=8192,
       DUAL_STOP=32768 };

static int ocean_at(const TxWorld* w, uint32_t x, uint32_t y) {
    return y <= (w->worldSurface + w->rockLayer) / 2.0 + 40.0 &&
           (x < 380u || (int32_t)x > w->maxTilesX - 380);
}

static uint32_t tile_counts(const TxWorld* w, uint16_t type) {
    switch (type) {
    case 60: case 61: case 62: case 74: case 225: return C(JUNGLE);
    case 226: return w->remixWorld ? 0 : C(JUNGLE);
    case 41: case 43: case 44: case 481: case 482: case 483: return C(DUNGEON);
    case 53: case 396: case 397: return C(DESERT);
    case 112: case 398: case 400: return C(DESERT)|C(CORRUPT);
    case 234: case 399: case 401: return C(DESERT)|C(CRIMSON);
    case 116: case 402: case 403: return C(DESERT)|C(HALLOW);
    case 147: case 148: case 161: case 162: return C(SNOW);
    case 163: return C(SNOW)|C(CORRUPT);
    case 200: return C(SNOW)|C(CRIMSON);
    case 164: return C(SNOW)|C(HALLOW);
    case 23: case 661: case 24: case 25: case 32: return C(CORRUPT);
    case 199: case 662: case 201: case 203: case 352: return C(CRIMSON);
    case 474: return w->remixWorld ? C(CORRUPT) : 0;
    case 195: return w->remixWorld ? C(CRIMSON) : 0;
    case 109: case 492: case 110: case 113: case 117: return C(HALLOW);
    case 70: case 71: case 72: case 528: return C(MUSHROOM);
    case 27: return C(FLOWERS);
    case 37: return C(METEOR);
    case 85: return C(GRAVES);
    default: return 0;
    }
}

static int jungle_wall(uint16_t wall) {
    return wall == 15 || wall == 64 || (wall >= 204 && wall <= 207);
}

static int desert_wall(uint16_t wall) {
    return wall == 187 || (wall >= 216 && wall <= 223) || wall == 275 ||
           (wall >= 304 && wall <= 310);
}

/* NPCSpawningFlagsForDualDungeons filtered through SolidTile3: plants,
 * vines and platforms cannot stop the downward search. */
static uint16_t dual_candidate(const TxTile* t, uint16_t above, double rock, uint32_t y) {
    if (!t->active || t->inactive) return 0;
    uint16_t region = 0;
    switch (t->type) {
    case 60: case 225: case 383: case 384: region = R_JUNGLE; break;
    case 41: case 43: case 44: case 481: case 482: case 483: region = y > rock ? R_DUNGEON : DUAL_STOP; break;
    case 226: region = R_JUNGLE; break;
    case 59: case 120: region = jungle_wall(above) ? R_JUNGLE : 0; break;
    case 1: case 38: region = above == 349 ? DUAL_STOP : 0; break;
    case 123: region = above == 349 ? DUAL_STOP : jungle_wall(above) || above == 87 ? R_JUNGLE : 0; break;
    case 22: case 140:
        if (above == 69 || above == 217 || above == 220 || above == 3 || above == 233) region = R_CORRUPT;
        break;
    case 204: case 347:
        if (above == 81 || above == 218 || above == 221 || above == 83 || above == 77) region = R_CRIMSON;
        break;
    case 109: case 116: case 117: case 118: case 164: case 385: case 402: case 403: case 492: region = R_HALLOW; break;
    case 147: case 148: case 161: case 162: case 206: case 224: region = R_SNOW; break;
    case 191: region = DUAL_STOP; break;
    case 23: case 25: case 112: case 398: case 400: case 474: case 661: region = R_CORRUPT; break;
    case 163: region = R_CORRUPT|R_SNOW; break;
    case 200: region = R_CRIMSON|R_SNOW; break;
    case 195: case 199: case 203: case 234: case 399: case 401: case 662: region = R_CRIMSON; break;
    case 70: region = R_MUSHROOM; break;
    }
    if ((t->type == 53 || t->type == 112 || t->type == 116 || t->type == 234 ||
         (t->type >= 396 && t->type <= 403)) && desert_wall(above)) region |= R_DESERT;
    return region;
}
