/*
 * terra_plr.c -- Pure C17 Terraria .plr reader, editor, and writer.
 *
 * The on-disk payload is the current Terraria 318/319 player format:
 * AES-128-CBC with the legacy UTF-16LE h3y_gUyZ key and PKCS#7 padding,
 * followed by a fully semantic player model.  The model is held as a small
 * JSON DOM so the C ABI can provide the same arbitrary RFC 6901 edits as
 * TerraR's PlayerDocument without introducing a second language runtime.
 */

#include "terra_plr.h"
#include "terra_types.h"
#include "terra_reader.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void *memset(void *dst, int value, unsigned long n);
extern uint8_t *tx_persistent_alloc(uint32_t size);
extern void tx_persistent_free(void *ptr);
extern void *tx_persistent_realloc(void *ptr, uint32_t size);
extern uint32_t tx_strlen(const char *s);
extern void tx_clear_error(void);
extern void tx_set_error(const char *code, const char *message);

/* -------------------------------------------------------------------------
 * Supported format and resource limits
 * ------------------------------------------------------------------------- */

#define PLR_MIN_VERSION 318
#define PLR_MAX_VERSION 319
#define PLR_PLAYER_FILE_TYPE 3u
#define PLR_ARMOR_SLOTS 20u
#define PLR_DYE_SLOTS 10u
#define PLR_INVENTORY_SLOTS 58u
#define PLR_MISC_SLOTS 5u
#define PLR_BANK_SLOTS 40u
#define PLR_BUFF_SLOTS 44u
#define PLR_HIDE_INFO_SLOTS 13u
#define PLR_DPAD_SLOTS 4u
#define PLR_BUILDER_STATUS_SLOTS 12u
#define PLR_TEMPORARY_SLOTS 4u
#define PLR_LOADOUTS 3u
#define PLR_MAX_SPAWN_POINTS 200u
#define PLR_MAX_SACRIFICES 4096u
#define PLR_MAX_PENDING_REFUNDS 4096u
#define PLR_MAX_DIALOGUES 4096u
#define PLR_MAX_FILE_BYTES (64u * 1024u * 1024u)
#define PLR_MAX_JSON_BYTES (32u * 1024u * 1024u)
#define PLR_MAX_STRING_BYTES (16u * 1024u * 1024u)
#define PLR_MAX_JSON_DEPTH 128u
#define PLR_MAX_JSON_ELEMENTS 100000u
#define PLR_MAX_DOCUMENTS 16u

/* "relogic" in the low 56 bits, as used by Terraria FileMetadata. */
#define PLR_METADATA_MAGIC_LOW_56 UINT64_C(27981915666277746)
#define PLR_DEFAULT_MAGIC_AND_TYPE \
    (PLR_METADATA_MAGIC_LOW_56 | ((uint64_t)PLR_PLAYER_FILE_TYPE << 56u))

static const uint8_t g_plr_key[16] = {
    104u, 0u, 51u, 0u, 121u, 0u, 95u, 0u,
    103u, 0u, 85u, 0u, 121u, 0u, 90u, 0u
};

static int g_plr_oom = 0;

/* All PLR DOM and transient buffers belong to TerraWasm's tracked native
 * allocation domain.  Keep the existing cleanup code readable while making
 * ownership visible to the heap lifecycle APIs. */
#define free(ptr) tx_persistent_free(ptr)

