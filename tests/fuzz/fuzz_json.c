#include "terra_reader.h"

#include <stddef.h>
#include <stdint.h>

/* Exercises bounded length/count arithmetic with arbitrary JSON-like bytes. */
int terra_fuzz_json(const uint8_t* data, size_t size) {
    uint32_t offset = 0;
    for (size_t i = 0; i < size && i < 256; ++i) {
        const uint32_t count = (uint32_t)data[i];
        const uint32_t width = (uint32_t)((data[i] >> 5) & 0x07u);
        (void)terra_reader_take_count(&offset, count, width, (uint32_t)size);
        if (offset > size) offset = (uint32_t)size;
    }
    return 0;
}
