#include "terra_world.h"
#include "terra_task.h"

#include <stddef.h>
#include <stdint.h>

/* Exercise both the direct parser and the cooperative task state machine under
 * ASan/UBSan/libFuzzer. Task fuzzing intentionally stops after a bounded number
 * of work units and cancels, covering partial copy/parse/render cleanup too. */
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

    if (size >= 16u) {
        uint32_t task = terra_world_open_begin(data, (uint32_t)size);
        if (task != 0u) {
            for (uint32_t iteration = 0u; iteration < 8u; iteration++) {
                status = terra_world_open_step(task, 1u);
                if (status != TERRAX_WORLD_STATUS_IN_PROGRESS) break;
            }
            if (status == TERRAX_WORLD_STATUS_OK) {
                uint32_t task_handle = 0u;
                if (terra_world_open_finish(task, &task_handle) == TERRAX_WORLD_STATUS_OK
                        && task_handle != 0u) {
                    (void)terra_world_close(task_handle);
                }
            } else {
                (void)terra_world_open_cancel(task);
            }
            (void)terra_world_task_close(task);
        }
    }
    return 0;
}

#ifdef LIBFUZZER
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return terra_fuzz_wld_open(data, size);
}
#endif
