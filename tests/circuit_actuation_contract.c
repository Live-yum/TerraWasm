#include "terra_circuit_world_internal.h"
#include "terra_circuit_actuation.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Small material/support fixtures transcribed from the pinned Wiring.DeActive,
 * WorldGen.CanKillTile and CheckBoulderChest branches. The actual reader is
 * sparse: missing context is an explicit error, never an empty world cell. */
enum { SIDE = 12, X = 4, Y = 6 };
typedef struct Fixture {
    CxWorld circuit;
    TxWorld world;
    TxTile cells[SIDE][SIDE];
    int32_t missing_x, missing_y, missing_result;
} Fixture;

static TxTile tile(uint16_t type, int16_t frame_x, int16_t frame_y) {
    TxTile t = {0}; t.active = 1; t.type = type;
    t.frame_x = frame_x; t.frame_y = frame_y; return t;
}
static void reset(Fixture* f) {
    memset(f, 0, sizeof(*f));
    f->circuit.world = &f->world;
    f->circuit.width = f->circuit.height = SIDE;
    f->circuit.x = X; f->circuit.column = f->cells[X];
    f->world.worldSurface = 5;
    f->missing_x = f->missing_y = -1;
    f->cells[X][Y] = tile(1, 0, 0);
}
static int32_t read_tile(void* context, uint32_t x, uint32_t y, TxTile* out) {
    Fixture* f = context;
    assert(x < SIDE && y < SIDE);
    if ((int32_t)x == f->missing_x && (int32_t)y == f->missing_y) return f->missing_result;
    *out = f->cells[x][y]; return 1;
}
static int allow(Fixture* f) {
    return cx_can_deactivate_with_reader(&f->circuit, X, Y, &f->cells[X][Y], read_tile, f);
}

static void material_and_progression(void) {
    Fixture f; reset(&f);
    const uint16_t allowed[] = {0, 1, 19, 130, 138, 235, 750};
    const uint16_t blocked[] = {10, 21, 88, 131, 314, 379, 386, 387, 388, 389, 420, 476, 65535};
    for (uint32_t i = 0; i < sizeof(allowed)/sizeof(allowed[0]); ++i) {
        f.cells[X][Y].type = allowed[i];
        assert(cx_actuatable_type(allowed[i]) == 1 && allow(&f) == 1);
    }
    for (uint32_t i = 0; i < sizeof(blocked)/sizeof(blocked[0]); ++i) {
        f.cells[X][Y].type = blocked[i];
        assert(cx_actuatable_type(blocked[i]) == 0 && allow(&f) == 0);
    }
    assert(cx_actuatable_type(UINT32_MAX) == 0);
    f.cells[X][Y] = tile(226, 0, 0);
    assert(allow(&f) == 0);
    f.world.worldSurface = Y;
    assert(allow(&f) == 1); /* Comparison is strictly below the surface. */
    f.world.worldSurface = Y - 1;
    f.world.downedPlantera = 1;
    assert(allow(&f) == 1);
    f.cells[X][Y].active = 0;
    assert(allow(&f) == 0);
}

static void direct_support_and_short_circuit(void) {
    Fixture f; reset(&f);
    const uint16_t protected[] = {21, 467, 26, 77, 88, 470, 475, 237, 597, 441, 468};
    for (uint32_t i = 0; i < sizeof(protected)/sizeof(protected[0]); ++i) {
        f.cells[X][Y-1] = tile(protected[i], 0, 0);
        assert(allow(&f) == 0);
        /* active() and inActive() are separate source flags. */
        f.cells[X][Y-1].inactive = 1;
        assert(allow(&f) == 0);
        f.cells[X][Y-1].active = 0;
        assert(allow(&f) == 1);
    }
    f.cells[X][Y].wall = 350;
    assert(allow(&f) == 1); /* Empty above skips all of CanKillTile. */
    f.cells[X][Y-1] = tile(1, 0, 0);
    assert(allow(&f) == 0);
    f.cells[X][Y].wall = 349;
    assert(allow(&f) == 1);
    f.cells[X][Y-1] = tile(488, 0, 0);
    assert(allow(&f) == 0);
    f.cells[X][Y-1] = tile(26, 0, 0);
    f.cells[X][Y].type = 26;
    assert(allow(&f) == 0); /* PreventsActuationUnder precedes ignoreType. */
}

static void tree_palm_and_cactus_frames(void) {
    Fixture f; reset(&f);
    const uint16_t trunks[] = {5, 72, 583, 584, 585, 586, 587, 588, 589, 596, 616, 634};
    for (uint32_t i = 0; i < sizeof(trunks)/sizeof(trunks[0]); ++i) {
        f.cells[X][Y-1] = tile(trunks[i], 0, 0);
        assert(allow(&f) == 0);
        f.cells[X][Y-1].frame_x = 66; f.cells[X][Y-1].frame_y = 44;
        /* Mushroom tree 72 has its own unconditional branch after the trunk
         * exceptions; all other trunk types use the three frame exceptions. */
        assert(allow(&f) == (trunks[i] == 72 ? 0 : 1));
        f.cells[X][Y-1].frame_y = 45;
        assert(allow(&f) == 0);
        f.cells[X][Y-1].frame_x = 88; f.cells[X][Y-1].frame_y = 66;
        assert(allow(&f) == (trunks[i] == 72 ? 0 : 1));
        f.cells[X][Y-1].frame_y = 110;
        assert(allow(&f) == (trunks[i] == 72 ? 0 : 1));
        f.cells[X][Y-1].frame_y = 111;
        assert(allow(&f) == 0);
        f.cells[X][Y-1].frame_x = 0; f.cells[X][Y-1].frame_y = 198;
        assert(allow(&f) == (trunks[i] == 72 ? 0 : 1));
    }
    f.cells[X][Y-1] = tile(323, 66, 0); assert(allow(&f) == 0);
    f.cells[X][Y-1].frame_x = 220; assert(allow(&f) == 0);
    f.cells[X][Y-1].frame_x = 44; assert(allow(&f) == 1);
    for (int16_t part = -2; part < 8; ++part) {
        f.cells[X][Y-1] = tile(80, (int16_t)(part * 18), 0);
        assert(allow(&f) == (part == 0 || part == 1 || part == 4 || part == 5 ? 0 : 1));
    }
}

