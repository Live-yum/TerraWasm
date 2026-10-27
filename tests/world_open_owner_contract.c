#include "terra_world.h"
#include "terra_task.h"
#include "terra_stream.h"
#include "terra_native_span.h"
#include "terra_types.h"
#include "terra_owner_guards.h"
#include "terra_circuit_world.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint32_t tx_native_heap_used(void);
extern uint32_t tx_get_world_open_count(void);
extern void tx_reset_heap(void);
extern uint8_t* tx_alloc(uint32_t size);
extern uint8_t* tx_last_ptr;
extern uint32_t tx_last_len, tx_last_width, tx_last_height;

static void assert_sync_busy(const uint8_t* bytes, uint32_t length) {
    uint32_t handle = 123;
    assert(terra_world_open_from_buffer(bytes, length, &handle) == TERRAX_WORLD_STATUS_STATE_ERROR);
    assert(handle == 0);
    handle = 123;
    assert(terra_world_create(&handle) == TERRAX_WORLD_STATUS_STATE_ERROR);
    assert(handle == 0);
    handle = 123;
    assert(terra_world_open("unused-while-owned.wld", &handle) == TERRAX_WORLD_STATUS_STATE_ERROR);
    assert(handle == 0);
}

static void staging_is_exclusive(const uint8_t* bytes, uint32_t length) {
    uint32_t* stream = tx_bridge_alloc_native(sizeof(*stream));
    assert(stream);
    for (unsigned allocated = 0; allocated != 2; ++allocated) {
        uint32_t before = tx_native_heap_used();
        uint32_t task = terra_world_open_begin(bytes, length);
        assert(task);
        assert(tx_open_task_has_world_owner());
        if (allocated) assert(terra_world_open_step(task, 1) == TERRAX_WORLD_STATUS_IN_PROGRESS);
        uint32_t owned = tx_native_heap_used();
        assert(!allocated || owned > before);
        assert_sync_busy(bytes, length);
        assert(terra_world_open_begin(bytes, length) == 0);
        *stream = 123;
        assert(terra_world_stream_open_begin(1, length, stream) < 0);
        assert(*stream == 0);
        tx_reset_heap();
        assert(tx_native_heap_used() == owned);
        assert(tx_get_world_open_count() == 0);
        assert(terra_world_open_cancel(task) == TERRAX_WORLD_STATUS_CANCELLED);
        assert(terra_world_task_close(task) == TERRAX_WORLD_STATUS_OK);
        assert(tx_native_heap_used() == before);
        uint32_t handle = 0;
        assert(terra_world_create(&handle) == TERRAX_WORLD_STATUS_OK);
        assert(terra_world_close(handle) == TERRAX_WORLD_STATUS_OK);
    }
    assert(terra_world_stream_open_begin(1, length, stream) == 0);
    uint32_t task = *stream;
    uint32_t owned = tx_native_heap_used();
    assert_sync_busy(bytes, length);
    assert(terra_world_open_begin(bytes, length) == 0);
    tx_reset_heap();
    assert(tx_native_heap_used() == owned);
    assert(terra_world_stream_cancel(task) == 0);
    assert(terra_world_stream_close(task) == 0);
    tx_bridge_free_native(stream);
    assert(tx_native_heap_used() == 0);
}

static void failure_releases_owner(void) {
    uint8_t invalid[16] = {0};
    uint32_t task = terra_world_open_begin(invalid, sizeof(invalid));
    assert(task);
    assert(terra_world_open_step(task, 16) == TERRAX_WORLD_STATUS_PARSE_ERROR);
    assert(tx_native_heap_used() == 0);
    assert(terra_world_task_close(task) == TERRAX_WORLD_STATUS_OK);
    uint32_t handle = 0;
    assert(terra_world_create(&handle) == TERRAX_WORLD_STATUS_OK);
    assert(terra_world_close(handle) == TERRAX_WORLD_STATUS_OK);
}

