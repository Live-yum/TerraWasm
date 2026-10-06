/* tModLoader gzip TagCompound/NBT and TileIO stream codec.
 * TileIO reference: tModLoader/tModLoader@2da2c59a6fd84990a5e01e48bc0e99dd0de87ee2,
 * patches/tModLoader/Terraria/ModLoader/IO/TileIO_Basic.cs.
 * Metadata is read before a second streaming pass because tileMap can occur after
 * a several-hundred-megabyte tileData array. No expanded world array is retained.
 */
#include "terra_circuit_twld.h"
#include <limits.h>
#include <stddef.h>
#include <string.h>
#include <zlib.h>

extern uint8_t* tx_persistent_alloc(uint32_t size);
extern void tx_persistent_free(void* pointer);

#define TW_BUFFER 65536u
#define TW_OVERHEAD 128u
#define TW_KNOWN 1u
#define TW_FRAMED 2u
#define TW_COLOR 4u

enum TwState { TW_ROOT, TW_TAG, TW_NAME_LENGTH, TW_NAME, TW_SCALAR,
    TW_STRING_LENGTH, TW_STRING, TW_LIST, TW_ARRAY_LENGTH,
    TW_ARRAY, TW_TILES, TW_DONE };
enum TwRole { TW_OTHER, TW_ROOT_ROLE, TW_TILES_ROLE, TW_TILE_DATA,
    TW_TILE_MAP, TW_MAP_ENTRY, TW_ENTRY_VALUE, TW_ENTRY_MOD,
    TW_ENTRY_NAME, TW_ENTRY_FRAMED, TW_ENTITY_LIST, TW_ENTITY,
    TW_ENTITY_X, TW_ENTITY_Y };
typedef union TwAllocation { struct { uint32_t bytes; } value; max_align_t alignment; } TwAllocation;
typedef struct TwContainer { uint32_t type, role, remaining, element; } TwContainer;

struct TerraTwld {
    TerraTwldCallbacks callbacks;
    void* context;
    z_stream inflater, deflater;
    uint32_t inflater_ready, deflater_ready, stream_end, output_enabled;
    uint32_t width, height, maximum, allocated, peak, pass, complete;
    uint32_t metadata_ready, replay_idle;
    uint64_t compressed, expanded, cells, mod_tiles, color_pixels;
    uint64_t source_bytes;
    uint32_t crc, source_crc, map_count, saw_map, saw_tiles;
    uint8_t* map;
    uint8_t *inflate_buffer, *deflate_buffer;
    TwContainer* stack;
    uint32_t depth, stack_capacity;
    uint32_t state, type, role, root_name, need, have, array_multiplier;
    uint8_t small[8];
    char text[64];
    uint32_t text_length, text_count;
    uint64_t remaining, tile_index;
    uint8_t tile_record[7];
    uint32_t tile_have, tile_need;
    uint16_t tile_type;
    uint32_t entry_value, entry_has_value, entry_framed, entry_mod, entry_name;
    uint32_t entry_x, entry_y, entry_has_x, entry_has_y, training_dummies, logic_sensors;
    int32_t error;
};

static void* tw_allocate(TerraTwld* t, uint32_t bytes) {
    uint64_t requested = (uint64_t)bytes + sizeof(TwAllocation);
    uint64_t charged = requested + TW_OVERHEAD;
    if (requested > UINT32_MAX || t->allocated > t->maximum || charged > (uint64_t)t->maximum - t->allocated) {
        t->error = TERRA_TWLD_LIMIT; return NULL;
    }
    TwAllocation* a = (TwAllocation*)tx_persistent_alloc((uint32_t)requested);
    if (!a) { t->error = TERRA_TWLD_OOM; return NULL; }
    a->value.bytes = (uint32_t)charged;
    t->allocated += (uint32_t)charged;
    if (t->allocated > t->peak) t->peak = t->allocated;
    return a + 1;
}
static void tw_free(TerraTwld* t, void* pointer) {
    if (!pointer) return;
    TwAllocation* a = (TwAllocation*)pointer - 1;
    t->allocated -= a->value.bytes;
    tx_persistent_free(a);
}
static voidpf tw_zalloc(voidpf opaque, uInt items, uInt size) {
    TerraTwld* t = (TerraTwld*)opaque;
    uint64_t bytes = (uint64_t)items * size;
    if (bytes > UINT32_MAX) { t->error = TERRA_TWLD_LIMIT; return NULL; }
    return tw_allocate(t, (uint32_t)bytes);
}
static void tw_zfree(voidpf opaque, voidpf address) { tw_free((TerraTwld*)opaque, address); }
static uint32_t tw_be(const uint8_t* p, uint32_t bytes) {
    uint32_t value = 0;
    for (uint32_t i = 0; i < bytes; ++i) value = (value << 8) | p[i];
    return value;
}

