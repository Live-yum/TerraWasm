#include "terra_world.h"
#include "terra_types.h"
#include "terra_checkpoint.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
extern TxWorld* tx_get_world(uint32_t);
extern void txw_test_fail_after(uint32_t);
int main(int argc, char** argv) {
    assert(argc == 2);
    unsigned failed = 0, passed = 0;
    for (uint32_t fault = 0; fault < 80; ++fault) {
        uint32_t handle = 0; uint64_t needed = 0;
        assert(terra_world_open(argv[1], &handle) == TERRAX_WORLD_STATUS_OK);
        TxWorld* world = tx_get_world(handle);
        char before[sizeof(world->worldName)]; memcpy(before, world->worldName, sizeof(before));
        txw_test_fail_after(fault);
        terrax_world_status status = terra_op_execute_json(handle, "header_patch",
            "{\"patch\":{\"worldName\":\"atomic response\"}}", NULL, 0, &needed);
        txw_test_fail_after(UINT32_MAX);
        assert(!tx_checkpoint_active());
        if (status != TERRAX_WORLD_STATUS_OK) {
            ++failed; assert(!memcmp(before, world->worldName, sizeof(before))); assert(needed == 0);
        } else {
            ++passed; assert(!strcmp(world->worldName, "atomic response"));
            char response[2048]; assert(needed > 0 && needed <= sizeof(response));
            /* Filling the cached response must allocate nothing or mutate again. */
            txw_test_fail_after(0);
            assert(terra_op_execute_json(handle, "header_patch", "{\"patch\":{\"worldName\":\"atomic response\"}}",
                response, sizeof(response), &needed) == TERRAX_WORLD_STATUS_OK);
            txw_test_fail_after(UINT32_MAX);
        }
        assert(terra_world_close(handle) == TERRAX_WORLD_STATUS_OK);
    }
    assert(failed > 0 && passed > 0);
    printf("atomic mutation response: %u injected failures rolled back, %u success/cache fills\n", failed, passed);
    return 0;
}
