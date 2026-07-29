#include "terra_reader.h"

#include <stddef.h>
#include <stdint.h>

/* libFuzzer-compatible body kept dependency-free for native smoke builds. */
int terra_fuzz_wld_open(const uint8_t* data, size_t size) {
    uint32_t offset = 0;
    for (size_t i = 0; i < size && i < 256; ++i) {
        const uint32_t width = (uint32_t)(data[i] & 0x1fu);
        (void)terra_reader_take(&offset, width, (uint32_t)size);
        if (offset > size) offset = (uint32_t)size;
        (void)terra_reader_take_count(&offset, data[i], width, (uint32_t)size);
        if (offset > size) offset = (uint32_t)size;
    }
    return 0;
}

#ifdef LIBFUZZER
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return terra_fuzz_wld_open(data, size);
}
#endif
