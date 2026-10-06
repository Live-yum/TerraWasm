#include "terra_pixel_workspace.h"
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern uint8_t* tx_persistent_alloc(uint32_t size);
extern void tx_persistent_free(void* pointer);

#define PX_HANDLES 16u
#define PX_MAX_BYTES (128u * 1024u * 1024u)
#define PX_ALLOC_OVERHEAD 128u
#define PX_BLOCK_CELLS 4096u
#define PX_IMPORT_COLORS 32768u

typedef struct PxBlock { uint16_t cells[PX_BLOCK_CELLS]; uint32_t used; } PxBlock;
typedef struct PxSnapshot PxSnapshot;
typedef struct PxSlot { PxBlock* block; PxSnapshot* snapshot; uint32_t version; } PxSlot;
struct PxSnapshot { PxSnapshot* next; uint32_t slot; uint16_t* before; uint32_t used; };
typedef struct PxChange { uint32_t cell; uint16_t before, after; } PxChange;
typedef struct PxHistory {
    struct PxHistory *previous, *next;
    uint32_t count, before_state, after_state, bytes;
    PxChange changes[];
} PxHistory;
typedef union PxAllocation {
    struct { uint32_t bytes; } value;
    max_align_t alignment;
} PxAllocation;
typedef struct PxWorkspace {
    uint32_t handle, width, height, columns, rows, slot_count;
    uint32_t maximum, budget, active, peak, palette_limit, palette_count;
    uint32_t blocks, used, revision, state, next_state, checkpoint;
    uint32_t in_transaction, transaction_palette, transaction_bytes;
    uint32_t undo_count, redo_count, history_bytes;
    int32_t allocation_error;
    PxSlot* slots;
    uint32_t* palette;
    uint32_t* palette_hash;
    uint32_t hash_capacity;
    uint32_t* import_cache;
    PxSnapshot* snapshots;
    PxHistory *history_first, *history_last, *current;
} PxWorkspace;
static PxWorkspace* px_workspaces[PX_HANDLES];
static uint32_t px_next_handle = 1u;

_Static_assert(sizeof(TerraPixelWorkspaceStats) == 96u, "pixel stats ABI");
_Static_assert(sizeof(TerraPixelCell) == 12u, "pixel cell ABI");
_Static_assert(sizeof(TerraPixelBlockInfo) == 8u, "pixel block ABI");
_Static_assert(sizeof(PxChange) == 8u, "pixel history compact diff");

