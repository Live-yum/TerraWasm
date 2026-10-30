#ifndef TERRA_REGISTRY_H
#define TERRA_REGISTRY_H

#include <stdint.h>
#include "terra_types.h"

/* Internal world-handle registry. Returned slots are borrowed and remain
 * owned by the registry; callers must not retain them beyond world lifetime. */
TxWorld* tx_world_registry_slots(void);
uint32_t tx_world_registry_next_handle(uint32_t slot_index);
TxWorld* tx_get_world(uint32_t handle);

/* Incremental task publication transfers the staging document into a fresh
 * generation without allocating or exposing the registry storage. */
terrax_world_status tx_world_adopt_open_task(TxWorld* candidate, uint32_t* out_handle);

#endif
