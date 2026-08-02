/*
 * terra_reader.h -- Overflow-safe bounded reader helpers.
 */
#ifndef TERRA_READER_H
#define TERRA_READER_H

#include <stdint.h>

int terra_reader_has(uint32_t offset, uint32_t needed, uint32_t limit);
int terra_reader_take(uint32_t* offset, uint32_t amount, uint32_t limit);
int terra_reader_take_count(uint32_t* offset, uint32_t count, uint32_t width, uint32_t limit);

#endif /* TERRA_READER_H */