static PxWorkspace* px_lookup(uint32_t handle) {
    if (!handle) return NULL;
    for (uint32_t i = 0; i < PX_HANDLES; ++i)
        if (px_workspaces[i] && px_workspaces[i]->handle == handle) return px_workspaces[i];
    return NULL;
}
static void* px_alloc(PxWorkspace* workspace, uint32_t bytes) {
    uint64_t total = (uint64_t)bytes + sizeof(PxAllocation) + PX_ALLOC_OVERHEAD;
    if (total > workspace->maximum - workspace->active) {
        workspace->allocation_error = TERRA_PIXEL_LIMIT;
        return NULL;
    }
    PxAllocation* allocation = (PxAllocation*)tx_persistent_alloc(bytes + (uint32_t)sizeof(PxAllocation));
    if (!allocation) { workspace->allocation_error = TERRA_PIXEL_OOM; return NULL; }
    allocation->value.bytes = (uint32_t)total;
    workspace->active += (uint32_t)total;
    if (workspace->active > workspace->peak) workspace->peak = workspace->active;
    return allocation + 1;
}
static void px_free(PxWorkspace* workspace, void* pointer) {
    if (!pointer) return;
    PxAllocation* allocation = ((PxAllocation*)pointer) - 1;
    workspace->active -= allocation->value.bytes;
    tx_persistent_free(allocation);
}
static uint32_t px_distance(uint32_t a, uint32_t b) {
    int32_t r = (int32_t)(a >> 16) - (int32_t)(b >> 16);
    int32_t g = (int32_t)((a >> 8) & 255u) - (int32_t)((b >> 8) & 255u);
    int32_t blue = (int32_t)(a & 255u) - (int32_t)(b & 255u);
    return (uint32_t)(r * r + g * g + blue * blue);
}
static uint32_t px_nearest(PxWorkspace* workspace, uint32_t rgb) {
    uint32_t result = 0, distance = UINT32_MAX;
    for (uint32_t i = 1; i < workspace->palette_count; ++i) {
        uint32_t candidate = px_distance(rgb, workspace->palette[i]);
        if (candidate < distance) { result = i; distance = candidate; if (!candidate) break; }
    }
    return result;
}
static uint32_t px_hash(uint32_t rgb) {
    rgb ^= rgb >> 16; rgb *= 0x7feb352du; rgb ^= rgb >> 15;
    rgb *= 0x846ca68bu; return rgb ^ (rgb >> 16);
}
static void px_rebuild_palette_hash(PxWorkspace* workspace) {
    memset(workspace->palette_hash, 0, workspace->hash_capacity * 4u);
    for (uint32_t i = 1; i < workspace->palette_count; ++i) {
        uint32_t slot = px_hash(workspace->palette[i]) & (workspace->hash_capacity - 1u);
        while (workspace->palette_hash[slot]) slot = (slot + 1u) & (workspace->hash_capacity - 1u);
        workspace->palette_hash[slot] = i;
    }
}
static uint32_t px_palette_add(PxWorkspace* workspace, uint32_t rgb) {
    uint32_t slot = px_hash(rgb) & (workspace->hash_capacity - 1u);
    while (workspace->palette_hash[slot]) {
        uint32_t index = workspace->palette_hash[slot];
        if (workspace->palette[index] == rgb) return index;
        slot = (slot + 1u) & (workspace->hash_capacity - 1u);
    }
    if (workspace->palette_count == workspace->palette_limit) return px_nearest(workspace, rgb);
    uint32_t index = workspace->palette_count++;
    workspace->palette[index] = rgb; workspace->palette_hash[slot] = index;
    /* Existing cached nearest-overflow answers cannot change: overflow only
     * starts once the palette is full. New exact entries are imported lazily. */
    return index;
}
static int px_coordinate(PxWorkspace* workspace, int32_t x, int32_t y) {
    return x >= 0 && y >= 0 && (uint32_t)x < workspace->width && (uint32_t)y < workspace->height;
}
static int px_rect(PxWorkspace* workspace, int32_t x, int32_t y, uint32_t width, uint32_t height) {
    return width && height && px_coordinate(workspace, x, y)
        && width <= workspace->width - (uint32_t)x && height <= workspace->height - (uint32_t)y;
}
static uint16_t px_get(PxWorkspace* workspace, uint32_t x, uint32_t y) {
    PxBlock* block = workspace->slots[(y >> 6) * workspace->columns + (x >> 6)].block;
    return block ? block->cells[((y & 63u) << 6) | (x & 63u)] : 0;
}
static void px_remove_empty(PxWorkspace* workspace) {
    for (uint32_t i = 0; i < workspace->slot_count; ++i) {
        PxBlock* block = workspace->slots[i].block;
        if (block && !block->used) {
            px_free(workspace, block); workspace->slots[i].block = NULL; --workspace->blocks;
        }
    }
}
static void px_drop_snapshots(PxWorkspace* workspace) {
    PxSnapshot* snapshot = workspace->snapshots;
    while (snapshot) {
        PxSnapshot* next = snapshot->next;
        workspace->slots[snapshot->slot].snapshot = NULL;
        px_free(workspace, snapshot->before); px_free(workspace, snapshot);
        snapshot = next;
    }
    workspace->snapshots = NULL;
    workspace->transaction_bytes = 0;
}
/* There is always a rollback revision reserved by px_mutation_start. */
static void px_rollback(PxWorkspace* workspace) {
    if (!workspace->in_transaction) return;
    if (workspace->snapshots) ++workspace->revision;
    for (PxSnapshot* snapshot = workspace->snapshots; snapshot; snapshot = snapshot->next) {
        PxSlot* slot = &workspace->slots[snapshot->slot];
        if (slot->block) workspace->used -= slot->block->used;
        if (snapshot->before) {
            memcpy(slot->block->cells, snapshot->before, sizeof(slot->block->cells));
            slot->block->used = snapshot->used; workspace->used += snapshot->used;
        } else {
            if (slot->block) { px_free(workspace, slot->block); --workspace->blocks; }
            slot->block = NULL;
        }
        slot->version = workspace->revision;
    }
    if (workspace->palette_count != workspace->transaction_palette) {
        workspace->palette_count = workspace->transaction_palette;
        px_rebuild_palette_hash(workspace);
        if (workspace->import_cache) memset(workspace->import_cache, 0, PX_IMPORT_COLORS * 4u);
    }
    px_drop_snapshots(workspace);
    workspace->in_transaction = 0;
}
static int32_t px_fail(PxWorkspace* workspace, int32_t status) {
    if (workspace) px_rollback(workspace);
    return status;
}
static int32_t px_mutation_start(PxWorkspace* workspace) {
    if (!workspace) return TERRA_PIXEL_HANDLE;
    if (!workspace->in_transaction) return TERRA_PIXEL_TRANSACTION;
    if (workspace->revision >= UINT32_MAX - 1u) return px_fail(workspace, TERRA_PIXEL_EXHAUSTED);
    ++workspace->revision;
    return TERRA_PIXEL_OK;
}
static int32_t px_set(PxWorkspace* workspace, uint32_t x, uint32_t y, uint16_t value) {
    uint32_t slot_index = (y >> 6) * workspace->columns + (x >> 6);
    uint32_t local = ((y & 63u) << 6) | (x & 63u);
    PxSlot* slot = &workspace->slots[slot_index];
    uint16_t previous = slot->block ? slot->block->cells[local] : 0;
    if (previous == value) return TERRA_PIXEL_OK;
    if (!slot->snapshot) {
        uint32_t baseline = workspace->active;
        PxSnapshot* snapshot = (PxSnapshot*)px_alloc(workspace, sizeof(PxSnapshot));
        if (!snapshot) return workspace->allocation_error;
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->slot = slot_index;
        if (slot->block) {
            snapshot->before = (uint16_t*)px_alloc(workspace, PX_BLOCK_CELLS * 2u);
            if (!snapshot->before) { px_free(workspace, snapshot); return workspace->allocation_error; }
            memcpy(snapshot->before, slot->block->cells, PX_BLOCK_CELLS * 2u);
            snapshot->used = slot->block->used;
        }
        snapshot->next = workspace->snapshots; workspace->snapshots = snapshot;
        slot->snapshot = snapshot;
        workspace->transaction_bytes += workspace->active - baseline;
    }
    if (!slot->block) {
        slot->block = (PxBlock*)px_alloc(workspace, sizeof(PxBlock));
        if (!slot->block) return workspace->allocation_error;
        memset(slot->block, 0, sizeof(*slot->block)); ++workspace->blocks;
    }
    if (!previous) { ++slot->block->used; ++workspace->used; }
    if (!value) { --slot->block->used; --workspace->used; }
    slot->block->cells[local] = value; slot->version = workspace->revision;
    return TERRA_PIXEL_OK;
}
static void px_history_remove(PxWorkspace* workspace, PxHistory* item) {
    if (item->previous) item->previous->next = item->next; else workspace->history_first = item->next;
    if (item->next) item->next->previous = item->previous; else workspace->history_last = item->previous;
    workspace->history_bytes -= item->bytes;
    px_free(workspace, item);
}
uint32_t terra_pixel_workspace_abi_version(void) { return TERRA_PIXEL_WORKSPACE_ABI; }
int32_t terra_pixel_workspace_create(uint32_t width, uint32_t height, uint32_t max_bytes,
        uint32_t history_bytes, uint32_t palette_limit, uint32_t* out_handle) {
    if (out_handle) *out_handle = 0;
    if (!out_handle || !width || !height || width > TERRA_PIXEL_MAX_DIMENSION
        || height > TERRA_PIXEL_MAX_DIMENSION || !max_bytes || max_bytes > PX_MAX_BYTES
        || history_bytes > max_bytes || !palette_limit || palette_limit > 65536u) return TERRA_PIXEL_INVALID;
    uint32_t free_slot = PX_HANDLES;
    for (uint32_t i = 0; i < PX_HANDLES; ++i) if (!px_workspaces[i]) { free_slot = i; break; }
    if (free_slot == PX_HANDLES) return TERRA_PIXEL_LIMIT;
    if (!px_next_handle) return TERRA_PIXEL_EXHAUSTED;
    if (sizeof(PxWorkspace) + PX_ALLOC_OVERHEAD > max_bytes) return TERRA_PIXEL_LIMIT;
    PxWorkspace* workspace = (PxWorkspace*)tx_persistent_alloc(sizeof(PxWorkspace));
    if (!workspace) return TERRA_PIXEL_OOM;
    memset(workspace, 0, sizeof(*workspace));
    workspace->width = width; workspace->height = height;
    workspace->columns = (width + 63u) >> 6; workspace->rows = (height + 63u) >> 6;
    workspace->slot_count = workspace->columns * workspace->rows;
    workspace->maximum = max_bytes; workspace->budget = history_bytes;
    workspace->active = (uint32_t)sizeof(*workspace) + PX_ALLOC_OVERHEAD; workspace->peak = workspace->active;
    workspace->palette_limit = palette_limit; workspace->palette_count = 1;
    workspace->state = workspace->next_state = workspace->checkpoint = 1;
    workspace->slots = (PxSlot*)px_alloc(workspace, workspace->slot_count * (uint32_t)sizeof(PxSlot));
    workspace->palette = (uint32_t*)px_alloc(workspace, palette_limit * 4u);
    workspace->hash_capacity = 2u;
    while (workspace->hash_capacity < palette_limit * 2u) workspace->hash_capacity *= 2u;
    workspace->palette_hash = (uint32_t*)px_alloc(workspace, workspace->hash_capacity * 4u);
    if (!workspace->slots || !workspace->palette || !workspace->palette_hash) {
        int32_t status = workspace->allocation_error;
        px_free(workspace, workspace->slots); px_free(workspace, workspace->palette); px_free(workspace, workspace->palette_hash);
        tx_persistent_free(workspace); return status;
    }
    memset(workspace->slots, 0, workspace->slot_count * sizeof(PxSlot)); workspace->palette[0] = 0;
    memset(workspace->palette_hash, 0, workspace->hash_capacity * 4u);
    workspace->handle = px_next_handle++;
    px_workspaces[free_slot] = workspace; *out_handle = workspace->handle;
    return TERRA_PIXEL_OK;
}
int32_t terra_pixel_workspace_close(uint32_t handle) {
    PxWorkspace* workspace = px_lookup(handle);
    if (!workspace) return TERRA_PIXEL_HANDLE;
    px_drop_snapshots(workspace);
    while (workspace->history_first) px_history_remove(workspace, workspace->history_first);
    for (uint32_t i = 0; i < workspace->slot_count; ++i) px_free(workspace, workspace->slots[i].block);
    px_free(workspace, workspace->slots); px_free(workspace, workspace->palette); px_free(workspace, workspace->import_cache); px_free(workspace, workspace->palette_hash);
    for (uint32_t i = 0; i < PX_HANDLES; ++i) if (px_workspaces[i] == workspace) px_workspaces[i] = NULL;
    tx_persistent_free(workspace);
    return TERRA_PIXEL_OK;
}
int32_t terra_pixel_workspace_stats(uint32_t handle, TerraPixelWorkspaceStats* out) {
    PxWorkspace* workspace = px_lookup(handle);
    if (!workspace) return TERRA_PIXEL_HANDLE;
    if (!out) return TERRA_PIXEL_INVALID;
    *out = (TerraPixelWorkspaceStats) {
        TERRA_PIXEL_WORKSPACE_ABI, workspace->width, workspace->height, workspace->columns, workspace->rows, workspace->blocks,
        workspace->palette_count, workspace->used, workspace->revision, workspace->state, workspace->checkpoint,
        workspace->state != workspace->checkpoint || workspace->snapshots != NULL,
        workspace->undo_count, workspace->redo_count, workspace->history_bytes, workspace->in_transaction, workspace->active, workspace->peak,
        workspace->maximum, workspace->budget, workspace->transaction_bytes, workspace->palette_limit, 0, 0
    };
    return TERRA_PIXEL_OK;
}
int32_t terra_pixel_workspace_tx_begin(uint32_t handle) {
    PxWorkspace* workspace = px_lookup(handle);
    if (!workspace) return TERRA_PIXEL_HANDLE;
    if (workspace->in_transaction) return TERRA_PIXEL_TRANSACTION;
    if (workspace->revision >= UINT32_MAX - 1u || workspace->next_state == UINT32_MAX) return TERRA_PIXEL_EXHAUSTED;
    workspace->in_transaction = 1; workspace->transaction_palette = workspace->palette_count;
    return TERRA_PIXEL_OK;
}
int32_t terra_pixel_workspace_tx_rollback(uint32_t handle) {
    PxWorkspace* workspace = px_lookup(handle);
    if (!workspace) return TERRA_PIXEL_HANDLE;
    if (!workspace->in_transaction) return TERRA_PIXEL_TRANSACTION;
    px_rollback(workspace); return TERRA_PIXEL_OK;
}
int32_t terra_pixel_workspace_tx_commit(uint32_t handle) {
    PxWorkspace* workspace = px_lookup(handle);
    if (!workspace) return TERRA_PIXEL_HANDLE;
    if (!workspace->in_transaction) return TERRA_PIXEL_TRANSACTION;
    uint32_t count = 0;
    for (PxSnapshot* snapshot = workspace->snapshots; snapshot; snapshot = snapshot->next) {
        PxBlock* block = workspace->slots[snapshot->slot].block;
        if (!block) continue;
        for (uint32_t i = 0; i < PX_BLOCK_CELLS; ++i)
            if (block->cells[i] != (snapshot->before ? snapshot->before[i] : 0u)) ++count;
    }
    PxHistory* item = NULL;
    if (count && workspace->budget) {
        uint64_t bytes = sizeof(PxHistory) + (uint64_t)count * sizeof(PxChange);
        uint64_t charged = bytes + sizeof(PxAllocation) + PX_ALLOC_OVERHEAD;
        if (charged > workspace->budget) return px_fail(workspace, TERRA_PIXEL_LIMIT);
        item = (PxHistory*)px_alloc(workspace, (uint32_t)bytes);
        if (!item) return px_fail(workspace, workspace->allocation_error);
        memset(item, 0, sizeof(*item)); item->count = count; item->bytes = (uint32_t)charged;
        item->before_state = workspace->state; item->after_state = workspace->next_state + 1u;
        uint32_t index = 0;
        for (PxSnapshot* snapshot = workspace->snapshots; snapshot; snapshot = snapshot->next) {
            PxBlock* block = workspace->slots[snapshot->slot].block;
            if (!block) continue;
            uint32_t bx = snapshot->slot % workspace->columns, by = snapshot->slot / workspace->columns;
            for (uint32_t i = 0; i < PX_BLOCK_CELLS; ++i) {
                uint16_t before = snapshot->before ? snapshot->before[i] : 0;
                if (block->cells[i] == before) continue;
                uint32_t x = (bx << 6) + (i & 63u), y = (by << 6) + (i >> 6);
                item->changes[index++] = (PxChange) { y * workspace->width + x, before, block->cells[i] };
            }
        }
    }
    if (count) {
        PxHistory* future = workspace->current ? workspace->current->next : workspace->history_first;
        while (future) { PxHistory* next = future->next; px_history_remove(workspace, future); future = next; }
        workspace->redo_count = 0;
        workspace->state = ++workspace->next_state;
        if (item) {
            while (workspace->history_first && workspace->history_bytes > workspace->budget - item->bytes) {
                PxHistory* oldest = workspace->history_first;
                if (workspace->current == oldest) workspace->current = NULL;
                px_history_remove(workspace, oldest); --workspace->undo_count;
            }
            item->previous = workspace->history_last;
            if (workspace->history_last) workspace->history_last->next = item; else workspace->history_first = item;
            workspace->history_last = workspace->current = item;
            workspace->history_bytes += item->bytes; ++workspace->undo_count;
        }
    }
    px_drop_snapshots(workspace); px_remove_empty(workspace); workspace->in_transaction = 0;
    return TERRA_PIXEL_OK;
}
int32_t terra_pixel_workspace_palette_add(uint32_t handle, uint32_t rgb, uint32_t* out_index) {
    PxWorkspace* workspace = px_lookup(handle);
    if (!workspace) return TERRA_PIXEL_HANDLE;
    if (!out_index || rgb > 0xffffffu) return px_fail(workspace, TERRA_PIXEL_INVALID);
    *out_index = px_palette_add(workspace, rgb); return TERRA_PIXEL_OK;
}
int32_t terra_pixel_workspace_palette_read(uint32_t handle, uint32_t first, uint32_t count, uint32_t* out) {
    PxWorkspace* workspace = px_lookup(handle);
    if (!workspace) return TERRA_PIXEL_HANDLE;
    if (count > TERRA_PIXEL_QUERY_CELLS || first > workspace->palette_count || count > workspace->palette_count - first
        || (count && !out)) return TERRA_PIXEL_INVALID;
    if (count) memcpy(out, workspace->palette + first, count * 4u);
    return TERRA_PIXEL_OK;
}
int32_t terra_pixel_workspace_nearest(uint32_t handle, const uint32_t* rgb, uint32_t count, uint16_t* out) {
    PxWorkspace* workspace = px_lookup(handle);
    if (!workspace) return TERRA_PIXEL_HANDLE;
    if (count > TERRA_PIXEL_QUERY_CELLS || (count && (!rgb || !out))) return TERRA_PIXEL_INVALID;
    for (uint32_t i = 0; i < count; ++i) if (rgb[i] > 0xffffffu) return TERRA_PIXEL_INVALID;
    for (uint32_t i = 0; i < count; ++i) out[i] = (uint16_t)px_nearest(workspace, rgb[i]);
    return TERRA_PIXEL_OK;
}
/* A compact per-call KD tree uses implicit median nodes and u32 ordinals only.
 * Three radix passes group duplicate RGBs first. Their best tie-ranked candidate
 * is sufficient for every query in this call; degenerate duplicate palettes do
 * not force a full candidate traversal for every exact-color query.
 * Scratch is bounded to 512 KiB, independent of the number of query colors. */
