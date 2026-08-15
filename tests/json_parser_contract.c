#include <assert.h>
#include <stdint.h>
#include <string.h>

extern int json_validate_document(const char* json, int jlen);
extern int json_find_key(const char* json, int jlen, const char* key);
extern int json_extract_str(const char* json, int jlen, int pos, char* out, int ocap);
extern int json_extract_int(const char* json, int jlen, int pos, int32_t* out);
extern int json_extract_u64(const char* json, int jlen, int pos, uint64_t* out);
extern int json_extract_bool(const char* json, int jlen, int pos, int* out);
extern int json_extract_float(const char* json, int jlen, int pos, double* out);
extern int json_array_count(const char* json, int jlen, int pos);
extern int json_array_element(const char* json, int jlen, int pos, int index);

static int text_len(const char* text) {
    return (int)strlen(text);
}

int main(void) {
    int32_t signed_value = 0;
    uint64_t unsigned_value = 0;
    int boolean_value = -1;
    double float_value = 0.0;
    char text[32];

    const char* valid_document = " {\"value\":1,\"ignored\":false} \n";
    assert(json_validate_document(valid_document, text_len(valid_document)));

    const char* signed_limits = "{\"max\":2147483647,\"min\":-2147483648}";
    int pos = json_find_key(signed_limits, text_len(signed_limits), "max");
    assert(pos >= 0);
    assert(json_extract_int(signed_limits, text_len(signed_limits), pos, &signed_value));
    assert(signed_value == INT32_MAX);
    pos = json_find_key(signed_limits, text_len(signed_limits), "min");
    assert(pos >= 0);
    assert(json_extract_int(signed_limits, text_len(signed_limits), pos, &signed_value));
    assert(signed_value == INT32_MIN);

    const char* signed_overflow = "{\"value\":2147483648}";
    pos = json_find_key(signed_overflow, text_len(signed_overflow), "value");
    assert(pos >= 0);
    assert(!json_extract_int(signed_overflow, text_len(signed_overflow), pos, &signed_value));

    const char* unsigned_limit = "{\"value\":18446744073709551615}";
    pos = json_find_key(unsigned_limit, text_len(unsigned_limit), "value");
    assert(pos >= 0);
    assert(json_extract_u64(unsigned_limit, text_len(unsigned_limit), pos, &unsigned_value));
    assert(unsigned_value == UINT64_MAX);

    const char* unsigned_overflow = "{\"value\":18446744073709551616}";
    pos = json_find_key(unsigned_overflow, text_len(unsigned_overflow), "value");
    assert(pos >= 0);
    assert(!json_extract_u64(unsigned_overflow, text_len(unsigned_overflow), pos, &unsigned_value));

    const char* booleans = "{\"yes\":true,\"no\":false,\"empty\":null}";
    pos = json_find_key(booleans, text_len(booleans), "yes");
    assert(json_extract_bool(booleans, text_len(booleans), pos, &boolean_value));
    assert(boolean_value == 1);
    pos = json_find_key(booleans, text_len(booleans), "no");
    assert(json_extract_bool(booleans, text_len(booleans), pos, &boolean_value));
    assert(boolean_value == 0);

    const char* malformed_documents[] = {
        "{\"value\":1]",
        "{\"ignored\":tru,\"value\":1}",
        "{\"value\":1,\"broken\":}",
        "[trash]",
        "{\"value\":1} trailing",
        "{\"value\":01}",
        "{\"value\":1,}",
        "{\"value\":1 \"other\":2}",
    };
    for (size_t i = 0; i < sizeof(malformed_documents) / sizeof(malformed_documents[0]); i++) {
        assert(!json_validate_document(malformed_documents[i], text_len(malformed_documents[i])));
        assert(json_find_key(malformed_documents[i], text_len(malformed_documents[i]), "value") < 0);
    }

    const char* escaped = "{\"value\":\"line\\n\\u0041\"}";
    pos = json_find_key(escaped, text_len(escaped), "value");
    assert(json_extract_str(escaped, text_len(escaped), pos, text, (int)sizeof(text)) == 6);
    assert(strcmp(text, "line\nA") == 0);

    const char* unterminated = "{\"value\":\"missing}";
    pos = json_find_key(unterminated, text_len(unterminated), "value");
    assert(pos < 0 || !json_extract_str(unterminated, text_len(unterminated), pos, text, (int)sizeof(text)));

    const char* nested = "{\"nested\":{\"id\":1},\"id\":2}";
    pos = json_find_key(nested, text_len(nested), "id");
    assert(pos >= 0);
    assert(json_extract_int(nested, text_len(nested), pos, &signed_value));
    assert(signed_value == 2);

    const char* number_with_suffix = "{\"value\":12oops}";
    pos = json_find_key(number_with_suffix, text_len(number_with_suffix), "value");
    assert(pos < 0);

    const char* leading_zero = "{\"value\":-01}";
    assert(!json_validate_document(leading_zero, text_len(leading_zero)));

    const char* floating = "{\"value\":-1.25e2}";
    pos = json_find_key(floating, text_len(floating), "value");
    assert(pos >= 0);
    assert(json_extract_float(floating, text_len(floating), pos, &float_value));
    assert(float_value == -125.0);

    const char* array = "[1,{\"nested\":[2,3]},true]";
    assert(json_array_count(array, text_len(array), 0) == 3);
    pos = json_array_element(array, text_len(array), 0, 1);
    assert(pos >= 0 && array[pos] == '{');
    assert(json_array_element(array, text_len(array), 0, 3) < 0);
    assert(json_array_count("[1,]", 4, 0) < 0);
    assert(json_array_count("not-array", 9, 0) < 0);

    const char* emoji = "{\"value\":\"\\uD83D\\uDE00\"}";
    pos = json_find_key(emoji, text_len(emoji), "value");
    assert(pos >= 0);
    char emoji_utf8[5];
    assert(json_extract_str(emoji, text_len(emoji), pos, emoji_utf8, sizeof(emoji_utf8)) == 4);
    assert((unsigned char)emoji_utf8[0] == 0xf0u);
    assert((unsigned char)emoji_utf8[1] == 0x9fu);
    assert((unsigned char)emoji_utf8[2] == 0x98u);
    assert((unsigned char)emoji_utf8[3] == 0x80u);
    char emoji_too_small[4];
    assert(!json_extract_str(emoji, text_len(emoji), pos, emoji_too_small, sizeof(emoji_too_small)));

    const char* isolated_high = "{\"value\":\"\\uD83D\"}";
    const char* isolated_low = "{\"value\":\"\\uDE00\"}";
    assert(!json_validate_document(isolated_high, text_len(isolated_high)));
    assert(!json_validate_document(isolated_low, text_len(isolated_low)));

    return 0;
}
