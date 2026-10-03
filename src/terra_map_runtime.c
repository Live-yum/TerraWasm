#include "terra_map_runtime.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern uint8_t* tx_persistent_alloc(uint32_t size);
extern void tx_persistent_free(void* payload);
extern uint32_t tx_get_world_open_count(void);
extern int tx_open_task_pending(void);
extern int tx_stream_task_pending(void);
extern int tx_bridge_range_is_valid(uint32_t ptr, uint32_t length);
extern uint32_t tx_bridge_allocation_size(uint32_t ptr);
extern void tx_set_error(const char* code, const char* message);
extern void tx_clear_error(void);

static uint8_t* runtime_data;
static uint32_t runtime_len;
static TxMapRuntimeLayout runtime_layout;

static int runtime_in_use(void) {
    return tx_get_world_open_count() || tx_open_task_pending() || tx_stream_task_pending();
}

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static uint16_t le16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8u));
}

static int invalid(const char* message) {
    tx_set_error("TERRAX_INVALID_ARGUMENT", message);
    return -1;
}

static int validate_lookup(const uint8_t* entries, uint32_t count,
                           uint32_t start, uint32_t end) {
    uint32_t next = start;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* p = entries + i * 4u;
        uint32_t base = le16(p);
        uint32_t options = p[2];
        if (p[3] != 0u) return 0;
        if (options == 0u) {
            if (base != 0u) return 0;
        } else {
            if (base != next || options > end - next) return 0;
            next += options;
        }
    }
    return next == end;
}

static int validate_runtime(const uint8_t* data, uint32_t len,
                            TxMapRuntimeLayout* layout) {
    uint64_t expected;
    uint32_t words[16];
    int sha_nonzero = 0;
    if (!data || len < TX_MAP_RUNTIME_HEADER_SIZE || len > TX_MAP_RUNTIME_MAX_BYTES)
        return invalid("TMRT payload length is out of range");
    for (uint32_t i = 0; i < 16u; i++) words[i] = le32(data + i * 4u);
    if (words[0] != TX_MAP_RUNTIME_MAGIC || words[1] != TX_MAP_RUNTIME_SCHEMA ||
        words[2] != len || words[3] != TX_MAP_RUNTIME_FORMAT)
        return invalid("TMRT magic, schema, length or MAP format is unsupported");
    for (uint32_t i = 64u; i < 96u; i++) sha_nonzero |= data[i];
    if (!words[4] || !sha_nonzero) return invalid("TMRT release identity is empty");

    layout->cur_release = words[4];
    layout->tile_count = words[5];
    layout->wall_count = words[6];
    layout->palette_count = words[7];
    layout->tile_pos = words[8];
    layout->wall_pos = words[9];
    layout->liquid_pos = words[10];
    layout->sky_pos = words[11];
    layout->dirt_pos = words[12];
    layout->rock_pos = words[13];
    layout->hell_pos = words[14];
    layout->paint_count = words[15];

    if (!layout->tile_count || !layout->wall_count ||
        layout->tile_count > 65535u || layout->wall_count > 65535u ||
        layout->palette_count > 65535u || layout->tile_pos != 1u ||
        layout->wall_pos <= layout->tile_pos ||
        layout->liquid_pos <= layout->wall_pos ||
        layout->sky_pos <= layout->liquid_pos ||
        layout->dirt_pos <= layout->sky_pos ||
        layout->rock_pos <= layout->dirt_pos ||
        layout->hell_pos <= layout->rock_pos ||
        layout->palette_count <= layout->hell_pos ||
        layout->sky_pos - layout->liquid_pos != 4u ||
        layout->dirt_pos - layout->sky_pos != 256u ||
        layout->rock_pos - layout->dirt_pos != 256u ||
        layout->hell_pos - layout->rock_pos != 256u ||
        layout->palette_count - layout->hell_pos != 1u ||
        layout->paint_count != 30u ||
        layout->liquid_pos > layout->palette_count ||
        layout->sky_pos > layout->palette_count ||
        layout->dirt_pos > layout->palette_count ||
        layout->rock_pos > layout->palette_count ||
        layout->hell_pos > layout->palette_count)
        return invalid("TMRT map layout is unsupported or out of range");

    expected = TX_MAP_RUNTIME_HEADER_SIZE +
        ((uint64_t)layout->tile_count + layout->wall_count) * 4u +
        (uint64_t)layout->palette_count * 4u +
        (uint64_t)layout->paint_count * 3u;
    if (expected != len) return invalid("TMRT sections do not fill payload exactly");
    if (!validate_lookup(data + TX_MAP_RUNTIME_HEADER_SIZE, layout->tile_count,
                         layout->tile_pos, layout->wall_pos) ||
        !validate_lookup(data + TX_MAP_RUNTIME_HEADER_SIZE + layout->tile_count * 4u,
                         layout->wall_count, layout->wall_pos, layout->liquid_pos))
        return invalid("TMRT lookup ranges are not contiguous");
    {
        const uint8_t* palette = data + TX_MAP_RUNTIME_HEADER_SIZE +
            (layout->tile_count + layout->wall_count) * 4u;
        for (uint32_t i = 1u; i < layout->palette_count; i++) {
            if (palette[i * 4u + 3u] != 255u)
                return invalid("TMRT palette is incomplete or nonopaque");
        }
    }
    return 0;
}