static void boulder_neighborhood(void) {
    Fixture f; reset(&f);
    f.cells[X][Y] = tile(138, 18, 18); /* Right bottom part, top-left (3,5). */
    f.cells[X][Y-1] = tile(138, 18, 0);
    assert(allow(&f) == 1);
    assert(cx_can_deactivate(&f.circuit, X, Y, &f.cells[X][Y]) == TCW_UNSUPPORTED);
    f.cells[X-1][Y-2] = tile(21, 0, 0);
    assert(allow(&f) == 0);
    f.cells[X-1][Y-2].active = 0;
    assert(allow(&f) == 0); /* The source helper has no active() guard here. */
    f.cells[X-1][Y-2] = tile(0, 0, 0);
    f.cells[X][Y-2] = tile(475, 0, 0);
    assert(allow(&f) == 0);
    f.cells[X][Y-2] = tile(77, 0, 0);
    assert(allow(&f) == 0);
    f.world.hardMode = 1;
    assert(allow(&f) == 1);
    f.cells[X][Y-2] = tile(10, 0, 594);
    assert(allow(&f) == 0);
    f.cells[X][Y-2].frame_y = 646; assert(allow(&f) == 0);
    f.cells[X][Y-2].frame_y = 647; assert(allow(&f) == 1);
    f.cells[X][Y-2].frame_y = 594; f.cells[X][Y-2].frame_x = 54;
    assert(allow(&f) == 1);
    f.cells[X][Y-2] = tile(488, 0, 0); assert(allow(&f) == 0);
    f.cells[X][Y-2] = tile(138, 0, 0); assert(allow(&f) == 1);
    /* Horizontal alternate sprite and vertical variant normalize identically. */
    f.cells[X][Y].frame_x = 54; f.cells[X][Y].frame_y = 54;
    f.cells[X-1][Y-2] = tile(441, 0, 0); assert(allow(&f) == 0);
    f.cells[X-1][Y-2] = tile(0, 0, 0);
    f.missing_x = X-1; f.missing_y = Y-2;
    assert(allow(&f) == TCW_UNSUPPORTED);
    f.missing_result = TCW_IO; assert(allow(&f) == TCW_IO);
}

static void teleporter_neighborhood(void) {
    Fixture f; reset(&f);
    f.cells[X][Y] = tile(235, 36, 0); /* Right part, upper footprint x2..4. */
    f.cells[X][Y-1] = tile(1, 0, 0);
    assert(allow(&f) == 1);
    assert(cx_can_deactivate(&f.circuit, X, Y, &f.cells[X][Y]) == TCW_UNSUPPORTED);
    f.cells[X-2][Y-1] = tile(468, 0, 0); assert(allow(&f) == 0);
    f.cells[X-2][Y-1].active = 0; assert(allow(&f) == 1);
    f.cells[X-1][Y-1] = tile(323, 0, 0); assert(allow(&f) == 0);
    f.cells[X-1][Y-1] = tile(77, 0, 0); assert(allow(&f) == 0);
    f.world.hardMode = 1; assert(allow(&f) == 1);
    f.cells[X-1][Y-1] = tile(10, 53, 612); assert(allow(&f) == 0);
    f.cells[X][Y-1].active = 0;
    assert(allow(&f) == 1); /* DeActive's empty-above shortcut still precedes it. */
    f.cells[X][Y-1].active = 1;
    f.cells[X-1][Y-1] = tile(235, 0, 0);
    assert(allow(&f) == 1);
    f.missing_x = X-2; f.missing_y = Y-1;
    assert(allow(&f) == TCW_UNSUPPORTED);
}

static void arguments_and_column_reader(void) {
    Fixture f; reset(&f);
    assert(cx_can_deactivate(&f.circuit, X, Y, &f.cells[X][Y]) == 1);
    assert(cx_can_deactivate(NULL, X, Y, &f.cells[X][Y]) == 0);
    assert(cx_can_deactivate(&f.circuit, X, Y, NULL) == 0);
    assert(cx_can_deactivate(&f.circuit, SIDE, Y, &f.cells[X][Y]) == 0);
    assert(cx_can_deactivate(&f.circuit, X, SIDE, &f.cells[X][Y]) == 0);
    assert(cx_can_deactivate(&f.circuit, X, 0, &f.cells[X][Y]) == 0);
    assert(cx_can_deactivate(&f.circuit, X+1, Y, &f.cells[X][Y]) == TCW_UNSUPPORTED);
    f.circuit.column = NULL;
    assert(cx_can_deactivate(&f.circuit, X, Y, &f.cells[X][Y]) == TCW_UNSUPPORTED);
    f.circuit.world = NULL;
    assert(allow(&f) == 0);
}

int main(void) {
    material_and_progression(); direct_support_and_short_circuit();
    tree_palm_and_cactus_frames(); boulder_neighborhood();
    teleporter_neighborhood(); arguments_and_column_reader();
    puts("circuit actuation: source material/progression, support/frame protections, boulder/teleporter neighborhoods and sparse-read failures passed");
    return 0;
}
