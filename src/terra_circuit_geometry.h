#ifndef TERRA_CIRCUIT_GEOMETRY_H
#define TERRA_CIRCUIT_GEOMETRY_H
#include "terra_circuit_world_internal.h"
/* Internal immutable source geometry for exact ordering.
 * Packed fields: wires[0:3], BFS route[4:5], wired lamp[6], active420 gate[7].
 * The source lease is the owning CxWorld session. No ownership transfer. */
#define CX_GEOMETRY_SLOTS 3u
#define CX_GEOMETRY_WINDOW 65536u
#define CX_GEOMETRY_COLD_BYTES (2u*1024u*1024u)
#define CX_GEOMETRY_ORDER_CACHE_BYTES (384u*1024u)
/* The control build disables only order-cache use, never this partition. */
#ifndef CX_ORDER_CACHE_ENABLED
#define CX_ORDER_CACHE_ENABLED 1
#endif
#define CX_GEOMETRY_COLD_ENTRIES 4096u
#define CX_GEOMETRY_COLD_WAYS 4u
enum { CX_GEOMETRY_READY=0, CX_GEOMETRY_MORE=1, CX_GEOMETRY_READ=2 };
typedef struct CxGeometryRead { uint32_t ticket,source,offset,length; } CxGeometryRead;
typedef struct CxGeometryStats { uint64_t hits,misses,evictions,decoded_records,read_requests;
    uint64_t cold_raw9_fills,cold_palette_fills,cold_hits,cold_fills,cold_raw_fills,cold_rle_fills,cold_overlap_evictions,cold_conflict_evictions,cold_oversize; } CxGeometryStats;
int cx_geometry_stats(CxWorld*,CxGeometryStats*);
int cx_geometry_create(CxWorld*);
/* Borrowed, initially zeroed, max_align_t-aligned owner-local storage. Valid
 * until geometry destruction; never free it. NULL/0 when cold is disabled.
 * Cancellation does not clear the tail: the order module owns invalidation. */
void* cx_geometry_order_cache_arena(CxWorld*,uint32_t* size);
void cx_geometry_destroy(CxWorld*);
void cx_geometry_cancel(CxWorld*);
int cx_geometry_begin(CxWorld*,uint32_t column);
int cx_geometry_point_begin(CxWorld*,uint32_t column,uint32_t row);
int cx_geometry_point_get(CxWorld*,uint32_t column,uint32_t row,uint8_t*);
int cx_geometry_step(CxWorld*,uint32_t work,CxGeometryRead*);
int cx_geometry_supply(CxWorld*,const CxGeometryRead*,const uint8_t*,uint32_t);
int cx_geometry_get_connectivity(CxWorld*,uint32_t column,uint32_t row,uint8_t*,uint8_t*);
int cx_geometry_get(CxWorld*,uint32_t column,uint32_t row,uint8_t*);
#endif