static int plr_memory_equal(
    const void *left, const void *right, uint32_t length) {
    const uint8_t *a = (const uint8_t *)left;
    const uint8_t *b = (const uint8_t *)right;
    if (!a || !b) return length == 0u;
    for (uint32_t i = 0u; i < length; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int plr_cstring_equal(const char *left, const char *right) {
    if (!left || !right) return 0;
    uint32_t i = 0u;
    while (left[i] || right[i]) {
        if (left[i] != right[i]) return 0;
        i++;
    }
    return 1;
}

/* Public text arguments are C strings rather than pointer/length pairs. Keep
 * the scan bounded so malformed ABI pointers cannot turn a JSON/path call
 * into an unbounded linear read. */
static int plr_cstring_length_bounded(
    const char *text, uint32_t maximum, uint32_t *out_length) {
    if (!text || !out_length) return 0;
    for (uint32_t i = 0u;; i++) {
        if (text[i] == 0) {
            *out_length = i;
            return 1;
        }
        if (i == maximum) break;
    }
    return 0;
}

static int plr_cstring_compare(const char *left, const char *right) {
    if (left == right) return 0;
    if (!left) return -1;
    if (!right) return 1;
    uint32_t i = 0u;
    while (left[i] && right[i] && left[i] == right[i]) i++;
    return (unsigned char)left[i] < (unsigned char)right[i] ? -1 :
        ((unsigned char)left[i] > (unsigned char)right[i] ? 1 : 0);
}

static void *plr_malloc(size_t size) {
    if (size == 0u) size = 1u;
    if (size > (size_t)UINT32_MAX) {
        g_plr_oom = 1;
        return NULL;
    }
    void *p = tx_persistent_alloc((uint32_t)size);
    if (!p) g_plr_oom = 1;
    return p;
}

static void *plr_calloc(size_t count, size_t size) {
    if (count != 0u && size > SIZE_MAX / count) {
        g_plr_oom = 1;
        return NULL;
    }
    void *p = plr_malloc(count * size);
    if (p) memset(p, 0, (unsigned long)(count * size));
    return p;
}

static void *plr_realloc(void *old, size_t size) {
    if (size == 0u) size = 1u;
    if (size > (size_t)UINT32_MAX) {
        g_plr_oom = 1;
        return NULL;
    }
    void *p = tx_persistent_realloc(old, (uint32_t)size);
    if (!p) g_plr_oom = 1;
    return p;
}

static char *plr_dup_bytes(const char *bytes, size_t length) {
    if (length > SIZE_MAX - 1u) {
        g_plr_oom = 1;
        return NULL;
    }
    char *out = (char *)plr_malloc(length + 1u);
    if (!out) return NULL;
    if (length) memcpy(out, bytes, (unsigned long)length);
    out[length] = 0;
    return out;
}

/* -------------------------------------------------------------------------
 * Small JSON DOM
 * ------------------------------------------------------------------------- */

enum {
    PLR_JSON_NULL = 0,
    PLR_JSON_BOOL = 1,
    PLR_JSON_NUMBER = 2,
    PLR_JSON_STRING = 3,
    PLR_JSON_ARRAY = 4,
    PLR_JSON_OBJECT = 5
};

enum {
    PLR_NUMBER_I64 = 0,
    PLR_NUMBER_U64 = 1,
    PLR_NUMBER_FLOAT = 2
};

typedef struct PlrJsonValue PlrJsonValue;

typedef struct PlrJsonNumber {
    uint8_t kind;
    int64_t i64;
    uint64_t u64;
    double floating;
} PlrJsonNumber;

typedef struct PlrJsonMember {
    char *key;
    PlrJsonValue *value;
} PlrJsonMember;

struct PlrJsonValue {
    uint8_t type;
    union {
        int boolean;
        PlrJsonNumber number;
        char *string;
        struct {
            PlrJsonValue **items;
            uint32_t count;
            uint32_t capacity;
        } array;
        struct {
            PlrJsonMember *members;
            uint32_t count;
            uint32_t capacity;
        } object;
    } as;
};

static PlrJsonValue *plr_json_new(uint8_t type) {
    PlrJsonValue *value = (PlrJsonValue *)plr_calloc(1u, sizeof(PlrJsonValue));
    if (value) value->type = type;
    return value;
}

static PlrJsonValue *plr_json_null(void) {
    return plr_json_new(PLR_JSON_NULL);
}

static PlrJsonValue *plr_json_bool(int value) {
    PlrJsonValue *out = plr_json_new(PLR_JSON_BOOL);
    if (out) out->as.boolean = value ? 1 : 0;
    return out;
}

static PlrJsonValue *plr_json_i64(int64_t value) {
    PlrJsonValue *out = plr_json_new(PLR_JSON_NUMBER);
    if (out) {
        out->as.number.kind = PLR_NUMBER_I64;
        out->as.number.i64 = value;
    }
    return out;
}

static PlrJsonValue *plr_json_u64(uint64_t value) {
    PlrJsonValue *out = plr_json_new(PLR_JSON_NUMBER);
    if (out) {
        out->as.number.kind = PLR_NUMBER_U64;
        out->as.number.u64 = value;
    }
    return out;
}

static PlrJsonValue *plr_json_float(double value) {
    PlrJsonValue *out = plr_json_new(PLR_JSON_NUMBER);
    if (out) {
        out->as.number.kind = PLR_NUMBER_FLOAT;
        out->as.number.floating = value;
    }
    return out;
}

static PlrJsonValue *plr_json_string_owned(char *value) {
    PlrJsonValue *out = plr_json_new(PLR_JSON_STRING);
    if (!out) {
        free(value);
        return NULL;
    }
    out->as.string = value;
    return out;
}

static PlrJsonValue *plr_json_string(const char *value) {
    if (!value) return NULL;
    return plr_json_string_owned(plr_dup_bytes(value, tx_strlen(value)));
}

static PlrJsonValue *plr_json_array(void) {
    return plr_json_new(PLR_JSON_ARRAY);
}

static PlrJsonValue *plr_json_object(void) {
    return plr_json_new(PLR_JSON_OBJECT);
}

static void plr_json_free(PlrJsonValue *value) {
    if (!value) return;
    if (value->type == PLR_JSON_STRING) {
        free(value->as.string);
    } else if (value->type == PLR_JSON_ARRAY) {
        for (uint32_t i = 0; i < value->as.array.count; i++)
            plr_json_free(value->as.array.items[i]);
        free(value->as.array.items);
    } else if (value->type == PLR_JSON_OBJECT) {
        for (uint32_t i = 0; i < value->as.object.count; i++) {
            free(value->as.object.members[i].key);
            plr_json_free(value->as.object.members[i].value);
        }
        free(value->as.object.members);
    }
    free(value);
}

static int plr_json_array_push(PlrJsonValue *array, PlrJsonValue *item) {
    if (!array || array->type != PLR_JSON_ARRAY || !item) return 0;
    if (array->as.array.count >= PLR_MAX_JSON_ELEMENTS) return 0;
    if (array->as.array.count == array->as.array.capacity) {
        uint32_t next = array->as.array.capacity ? array->as.array.capacity * 2u : 8u;
        if (next < array->as.array.count + 1u ||
            next > PLR_MAX_JSON_ELEMENTS) {
            next = PLR_MAX_JSON_ELEMENTS;
        }
        if (next < array->as.array.count + 1u) return 0;
        PlrJsonValue **items = (PlrJsonValue **)plr_realloc(
            array->as.array.items, (size_t)next * sizeof(PlrJsonValue *));
        if (!items) return 0;
        array->as.array.items = items;
        array->as.array.capacity = next;
    }
    array->as.array.items[array->as.array.count++] = item;
    return 1;
}

static int plr_json_key_exists(const PlrJsonValue *object, const char *key) {
    if (!object || object->type != PLR_JSON_OBJECT || !key) return 0;
    for (uint32_t i = 0; i < object->as.object.count; i++) {
        if (plr_cstring_equal(object->as.object.members[i].key, key)) return 1;
    }
    return 0;
}

static int plr_json_object_put_owned(
    PlrJsonValue *object,
    char *key,
    PlrJsonValue *value) {
    if (!object || object->type != PLR_JSON_OBJECT || !key || !value ||
        plr_json_key_exists(object, key) ||
        object->as.object.count >= PLR_MAX_JSON_ELEMENTS) {
        free(key);
        plr_json_free(value);
        return 0;
    }
    if (object->as.object.count == object->as.object.capacity) {
        uint32_t next = object->as.object.capacity ?
            object->as.object.capacity * 2u : 16u;
        if (next < object->as.object.count + 1u ||
            next > PLR_MAX_JSON_ELEMENTS) next = PLR_MAX_JSON_ELEMENTS;
        if (next < object->as.object.count + 1u) {
            free(key);
            plr_json_free(value);
            return 0;
        }
        PlrJsonMember *members = (PlrJsonMember *)plr_realloc(
            object->as.object.members, (size_t)next * sizeof(PlrJsonMember));
        if (!members) {
            free(key);
            plr_json_free(value);
            return 0;
        }
        object->as.object.members = members;
        object->as.object.capacity = next;
    }
    object->as.object.members[object->as.object.count].key = key;
    object->as.object.members[object->as.object.count].value = value;
    object->as.object.count++;
    return 1;
}

static int plr_json_object_put(
    PlrJsonValue *object,
    const char *key,
    PlrJsonValue *value) {
    char *copy = key ? plr_dup_bytes(key, tx_strlen(key)) : NULL;
    if (!copy) {
        plr_json_free(value);
        return 0;
    }
    return plr_json_object_put_owned(object, copy, value);
}

static PlrJsonValue *plr_json_object_get(
    const PlrJsonValue *object,
    const char *key) {
    if (!object || object->type != PLR_JSON_OBJECT || !key) return NULL;
    for (uint32_t i = 0; i < object->as.object.count; i++) {
        if (plr_cstring_equal(object->as.object.members[i].key, key))
            return object->as.object.members[i].value;
    }
    return NULL;
}

static int plr_json_object_put_i64(
    PlrJsonValue *object, const char *key, int64_t value) {
    return plr_json_object_put(object, key, plr_json_i64(value));
}

static int plr_json_object_put_u64(
    PlrJsonValue *object, const char *key, uint64_t value) {
    return plr_json_object_put(object, key, plr_json_u64(value));
}

static int plr_json_object_put_bool(
    PlrJsonValue *object, const char *key, int value) {
    return plr_json_object_put(object, key, plr_json_bool(value));
}

static int plr_json_object_put_float(
    PlrJsonValue *object, const char *key, double value) {
    return plr_json_object_put(object, key, plr_json_float(value));
}

static int plr_json_object_put_string_owned(
    PlrJsonValue *object, const char *key, char *value) {
    return plr_json_object_put(object, key, plr_json_string_owned(value));
}

static PlrJsonValue *plr_json_clone(const PlrJsonValue *value) {
    if (!value) return NULL;
    switch (value->type) {
        case PLR_JSON_NULL:
            return plr_json_null();
        case PLR_JSON_BOOL:
            return plr_json_bool(value->as.boolean);
        case PLR_JSON_NUMBER:
            if (value->as.number.kind == PLR_NUMBER_I64)
                return plr_json_i64(value->as.number.i64);
            if (value->as.number.kind == PLR_NUMBER_U64)
                return plr_json_u64(value->as.number.u64);
            return plr_json_float(value->as.number.floating);
        case PLR_JSON_STRING:
            return plr_json_string(value->as.string);
        case PLR_JSON_ARRAY: {
            PlrJsonValue *out = plr_json_array();
            if (!out) return NULL;
            for (uint32_t i = 0; i < value->as.array.count; i++) {
                PlrJsonValue *item = plr_json_clone(value->as.array.items[i]);
                if (!item || !plr_json_array_push(out, item)) {
                    plr_json_free(item);
                    plr_json_free(out);
                    return NULL;
                }
            }
            return out;
        }
        case PLR_JSON_OBJECT: {
            PlrJsonValue *out = plr_json_object();
            if (!out) return NULL;
            for (uint32_t i = 0; i < value->as.object.count; i++) {
                PlrJsonValue *item = plr_json_clone(
                    value->as.object.members[i].value);
                if (!item) {
                    plr_json_free(out);
                    return NULL;
                }
                /* plr_json_object_put owns and frees item on insertion failure. */
                if (!plr_json_object_put(
                    out, value->as.object.members[i].key, item)) {
                    plr_json_free(out);
                    return NULL;
                }
            }
            return out;
        }
        default:
            return NULL;
    }
}

/* -------------------------------------------------------------------------
 * UTF-8 and JSON parser
 * ------------------------------------------------------------------------- */

typedef struct PlrByteBuffer {
    uint8_t *data;
    uint32_t length;
    uint32_t capacity;
} PlrByteBuffer;

static int plr_byte_reserve(PlrByteBuffer *buffer, uint32_t extra) {
    if (!buffer || extra > UINT32_MAX - buffer->length) return 0;
    uint32_t need = buffer->length + extra;
    if (need <= buffer->capacity) return 1;
    uint32_t next = buffer->capacity ? buffer->capacity : 64u;
    while (next < need) {
        uint32_t old = next;
        next = next < 1048576u ? next * 2u : next + 1048576u;
        if (next < old || next > PLR_MAX_STRING_BYTES + 1u) {
            next = need;
            break;
        }
    }
    if (next > PLR_MAX_STRING_BYTES + 1u) return 0;
    uint8_t *data = (uint8_t *)plr_realloc(buffer->data, next);
    if (!data) return 0;
    buffer->data = data;
    buffer->capacity = next;
    return 1;
}

static int plr_byte_push(PlrByteBuffer *buffer, uint8_t byte) {
    if (!plr_byte_reserve(buffer, 1u)) return 0;
    buffer->data[buffer->length++] = byte;
    return 1;
}

static int plr_utf8_sequence(
    const uint8_t *data,
    uint32_t length,
    uint32_t offset,
    uint32_t *out_size,
    uint32_t *out_codepoint) {
    if (!data || offset >= length || !out_size || !out_codepoint) return 0;
    uint8_t first = data[offset];
    uint32_t size = 0u;
    uint32_t value = 0u;
    uint32_t minimum = 0u;
    if (first < 0x80u) {
        *out_size = 1u;
        *out_codepoint = first;
        return 1;
    } else if (first >= 0xc2u && first <= 0xdfu) {
        size = 2u;
        value = first & 0x1fu;
        minimum = 0x80u;
    } else if (first >= 0xe0u && first <= 0xefu) {
        size = 3u;
        value = first & 0x0fu;
        minimum = 0x800u;
    } else if (first >= 0xf0u && first <= 0xf4u) {
        size = 4u;
        value = first & 0x07u;
        minimum = 0x10000u;
    } else {
        return 0;
    }
    if (!terra_reader_has(offset, size, length)) return 0;
    for (uint32_t i = 1u; i < size; i++) {
        uint8_t byte = data[offset + i];
        if ((byte & 0xc0u) != 0x80u) return 0;
        value = (value << 6u) | (byte & 0x3fu);
    }
    /* The C ABI stores strings as NUL-terminated UTF-8.  Reject an embedded
     * U+0000 rather than silently truncating a valid JSON/PLR string. */
    if (value == 0u || value < minimum || value > 0x10ffffu ||
        (value >= 0xd800u && value <= 0xdfffu)) return 0;
    if (size == 3u) {
        if (first == 0xe0u && data[offset + 1u] < 0xa0u) return 0;
        if (first == 0xedu && data[offset + 1u] >= 0xa0u) return 0;
    } else if (size == 4u) {
        if (first == 0xf0u && data[offset + 1u] < 0x90u) return 0;
        if (first == 0xf4u && data[offset + 1u] > 0x8fu) return 0;
    }
    *out_size = size;
    *out_codepoint = value;
    return 1;
}

static int plr_utf8_valid(const uint8_t *data, uint32_t length) {
    uint32_t offset = 0u;
    while (offset < length) {
        uint32_t size = 0u;
        uint32_t codepoint = 0u;
        if (!plr_utf8_sequence(data, length, offset, &size, &codepoint))
            return 0;
        (void)codepoint;
        offset += size;
    }
    return 1;
}

static int plr_byte_codepoint(PlrByteBuffer *buffer, uint32_t codepoint) {
    if (codepoint <= 0x7fu) return plr_byte_push(buffer, (uint8_t)codepoint);
    if (codepoint <= 0x7ffu) {
        return plr_byte_push(buffer, (uint8_t)(0xc0u | (codepoint >> 6u))) &&
               plr_byte_push(buffer, (uint8_t)(0x80u | (codepoint & 0x3fu)));
    }
    if (codepoint <= 0xffffu) {
        return plr_byte_push(buffer, (uint8_t)(0xe0u | (codepoint >> 12u))) &&
               plr_byte_push(buffer, (uint8_t)(0x80u | ((codepoint >> 6u) & 0x3fu))) &&
               plr_byte_push(buffer, (uint8_t)(0x80u | (codepoint & 0x3fu)));
    }
    if (codepoint <= 0x10ffffu) {
        return plr_byte_push(buffer, (uint8_t)(0xf0u | (codepoint >> 18u))) &&
               plr_byte_push(buffer, (uint8_t)(0x80u | ((codepoint >> 12u) & 0x3fu))) &&
               plr_byte_push(buffer, (uint8_t)(0x80u | ((codepoint >> 6u) & 0x3fu))) &&
               plr_byte_push(buffer, (uint8_t)(0x80u | (codepoint & 0x3fu)));
    }
    return 0;
}

typedef struct PlrJsonParser {
    const char *text;
    uint32_t length;
    uint32_t position;
    uint32_t elements;
    int failed;
} PlrJsonParser;

static void plr_json_skip_space(PlrJsonParser *parser) {
    while (parser->position < parser->length) {
        char c = parser->text[parser->position];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        parser->position++;
    }
}

static int plr_json_hex(char c, uint32_t *value) {
    if (c >= '0' && c <= '9') *value = (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') *value = (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') *value = (uint32_t)(c - 'A' + 10);
    else return 0;
    return 1;
}

static int plr_json_hex4(PlrJsonParser *parser, uint32_t *value) {
    if (!parser || !value || parser->position > parser->length ||
        parser->length - parser->position < 4u) return 0;
    uint32_t result = 0u;
    for (uint32_t i = 0u; i < 4u; i++) {
        uint32_t digit = 0u;
        if (!plr_json_hex(parser->text[parser->position + i], &digit)) return 0;
        result = (result << 4u) | digit;
    }
    parser->position += 4u;
    *value = result;
    return 1;
}

static char *plr_json_parse_string_text(PlrJsonParser *parser) {
    if (!parser || parser->position >= parser->length ||
        parser->text[parser->position] != '"') return NULL;
    parser->position++;
    PlrByteBuffer output = {0};
    while (parser->position < parser->length) {
        uint8_t c = (uint8_t)parser->text[parser->position++];
        if (c == '"') {
            if (!plr_byte_reserve(&output, 1u)) {
                free(output.data);
                return NULL;
            }
            output.data[output.length] = 0u;
            char *result = (char *)output.data;
            return result;
        }
        if (c < 0x20u) {
            free(output.data);
            return NULL;
        }
        if (c != '\\') {
            uint32_t start = parser->position - 1u;
            uint32_t size = 0u;
            uint32_t codepoint = 0u;
            if (!plr_utf8_sequence(
                (const uint8_t *)parser->text, parser->length, start,
                &size, &codepoint)) {
                free(output.data);
                return NULL;
            }
            if (size > 1u) parser->position = start + size;
            if (!plr_byte_reserve(&output, size)) {
                free(output.data);
                return NULL;
            }
            for (uint32_t i = 0u; i < size; i++)
                output.data[output.length++] = (uint8_t)parser->text[start + i];
            (void)codepoint;
            continue;
        }
        if (parser->position >= parser->length) {
            free(output.data);
            return NULL;
        }
        char escaped = parser->text[parser->position++];
        switch (escaped) {
            case '"': if (!plr_byte_push(&output, '"')) goto string_error; break;
            case '\\': if (!plr_byte_push(&output, '\\')) goto string_error; break;
            case '/': if (!plr_byte_push(&output, '/')) goto string_error; break;
            case 'b': if (!plr_byte_push(&output, '\b')) goto string_error; break;
            case 'f': if (!plr_byte_push(&output, '\f')) goto string_error; break;
            case 'n': if (!plr_byte_push(&output, '\n')) goto string_error; break;
            case 'r': if (!plr_byte_push(&output, '\r')) goto string_error; break;
            case 't': if (!plr_byte_push(&output, '\t')) goto string_error; break;
            case 'u': {
                uint32_t high = 0u;
                if (!plr_json_hex4(parser, &high)) goto string_error;
                uint32_t codepoint = high;
                if (high >= 0xd800u && high <= 0xdbffu) {
                    if (parser->position + 6u > parser->length ||
                        parser->text[parser->position] != '\\' ||
                        parser->text[parser->position + 1u] != 'u')
                        goto string_error;
                    parser->position += 2u;
                    uint32_t low = 0u;
                    if (!plr_json_hex4(parser, &low) ||
                        low < 0xdc00u || low > 0xdfffu) goto string_error;
                    codepoint = 0x10000u + ((high - 0xd800u) << 10u) +
                        (low - 0xdc00u);
                } else if (high >= 0xdc00u && high <= 0xdfffu) {
                    goto string_error;
                }
                if (codepoint == 0u || !plr_byte_codepoint(&output, codepoint)) goto string_error;
                break;
            }
            default:
                goto string_error;
        }
        if (output.length > PLR_MAX_STRING_BYTES) goto string_error;
    }
string_error:
    free(output.data);
    return NULL;
}

static int plr_json_parse_double_token(
    const char *text, uint32_t length, double *out) {
    if (!text || !out || length == 0u) return 0;
    uint32_t position = 0u;
    int negative = 0;
    if (text[position] == '-') {
        negative = 1;
        position++;
    }
    if (position >= length || text[position] < '0' || text[position] > '9') return 0;
    double value = 0.0;
    while (position < length && text[position] >= '0' && text[position] <= '9') {
        value = value * 10.0 + (double)(text[position] - '0');
        position++;
    }
    if (position < length && text[position] == '.') {
        position++;
        if (position >= length || text[position] < '0' || text[position] > '9') return 0;
        double scale = 0.1;
        while (position < length && text[position] >= '0' && text[position] <= '9') {
            value += (double)(text[position] - '0') * scale;
            scale *= 0.1;
            position++;
        }
    }
    int exponent_negative = 0;
    int exponent = 0;
    if (position < length && (text[position] == 'e' || text[position] == 'E')) {
        position++;
        if (position < length && (text[position] == '+' || text[position] == '-')) {
            exponent_negative = text[position] == '-';
            position++;
        }
        if (position >= length || text[position] < '0' || text[position] > '9') return 0;
        while (position < length && text[position] >= '0' && text[position] <= '9') {
            if (exponent > 308) return 0;
            exponent = exponent * 10 + (text[position] - '0');
            position++;
        }
    }
    if (position != length || !isfinite(value)) return 0;
    double scale = 1.0;
    for (int i = 0; i < exponent; i++) scale *= 10.0;
    value = exponent_negative ? value / scale : value * scale;
    if (!isfinite(value)) return 0;
    *out = negative ? -value : value;
    return 1;
}

static PlrJsonValue *plr_json_parse_value(PlrJsonParser *parser, uint32_t depth);

static PlrJsonValue *plr_json_parse_number(PlrJsonParser *parser) {
    uint32_t start = parser->position;
    if (parser->position < parser->length && parser->text[parser->position] == '-')
        parser->position++;
    if (parser->position >= parser->length) return NULL;
    if (parser->text[parser->position] == '0') {
        parser->position++;
        if (parser->position < parser->length &&
            parser->text[parser->position] >= '0' &&
            parser->text[parser->position] <= '9') return NULL;
    } else {
        if (parser->text[parser->position] < '1' ||
            parser->text[parser->position] > '9') return NULL;
        while (parser->position < parser->length &&
               parser->text[parser->position] >= '0' &&
               parser->text[parser->position] <= '9') parser->position++;
    }
    int floating = 0;
    if (parser->position < parser->length && parser->text[parser->position] == '.') {
        floating = 1;
        parser->position++;
        if (parser->position >= parser->length ||
            parser->text[parser->position] < '0' ||
            parser->text[parser->position] > '9') return NULL;
        while (parser->position < parser->length &&
               parser->text[parser->position] >= '0' &&
               parser->text[parser->position] <= '9') parser->position++;
    }
    if (parser->position < parser->length &&
        (parser->text[parser->position] == 'e' ||
         parser->text[parser->position] == 'E')) {
        floating = 1;
        parser->position++;
        if (parser->position < parser->length &&
            (parser->text[parser->position] == '+' ||
             parser->text[parser->position] == '-')) parser->position++;
        if (parser->position >= parser->length ||
            parser->text[parser->position] < '0' ||
            parser->text[parser->position] > '9') return NULL;
        while (parser->position < parser->length &&
               parser->text[parser->position] >= '0' &&
               parser->text[parser->position] <= '9') parser->position++;
    }
    uint32_t length = parser->position - start;
    if (floating) {
        double value = 0.0;
        if (!plr_json_parse_double_token(parser->text + start, length, &value)) return NULL;
        return plr_json_float(value);
    }

    uint32_t position = start;
    int negative = 0;
    if (parser->text[position] == '-') {
        negative = 1;
        position++;
    }
    uint64_t magnitude = 0u;
    while (position < parser->position) {
        if (parser->text[position] < '0' || parser->text[position] > '9') return NULL;
        uint32_t digit = (uint32_t)(parser->text[position] - '0');
        if (magnitude > (UINT64_MAX - digit) / 10u) return NULL;
        magnitude = magnitude * 10u + digit;
        position++;
    }
    if (negative) {
        if (magnitude > UINT64_C(9223372036854775808)) return NULL;
        if (magnitude == UINT64_C(9223372036854775808))
            return plr_json_i64(INT64_MIN);
        return plr_json_i64(-(int64_t)magnitude);
    }
    if (magnitude <= UINT64_C(9223372036854775807))
        return plr_json_i64((int64_t)magnitude);
    return plr_json_u64(magnitude);
}

static PlrJsonValue *plr_json_parse_array(PlrJsonParser *parser, uint32_t depth) {
    if (parser->text[parser->position] != '[') return NULL;
    parser->position++;
    PlrJsonValue *array = plr_json_array();
    if (!array) return NULL;
    plr_json_skip_space(parser);
    if (parser->position < parser->length && parser->text[parser->position] == ']') {
        parser->position++;
        return array;
    }
    for (;;) {
        plr_json_skip_space(parser);
        PlrJsonValue *item = plr_json_parse_value(parser, depth + 1u);
        if (!item || !plr_json_array_push(array, item)) {
            plr_json_free(item);
            plr_json_free(array);
            return NULL;
        }
        parser->elements++;
        plr_json_skip_space(parser);
        if (parser->position >= parser->length) break;
        if (parser->text[parser->position] == ']') {
            parser->position++;
            return array;
        }
        if (parser->text[parser->position] != ',') break;
        parser->position++;
    }
    plr_json_free(array);
    return NULL;
}

static PlrJsonValue *plr_json_parse_object(PlrJsonParser *parser, uint32_t depth) {
    if (parser->text[parser->position] != '{') return NULL;
    parser->position++;
    PlrJsonValue *object = plr_json_object();
    if (!object) return NULL;
    plr_json_skip_space(parser);
    if (parser->position < parser->length && parser->text[parser->position] == '}') {
        parser->position++;
        return object;
    }
    for (;;) {
        plr_json_skip_space(parser);
        char *key = plr_json_parse_string_text(parser);
        if (!key) break;
        plr_json_skip_space(parser);
        if (parser->position >= parser->length || parser->text[parser->position] != ':') {
            free(key);
            break;
        }
        parser->position++;
        plr_json_skip_space(parser);
        PlrJsonValue *value = plr_json_parse_value(parser, depth + 1u);
        if (!value) {
            free(key);
            break;
        }
        /* plr_json_object_put_owned frees key and value on failure. */
        if (!plr_json_object_put_owned(object, key, value)) break;
        parser->elements++;
        plr_json_skip_space(parser);
        if (parser->position >= parser->length) break;
        if (parser->text[parser->position] == '}') {
            parser->position++;
            return object;
        }
        if (parser->text[parser->position] != ',') break;
        parser->position++;
    }
    plr_json_free(object);
    return NULL;
}

static PlrJsonValue *plr_json_parse_value(PlrJsonParser *parser, uint32_t depth) {
    if (!parser || depth > PLR_MAX_JSON_DEPTH) return NULL;
    plr_json_skip_space(parser);
    if (parser->position >= parser->length) return NULL;
    char c = parser->text[parser->position];
    if (c == '"') {
        char *string = plr_json_parse_string_text(parser);
        return string ? plr_json_string_owned(string) : NULL;
    }
    if (c == '{') return plr_json_parse_object(parser, depth);
    if (c == '[') return plr_json_parse_array(parser, depth);
    if (c == 't' && parser->position + 4u <= parser->length &&
        plr_memory_equal(parser->text + parser->position, "true", 4u)) {
        parser->position += 4u;
        return plr_json_bool(1);
    }
    if (c == 'f' && parser->position + 5u <= parser->length &&
        plr_memory_equal(parser->text + parser->position, "false", 5u)) {
        parser->position += 5u;
        return plr_json_bool(0);
    }
    if (c == 'n' && parser->position + 4u <= parser->length &&
        plr_memory_equal(parser->text + parser->position, "null", 4u)) {
        parser->position += 4u;
        return plr_json_null();
    }
    if (c == '-' || (c >= '0' && c <= '9')) return plr_json_parse_number(parser);
    return NULL;
}

static PlrJsonValue *plr_json_parse_text(const char *text) {
    uint32_t length = 0u;
    if (!plr_cstring_length_bounded(text, PLR_MAX_JSON_BYTES, &length) ||
        length == 0u) return NULL;
    PlrJsonParser parser = {text, length, 0u, 0u, 0};
    PlrJsonValue *value = plr_json_parse_value(&parser, 0u);
    if (!value) return NULL;
    plr_json_skip_space(&parser);
    if (parser.position != parser.length || parser.elements > PLR_MAX_JSON_ELEMENTS) {
        plr_json_free(value);
        return NULL;
    }
    return value;
}

/* -------------------------------------------------------------------------
 * JSON serializer
 * ------------------------------------------------------------------------- */

typedef struct PlrOutput {
    uint8_t *data;
    uint32_t length;
    uint32_t capacity;
    int ok;
} PlrOutput;

static int plr_output_reserve(PlrOutput *output, uint32_t extra) {
    if (!output || !output->ok || extra > UINT32_MAX - output->length) {
        if (output) output->ok = 0;
        return 0;
    }
    uint32_t need = output->length + extra;
    if (need <= output->capacity) return 1;
    uint32_t next = output->capacity ? output->capacity : 4096u;
    while (next < need) {
        uint32_t old = next;
        next = next < 1048576u ? next * 2u : next + 1048576u;
        if (next < old) { next = need; break; }
    }
    uint8_t *data = (uint8_t *)plr_realloc(output->data, next);
    if (!data) {
        output->ok = 0;
        return 0;
    }
    output->data = data;
    output->capacity = next;
    return 1;
}

static void plr_output_byte(PlrOutput *output, uint8_t byte) {
    if (plr_output_reserve(output, 1u)) output->data[output->length++] = byte;
}

static void plr_output_bytes(PlrOutput *output, const void *data, uint32_t length) {
    if (!plr_output_reserve(output, length)) return;
    if (length) memcpy(output->data + output->length, data, length);
    output->length += length;
}

static void plr_output_ascii(PlrOutput *output, const char *text) {
    if (text) plr_output_bytes(output, text, tx_strlen(text));
}

static void plr_output_u64(PlrOutput *output, uint64_t value) {
    char digits[32];
    uint32_t count = 0u;
    if (value == 0u) {
        plr_output_byte(output, '0');
        return;
    }
    while (value && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (count) plr_output_byte(output, (uint8_t)digits[--count]);
}

static void plr_output_i64(PlrOutput *output, int64_t value) {
    if (value < 0) {
        plr_output_byte(output, '-');
        plr_output_u64(output, (uint64_t)(-(value + 1)) + 1u);
    } else {
        plr_output_u64(output, (uint64_t)value);
    }
}

static void plr_output_json_string(PlrOutput *output, const char *text) {
    plr_output_byte(output, '"');
    if (text) {
        uint32_t length = tx_strlen(text);
        for (uint32_t i = 0u; i < length; i++) {
            uint8_t c = (uint8_t)text[i];
            switch (c) {
                case '"': plr_output_ascii(output, "\\\""); break;
                case '\\': plr_output_ascii(output, "\\\\"); break;
                case '\b': plr_output_ascii(output, "\\b"); break;
                case '\f': plr_output_ascii(output, "\\f"); break;
                case '\n': plr_output_ascii(output, "\\n"); break;
                case '\r': plr_output_ascii(output, "\\r"); break;
                case '\t': plr_output_ascii(output, "\\t"); break;
                default:
                    if (c < 0x20u) {
                        static const char hex[] = "0123456789abcdef";
                        plr_output_ascii(output, "\\u00");
                        plr_output_byte(output, (uint8_t)hex[c >> 4u]);
                        plr_output_byte(output, (uint8_t)hex[c & 0x0fu]);
                    } else {
                        plr_output_byte(output, c);
                    }
                    break;
            }
        }
    }
    plr_output_byte(output, '"');
}

static void plr_output_number(PlrOutput *output, const PlrJsonNumber *number) {
    if (!number) { output->ok = 0; return; }
    if (number->kind == PLR_NUMBER_I64) {
        plr_output_i64(output, number->i64);
    } else if (number->kind == PLR_NUMBER_U64) {
        plr_output_u64(output, number->u64);
    } else {
        if (!isfinite(number->floating)) { output->ok = 0; return; }
        char text[64];
        int length = snprintf(text, sizeof(text), "%.9g", number->floating);
        if (length <= 0 || length >= (int)sizeof(text)) {
            output->ok = 0;
            return;
        }
        plr_output_bytes(output, text, (uint32_t)length);
    }
}

static void plr_json_write_value(
    PlrOutput *output, const PlrJsonValue *value, uint32_t depth) {
    if (!output || !output->ok || !value || depth > PLR_MAX_JSON_DEPTH) {
        if (output) output->ok = 0;
        return;
    }
    switch (value->type) {
        case PLR_JSON_NULL:
            plr_output_ascii(output, "null");
            break;
        case PLR_JSON_BOOL:
            plr_output_ascii(output, value->as.boolean ? "true" : "false");
            break;
        case PLR_JSON_NUMBER:
            plr_output_number(output, &value->as.number);
            break;
        case PLR_JSON_STRING:
            if (!plr_utf8_valid((const uint8_t *)value->as.string, tx_strlen(value->as.string))) {
                output->ok = 0;
                return;
            }
            plr_output_json_string(output, value->as.string);
            break;
        case PLR_JSON_ARRAY:
            plr_output_byte(output, '[');
            for (uint32_t i = 0u; i < value->as.array.count; i++) {
                if (i) plr_output_byte(output, ',');
                plr_json_write_value(output, value->as.array.items[i], depth + 1u);
            }
            plr_output_byte(output, ']');
            break;
        case PLR_JSON_OBJECT: {
            uint32_t count = value->as.object.count;
            uint32_t *order = NULL;
            if (count > 1u) {
                order = (uint32_t *)plr_malloc((size_t)count * sizeof(uint32_t));
                if (!order) {
                    output->ok = 0;
                    return;
                }
                for (uint32_t i = 0u; i < count; i++) order[i] = i;
                for (uint32_t i = 1u; i < count; i++) {
                    uint32_t selected = order[i];
                    uint32_t j = i;
                    while (j > 0u && plr_cstring_compare(
                        value->as.object.members[order[j - 1u]].key,
                        value->as.object.members[selected].key) > 0) {
                        order[j] = order[j - 1u];
                        j--;
                    }
                    order[j] = selected;
                }
            }
            plr_output_byte(output, '{');
            for (uint32_t i = 0u; i < count; i++) {
                uint32_t member_index = order ? order[i] : i;
                if (i) plr_output_byte(output, ',');
                plr_output_json_string(output, value->as.object.members[member_index].key);
                plr_output_byte(output, ':');
                plr_json_write_value(output, value->as.object.members[member_index].value, depth + 1u);
            }
            plr_output_byte(output, '}');
            free(order);
            break;
        }
        default:
            output->ok = 0;
            break;
    }
}

static uint8_t *plr_json_serialize(
    const PlrJsonValue *value, uint32_t *out_length) {
    if (out_length) *out_length = 0u;
    PlrOutput output = {NULL, 0u, 0u, 1};
    plr_json_write_value(&output, value, 0u);
    if (!output.ok || !plr_output_reserve(&output, 1u)) {
        free(output.data);
        return NULL;
    }
    output.data[output.length] = 0u;
    if (out_length) *out_length = output.length;
    return output.data;
}

/* -------------------------------------------------------------------------
 * Bounded binary reader and writer
 * ------------------------------------------------------------------------- */

typedef struct PlrReader {
    const uint8_t *data;
    uint32_t length;
    uint32_t offset;
    int ok;
} PlrReader;

static int plr_reader_has(const PlrReader *reader, uint32_t amount) {
    return reader && reader->ok &&
        terra_reader_has(reader->offset, amount, reader->length);
}

static uint8_t plr_read_u8(PlrReader *reader) {
    if (!plr_reader_has(reader, 1u)) {
        if (reader) reader->ok = 0;
        return 0u;
    }
    return reader->data[reader->offset++];
}

static uint16_t plr_read_u16(PlrReader *reader) {
    if (!plr_reader_has(reader, 2u)) {
        if (reader) reader->ok = 0;
        return 0u;
    }
    uint32_t offset = reader->offset;
    reader->offset += 2u;
    return (uint16_t)(reader->data[offset] | ((uint16_t)reader->data[offset + 1u] << 8u));
}

static uint32_t plr_read_u32(PlrReader *reader) {
    if (!plr_reader_has(reader, 4u)) {
        if (reader) reader->ok = 0;
        return 0u;
    }
    uint32_t offset = reader->offset;
    reader->offset += 4u;
    return (uint32_t)reader->data[offset] |
           ((uint32_t)reader->data[offset + 1u] << 8u) |
           ((uint32_t)reader->data[offset + 2u] << 16u) |
           ((uint32_t)reader->data[offset + 3u] << 24u);
}

static uint64_t plr_read_u64(PlrReader *reader) {
    if (!plr_reader_has(reader, 8u)) {
        if (reader) reader->ok = 0;
        return 0u;
    }
    uint64_t value = 0u;
    uint32_t offset = reader->offset;
    reader->offset += 8u;
    for (uint32_t i = 0u; i < 8u; i++)
        value |= (uint64_t)reader->data[offset + i] << (8u * i);
    return value;
}

static int32_t plr_read_i32(PlrReader *reader) {
    return (int32_t)plr_read_u32(reader);
}

static int64_t plr_read_i64(PlrReader *reader) {
    return (int64_t)plr_read_u64(reader);
}

static float plr_read_f32(PlrReader *reader) {
    union { uint32_t bits; float value; } decoded;
    decoded.bits = plr_read_u32(reader);
    return decoded.value;
}

static uint32_t plr_read_7bit(PlrReader *reader) {
    uint32_t result = 0u;
    for (uint32_t i = 0u; i < 5u; i++) {
        uint8_t byte = plr_read_u8(reader);
        if (!reader->ok) return 0u;
        if (i == 4u && (byte & 0x7fu) > 0x0fu) {
            reader->ok = 0;
            return 0u;
        }
        result |= (uint32_t)(byte & 0x7fu) << (7u * i);
        if ((byte & 0x80u) == 0u) return result;
    }
    reader->ok = 0;
    return 0u;
}

static char *plr_read_string(PlrReader *reader) {
    uint32_t length = plr_read_7bit(reader);
    if (!reader->ok || length > PLR_MAX_STRING_BYTES ||
        !plr_reader_has(reader, length)) {
        reader->ok = 0;
        return NULL;
    }
    const uint8_t *data = reader->data + reader->offset;
    if (!plr_utf8_valid(data, length)) {
        reader->ok = 0;
        return NULL;
    }
    char *string = plr_dup_bytes((const char *)data, length);
    if (!string) {
        reader->ok = 0;
        return NULL;
    }
    reader->offset += length;
    return string;
}

typedef struct PlrWriter {
    uint8_t *data;
    uint32_t length;
    uint32_t capacity;
    int ok;
} PlrWriter;

static int plr_writer_reserve(PlrWriter *writer, uint32_t extra) {
    if (!writer || !writer->ok || extra > UINT32_MAX - writer->length) {
        if (writer) writer->ok = 0;
        return 0;
    }
    uint32_t need = writer->length + extra;
    if (need <= writer->capacity) return 1;
    uint32_t next = writer->capacity ? writer->capacity : 4096u;
    while (next < need) {
        uint32_t old = next;
        next = next < 1048576u ? next * 2u : next + 1048576u;
        if (next < old) { next = need; break; }
    }
    uint8_t *data = (uint8_t *)plr_realloc(writer->data, next);
    if (!data) {
        writer->ok = 0;
        return 0;
    }
    writer->data = data;
    writer->capacity = next;
    return 1;
}

static void plr_writer_bytes(PlrWriter *writer, const void *data, uint32_t length) {
    if (!plr_writer_reserve(writer, length)) return;
    if (length) memcpy(writer->data + writer->length, data, length);
    writer->length += length;
}

static void plr_writer_u8(PlrWriter *writer, uint8_t value) {
    plr_writer_bytes(writer, &value, 1u);
}

static void plr_writer_u16(PlrWriter *writer, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8u)};
    plr_writer_bytes(writer, bytes, 2u);
}

static void plr_writer_u32(PlrWriter *writer, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t)value, (uint8_t)(value >> 8u),
        (uint8_t)(value >> 16u), (uint8_t)(value >> 24u)
    };
    plr_writer_bytes(writer, bytes, 4u);
}

