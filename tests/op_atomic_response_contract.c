#include "terra_world.h"
#include "terra_types.h"
#include "terra_checkpoint.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
extern TxWorld* tx_get_world(uint32_t);
extern void txw_test_fail_after(uint32_t);
extern uint32_t tx_native_heap_used(void);
extern uint64_t txw_test_checkpoint_copy_bytes(void);
static void immutable_source_operations(const char* path) {
    const char* names[] = {"header_patch", "replace_chests", "replace_bestiary", "batch_update_tiles"};
    const char* requests[] = {
        "{\"patch\":{\"worldName\":\"read only source\"}}",
        "{\"chests\":[]}",
        "{\"kills\":[],\"sightings\":[],\"chats\":[]}",
        "{\"rules\":[{\"where\":{\"type\":1},\"patch\":{\"tile_color\":1}}]}"
    };
    for (unsigned operation = 0; operation < 4; ++operation) {
        uint32_t handle = 0; uint64_t needed = 0;
        assert(terra_world_open(path, &handle) == TERRAX_WORLD_STATUS_OK);
        TxWorld* world = tx_get_world(handle);
        uint8_t* original = malloc(world->file_len); assert(original);
        memcpy(original, world->file, world->file_len);
        uint64_t copied_before = txw_test_checkpoint_copy_bytes();
        char response[2048];
        terrax_world_status status = terra_op_execute_json(handle, names[operation], requests[operation],
            response, sizeof(response), &needed);
        if (status != TERRAX_WORLD_STATUS_OK) {
            char error[512]; uint64_t error_size = 0;
            terra_info_get_last_error_json(error, sizeof(error), &error_size);
            fprintf(stderr, "%s failed: %d %s\n", names[operation], status, error);
        }
        assert(status == TERRAX_WORLD_STATUS_OK);
        assert(!tx_checkpoint_active());
        assert(txw_test_checkpoint_copy_bytes() - copied_before < world->file_len);
        assert(!memcmp(original, world->file, world->file_len));
        assert(terra_world_close(handle) == TERRAX_WORLD_STATUS_OK);
        assert(tx_native_heap_used() == 0);
        free(original);
    }
}
int main(int argc, char** argv) {
    assert(argc == 2 || argc == 3);
    if (argc == 3) immutable_source_operations(argv[2]);
    unsigned failed = 0, passed = 0;
    for (unsigned prepared = 0; prepared < 2; ++prepared) for (uint32_t fault = 0; fault < 80; ++fault) {
        uint32_t handle = 0; uint64_t needed = 0;
        assert(terra_world_open(argv[1], &handle) == TERRAX_WORLD_STATUS_OK);
        if (prepared) {
            char response[256]; uint64_t required = 0;
            assert(terra_op_execute_json(handle, "begin_output_preparation", "{\"preview_width\":64}",
                response, sizeof(response), &required) == TERRAX_WORLD_STATUS_OK);
        }
        TxWorld* world = tx_get_world(handle);
        uint8_t* original = malloc(world->file_len); assert(original);
        memcpy(original, world->file, world->file_len);
        uint64_t copied_before = txw_test_checkpoint_copy_bytes();
        char before[sizeof(world->worldName)]; memcpy(before, world->worldName, sizeof(before));
        txw_test_fail_after(fault);
        terrax_world_status status = terra_op_execute_json(handle, "header_patch",
            "{\"patch\":{\"worldName\":\"atomic response\"}}", NULL, 0, &needed);
        txw_test_fail_after(UINT32_MAX);
        assert(!tx_checkpoint_active());
        assert(!memcmp(original, world->file, world->file_len));
        assert(txw_test_checkpoint_copy_bytes() - copied_before < world->file_len &&
            "standalone header edits must not snapshot the immutable source WLD");
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
        assert(tx_native_heap_used() == 0);
        free(original);
    }
    assert(failed > 0 && passed > 0);
    printf("atomic mutation response: %u injected failures rolled back, %u success/cache fills\n", failed, passed);
    return 0;
}