static int32_t tw_output(TerraTwld* t, const uint8_t* bytes, uint32_t count, int finish) {
    if (!t->output_enabled) return 0;
    t->deflater.next_in = (Bytef*)bytes; t->deflater.avail_in = count;
    int code;
    do {
        t->deflater.next_out = t->deflate_buffer; t->deflater.avail_out = TW_BUFFER;
        code = deflate(&t->deflater, finish ? Z_FINISH : Z_NO_FLUSH);
        if (code != Z_OK && code != Z_STREAM_END) return t->error ? t->error : TERRA_TWLD_FORMAT;
        uint32_t size = TW_BUFFER - t->deflater.avail_out;
        if (size && t->callbacks.output(t->context, t->deflate_buffer, size) < 0) return TERRA_TWLD_CALLBACK;
    } while (t->deflater.avail_in || !t->deflater.avail_out || (finish && code != Z_STREAM_END));
    return 0;
}

static void tw_need(TerraTwld* t, uint32_t state, uint32_t bytes) {
    t->state = state; t->need = bytes; t->have = 0;
}
static int32_t tw_begin_value(TerraTwld* t, uint32_t type, uint32_t role);

static int32_t tw_finish_value(TerraTwld* t) {
    while (t->depth) {
        TwContainer* parent = &t->stack[t->depth - 1];
        if (parent->type == 10) { t->state = TW_TAG; return 0; }
        if (parent->type != 9 || !parent->remaining) return TERRA_TWLD_FORMAT;
        if (--parent->remaining) {
            uint32_t role = parent->role == TW_TILE_MAP ? TW_MAP_ENTRY :
                parent->role == TW_ENTITY_LIST ? TW_ENTITY : TW_OTHER;
            return tw_begin_value(t, parent->element, role);
        }
        --t->depth;
    }
    t->state = TW_DONE; return 0;
}

static int32_t tw_push(TerraTwld* t, uint32_t type, uint32_t role, uint32_t count, uint32_t element) {
    if (t->depth == t->stack_capacity) {
        if (t->stack_capacity > UINT32_MAX / 2u / sizeof(TwContainer)) return TERRA_TWLD_LIMIT;
        uint32_t size = t->stack_capacity ? t->stack_capacity * 2u : 16u;
        TwContainer* stack = (TwContainer*)tw_allocate(t, size * (uint32_t)sizeof(TwContainer));
        if (!stack) return t->error;
        if (t->depth) memcpy(stack, t->stack, t->depth * sizeof(TwContainer));
        tw_free(t, t->stack); t->stack = stack; t->stack_capacity = size;
    }
    t->stack[t->depth++] = (TwContainer){type, role, count, element};
    if (role == TW_MAP_ENTRY || role == TW_ENTITY) {
        t->entry_value = t->entry_has_value = t->entry_framed = t->entry_mod = t->entry_name = 0;
        t->entry_x = t->entry_y = t->entry_has_x = t->entry_has_y = 0;
    }
    return 0;
}