static void finished_task_survives_commit(const uint8_t* bytes, uint32_t length) {
    for (unsigned cancel = 0; cancel != 2; ++cancel) {
        uint32_t task = terra_world_open_begin(bytes, length);
        assert(task);
        terrax_world_status status = TERRAX_WORLD_STATUS_IN_PROGRESS;
        for (unsigned step = 0; step < 100000 && status == TERRAX_WORLD_STATUS_IN_PROGRESS; ++step)
            status = terra_world_open_step(task, 1);
        assert(status == TERRAX_WORLD_STATUS_OK);
        assert(tx_open_task_has_world_owner()); /* ready but not transferred */
        assert(!tx_world_can_activate_stream());
        uint32_t handle = 0, replacement = 0, required = 0;
        assert(terra_world_open_finish(task, &handle) == TERRAX_WORLD_STATUS_OK && handle);
        assert(!tx_open_task_has_world_owner());
        assert(terra_world_task_get_status(task) == TERRAX_WORLD_STATUS_OK);
        assert(terra_world_task_get_progress(task) == 100);
        assert(terra_world_task_get_world_handle(task) == 0);
        assert(terra_world_commit_to_buffer(handle, NULL, 0, &required, &replacement) == TERRAX_WORLD_STATUS_OK);
        assert(required == length && replacement == 0);
        uint8_t* output = malloc(required);
        assert(output);
        assert(terra_world_commit_to_buffer(handle, output, required, &required, &replacement) == TERRAX_WORLD_STATUS_OK);
        assert(replacement && replacement != handle && !memcmp(output, bytes, length));
        assert(terra_world_task_get_status(task) == TERRAX_WORLD_STATUS_OK);
        assert(terra_world_open_step(task, 1) == TERRAX_WORLD_STATUS_OK);
        if (cancel) {
            /* This terminal handle cannot clear output owned by another call. */
            tx_last_ptr = output; tx_last_len = length; tx_last_width = 7; tx_last_height = 9;
            assert(terra_world_open_cancel(task) == TERRAX_WORLD_STATUS_CANCELLED);
            assert(tx_last_ptr == output && tx_last_len == length && tx_last_width == 7 && tx_last_height == 9);
            assert(terra_world_task_get_status(task) == TERRAX_WORLD_STATUS_CANCELLED);
        } else {
            assert(terra_world_task_close(task) == TERRAX_WORLD_STATUS_OK);
        }
        assert(tx_get_world_open_count() == 1);
        assert(terra_world_save_to_buffer(replacement, output, length, &required) == TERRAX_WORLD_STATUS_OK);
        assert(required == length && !memcmp(output, bytes, length));
        assert(terra_world_close(replacement) == TERRAX_WORLD_STATUS_OK);
        assert(tx_get_world_open_count() == 0 && tx_native_heap_used() == 0);
        if (cancel) {
            /* A terminal task is still reset-pinned until task_close, even
             * after its transferred/replacement world has been closed. */
            uint8_t* scratch = tx_alloc(64);
            assert(scratch); scratch[0] = 0x5a;
            uint32_t owned = tx_native_heap_used();
            tx_reset_heap();
            assert(tx_native_heap_used() == owned && scratch[0] == 0x5a);
            assert(terra_world_task_close(task) == TERRAX_WORLD_STATUS_OK);
            tx_reset_heap();
            assert(tx_native_heap_used() == 0);
        }
        free(output);
    }
}

static uint32_t ready_task(const uint8_t* bytes, uint32_t length) {
    uint32_t task = terra_world_open_begin(bytes, length);
    assert(task);
    terrax_world_status status = TERRAX_WORLD_STATUS_IN_PROGRESS;
    for (unsigned step = 0; step < 100000 && status == TERRAX_WORLD_STATUS_IN_PROGRESS; ++step)
        status = terra_world_open_step(task, 1);
    assert(status == TERRAX_WORLD_STATUS_OK);
    return task;
}

