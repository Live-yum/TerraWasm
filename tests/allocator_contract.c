#include <assert.h>
#include <stdint.h>
extern uint32_t tx_heap_used(void);
extern uint32_t tx_native_heap_used(void);
extern uint32_t tx_native_heap_peak(void);
extern uint32_t tx_mark(void);
extern void tx_rewind(uint32_t);
extern uint8_t* tx_alloc(uint32_t);
extern uint8_t* tx_persistent_alloc(uint32_t);
extern void tx_persistent_free(void*);
extern void tx_internal_free(void*);
extern void* tx_internal_realloc(void*, uint32_t);

int main(void) {
    const uint32_t baseline = tx_heap_used();
    const uint32_t native_baseline = tx_native_heap_used();
    for (unsigned round = 0; round < 20; ++round) {
        uint8_t* persistent = tx_persistent_alloc(1048576);
        assert(persistent);
        persistent[0] = (uint8_t)round;
        assert(tx_native_heap_used() == native_baseline + 1048576);
        uint32_t mark = tx_mark();
        assert(tx_alloc(4096));
        tx_rewind(mark);
        assert(tx_native_heap_used() == native_baseline + 1048576);
        persistent = tx_internal_realloc(persistent, 2097152);
        assert(persistent && persistent[0] == (uint8_t)round);
        assert(tx_native_heap_used() == native_baseline + 2097152);
        assert(tx_native_heap_peak() >= tx_native_heap_used());
        tx_internal_free(persistent);
        assert(tx_heap_used() == baseline);
        assert(tx_native_heap_used() == native_baseline);
        persistent = tx_persistent_alloc(1024);
        assert(persistent);
        tx_persistent_free(persistent);
        assert(tx_heap_used() == baseline);
        uint8_t* temporary = tx_alloc(1024);
        assert(temporary);
        tx_internal_free(temporary);
        assert(tx_heap_used() == baseline);
    }
    return 0;
}
