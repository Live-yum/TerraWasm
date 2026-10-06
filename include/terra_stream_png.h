#ifndef TERRA_STREAM_PNG_H
#define TERRA_STREAM_PNG_H
#include "terra_map.h"
typedef struct TxStreamPng TxStreamPng;
/* Full-resolution only. Marker arrays/world remain borrowed until free.
 * range may repeat a strip for legacy marker painting. Drain/ack output before
 * requesting the next strip; returned bytes are immutable until ack. */
TxStreamPng* tx_stream_png_begin(TxWorld*, uint32_t, uint32_t,
    const MapMarkerEntry*, uint32_t, const MapMarkerEntry*, uint32_t);
int tx_stream_png_range(TxStreamPng*, uint32_t*, uint32_t*);
int tx_stream_png_run(TxStreamPng*, uint32_t, uint32_t, const TxTile*, uint32_t);
/* Fill the active base strip from row-major RGB; returns 0 for marker pass. */
int tx_stream_png_marker_pass(const TxStreamPng*);
int tx_stream_png_rgb(TxStreamPng*, const uint8_t*);
int tx_stream_png_finish_strip(TxStreamPng*);
int tx_stream_png_pull(TxStreamPng*, uint32_t*, const uint8_t**, uint32_t*);
int tx_stream_png_ack(TxStreamPng*);
void tx_stream_png_free(TxStreamPng*);
#endif