static void plr_writer_u64(PlrWriter *writer, uint64_t value) {
    uint8_t bytes[8];
    for (uint32_t i = 0u; i < 8u; i++) bytes[i] = (uint8_t)(value >> (8u * i));
    plr_writer_bytes(writer, bytes, 8u);
}

static void plr_writer_i32(PlrWriter *writer, int32_t value) {
    plr_writer_u32(writer, (uint32_t)value);
}

static void plr_writer_i64(PlrWriter *writer, int64_t value) {
    plr_writer_u64(writer, (uint64_t)value);
}

static void plr_writer_f32(PlrWriter *writer, float value) {
    union { uint32_t bits; float value; } encoded;
    encoded.value = value;
    plr_writer_u32(writer, encoded.bits);
}

static void plr_writer_7bit(PlrWriter *writer, uint32_t value) {
    while (value >= 0x80u) {
        plr_writer_u8(writer, (uint8_t)(value | 0x80u));
        value >>= 7u;
    }
    plr_writer_u8(writer, (uint8_t)value);
}

static void plr_writer_string(PlrWriter *writer, const char *string) {
    uint32_t length = string ? tx_strlen(string) : 0u;
    if (length > PLR_MAX_STRING_BYTES || !plr_utf8_valid((const uint8_t *)(string ? string : ""), length)) {
        writer->ok = 0;
        return;
    }
    plr_writer_7bit(writer, length);
    plr_writer_bytes(writer, string, length);
}

/* -------------------------------------------------------------------------
 * AES-128 implementation
 * ------------------------------------------------------------------------- */

static const uint8_t g_aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t g_aes_inv_sbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

static const uint8_t g_aes_rcon[11] = {
    0u, 1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u, 27u, 54u
};

static uint8_t plr_aes_mul(uint8_t left, uint8_t right) {
    uint8_t result = 0u;
    while (right) {
        if (right & 1u) result ^= left;
        left = (uint8_t)((left << 1u) ^ ((left & 0x80u) ? 0x1bu : 0u));
        right >>= 1u;
    }
    return result;
}

static void plr_aes_expand_key(const uint8_t key[16], uint8_t expanded[176]) {
    for (uint32_t i = 0u; i < 16u; i++) expanded[i] = key[i];
    uint32_t bytes = 16u;
    uint32_t round = 1u;
    uint8_t temp[4];
    while (bytes < 176u) {
        for (uint32_t i = 0u; i < 4u; i++) temp[i] = expanded[bytes - 4u + i];
        if ((bytes & 15u) == 0u) {
            uint8_t first = temp[0];
            temp[0] = g_aes_sbox[temp[1]] ^ g_aes_rcon[round];
            temp[1] = g_aes_sbox[temp[2]];
            temp[2] = g_aes_sbox[temp[3]];
            temp[3] = g_aes_sbox[first];
            round++;
        }
        for (uint32_t i = 0u; i < 4u; i++) {
            expanded[bytes] = expanded[bytes - 16u] ^ temp[i];
            bytes++;
        }
    }
}

static void plr_aes_add_round_key(uint8_t state[16], const uint8_t *round_key) {
    for (uint32_t i = 0u; i < 16u; i++) state[i] ^= round_key[i];
}

static void plr_aes_sub_bytes(uint8_t state[16]) {
    for (uint32_t i = 0u; i < 16u; i++) state[i] = g_aes_sbox[state[i]];
}

static void plr_aes_inv_sub_bytes(uint8_t state[16]) {
    for (uint32_t i = 0u; i < 16u; i++) state[i] = g_aes_inv_sbox[state[i]];
}

static void plr_aes_shift_rows(uint8_t state[16]) {
    uint8_t copy[16];
    memcpy(copy, state, 16u);
    for (uint32_t row = 0u; row < 4u; row++) {
        for (uint32_t column = 0u; column < 4u; column++)
            state[row + 4u * column] = copy[row + 4u * ((column + row) & 3u)];
    }
}

static void plr_aes_inv_shift_rows(uint8_t state[16]) {
    uint8_t copy[16];
    memcpy(copy, state, 16u);
    for (uint32_t row = 0u; row < 4u; row++) {
        for (uint32_t column = 0u; column < 4u; column++)
            state[row + 4u * column] = copy[row + 4u * ((column + 4u - row) & 3u)];
    }
}

static void plr_aes_mix_columns(uint8_t state[16]) {
    for (uint32_t column = 0u; column < 4u; column++) {
        uint8_t *s = state + column * 4u;
        uint8_t a = s[0], b = s[1], c = s[2], d = s[3];
        s[0] = plr_aes_mul(a, 2u) ^ plr_aes_mul(b, 3u) ^ c ^ d;
        s[1] = a ^ plr_aes_mul(b, 2u) ^ plr_aes_mul(c, 3u) ^ d;
        s[2] = a ^ b ^ plr_aes_mul(c, 2u) ^ plr_aes_mul(d, 3u);
        s[3] = plr_aes_mul(a, 3u) ^ b ^ c ^ plr_aes_mul(d, 2u);
    }
}

static void plr_aes_inv_mix_columns(uint8_t state[16]) {
    for (uint32_t column = 0u; column < 4u; column++) {
        uint8_t *s = state + column * 4u;
        uint8_t a = s[0], b = s[1], c = s[2], d = s[3];
        s[0] = plr_aes_mul(a, 14u) ^ plr_aes_mul(b, 11u) ^
               plr_aes_mul(c, 13u) ^ plr_aes_mul(d, 9u);
        s[1] = plr_aes_mul(a, 9u) ^ plr_aes_mul(b, 14u) ^
               plr_aes_mul(c, 11u) ^ plr_aes_mul(d, 13u);
        s[2] = plr_aes_mul(a, 13u) ^ plr_aes_mul(b, 9u) ^
               plr_aes_mul(c, 14u) ^ plr_aes_mul(d, 11u);
        s[3] = plr_aes_mul(a, 11u) ^ plr_aes_mul(b, 13u) ^
               plr_aes_mul(c, 9u) ^ plr_aes_mul(d, 14u);
    }
}

static void plr_aes_encrypt_block(const uint8_t input[16], uint8_t output[16], const uint8_t key[176]) {
    uint8_t state[16];
    memcpy(state, input, 16u);
    plr_aes_add_round_key(state, key);
    for (uint32_t round = 1u; round < 10u; round++) {
        plr_aes_sub_bytes(state);
        plr_aes_shift_rows(state);
        plr_aes_mix_columns(state);
        plr_aes_add_round_key(state, key + 16u * round);
    }
    plr_aes_sub_bytes(state);
    plr_aes_shift_rows(state);
    plr_aes_add_round_key(state, key + 160u);
    memcpy(output, state, 16u);
}

static void plr_aes_decrypt_block(const uint8_t input[16], uint8_t output[16], const uint8_t key[176]) {
    uint8_t state[16];
    memcpy(state, input, 16u);
    plr_aes_add_round_key(state, key + 160u);
    for (uint32_t round = 9u; round > 0u; round--) {
        plr_aes_inv_shift_rows(state);
        plr_aes_inv_sub_bytes(state);
        plr_aes_add_round_key(state, key + 16u * round);
        plr_aes_inv_mix_columns(state);
    }
    plr_aes_inv_shift_rows(state);
    plr_aes_inv_sub_bytes(state);
    plr_aes_add_round_key(state, key);
    memcpy(output, state, 16u);
}

static uint8_t *plr_decrypt(
    const uint8_t *encrypted, uint32_t encrypted_length, uint32_t *out_length) {
    if (out_length) *out_length = 0u;
    if (!encrypted || encrypted_length < 16u ||
        (encrypted_length & 15u) != 0u || encrypted_length > PLR_MAX_FILE_BYTES)
        return NULL;
    uint8_t expanded[176];
    plr_aes_expand_key(g_plr_key, expanded);
    uint8_t *plain_padded = (uint8_t *)plr_malloc(encrypted_length);
    if (!plain_padded) return NULL;
    uint8_t previous[16];
    memcpy(previous, g_plr_key, 16u);
    for (uint32_t offset = 0u; offset < encrypted_length; offset += 16u) {
        uint8_t block[16];
        plr_aes_decrypt_block(encrypted + offset, block, expanded);
        for (uint32_t i = 0u; i < 16u; i++) plain_padded[offset + i] = block[i] ^ previous[i];
        memcpy(previous, encrypted + offset, 16u);
    }
    uint8_t padding = plain_padded[encrypted_length - 1u];
    if (padding == 0u || padding > 16u || padding > encrypted_length) {
        free(plain_padded);
        return NULL;
    }
    for (uint32_t i = 0u; i < padding; i++) {
        if (plain_padded[encrypted_length - 1u - i] != padding) {
            free(plain_padded);
            return NULL;
        }
    }
    uint32_t length = encrypted_length - padding;
    if (length == 0u) {
        free(plain_padded);
        return NULL;
    }
    uint8_t *plain = (uint8_t *)plr_malloc(length);
    if (!plain) {
        free(plain_padded);
        return NULL;
    }
    memcpy(plain, plain_padded, length);
    free(plain_padded);
    if (out_length) *out_length = length;
    return plain;
}

static uint8_t *plr_encrypt(
    const uint8_t *plain, uint32_t plain_length, uint32_t *out_length) {
    if (out_length) *out_length = 0u;
    if (!plain || plain_length == 0u || plain_length > PLR_MAX_FILE_BYTES - 16u)
        return NULL;
    uint32_t padding = 16u - (plain_length & 15u);
    uint32_t encrypted_length = plain_length + padding;
    uint8_t *encrypted = (uint8_t *)plr_malloc(encrypted_length);
    if (!encrypted) return NULL;
    uint8_t expanded[176];
    plr_aes_expand_key(g_plr_key, expanded);
    uint8_t previous[16];
    memcpy(previous, g_plr_key, 16u);
    for (uint32_t offset = 0u; offset < encrypted_length; offset += 16u) {
        uint8_t block[16];
        for (uint32_t i = 0u; i < 16u; i++) {
            uint32_t index = offset + i;
            block[i] = index < plain_length ? plain[index] : (uint8_t)padding;
            block[i] ^= previous[i];
        }
        plr_aes_encrypt_block(block, encrypted + offset, expanded);
        memcpy(previous, encrypted + offset, 16u);
    }
    if (out_length) *out_length = encrypted_length;
    return encrypted;
}

/* -------------------------------------------------------------------------
 * Semantic model readers
 * ------------------------------------------------------------------------- */

static int plr_put_reader_i32(
    PlrJsonValue *object, const char *key, PlrReader *reader) {
    return plr_json_object_put_i64(object, key, plr_read_i32(reader));
}

static int plr_put_reader_i64(
    PlrJsonValue *object, const char *key, PlrReader *reader) {
    return plr_json_object_put_i64(object, key, plr_read_i64(reader));
}

static int plr_put_reader_u8(
    PlrJsonValue *object, const char *key, PlrReader *reader) {
    return plr_json_object_put_u64(object, key, plr_read_u8(reader));
}

static int plr_put_reader_bool(
    PlrJsonValue *object, const char *key, PlrReader *reader) {
    return plr_json_object_put_bool(object, key, plr_read_u8(reader) != 0u);
}

static PlrJsonValue *plr_read_color(PlrReader *reader) {
    uint8_t r = plr_read_u8(reader);
    uint8_t g = plr_read_u8(reader);
    uint8_t b = plr_read_u8(reader);
    PlrJsonValue *color = plr_json_object();
    if (!color || !reader->ok ||
        !plr_json_object_put_u64(color, "b", b) ||
        !plr_json_object_put_u64(color, "g", g) ||
        !plr_json_object_put_u64(color, "r", r)) {
        plr_json_free(color);
        return NULL;
    }
    return color;
}

static PlrJsonValue *plr_make_item(
    int32_t item_type, int32_t stack, uint8_t prefix, int favorited) {
    PlrJsonValue *item = plr_json_object();
    if (!item) return NULL;
    if (!plr_json_object_put_bool(item, "favorited", favorited) ||
        !plr_json_object_put_i64(item, "itemType", item_type) ||
        !plr_json_object_put_u64(item, "prefix", prefix) ||
        !plr_json_object_put_i64(item, "stack", stack)) {
        plr_json_free(item);
        return NULL;
    }
    return item;
}

static PlrJsonValue *plr_read_full_item(PlrReader *reader, int with_favorited) {
    int32_t item_type = plr_read_i32(reader);
    int32_t stack = plr_read_i32(reader);
    uint8_t prefix = plr_read_u8(reader);
    int favorited = with_favorited ? (plr_read_u8(reader) != 0u) : 0;
    if (!reader->ok) return NULL;
    return plr_make_item(item_type, stack, prefix, favorited);
}

static PlrJsonValue *plr_read_type_prefix_item(PlrReader *reader) {
    int32_t item_type = plr_read_i32(reader);
    uint8_t prefix = plr_read_u8(reader);
    if (!reader->ok) return NULL;
    return plr_make_item(item_type, 0, prefix, 0);
}

static PlrJsonValue *plr_read_item_array(
    PlrReader *reader, uint32_t count, int with_favorited, int type_prefix) {
    PlrJsonValue *array = plr_json_array();
    if (!array) return NULL;
    for (uint32_t i = 0u; i < count; i++) {
        PlrJsonValue *item = type_prefix ?
            plr_read_type_prefix_item(reader) : plr_read_full_item(reader, with_favorited);
        if (!item || !plr_json_array_push(array, item)) {
            plr_json_free(item);
            plr_json_free(array);
            return NULL;
        }
    }
    return array;
}

static PlrJsonValue *plr_read_buffs(PlrReader *reader) {
    PlrJsonValue *array = plr_json_array();
    if (!array) return NULL;
    for (uint32_t i = 0u; i < PLR_BUFF_SLOTS; i++) {
        int32_t buff_type = plr_read_i32(reader);
        int32_t buff_time = plr_read_i32(reader);
        PlrJsonValue *buff = plr_json_object();
        if (!buff || !reader->ok ||
            !plr_json_object_put_i64(buff, "buffTime", buff_time) ||
            !plr_json_object_put_i64(buff, "buffType", buff_type) ||
            !plr_json_array_push(array, buff)) {
            plr_json_free(buff);
            plr_json_free(array);
            return NULL;
        }
    }
    return array;
}

static PlrJsonValue *plr_read_bool_array(PlrReader *reader, uint32_t count) {
    PlrJsonValue *array = plr_json_array();
    if (!array) return NULL;
    for (uint32_t i = 0u; i < count; i++) {
        PlrJsonValue *item = plr_json_bool(plr_read_u8(reader) != 0u);
        if (!item || !plr_json_array_push(array, item)) {
            plr_json_free(item);
            plr_json_free(array);
            return NULL;
        }
    }
    return array;
}

static PlrJsonValue *plr_read_i32_array(PlrReader *reader, uint32_t count) {
    PlrJsonValue *array = plr_json_array();
    if (!array) return NULL;
    for (uint32_t i = 0u; i < count; i++) {
        PlrJsonValue *item = plr_json_i64(plr_read_i32(reader));
        if (!item || !plr_json_array_push(array, item)) {
            plr_json_free(item);
            plr_json_free(array);
            return NULL;
        }
    }
    return array;
}

static PlrJsonValue *plr_read_spawn_points(PlrReader *reader) {
    PlrJsonValue *array = plr_json_array();
    if (!array) return NULL;
    for (uint32_t i = 0u; i < PLR_MAX_SPAWN_POINTS; i++) {
        int32_t x = plr_read_i32(reader);
        if (!reader->ok) break;
        if (x == -1) return array;
        int32_t y = plr_read_i32(reader);
        int32_t icon = plr_read_i32(reader);
        char *name = plr_read_string(reader);
        PlrJsonValue *point = plr_json_object();
        if (!point || !name) {
            free(name);
            plr_json_free(point);
            plr_json_free(array);
            return NULL;
        }
        if (!plr_json_object_put_i64(point, "icon", icon)) {
            free(name); plr_json_free(point); plr_json_free(array); return NULL;
        }
        /* The string-value constructor consumes name on both outcomes. */
        char *owned_name = name;
        name = NULL;
        if (!plr_json_object_put_string_owned(point, "name", owned_name) ||
            !plr_json_object_put_i64(point, "x", x) ||
            !plr_json_object_put_i64(point, "y", y)) {
            plr_json_free(point); plr_json_free(array); return NULL;
        }
        if (!plr_json_array_push(array, point)) {
            plr_json_free(point); plr_json_free(array); return NULL;
        }
        point = NULL;
        if (!reader->ok) {
            plr_json_free(array); return NULL;
        }
    }
    reader->ok = 0; /* a bounded spawn list must have a -1 sentinel */
    plr_json_free(array);
    return NULL;
}

static int plr_read_count(PlrReader *reader, uint32_t maximum, uint32_t minimum_bytes, uint32_t *out) {
    int32_t count = plr_read_i32(reader);
    if (!reader->ok || count < 0 || (uint32_t)count > maximum) {
        if (reader) reader->ok = 0;
        return 0;
    }
    if (minimum_bytes != 0u &&
        (uint32_t)count > (reader->length - reader->offset) / minimum_bytes) {
        reader->ok = 0;
        return 0;
    }
    *out = (uint32_t)count;
    return 1;
}

