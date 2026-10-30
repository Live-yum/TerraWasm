/*
 * terra_registry.c -- World handle slots and generation ownership.
 *
 * This module owns the global registry. The V2 API validates and orchestrates
 * operations, but no longer defines or allocates the handle table itself.
 */
#include "terra_registry.h"
#include "terra_checkpoint.h"

#define TX_HANDLE_SLOT_BITS 8u
#define TX_HANDLE_SLOT_MASK ((1u << TX_HANDLE_SLOT_BITS) - 1u)
#define TX_HANDLE_GENERATION_MASK 0x00ffffffu

static TxWorld g_worlds[TX_MAX_WORLDS];
static uint32_t g_next_generation = 1u;

extern void tx_set_error(const char* code, const char* message);
extern void tx_clear_error(void);
extern uint32_t tx_get_world_open_count(void);
extern void tx_set_world_open_count(uint32_t count);
extern uint32_t tx_mark(void);
extern int tx_stream_has_task(void);
extern int tx_world_has_task(void);

TxWorld* tx_world_registry_slots(void) {
    return g_worlds;
}

uint32_t tx_world_registry_next_handle(uint32_t slot_index) {
    uint32_t generation = g_next_generation++ & TX_HANDLE_GENERATION_MASK;
    if (generation == 0u) {
        generation = 1u;
        g_next_generation = 2u;
    }
    return (generation << TX_HANDLE_SLOT_BITS) | (slot_index + 1u);
}

TxWorld* tx_get_world(uint32_t handle) {
    uint32_t encoded_slot = handle & TX_HANDLE_SLOT_MASK;
    if (!handle || encoded_slot == 0u || encoded_slot > TX_MAX_WORLDS) return NULL;
    TxWorld* world = &g_worlds[encoded_slot - 1u];
    return world->active && world->handle == handle ? world : NULL;
}

/* The registry is the only module that publishes an incremental opener's
 * staged owner into the active generation table. This is an allocation-free
 * ownership transfer; the task's original allocation mark remains intact. */
terrax_world_status tx_world_adopt_open_task(TxWorld* candidate, uint32_t* out_handle) {
    if (out_handle) *out_handle = 0u;
    if (!candidate || !out_handle || !tx_world_has_task() ||
        tx_get_world_open_count() || tx_stream_has_task() || tx_checkpoint_active()) {
        tx_set_error("TERRAX_STATE_ERROR", "incremental world owner cannot be activated");
        return TERRAX_WORLD_STATUS_STATE_ERROR;
    }
    TxWorld* target = &g_worlds[0];
    *target = *candidate;
    target->handle = tx_world_registry_next_handle(0u);
    target->active = 1u;
    target->heap_mark = tx_mark();
    tx_set_world_open_count(1u);
    *out_handle = target->handle;
    tx_clear_error();
    return TERRAX_WORLD_STATUS_OK;
}
