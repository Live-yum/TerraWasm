#ifndef TERRA_CIRCUIT_ORDER_REPLAY_H
#define TERRA_CIRCUIT_ORDER_REPLAY_H
#include "terra_circuit_world_internal.h"
/* Private, session-borrowing connectivity replay. No ownership transfer and no
 * mutation of the compiler/query cursor or its buffers. Destroy before owner. */
typedef struct CxOrderReplay CxOrderReplay;
/* Return 0 ready, 1 bounded progress, 2 external read, or a negative status.
 * compiler_route: 0 ordinary; 1 axis; 2 left/up joined; 3 swapped axes.
 * It is deliberately separate from BFS routing (notably raw type 0xfffe). */
typedef int (*CxOrderReplayGeometry)(void*,uint32_t,uint32_t,uint8_t*,uint8_t*);
int cx_order_replay_create(CxWorld*,CxOrderReplay**);
void cx_order_replay_destroy(CxWorld*,CxOrderReplay*);
void cx_order_replay_cancel(CxOrderReplay*);
int cx_order_replay_begin(CxOrderReplay*,uint32_t,uint32_t);
/* Returns 0 complete, 1 more, 2 external read, or negative status. Output is
 * written only on completion. A yielded callback must be resumed by its owner
 * before another lookup is begun; cancel does not cancel external source IO. */
int cx_order_replay_step(CxWorld*,CxOrderReplay*,uint32_t,CxOrderReplayGeometry,void*,uint32_t[4]);
/* Batch visitors observe already committed cells in x/colour/row order.
 * Return 0 when the cell is fully handled, 1 for bounded progress, or a negative
 * status. Return 1 only with caller-owned resumable visitor state: replay invokes
 * the SAME cell again, without re-reading geometry or consuming another label.
 * A visitor cannot request external IO (return 2 is rejected). */
typedef int (*CxOrderReplayVisitor)(void*,uint32_t,uint32_t,uint32_t,uint32_t,uint8_t);
/* Replay starts at max_x/64's checkpoint. max_y is the greatest candidate row
 * AT max_x, not the greatest row across earlier columns. Earlier columns finish
 * all four colours and all rows; the final cell's visitor must finish as well.
 * These APIs are private and do not allocate candidate or world-wide indices. */
int cx_order_replay_begin_batch(CxOrderReplay*,uint32_t max_x,uint32_t max_y);
int cx_order_replay_step_batch(CxWorld*,CxOrderReplay*,uint32_t,CxOrderReplayGeometry,void*,CxOrderReplayVisitor,void*);
#endif