static int32_t tw_begin_value(TerraTwld* t, uint32_t type, uint32_t role) {
    t->type = type; t->role = role;
    if ((role == TW_ROOT_ROLE || role == TW_TILES_ROLE || role == TW_MAP_ENTRY || role == TW_ENTITY) && type != 10)
        return TERRA_TWLD_FORMAT;
    if ((role == TW_TILE_MAP || role == TW_ENTITY_LIST) && type != 9) return TERRA_TWLD_FORMAT;
    if (role == TW_TILE_DATA && type != 7) return TERRA_TWLD_FORMAT;
    switch (type) {
    case 1: tw_need(t, TW_SCALAR, 1); return 0;
    case 2: tw_need(t, TW_SCALAR, 2); return 0;
    case 3: case 5: tw_need(t, TW_SCALAR, 4); return 0;
    case 4: case 6: tw_need(t, TW_SCALAR, 8); return 0;
    case 7: t->array_multiplier = 1; tw_need(t, TW_ARRAY_LENGTH, 4); return 0;
    case 8: tw_need(t, TW_STRING_LENGTH, 2); return 0;
    case 9: tw_need(t, TW_LIST, 5); return 0;
    case 10: {
        int32_t status = tw_push(t, 10, role, 0, 0);
        if (status < 0) return status;
        t->state = TW_TAG; return 0;
    }
    case 11: t->array_multiplier = 4; tw_need(t, TW_ARRAY_LENGTH, 4); return 0;
    case 12: t->array_multiplier = 8; tw_need(t, TW_ARRAY_LENGTH, 4); return 0;
    default: return TERRA_TWLD_FORMAT;
    }
}

static uint32_t tw_name_role(const TerraTwld* t) {
    if (t->root_name) return TW_ROOT_ROLE;
    if (!t->depth || t->text_length >= sizeof(t->text)) return TW_OTHER;
    uint32_t parent = t->stack[t->depth - 1].role;
    if (parent == TW_ROOT_ROLE && !strcmp(t->text, "tiles")) return TW_TILES_ROLE;
    if (parent == TW_ROOT_ROLE && !strcmp(t->text, "tileEntities")) return TW_ENTITY_LIST;
    if (parent == TW_TILES_ROLE) {
        if (!strcmp(t->text, "tileData")) return TW_TILE_DATA;
        if (!strcmp(t->text, "tileMap")) return TW_TILE_MAP;
    }
    if (parent == TW_MAP_ENTRY) {
        if (!strcmp(t->text, "value")) return TW_ENTRY_VALUE;
        if (!strcmp(t->text, "mod")) return TW_ENTRY_MOD;
        if (!strcmp(t->text, "name")) return TW_ENTRY_NAME;
        if (!strcmp(t->text, "framed")) return TW_ENTRY_FRAMED;
    }
    if (parent == TW_ENTITY) {
        if (!strcmp(t->text, "mod")) return TW_ENTRY_MOD;
        if (!strcmp(t->text, "name")) return TW_ENTRY_NAME;
        if (!strcmp(t->text, "X")) return TW_ENTITY_X;
        if (!strcmp(t->text, "Y")) return TW_ENTITY_Y;
    }
    return TW_OTHER;
}

static int32_t tw_finish_entry(TerraTwld* t) {
    if (!t->entry_has_value || t->entry_value > UINT16_MAX || !t->entry_value) return TERRA_TWLD_FORMAT;
    uint8_t flags = TW_KNOWN | (t->entry_framed ? TW_FRAMED : 0u) |
        ((t->entry_mod && t->entry_name) ? TW_COLOR : 0u);
    if (t->pass == 1) {
        if (t->map[t->entry_value]) return TERRA_TWLD_FORMAT;
        t->map[t->entry_value] = flags; ++t->map_count;
    } else if (t->map[t->entry_value] != flags) return TERRA_TWLD_CHANGED;
    return 0;
}

static int32_t tw_finish_entity(TerraTwld* t) {
    if (t->pass == 1 || !t->entry_mod || !t->entry_name) return 0;
    if (!t->entry_has_x || !t->entry_has_y || t->entry_x >= t->width || t->entry_y >= t->height)
        return TERRA_TWLD_FORMAT;
    TerraTwldEntity entity = {t->entry_x, t->entry_y, t->entry_name};
    if (entity.kind == TERRA_TWLD_TRAINING_DUMMY) ++t->training_dummies;
    else if (entity.kind == TERRA_TWLD_LOGIC_SENSOR) ++t->logic_sensors;
    if (t->callbacks.entity && t->callbacks.entity(t->context, &entity) < 0) return TERRA_TWLD_CALLBACK;
    return 0;
}

