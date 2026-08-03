#include "terra_reader.h"

#include <stddef.h>
#include <stdint.h>

extern int json_find_key(const char* json, int jlen, const char* key);
extern int json_skip_value(const char* json, int jlen, int pos);
extern int json_extract_str(const char* json, int jlen, int pos, char* out, int ocap);
extern int json_extract_int(const char* json, int jlen, int pos, int32_t* out);
extern int json_extract_u64(const char* json, int jlen, int pos, uint64_t* out);
extern int json_extract_bool(const char* json, int jlen, int pos, int* out);
extern int json_extract_float(const char* json, int jlen, int pos, double* out);
extern int json_array_count(const char* json, int jlen, int pos);
extern int json_array_element(const char* json, int jlen, int pos, int index);

/* Exercises bounded reader arithmetic and every public JSON parser entry point. */
int terra_fuzz_json(const uint8_t* data, size_t size) {
    uint32_t offset = 0;
    for (size_t i = 0; i < size && i < 256; ++i) {
        const uint32_t count = (uint32_t)data[i];
        const uint32_t width = (uint32_t)((data[i] >> 5) & 0x07u);
        (void)terra_reader_take_count(&offset, count, width, (uint32_t)size);
        if (offset > size) offset = (uint32_t)size;
    }

    char json[257];
    size_t length = size < 256u ? size : 256u;
    for (size_t i = 0; i < length; i++) json[i] = (char)data[i];
    json[length] = 0;

    int jlen = (int)length;
    int pos = json_find_key(json, jlen, "value");
    if (pos < 0) pos = 0;

    char text[64];
    int32_t signed_value = 0;
    uint64_t unsigned_value = 0;
    int boolean_value = 0;
    double float_value = 0.0;

    (void)json_skip_value(json, jlen, pos);
    (void)json_extract_str(json, jlen, pos, text, (int)sizeof(text));
    (void)json_extract_int(json, jlen, pos, &signed_value);
    (void)json_extract_u64(json, jlen, pos, &unsigned_value);
    (void)json_extract_bool(json, jlen, pos, &boolean_value);
    (void)json_extract_float(json, jlen, pos, &float_value);

    int count = json_array_count(json, jlen, 0);
    if (count > 0) {
        int index = data && size ? (int)(data[0] % (uint8_t)count) : 0;
        (void)json_array_element(json, jlen, 0, index);
    }
    return 0;
}
