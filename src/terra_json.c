/*
 * terra_json.c -- Minimal JSON builder and bounded parser.
 *
 * Builder: writes JSON values into a TxBuf.
 * Parser: extracts top-level object values with strict token boundaries.
 */
#include "terra_types.h"

#include <limits.h>
#include <stdint.h>

/* Forward declarations from terra_mem.c */
uint32_t tx_strlen(const char* s);
void buf_u8(TxBuf* b, uint8_t v);
void buf_cstr(TxBuf* b, const char* s);

/* ====================================================================
 * JSON Builder -- writes JSON into TxBuf
 * ==================================================================== */

void json_string(TxBuf* b, const char* s) {
    buf_u8(b, '"');
    if (s) {
        for (uint32_t i = 0; s[i]; i++) {
            unsigned char c = (unsigned char)s[i];
            if (c == '"' || c == '\\') {
                buf_u8(b, '\\');
                buf_u8(b, c);
            } else if (c == '\n') {
                buf_cstr(b, "\\n");
            } else if (c == '\r') {
                buf_cstr(b, "\\r");
            } else if (c == '\t') {
                buf_cstr(b, "\\t");
            } else if (c < 32u) {
                buf_cstr(b, " ");
            } else {
                buf_u8(b, c);
            }
        }
    }
    buf_u8(b, '"');
}

void json_u32(TxBuf* b, uint32_t v) {
    char tmp[16];
    uint32_t n = 0;
    if (v == 0u) { buf_u8(b, '0'); return; }
    while (v && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n) buf_u8(b, (uint8_t)tmp[--n]);
}

void json_i32(TxBuf* b, int32_t v) {
    if (v < 0) {
        buf_u8(b, '-');
        json_u32(b, (uint32_t)(-(int64_t)v));
    } else {
        json_u32(b, (uint32_t)v);
    }
}

void json_u64(TxBuf* b, uint64_t v) {
    char tmp[32];
    uint32_t n = 0;
    if (v == 0u) { buf_u8(b, '0'); return; }
    while (v && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (uint32_t)(v % 10u));
        v /= 10u;
    }
    while (n) buf_u8(b, (uint8_t)tmp[--n]);
}

void json_bool(TxBuf* b, int v) {
    buf_cstr(b, v ? "true" : "false");
}

void json_null(TxBuf* b) {
    buf_cstr(b, "null");
}

void json_float(TxBuf* b, double v) {
    /* Simple float-to-string: integer part + optional fractional part */
    if (v < 0.0) { buf_u8(b, '-'); v = -v; }
    uint64_t integer = (uint64_t)v;
    double frac = v - (double)integer;
    json_u64(b, integer);
    if (frac > 1e-9) {
        buf_u8(b, '.');
        for (int i = 0; i < 6; i++) {
            frac *= 10.0;
            int digit = (int)frac;
            buf_u8(b, (uint8_t)('0' + digit));
            frac -= (double)digit;
        }
    }
}

/* ====================================================================
 * JSON Parser -- bounded structural and token helpers
 * ==================================================================== */

static int json_is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int json_skip_whitespace(const char* s, int len, int pos) {
    while (pos >= 0 && pos < len && json_is_whitespace(s[pos])) pos++;
    return pos;
}

