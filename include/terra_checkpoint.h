#ifndef TERRA_CHECKPOINT_H
#define TERRA_CHECKPOINT_H
#include <stdint.h>
/* Internal single-owner allocator transaction. Only native-domain allocations
 * and explicitly selected persistent world roots belong to this transaction. */
uint32_t tx_checkpoint_bytes(void* const* persistent_roots, uint32_t count);
int tx_checkpoint_begin(void* const* persistent_roots, uint32_t count, uint32_t max_bytes);
int tx_checkpoint_finish(int rollback);
int tx_checkpoint_active(void);
/* Internal operation-only variant. The caller proves that readonly_root bytes
 * are immutable until finish. Its allocation is still pinned/deferred-free;
 * only the redundant byte copy is omitted. Public workspace snapshots remain
 * complete so their existing in-place rollback contract is unchanged. */
uint32_t tx_checkpoint_bytes_readonly(void* const* persistent_roots, uint32_t count, const void* readonly_root);
int tx_checkpoint_begin_readonly(void* const* persistent_roots, uint32_t count, uint32_t max_bytes, const void* readonly_root);
#endif
