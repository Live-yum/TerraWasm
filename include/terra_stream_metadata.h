#ifndef TERRA_STREAM_METADATA_H
#define TERRA_STREAM_METADATA_H
#include "terra_types.h"
typedef struct TxStreamObject {
    uint32_t start, end, section, width, height;
    int32_t x, y;
    uint8_t replaced, third, dresser;
} TxStreamObject;
typedef struct TxStreamMetadata { TxStreamObject* objects; uint32_t count, width; uint32_t* chest_heads; uint32_t* chest_next; } TxStreamMetadata;
int tx_stream_metadata_begin(TxWorld*,TxStreamMetadata*);
void tx_stream_metadata_pixels(TxWorld*,TxStreamMetadata*,uint32_t,uint32_t);
void tx_stream_metadata_source(TxStreamMetadata*,uint32_t,uint32_t,const TxTile*,uint32_t);
int tx_stream_metadata_finish(TxWorld*,TxStreamMetadata*);
void tx_stream_metadata_discard(TxStreamMetadata*);
#endif
