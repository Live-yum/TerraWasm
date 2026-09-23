#ifndef TERRA_REGIONS_H
#define TERRA_REGIONS_H
#include "terra_types.h"

enum { TX_REGION_COUNT = 14 };
/* Batch-local membership, packed using only the requested environments. */
int tx_regions_build(TxWorld*, TxTileRule*, uint32_t);
uint32_t tx_region_run(const TxWorld*, uint32_t, uint32_t, uint32_t);
uint16_t tx_region_at(const TxWorld*, uint32_t, uint32_t);
#endif