static void rejected_cleanup_retains_owner(const uint8_t* bytes, uint32_t length) {
    for (unsigned cancel = 0; cancel != 2; ++cancel) {
        uint32_t task = ready_task(bytes, length);
        uint32_t world = terra_world_task_get_world_handle(task), circuit = 0;
        assert(world);
        assert(terra_circuit_world_begin(world, 77, 0, 0, 8u * 1024u * 1024u, &circuit) == 0);
        uint32_t owned = tx_native_heap_used();
        for (unsigned attempt = 0; attempt != 2; ++attempt) {
            terrax_world_status status = cancel ? terra_world_open_cancel(task) : terra_world_task_close(task);
            assert(status == TERRAX_WORLD_STATUS_STATE_ERROR);
            assert(terra_world_task_get_world_handle(task) == world);
            assert(terra_world_task_get_status(task) == TERRAX_WORLD_STATUS_OK);
            assert(terra_world_task_get_progress(task) == 100);
            assert(tx_open_task_has_world_owner() && tx_get_world_open_count() == 1);
            tx_reset_heap();
            assert(tx_native_heap_used() == owned);
            TerraCircuitWorldStats stats;
            assert(terra_circuit_world_stats(circuit, &stats) == 0);
        }
        assert(terra_circuit_world_close(circuit) == 0);
        if (cancel) assert(terra_world_open_cancel(task) == TERRAX_WORLD_STATUS_CANCELLED);
        assert(terra_world_task_close(task) == TERRAX_WORLD_STATUS_OK);
        assert(tx_get_world_open_count() == 0 && tx_native_heap_used() == 0);
    }
}

static void retired_world_is_not_transferred(const uint8_t* bytes, uint32_t length) {
    uint32_t task = ready_task(bytes, length);
    uint32_t world = terra_world_task_get_world_handle(task), out = 123;
    assert(world && terra_world_close(world) == TERRAX_WORLD_STATUS_OK);
    assert(terra_world_open_finish(task, &out) == TERRAX_WORLD_STATUS_STATE_ERROR);
    assert(out == 0);
    assert(terra_world_task_close(task) == TERRAX_WORLD_STATUS_OK);
    assert(tx_get_world_open_count() == 0 && tx_native_heap_used() == 0);
}

static void finish_then_reopen(const char* path) {
    FILE* file = fopen(path, "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    assert(length >= 16 && length <= UINT32_MAX && fseek(file, 0, SEEK_SET) == 0);
    uint8_t* bytes = malloc((size_t)length);
    assert(bytes && fread(bytes, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    staging_is_exclusive(bytes, (uint32_t)length);
    finished_task_survives_commit(bytes, (uint32_t)length);
    rejected_cleanup_retains_owner(bytes, (uint32_t)length);
    retired_world_is_not_transferred(bytes, (uint32_t)length);
    uint32_t task = terra_world_open_begin(bytes, (uint32_t)length);
    assert(task);
    terrax_world_status status = TERRAX_WORLD_STATUS_IN_PROGRESS;
    for (unsigned step = 0; step < 100000 && status == TERRAX_WORLD_STATUS_IN_PROGRESS; ++step)
        status = terra_world_open_step(task, 1);
    assert(status == TERRAX_WORLD_STATUS_OK);
    uint32_t handle = 0;
    assert(terra_world_open_finish(task, &handle) == TERRAX_WORLD_STATUS_OK && handle);
    assert(terra_world_task_close(task) == TERRAX_WORLD_STATUS_OK);
    assert(tx_get_world_open_count() == 1 && tx_native_heap_used() > 0);
    tx_reset_heap();
    uint32_t required = 0;
    assert(terra_world_save_to_buffer(handle, NULL, 0, &required) == TERRAX_WORLD_STATUS_OK);
    assert(required == (uint32_t)length);
    assert(terra_world_close(handle) == TERRAX_WORLD_STATUS_OK);
    assert(tx_native_heap_used() == 0);
    assert(terra_world_open_from_buffer(bytes, (uint32_t)length, &handle) == TERRAX_WORLD_STATUS_OK);
    assert(terra_world_close(handle) == TERRAX_WORLD_STATUS_OK);
    free(bytes);
    assert(tx_native_heap_used() == 0);
}

int main(int argc, char** argv) {
    uint8_t bytes[16] = {0};
    staging_is_exclusive(bytes, sizeof(bytes));
    failure_releases_owner();
    if (argc > 1) finish_then_reopen(argv[1]);
    puts("world open ownership: mixed entry rejection, safe reset, cancel/failure cleanup and activation passed");
    return 0;
}