static PlrJsonValue *plr_read_sacrifices(PlrReader *reader) {
    int has_new_unlocks = plr_read_u8(reader) != 0u;
    uint32_t count = 0u;
    if (!plr_read_count(reader, PLR_MAX_SACRIFICES, 5u, &count)) return NULL;
    PlrJsonValue *result = plr_json_object();
    PlrJsonValue *items = plr_json_array();
    if (!result || !items ||
        !plr_json_object_put_bool(result, "hasNewUnlocks", has_new_unlocks)) {
        plr_json_free(result);
        plr_json_free(items);
        return NULL;
    }
    for (uint32_t i = 0u; i < count; i++) {
        char *persistent_id = plr_read_string(reader);
        PlrJsonValue *sacrifice = plr_json_object();
        if (!persistent_id || !sacrifice) {
            free(persistent_id); plr_json_free(sacrifice);
            plr_json_free(items); plr_json_free(result); return NULL;
        }
        int32_t amount = plr_read_i32(reader);
        if (!plr_json_object_put_i64(sacrifice, "amount", amount)) {
            free(persistent_id); plr_json_free(sacrifice);
            plr_json_free(items); plr_json_free(result); return NULL;
        }
        char *owned_id = persistent_id;
        persistent_id = NULL;
        if (!plr_json_object_put_string_owned(sacrifice, "persistentId", owned_id)) {
            plr_json_free(sacrifice); plr_json_free(items); plr_json_free(result); return NULL;
        }
        if (!plr_json_array_push(items, sacrifice)) {
            plr_json_free(sacrifice); plr_json_free(items); plr_json_free(result); return NULL;
        }
        sacrifice = NULL;
        if (!reader->ok) {
            plr_json_free(items); plr_json_free(result); return NULL;
        }
    }
    if (!plr_json_object_put(result, "items", items)) {
        plr_json_free(result);
        return NULL;
    }
    return result;
}

static PlrJsonValue *plr_read_temporary_slots(PlrReader *reader) {
    uint8_t flags = plr_read_u8(reader);
    PlrJsonValue *array = plr_json_array();
    if (!array) return NULL;
    for (uint32_t i = 0u; i < PLR_TEMPORARY_SLOTS; i++) {
        PlrJsonValue *item = (flags & (1u << i)) ?
            plr_read_full_item(reader, 0) : plr_json_null();
        if (!item || !plr_json_array_push(array, item)) {
            plr_json_free(item);
            plr_json_free(array);
            return NULL;
        }
    }
    return array;
}

static PlrJsonValue *plr_read_creative_powers(PlrReader *reader) {
    int godmode = 0;
    int far_placement = 0;
    float spawn_rate = 0.0f;
    for (;;) {
        int present = plr_read_u8(reader) != 0u;
        if (!reader->ok) return NULL;
        if (!present) break;
        uint16_t id = plr_read_u16(reader);
        if (!reader->ok) return NULL;
        if (id == 5u) godmode = plr_read_u8(reader) != 0u;
        else if (id == 11u) far_placement = plr_read_u8(reader) != 0u;
        else if (id == 14u) spawn_rate = plr_read_f32(reader);
        else {
            reader->ok = 0;
            return NULL;
        }
    }
    PlrJsonValue *powers = plr_json_object();
    if (!powers ||
        !plr_json_object_put_bool(powers, "farPlacementEnabled", far_placement) ||
        !plr_json_object_put_bool(powers, "godmodeEnabled", godmode) ||
        !plr_json_object_put_float(powers, "spawnRateSlider", spawn_rate)) {
        plr_json_free(powers);
        return NULL;
    }
    return powers;
}

static PlrJsonValue *plr_read_loadouts(PlrReader *reader) {
    PlrJsonValue *loadouts = plr_json_array();
    if (!loadouts) return NULL;
    for (uint32_t i = 0u; i < PLR_LOADOUTS; i++) {
        PlrJsonValue *loadout = plr_json_object();
        PlrJsonValue *armor = plr_read_item_array(reader, PLR_ARMOR_SLOTS, 0, 0);
        PlrJsonValue *dyes = plr_read_item_array(reader, PLR_DYE_SLOTS, 0, 0);
        PlrJsonValue *hide = plr_read_bool_array(reader, PLR_DYE_SLOTS);
        if (!loadout || !armor || !dyes || !hide) {
            plr_json_free(loadout); plr_json_free(armor); plr_json_free(dyes);
            plr_json_free(hide); plr_json_free(loadouts); return NULL;
        }
        if (!plr_json_object_put(loadout, "armor", armor)) {
            plr_json_free(dyes); plr_json_free(hide); plr_json_free(loadout); plr_json_free(loadouts); return NULL;
        }
        if (!plr_json_object_put(loadout, "dyes", dyes)) {
            plr_json_free(hide); plr_json_free(loadout); plr_json_free(loadouts); return NULL;
        }
        if (!plr_json_object_put(loadout, "hide", hide)) {
            plr_json_free(loadout); plr_json_free(loadouts); return NULL;
        }
        if (!plr_json_array_push(loadouts, loadout)) {
            plr_json_free(loadout); plr_json_free(loadouts); return NULL;
        }
    }
    return loadouts;
}

static PlrJsonValue *plr_read_dialogues(PlrReader *reader) {
    uint32_t count = 0u;
    if (!plr_read_count(reader, PLR_MAX_DIALOGUES, 1u, &count)) return NULL;
    PlrJsonValue *array = plr_json_array();
    if (!array) return NULL;
    for (uint32_t i = 0u; i < count; i++) {
        char *text = plr_read_string(reader);
        /* Transfer text ownership even when constructing the JSON node fails;
         * plr_json_string_owned releases it on its failure path. */
        PlrJsonValue *value = text ? plr_json_string_owned(text) : NULL;
        text = NULL;
        if (!value || !plr_json_array_push(array, value)) {
            free(text);
            plr_json_free(value);
            plr_json_free(array);
            return NULL;
        }
    }
    return array;
}

static int plr_root_put(PlrJsonValue *root, const char *key, PlrJsonValue *value) {
    return plr_json_object_put(root, key, value);
}

static PlrJsonValue *plr_parse_plain(
    const uint8_t *plain, uint32_t plain_length) {
    PlrReader reader = {plain, plain_length, 0u, 1};
    int32_t version = plr_read_i32(&reader);
    if (!reader.ok || version < PLR_MIN_VERSION || version > PLR_MAX_VERSION) return NULL;

    uint64_t magic_and_type = plr_read_u64(&reader);
    uint32_t revision = plr_read_u32(&reader);
    uint64_t favorite_flags = plr_read_u64(&reader);
    if (!reader.ok || (magic_and_type & UINT64_C(0x00ffffffffffffff)) !=
        PLR_METADATA_MAGIC_LOW_56 ||
        ((magic_and_type >> 56u) & 0xffu) != PLR_PLAYER_FILE_TYPE) {
        reader.ok = 0;
        return NULL;
    }
    PlrJsonValue *root = plr_json_object();
    PlrJsonValue *metadata = plr_json_object();
    if (!root || !metadata) {
        plr_json_free(metadata);
        plr_json_free(root);
        return NULL;
    }
    if (!plr_json_object_put_u64(metadata, "favoriteFlags", favorite_flags) ||
        !plr_json_object_put_u64(metadata, "magicAndType", magic_and_type) ||
        !plr_json_object_put_u64(metadata, "revision", revision)) {
        plr_json_free(metadata);
        plr_json_free(root);
        return NULL;
    }
    if (!plr_root_put(root, "metadata", metadata)) {
        /* plr_root_put owns metadata even when insertion fails. */
        plr_json_free(root);
        return NULL;
    }
    metadata = NULL;
    if (!plr_json_object_put_i64(root, "version", version)) {
        plr_json_free(root);
        return NULL;
    }

    char *name = plr_read_string(&reader);
    if (!name) {
        plr_json_free(root);
        return NULL;
    }
    /* The string-value constructor takes ownership on both success and failure. */
    char *owned_name = name;
    name = NULL;
    if (!plr_json_object_put_string_owned(root, "name", owned_name) ||
        !plr_json_object_put_u64(root, "difficulty", plr_read_u8(&reader)) ||
        !plr_put_reader_i64(root, "playTimeTicks", &reader) ||
        !plr_put_reader_i32(root, "hair", &reader) ||
        !plr_put_reader_u8(root, "hairDye", &reader) ||
        !plr_put_reader_u8(root, "team", &reader) || !reader.ok) {
        plr_json_free(root);
        return NULL;
    }

    uint8_t hide_lower = plr_read_u8(&reader);
    uint8_t hide_upper = plr_read_u8(&reader);
    PlrJsonValue *hide_accessory = plr_json_array();
    if (!hide_accessory) {
        plr_json_free(root);
        return NULL;
    }
    for (uint32_t i = 0u; i < PLR_DYE_SLOTS; i++) {
        int value = i < 8u ? ((hide_lower >> i) & 1u) != 0u :
            ((hide_upper >> (i - 8u)) & 1u) != 0u;
        PlrJsonValue *boolean = plr_json_bool(value);
        if (!boolean || !plr_json_array_push(hide_accessory, boolean)) {
            plr_json_free(boolean);
            plr_json_free(hide_accessory);
            plr_json_free(root);
            return NULL;
        }
    }
    if (!plr_root_put(root, "hideVisibleAccessory", hide_accessory)) {
        /* plr_root_put owns hide_accessory on insertion failure. */
        plr_json_free(root);
        return NULL;
    }
    hide_accessory = NULL;
    if (!plr_put_reader_u8(root, "hideMisc", &reader) ||
        !plr_put_reader_u8(root, "skinVariant", &reader) ||
        !plr_put_reader_i32(root, "statLife", &reader) ||
        !plr_put_reader_i32(root, "statLifeMax", &reader) ||
        !plr_put_reader_i32(root, "statMana", &reader) ||
        !plr_put_reader_i32(root, "statManaMax", &reader) ||
        !plr_put_reader_bool(root, "extraAccessory", &reader) ||
        !plr_put_reader_bool(root, "unlockedBiomeTorches", &reader) ||
        !plr_put_reader_bool(root, "usingBiomeTorches", &reader) ||
        !plr_put_reader_bool(root, "ateArtisanBread", &reader) ||
        !plr_put_reader_bool(root, "usedAegisCrystal", &reader) ||
        !plr_put_reader_bool(root, "usedAegisFruit", &reader) ||
        !plr_put_reader_bool(root, "usedArcaneCrystal", &reader) ||
        !plr_put_reader_bool(root, "usedGalaxyPearl", &reader) ||
        !plr_put_reader_bool(root, "usedGummyWorm", &reader) ||
        !plr_put_reader_bool(root, "usedAmbrosia", &reader) ||
        !plr_put_reader_bool(root, "downedDd2EventAnyDifficulty", &reader) ||
        !plr_put_reader_i32(root, "taxMoney", &reader) ||
        !plr_put_reader_i32(root, "numberOfDeathsPve", &reader) ||
        !plr_put_reader_i32(root, "numberOfDeathsPvp", &reader) || !reader.ok) {
        plr_json_free(root);
        return NULL;
    }

    const char *colors[] = {
        "hairColor", "skinColor", "eyeColor", "shirtColor",
        "underShirtColor", "pantsColor", "shoeColor"
    };
    for (uint32_t i = 0u; i < 7u; i++) {
        PlrJsonValue *color = plr_read_color(&reader);
        if (!color) {
            plr_json_free(root);
            return NULL;
        }
        /* root insertion owns color on failure as well as success. */
        if (!plr_root_put(root, colors[i], color)) {
            plr_json_free(root);
            return NULL;
        }
    }

    PlrJsonValue *armor = plr_read_item_array(&reader, PLR_ARMOR_SLOTS, 0, 1);
    PlrJsonValue *dyes = plr_read_item_array(&reader, PLR_DYE_SLOTS, 0, 1);
    PlrJsonValue *inventory = plr_read_item_array(&reader, PLR_INVENTORY_SLOTS, 1, 0);
    PlrJsonValue *misc_equips = plr_json_array();
    PlrJsonValue *misc_dyes = plr_json_array();
    if (!armor || !dyes || !inventory || !misc_equips || !misc_dyes) {
        plr_json_free(armor); plr_json_free(dyes); plr_json_free(inventory);
        plr_json_free(misc_equips); plr_json_free(misc_dyes); plr_json_free(root);
        return NULL;
    }
    for (uint32_t i = 0u; i < PLR_MISC_SLOTS; i++) {
        PlrJsonValue *equip = plr_read_type_prefix_item(&reader);
        PlrJsonValue *dye = plr_read_type_prefix_item(&reader);
        if (!equip || !dye) {
            plr_json_free(equip); plr_json_free(dye);
            plr_json_free(armor); plr_json_free(dyes); plr_json_free(inventory);
            plr_json_free(misc_equips); plr_json_free(misc_dyes); plr_json_free(root);
            return NULL;
        }
        if (!plr_json_array_push(misc_equips, equip)) {
            plr_json_free(equip); plr_json_free(dye);
            plr_json_free(armor); plr_json_free(dyes); plr_json_free(inventory);
            plr_json_free(misc_equips); plr_json_free(misc_dyes); plr_json_free(root);
            return NULL;
        }
        equip = NULL;
        if (!plr_json_array_push(misc_dyes, dye)) {
            plr_json_free(dye);
            plr_json_free(armor); plr_json_free(dyes); plr_json_free(inventory);
            plr_json_free(misc_equips); plr_json_free(misc_dyes); plr_json_free(root);
            return NULL;
        }
    }
    PlrJsonValue *piggy = plr_read_item_array(&reader, PLR_BANK_SLOTS, 0, 0);
    PlrJsonValue *safe = plr_read_item_array(&reader, PLR_BANK_SLOTS, 0, 0);
    PlrJsonValue *forge = plr_read_item_array(&reader, PLR_BANK_SLOTS, 0, 0);
    PlrJsonValue *vault = plr_read_item_array(&reader, PLR_BANK_SLOTS, 1, 0);
    if (!piggy || !safe || !forge || !vault) {
        plr_json_free(armor); plr_json_free(dyes); plr_json_free(inventory);
        plr_json_free(misc_equips); plr_json_free(misc_dyes);
        plr_json_free(piggy); plr_json_free(safe); plr_json_free(forge); plr_json_free(vault);
        plr_json_free(root);
        return NULL;
    }
    if (!plr_root_put(root, "armor", armor)) {
        plr_json_free(dyes); plr_json_free(inventory);
        plr_json_free(misc_equips); plr_json_free(misc_dyes);
        plr_json_free(piggy); plr_json_free(safe); plr_json_free(forge);
        plr_json_free(vault); plr_json_free(root); return NULL;
    }
    armor = NULL;
    if (!plr_root_put(root, "dyes", dyes)) {
        plr_json_free(inventory); plr_json_free(misc_equips); plr_json_free(misc_dyes);
        plr_json_free(piggy); plr_json_free(safe); plr_json_free(forge);
        plr_json_free(vault); plr_json_free(root); return NULL;
    }
    dyes = NULL;
    if (!plr_root_put(root, "inventory", inventory)) {
        plr_json_free(misc_equips); plr_json_free(misc_dyes);
        plr_json_free(piggy); plr_json_free(safe); plr_json_free(forge);
        plr_json_free(vault); plr_json_free(root); return NULL;
    }
    inventory = NULL;
    if (!plr_root_put(root, "miscEquips", misc_equips)) {
        plr_json_free(misc_dyes); plr_json_free(piggy); plr_json_free(safe);
        plr_json_free(forge); plr_json_free(vault); plr_json_free(root); return NULL;
    }
    misc_equips = NULL;
    if (!plr_root_put(root, "miscDyes", misc_dyes)) {
        plr_json_free(piggy); plr_json_free(safe); plr_json_free(forge);
        plr_json_free(vault); plr_json_free(root); return NULL;
    }
    misc_dyes = NULL;
    if (!plr_root_put(root, "piggyBank", piggy)) {
        plr_json_free(safe); plr_json_free(forge); plr_json_free(vault);
        plr_json_free(root); return NULL;
    }
    piggy = NULL;
    if (!plr_root_put(root, "safe", safe)) {
        plr_json_free(forge); plr_json_free(vault); plr_json_free(root); return NULL;
    }
    safe = NULL;
    if (!plr_root_put(root, "defendersForge", forge)) {
        plr_json_free(vault); plr_json_free(root); return NULL;
    }
    forge = NULL;
    if (!plr_root_put(root, "voidVault", vault)) {
        plr_json_free(root); return NULL;
    }
    vault = NULL;
    if (!plr_put_reader_u8(root, "voidVaultInfo", &reader) || !reader.ok) {
        plr_json_free(root); return NULL;
    }
    PlrJsonValue *buffs = plr_read_buffs(&reader);
    if (!buffs) {
        plr_json_free(root); return NULL;
    }
    if (!plr_root_put(root, "buffs", buffs)) {
        plr_json_free(root); return NULL;
    }

    PlrJsonValue *spawn_points = plr_read_spawn_points(&reader);
    if (!spawn_points) {
        plr_json_free(root); return NULL;
    }
    if (!plr_root_put(root, "spawnPoints", spawn_points)) {
        plr_json_free(root); return NULL;
    }
    if (!plr_put_reader_bool(root, "hbLocked", &reader) || !reader.ok) {
        plr_json_free(root); return NULL;
    }
    PlrJsonValue *hide_info = plr_read_bool_array(&reader, PLR_HIDE_INFO_SLOTS);
    int32_t angler_quests_finished = plr_read_i32(&reader);
    PlrJsonValue *dpad = plr_read_i32_array(&reader, PLR_DPAD_SLOTS);
    PlrJsonValue *builder_status = plr_read_i32_array(&reader, PLR_BUILDER_STATUS_SLOTS);
    int32_t bartender_quest_log = plr_read_i32(&reader);
    uint8_t dead = plr_read_u8(&reader);
    if (!hide_info || !dpad || !builder_status || !reader.ok) {
        plr_json_free(hide_info); plr_json_free(dpad); plr_json_free(builder_status);
        plr_json_free(root); return NULL;
    }
    if (!plr_root_put(root, "hideInfo", hide_info)) {
        plr_json_free(dpad); plr_json_free(builder_status); plr_json_free(root); return NULL;
    }
    if (!plr_json_object_put_i64(root, "anglerQuestsFinished", angler_quests_finished)) {
        plr_json_free(dpad); plr_json_free(builder_status); plr_json_free(root); return NULL;
    }
    if (!plr_root_put(root, "dpadRadialBindings", dpad)) {
        plr_json_free(builder_status); plr_json_free(root); return NULL;
    }
    if (!plr_root_put(root, "builderAccStatus", builder_status)) {
        plr_json_free(root); return NULL;
    }
    if (!plr_json_object_put_i64(root, "bartenderQuestLog", bartender_quest_log) ||
        !plr_json_object_put_bool(root, "dead", dead != 0u)) {
        plr_json_free(root); return NULL;
    }
    PlrJsonValue *respawn = plr_json_null();
    PlrJsonValue *dead_value = plr_json_object_get(root, "dead");
    if (dead_value && dead_value->type == PLR_JSON_BOOL && dead_value->as.boolean) {
        plr_json_free(respawn);
        respawn = plr_json_i64(plr_read_i32(&reader));
    }
    if (!respawn) {
        plr_json_free(root); return NULL;
    }
    if (!plr_root_put(root, "respawnTimer", respawn)) {
        plr_json_free(root); return NULL;
    }
    if (!plr_put_reader_i64(root, "lastSaveUtcTicks", &reader) ||
        !plr_put_reader_i32(root, "golferScoreAccumulated", &reader) || !reader.ok) {
        plr_json_free(root); return NULL;
    }

    PlrJsonValue *sacrifices = plr_read_sacrifices(&reader);
    PlrJsonValue *creative_sacrifice_items = sacrifices ?
        plr_json_object_get(sacrifices, "items") : NULL;
    PlrJsonValue *creative_unlocks = sacrifices ?
        plr_json_object_get(sacrifices, "hasNewUnlocks") : NULL;
    if (!sacrifices || !creative_sacrifice_items || !creative_unlocks ||
        !plr_root_put(root, "creativeTrackerHasNewUnlocks",
            plr_json_clone(creative_unlocks)) ||
        !plr_root_put(root, "creativeItemSacrifices",
            plr_json_clone(creative_sacrifice_items))) {
        plr_json_free(sacrifices); plr_json_free(root); return NULL;
    }
    plr_json_free(sacrifices);

    PlrJsonValue *temporary = plr_read_temporary_slots(&reader);
    PlrJsonValue *powers = plr_read_creative_powers(&reader);
    uint8_t super_flags = plr_read_u8(&reader);
    PlrJsonValue *unlocked_super = plr_json_bool((super_flags & 1u) != 0u);
    PlrJsonValue *enabled_super = plr_json_bool((super_flags & 2u) != 0u);
    if (!temporary || !powers || !unlocked_super || !enabled_super || !reader.ok) {
        plr_json_free(temporary); plr_json_free(powers);
        plr_json_free(unlocked_super); plr_json_free(enabled_super);
        plr_json_free(root); return NULL;
    }
    if (!plr_root_put(root, "temporarySlots", temporary)) {
        plr_json_free(powers); plr_json_free(unlocked_super); plr_json_free(enabled_super);
        plr_json_free(root); return NULL;
    }
    if (!plr_root_put(root, "creativePowers", powers)) {
        plr_json_free(unlocked_super); plr_json_free(enabled_super);
        plr_json_free(root); return NULL;
    }
    if (!plr_root_put(root, "unlockedSuperCart", unlocked_super)) {
        plr_json_free(enabled_super); plr_json_free(root); return NULL;
    }
    if (!plr_root_put(root, "enabledSuperCart", enabled_super) ||
        !plr_put_reader_i32(root, "currentLoadoutIndex", &reader) || !reader.ok) {
        plr_json_free(root); return NULL;
    }
    PlrJsonValue *loadouts = plr_read_loadouts(&reader);
    if (!loadouts) {
        plr_json_free(root); return NULL;
    }
    if (!plr_root_put(root, "loadouts", loadouts) ||
        !plr_put_reader_u8(root, "voiceVariant", &reader) || !reader.ok) {
        plr_json_free(root); return NULL;
    }
    float voice_pitch = plr_read_f32(&reader);
    if (!reader.ok || !plr_json_object_put_float(root, "voicePitchOffset", voice_pitch)) {
        plr_json_free(root); return NULL;
    }
    uint32_t pending_count = 0u;
    if (!plr_read_count(&reader, PLR_MAX_PENDING_REFUNDS, 9u, &pending_count)) {
        plr_json_free(root); return NULL;
    }
    PlrJsonValue *pending = plr_read_item_array(&reader, pending_count, 0, 0);
    PlrJsonValue *dialogues = plr_read_dialogues(&reader);
    if (!pending || !dialogues) {
        plr_json_free(pending); plr_json_free(dialogues); plr_json_free(root); return NULL;
    }
    if (!plr_root_put(root, "pendingRefunds", pending)) {
        plr_json_free(dialogues); plr_json_free(root); return NULL;
    }
    if (!plr_root_put(root, "oneTimeDialoguesSeen", dialogues)) {
        plr_json_free(root); return NULL;
    }

    PlrJsonValue *layout = plr_json_object();
    if (!layout ||
        !plr_json_object_put_u64(layout, "builderAccStatusCount", PLR_BUILDER_STATUS_SLOTS) ||
        !plr_json_object_put_bool(layout, "includesDeathMetadata", 1)) {
        plr_json_free(layout); plr_json_free(root); return NULL;
    }
    if (!plr_root_put(root, "tailLayout", layout)) {
        plr_json_free(root); return NULL;
    }
    if (!reader.ok || reader.offset != reader.length) {
        plr_json_free(root); return NULL;
    }
    return root;
}

