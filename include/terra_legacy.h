#ifndef TERRA_LEGACY_H
#define TERRA_LEGACY_H

#include <stdint.h>

/* Same name-to-net-ID conversion as NPCID.FromLegacyName. Unknown names are 0. */
int32_t tx_legacy_npc_id(const char *name);

#endif