static int32_t tw_record(TerraTwld* t) {
    if (t->tile_index >= t->cells) return TERRA_TWLD_FORMAT;
    uint8_t flags = t->map[t->tile_type];
    TerraTwldTile tile;
    memset(&tile, 0, sizeof(tile));
    tile.x = (uint32_t)(t->tile_index / t->height);
    tile.y = (uint32_t)(t->tile_index % t->height);
    tile.saved_type = t->tile_type; tile.paint = t->tile_record[2];
    tile.framed = !!(flags & TW_FRAMED); tile.color_pixel_box = !!(flags & TW_COLOR);
    if (tile.framed) {
        tile.frame_x = (int16_t)((uint16_t)t->tile_record[3] | ((uint16_t)t->tile_record[4] << 8));
        tile.frame_y = (int16_t)((uint16_t)t->tile_record[5] | ((uint16_t)t->tile_record[6] << 8));
    }
    if (t->callbacks.tile && t->callbacks.tile(t->context, &tile) < 0) return TERRA_TWLD_CALLBACK;
    if (t->output_enabled && t->callbacks.patch) {
        TerraTwldTile previous = tile;
        if (t->callbacks.patch(t->context, &tile) < 0) return TERRA_TWLD_CALLBACK;
        if (tile.x != previous.x || tile.y != previous.y || tile.saved_type != previous.saved_type ||
            tile.framed != previous.framed || tile.color_pixel_box != previous.color_pixel_box) return TERRA_TWLD_CALLBACK;
        t->tile_record[2] = tile.paint;
        if (tile.framed) {
            t->tile_record[3] = (uint8_t)tile.frame_x; t->tile_record[4] = (uint8_t)((uint16_t)tile.frame_x >> 8);
            t->tile_record[5] = (uint8_t)tile.frame_y; t->tile_record[6] = (uint8_t)((uint16_t)tile.frame_y >> 8);
        }
    }
    int32_t status = tw_output(t, t->tile_record, t->tile_need, 0);
    if (status < 0) return status;
    ++t->tile_index; ++t->mod_tiles;
    if (tile.color_pixel_box) ++t->color_pixels;
    t->tile_have = 0; t->tile_need = 2;
    return 0;
}

static uint32_t tw_zero_prefix(const uint8_t* data, uint32_t size) {
    uint32_t count = 0;
    while (size - count >= 8) {
        uint64_t value; memcpy(&value, data + count, 8);
        if (value) break;
        count += 8;
    }
    while (count < size && data[count] == 0) ++count;
    return count;
}

static int32_t tw_parse_tiles(TerraTwld* t, const uint8_t* bytes, uint32_t size, uint32_t* consumed) {
    uint32_t index = 0;
    if ((uint64_t)size > t->remaining) size = (uint32_t)t->remaining;
    while (index < size) {
        if (!t->tile_have && t->tile_need == 2) {
            uint32_t zeros = tw_zero_prefix(bytes + index, size - index) & ~1u;
            if (zeros) {
                if (t->tile_index + zeros / 2u > t->cells) return TERRA_TWLD_FORMAT;
                int32_t status = tw_output(t, bytes + index, zeros, 0);
                if (status < 0) return status;
                t->tile_index += zeros / 2u; index += zeros;
                continue;
            }
        }
        uint32_t chunk = t->tile_need - t->tile_have;
        if (chunk > size - index) chunk = size - index;
        memcpy(t->tile_record + t->tile_have, bytes + index, chunk);
        t->tile_have += chunk; index += chunk;
        if (t->tile_have != t->tile_need) continue;
        if (t->tile_need == 2) {
            t->tile_type = (uint16_t)t->tile_record[0] | ((uint16_t)t->tile_record[1] << 8);
            if (!t->tile_type) {
                if (++t->tile_index > t->cells) return TERRA_TWLD_FORMAT;
                int32_t status = tw_output(t, t->tile_record, 2, 0);
                if (status < 0) return status;
                t->tile_have = 0;
            } else {
                uint8_t flags = t->map[t->tile_type];
                if (!(flags & TW_KNOWN)) return TERRA_TWLD_FORMAT;
                t->tile_need = flags & TW_FRAMED ? 7u : 3u;
            }
        } else {
            int32_t status = tw_record(t);
            if (status < 0) return status;
        }
    }
    t->remaining -= index;
    *consumed = index;
    if (!t->remaining) {
        if (t->tile_have || t->tile_index != t->cells) return TERRA_TWLD_FORMAT;
        return tw_finish_value(t);
    }
    return 0;
}