/* -------------------------------------------------------------------------
 * Semantic model validation and typed accessors
 * -------------------------------------------------------------------------
 */

static int plr_model_error(const char *message) {
    tx_set_error("TERRAX_PLR_VALIDATION_ERROR", message ? message : "invalid PLR model");
    return 0;
}

static int plr_value_is_number(const PlrJsonValue *value) {
    return value && value->type == PLR_JSON_NUMBER;
}

static int plr_value_i64(const PlrJsonValue *value, int64_t *out) {
    if (!plr_value_is_number(value)) return 0;
    if (value->as.number.kind == PLR_NUMBER_I64) {
        if (out) *out = value->as.number.i64;
        return 1;
    }
    if (value->as.number.kind == PLR_NUMBER_U64 &&
        value->as.number.u64 <= UINT64_C(9223372036854775807)) {
        if (out) *out = (int64_t)value->as.number.u64;
        return 1;
    }
    return 0;
}

static int plr_value_u64(const PlrJsonValue *value, uint64_t *out) {
    if (!plr_value_is_number(value)) return 0;
    if (value->as.number.kind == PLR_NUMBER_U64) {
        if (out) *out = value->as.number.u64;
        return 1;
    }
    if (value->as.number.kind == PLR_NUMBER_I64 && value->as.number.i64 >= 0) {
        if (out) *out = (uint64_t)value->as.number.i64;
        return 1;
    }
    return 0;
}

/* JavaScript Numbers cannot carry Terraria's 56-bit metadata payload without
 * rounding. TerraR normalizes this invariant when JSON crosses its API; do
 * the same in-place before validating an editable model. Binary input has
 * already passed the strict on-disk magic/type check in plr_parse_plain. */
static void plr_normalize_metadata_magic(PlrJsonValue *root) {
    if (!root || root->type != PLR_JSON_OBJECT) return;
    PlrJsonValue *metadata = plr_json_object_get(root, "metadata");
    if (!metadata || metadata->type == PLR_JSON_NULL) return;
    PlrJsonValue *magic = plr_json_object_get(metadata, "magicAndType");
    uint64_t ignored = 0u;
    if (!plr_value_u64(magic, &ignored)) return;
    magic->as.number.kind = PLR_NUMBER_U64;
    magic->as.number.u64 = PLR_DEFAULT_MAGIC_AND_TYPE;
    magic->as.number.i64 = 0;
    magic->as.number.floating = 0.0;
}

static int plr_value_i32(const PlrJsonValue *value, int32_t *out) {
    int64_t number = 0;
    if (!plr_value_i64(value, &number) || number < INT32_MIN || number > INT32_MAX)
        return 0;
    if (out) *out = (int32_t)number;
    return 1;
}

static int plr_value_u8(const PlrJsonValue *value, uint8_t *out) {
    uint64_t number = 0;
    if (!plr_value_u64(value, &number) || number > UINT8_MAX) return 0;
    if (out) *out = (uint8_t)number;
    return 1;
}

static int plr_value_u32(const PlrJsonValue *value, uint32_t *out) {
    uint64_t number = 0;
    if (!plr_value_u64(value, &number) || number > UINT32_MAX) return 0;
    if (out) *out = (uint32_t)number;
    return 1;
}

static int plr_value_float(const PlrJsonValue *value, float *out) {
    if (!plr_value_is_number(value)) return 0;
    double number = value->as.number.floating;
    if (value->as.number.kind == PLR_NUMBER_I64)
        number = (double)value->as.number.i64;
    else if (value->as.number.kind == PLR_NUMBER_U64)
        number = (double)value->as.number.u64;
    if (!isfinite(number) || number < -FLT_MAX || number > FLT_MAX) return 0;
    if (out) *out = (float)number;
    return 1;
}

static int plr_value_bool(const PlrJsonValue *value, int *out) {
    if (!value || value->type != PLR_JSON_BOOL) return 0;
    if (out) *out = value->as.boolean ? 1 : 0;
    return 1;
}

static int plr_value_string(const PlrJsonValue *value, const char **out) {
    if (!value || value->type != PLR_JSON_STRING || !value->as.string) return 0;
    uint32_t length = tx_strlen(value->as.string);
    if (length > PLR_MAX_STRING_BYTES ||
        !plr_utf8_valid((const uint8_t *)value->as.string, length)) return 0;
    if (out) *out = value->as.string;
    return 1;
}

static int plr_required_i32(const PlrJsonValue *object, const char *key) {
    return plr_value_i32(plr_json_object_get(object, key), NULL);
}

static int plr_required_i64(const PlrJsonValue *object, const char *key) {
    return plr_value_i64(plr_json_object_get(object, key), NULL);
}

static int plr_required_u64(const PlrJsonValue *object, const char *key) {
    return plr_value_u64(plr_json_object_get(object, key), NULL);
}

static int plr_required_u8(const PlrJsonValue *object, const char *key) {
    return plr_value_u8(plr_json_object_get(object, key), NULL);
}

static int plr_required_u32(const PlrJsonValue *object, const char *key) {
    return plr_value_u32(plr_json_object_get(object, key), NULL);
}

static int plr_required_bool(const PlrJsonValue *object, const char *key) {
    return plr_value_bool(plr_json_object_get(object, key), NULL);
}

static int plr_required_string(const PlrJsonValue *object, const char *key) {
    return plr_value_string(plr_json_object_get(object, key), NULL);
}

static int plr_validate_color(const PlrJsonValue *value) {
    return value && value->type == PLR_JSON_OBJECT &&
        plr_required_u8(value, "r") && plr_required_u8(value, "g") &&
        plr_required_u8(value, "b");
}

static int plr_validate_item(const PlrJsonValue *value) {
    return value && value->type == PLR_JSON_OBJECT &&
        plr_required_i32(value, "itemType") &&
        plr_required_i32(value, "stack") &&
        plr_required_u8(value, "prefix") &&
        plr_required_bool(value, "favorited");
}

static int plr_validate_fixed_item_array(
    const PlrJsonValue *object, const char *key, uint32_t count) {
    const PlrJsonValue *array = plr_json_object_get(object, key);
    if (!array || array->type != PLR_JSON_ARRAY || array->as.array.count != count)
        return 0;
    for (uint32_t i = 0u; i < count; i++) {
        if (!plr_validate_item(array->as.array.items[i])) return 0;
    }
    return 1;
}

static int plr_validate_bool_array(
    const PlrJsonValue *object, const char *key, uint32_t count) {
    const PlrJsonValue *array = plr_json_object_get(object, key);
    if (!array || array->type != PLR_JSON_ARRAY || array->as.array.count != count)
        return 0;
    for (uint32_t i = 0u; i < count; i++) {
        if (!plr_value_bool(array->as.array.items[i], NULL)) return 0;
    }
    return 1;
}

static int plr_validate_i32_array(
    const PlrJsonValue *object, const char *key, uint32_t count) {
    const PlrJsonValue *array = plr_json_object_get(object, key);
    if (!array || array->type != PLR_JSON_ARRAY || array->as.array.count != count)
        return 0;
    for (uint32_t i = 0u; i < count; i++) {
        if (!plr_value_i32(array->as.array.items[i], NULL)) return 0;
    }
    return 1;
}

static int plr_validate_spawn_points(const PlrJsonValue *object) {
    const PlrJsonValue *array = plr_json_object_get(object, "spawnPoints");
    if (!array || array->type != PLR_JSON_ARRAY ||
        array->as.array.count > PLR_MAX_SPAWN_POINTS) return 0;
    for (uint32_t i = 0u; i < array->as.array.count; i++) {
        const PlrJsonValue *point = array->as.array.items[i];
        if (!point || point->type != PLR_JSON_OBJECT ||
            !plr_required_i32(point, "x") || !plr_required_i32(point, "y") ||
            !plr_required_i32(point, "icon") || !plr_required_string(point, "name"))
            return 0;
    }
    return 1;
}

static int plr_validate_sacrifices(const PlrJsonValue *object) {
    const PlrJsonValue *array = plr_json_object_get(object, "creativeItemSacrifices");
    if (!array || array->type != PLR_JSON_ARRAY ||
        array->as.array.count > PLR_MAX_SACRIFICES) return 0;
    for (uint32_t i = 0u; i < array->as.array.count; i++) {
        const PlrJsonValue *item = array->as.array.items[i];
        if (!item || item->type != PLR_JSON_OBJECT ||
            !plr_required_i32(item, "amount") ||
            !plr_required_string(item, "persistentId")) return 0;
    }
    return plr_required_bool(object, "creativeTrackerHasNewUnlocks");
}

static int plr_validate_temporary_slots(const PlrJsonValue *object) {
    const PlrJsonValue *array = plr_json_object_get(object, "temporarySlots");
    if (!array || array->type != PLR_JSON_ARRAY ||
        array->as.array.count != PLR_TEMPORARY_SLOTS) return 0;
    for (uint32_t i = 0u; i < PLR_TEMPORARY_SLOTS; i++) {
        const PlrJsonValue *item = array->as.array.items[i];
        if (item->type != PLR_JSON_NULL && !plr_validate_item(item)) return 0;
    }
    return 1;
}

static int plr_validate_loadouts(const PlrJsonValue *object) {
    const PlrJsonValue *array = plr_json_object_get(object, "loadouts");
    if (!array || array->type != PLR_JSON_ARRAY || array->as.array.count != PLR_LOADOUTS)
        return 0;
    for (uint32_t i = 0u; i < PLR_LOADOUTS; i++) {
        const PlrJsonValue *loadout = array->as.array.items[i];
        if (!loadout || loadout->type != PLR_JSON_OBJECT ||
            !plr_validate_fixed_item_array(loadout, "armor", PLR_ARMOR_SLOTS) ||
            !plr_validate_fixed_item_array(loadout, "dyes", PLR_DYE_SLOTS) ||
            !plr_validate_bool_array(loadout, "hide", PLR_DYE_SLOTS)) return 0;
    }
    return 1;
}

static int plr_validate_model(const PlrJsonValue *root) {
    if (!root || root->type != PLR_JSON_OBJECT) return plr_model_error("PLR model must be an object");
    if (!plr_required_i32(root, "version")) return plr_model_error("PLR version is missing or invalid");
    int32_t version = 0;
    if (!plr_value_i32(plr_json_object_get(root, "version"), &version) ||
        version < PLR_MIN_VERSION || version > PLR_MAX_VERSION)
        return plr_model_error("PLR version must be 318 or 319");

    const PlrJsonValue *metadata = plr_json_object_get(root, "metadata");
    if (!metadata) return plr_model_error("PLR metadata is missing or invalid");
    if (metadata->type != PLR_JSON_NULL) {
        if (metadata->type != PLR_JSON_OBJECT)
            return plr_model_error("PLR metadata is not an object");
        if (!plr_required_u64(metadata, "magicAndType"))
            return plr_model_error("PLR metadata magicAndType is invalid");
        if (!plr_required_u32(metadata, "revision"))
            return plr_model_error("PLR metadata revision is invalid");
        if (!plr_value_u64(plr_json_object_get(metadata, "favoriteFlags"), NULL))
            return plr_model_error("PLR metadata favoriteFlags is invalid");
    }

    if (!plr_required_string(root, "name") || !plr_required_u8(root, "difficulty") ||
        !plr_required_i64(root, "playTimeTicks") || !plr_required_i32(root, "hair") ||
        !plr_required_u8(root, "hairDye") || !plr_required_u8(root, "team") ||
        !plr_validate_bool_array(root, "hideVisibleAccessory", PLR_DYE_SLOTS) ||
        !plr_required_u8(root, "hideMisc") || !plr_required_u8(root, "skinVariant") ||
        !plr_required_i32(root, "statLife") || !plr_required_i32(root, "statLifeMax") ||
        !plr_required_i32(root, "statMana") || !plr_required_i32(root, "statManaMax") ||
        !plr_required_bool(root, "extraAccessory") ||
        !plr_required_bool(root, "unlockedBiomeTorches") ||
        !plr_required_bool(root, "usingBiomeTorches") ||
        !plr_required_bool(root, "ateArtisanBread") ||
        !plr_required_bool(root, "usedAegisCrystal") ||
        !plr_required_bool(root, "usedAegisFruit") ||
        !plr_required_bool(root, "usedArcaneCrystal") ||
        !plr_required_bool(root, "usedGalaxyPearl") ||
        !plr_required_bool(root, "usedGummyWorm") ||
        !plr_required_bool(root, "usedAmbrosia") ||
        !plr_required_bool(root, "downedDd2EventAnyDifficulty") ||
        !plr_required_i32(root, "taxMoney") ||
        !plr_required_i32(root, "numberOfDeathsPve") ||
        !plr_required_i32(root, "numberOfDeathsPvp"))
        return plr_model_error("PLR body scalar field is missing or invalid");

    const char *colors[] = {
        "hairColor", "skinColor", "eyeColor", "shirtColor",
        "underShirtColor", "pantsColor", "shoeColor"
    };
    for (uint32_t i = 0u; i < 7u; i++) {
        if (!plr_validate_color(plr_json_object_get(root, colors[i])))
            return plr_model_error("PLR color field is missing or invalid");
    }
    if (!plr_validate_fixed_item_array(root, "armor", PLR_ARMOR_SLOTS) ||
        !plr_validate_fixed_item_array(root, "dyes", PLR_DYE_SLOTS) ||
        !plr_validate_fixed_item_array(root, "inventory", PLR_INVENTORY_SLOTS) ||
        !plr_validate_fixed_item_array(root, "miscEquips", PLR_MISC_SLOTS) ||
        !plr_validate_fixed_item_array(root, "miscDyes", PLR_MISC_SLOTS) ||
        !plr_validate_fixed_item_array(root, "piggyBank", PLR_BANK_SLOTS) ||
        !plr_validate_fixed_item_array(root, "safe", PLR_BANK_SLOTS) ||
        !plr_validate_fixed_item_array(root, "defendersForge", PLR_BANK_SLOTS) ||
        !plr_validate_fixed_item_array(root, "voidVault", PLR_BANK_SLOTS) ||
        !plr_required_u8(root, "voidVaultInfo"))
        return plr_model_error("PLR item section has an invalid slot count or value");

    const PlrJsonValue *buffs = plr_json_object_get(root, "buffs");
    if (!buffs || buffs->type != PLR_JSON_ARRAY || buffs->as.array.count != PLR_BUFF_SLOTS)
        return plr_model_error("PLR buff slot count is invalid");
    for (uint32_t i = 0u; i < PLR_BUFF_SLOTS; i++) {
        const PlrJsonValue *buff = buffs->as.array.items[i];
        if (!buff || buff->type != PLR_JSON_OBJECT ||
            !plr_required_i32(buff, "buffType") || !plr_required_i32(buff, "buffTime"))
            return plr_model_error("PLR buff value is invalid");
    }

    const PlrJsonValue *layout = plr_json_object_get(root, "tailLayout");
    uint32_t builder_count = 0u;
    if (!layout || layout->type != PLR_JSON_OBJECT ||
        !plr_required_u32(layout, "builderAccStatusCount") ||
        !plr_required_bool(layout, "includesDeathMetadata") ||
        !plr_value_u32(plr_json_object_get(layout, "builderAccStatusCount"), &builder_count) ||
        builder_count > 64u)
        return plr_model_error("PLR tail layout is invalid");
    uint32_t expected_builder_count = builder_count ? builder_count : PLR_BUILDER_STATUS_SLOTS;
    if (!plr_validate_spawn_points(root) || !plr_required_bool(root, "hbLocked") ||
        !plr_validate_bool_array(root, "hideInfo", PLR_HIDE_INFO_SLOTS) ||
        !plr_required_i32(root, "anglerQuestsFinished") ||
        !plr_validate_i32_array(root, "dpadRadialBindings", PLR_DPAD_SLOTS) ||
        !plr_validate_i32_array(root, "builderAccStatus", expected_builder_count) ||
        !plr_required_i32(root, "bartenderQuestLog") || !plr_required_bool(root, "dead"))
        return plr_model_error("PLR tail field is missing or invalid");
    const PlrJsonValue *respawn = plr_json_object_get(root, "respawnTimer");
    if (!respawn || (respawn->type != PLR_JSON_NULL && !plr_value_i32(respawn, NULL)) ||
        !plr_required_i64(root, "lastSaveUtcTicks") ||
        !plr_required_i32(root, "golferScoreAccumulated") ||
        !plr_validate_sacrifices(root) || !plr_validate_temporary_slots(root))
        return plr_model_error("PLR tail value is invalid");

    const PlrJsonValue *powers = plr_json_object_get(root, "creativePowers");
    if (!powers || powers->type != PLR_JSON_OBJECT ||
        !plr_required_bool(powers, "godmodeEnabled") ||
        !plr_required_bool(powers, "farPlacementEnabled") ||
        !plr_value_float(plr_json_object_get(powers, "spawnRateSlider"), NULL) ||
        !plr_required_bool(root, "unlockedSuperCart") ||
        !plr_required_bool(root, "enabledSuperCart") ||
        !plr_required_i32(root, "currentLoadoutIndex") ||
        !plr_validate_loadouts(root) || !plr_required_u8(root, "voiceVariant") ||
        !plr_value_float(plr_json_object_get(root, "voicePitchOffset"), NULL))
        return plr_model_error("PLR creative/loadout field is invalid");

    const PlrJsonValue *pending = plr_json_object_get(root, "pendingRefunds");
    if (!pending || pending->type != PLR_JSON_ARRAY ||
        pending->as.array.count > PLR_MAX_PENDING_REFUNDS)
        return plr_model_error("PLR pending refund count is invalid");
    for (uint32_t i = 0u; i < pending->as.array.count; i++) {
        if (!plr_validate_item(pending->as.array.items[i]))
            return plr_model_error("PLR pending refund value is invalid");
    }
    const PlrJsonValue *dialogues = plr_json_object_get(root, "oneTimeDialoguesSeen");
    if (!dialogues || dialogues->type != PLR_JSON_ARRAY ||
        dialogues->as.array.count > PLR_MAX_DIALOGUES)
        return plr_model_error("PLR dialogue count is invalid");
    for (uint32_t i = 0u; i < dialogues->as.array.count; i++) {
        if (!plr_value_string(dialogues->as.array.items[i], NULL))
            return plr_model_error("PLR dialogue value is invalid");
    }
    return 1;
}

/* -------------------------------------------------------------------------
 * Semantic model writer
 * -------------------------------------------------------------------------
 */

static const PlrJsonValue *plr_field(
    const PlrJsonValue *object, const char *key) {
    return plr_json_object_get(object, key);
}

static int plr_field_i32(
    const PlrJsonValue *object, const char *key, int32_t *out) {
    return plr_value_i32(plr_field(object, key), out);
}

static int plr_field_i64(
    const PlrJsonValue *object, const char *key, int64_t *out) {
    return plr_value_i64(plr_field(object, key), out);
}

static int plr_field_u8(
    const PlrJsonValue *object, const char *key, uint8_t *out) {
    return plr_value_u8(plr_field(object, key), out);
}

static int plr_field_u32(
    const PlrJsonValue *object, const char *key, uint32_t *out) {
    return plr_value_u32(plr_field(object, key), out);
}

static int plr_field_u64(
    const PlrJsonValue *object, const char *key, uint64_t *out) {
    return plr_value_u64(plr_field(object, key), out);
}

static int plr_field_bool(
    const PlrJsonValue *object, const char *key, int *out) {
    return plr_value_bool(plr_field(object, key), out);
}

