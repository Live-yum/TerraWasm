/* Bounded, borrowed BinaryWriter strings in the active WLD header. */
#ifndef TERRA_HEADER_H
#define TERRA_HEADER_H

#include "terra_types.h"

typedef struct TxHeaderString {
    const uint8_t *encoded;
    uint32_t prefix_len;
    uint32_t len;
} TxHeaderString;

/* offset is relative to the header, including when an override is active.
 * The view is valid only while that source/override remains owned by world. */
int tx_header_string_view(const TxWorld *world,uint32_t offset,TxHeaderString *view);

#endif