static int32_t tw_finish_scalar(TerraTwld* t) {
    uint32_t value;
    switch (t->state) {
    case TW_NAME_LENGTH:
        t->text_length = tw_be(t->small, 2); t->text_count = 0;
        memset(t->text, 0, sizeof(t->text)); t->state = TW_NAME;
        if (!t->text_length) {
            uint32_t role = tw_name_role(t); t->root_name = 0;
            return tw_begin_value(t, t->type, role);
        }
        return 0;
    case TW_SCALAR:
        if (t->role == TW_ENTRY_VALUE || t->role == TW_ENTRY_FRAMED ||
            t->role == TW_ENTITY_X || t->role == TW_ENTITY_Y) {
            if (t->type < 1 || t->type > 3) return TERRA_TWLD_FORMAT;
            value = tw_be(t->small, t->need);
            if (t->role == TW_ENTRY_VALUE) {
                if (!value || value > UINT16_MAX || t->entry_has_value) return TERRA_TWLD_FORMAT;
                t->entry_value = value; t->entry_has_value = 1;
            } else if (t->role == TW_ENTRY_FRAMED) {
                if (value > 1u) return TERRA_TWLD_FORMAT;
                t->entry_framed = value;
            } else if (t->role == TW_ENTITY_X) {
                if (t->entry_has_x) return TERRA_TWLD_FORMAT;
                t->entry_x = value; t->entry_has_x = 1;
            } else {
                if (t->entry_has_y) return TERRA_TWLD_FORMAT;
                t->entry_y = value; t->entry_has_y = 1;
            }
        }
        return tw_finish_value(t);
    case TW_STRING_LENGTH:
        t->text_length = tw_be(t->small, 2); t->text_count = 0;
        memset(t->text, 0, sizeof(t->text)); t->state = TW_STRING;
        return t->text_length ? 0 : tw_finish_value(t);
    case TW_LIST: {
        uint32_t element = t->small[0]; value = tw_be(t->small + 1, 4);
        if (value > INT32_MAX || element > 12u || (value && !element)) return TERRA_TWLD_FORMAT;
        if (t->role == TW_TILE_MAP) {
            if (t->saw_map || (value && element != 10)) return TERRA_TWLD_FORMAT;
            t->saw_map = 1;
        }
        if (!value) return tw_finish_value(t);
        uint32_t role = t->role;
        int32_t status = tw_push(t, 9, role, value, element);
        if (status < 0) return status;
        return tw_begin_value(t, element, role == TW_TILE_MAP ? TW_MAP_ENTRY :
            role == TW_ENTITY_LIST ? TW_ENTITY : TW_OTHER);
    }
    case TW_ARRAY_LENGTH:
        value = tw_be(t->small, 4);
        if (value > INT32_MAX) return TERRA_TWLD_FORMAT;
        t->remaining = (uint64_t)value * t->array_multiplier;
        if (t->role == TW_TILE_DATA) {
            if (t->saw_tiles) return TERRA_TWLD_FORMAT;
            t->saw_tiles = 1;
            if (t->pass == 2) {
                t->tile_index = t->tile_have = 0; t->tile_need = 2;
                t->state = TW_TILES;
                if (!t->remaining) return TERRA_TWLD_FORMAT;
                return 0;
            }
        }
        t->state = TW_ARRAY;
        return t->remaining ? 0 : tw_finish_value(t);
    default: return TERRA_TWLD_FORMAT;
    }
}