static int plr_field_float(
    const PlrJsonValue *object, const char *key, float *out) {
    return plr_value_float(plr_field(object, key), out);
}

static int plr_field_string(
    const PlrJsonValue *object, const char *key, const char **out) {
    return plr_value_string(plr_field(object, key), out);
}

static const PlrJsonValue *plr_field_array(
    const PlrJsonValue *object, const char *key) {
    const PlrJsonValue *value = plr_field(object, key);
    return value && value->type == PLR_JSON_ARRAY ? value : NULL;
}

static void plr_writer_item(
    PlrWriter *writer, const PlrJsonValue *item, int with_favorited) {
    int32_t item_type = 0, stack = 0;
    uint8_t prefix = 0;
    int favorited = 0;
    if (!plr_field_i32(item, "itemType", &item_type) ||
        !plr_field_i32(item, "stack", &stack) ||
        !plr_field_u8(item, "prefix", &prefix) ||
        (with_favorited && !plr_field_bool(item, "favorited", &favorited))) {
        writer->ok = 0;
        return;
    }
    plr_writer_i32(writer, item_type);
    plr_writer_i32(writer, stack);
    plr_writer_u8(writer, prefix);
    if (with_favorited) plr_writer_u8(writer, favorited ? 1u : 0u);
}

static void plr_writer_type_prefix_item(
    PlrWriter *writer, const PlrJsonValue *item) {
    int32_t item_type = 0;
    uint8_t prefix = 0;
    if (!plr_field_i32(item, "itemType", &item_type) ||
        !plr_field_u8(item, "prefix", &prefix)) {
        writer->ok = 0;
        return;
    }
    plr_writer_i32(writer, item_type);
    plr_writer_u8(writer, prefix);
}

static void plr_writer_item_array(
    PlrWriter *writer, const PlrJsonValue *object, const char *key,
    uint32_t expected, int with_favorited, int type_prefix) {
    const PlrJsonValue *array = plr_field_array(object, key);
    if (!array || array->as.array.count != expected) {
        writer->ok = 0;
        return;
    }
    for (uint32_t i = 0u; i < expected; i++) {
        if (type_prefix) plr_writer_type_prefix_item(writer, array->as.array.items[i]);
        else plr_writer_item(writer, array->as.array.items[i], with_favorited);
    }
}

static void plr_writer_bool_array(
    PlrWriter *writer, const PlrJsonValue *object, const char *key,
    uint32_t expected) {
    const PlrJsonValue *array = plr_field_array(object, key);
    if (!array || array->as.array.count != expected) {
        writer->ok = 0;
        return;
    }
    for (uint32_t i = 0u; i < expected; i++) {
        int value = 0;
        if (!plr_value_bool(array->as.array.items[i], &value)) {
            writer->ok = 0;
            return;
        }
        plr_writer_u8(writer, value ? 1u : 0u);
    }
}

static void plr_writer_i32_array(
    PlrWriter *writer, const PlrJsonValue *object, const char *key,
    uint32_t expected) {
    const PlrJsonValue *array = plr_field_array(object, key);
    if (!array || array->as.array.count != expected) {
        writer->ok = 0;
        return;
    }
    for (uint32_t i = 0u; i < expected; i++) {
        int32_t value = 0;
        if (!plr_value_i32(array->as.array.items[i], &value)) {
            writer->ok = 0;
            return;
        }
        plr_writer_i32(writer, value);
    }
}

static void plr_writer_color(
    PlrWriter *writer, const PlrJsonValue *object, const char *key) {
    const PlrJsonValue *color = plr_field(object, key);
    uint8_t r = 0, g = 0, b = 0;
    if (!plr_field_u8(color, "r", &r) || !plr_field_u8(color, "g", &g) ||
        !plr_field_u8(color, "b", &b)) {
        writer->ok = 0;
        return;
    }
    plr_writer_u8(writer, r);
    plr_writer_u8(writer, g);
    plr_writer_u8(writer, b);
}

static void plr_writer_spawn_points(PlrWriter *writer, const PlrJsonValue *root) {
    const PlrJsonValue *array = plr_field_array(root, "spawnPoints");
    if (!array || array->as.array.count > PLR_MAX_SPAWN_POINTS) {
        writer->ok = 0;
        return;
    }
    for (uint32_t i = 0u; i < array->as.array.count; i++) {
        const PlrJsonValue *point = array->as.array.items[i];
        int32_t x = 0, y = 0, icon = 0;
        const char *name = NULL;
        if (!plr_field_i32(point, "x", &x) || !plr_field_i32(point, "y", &y) ||
            !plr_field_i32(point, "icon", &icon) ||
            !plr_field_string(point, "name", &name)) {
            writer->ok = 0;
            return;
        }
        plr_writer_i32(writer, x);
        plr_writer_i32(writer, y);
        plr_writer_i32(writer, icon);
        plr_writer_string(writer, name);
    }
    plr_writer_i32(writer, -1);
}

static void plr_writer_sacrifices(PlrWriter *writer, const PlrJsonValue *root) {
    int has_new_unlocks = 0;
    const PlrJsonValue *array = plr_field_array(root, "creativeItemSacrifices");
    if (!plr_field_bool(root, "creativeTrackerHasNewUnlocks", &has_new_unlocks) ||
        !array || array->as.array.count > PLR_MAX_SACRIFICES ||
        array->as.array.count > INT32_MAX) {
        writer->ok = 0;
        return;
    }
    plr_writer_u8(writer, has_new_unlocks ? 1u : 0u);
    plr_writer_i32(writer, (int32_t)array->as.array.count);
    for (uint32_t i = 0u; i < array->as.array.count; i++) {
        const PlrJsonValue *item = array->as.array.items[i];
        int32_t amount = 0;
        const char *persistent_id = NULL;
        if (!plr_field_i32(item, "amount", &amount) ||
            !plr_field_string(item, "persistentId", &persistent_id)) {
            writer->ok = 0;
            return;
        }
        plr_writer_string(writer, persistent_id);
        plr_writer_i32(writer, amount);
    }
}

static void plr_writer_temporary_slots(PlrWriter *writer, const PlrJsonValue *root) {
    const PlrJsonValue *array = plr_field_array(root, "temporarySlots");
    if (!array || array->as.array.count != PLR_TEMPORARY_SLOTS) {
        writer->ok = 0;
        return;
    }
    uint8_t flags = 0u;
    for (uint32_t i = 0u; i < PLR_TEMPORARY_SLOTS; i++) {
        const PlrJsonValue *item = array->as.array.items[i];
        if (item->type != PLR_JSON_NULL) flags |= (uint8_t)(1u << i);
    }
    plr_writer_u8(writer, flags);
    for (uint32_t i = 0u; i < PLR_TEMPORARY_SLOTS; i++) {
        const PlrJsonValue *item = array->as.array.items[i];
        if (item->type != PLR_JSON_NULL) plr_writer_item(writer, item, 0);
    }
}

static void plr_writer_creative_powers(PlrWriter *writer, const PlrJsonValue *root) {
    const PlrJsonValue *powers = plr_field(root, "creativePowers");
    int godmode = 0, far_placement = 0;
    float spawn_rate = 0.0f;
    if (!plr_field_bool(powers, "godmodeEnabled", &godmode) ||
        !plr_field_bool(powers, "farPlacementEnabled", &far_placement) ||
        !plr_field_float(powers, "spawnRateSlider", &spawn_rate)) {
        writer->ok = 0;
        return;
    }
    plr_writer_u8(writer, 1u);
    plr_writer_u16(writer, 5u);
    plr_writer_u8(writer, godmode ? 1u : 0u);
    plr_writer_u8(writer, 1u);
    plr_writer_u16(writer, 11u);
    plr_writer_u8(writer, far_placement ? 1u : 0u);
    plr_writer_u8(writer, 1u);
    plr_writer_u16(writer, 14u);
    plr_writer_f32(writer, spawn_rate);
    plr_writer_u8(writer, 0u);
}

static void plr_writer_loadouts(PlrWriter *writer, const PlrJsonValue *root) {
    const PlrJsonValue *array = plr_field_array(root, "loadouts");
    if (!array || array->as.array.count != PLR_LOADOUTS) {
        writer->ok = 0;
        return;
    }
    for (uint32_t i = 0u; i < PLR_LOADOUTS; i++) {
        const PlrJsonValue *loadout = array->as.array.items[i];
        plr_writer_item_array(writer, loadout, "armor", PLR_ARMOR_SLOTS, 0, 0);
        plr_writer_item_array(writer, loadout, "dyes", PLR_DYE_SLOTS, 0, 0);
        plr_writer_bool_array(writer, loadout, "hide", PLR_DYE_SLOTS);
    }
}

static uint8_t *plr_encode_plain(
    const PlrJsonValue *root, uint32_t *out_length) {
    if (out_length) *out_length = 0u;
    if (!plr_validate_model(root)) return NULL;
    PlrWriter writer = {NULL, 0u, 0u, 1};
    int32_t version = 0;
    if (!plr_field_i32(root, "version", &version)) writer.ok = 0;
    plr_writer_i32(&writer, version);

    const PlrJsonValue *metadata = plr_field(root, "metadata");
    uint64_t magic_and_type = PLR_DEFAULT_MAGIC_AND_TYPE;
    uint64_t favorite_flags = 0u;
    uint32_t revision = 0u;
    if (metadata->type != PLR_JSON_NULL &&
        (!plr_field_u64(metadata, "magicAndType", &magic_and_type) ||
         !plr_field_u32(metadata, "revision", &revision) ||
         !plr_field_u64(metadata, "favoriteFlags", &favorite_flags))) {
        writer.ok = 0;
    }
    plr_writer_u64(&writer, magic_and_type);
    plr_writer_u32(&writer, revision);
    plr_writer_u64(&writer, favorite_flags);

    const char *name = NULL;
    int32_t i32 = 0;
    int64_t i64 = 0;
    uint8_t u8 = 0;
    int boolean = 0;
    if (!plr_field_string(root, "name", &name)) writer.ok = 0;
    plr_writer_string(&writer, name ? name : "");
    if (!plr_field_u8(root, "difficulty", &u8)) writer.ok = 0;
    plr_writer_u8(&writer, u8);
    if (!plr_field_i64(root, "playTimeTicks", &i64)) writer.ok = 0;
    plr_writer_i64(&writer, i64);
    if (!plr_field_i32(root, "hair", &i32)) writer.ok = 0;
    plr_writer_i32(&writer, i32);
    if (!plr_field_u8(root, "hairDye", &u8)) writer.ok = 0;
    plr_writer_u8(&writer, u8);
    if (!plr_field_u8(root, "team", &u8)) writer.ok = 0;
    plr_writer_u8(&writer, u8);

    const PlrJsonValue *hide = plr_field_array(root, "hideVisibleAccessory");
    uint8_t hide_lower = 0u, hide_upper = 0u;
    if (!hide || hide->as.array.count != PLR_DYE_SLOTS) writer.ok = 0;
    else for (uint32_t index = 0u; index < PLR_DYE_SLOTS; index++) {
        if (!plr_value_bool(hide->as.array.items[index], &boolean)) writer.ok = 0;
        else if (boolean && index < 8u) hide_lower |= (uint8_t)(1u << index);
        else if (boolean) hide_upper |= (uint8_t)(1u << (index - 8u));
    }
    plr_writer_u8(&writer, hide_lower);
    plr_writer_u8(&writer, hide_upper);
    const char *body_u8_fields[] = {"hideMisc", "skinVariant"};
    for (uint32_t index = 0u; index < 2u; index++) {
        if (!plr_field_u8(root, body_u8_fields[index], &u8)) writer.ok = 0;
        plr_writer_u8(&writer, u8);
    }
    const char *body_i32_fields[] = {"statLife", "statLifeMax", "statMana", "statManaMax"};
    for (uint32_t index = 0u; index < 4u; index++) {
        if (!plr_field_i32(root, body_i32_fields[index], &i32)) writer.ok = 0;
        plr_writer_i32(&writer, i32);
    }
    const char *body_bool_fields[] = {
        "extraAccessory", "unlockedBiomeTorches", "usingBiomeTorches",
        "ateArtisanBread", "usedAegisCrystal", "usedAegisFruit",
        "usedArcaneCrystal", "usedGalaxyPearl", "usedGummyWorm",
        "usedAmbrosia", "downedDd2EventAnyDifficulty"
    };
    for (uint32_t index = 0u; index < 11u; index++) {
        if (!plr_field_bool(root, body_bool_fields[index], &boolean)) writer.ok = 0;
        plr_writer_u8(&writer, boolean ? 1u : 0u);
    }
    const char *death_fields[] = {"taxMoney", "numberOfDeathsPve", "numberOfDeathsPvp"};
    for (uint32_t index = 0u; index < 3u; index++) {
        if (!plr_field_i32(root, death_fields[index], &i32)) writer.ok = 0;
        plr_writer_i32(&writer, i32);
    }
    const char *color_fields[] = {
        "hairColor", "skinColor", "eyeColor", "shirtColor",
        "underShirtColor", "pantsColor", "shoeColor"
    };
    for (uint32_t index = 0u; index < 7u; index++)
        plr_writer_color(&writer, root, color_fields[index]);

    plr_writer_item_array(&writer, root, "armor", PLR_ARMOR_SLOTS, 0, 1);
    plr_writer_item_array(&writer, root, "dyes", PLR_DYE_SLOTS, 0, 1);
    plr_writer_item_array(&writer, root, "inventory", PLR_INVENTORY_SLOTS, 1, 0);
    const PlrJsonValue *misc_equips = plr_field_array(root, "miscEquips");
    const PlrJsonValue *misc_dyes = plr_field_array(root, "miscDyes");
    if (!misc_equips || !misc_dyes || misc_equips->as.array.count != PLR_MISC_SLOTS ||
        misc_dyes->as.array.count != PLR_MISC_SLOTS) writer.ok = 0;
    else for (uint32_t index = 0u; index < PLR_MISC_SLOTS; index++) {
        plr_writer_type_prefix_item(&writer, misc_equips->as.array.items[index]);
        plr_writer_type_prefix_item(&writer, misc_dyes->as.array.items[index]);
    }
    plr_writer_item_array(&writer, root, "piggyBank", PLR_BANK_SLOTS, 0, 0);
    plr_writer_item_array(&writer, root, "safe", PLR_BANK_SLOTS, 0, 0);
    plr_writer_item_array(&writer, root, "defendersForge", PLR_BANK_SLOTS, 0, 0);
    plr_writer_item_array(&writer, root, "voidVault", PLR_BANK_SLOTS, 1, 0);
    if (!plr_field_u8(root, "voidVaultInfo", &u8)) writer.ok = 0;
    plr_writer_u8(&writer, u8);
    const PlrJsonValue *buffs = plr_field_array(root, "buffs");
    if (!buffs || buffs->as.array.count != PLR_BUFF_SLOTS) writer.ok = 0;
    else for (uint32_t index = 0u; index < PLR_BUFF_SLOTS; index++) {
        if (!plr_field_i32(buffs->as.array.items[index], "buffType", &i32)) writer.ok = 0;
        plr_writer_i32(&writer, i32);
        if (!plr_field_i32(buffs->as.array.items[index], "buffTime", &i32)) writer.ok = 0;
        plr_writer_i32(&writer, i32);
    }

    plr_writer_spawn_points(&writer, root);
    if (!plr_field_bool(root, "hbLocked", &boolean)) writer.ok = 0;
    plr_writer_u8(&writer, boolean ? 1u : 0u);
    plr_writer_bool_array(&writer, root, "hideInfo", PLR_HIDE_INFO_SLOTS);
    if (!plr_field_i32(root, "anglerQuestsFinished", &i32)) writer.ok = 0;
    plr_writer_i32(&writer, i32);
    plr_writer_i32_array(&writer, root, "dpadRadialBindings", PLR_DPAD_SLOTS);
    const PlrJsonValue *layout = plr_field(root, "tailLayout");
    uint32_t builder_count = 0u;
    if (!plr_field_u32(layout, "builderAccStatusCount", &builder_count)) writer.ok = 0;
    if (builder_count == 0u) builder_count = PLR_BUILDER_STATUS_SLOTS;
    plr_writer_i32_array(&writer, root, "builderAccStatus", builder_count);
    if (!plr_field_i32(root, "bartenderQuestLog", &i32)) writer.ok = 0;
    plr_writer_i32(&writer, i32);
    if (!plr_field_bool(root, "dead", &boolean)) writer.ok = 0;
    plr_writer_u8(&writer, boolean ? 1u : 0u);
    if (boolean) {
        const PlrJsonValue *respawn = plr_field(root, "respawnTimer");
        if (respawn->type == PLR_JSON_NULL) i32 = 0;
        else if (!plr_value_i32(respawn, &i32)) writer.ok = 0;
        plr_writer_i32(&writer, i32);
    }
    if (!plr_field_i64(root, "lastSaveUtcTicks", &i64)) writer.ok = 0;
    plr_writer_i64(&writer, i64);
    if (!plr_field_i32(root, "golferScoreAccumulated", &i32)) writer.ok = 0;
    plr_writer_i32(&writer, i32);
    plr_writer_sacrifices(&writer, root);
    plr_writer_temporary_slots(&writer, root);
    plr_writer_creative_powers(&writer, root);
    int unlocked = 0, enabled = 0;
    if (!plr_field_bool(root, "unlockedSuperCart", &unlocked) ||
        !plr_field_bool(root, "enabledSuperCart", &enabled)) writer.ok = 0;
    plr_writer_u8(&writer, (uint8_t)((unlocked ? 1u : 0u) | (enabled ? 2u : 0u)));
    if (!plr_field_i32(root, "currentLoadoutIndex", &i32)) writer.ok = 0;
    plr_writer_i32(&writer, i32);
    plr_writer_loadouts(&writer, root);
    if (!plr_field_u8(root, "voiceVariant", &u8)) writer.ok = 0;
    plr_writer_u8(&writer, u8);
    float voice_pitch = 0.0f;
    if (!plr_field_float(root, "voicePitchOffset", &voice_pitch)) writer.ok = 0;
    plr_writer_f32(&writer, voice_pitch);
    const PlrJsonValue *pending = plr_field_array(root, "pendingRefunds");
    if (!pending || pending->as.array.count > PLR_MAX_PENDING_REFUNDS ||
        pending->as.array.count > INT32_MAX) writer.ok = 0;
    else {
        plr_writer_i32(&writer, (int32_t)pending->as.array.count);
        for (uint32_t index = 0u; index < pending->as.array.count; index++)
            plr_writer_item(&writer, pending->as.array.items[index], 0);
    }
    const PlrJsonValue *dialogues = plr_field_array(root, "oneTimeDialoguesSeen");
    if (!dialogues || dialogues->as.array.count > PLR_MAX_DIALOGUES ||
        dialogues->as.array.count > INT32_MAX) writer.ok = 0;
    else {
        plr_writer_i32(&writer, (int32_t)dialogues->as.array.count);
        for (uint32_t index = 0u; index < dialogues->as.array.count; index++) {
            const char *dialogue = NULL;
            if (!plr_value_string(dialogues->as.array.items[index], &dialogue)) writer.ok = 0;
            plr_writer_string(&writer, dialogue ? dialogue : "");
        }
    }
    if (!writer.ok || writer.length == 0u) {
        free(writer.data);
        return NULL;
    }
    if (out_length) *out_length = writer.length;
    return writer.data;
}

/* -------------------------------------------------------------------------
 * RFC 6901 JSON Pointer editing
 * -------------------------------------------------------------------------
 */

static char *plr_decode_pointer_segment(
    const char *path, uint32_t start, uint32_t end) {
    if (!path || end < start || end - start > PLR_MAX_STRING_BYTES) return NULL;
    uint32_t length = end - start;
    char *segment = (char *)plr_malloc((size_t)length + 1u);
    if (!segment) return NULL;
    uint32_t out = 0u;
    for (uint32_t i = start; i < end; i++) {
        char c = path[i];
        if (c != '~') {
            segment[out++] = c;
            continue;
        }
        if (i + 1u >= end || (path[i + 1u] != '0' && path[i + 1u] != '1')) {
            free(segment);
            return NULL;
        }
        segment[out++] = path[++i] == '0' ? '~' : '/';
    }
    segment[out] = 0;
    return segment;
}

static PlrJsonValue **plr_json_slot(
    PlrJsonValue *parent, const char *segment) {
    if (!parent || !segment) return NULL;
    if (parent->type == PLR_JSON_OBJECT) {
        for (uint32_t i = 0u; i < parent->as.object.count; i++) {
            if (plr_cstring_equal(parent->as.object.members[i].key, segment))
                return &parent->as.object.members[i].value;
        }
        return NULL;
    }
    if (parent->type == PLR_JSON_ARRAY) {
        uint32_t length = 0u;
        while (segment[length]) length++;
        if (length == 0u || (length > 1u && segment[0] == '0')) return NULL;
        uint32_t index = 0u;
        for (uint32_t i = 0u; i < length; i++) {
            if (segment[i] < '0' || segment[i] > '9') return NULL;
            uint32_t digit = (uint32_t)(segment[i] - '0');
            if (index > (UINT32_MAX - digit) / 10u) return NULL;
            index = index * 10u + digit;
        }
        if (index >= parent->as.array.count) return NULL;
        return &parent->as.array.items[index];
    }
    return NULL;
}

