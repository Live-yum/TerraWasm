/*
 * terra_reader.c -- Overflow-safe bounded reader helpers.
 */
#include "terra_reader.h"

#include <stdint.h>

int terra_reader_has(uint32_t offset, uint32_t needed, uint32_t limit) {
    return offset <= limit && needed <= limit - offset;
}

int terra_reader_take(uint32_t* offset, uint32_t amount, uint32_t limit) {
    if (!offset || !terra_reader_has(*offset, amount, limit)) {
        if (offset) *offset = limit;
        return 0;
    }
    *offset += amount;
    return 1;
}

int terra_reader_take_count(uint32_t* offset, uint32_t count, uint32_t width, uint32_t limit) {
    if (width != 0u && count > UINT32_MAX / width) {
        if (offset) *offset = limit;
        return 0;
    }
    return terra_reader_take(offset, count * width, limit);
}