static int32_t tw_parse(TerraTwld* t, const uint8_t* bytes, uint32_t size) {
    uint32_t index = 0;
    while (index < size) {
        uint32_t chunk = 1;
        int32_t status;
        if (t->state == TW_TILES) {
            status = tw_parse_tiles(t, bytes + index, size - index, &chunk);
            if (status < 0) return status;
            if (!chunk) return TERRA_TWLD_FORMAT;
            index += chunk; continue;
        }
        if (t->state == TW_ARRAY) {
            chunk = size - index;
            if ((uint64_t)chunk > t->remaining) chunk = (uint32_t)t->remaining;
            status = tw_output(t, bytes + index, chunk, 0);
            if (status < 0) return status;
            index += chunk; t->remaining -= chunk;
            if (!t->remaining) { status = tw_finish_value(t); if (status < 0) return status; }
            continue;
        }
        if (t->state == TW_NAME || t->state == TW_STRING) {
            chunk = t->text_length - t->text_count;
            if (chunk > size - index) chunk = size - index;
            status = tw_output(t, bytes + index, chunk, 0);
            if (status < 0) return status;
            uint32_t capture = sizeof(t->text) - 1u;
            if (t->text_count < capture) {
                uint32_t take = chunk;
                if (take > capture - t->text_count) take = capture - t->text_count;
                memcpy(t->text + t->text_count, bytes + index, take);
            }
            t->text_count += chunk; index += chunk;
            if (t->text_count == t->text_length) {
                if (t->state == TW_NAME) {
                    uint32_t role = tw_name_role(t); t->root_name = 0;
                    status = tw_begin_value(t, t->type, role);
                } else {
                    if (t->text_length < sizeof(t->text)) {
                        int entity = t->depth && t->stack[t->depth - 1].role == TW_ENTITY;
                        if (t->role == TW_ENTRY_MOD) t->entry_mod = !strcmp(t->text, entity ? "Terraria" : "WireHead");
                        if (t->role == TW_ENTRY_NAME) {
                            t->entry_name = entity ? (!strcmp(t->text, "TETrainingDummy") ? TERRA_TWLD_TRAINING_DUMMY :
                                !strcmp(t->text, "TELogicSensor") ? TERRA_TWLD_LOGIC_SENSOR : 0u) :
                                (uint32_t)!strcmp(t->text, "ColorPixelBox");
                        }
                    }
                    status = tw_finish_value(t);
                }
                if (status < 0) return status;
            }
            continue;
        }
        if (t->state == TW_ROOT || t->state == TW_TAG) {
            uint8_t type = bytes[index++];
            status = tw_output(t, &type, 1, 0); if (status < 0) return status;
            if (t->state == TW_ROOT) {
                if (type != 10) return TERRA_TWLD_FORMAT;
                t->root_name = 1;
            } else if (!type) {
                if (!t->depth || t->stack[t->depth - 1].type != 10) return TERRA_TWLD_FORMAT;
                uint32_t role = t->stack[--t->depth].role;
                if (role == TW_MAP_ENTRY) { status = tw_finish_entry(t); if (status < 0) return status; }
                if (role == TW_ENTITY) { status = tw_finish_entity(t); if (status < 0) return status; }
                status = tw_finish_value(t); if (status < 0) return status;
                continue;
            } else if (type > 12u) return TERRA_TWLD_FORMAT;
            t->type = type; tw_need(t, TW_NAME_LENGTH, 2);
            continue;
        }
        if (t->state == TW_DONE) return TERRA_TWLD_FORMAT;
        if (!t->need || t->need > sizeof(t->small) || t->have >= t->need) return TERRA_TWLD_FORMAT;
        chunk = t->need - t->have;
        if (chunk > size - index) chunk = size - index;
        status = tw_output(t, bytes + index, chunk, 0); if (status < 0) return status;
        memcpy(t->small + t->have, bytes + index, chunk);
        t->have += chunk; index += chunk;
        if (t->have == t->need) { status = tw_finish_scalar(t); if (status < 0) return status; }
    }
    return 0;
}

static int32_t tw_inflate_start(TerraTwld* t) {
    memset(&t->inflater, 0, sizeof(t->inflater));
    t->inflater.zalloc = tw_zalloc; t->inflater.zfree = tw_zfree; t->inflater.opaque = t;
    int status = inflateInit2(&t->inflater, 31);
    if (status != Z_OK) return t->error ? t->error : TERRA_TWLD_FORMAT;
    t->inflater_ready = 1; return 0;
}