static uint32_t px_candidate_rank(uint32_t properties, uint32_t flags) {
    return (properties & 1u) * 2u + (((flags & 4u) && !(properties & 2u)) ? 1u : 0u);
}
static int px_axis_less(const TerraPixelCandidate* candidates, uint32_t a, uint32_t b, uint32_t shift) {
    uint32_t aa = (candidates[a].rgb >> shift) & 255u, bb = (candidates[b].rgb >> shift) & 255u;
    return aa < bb || (aa == bb && a < b);
}
static void px_index_swap(uint32_t* a, uint32_t* b) { uint32_t temporary = *a; *a = *b; *b = temporary; }
static void px_heap_sift(const TerraPixelCandidate* candidates, uint32_t* indices, uint32_t root,
        uint32_t count, uint32_t shift) {
    while (root < count / 2u) {
        uint32_t child = root * 2u + 1u;
        if (child + 1u < count && px_axis_less(candidates, indices[child], indices[child + 1u], shift)) ++child;
        if (!px_axis_less(candidates, indices[root], indices[child], shift)) return;
        px_index_swap(&indices[root], &indices[child]); root = child;
    }
}
static void px_heap_sort(const TerraPixelCandidate* candidates, uint32_t* indices, uint32_t count, uint32_t shift) {
    for (uint32_t i = count / 2u; i; --i) px_heap_sift(candidates, indices, i - 1u, count, shift);
    for (uint32_t i = count; i > 1u; --i) {
        px_index_swap(&indices[0], &indices[i - 1u]); px_heap_sift(candidates, indices, 0, i - 1u, shift);
    }
}
static void px_select(const TerraPixelCandidate* candidates, uint32_t* indices, uint32_t first,
        uint32_t last, uint32_t middle, uint32_t shift) {
    uint32_t attempts = 0, size = last - first;
    for (uint32_t n = size; n; n >>= 1u) attempts += 2u;
    while (last - first > 1u) {
        if (!attempts--) { px_heap_sort(candidates, indices + first, last - first, shift); return; }
        uint32_t center = first + (last - first) / 2u, tail = last - 1u;
        if (px_axis_less(candidates, indices[center], indices[first], shift)) px_index_swap(&indices[center], &indices[first]);
        if (px_axis_less(candidates, indices[tail], indices[first], shift)) px_index_swap(&indices[tail], &indices[first]);
        if (px_axis_less(candidates, indices[tail], indices[center], shift)) px_index_swap(&indices[tail], &indices[center]);
        px_index_swap(&indices[center], &indices[tail]);
        uint32_t pivot = indices[tail], destination = first;
        for (uint32_t i = first; i < tail; ++i)
            if (px_axis_less(candidates, indices[i], pivot, shift)) px_index_swap(&indices[i], &indices[destination++]);
        px_index_swap(&indices[destination], &indices[tail]);
        if (destination == middle) return;
        if (destination < middle) first = destination + 1u; else last = destination;
    }
}
static void px_kd_build(const TerraPixelCandidate* candidates, uint32_t* indices, uint32_t first,
        uint32_t last, uint32_t axis) {
    if (first == last) return;
    uint32_t middle = first + (last - first) / 2u;
    px_select(candidates, indices, first, last, middle, (2u - axis) * 8u);
    px_kd_build(candidates, indices, first, middle, (axis + 1u) % 3u);
    px_kd_build(candidates, indices, middle + 1u, last, (axis + 1u) % 3u);
}
typedef struct PxNearestCandidate { uint32_t index, distance, rank; } PxNearestCandidate;
static void px_kd_query(const TerraPixelCandidate* candidates, const uint32_t* indices,
        uint32_t first, uint32_t last, uint32_t axis, uint32_t rgb, uint32_t flags, PxNearestCandidate* best) {
    if (first == last) return;
    uint32_t middle = first + (last - first) / 2u, index = indices[middle];
    uint32_t distance = px_distance(rgb, candidates[index].rgb), rank = px_candidate_rank(candidates[index].flags, flags);
    if (distance < best->distance || (distance == best->distance && (rank < best->rank
        || (rank == best->rank && index < best->index)))) *best = (PxNearestCandidate) { index, distance, rank };
    uint32_t shift = (2u - axis) * 8u;
    int32_t delta = (int32_t)((rgb >> shift) & 255u) - (int32_t)((candidates[index].rgb >> shift) & 255u);
    uint32_t next = (axis + 1u) % 3u;
    if (delta <= 0) {
        px_kd_query(candidates, indices, first, middle, next, rgb, flags, best);
        if ((uint32_t)(delta * delta) <= best->distance) px_kd_query(candidates, indices, middle + 1u, last, next, rgb, flags, best);
    } else {
        px_kd_query(candidates, indices, middle + 1u, last, next, rgb, flags, best);
        if ((uint32_t)(delta * delta) <= best->distance) px_kd_query(candidates, indices, first, middle, next, rgb, flags, best);
    }
}
int32_t terra_pixel_workspace_match_colors(const TerraPixelCandidate* candidates, uint32_t candidate_count,
        const uint32_t* rgb, uint32_t count, uint32_t flags, uint32_t* out) {
    if (candidate_count > 65536u || count > TERRA_PIXEL_BATCH_CELLS || (candidate_count && !candidates)
        || (count && (!rgb || !out)) || (flags & ~31u)
        || ((flags & 2u) && (flags & 8u)) || ((flags & 1u) && (flags & 16u))) return TERRA_PIXEL_INVALID;
    for (uint32_t i = 0; i < candidate_count; ++i)
        if (candidates[i].rgb > 0xffffffu || candidates[i].flags > 3u) return TERRA_PIXEL_INVALID;
    for (uint32_t i = 0; i < count; ++i) if (rgb[i] > 0xffffffu) return TERRA_PIXEL_INVALID;
    if (!count) return TERRA_PIXEL_OK;
    if (!candidate_count) { for (uint32_t i = 0; i < count; ++i) out[i] = UINT32_MAX; return TERRA_PIXEL_OK; }
    uint32_t* scratch = (uint32_t*)tx_persistent_alloc(candidate_count * 8u);
    if (!scratch) return TERRA_PIXEL_OOM;
    uint32_t *indices = scratch, *temporary = scratch + candidate_count, selected = 0;
    for (uint32_t i = 0; i < candidate_count; ++i) {
        uint32_t properties = candidates[i].flags;
        if (((flags & 1u) && (properties & 1u)) || ((flags & 2u) && !(properties & 2u))
            || ((flags & 8u) && (properties & 2u)) || ((flags & 16u) && !(properties & 1u))) continue;
        indices[selected++] = i;
    }
    for (uint32_t shift = 0; shift < 24u; shift += 8u) {
        uint32_t buckets[256] = {0}, offsets[256], offset = 0;
        for (uint32_t i = 0; i < selected; ++i) ++buckets[(candidates[indices[i]].rgb >> shift) & 255u];
        for (uint32_t i = 0; i < 256u; ++i) { offsets[i] = offset; offset += buckets[i]; }
        for (uint32_t i = 0; i < selected; ++i) temporary[offsets[(candidates[indices[i]].rgb >> shift) & 255u]++] = indices[i];
        uint32_t* swap = indices; indices = temporary; temporary = swap;
    }
    uint32_t unique = 0;
    for (uint32_t i = 0; i < selected; ++i) {
        uint32_t index = indices[i];
        if (unique && candidates[indices[unique - 1u]].rgb == candidates[index].rgb) {
            uint32_t old = indices[unique - 1u], old_rank = px_candidate_rank(candidates[old].flags, flags);
            uint32_t rank = px_candidate_rank(candidates[index].flags, flags);
            if (rank < old_rank || (rank == old_rank && index < old)) indices[unique - 1u] = index;
        } else indices[unique++] = index;
    }
    px_kd_build(candidates, indices, 0, unique, 0);
    for (uint32_t i = 0; i < count; ++i) {
        PxNearestCandidate best = { UINT32_MAX, UINT32_MAX, UINT32_MAX };
        px_kd_query(candidates, indices, 0, unique, 0, rgb[i], flags, &best); out[i] = best.index;
    }
    tx_persistent_free(scratch); return TERRA_PIXEL_OK;
}
int32_t terra_pixel_workspace_cells(uint32_t handle, const TerraPixelCell* cells, uint32_t count) {
    PxWorkspace* workspace = px_lookup(handle);
    if (!workspace) return TERRA_PIXEL_HANDLE;
    if (count > TERRA_PIXEL_BATCH_CELLS || (count && !cells)) return px_fail(workspace, TERRA_PIXEL_INVALID);
    for (uint32_t i = 0; i < count; ++i) {
        if (!px_coordinate(workspace, cells[i].x, cells[i].y)) return px_fail(workspace, TERRA_PIXEL_BOUNDS);
        if (cells[i].index >= workspace->palette_count) return px_fail(workspace, TERRA_PIXEL_PALETTE);
    }
    int32_t status = px_mutation_start(workspace); if (status) return status;
    for (uint32_t i = 0; i < count; ++i) {
        status = px_set(workspace, (uint32_t)cells[i].x, (uint32_t)cells[i].y, (uint16_t)cells[i].index);
        if (status) return px_fail(workspace, status);
    }
    return TERRA_PIXEL_OK;
}
static int32_t px_brush(PxWorkspace* workspace, int32_t x, int32_t y, uint16_t index, uint32_t size) {
    int32_t start_x = x - (int32_t)(size / 2u), start_y = y - (int32_t)(size / 2u);
    for (uint32_t dy = 0; dy < size; ++dy) for (uint32_t dx = 0; dx < size; ++dx) {
        int32_t xx = start_x + (int32_t)dx, yy = start_y + (int32_t)dy;
        if (!px_coordinate(workspace, xx, yy)) continue;
        int32_t status = px_set(workspace, (uint32_t)xx, (uint32_t)yy, index); if (status) return status;
    }
    return TERRA_PIXEL_OK;
}
int32_t terra_pixel_workspace_stroke(uint32_t handle, const TerraPixelPoint* points, uint32_t count,
        uint32_t index, uint32_t brush_size) {
    PxWorkspace* workspace = px_lookup(handle);
    if (!workspace) return TERRA_PIXEL_HANDLE;
    if (!count || count > TERRA_PIXEL_BATCH_POINTS || !points || !brush_size || brush_size > 64u)
        return px_fail(workspace, TERRA_PIXEL_INVALID);
    if (index >= workspace->palette_count) return px_fail(workspace, TERRA_PIXEL_PALETTE);
    uint64_t work = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (points[i].x < -32768 || points[i].x > 32768 || points[i].y < -32768 || points[i].y > 32768)
            return px_fail(workspace, TERRA_PIXEL_BOUNDS);
        int32_t dx = points[i].x - points[i ? i - 1u : 0u].x;
        int32_t dy = points[i].y - points[i ? i - 1u : 0u].y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        work += (uint64_t)((dx > dy ? dx : dy) + 1) * brush_size * brush_size;
    }
    if (work > 16u * 1024u * 1024u) return px_fail(workspace, TERRA_PIXEL_LIMIT);
    int32_t status = px_mutation_start(workspace); if (status) return status;
    for (uint32_t i = 0; i < count; ++i) {
        int32_t x = points[i ? i - 1u : 0u].x, y = points[i ? i - 1u : 0u].y;
        int32_t end_x = points[i].x, end_y = points[i].y;
        int32_t dx = end_x >= x ? end_x - x : x - end_x, dy = end_y >= y ? end_y - y : y - end_y;
        int32_t sx = x < end_x ? 1 : -1, sy = y < end_y ? 1 : -1, error = dx - dy;
        for (;;) {
            status = px_brush(workspace, x, y, (uint16_t)index, brush_size);
            if (status) return px_fail(workspace, status);
            if (x == end_x && y == end_y) break;
            int32_t twice = 2 * error;
            if (twice > -dy) { error -= dy; x += sx; }
            if (twice < dx) { error += dx; y += sy; }
        }
    }
    return TERRA_PIXEL_OK;
}
static int32_t px_stack_push(PxWorkspace* workspace, uint32_t** stack, uint32_t* count, uint32_t* capacity, uint32_t cell) {
    if (*count == *capacity) {
        uint32_t limit = workspace->width * workspace->height;
        uint32_t next = *capacity ? *capacity * 2u : 256u;
        if (next > limit) next = limit;
        if (next <= *capacity) return TERRA_PIXEL_LIMIT;
        uint32_t* replacement = (uint32_t*)px_alloc(workspace, next * 4u);
        if (!replacement) return workspace->allocation_error;
        if (*count) memcpy(replacement, *stack, *count * 4u);
        px_free(workspace, *stack); *stack = replacement; *capacity = next;
    }
    (*stack)[(*count)++] = cell; return TERRA_PIXEL_OK;
}
int32_t terra_pixel_workspace_fill(uint32_t handle, int32_t x, int32_t y, uint32_t index) {
    PxWorkspace* workspace = px_lookup(handle);
    if (!workspace) return TERRA_PIXEL_HANDLE;
    if (!px_coordinate(workspace, x, y)) return px_fail(workspace, TERRA_PIXEL_BOUNDS);
    if (index >= workspace->palette_count) return px_fail(workspace, TERRA_PIXEL_PALETTE);
    int32_t status = px_mutation_start(workspace); if (status) return status;
    uint16_t target = px_get(workspace, (uint32_t)x, (uint32_t)y);
    if (target == index) return TERRA_PIXEL_OK;
    uint32_t *stack = NULL, count = 0, capacity = 0;
    status = px_stack_push(workspace, &stack, &count, &capacity, (uint32_t)y * workspace->width + (uint32_t)x);
    while (!status && count) {
        uint32_t cell = stack[--count], yy = cell / workspace->width, xx = cell % workspace->width;
        if (px_get(workspace, xx, yy) != target) continue;
        uint32_t left = xx, right = xx;
        while (left && px_get(workspace, left - 1u, yy) == target) --left;
        while (right + 1u < workspace->width && px_get(workspace, right + 1u, yy) == target) ++right;
        for (xx = left; xx <= right; ++xx) {
            status = px_set(workspace, xx, yy, (uint16_t)index); if (status) break;
        }
        if (status) break;
        for (uint32_t direction = 0; direction < 2; ++direction) {
            if ((!direction && !yy) || (direction && yy + 1u == workspace->height)) continue;
            uint32_t neighbor_y = direction ? yy + 1u : yy - 1u; int in_span = 0;
            for (xx = left; xx <= right; ++xx) {
                if (px_get(workspace, xx, neighbor_y) == target) {
                    if (!in_span) {
                        status = px_stack_push(workspace, &stack, &count, &capacity, neighbor_y * workspace->width + xx);
                        if (status) break;
                    }
                    in_span = 1;
                } else in_span = 0;
            }
            if (status) break;
        }
    }
    px_free(workspace, stack);
    return status ? px_fail(workspace, status) : TERRA_PIXEL_OK;
}
int32_t terra_pixel_workspace_replace(uint32_t handle, uint32_t from_index, uint32_t to_index) {
    PxWorkspace* workspace = px_lookup(handle);
    if (!workspace) return TERRA_PIXEL_HANDLE;
    if (from_index >= workspace->palette_count || to_index >= workspace->palette_count)
        return px_fail(workspace, TERRA_PIXEL_PALETTE);
    int32_t status = px_mutation_start(workspace); if (status) return status;
    if (from_index == to_index) return TERRA_PIXEL_OK;
    for (uint32_t slot = 0; slot < workspace->slot_count; ++slot) {
        if (from_index && !workspace->slots[slot].block) continue;
        uint32_t origin_x = (slot % workspace->columns) << 6, origin_y = (slot / workspace->columns) << 6;
        for (uint32_t y = origin_y; y < origin_y + 64u && y < workspace->height; ++y)
            for (uint32_t x = origin_x; x < origin_x + 64u && x < workspace->width; ++x)
                if (px_get(workspace, x, y) == from_index) {
                    status = px_set(workspace, x, y, (uint16_t)to_index);
                    if (status) return px_fail(workspace, status);
                }
    }
    return TERRA_PIXEL_OK;
}
static int32_t px_swap(PxWorkspace* workspace, uint32_t ax, uint32_t ay, uint32_t bx, uint32_t by) {
    uint16_t a = px_get(workspace, ax, ay), b = px_get(workspace, bx, by);
    int32_t status = px_set(workspace, ax, ay, b);
    return status ? status : px_set(workspace, bx, by, a);
}
int32_t terra_pixel_workspace_transform(uint32_t handle, int32_t x, int32_t y,
        uint32_t width, uint32_t height, uint32_t kind) {
    PxWorkspace* workspace = px_lookup(handle);
    if (!workspace) return TERRA_PIXEL_HANDLE;
    if (!px_rect(workspace, x, y, width, height)) return px_fail(workspace, TERRA_PIXEL_BOUNDS);
    if (kind < TERRA_PIXEL_FLIP_X || kind > TERRA_PIXEL_ROTATE_CCW
        || (kind >= TERRA_PIXEL_ROTATE_CW && width != height)) return px_fail(workspace, TERRA_PIXEL_INVALID);
    int32_t status = px_mutation_start(workspace); if (status) return status;
    uint32_t xx = (uint32_t)x, yy = (uint32_t)y;
    if (kind <= TERRA_PIXEL_ROTATE_180) {
        uint32_t total = width * height;
        for (uint32_t i = 0; i < total; ++i) {
            uint32_t ax = i % width, ay = i / width;
            uint32_t bx = kind == TERRA_PIXEL_FLIP_Y ? ax : width - 1u - ax;
            uint32_t by = kind == TERRA_PIXEL_FLIP_X ? ay : height - 1u - ay;
            if (by * width + bx <= i) continue;
            status = px_swap(workspace, xx + ax, yy + ay, xx + bx, yy + by);
            if (status) return px_fail(workspace, status);
        }
    } else {
        for (uint32_t layer = 0; layer < width / 2u; ++layer) {
            uint32_t last = width - 1u - layer;
            for (uint32_t i = layer; i < last; ++i) {
                uint32_t offset = i - layer;
                uint32_t xs[4] = { xx + i, xx + last, xx + last - offset, xx + layer };
                uint32_t ys[4] = { yy + layer, yy + i, yy + last, yy + last - offset };
                uint16_t values[4]; for (uint32_t j = 0; j < 4; ++j) values[j] = px_get(workspace, xs[j], ys[j]);
                for (uint32_t j = 0; j < 4; ++j) {
                    status = px_set(workspace, xs[j], ys[j], values[(j + (kind == TERRA_PIXEL_ROTATE_CW ? 3u : 1u)) & 3u]);
                    if (status) return px_fail(workspace, status);
                }
            }
        }
    }
    return TERRA_PIXEL_OK;
}
static uint32_t px_quantize(uint32_t component) { return ((component * 31u + 127u) / 255u) * 255u / 31u; }
int32_t terra_pixel_workspace_import_rgba(uint32_t handle, int32_t x, int32_t y,
        uint32_t width, uint32_t height, const uint8_t* rgba, uint32_t byte_length, uint32_t stride) {
    PxWorkspace* workspace = px_lookup(handle);
    if (!workspace) return TERRA_PIXEL_HANDLE;
    if (!px_rect(workspace, x, y, width, height)) return px_fail(workspace, TERRA_PIXEL_BOUNDS);
    if ((uint64_t)width * height > TERRA_PIXEL_BATCH_CELLS || !rgba || stride < width * 4u
        || (uint64_t)(height - 1u) * stride + width * 4u > byte_length) return px_fail(workspace, TERRA_PIXEL_INVALID);
    int32_t status = px_mutation_start(workspace); if (status) return status;
    if (!workspace->import_cache) {
        workspace->import_cache = (uint32_t*)px_alloc(workspace, PX_IMPORT_COLORS * 4u);
        if (!workspace->import_cache) return px_fail(workspace, workspace->allocation_error);
        memset(workspace->import_cache, 0, PX_IMPORT_COLORS * 4u);
    }
    for (uint32_t row = 0; row < height; ++row) for (uint32_t col = 0; col < width; ++col) {
        const uint8_t* pixel = rgba + row * stride + col * 4u;
        if (pixel[3] < 16u) continue;
        uint32_t key = ((pixel[0] * 31u + 127u) / 255u << 10)
            | ((pixel[1] * 31u + 127u) / 255u << 5) | ((pixel[2] * 31u + 127u) / 255u);
        uint32_t encoded = workspace->import_cache[key];
        if (!encoded) {
            uint32_t rgb = (px_quantize(pixel[0]) << 16) | (px_quantize(pixel[1]) << 8) | px_quantize(pixel[2]);
            encoded = px_palette_add(workspace, rgb) + 1u; workspace->import_cache[key] = encoded;
        }
        if (encoded == 1u) continue;
        status = px_set(workspace, (uint32_t)x + col, (uint32_t)y + row, (uint16_t)(encoded - 1u));
        if (status) return px_fail(workspace, status);
    }
    return TERRA_PIXEL_OK;
}
int32_t terra_pixel_workspace_read_rect(uint32_t handle, int32_t x, int32_t y,
        uint32_t width, uint32_t height, uint16_t* out, uint32_t capacity_cells) {
    PxWorkspace* workspace = px_lookup(handle);
    if (!workspace) return TERRA_PIXEL_HANDLE;
    if (!px_rect(workspace, x, y, width, height)) return TERRA_PIXEL_BOUNDS;
    uint64_t count = (uint64_t)width * height;
    if (!out || count > TERRA_PIXEL_QUERY_CELLS || count > capacity_cells) return TERRA_PIXEL_INVALID;
    for (uint32_t row = 0; row < height; ++row) for (uint32_t col = 0; col < width; ++col)
        out[row * width + col] = px_get(workspace, (uint32_t)x + col, (uint32_t)y + row);
    return TERRA_PIXEL_OK;
}
int32_t terra_pixel_workspace_read_block(uint32_t handle, uint32_t bx, uint32_t by,
        uint16_t* out, uint32_t capacity_cells) {
    PxWorkspace* workspace = px_lookup(handle);
    if (!workspace) return TERRA_PIXEL_HANDLE;
    if (bx >= workspace->columns || by >= workspace->rows) return TERRA_PIXEL_BOUNDS;
    if (!out || capacity_cells < PX_BLOCK_CELLS) return TERRA_PIXEL_INVALID;
    PxBlock* block = workspace->slots[by * workspace->columns + bx].block;
    if (block) memcpy(out, block->cells, PX_BLOCK_CELLS * 2u); else memset(out, 0, PX_BLOCK_CELLS * 2u);
    return TERRA_PIXEL_OK;
}
int32_t terra_pixel_workspace_raster_block(uint32_t handle, uint32_t bx, uint32_t by,
        uint32_t level, uint8_t* out, uint32_t capacity_bytes) {
    PxWorkspace* workspace = px_lookup(handle);
    if (!workspace) return TERRA_PIXEL_HANDLE;
    if (bx >= workspace->columns || by >= workspace->rows) return TERRA_PIXEL_BOUNDS;
    if (level > 6u || !out) return TERRA_PIXEL_INVALID;
    uint32_t side = 64u >> level;
    if (capacity_bytes < side * side * 4u) return TERRA_PIXEL_INVALID;
    PxBlock* block = workspace->slots[by * workspace->columns + bx].block;
    for (uint32_t y = 0; y < side; ++y) for (uint32_t x = 0; x < side; ++x) {
        uint16_t index = block ? block->cells[((y << level) << 6) | (x << level)] : 0;
        uint32_t offset = (y * side + x) * 4u, rgb = index ? workspace->palette[index] : 0;
        out[offset] = (uint8_t)(rgb >> 16); out[offset + 1u] = (uint8_t)(rgb >> 8);
        out[offset + 2u] = (uint8_t)rgb; out[offset + 3u] = index ? 255u : 0u;
    }
    return TERRA_PIXEL_OK;
}
int32_t terra_pixel_workspace_block_versions(uint32_t handle, uint32_t first, uint32_t count,
        TerraPixelBlockInfo* out) {
    PxWorkspace* workspace = px_lookup(handle);
    if (!workspace) return TERRA_PIXEL_HANDLE;
    if (first > workspace->slot_count || count > workspace->slot_count - first
        || count > TERRA_PIXEL_QUERY_CELLS || (count && !out)) return TERRA_PIXEL_INVALID;
    for (uint32_t i = 0; i < count; ++i) {
        PxSlot* slot = &workspace->slots[first + i];
        out[i] = (TerraPixelBlockInfo) { slot->version, slot->block ? slot->block->used : 0u };
    }
    return TERRA_PIXEL_OK;
}
static int32_t px_history_apply(PxWorkspace* workspace, int redo) {
    if (workspace->in_transaction) return TERRA_PIXEL_TRANSACTION;
    if (workspace->revision == UINT32_MAX) return TERRA_PIXEL_EXHAUSTED;
    PxHistory* item = redo ? (workspace->current ? workspace->current->next : workspace->history_first) : workspace->current;
    if (!item) return TERRA_PIXEL_HISTORY;
    /* Allocate every destination block before changing any cell; failure is atomic. */
    for (uint32_t i = 0; i < item->count; ++i) {
        PxChange* change = &item->changes[i];
        uint16_t value = redo ? change->after : change->before;
        if (!value) continue;
        uint32_t x = change->cell % workspace->width, y = change->cell / workspace->width;
        PxSlot* slot = &workspace->slots[(y >> 6) * workspace->columns + (x >> 6)];
        if (slot->block) continue;
        slot->block = (PxBlock*)px_alloc(workspace, sizeof(PxBlock));
        if (!slot->block) { px_remove_empty(workspace); return workspace->allocation_error; }
        memset(slot->block, 0, sizeof(PxBlock)); ++workspace->blocks;
    }
    ++workspace->revision;
    for (uint32_t i = 0; i < item->count; ++i) {
        PxChange* change = &item->changes[i];
        uint16_t value = redo ? change->after : change->before;
        uint32_t x = change->cell % workspace->width, y = change->cell / workspace->width;
        PxSlot* slot = &workspace->slots[(y >> 6) * workspace->columns + (x >> 6)];
        uint32_t local = ((y & 63u) << 6) | (x & 63u);
        uint16_t previous = slot->block ? slot->block->cells[local] : 0;
        if (previous != value) {
            if (!previous) { ++slot->block->used; ++workspace->used; }
            if (!value) { --slot->block->used; --workspace->used; }
            slot->block->cells[local] = value;
        }
        slot->version = workspace->revision;
    }
    if (redo) { workspace->state = item->after_state; workspace->current = item; ++workspace->undo_count; --workspace->redo_count; }
    else { workspace->state = item->before_state; workspace->current = item->previous; --workspace->undo_count; ++workspace->redo_count; }
    px_remove_empty(workspace);
    return TERRA_PIXEL_OK;
}
int32_t terra_pixel_workspace_undo(uint32_t handle) {
    PxWorkspace* workspace = px_lookup(handle); return workspace ? px_history_apply(workspace, 0) : TERRA_PIXEL_HANDLE;
}
int32_t terra_pixel_workspace_redo(uint32_t handle) {
    PxWorkspace* workspace = px_lookup(handle); return workspace ? px_history_apply(workspace, 1) : TERRA_PIXEL_HANDLE;
}
int32_t terra_pixel_workspace_checkpoint(uint32_t handle) {
    PxWorkspace* workspace = px_lookup(handle);
    if (!workspace) return TERRA_PIXEL_HANDLE;
    if (workspace->in_transaction) return TERRA_PIXEL_TRANSACTION;
    workspace->checkpoint = workspace->state; return TERRA_PIXEL_OK;
}

int32_t terra_pixel_workspace_clear_history(uint32_t handle) {
    PxWorkspace* workspace = px_lookup(handle);
    if (!workspace) return TERRA_PIXEL_HANDLE;
    if (workspace->in_transaction) return TERRA_PIXEL_TRANSACTION;
    while (workspace->history_first) px_history_remove(workspace, workspace->history_first);
    workspace->current = NULL; workspace->undo_count = workspace->redo_count = 0;
    return TERRA_PIXEL_OK;
}
