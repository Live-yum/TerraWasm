#ifndef TERRA_CIRCUIT_ACTUATION_H
#define TERRA_CIRCUIT_ACTUATION_H
#include <stdint.h>
#include "terra_types.h"
typedef struct CxWorld CxWorld;

/* Reader returns 1 for an available cell (including an empty one), 0 when its
 * sparse neighborhood has not been prepared, or a negative TCW status. */
typedef int32_t (*CxActuationTileReader)(void* context, uint32_t x, uint32_t y,
                                       TxTile* out);
/* Type-only part of DeActive. Evaluate it again after a wired type change
 * (notably ActiveStoneBlock 130 / InactiveStoneBlock 131). */
int cx_actuatable_type(uint32_t type);
/* Source DeActive eligibility only. ReActive always clears inActive separately.
 * Return 1 to allow, 0 for a source-rule protection, or a negative TCW status.
 * The simple form can inspect the current decoded column. Cross-column boulder
 * and teleporter support rules return TCW_UNSUPPORTED unless the reader form
 * supplies their two/three-cell upper neighborhood. Never interpret a negative
 * result as permission. No item drops, furniture destruction or NPC motion is
 * performed by these predicates. */
int cx_can_deactivate(CxWorld* world, uint32_t x, uint32_t y, const TxTile* tile);
int cx_can_deactivate_with_reader(CxWorld* world, uint32_t x, uint32_t y,
    const TxTile* tile, CxActuationTileReader reader, void* context);
#endif
