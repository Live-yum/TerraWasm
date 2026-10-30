#ifndef TERRA_REGISTRY_H
#define TERRA_REGISTRY_H

#include <stdint.h>
#include "terra_types.h"

/* Registry-owned slot acquisition and active-world visitation. Slot pointers
 * are borrowed document views; the slot table itself never leaves this module. */
TxWorld* tx_world_registry_claim_slot(void);
TxWorld* tx_world_registry_primary_slot(void);
uint32_t tx_world_registry_active_heap_mark(void);
void tx_world_registry_visit_active(void (*visitor)(TxWorld*, void*), void* context);
uint32_t tx_world_registry_next_handle(uint32_t slot_index);
TxWorld* tx_get_world(uint32_t handle);

/* Incremental task publication transfers the staging document into a fresh
 * generation without allocating or exposing the registry storage. */
terrax_world_status tx_world_adopt_open_task(TxWorld* candidate, uint32_t* out_handle);

#endif
