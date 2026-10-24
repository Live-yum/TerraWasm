/* Acquire real owners first, then deny every new allocation. All scenarios run
 * repeatedly in this one allocator/process; no reset or larger limit hides leaks. */
#include "terra_world.h"
#include "terra_types.h"
#include "terra_outcome.h"
#include "terra_stream.h"
#include "terra_pixel_workspace.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint32_t tx_heap_used(void);
extern uint32_t tx_native_heap_used(void);
extern uint32_t tx_bridge_heap_used(void);
extern uint64_t txw_test_allocation_attempts(void);
extern void txw_test_fail_after(uint32_t);
extern void txw_test_live_allocation_limit(uint64_t);
extern TxWorld* tx_get_world(uint32_t);

static uint64_t exhaust(void) {
    txw_test_live_allocation_limit(tx_heap_used());
    txw_test_fail_after(0);
    assert(!tx_bridge_alloc_native(1)); /* prove exhaustion after ownership */
    return txw_test_allocation_attempts();
}
static void restored(uint64_t attempts) {
    assert(txw_test_allocation_attempts() == attempts);
    txw_test_fail_after(UINT32_MAX);
    txw_test_live_allocation_limit(UINT64_MAX);
}
static void no_payload(void) {
    assert(tx_native_heap_used() == 0 && tx_bridge_heap_used() == 0);
}
static void pixel_cleanup(void) {
    for (unsigned close_pending = 0; close_pending < 2; ++close_pending) {
        uint32_t owner = 0, color = 0;
        assert(!terra_pixel_workspace_create(128, 128, 1048576, 65536, 256, &owner));
        assert(!terra_pixel_workspace_palette_add(owner, 0x123456, &color));
        assert(!terra_pixel_workspace_tx_begin(owner));
        TerraPixelCell cells[] = {{1, 1, color}, {127, 127, color}};
        assert(!terra_pixel_workspace_cells(owner, cells, 2));
        TerraPixelWorkspaceStats before;
        assert(!terra_pixel_workspace_stats(owner, &before));
        assert(before.transaction_open && before.used_cells == 2);
        uint64_t attempts = exhaust();
        if (!close_pending) {
            assert(!terra_pixel_workspace_tx_rollback(owner));
            TerraPixelWorkspaceStats after;
            assert(!terra_pixel_workspace_stats(owner, &after));
            assert(!after.transaction_open && !after.used_cells && !after.dirty);
        }
        assert(!terra_pixel_workspace_close(owner));
        assert(terra_pixel_workspace_close(owner) == TERRA_PIXEL_HANDLE);
        restored(attempts);
        no_payload();
    }
}
static uint32_t word(const uint8_t* p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void workspace_cleanup(const char* file, int rollback) {
    uint32_t owner = 0, token = 0;
    assert(!terra_world_open(file, &owner));
    TxWorld* world = tx_get_world(owner);
    char original[sizeof world->worldName];
    memcpy(original, world->worldName, sizeof original);
    uint8_t* receipt = tx_bridge_alloc_native(TERRA_OUTCOME_WIRE_BYTES);
    assert(receipt);
    assert(!terra_world_workspace_begin(owner, UINT32_MAX, &token));
    uint64_t required = 0;
    assert(!terra_op_execute_json(owner, "header_patch", "{\"patch\":{\"worldName\":\"cleanup pressure\"}}", NULL, 0, &required));
    assert(strcmp(world->worldName, original));
    uint64_t attempts = exhaust();
    int result = rollback
        ? terra_world_workspace_rollback_receipt(owner, token, receipt, TERRA_OUTCOME_WIRE_BYTES)
        : terra_world_workspace_commit_receipt(owner, token, receipt, TERRA_OUTCOME_WIRE_BYTES);
    assert(!result && word(receipt) == TERRA_OUTCOME_WIRE_BYTES && word(receipt + 4) == 2);
    assert(word(receipt + 12) == (rollback ? TERRA_EFFECT_ROLLED_BACK : TERRA_EFFECT_APPLIED));
    assert(word(receipt + 16) == (rollback ? TERRA_TX_ROLLED_BACK : TERRA_TX_COMMITTED));
    assert(rollback ? !strcmp(world->worldName, original) : !strcmp(world->worldName, "cleanup pressure"));
    assert(!terra_world_close(owner));
    tx_bridge_free_native(receipt);
    restored(attempts);
    no_payload();
}
static void pump_ready(uint32_t task, const uint8_t* source, uint32_t size,
                       TxStreamEvent* event, TxStreamInputLease* lease) {
    for (unsigned step = 0; step < 100000; ++step) {
        TerraConstSpan output;
        assert(!terra_world_stream_step_native(task, 64, event, &output));
        if (event->kind == TX_STREAM_NEED_SOURCE) {
            TerraMutableSpan input;
            assert(!terra_world_stream_acquire_input_native(task, lease, &input));
            assert(lease->source_id == 1 && lease->offset <= size && lease->length <= size - lease->offset);
            memcpy(input.data, source + lease->offset, lease->length);
            assert(!terra_world_stream_commit_input(task, lease->lease_id, 1, lease->offset, lease->length));
        } else if (event->kind == TX_STREAM_OUTPUT) {
            assert(output.data && output.length);
            assert(!terra_world_stream_ack_output(task));
        } else if (event->kind == TX_STREAM_READY) return;
        else assert(event->kind == TX_STREAM_MORE);
    }
    assert(!"READY task did not finish");
}
static void ready_cleanup(const uint8_t* source, uint32_t size, int cancel) {
    uint32_t* handle = tx_bridge_alloc_native(4);
    TxStreamEvent* event = tx_bridge_alloc_native(sizeof *event);
    TxStreamInputLease* lease = tx_bridge_alloc_native(sizeof *lease);
    const char operation[] = "header_patch", request[] = "{\"patch\":{\"worldName\":\"READY candidate\"}}";
    char* op = tx_bridge_alloc_native(sizeof operation);
    char* req = tx_bridge_alloc_native(sizeof request);
    assert(handle && event && lease && op && req);
    memcpy(op, operation, sizeof operation); memcpy(req, request, sizeof request);
    assert(!terra_world_stream_open_begin(1, size, handle));
    uint32_t task = *handle;
    pump_ready(task, source, size, event, lease);
    assert(!terra_world_stream_adopt(task, 1, handle));
    uint32_t world = *handle;
    assert(!terra_world_stream_close(task));
    uint32_t baseline = tx_native_heap_used();
    char original[sizeof tx_get_world(world)->worldName];
    memcpy(original, tx_get_world(world)->worldName, sizeof original);
    assert(!terra_world_stream_operation_begin(world, op, req, handle));
    task = *handle;
    pump_ready(task, source, size, event, lease);
    assert(event->kind == TX_STREAM_READY && tx_native_heap_used() > baseline);
    uint64_t attempts = exhaust();
    if (cancel) {
        assert(!terra_world_stream_cancel(task));
        assert(!terra_world_stream_cancel(task));
    }
    assert(!terra_world_stream_close(task));
    assert(terra_world_stream_close(task) < 0); /* stale close is not a new owner */
    assert(tx_native_heap_used() == baseline && !strcmp(tx_get_world(world)->worldName, original));
    assert(!terra_world_close(world));
    tx_bridge_free_native(handle); tx_bridge_free_native(event); tx_bridge_free_native(lease);
    tx_bridge_free_native(op); tx_bridge_free_native(req);
    restored(attempts);
    no_payload();
}
int main(int argc, char** argv) {
    assert(argc == 2);
    FILE* file = fopen(argv[1], "rb"); assert(file);
    assert(!fseek(file, 0, SEEK_END)); long length = ftell(file); rewind(file);
    assert(length > 0 && (unsigned long)length < UINT32_MAX);
    uint8_t* source = malloc((size_t)length); assert(source);
    assert(fread(source, 1, (size_t)length, file) == (size_t)length); fclose(file);
    for (unsigned iteration = 0; iteration < 16; ++iteration) {
        pixel_cleanup();
        workspace_cleanup(argv[1], 1);
        workspace_cleanup(argv[1], 0);
        ready_cleanup(source, (uint32_t)length, 1);
        ready_cleanup(source, (uint32_t)length, 0);
    }
    free(source);
    puts("16 continuous cycles: pixel rollback/close, workspace rollback/commit, READY cancel/close; zero cleanup allocations and final payload");
    return 0;
}