static int json_is_hex(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static uint32_t json_hex_value(char c) {
    if (c >= '0' && c <= '9') return (uint32_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint32_t)(c - 'a' + 10);
    return (uint32_t)(c - 'A' + 10);
}

static int json_read_hex4(const char* s, int len, int pos, uint32_t* out) {
    if (!s || !out || pos < 0 || pos > len - 4) return 0;
    uint32_t value = 0u;
    for (int i = 0; i < 4; i++) {
        if (!json_is_hex(s[pos + i])) return 0;
        value = (value << 4) | json_hex_value(s[pos + i]);
    }
    *out = value;
    return 1;
}

static int json_is_high_surrogate(uint32_t value) {
    return value >= 0xd800u && value <= 0xdbffu;
}

static int json_is_low_surrogate(uint32_t value) {
    return value >= 0xdc00u && value <= 0xdfffu;
}

static int json_skip_string(const char* s, int len, int pos) {
    if (!s || pos < 0 || pos >= len || s[pos] != '"') return -1;
    pos++;
    while (pos < len) {
        unsigned char c = (unsigned char)s[pos++];
        if (c == '"') return pos;
        if (c < 0x20u) return -1;
        if (c != '\\') continue;
        if (pos >= len) return -1;
        char escaped = s[pos++];
        if (escaped == 'u') {
            uint32_t codepoint = 0u;
            if (!json_read_hex4(s, len, pos, &codepoint)) return -1;
            pos += 4;
            if (json_is_high_surrogate(codepoint)) {
                uint32_t low = 0u;
                if (pos > len - 6 || s[pos] != '\\' || s[pos + 1] != 'u' ||
                    !json_read_hex4(s, len, pos + 2, &low) ||
                    !json_is_low_surrogate(low)) return -1;
                pos += 6;
            } else if (json_is_low_surrogate(codepoint)) {
                return -1;
            }
        } else if (escaped != '"' && escaped != '\\' && escaped != '/' &&
                   escaped != 'b' && escaped != 'f' && escaped != 'n' &&
                   escaped != 'r' && escaped != 't') {
            return -1;
        }
    }
    return -1;
}

static int json_parse_number(const char* s, int len, int pos) {
    if (!s || pos < 0 || pos >= len) return -1;
    if (s[pos] == '-') {
        pos++;
        if (pos >= len) return -1;
    }
    if (s[pos] == '0') {
        pos++;
        if (pos < len && s[pos] >= '0' && s[pos] <= '9') return -1;
    } else {
        if (s[pos] < '1' || s[pos] > '9') return -1;
        while (pos < len && s[pos] >= '0' && s[pos] <= '9') pos++;
    }
    if (pos < len && s[pos] == '.') {
        pos++;
        if (pos >= len || s[pos] < '0' || s[pos] > '9') return -1;
        while (pos < len && s[pos] >= '0' && s[pos] <= '9') pos++;
    }
    if (pos < len && (s[pos] == 'e' || s[pos] == 'E')) {
        pos++;
        if (pos < len && (s[pos] == '+' || s[pos] == '-')) pos++;
        if (pos >= len || s[pos] < '0' || s[pos] > '9') return -1;
        while (pos < len && s[pos] >= '0' && s[pos] <= '9') pos++;
    }
    return pos;
}

static int json_parse_value(const char* s, int len, int pos, int depth) {
    if (!s || pos < 0 || depth >= 64) return -1;
    pos = json_skip_whitespace(s, len, pos);
    if (pos < 0 || pos >= len) return -1;

    if (s[pos] == '"') return json_skip_string(s, len, pos);
    if (s[pos] == 't') {
        if (pos > len - 4 || s[pos + 1] != 'r' || s[pos + 2] != 'u' || s[pos + 3] != 'e') return -1;
        return pos + 4;
    }
    if (s[pos] == 'f') {
        if (pos > len - 5 || s[pos + 1] != 'a' || s[pos + 2] != 'l' ||
            s[pos + 3] != 's' || s[pos + 4] != 'e') return -1;
        return pos + 5;
    }
    if (s[pos] == 'n') {
        if (pos > len - 4 || s[pos + 1] != 'u' || s[pos + 2] != 'l' || s[pos + 3] != 'l') return -1;
        return pos + 4;
    }
    if (s[pos] == '-' || (s[pos] >= '0' && s[pos] <= '9')) {
        return json_parse_number(s, len, pos);
    }

    if (s[pos] == '{') {
        pos = json_skip_whitespace(s, len, pos + 1);
        if (pos < len && s[pos] == '}') return pos + 1;
        while (pos < len) {
            if (s[pos] != '"') return -1;
            pos = json_skip_string(s, len, pos);
            if (pos < 0) return -1;
            pos = json_skip_whitespace(s, len, pos);
            if (pos >= len || s[pos] != ':') return -1;
            pos = json_parse_value(s, len, pos + 1, depth + 1);
            if (pos < 0) return -1;
            pos = json_skip_whitespace(s, len, pos);
            if (pos < len && s[pos] == '}') return pos + 1;
            if (pos >= len || s[pos] != ',') return -1;
            pos = json_skip_whitespace(s, len, pos + 1);
            if (pos >= len || s[pos] == '}') return -1;
        }
        return -1;
    }

    if (s[pos] == '[') {
        pos = json_skip_whitespace(s, len, pos + 1);
        if (pos < len && s[pos] == ']') return pos + 1;
        while (pos < len) {
            pos = json_parse_value(s, len, pos, depth + 1);
            if (pos < 0) return -1;
            pos = json_skip_whitespace(s, len, pos);
            if (pos < len && s[pos] == ']') return pos + 1;
            if (pos >= len || s[pos] != ',') return -1;
            pos = json_skip_whitespace(s, len, pos + 1);
            if (pos >= len || s[pos] == ']') return -1;
        }
    }
    return -1;
}

int json_validate_document(const char* json, int jlen) {
    if (!json || jlen <= 0) return 0;
    int end = json_parse_value(json, jlen, 0, 0);
    return end >= 0 && json_skip_whitespace(json, jlen, end) == jlen;
}

int json_skip_value(const char* s, int len, int pos) {
    return json_parse_value(s, len, pos, 0);
}

static int json_key_equals(const char* json, int start, int end, const char* key) {
    int key_len = (int)tx_strlen(key);
    if (end - start != key_len) return 0;
    for (int i = 0; i < key_len; i++) {
        if (json[start + i] == '\\' || json[start + i] != key[i]) return 0;
    }
    return 1;
}

int json_find_key(const char* json, int jlen, const char* key) {
    if (!json || !key || !json_validate_document(json, jlen)) return -1;
    int pos = json_skip_whitespace(json, jlen, 0);
    if (pos >= jlen || json[pos] != '{') return -1;
    pos = json_skip_whitespace(json, jlen, pos + 1);
    if (pos < jlen && json[pos] == '}') return -1;

    int found = -1;
    while (pos < jlen) {
        if (json[pos] != '"') return -1;
        int key_start = pos + 1;
        int key_end_pos = json_skip_string(json, jlen, pos);
        if (key_end_pos < 0) return -1;
        int key_end = key_end_pos - 1;

        pos = json_skip_whitespace(json, jlen, key_end_pos);
        if (pos >= jlen || json[pos] != ':') return -1;
        pos = json_skip_whitespace(json, jlen, pos + 1);
        if (pos >= jlen) return -1;
        int value_pos = pos;
        pos = json_skip_value(json, jlen, pos);
        if (pos < 0) return -1;
        if (found < 0 && json_key_equals(json, key_start, key_end, key)) found = value_pos;

        pos = json_skip_whitespace(json, jlen, pos);
        if (pos < jlen && json[pos] == '}') return found;
        if (pos >= jlen || json[pos] != ',') return -1;
        pos = json_skip_whitespace(json, jlen, pos + 1);
        if (pos >= jlen || json[pos] == '}') return -1;
    }
    return -1;
}

int json_value_eq(const char* json, int jlen, int pos, const char* val) {
    if (!json || !val || !json_validate_document(json, jlen)) return 0;
    pos = json_skip_whitespace(json, jlen, pos);
    int vlen = (int)tx_strlen(val);
    if (pos < 0 || vlen <= 0 || pos > jlen - vlen) return 0;
    int end = json_skip_value(json, jlen, pos);
    if (end != pos + vlen) return 0;
    for (int i = 0; i < vlen; i++) {
        if (json[pos + i] != val[i]) return 0;
    }
    return 1;
}

int json_is_null(const char* json, int jlen, int pos) {
    return json_value_eq(json, jlen, pos, "null");
}

static int json_append_byte(char* out, int ocap, int* length, unsigned char value) {
    if (!out || !length || *length < 0 || *length >= ocap - 1) return 0;
    out[(*length)++] = (char)value;
    return 1;
}

static int json_append_codepoint(char* out, int ocap, int* length, uint32_t value) {
    int bytes = value <= 0x7fu ? 1 : value <= 0x7ffu ? 2 : value <= 0xffffu ? 3 : value <= 0x10ffffu ? 4 : 0;
    if (!bytes || !out || !length || *length < 0 || ocap <= 0 ||
        *length > ocap - 1 - bytes) return 0;
    if (value <= 0x7fu) {
        return json_append_byte(out, ocap, length, (unsigned char)value);
    }
    if (value <= 0x7ffu) {
        return json_append_byte(out, ocap, length, (unsigned char)(0xc0u | (value >> 6))) &&
               json_append_byte(out, ocap, length, (unsigned char)(0x80u | (value & 0x3fu)));
    }
    if (value <= 0xffffu) {
        return json_append_byte(out, ocap, length, (unsigned char)(0xe0u | (value >> 12))) &&
               json_append_byte(out, ocap, length, (unsigned char)(0x80u | ((value >> 6) & 0x3fu))) &&
               json_append_byte(out, ocap, length, (unsigned char)(0x80u | (value & 0x3fu)));
    }
    return json_append_byte(out, ocap, length, (unsigned char)(0xf0u | (value >> 18))) &&
           json_append_byte(out, ocap, length, (unsigned char)(0x80u | ((value >> 12) & 0x3fu))) &&
           json_append_byte(out, ocap, length, (unsigned char)(0x80u | ((value >> 6) & 0x3fu))) &&
           json_append_byte(out, ocap, length, (unsigned char)(0x80u | (value & 0x3fu)));
}

int json_extract_str(const char* json, int jlen, int pos, char* out, int ocap) {
    if (!out || ocap <= 0 || !json_validate_document(json, jlen)) return 0;
    out[0] = 0;
    pos = json_skip_whitespace(json, jlen, pos);
    if (pos < 0 || pos >= jlen || json[pos] != '"') return 0;
    pos++;
    int n = 0;

    while (pos < jlen) {
        unsigned char c = (unsigned char)json[pos++];
        if (c == '"') {
            out[n] = 0;
            return n;
        }
        if (c < 0x20u) return 0;
        if (c != '\\') {
            if (!json_append_byte(out, ocap, &n, c)) return 0;
            continue;
        }

        if (pos >= jlen) return 0;
        char escaped = json[pos++];
        unsigned char decoded = 0;
        if (escaped == '"' || escaped == '\\' || escaped == '/') decoded = (unsigned char)escaped;
        else if (escaped == 'b') decoded = '\b';
        else if (escaped == 'f') decoded = '\f';
        else if (escaped == 'n') decoded = '\n';
        else if (escaped == 'r') decoded = '\r';
        else if (escaped == 't') decoded = '\t';
        else if (escaped == 'u') {
            if (pos > jlen - 4) return 0;
            uint32_t codepoint = 0u;
            if (!json_read_hex4(json, jlen, pos, &codepoint)) return 0;
            pos += 4;
            if (json_is_high_surrogate(codepoint)) {
                uint32_t low = 0u;
                if (pos > jlen - 6 || json[pos] != '\\' || json[pos + 1] != 'u' ||
                    !json_read_hex4(json, jlen, pos + 2, &low) ||
                    !json_is_low_surrogate(low)) return 0;
                codepoint = 0x10000u + ((codepoint - 0xd800u) << 10) + (low - 0xdc00u);
                pos += 6;
            } else if (json_is_low_surrogate(codepoint)) {
                return 0;
            }
            if (!json_append_codepoint(out, ocap, &n, codepoint)) return 0;
            continue;
        } else {
            return 0;
        }
        if (!json_append_byte(out, ocap, &n, decoded)) return 0;
    }
    out[0] = 0;
    return 0;
}

int json_extract_int(const char* json, int jlen, int pos, int32_t* out) {
    if (!json || !out || !json_validate_document(json, jlen)) return 0;
    pos = json_skip_whitespace(json, jlen, pos);
    if (pos < 0 || pos >= jlen) return 0;

    int start = pos;
    int end = json_parse_number(json, jlen, start);
    if (end < 0) return 0;
    int negative = 0;
    if (json[pos] == '-') {
        negative = 1;
        pos++;
    }
    if (pos >= jlen || json[pos] < '0' || json[pos] > '9') return 0;

    uint64_t limit = negative ? 2147483648ULL : 2147483647ULL;
    uint64_t value = 0u;
    while (pos < end && json[pos] >= '0' && json[pos] <= '9') {
        uint32_t digit = (uint32_t)(json[pos] - '0');
        if (value > (limit - digit) / 10u) return 0;
        value = value * 10u + digit;
        pos++;
    }
    if (pos != end) return 0;
    *out = negative ? (int32_t)(-(int64_t)value) : (int32_t)value;
    return 1;
}

int json_extract_u64(const char* json, int jlen, int pos, uint64_t* out) {
    if (!json || !out || !json_validate_document(json, jlen)) return 0;
    pos = json_skip_whitespace(json, jlen, pos);
    if (pos < 0 || pos >= jlen || json[pos] < '0' || json[pos] > '9') return 0;

    int end = json_parse_number(json, jlen, pos);
    if (end < 0) return 0;
    uint64_t value = 0u;
    while (pos < end && json[pos] >= '0' && json[pos] <= '9') {
        uint32_t digit = (uint32_t)(json[pos] - '0');
        if (value > (UINT64_MAX - digit) / 10u) return 0;
        value = value * 10u + digit;
        pos++;
    }
    if (pos != end) return 0;
    *out = value;
    return 1;
}

int json_extract_bool(const char* json, int jlen, int pos, int* out) {
    if (!out) return 0;
    if (json_value_eq(json, jlen, pos, "true")) {
        *out = 1;
        return 1;
    }
    if (json_value_eq(json, jlen, pos, "false")) {
        *out = 0;
        return 1;
    }
    return 0;
}

static int parse_double_from_str(const char* s, int len, double* out) {
    if (!s || !out || len <= 0) return 0;
    int pos = 0;
    int negative = 0;
    if (s[pos] == '-') {
        negative = 1;
        pos++;
    }
    if (pos >= len || s[pos] < '0' || s[pos] > '9') return 0;

    double value = 0.0;
    while (pos < len && s[pos] >= '0' && s[pos] <= '9') {
        value = value * 10.0 + (double)(s[pos] - '0');
        pos++;
    }
    if (pos < len && s[pos] == '.') {
        pos++;
        if (pos >= len || s[pos] < '0' || s[pos] > '9') return 0;
        double scale = 0.1;
        while (pos < len && s[pos] >= '0' && s[pos] <= '9') {
            value += (double)(s[pos] - '0') * scale;
            scale *= 0.1;
            pos++;
        }
    }

    int exponent = 0;
    int exponent_negative = 0;
    if (pos < len && (s[pos] == 'e' || s[pos] == 'E')) {
        pos++;
        if (pos < len && (s[pos] == '+' || s[pos] == '-')) {
            exponent_negative = s[pos] == '-';
            pos++;
        }
        if (pos >= len || s[pos] < '0' || s[pos] > '9') return 0;
        while (pos < len && s[pos] >= '0' && s[pos] <= '9') {
            if (exponent > 308) return 0;
            exponent = exponent * 10 + (s[pos] - '0');
            pos++;
        }
        if (exponent > 308) return 0;
    }
    if (pos != len) return 0;

    while (exponent-- > 0) value = exponent_negative ? value / 10.0 : value * 10.0;
    *out = negative ? -value : value;
    return 1;
}

int json_extract_float(const char* json, int jlen, int pos, double* out) {
    if (!json || !out || !json_validate_document(json, jlen)) return 0;
    pos = json_skip_whitespace(json, jlen, pos);
    if (pos < 0 || pos >= jlen) return 0;

    if (json[pos] == '"') {
        char tmp[96];
        int n = json_extract_str(json, jlen, pos, tmp, (int)sizeof(tmp));
        return n > 0 && parse_double_from_str(tmp, n, out);
    }

    int start = pos;
    int end = json_parse_number(json, jlen, start);
    if (end < 0) return 0;
    return parse_double_from_str(json + start, end - start, out);
}

int json_array_count(const char* json, int jlen, int pos) {
    if (!json || !json_validate_document(json, jlen)) return -1;
    pos = json_skip_whitespace(json, jlen, pos);
    if (pos < 0 || pos >= jlen || json[pos] != '[') return -1;
    pos = json_skip_whitespace(json, jlen, pos + 1);
    if (pos < jlen && json[pos] == ']') return 0;

    int count = 0;
    while (pos < jlen) {
        int end = json_skip_value(json, jlen, pos);
        if (end < 0) return -1;
        count++;
        pos = json_skip_whitespace(json, jlen, end);
        if (pos >= jlen) return -1;
        if (json[pos] == ']') return count;
        if (json[pos] != ',') return -1;
        pos = json_skip_whitespace(json, jlen, pos + 1);
        if (pos >= jlen || json[pos] == ']') return -1;
    }
    return -1;
}

int json_array_element(const char* json, int jlen, int pos, int index) {
    if (!json || index < 0 || !json_validate_document(json, jlen)) return -1;
    pos = json_skip_whitespace(json, jlen, pos);
    if (pos < 0 || pos >= jlen || json[pos] != '[') return -1;
    pos = json_skip_whitespace(json, jlen, pos + 1);
    if (pos >= jlen || json[pos] == ']') return -1;

    int current = 0;
    while (pos < jlen) {
        if (current == index) return pos;
        int end = json_skip_value(json, jlen, pos);
        if (end < 0) return -1;
        pos = json_skip_whitespace(json, jlen, end);
        if (pos >= jlen || json[pos] != ',') return -1;
        pos = json_skip_whitespace(json, jlen, pos + 1);
        if (pos >= jlen || json[pos] == ']') return -1;
        current++;
    }
    return -1;
}
