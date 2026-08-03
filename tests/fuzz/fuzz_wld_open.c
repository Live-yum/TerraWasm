#include "terra_world.h"

#include <stddef.h>
#include <stdint.h>

/* Exercise the real parser entry point under ASan/UBSan and libFuzzer. */
int terra_fuzz_wld_open(const uint8_t* data, size_t size) {
    if (!data || size > UINT32_MAX) return 0;

    uint32_t handle = 0;
    terrax_world_status status = terra_world_open_from_buffer(
        data,
        (uint32_t)size,
        &handle);
    if (status == TERRAX_WORLD_STATUS_OK && handle != 0u) {
        (void)terra_world_close(handle);
    }
    return 0;
}

#ifdef LIBFUZZER
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return terra_fuzz_wld_open(data, size);
}
#endif