static PlrJsonValue **plr_json_pointer_slot(
    PlrJsonValue *root, const char *path) {
    if (!root || !path) return NULL;
    if (path[0] == 0) return NULL; /* caller handles the document root */
    if (path[0] != '/') return NULL;
    uint32_t length = 0u;
    if (!plr_cstring_length_bounded(path, PLR_MAX_JSON_BYTES, &length)) return NULL;
    PlrJsonValue *current = root;
    uint32_t start = 1u;
    for (;;) {
        uint32_t end = start;
        while (end < length && path[end] != '/') end++;
        char *segment = plr_decode_pointer_segment(path, start, end);
        if (!segment) return NULL;
        PlrJsonValue **slot = plr_json_slot(current, segment);
        free(segment);
        if (!slot) return NULL;
        if (end == length) return slot;
        current = *slot;
        if (!current) return NULL;
        start = end + 1u;
    }
}

static const PlrJsonValue *plr_json_pointer_value(
    const PlrJsonValue *root, const char *path) {
    if (!root || !path) return NULL;
    if (path[0] == 0) return root;
    PlrJsonValue **slot = plr_json_pointer_slot((PlrJsonValue *)root, path);
    return slot ? *slot : NULL;
}

static int plr_json_pointer_replace(
    PlrJsonValue **root, const char *path, PlrJsonValue *replacement) {
    if (!root || !*root || !path || !replacement) return 0;
    if (path[0] == 0) {
        plr_json_free(*root);
        *root = replacement;
        return 1;
    }
    PlrJsonValue **slot = plr_json_pointer_slot(*root, path);
    if (!slot) return 0;
    plr_json_free(*slot);
    *slot = replacement;
    return 1;
}

/* -------------------------------------------------------------------------
 * Public document handles and API plumbing
 * -------------------------------------------------------------------------
 */

typedef struct PlrDocumentSlot {
    uint8_t active;
    uint32_t handle;
    PlrJsonValue *root;
    uint8_t *original_encrypted;
    uint32_t original_length;
    uint8_t dirty;
} PlrDocumentSlot;

static PlrDocumentSlot g_plr_documents[PLR_MAX_DOCUMENTS];
static uint32_t g_plr_generation = 1u;

static terrax_world_status plr_status_error(
    terrax_world_status status, const char *code, const char *message) {
    tx_set_error(code, message);
    return status;
}

static PlrDocumentSlot *plr_document(uint32_t handle) {
    uint32_t token = handle & 0xffu;
    if (!handle || token == 0u || token > PLR_MAX_DOCUMENTS) return NULL;
    PlrDocumentSlot *document = &g_plr_documents[token - 1u];
    return document->active && document->handle == handle ? document : NULL;
}

static terrax_world_status plr_invalid_handle(void) {
    return plr_status_error(
        TERRAX_WORLD_STATUS_NOT_FOUND,
        "TERRAX_INVALID_HANDLE",
        "player handle is stale or invalid");
}

static void plr_destroy_document(PlrDocumentSlot *document) {
    if (!document) return;
    plr_json_free(document->root);
    free(document->original_encrypted);
    memset(document, 0, sizeof(*document));
}

static PlrDocumentSlot *plr_claim_document(void) {
    for (uint32_t i = 0u; i < PLR_MAX_DOCUMENTS; i++) {
        if (!g_plr_documents[i].active) return &g_plr_documents[i];
    }
    return NULL;
}

static uint32_t plr_next_handle(uint32_t slot) {
    uint32_t generation = g_plr_generation++ & 0x00ffffffu;
    if (generation == 0u) {
        generation = 1u;
        g_plr_generation = 2u;
    }
    return (generation << 8u) | (slot + 1u);
}

static void plr_normalize_metadata_magic(PlrJsonValue *root);

static terrax_world_status plr_commit_root(
    PlrDocumentSlot *document, PlrJsonValue *root) {
    if (!document || !root) return TERRAX_WORLD_STATUS_INTERNAL_ERROR;
    plr_normalize_metadata_magic(root);
    if (!plr_validate_model(root)) return TERRAX_WORLD_STATUS_VALIDATION_ERROR;
    plr_json_free(document->root);
    document->root = root;
    free(document->original_encrypted);
    document->original_encrypted = NULL;
    document->original_length = 0u;
    document->dirty = 1u;
    tx_clear_error();
    return TERRAX_WORLD_STATUS_OK;
}

static terrax_world_status plr_parse_error(void) {
    return plr_status_error(
        g_plr_oom ? TERRAX_WORLD_STATUS_INTERNAL_ERROR : TERRAX_WORLD_STATUS_PARSE_ERROR,
        g_plr_oom ? "TERRAX_WASM_OOM" : "TERRAX_PLR_PARSE_ERROR",
        g_plr_oom ? "out of memory while parsing PLR" : "invalid or unsupported PLR payload");
}

static terrax_world_status plr_json_error(void) {
    return plr_status_error(
        g_plr_oom ? TERRAX_WORLD_STATUS_INTERNAL_ERROR : TERRAX_WORLD_STATUS_VALIDATION_ERROR,
        g_plr_oom ? "TERRAX_WASM_OOM" : "TERRAX_PLR_JSON_ERROR",
        g_plr_oom ? "out of memory while parsing PLR JSON" : "invalid PLR JSON");
}

static terrax_world_status plr_open_parsed(
    PlrJsonValue *root, uint8_t *original, uint32_t original_length,
    uint32_t *out_handle) {
    PlrDocumentSlot *document = plr_claim_document();
    if (!document) {
        plr_json_free(root);
        free(original);
        return plr_status_error(
            TERRAX_WORLD_STATUS_STATE_ERROR,
            "TERRAX_PLR_HANDLE_LIMIT",
            "too many open player documents");
    }
    uint32_t slot = (uint32_t)(document - g_plr_documents);
    memset(document, 0, sizeof(*document));
    document->root = root;
    document->original_encrypted = original;
    document->original_length = original_length;
    document->dirty = original == NULL ? 1u : 0u;
    document->handle = plr_next_handle(slot);
    document->active = 1u;
    *out_handle = document->handle;
    tx_clear_error();
    return TERRAX_WORLD_STATUS_OK;
}

static terrax_world_status plr_open_from_encrypted(
    const uint8_t *buffer, uint32_t buffer_length, uint32_t *out_handle) {
    g_plr_oom = 0;
    uint8_t *original = (uint8_t *)plr_malloc(buffer_length);
    if (!original) return plr_parse_error();
    memcpy(original, buffer, buffer_length);
    uint32_t plain_length = 0u;
    uint8_t *plain = plr_decrypt(buffer, buffer_length, &plain_length);
    if (!plain) {
        free(original);
        return plr_parse_error();
    }
    PlrJsonValue *root = plr_parse_plain(plain, plain_length);
    free(plain);
    if (!root) {
        free(original);
        return plr_parse_error();
    }
    return plr_open_parsed(root, original, buffer_length, out_handle);
}

static terrax_world_status plr_copy_json_result(
    const PlrJsonValue *value,
    char *buffer,
    uint64_t buffer_size,
    uint32_t *required_size) {
    if (!required_size) {
        return plr_status_error(
            TERRAX_WORLD_STATUS_INVALID_ARGUMENT,
            "TERRAX_INVALID_ARGUMENT",
            "null required-size pointer");
    }
    uint32_t length = 0u;
    uint8_t *json = plr_json_serialize(value, &length);
    if (!json) {
        return plr_status_error(
            TERRAX_WORLD_STATUS_INTERNAL_ERROR,
            "TERRAX_WASM_OOM",
            "failed to serialize PLR JSON");
    }
    uint64_t needed = (uint64_t)length + 1u;
    if (needed > UINT32_MAX) {
        free(json);
        return plr_status_error(
            TERRAX_WORLD_STATUS_INTERNAL_ERROR,
            "TERRAX_WASM_OOM",
            "PLR JSON output exceeds the ABI size limit");
    }
    *required_size = (uint32_t)needed;
    if (!buffer || buffer_size == 0u) {
        free(json);
        tx_clear_error();
        return TERRAX_WORLD_STATUS_OK;
    }
    if (buffer_size < needed) {
        free(json);
        return plr_status_error(
            TERRAX_WORLD_STATUS_BUFFER_TOO_SMALL,
            "TERRAX_BUFFER_TOO_SMALL",
            "PLR JSON output buffer is too small");
    }
    memcpy(buffer, json, (unsigned long)needed);
    free(json);
    tx_clear_error();
    return TERRAX_WORLD_STATUS_OK;
}

static uint8_t *plr_encoded_document(
    const PlrDocumentSlot *document, uint32_t *out_length,
    terrax_world_status *out_status) {
    if (out_length) *out_length = 0u;
    if (out_status) *out_status = TERRAX_WORLD_STATUS_OK;
    if (!document || !document->root) {
        if (out_status) *out_status = plr_status_error(
            TERRAX_WORLD_STATUS_STATE_ERROR,
            "TERRAX_PLR_STATE_ERROR", "PLR document has no model");
        return NULL;
    }
    g_plr_oom = 0;
    if (!document->dirty && document->original_encrypted) {
        uint8_t *copy = (uint8_t *)plr_malloc(document->original_length);
        if (!copy) {
            if (out_status) *out_status = plr_status_error(
                TERRAX_WORLD_STATUS_INTERNAL_ERROR, "TERRAX_WASM_OOM",
                "failed to allocate PLR output");
            return NULL;
        }
        memcpy(copy, document->original_encrypted, document->original_length);
        if (out_length) *out_length = document->original_length;
        return copy;
    }
    g_plr_oom = 0;
    uint32_t plain_length = 0u;
    uint8_t *plain = plr_encode_plain(document->root, &plain_length);
    if (!plain) {
        if (out_status) *out_status = g_plr_oom ?
            plr_status_error(TERRAX_WORLD_STATUS_INTERNAL_ERROR, "TERRAX_WASM_OOM", "out of memory while encoding PLR") :
            TERRAX_WORLD_STATUS_VALIDATION_ERROR;
        free(plain);
        return NULL;
    }
    uint32_t encrypted_length = 0u;
    uint8_t *encrypted = plr_encrypt(plain, plain_length, &encrypted_length);
    free(plain);
    if (!encrypted) {
        if (out_status) *out_status = plr_status_error(
            TERRAX_WORLD_STATUS_INTERNAL_ERROR, "TERRAX_WASM_OOM",
            "failed to encrypt PLR output");
        return NULL;
    }
    if (out_length) *out_length = encrypted_length;
    return encrypted;
}

/* -------------------------------------------------------------------------
 * File helpers and exported C ABI
 * -------------------------------------------------------------------------
 */

