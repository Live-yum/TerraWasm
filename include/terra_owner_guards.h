#ifndef TERRA_OWNER_GUARDS_H
#define TERRA_OWNER_GUARDS_H
#include <stdint.h>
/* Internal ownership guards. A live circuit session pins its source world. */
int tx_circuit_world_owns_world(uint32_t handle);
int tx_circuit_world_has_sessions(void);
int tx_circuit_world_require_unleased(uint32_t handle);
int tx_world_can_activate_stream(void);
#endif
