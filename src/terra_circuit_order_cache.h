#ifndef TERRA_CIRCUIT_ORDER_CACHE_H
#define TERRA_CIRCUIT_ORDER_CACHE_H
#include "terra_circuit_vm.h"
#include <stddef.h>
#include <stdint.h>
typedef struct CxOrderCache CxOrderCache;
typedef struct CxOrderCacheKey {
    uint32_t source_gate,sx,sy,sw,sh,mask,count;
} CxOrderCacheKey;
enum { CX_ORDER_CACHE_MORE=0, CX_ORDER_CACHE_MISS=1, CX_ORDER_CACHE_HIT=2 };
/* Borrowed arena only. Must remain exclusive to one immutable live world and
 * its source lease. Reinitialization is required for any new owner/session.
 * No allocation, freeing, owner dereference, or runtime whole-table reset. */
size_t cx_order_cache_required_bytes(void);
size_t cx_order_cache_alignment(void);
CxOrderCache* cx_order_cache_init(void* arena,size_t bytes,const void* owner);
/* Arena must be initialized or zero-filled storage. Read-only owner check. */
CxOrderCache* cx_order_cache_for_owner(void* arena,size_t bytes,const void* owner);
/* begin while an operation is active conservatively cancels all records.
 * Inputs must remain unchanged until lookup completes; output mutation starts
 * only after HIT, or through the original ordering path after MISS. */
void cx_order_cache_begin(CxOrderCache*,const CxOrderCacheKey*);
/* One work unit: one input snapshot, scalar comparison, record admission, or
 * member comparison. Zero work makes no progress. MISS for >256/disabled is
 * immediate and does NOT promise a snapshot. Other MISS has complete input. */
int cx_order_cache_lookup_step(CxOrderCache*,const TerraVmGateRef*,uint32_t work);
/* One work unit copies one output. HIT means all output copied. */
int cx_order_cache_copy_step(CxOrderCache*,TerraVmGateRef*,uint32_t work);
/* MISS path only: original successful phase-7 output, ascending exactly once.
 * complete publishes only after count captures; errors must call cancel. */
void cx_order_cache_capture_output(CxOrderCache*,uint32_t,TerraVmGateRef);
void cx_order_cache_complete(CxOrderCache*);
/* O(1) epoch invalidation, including partial hit copies. Caller/VM owns rollback
 * of any output already copied. Epoch exhaustion permanently disables cache. */
void cx_order_cache_cancel(CxOrderCache*);
#endif
