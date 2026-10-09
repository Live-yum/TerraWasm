#ifndef TERRA_OUTPUT_H
#define TERRA_OUTPUT_H
#include "terra_map.h"

typedef struct TxPreparedMap TxPreparedMap;
struct TxPreparedOutput {
    uint8_t *rgb, *list_rgba, *preview_rgba;
    uint32_t width, height, list_width, list_height;
    uint32_t preview_width, preview_height;
    /* Integer coordinate projection is identical for every run in a column.
     * Both optional row tables share the allocation owned by list_rows. */
    uint32_t *list_rows, *preview_rows;
    uint32_t projected_x, list_x, preview_x;
    MapMarkerEntry markers[256];
    uint32_t marker_count;
    TxBuf points;
    TxPreparedMap* map;
    uint32_t ready, source_runs;
};

typedef struct TxMarkerScan TxMarkerScan;
TxMarkerScan* tx_marker_stream_begin(TxWorld*, const MapMarkerEntry*, uint32_t);
int tx_marker_stream_run(TxMarkerScan*, uint32_t, uint32_t, const TxTile*, uint32_t);
int tx_marker_stream_column(TxMarkerScan*);
int tx_marker_stream_finish(TxMarkerScan*, TxBuf*);
void tx_marker_stream_free(TxMarkerScan*);

typedef int (*TxTileVisitor)(TxWorld*, uint32_t, uint32_t, TxTile*, uint32_t, void*);
int tx_scan_tile_markers(TxWorld*, const MapMarkerEntry*, uint32_t, TxBuf*, TxTileVisitor, void*);
int tx_output_begin(TxWorld*, const MapMarkerEntry*, uint32_t, int, uint32_t);
int tx_output_scan(TxWorld*, TxTileRule*, uint32_t, TxBuf*);
int tx_output_stream_run(TxWorld*, uint32_t, uint32_t, TxTile*, uint32_t);
void tx_output_stream_begin_slice(void);
void tx_output_stream_finish(TxWorld*);
void tx_output_free(TxPreparedOutput*);
void tx_output_clear(TxWorld*);
void tx_apply_tile_rules(TxTile*, TxTileRule*, uint32_t, uint32_t, uint16_t, uint32_t, double);
uint8_t* tx_output_take_rgb(TxWorld*, uint32_t, uint32_t);
int tx_output_copy_rows(TxWorld*, uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t);
TxPreparedMap* tx_map_base_begin(TxWorld*);
int tx_map_base_run(TxWorld*, TxPreparedMap*, uint32_t, uint32_t, const TxTile*, uint32_t);
int tx_map_base_strip(TxPreparedMap*, uint32_t, uint32_t*);
void tx_map_base_free(TxPreparedMap*);

extern uint8_t* tx_persistent_alloc(uint32_t);
extern void* tx_persistent_realloc(void*, uint32_t);
extern void tx_persistent_free(void*);
#endif