int32_t terra_twld_create(uint32_t width, uint32_t height, uint32_t max_bytes,
                          const TerraTwldCallbacks* callbacks, void* context,
                          TerraTwld** out) {
    if (!out || !width || !height) return TERRA_TWLD_INVALID;
    *out = NULL;
    if (!max_bytes) max_bytes = UINT32_MAX;
    if ((uint64_t)sizeof(TerraTwld) + TW_OVERHEAD > max_bytes) return TERRA_TWLD_LIMIT;
    TerraTwld* t = (TerraTwld*)tx_persistent_alloc((uint32_t)sizeof(TerraTwld));
    if (!t) return TERRA_TWLD_OOM;
    memset(t, 0, sizeof(*t));
    if (callbacks) t->callbacks = *callbacks;
    t->context = context; t->width = width; t->height = height; t->cells = (uint64_t)width * height;
    t->maximum = max_bytes; t->allocated = t->peak = (uint32_t)sizeof(TerraTwld) + TW_OVERHEAD;
    t->pass = 1; t->state = TW_ROOT;
    t->map = (uint8_t*)tw_allocate(t, 65536u);
    t->inflate_buffer = (uint8_t*)tw_allocate(t, TW_BUFFER);
    if (!t->map || !t->inflate_buffer) {
        int32_t status = t->error; terra_twld_destroy(t); return status;
    }
    memset(t->map, 0, 65536u);
    int32_t status = tw_inflate_start(t);
    if (status < 0) { terra_twld_destroy(t); return status; }
    *out = t; return 0;
}

int32_t terra_twld_feed(TerraTwld* t, const uint8_t* bytes, uint32_t count, uint32_t final) {
    if (!t || (count && !bytes) || final > 1u) return TERRA_TWLD_INVALID;
    if (t->complete || t->replay_idle || t->error) return t->error ? t->error : TERRA_TWLD_STATE;
    if (t->allocated > t->maximum) { t->error = TERRA_TWLD_LIMIT; return t->error; }
    if (t->stream_end && count) { t->error = TERRA_TWLD_FORMAT; return t->error; }
    if (count) t->crc = (uint32_t)crc32(t->crc, bytes, count);
    t->compressed += count;
    t->inflater.next_in = (Bytef*)bytes; t->inflater.avail_in = count;
    while (!t->stream_end && (t->inflater.avail_in || final)) {
        uint32_t before = t->inflater.avail_in;
        t->inflater.next_out = t->inflate_buffer; t->inflater.avail_out = TW_BUFFER;
        int code = inflate(&t->inflater, Z_NO_FLUSH);
        if (code != Z_OK && code != Z_STREAM_END && code != Z_BUF_ERROR) {
            t->error = t->error ? t->error : TERRA_TWLD_FORMAT; return t->error;
        }
        uint32_t produced = TW_BUFFER - t->inflater.avail_out;
        if (produced) {
            int32_t status = tw_parse(t, t->inflate_buffer, produced);
            t->expanded += produced;
            if (status < 0) { t->error = status; return status; }
        }
        if (code == Z_STREAM_END) {
            t->stream_end = 1;
            if (t->inflater.avail_in) { t->error = TERRA_TWLD_FORMAT; return t->error; }
            break;
        }
        if (!produced && before == t->inflater.avail_in) break;
        if (!t->inflater.avail_in && t->inflater.avail_out && !final) break;
    }
    if (!final) return TERRA_TWLD_MORE;
    if (!t->stream_end || t->state != TW_DONE || t->depth) { t->error = TERRA_TWLD_TRUNCATED; return t->error; }
    if (!t->saw_tiles || !t->saw_map) { t->error = TERRA_TWLD_FORMAT; return t->error; }
    if (t->pass == 1) { t->source_bytes = t->compressed; t->source_crc = t->crc; }
    else if (t->compressed != t->source_bytes || t->crc != t->source_crc) { t->error = TERRA_TWLD_CHANGED; return t->error; }
    int32_t status = tw_output(t, NULL, 0, 1);
    if (status < 0) { t->error = status; return status; }
    t->complete = 1;
    if (t->pass == 1) t->metadata_ready = 1;
    return TERRA_TWLD_OK;
}