static int plr_read_file(
    const char *path, uint8_t **out_data, uint32_t *out_length) {
    if (!path || !out_data || !out_length) return 0;
    *out_data = NULL;
    *out_length = 0u;
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    long size = ftell(file);
    if (size <= 0 || (uint64_t)size > PLR_MAX_FILE_BYTES ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    uint8_t *data = (uint8_t *)plr_malloc((size_t)size);
    if (!data) {
        fclose(file);
        return 0;
    }
    size_t read = fread(data, 1u, (size_t)size, file);
    fclose(file);
    if (read != (size_t)size) {
        free(data);
        return 0;
    }
    *out_data = data;
    *out_length = (uint32_t)size;
    return 1;
}

static int plr_write_file(
    const char *path, const uint8_t *data, uint32_t length) {
    if (!path || (!data && length)) return 0;
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    size_t written = fwrite(data, 1u, length, file);
    int ok = written == length && fflush(file) == 0;
    if (fclose(file) != 0) ok = 0;
    return ok;
}

terrax_world_status terra_plr_open_from_buffer(
    const uint8_t *buffer, uint32_t buffer_len, uint32_t *out_handle) {
    if (out_handle) *out_handle = 0u;
    if (!buffer || !out_handle || buffer_len == 0u) {
        return plr_status_error(
            TERRAX_WORLD_STATUS_INVALID_ARGUMENT,
            "TERRAX_INVALID_ARGUMENT",
            "null or empty PLR buffer");
    }
    if (buffer_len > PLR_MAX_FILE_BYTES) {
        return plr_status_error(
            TERRAX_WORLD_STATUS_PARSE_ERROR,
            "TERRAX_PLR_PARSE_ERROR",
            "PLR buffer exceeds the supported size limit");
    }
    return plr_open_from_encrypted(buffer, buffer_len, out_handle);
}

terrax_world_status terra_plr_open(
    const char *path_utf8, uint32_t *out_handle) {
    if (out_handle) *out_handle = 0u;
    uint32_t path_length = 0u;
    if (!path_utf8 || !out_handle ||
        !plr_cstring_length_bounded(path_utf8, PLR_MAX_JSON_BYTES, &path_length) ||
        path_length == 0u) {
        return plr_status_error(
            TERRAX_WORLD_STATUS_INVALID_ARGUMENT,
            "TERRAX_INVALID_ARGUMENT",
            "null, empty, or unterminated PLR path");
    }
    g_plr_oom = 0;
    uint8_t *data = NULL;
    uint32_t length = 0u;
    if (!plr_read_file(path_utf8, &data, &length)) {
        return plr_status_error(
            g_plr_oom ? TERRAX_WORLD_STATUS_INTERNAL_ERROR : TERRAX_WORLD_STATUS_IO_ERROR,
            g_plr_oom ? "TERRAX_WASM_OOM" : "TERRAX_IO_ERROR",
            g_plr_oom ? "out of memory while reading PLR" : "failed to read PLR file");
    }
    terrax_world_status status = terra_plr_open_from_buffer(data, length, out_handle);
    free(data);
    return status;
}

terrax_world_status terra_plr_open_json(
    const char *json_utf8, uint32_t *out_handle) {
    if (out_handle) *out_handle = 0u;
    if (!json_utf8 || !out_handle) {
        return plr_status_error(
            TERRAX_WORLD_STATUS_INVALID_ARGUMENT,
            "TERRAX_INVALID_ARGUMENT",
            "null PLR JSON or output pointer");
    }
    g_plr_oom = 0;
    PlrJsonValue *root = plr_json_parse_text(json_utf8);
    if (!root) return plr_json_error();
    plr_normalize_metadata_magic(root);
    if (!plr_validate_model(root)) {
        plr_json_free(root);
        return TERRAX_WORLD_STATUS_VALIDATION_ERROR;
    }
    return plr_open_parsed(root, NULL, 0u, out_handle);
}

terrax_world_status terra_plr_close(uint32_t handle) {
    PlrDocumentSlot *document = plr_document(handle);
    if (!document) return plr_invalid_handle();
    plr_destroy_document(document);
    tx_clear_error();
    return TERRAX_WORLD_STATUS_OK;
}

terrax_world_status terra_plr_validate_handle(uint32_t handle) {
    if (!plr_document(handle)) return plr_invalid_handle();
    tx_clear_error();
    return TERRAX_WORLD_STATUS_OK;
}

terrax_world_status terra_plr_get_json(
    uint32_t handle, char *buffer, uint64_t buffer_size, uint32_t *required_size) {
    PlrDocumentSlot *document = plr_document(handle);
    if (!document) return plr_invalid_handle();
    g_plr_oom = 0;
    return plr_copy_json_result(document->root, buffer, buffer_size, required_size);
}

terrax_world_status terra_plr_get(
    uint32_t handle, const char *pointer_utf8, char *buffer,
    uint64_t buffer_size, uint32_t *required_size) {
    PlrDocumentSlot *document = plr_document(handle);
    if (!document) return plr_invalid_handle();
    uint32_t pointer_length = 0u;
    if (!pointer_utf8 ||
        !plr_cstring_length_bounded(pointer_utf8, PLR_MAX_JSON_BYTES, &pointer_length)) {
        return plr_status_error(
            TERRAX_WORLD_STATUS_INVALID_ARGUMENT,
            "TERRAX_INVALID_ARGUMENT",
            "null or unterminated PLR JSON pointer");
    }
    (void)pointer_length;
    if (pointer_utf8[0] != 0u && pointer_utf8[0] != '/') {
        return plr_status_error(
            TERRAX_WORLD_STATUS_NOT_FOUND,
            "TERRAX_PLR_FIELD_NOT_FOUND",
            "PLR JSON pointer must be empty or start with '/'");
    }
    g_plr_oom = 0;
    const PlrJsonValue *value = plr_json_pointer_value(document->root, pointer_utf8);
    if (!value) {
        return g_plr_oom ?
            plr_status_error(TERRAX_WORLD_STATUS_INTERNAL_ERROR, "TERRAX_WASM_OOM",
                "out of memory while resolving PLR JSON pointer") :
            plr_status_error(
                TERRAX_WORLD_STATUS_NOT_FOUND,
                "TERRAX_PLR_FIELD_NOT_FOUND",
                "PLR JSON pointer was not found");
    }
    g_plr_oom = 0;
    return plr_copy_json_result(value, buffer, buffer_size, required_size);
}

terrax_world_status terra_plr_get_field_json(
    uint32_t handle, const char *pointer_utf8, char *buffer,
    uint64_t buffer_size, uint32_t *required_size) {
    return terra_plr_get(handle, pointer_utf8, buffer, buffer_size, required_size);
}

terrax_world_status terra_plr_replace_json(
    uint32_t handle, const char *json_utf8) {
    PlrDocumentSlot *document = plr_document(handle);
    if (!document) return plr_invalid_handle();
    if (!json_utf8) {
        return plr_status_error(
            TERRAX_WORLD_STATUS_INVALID_ARGUMENT,
            "TERRAX_INVALID_ARGUMENT",
            "null PLR JSON");
    }
    g_plr_oom = 0;
    PlrJsonValue *root = plr_json_parse_text(json_utf8);
    if (!root) return plr_json_error();
    terrax_world_status status = plr_commit_root(document, root);
    if (status != TERRAX_WORLD_STATUS_OK) plr_json_free(root);
    return status;
}

terrax_world_status terra_plr_set(
    uint32_t handle, const char *pointer_utf8, const char *value_json_utf8) {
    PlrDocumentSlot *document = plr_document(handle);
    if (!document) return plr_invalid_handle();
    uint32_t pointer_length = 0u;
    if (!pointer_utf8 || !value_json_utf8 ||
        !plr_cstring_length_bounded(pointer_utf8, PLR_MAX_JSON_BYTES, &pointer_length)) {
        return plr_status_error(
            TERRAX_WORLD_STATUS_INVALID_ARGUMENT,
            "TERRAX_INVALID_ARGUMENT",
            "null or unterminated PLR pointer/value JSON");
    }
    (void)pointer_length;
    g_plr_oom = 0;
    PlrJsonValue *value = plr_json_parse_text(value_json_utf8);
    if (!value) return plr_status_error(
        TERRAX_WORLD_STATUS_VALIDATION_ERROR,
        g_plr_oom ? "TERRAX_WASM_OOM" : "TERRAX_PLR_JSON_ERROR",
        g_plr_oom ? "out of memory while parsing PLR value" : "invalid PLR value JSON");
    PlrJsonValue *candidate = plr_json_clone(document->root);
    if (!candidate) {
        plr_json_free(value);
        return plr_status_error(
            TERRAX_WORLD_STATUS_INTERNAL_ERROR,
            "TERRAX_WASM_OOM",
            "out of memory while copying PLR model");
    }
    if (!plr_json_pointer_replace(&candidate, pointer_utf8, value)) {
        plr_json_free(value);
        plr_json_free(candidate);
        return g_plr_oom ?
            plr_status_error(TERRAX_WORLD_STATUS_INTERNAL_ERROR, "TERRAX_WASM_OOM",
                "out of memory while resolving PLR JSON pointer") :
            plr_status_error(
                TERRAX_WORLD_STATUS_VALIDATION_ERROR,
                "TERRAX_PLR_FIELD_NOT_FOUND",
                "PLR JSON pointer was not found");
    }
    terrax_world_status status = plr_commit_root(document, candidate);
    if (status != TERRAX_WORLD_STATUS_OK) plr_json_free(candidate);
    return status;
}

terrax_world_status terra_plr_set_many(
    uint32_t handle, const char *edits_json_utf8) {
    PlrDocumentSlot *document = plr_document(handle);
    if (!document) return plr_invalid_handle();
    if (!edits_json_utf8) {
        return plr_status_error(
            TERRAX_WORLD_STATUS_INVALID_ARGUMENT,
            "TERRAX_INVALID_ARGUMENT",
            "null PLR edits JSON");
    }
    g_plr_oom = 0;
    PlrJsonValue *edits = plr_json_parse_text(edits_json_utf8);
    if (!edits) return plr_json_error();
    if (edits->type != PLR_JSON_ARRAY) {
        plr_json_free(edits);
        return plr_status_error(
            TERRAX_WORLD_STATUS_VALIDATION_ERROR,
            "TERRAX_PLR_JSON_ERROR",
            "PLR setMany value must be an array");
    }
    PlrJsonValue *candidate = plr_json_clone(document->root);
    if (!candidate) {
        plr_json_free(edits);
        return plr_status_error(
            TERRAX_WORLD_STATUS_INTERNAL_ERROR,
            "TERRAX_WASM_OOM",
            "out of memory while copying PLR model");
    }
    for (uint32_t i = 0u; i < edits->as.array.count; i++) {
        const PlrJsonValue *edit = edits->as.array.items[i];
        const char *path = NULL;
        const PlrJsonValue *value = NULL;
        if (!edit || edit->type != PLR_JSON_OBJECT ||
            !plr_value_string(plr_json_object_get(edit, "path"), &path) ||
            !(value = plr_json_object_get(edit, "value"))) {
            plr_json_free(candidate);
            plr_json_free(edits);
            return plr_status_error(
                TERRAX_WORLD_STATUS_VALIDATION_ERROR,
                "TERRAX_PLR_JSON_ERROR",
                "each PLR edit must contain path and value");
        }
        PlrJsonValue *replacement = plr_json_clone(value);
        if (!replacement || !plr_json_pointer_replace(&candidate, path, replacement)) {
            plr_json_free(replacement);
            plr_json_free(candidate);
            plr_json_free(edits);
            return g_plr_oom ?
                plr_status_error(TERRAX_WORLD_STATUS_INTERNAL_ERROR, "TERRAX_WASM_OOM",
                    "out of memory while applying PLR JSON edits") :
                plr_status_error(
                    TERRAX_WORLD_STATUS_VALIDATION_ERROR,
                    "TERRAX_PLR_FIELD_NOT_FOUND",
                    "one PLR JSON pointer was not found");
        }
    }
    plr_json_free(edits);
    terrax_world_status status = plr_commit_root(document, candidate);
    if (status != TERRAX_WORLD_STATUS_OK) plr_json_free(candidate);
    return status;
}

terrax_world_status terra_plr_set_field_json(
    uint32_t handle, const char *pointer_utf8, const char *value_json_utf8) {
    return terra_plr_set(handle, pointer_utf8, value_json_utf8);
}

terrax_world_status terra_plr_set_json(
    uint32_t handle, const char *json_utf8) {
    return terra_plr_replace_json(handle, json_utf8);
}

terrax_world_status terra_plr_save_to_buffer(
    uint32_t handle, uint8_t *output, uint32_t capacity, uint32_t *out_required) {
    PlrDocumentSlot *document = plr_document(handle);
    if (!document) return plr_invalid_handle();
    if (!out_required) {
        return plr_status_error(
            TERRAX_WORLD_STATUS_INVALID_ARGUMENT,
            "TERRAX_INVALID_ARGUMENT",
            "null PLR required-size pointer");
    }
    *out_required = 0u;
    terrax_world_status status = TERRAX_WORLD_STATUS_OK;
    uint32_t length = 0u;
    uint8_t *encoded = plr_encoded_document(document, &length, &status);
    if (!encoded) return status;
    *out_required = length;
    if (!output || capacity == 0u) {
        free(encoded);
        tx_clear_error();
        return TERRAX_WORLD_STATUS_OK;
    }
    if (capacity < length) {
        free(encoded);
        return plr_status_error(
            TERRAX_WORLD_STATUS_BUFFER_TOO_SMALL,
            "TERRAX_BUFFER_TOO_SMALL",
            "PLR output buffer is too small");
    }
    memcpy(output, encoded, length);
    free(encoded);
    tx_clear_error();
    return TERRAX_WORLD_STATUS_OK;
}

terrax_world_status terra_plr_encode(
    uint32_t handle, uint8_t *output, uint32_t capacity, uint32_t *out_required) {
    return terra_plr_save_to_buffer(handle, output, capacity, out_required);
}

terrax_world_status terra_plr_save(
    uint32_t handle, const char *path_utf8) {
    PlrDocumentSlot *document = plr_document(handle);
    if (!document) return plr_invalid_handle();
    uint32_t path_length = 0u;
    if (!path_utf8 ||
        !plr_cstring_length_bounded(path_utf8, PLR_MAX_JSON_BYTES, &path_length) ||
        path_length == 0u) {
        return plr_status_error(
            TERRAX_WORLD_STATUS_INVALID_ARGUMENT,
            "TERRAX_INVALID_ARGUMENT",
            "null, empty, or unterminated PLR path");
    }
    terrax_world_status status = TERRAX_WORLD_STATUS_OK;
    uint32_t length = 0u;
    uint8_t *encoded = plr_encoded_document(document, &length, &status);
    if (!encoded) return status;
    int ok = plr_write_file(path_utf8, encoded, length);
    free(encoded);
    if (!ok) return plr_status_error(
        TERRAX_WORLD_STATUS_IO_ERROR,
        "TERRAX_IO_ERROR",
        "failed to write PLR file");
    tx_clear_error();
    return TERRAX_WORLD_STATUS_OK;
}

/* Structured patch compatibility used by TerraR's player editor. */
static const char *plr_patch_section(const char *name) {
    if (!name) return NULL;
    if (plr_cstring_equal(name, "inventory") || plr_cstring_equal(name, "Inventory")) return "inventory";
    if (plr_cstring_equal(name, "piggyBank") || plr_cstring_equal(name, "PiggyBank")) return "piggyBank";
    if (plr_cstring_equal(name, "safe") || plr_cstring_equal(name, "Safe")) return "safe";
    if (plr_cstring_equal(name, "defendersForge") || plr_cstring_equal(name, "DefendersForge")) return "defendersForge";
    if (plr_cstring_equal(name, "voidVault") || plr_cstring_equal(name, "VoidVault")) return "voidVault";
    if (plr_cstring_equal(name, "armor") || plr_cstring_equal(name, "Armor")) return "armor";
    if (plr_cstring_equal(name, "dyes") || plr_cstring_equal(name, "Dyes")) return "dyes";
    if (plr_cstring_equal(name, "miscEquips") || plr_cstring_equal(name, "MiscEquips")) return "miscEquips";
    if (plr_cstring_equal(name, "miscDyes") || plr_cstring_equal(name, "MiscDyes")) return "miscDyes";
    return NULL;
}

static char *plr_pointer_index(const char *section, uint32_t index) {
    char text[128];
    int length = snprintf(text, sizeof(text), "/%s/%u", section, index);
    if (length <= 0 || length >= (int)sizeof(text)) return NULL;
    return plr_dup_bytes(text, (size_t)length);
}

static char *plr_pointer_loadout(
    uint32_t loadout, const char *section, uint32_t index) {
    char text[128];
    int length = snprintf(text, sizeof(text), "/loadouts/%u/%s/%u",
        loadout, section, index);
    if (length <= 0 || length >= (int)sizeof(text)) return NULL;
    return plr_dup_bytes(text, (size_t)length);
}

static char *plr_camel_name(const char *name) {
    if (!name) return NULL;
    uint32_t length = tx_strlen(name);
    char *result = (char *)plr_malloc((size_t)length + 1u);
    if (!result) return NULL;
    uint32_t out = 0u;
    int capitalize = 0;
    for (uint32_t i = 0u; i < length; i++) {
        char c = name[i];
        if (c == '_') {
            capitalize = 1;
            continue;
        }
        if (capitalize && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        capitalize = 0;
        result[out++] = c;
    }
    result[out] = 0;
    return result;
}

static char *plr_field_pointer(const char *name) {
    if (!name) return NULL;
    uint32_t length = tx_strlen(name);
    uint32_t required = 1u;
    for (uint32_t i = 0u; i < length; i++) {
        if (name[i] == '~' || name[i] == '/') {
            if (required == UINT32_MAX) return NULL;
            required++;
        }
        if (required == UINT32_MAX && i + 1u < length) return NULL;
        required++;
    }
    char *path = (char *)plr_malloc((size_t)required + 1u);
    if (!path) return NULL;
    uint32_t out = 0u;
    path[out++] = '/';
    for (uint32_t i = 0u; i < length; i++) {
        if (name[i] == '~') {
            path[out++] = '~'; path[out++] = '0';
        } else if (name[i] == '/') {
            path[out++] = '~'; path[out++] = '1';
        } else {
            path[out++] = name[i];
        }
    }
    path[out] = 0;
    return path;
}

static int plr_patch_i32(
    const PlrJsonValue *object, const char *key, int32_t default_value, int32_t *out) {
    const PlrJsonValue *value = plr_json_object_get(object, key);
    if (!value) {
        *out = default_value;
        return 1;
    }
    return plr_value_i32(value, out);
}

static int plr_patch_u8(
    const PlrJsonValue *object, const char *key, uint8_t default_value, uint8_t *out) {
    const PlrJsonValue *value = plr_json_object_get(object, key);
    if (!value) {
        *out = default_value;
        return 1;
    }
    return plr_value_u8(value, out);
}

static int plr_patch_bool(
    const PlrJsonValue *object, const char *key, int default_value, int *out) {
    const PlrJsonValue *value = plr_json_object_get(object, key);
    if (!value) {
        *out = default_value;
        return 1;
    }
    return plr_value_bool(value, out);
}

static int plr_patch_nullable_i32(
    const PlrJsonValue *object, const char *key, int32_t default_value, int32_t *out) {
    const PlrJsonValue *value = plr_json_object_get(object, key);
    if (!value || value->type == PLR_JSON_NULL) {
        *out = default_value;
        return 1;
    }
    return plr_value_i32(value, out);
}

static int plr_patch_nullable_u8(
    const PlrJsonValue *object, const char *key, uint8_t default_value, uint8_t *out) {
    const PlrJsonValue *value = plr_json_object_get(object, key);
    if (!value || value->type == PLR_JSON_NULL) {
        *out = default_value;
        return 1;
    }
    return plr_value_u8(value, out);
}

static int plr_patch_nullable_bool(
    const PlrJsonValue *object, const char *key, int default_value, int *out) {
    const PlrJsonValue *value = plr_json_object_get(object, key);
    if (!value || value->type == PLR_JSON_NULL) {
        *out = default_value;
        return 1;
    }
    return plr_value_bool(value, out);
}

static terrax_world_status plr_patch_invalid(const char *message) {
    return plr_status_error(
        TERRAX_WORLD_STATUS_VALIDATION_ERROR,
        "TERRAX_PLR_VALIDATION_ERROR", message);
}

static terrax_world_status plr_patch_allocation_or_invalid(const char *message) {
    return g_plr_oom ?
        plr_status_error(TERRAX_WORLD_STATUS_INTERNAL_ERROR, "TERRAX_WASM_OOM",
            "out of memory while applying PLR patch") :
        plr_patch_invalid(message);
}

static terrax_world_status plr_patch_apply(
    PlrJsonValue **candidate, const PlrJsonValue *patch) {
    if (!candidate || !*candidate || !patch || patch->type != PLR_JSON_OBJECT)
        return plr_patch_invalid("PLR patch must be an object");

    const PlrJsonValue *fields = plr_json_object_get(patch, "fields");
    if (fields) {
        if (fields->type != PLR_JSON_OBJECT)
            return plr_patch_invalid("PLR patch fields must be an object");
        for (uint32_t i = 0u; i < fields->as.object.count; i++) {
            char *name = plr_camel_name(fields->as.object.members[i].key);
            if (!name) return plr_status_error(
                TERRAX_WORLD_STATUS_INTERNAL_ERROR, "TERRAX_WASM_OOM", "out of memory for PLR patch");
            char *path = plr_field_pointer(name);
            free(name);
            if (!path) return plr_patch_allocation_or_invalid(
                "PLR patch field name is too long");
            PlrJsonValue *replacement = plr_json_clone(fields->as.object.members[i].value);
            if (!replacement || !plr_json_pointer_replace(candidate, path, replacement)) {
                plr_json_free(replacement);
                free(path);
                return plr_patch_allocation_or_invalid(
                    "PLR patch field was not found");
            }
            free(path);
        }
    }

    const PlrJsonValue *items = plr_json_object_get(patch, "items");
    if (items) {
        if (items->type != PLR_JSON_ARRAY)
            return plr_patch_invalid("PLR patch items must be an array");
        for (uint32_t i = 0u; i < items->as.array.count; i++) {
            const PlrJsonValue *entry = items->as.array.items[i];
            const char *section_name = NULL;
            uint32_t index = 0u;
            int32_t item_type = 0, stack = 1;
            uint8_t prefix = 0;
            int favorited = 0;
            if (!entry || entry->type != PLR_JSON_OBJECT ||
                !plr_value_string(plr_json_object_get(entry, "section"), &section_name) ||
                !plr_value_u32(plr_json_object_get(entry, "index"), &index) ||
                !plr_value_i32(plr_json_object_get(entry, "itemType"), &item_type) ||
                !plr_patch_i32(entry, "stack", 1, &stack) ||
                !plr_patch_u8(entry, "prefix", 0, &prefix) ||
                !plr_patch_bool(entry, "favorited", 0, &favorited))
                return plr_patch_invalid("PLR item patch is invalid");
            const char *section = plr_patch_section(section_name);
            if (!section) return plr_patch_invalid("PLR item section is invalid");
            char *path = plr_pointer_index(section, index);
            PlrJsonValue *replacement = plr_make_item(item_type, stack, prefix, favorited);
            if (!path || !replacement || !plr_json_pointer_replace(candidate, path, replacement)) {
                free(path);
                plr_json_free(replacement);
                return plr_patch_allocation_or_invalid(
                    "PLR item patch index is out of range");
            }
            free(path);
        }
    }

    const PlrJsonValue *buffs = plr_json_object_get(patch, "buffs");
    if (buffs) {
        if (buffs->type != PLR_JSON_ARRAY)
            return plr_patch_invalid("PLR patch buffs must be an array");
        for (uint32_t i = 0u; i < buffs->as.array.count; i++) {
            const PlrJsonValue *entry = buffs->as.array.items[i];
            uint32_t index = 0u;
            int32_t type = 0, time = 0;
            if (!entry || entry->type != PLR_JSON_OBJECT ||
                !plr_value_u32(plr_json_object_get(entry, "index"), &index) ||
                !plr_value_i32(plr_json_object_get(entry, "buffType"), &type) ||
                !plr_value_i32(plr_json_object_get(entry, "buffTime"), &time))
                return plr_patch_invalid("PLR buff patch is invalid");
            char *path = plr_pointer_index("buffs", index);
            PlrJsonValue *replacement = plr_json_object();
            if (!path || !replacement ||
                !plr_json_object_put_i64(replacement, "buffTime", time) ||
                !plr_json_object_put_i64(replacement, "buffType", type) ||
                !plr_json_pointer_replace(candidate, path, replacement)) {
                free(path);
                plr_json_free(replacement);
                return plr_patch_allocation_or_invalid(
                    "PLR buff patch index is out of range");
            }
            free(path);
        }
    }

    const PlrJsonValue *slots = plr_json_object_get(patch, "loadoutSlots");
    if (slots) {
        if (slots->type != PLR_JSON_ARRAY)
            return plr_patch_invalid("PLR loadoutSlots must be an array");
        for (uint32_t i = 0u; i < slots->as.array.count; i++) {
            const PlrJsonValue *entry = slots->as.array.items[i];
            const char *kind_name = NULL;
            uint32_t loadout = 0u, index = 0u;
            if (!entry || entry->type != PLR_JSON_OBJECT ||
                !plr_value_u32(plr_json_object_get(entry, "loadoutIndex"), &loadout) ||
                !plr_value_string(plr_json_object_get(entry, "slotKind"), &kind_name) ||
                !plr_value_u32(plr_json_object_get(entry, "slotIndex"), &index) ||
                loadout >= PLR_LOADOUTS)
                return plr_patch_invalid("PLR loadout coordinates are invalid");
            const char *kind = NULL;
            int is_hide = 0;
            if (plr_cstring_equal(kind_name, "Armor") || plr_cstring_equal(kind_name, "armor")) kind = "armor";
            else if (plr_cstring_equal(kind_name, "Dye") || plr_cstring_equal(kind_name, "dye")) kind = "dyes";
            else if (plr_cstring_equal(kind_name, "Hide") || plr_cstring_equal(kind_name, "hide")) {
                kind = "hide";
                is_hide = 1;
            } else return plr_patch_invalid("PLR loadout slot kind is invalid");
            if (index >= (is_hide || plr_cstring_equal(kind, "dyes") ? PLR_DYE_SLOTS : PLR_ARMOR_SLOTS))
                return plr_patch_invalid("PLR loadout slot index is invalid");
            char *path = plr_pointer_loadout(loadout, kind, index);
            PlrJsonValue *replacement = NULL;
            if (is_hide) {
                int value = 0;
                if (!plr_patch_nullable_bool(entry, "hide", 0, &value)) {
                    free(path);
                    return plr_patch_invalid("PLR loadout hide value is invalid");
                }
                replacement = plr_json_bool(value);
            } else {
                int32_t item_type = 0, stack = 1;
                uint8_t prefix = 0;
                if (!plr_patch_nullable_i32(entry, "itemType", 0, &item_type) ||
                    !plr_patch_nullable_i32(entry, "stack", 1, &stack) ||
                    !plr_patch_nullable_u8(entry, "prefix", 0, &prefix)) {
                    free(path);
                    return plr_patch_invalid("PLR loadout item value is invalid");
                }
                replacement = plr_make_item(item_type, stack, prefix, 0);
            }
            if (!path || !replacement || !plr_json_pointer_replace(candidate, path, replacement)) {
                free(path);
                plr_json_free(replacement);
                return plr_patch_allocation_or_invalid(
                    "PLR loadout slot is invalid");
            }
            free(path);
        }
    }
    return TERRAX_WORLD_STATUS_OK;
}

terrax_world_status terra_plr_apply_patch_json(
    uint32_t handle, const char *patch_json_utf8) {
    PlrDocumentSlot *document = plr_document(handle);
    if (!document) return plr_invalid_handle();
    if (!patch_json_utf8) return plr_status_error(
        TERRAX_WORLD_STATUS_INVALID_ARGUMENT, "TERRAX_INVALID_ARGUMENT", "null PLR patch JSON");
    g_plr_oom = 0;
    PlrJsonValue *patch = plr_json_parse_text(patch_json_utf8);
    if (!patch) return plr_json_error();
    PlrJsonValue *candidate = plr_json_clone(document->root);
    if (!candidate) {
        plr_json_free(patch);
        return plr_status_error(TERRAX_WORLD_STATUS_INTERNAL_ERROR, "TERRAX_WASM_OOM", "out of memory while copying PLR model");
    }
    terrax_world_status status = plr_patch_apply(&candidate, patch);
    plr_json_free(patch);
    if (status != TERRAX_WORLD_STATUS_OK) {
        plr_json_free(candidate);
        return status;
    }
    status = plr_commit_root(document, candidate);
    if (status != TERRAX_WORLD_STATUS_OK) plr_json_free(candidate);
    return status;
}

/* Legacy aliases retained for existing TerraR/TerraWasm integrations. */
terrax_world_status terra_player_open_from_buffer(
    const uint8_t *buffer, uint32_t buffer_len, uint32_t *out_handle) {
    return terra_plr_open_from_buffer(buffer, buffer_len, out_handle);
}
terrax_world_status terra_player_close(uint32_t handle) { return terra_plr_close(handle); }
terrax_world_status terra_player_get_json(
    uint32_t handle, char *buffer, uint64_t buffer_size, uint32_t *required_size) {
    return terra_plr_get_json(handle, buffer, buffer_size, required_size);
}
terrax_world_status terra_player_replace_json(uint32_t handle, const char *json_utf8) {
    return terra_plr_replace_json(handle, json_utf8);
}
terrax_world_status terra_player_get_field_json(
    uint32_t handle, const char *pointer_utf8, char *buffer,
    uint64_t buffer_size, uint32_t *required_size) {
    return terra_plr_get(handle, pointer_utf8, buffer, buffer_size, required_size);
}
terrax_world_status terra_player_set_field_json(
    uint32_t handle, const char *pointer_utf8, const char *value_json_utf8) {
    return terra_plr_set(handle, pointer_utf8, value_json_utf8);
}
terrax_world_status terra_player_save_to_buffer(
    uint32_t handle, uint8_t *output, uint32_t capacity, uint32_t *out_required) {
    return terra_plr_save_to_buffer(handle, output, capacity, out_required);
}
terrax_world_status terra_player_set_json(uint32_t handle, const char *json_utf8) {
    return terra_plr_replace_json(handle, json_utf8);
}
terrax_world_status terra_player_get(
    uint32_t handle, const char *pointer_utf8, char *buffer,
    uint64_t buffer_size, uint32_t *required_size) {
    return terra_plr_get(handle, pointer_utf8, buffer, buffer_size, required_size);
}
terrax_world_status terra_player_set(
    uint32_t handle, const char *pointer_utf8, const char *value_json_utf8) {
    return terra_plr_set(handle, pointer_utf8, value_json_utf8);
}
terrax_world_status terra_player_encode(
    uint32_t handle, uint8_t *output, uint32_t capacity, uint32_t *out_required) {
    return terra_plr_save_to_buffer(handle, output, capacity, out_required);
}
terrax_world_status terra_player_validate_handle(uint32_t handle) {
    return terra_plr_validate_handle(handle);
}
terrax_world_status terra_player_open_json(
    const char *json_utf8, uint32_t *out_handle) {
    return terra_plr_open_json(json_utf8, out_handle);
}
terrax_world_status terra_player_apply_patch_json(
    uint32_t handle, const char *patch_json_utf8) {
    return terra_plr_apply_patch_json(handle, patch_json_utf8);
}
