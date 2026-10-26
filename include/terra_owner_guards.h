#ifndef TERRA_OWNER_GUARDS_H
#define TERRA_OWNER_GUARDS_H
#include <stdint.h>
/* Internal ownership guards. A live circuit session pins its source world. */
int tx_circuit_world_owns_world(uint32_t handle);
int tx_circuit_world_has_sessions(void);
int tx_circuit_world_require_unleased(uint32_t handle);
int tx_world_can_activate_stream(void);
/* A finished open-task handle may remain queryable without owning a world.
 * Its reset pin still lasts until task_close; do not use this for slot reuse. */
int tx_open_task_has_world_owner(void);
/* Pending owners can exist before a public world handle. Keep the allocator
 * independent of feature modules while protecting their roots from reset. */
void tx_heap_reset_pin(void);
void tx_heap_reset_unpin(void);
#endif
