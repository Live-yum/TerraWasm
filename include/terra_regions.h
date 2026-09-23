#ifndef TERRA_REGIONS_H
#define TERRA_REGIONS_H
#include "terra_types.h"

/* A batch-local two-bit mask: dungeon=1, jungle (including temple)=2. */
int tx_regions_build(TxWorld*, const TxTileRule*, uint32_t);
uint32_t tx_region_run(const TxWorld*, uint32_t, uint32_t, uint32_t);
uint8_t tx_region_at(const TxWorld*, uint32_t, uint32_t);
#endif
