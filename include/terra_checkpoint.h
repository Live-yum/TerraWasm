#ifndef TERRA_CHECKPOINT_H
#define TERRA_CHECKPOINT_H
#include <stdint.h>
/* Internal single-owner allocator transaction. Only native-domain allocations
 * and explicitly selected persistent world roots belong to this transaction. */
uint32_t tx_checkpoint_bytes(void* const* persistent_roots, uint32_t count);
int tx_checkpoint_begin(void* const* persistent_roots, uint32_t count, uint32_t max_bytes);
int tx_checkpoint_finish(int rollback);
int tx_checkpoint_active(void);
#endif
