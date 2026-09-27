#ifndef TERRA_STREAM_MAP_H
#define TERRA_STREAM_MAP_H
#include "terra_map.h"
typedef struct TxStreamMap TxStreamMap;
TxStreamMap* tx_stream_map_begin(TxWorld*,const MapMarkerEntry*,uint32_t,const MapMarkerEntry*,uint32_t);
int tx_stream_map_range(TxStreamMap*,uint32_t*,uint32_t*);
int tx_stream_map_run(TxStreamMap*,uint32_t,uint32_t,const TxTile*,uint32_t);
int tx_stream_map_finish_strip(TxStreamMap*);
int tx_stream_map_pull(TxStreamMap*,uint32_t*,const uint8_t**,uint32_t*);
int tx_stream_map_ack(TxStreamMap*);
uint32_t tx_stream_map_size(TxStreamMap*);
void tx_stream_map_free(TxStreamMap*);
#endif