static void tw_close_streams(TerraTwld* t) {
    if (t->inflater_ready) { inflateEnd(&t->inflater); t->inflater_ready = 0; }
    if (t->deflater_ready) { deflateEnd(&t->deflater); t->deflater_ready = 0; }
}

static void tw_reset_replay(TerraTwld* t) {
    t->pass = 2; t->complete = t->stream_end = t->depth = t->saw_map = t->saw_tiles = 0;
    t->state = TW_ROOT; t->root_name = t->need = t->have = 0;
    t->compressed = t->expanded = t->mod_tiles = t->color_pixels = 0;
    t->training_dummies = t->logic_sensors = 0;
    t->remaining = t->tile_index = t->tile_have = t->tile_need = 0;
    t->crc = t->output_enabled = 0; t->error = 0;
}

int32_t terra_twld_abort_replay(TerraTwld* t) {
    if (!t) return TERRA_TWLD_INVALID;
    if (!t->metadata_ready) return TERRA_TWLD_STATE;
    if (t->pass == 1) return TERRA_TWLD_OK;
    tw_close_streams(t);
    tw_free(t, t->deflate_buffer); t->deflate_buffer = NULL;
    tw_reset_replay(t); t->replay_idle = 1;
    return TERRA_TWLD_OK;
}

int32_t terra_twld_rewind(TerraTwld* t, uint32_t emit_output) {
    if (!t || emit_output > 1u) return TERRA_TWLD_INVALID;
    if (!t->metadata_ready || (!t->complete && !t->replay_idle) || t->error ||
        (emit_output && !t->callbacks.output)) return TERRA_TWLD_STATE;
    tw_close_streams(t);
    tw_reset_replay(t); t->replay_idle = 0;
    if (t->allocated > t->maximum) {
        terra_twld_abort_replay(t); return TERRA_TWLD_LIMIT;
    }
    t->output_enabled = emit_output;
    int32_t status = tw_inflate_start(t);
    if (status < 0) { terra_twld_abort_replay(t); return status; }
    if (emit_output) {
        if (!t->deflate_buffer) t->deflate_buffer = (uint8_t*)tw_allocate(t, TW_BUFFER);
        if (!t->deflate_buffer) {
            status = t->error; terra_twld_abort_replay(t); return status;
        }
        memset(&t->deflater, 0, sizeof(t->deflater));
        t->deflater.zalloc = tw_zalloc; t->deflater.zfree = tw_zfree; t->deflater.opaque = t;
        if (deflateInit2(&t->deflater, 6, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            status = t->error ? t->error : TERRA_TWLD_FORMAT;
            terra_twld_abort_replay(t); return status;
        }
        t->deflater_ready = 1;
    }
    return 0;
}

int32_t terra_twld_set_budget(TerraTwld* t, uint32_t max_bytes) {
    if (!t) return TERRA_TWLD_INVALID;
    t->maximum = max_bytes ? max_bytes : UINT32_MAX;
    return t->allocated <= t->maximum ? 0 : TERRA_TWLD_LIMIT;
}

void terra_twld_stats(const TerraTwld* t, TerraTwldStats* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out)); if (!t) return;
    out->compressed_bytes = t->compressed; out->expanded_bytes = t->expanded;
    out->world_cells = t->cells; out->mod_tiles = t->mod_tiles; out->color_pixels = t->color_pixels;
    out->map_entries = t->map_count; out->pass = t->pass; out->complete = t->complete;
    out->allocated_bytes = t->allocated; out->peak_bytes = t->peak;
    out->training_dummies = t->training_dummies; out->logic_sensors = t->logic_sensors;
}

void terra_twld_destroy(TerraTwld* t) {
    if (!t) return;
    if (t->inflater_ready) inflateEnd(&t->inflater);
    if (t->deflater_ready) deflateEnd(&t->deflater);
    tw_free(t, t->stack); tw_free(t, t->map);
    tw_free(t, t->inflate_buffer); tw_free(t, t->deflate_buffer);
    tx_persistent_free(t);
}
