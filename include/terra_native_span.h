#ifndef TERRA_NATIVE_SPAN_H
#define TERRA_NATIVE_SPAN_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Native interfaces carry real pointers and explicit lengths, never Wasm offsets.
 * Borrowed spans expire on the corresponding ack/commit/cancel/close. They do not
 * own memory. Allocate native bridge/control buffers with the helpers below. */
typedef struct TerraConstSpan { const uint8_t* data; size_t length; } TerraConstSpan;
typedef struct TerraMutableSpan { uint8_t* data; size_t length; size_t capacity; } TerraMutableSpan;
void* tx_bridge_alloc_native(size_t size);
void tx_bridge_free_native(void* pointer);
size_t tx_bridge_allocation_size_native(const void* pointer);
int tx_bridge_contains_native(const void* pointer, size_t length);
/* Checked wire adapter. A pointer outside Wasm32 is rejected, never truncated. */
int tx_wasm_offset32(const void* pointer, uint32_t* out_offset);
int tx_wasm_span32(const void* pointer, size_t length, uint32_t* out_offset);
#ifdef __cplusplus
}
#endif
#endif
