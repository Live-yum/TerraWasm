/* Wiring.DeActive and the applicable WorldGen.CanKillTile support predicates.
 * Source commit: 8255d34616c780af12079425ac92a0a7aed87d71.
 * Material truth values are generated from Main/TileID source-derived flags.
 */
#include "terra_circuit_world_internal.h"
#include "terra_circuit_actuation.h"
#include "terra_circuit_materials.h"

static int read_cell(CxWorld* w, int64_t x, int64_t y, CxActuationTileReader reader,
                     void* context, TxTile* out) {
    if (x < 0 || y < 0 || x >= w->width || y >= w->height) return TCW_UNSUPPORTED;
    if (reader) {
        int s = reader(context, (uint32_t)x, (uint32_t)y, out);
        return s == 1 ? TCW_OK : s < 0 ? s : TCW_UNSUPPORTED;
    }
    if (!w->column || (uint32_t)x != w->x) return TCW_UNSUPPORTED;
    *out = w->column[(uint32_t)y]; return TCW_OK;
}

static int locked_door(const TxTile* tile) {
    return tile->type == 10u && tile->frame_y >= 594 && tile->frame_y <= 646 && tile->frame_x < 54;
}

static int breakability_protected(CxWorld* w, uint32_t ignore_type, const TxTile* above) {
    if (above->type != ignore_type) {
        if (above->type == 77u && !w->world->hardMode) return 1;
        if (cx_material_has(above->type, CX_MATERIAL_PREVENTS_REMOVAL_UNDER)) return 1;
    }
    return locked_door(above) || cx_material_has(above->type, CX_MATERIAL_CONTAINER);
}

static int can_kill(CxWorld* w, uint32_t x, uint32_t y, const TxTile* tile,
                    const TxTile* above, CxActuationTileReader reader, void* context) {
    if (tile->wall == 350u) return 0;
    if (above->active) {
        uint32_t type = above->type;
        if (type != tile->type && cx_material_has(type, CX_MATERIAL_TREE_TRUNK) &&
            !(above->frame_x == 66 && above->frame_y >= 0 && above->frame_y <= 44) &&
            !(above->frame_x == 88 && above->frame_y >= 66 && above->frame_y <= 110) &&
            above->frame_y < 198) return 0;
        if (type != tile->type) {
            switch (type) {
            case 323:
                if (above->frame_x == 66 || above->frame_x == 220) return 0;
                break;
            case 21: case 26: case 72: case 77: case 88: case 467: case 488:
                return 0;
            case 80: {
                int part = above->frame_x / 18;
                if ((uint32_t)part <= 1u || (uint32_t)(part - 4) <= 1u) return 0;
                break;
            }
            default: break;
            }
        }
    }
    if (cx_material_has(tile->type, CX_MATERIAL_BOULDER)) {
        /* Exact CheckBoulderChest frame normalization and upper-cell checks. */
        int offset_x = -(tile->frame_x / 18);
        if (offset_x < -1) offset_x += 2;
        int frame_y = tile->frame_y;
        while (frame_y >= 36) frame_y -= 36;
        int64_t left = (int64_t)x + offset_x, top = (int64_t)y - frame_y / 18;
        for (uint32_t i = 0; i < 2; ++i) {
            TxTile support; int s = read_cell(w, left + i, top - 1, reader, context, &support);
            if (s < 0) return s;
            /* Unlike the teleporter case, CheckBoulderChest does not add an
             * active() guard around CheckTileBreakability_HasReasonToReturnEarly. */
            if (breakability_protected(w, tile->type, &support)) return 0;
        }
    }
    if (tile->type == 235u) {
        int64_t left = (int64_t)x - (tile->frame_x % 54) / 18;
        for (uint32_t i = 0; i < 3; ++i) {
            TxTile support; int s = read_cell(w, left + i, (int64_t)y - 1, reader, context, &support);
            if (s < 0) return s;
            if (support.active && breakability_protected(w, tile->type, &support)) return 0;
        }
    }
    /* The remaining CanKillTile inventory/locked-door branches are unreachable
     * here: containers are non-solid and door 10 is NotReallySolid. */
    return 1;
}

int cx_actuatable_type(uint32_t type) {
    return cx_material_has(type, CX_MATERIAL_SOLID) &&
        !cx_material_has(type, CX_MATERIAL_NOT_REALLY_SOLID | CX_MATERIAL_DEACTIVE_EXCLUDED);
}

int cx_can_deactivate_with_reader(CxWorld* w, uint32_t x, uint32_t y,
    const TxTile* tile, CxActuationTileReader reader, void* context) {
    if (!w || !w->world || !tile || x >= w->width || y == 0 || y >= w->height) return 0;
    if (!tile->active || !cx_actuatable_type(tile->type)) return 0;
    if (tile->type == 226u && (double)y > w->world->worldSurface && !w->world->downedPlantera) return 0;
    TxTile above; int s = read_cell(w, x, (int64_t)y - 1, reader, context, &above);
    if (s < 0) return s;
    /* This short-circuit order is observable: vanilla does not call CanKillTile
     * (including its wall and wide-support tests) when the cell above is empty. */
    if (!above.active) return 1;
    if (cx_material_has(above.type, CX_MATERIAL_PREVENTS_ACTUATION_UNDER)) return 0;
    return can_kill(w, x, y, tile, &above, reader, context);
}

int cx_can_deactivate(CxWorld* w, uint32_t x, uint32_t y, const TxTile* tile) {
    return cx_can_deactivate_with_reader(w, x, y, tile, NULL, NULL);
}