int32_t txw_set_map_runtime_from_buffer(const uint8_t* data, uint32_t data_len) {
    TxMapRuntimeLayout candidate;
    uint8_t* copy;
    if (!data && data_len == 0u) {
        if (runtime_in_use()) return invalid("close worlds and tasks before clearing TMRT");
        tx_persistent_free(runtime_data);
        runtime_data = NULL;
        runtime_len = 0u;
        memset(&runtime_layout, 0, sizeof(runtime_layout));
        tx_clear_error();
        return 0;
    }
    if (validate_runtime(data, data_len, &candidate) < 0) return -1;
    if (runtime_data && data_len == runtime_len &&
        memcmp(runtime_data, data, data_len) == 0) {
        tx_clear_error();
        return 0;
    }
    if (tx_open_task_pending() || tx_stream_task_pending())
        return invalid("close tasks before installing TMRT");
    if (runtime_data && tx_get_world_open_count())
        return invalid("close worlds and tasks before replacing TMRT");
    copy = tx_persistent_alloc(data_len);
    if (!copy) {
        tx_set_error("TERRAX_WASM_OOM", "TMRT persistent allocation failed");
        return -1;
    }
    memcpy(copy, data, data_len);
    tx_persistent_free(runtime_data);
    runtime_data = copy;
    runtime_len = data_len;
    runtime_layout = candidate;
    tx_clear_error();
    return 0;
}

int32_t txw_set_map_runtime(uint32_t data_ptr, uint32_t data_len) {
    if (!data_ptr && !data_len) return txw_set_map_runtime_from_buffer(NULL, 0u);
    if (!data_ptr || !data_len || !tx_bridge_range_is_valid(data_ptr, data_len) ||
        tx_bridge_allocation_size(data_ptr) != data_len)
        return invalid("TMRT payload exceeds its bridge allocation");
    return txw_set_map_runtime_from_buffer((const uint8_t*)(uintptr_t)data_ptr, data_len);
}

int tx_map_runtime_is_set(void) { return runtime_data != NULL; }

const TxMapRuntimeLayout* tx_map_runtime_layout(void) {
    return runtime_data ? &runtime_layout : NULL;
}

int tx_map_runtime_lookup(int wall, uint32_t type, uint16_t* map_index, uint8_t* options) {
    const uint8_t* p;
    uint32_t offset;
    if (!runtime_data || !map_index || !options) return 0;
    if (wall) {
        if (type >= runtime_layout.wall_count) return 0;
        offset = runtime_layout.tile_count + type;
    } else {
        if (type >= runtime_layout.tile_count) return 0;
        offset = type;
    }
    p = runtime_data + TX_MAP_RUNTIME_HEADER_SIZE + offset * 4u;
    *map_index = le16(p);
    *options = p[2];
    return p[2] != 0u;
}

int tx_map_runtime_color(uint32_t map_index, uint8_t rgba[4]) {
    const uint8_t* p;
    if (!runtime_data || !rgba || map_index >= runtime_layout.palette_count) return 0;
    p = runtime_data + TX_MAP_RUNTIME_HEADER_SIZE +
        (runtime_layout.tile_count + runtime_layout.wall_count) * 4u + map_index * 4u;
    memcpy(rgba, p, 4u);
    return 1;
}

int tx_map_runtime_paint(uint32_t paint_id, uint8_t rgb[3]) {
    const uint8_t* p;
    if (!runtime_data || !rgb || paint_id == 0u || paint_id > runtime_layout.paint_count) return 0;
    p = runtime_data + TX_MAP_RUNTIME_HEADER_SIZE +
        (runtime_layout.tile_count + runtime_layout.wall_count + runtime_layout.palette_count) * 4u +
        (paint_id - 1u) * 3u;
    memcpy(rgb, p, 3u);
    return 1;
}
