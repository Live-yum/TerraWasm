/*
 * terra_json.c -- Minimal JSON builder and parser.
 *
 * Builder: writes JSON values into a TxBuf.
 * Parser: extracts values from a JSON string by key lookup.
 */
#include "terra_types.h"

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
 * JSON Parser -- extracts values by key
 * ==================================================================== */

static int json_skip_whitespace(const char* s, int len, int pos) {
    while (pos < len && (s[pos] == ' ' || s[pos] == '\t' ||
                         s[pos] == '\n' || s[pos] == '\r'))
        pos++;
    return pos;
}

static int json_skip_string(const char* s, int len, int pos) {
    if (pos >= len || s[pos] != '"') return pos;
    pos++;
    while (pos < len) {
        if (s[pos] == '\\') { pos += 2; }
        else if (s[pos] == '"') { return pos + 1; }
        else pos++;
    }
    return pos;
}

int json_skip_value(const char* s, int len, int pos) {
    pos = json_skip_whitespace(s, len, pos);
    if (pos >= len) return pos;
    char c = s[pos];
    if (c == '"') return json_skip_string(s, len, pos);
    if (c == '{') {
        pos++;
        int depth = 1;
        while (pos < len && depth > 0) {
            if (s[pos] == '"') pos = json_skip_string(s, len, pos);
            else {
                if (s[pos] == '{') depth++;
                if (s[pos] == '}') depth--;
                pos++;
            }
        }
        return pos;
    }
    if (c == '[') {
        pos++;
        int depth = 1;
        while (pos < len && depth > 0) {
            if (s[pos] == '"') pos = json_skip_string(s, len, pos);
            else {
                if (s[pos] == '[') depth++;
                if (s[pos] == ']') depth--;
                pos++;
            }
        }
        return pos;
    }
    /* Primitive: scan to delimiter */
    while (pos < len && s[pos] != ',' && s[pos] != '}' && s[pos] != ']' &&
           s[pos] != ' ' && s[pos] != '\t' && s[pos] != '\n' && s[pos] != '\r')
        pos++;
    return pos;
}

int json_find_key(const char* json, int jlen, const char* key) {
    int klen = (int)tx_strlen(key);
    for (int i = 0; i < jlen; i++) {
        if (json[i] != '"') continue;
        int ks = i + 1;
        int ke = ks;
        while (ke < jlen && json[ke] != '"') {
            if (json[ke] == '\\') ke++;
            ke++;
        }
        if (ke - ks == klen) {
            int match = 1;
            for (int k = 0; k < klen; k++) {
                if (json[ks + k] != key[k]) { match = 0; break; }
            }
            if (match) {
                int p = ke + 1;
                p = json_skip_whitespace(json, jlen, p);
                if (p < jlen && json[p] == ':') {
                    p++;
                    return json_skip_whitespace(json, jlen, p);
                }
            }
        }
        i = ke;
    }
    return -1;
}

int json_value_eq(const char* json, int jlen, int pos, const char* val) {
    pos = json_skip_whitespace(json, jlen, pos);
    int vlen = (int)tx_strlen(val);
    if (pos + vlen > jlen) return 0;
    for (int i = 0; i < vlen; i++) {
        if (json[pos + i] != val[i]) return 0;
    }
    return 1;
}

int json_is_null(const char* json, int jlen, int pos) {
    return json_value_eq(json, jlen, pos, "null");
}

int json_extract_str(const char* json, int jlen, int pos, char* out, int ocap) {
    pos = json_skip_whitespace(json, jlen, pos);
    if (pos >= jlen || json[pos] != '"') return 0;
    pos++;
    int n = 0;
    while (pos < jlen && json[pos] != '"' && n < ocap - 1) {
        if (json[pos] == '\\') {
            pos++;
            if (pos < jlen && n < ocap - 1) out[n++] = json[pos];
        } else {
            out[n++] = json[pos];
        }
        pos++;
    }
    out[n] = 0;
    return n;
}

int json_extract_int(const char* json, int jlen, int pos, int32_t* out) {
    pos = json_skip_whitespace(json, jlen, pos);
    if (pos >= jlen) return 0;
    int neg = 0;
    if (json[pos] == '-') { neg = 1; pos++; }
    int64_t v = 0;
    int digits = 0;
    while (pos < jlen && json[pos] >= '0' && json[pos] <= '9') {
        v = v * 10 + (json[pos] - '0');
        pos++;
        digits++;
    }
    if (!digits) return 0;
    *out = neg ? (int32_t)(-v) : (int32_t)v;
    return 1;
}

int json_extract_u64(const char* json, int jlen, int pos, uint64_t* out) {
    pos = json_skip_whitespace(json, jlen, pos);
    if (pos >= jlen) return 0;
    uint64_t v = 0;
    int digits = 0;
    while (pos < jlen && json[pos] >= '0' && json[pos] <= '9') {
        v = v * 10 + (uint64_t)(json[pos] - '0');
        pos++;
        digits++;
    }
    if (!digits) return 0;
    *out = v;
    return 1;
}

int json_extract_bool(const char* json, int jlen, int pos, int* out) {
    pos = json_skip_whitespace(json, jlen, pos);
    if (pos < jlen && json[pos] == 't') { *out = 1; return 1; }
    if (pos < jlen && json[pos] == 'f') { *out = 0; return 1; }
    return 0;
}

static double parse_double_from_str(const char* s, int len) {
    int i = 0, neg = 0;
    double val = 0.0;
    if (i < len && s[i] == '-') { neg = 1; i++; }
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        val = val * 10.0 + (double)(s[i] - '0');
        i++;
    }
    if (i < len && s[i] == '.') {
        i++;
        double frac = 0.1;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            val += (double)(s[i] - '0') * frac;
            frac *= 0.1;
            i++;
        }
    }
    return neg ? -val : val;
}

int json_extract_float(const char* json, int jlen, int pos, double* out) {
    pos = json_skip_whitespace(json, jlen, pos);
    if (pos >= jlen) return 0;
    if (json[pos] == '"') {
        char tmp[64];
        int n = json_extract_str(json, jlen, pos, tmp, 64);
        if (!n) return 0;
        *out = parse_double_from_str(tmp, n);
        return 1;
    }
    int start = pos;
    if (json[pos] == '-') pos++;
    while (pos < jlen && json[pos] >= '0' && json[pos] <= '9') pos++;
    if (pos < jlen && json[pos] == '.') {
        pos++;
        while (pos < jlen && json[pos] >= '0' && json[pos] <= '9') pos++;
    }
    char tmp[64];
    int n = pos - start;
    if (n >= 64) n = 63;
    { int i; for (i = 0; i < n; i++) tmp[i] = json[start + i]; tmp[n] = 0; }
    *out = parse_double_from_str(tmp, n);
    return 1;
}

/* Extract a JSON array element count (count commas + 1 for non-empty arrays) */
int json_array_count(const char* json, int jlen, int pos) {
    pos = json_skip_whitespace(json, jlen, pos);
    if (pos >= jlen || json[pos] != '[') return 0;
    pos++;
    pos = json_skip_whitespace(json, jlen, pos);
    if (pos < jlen && json[pos] == ']') return 0;  /* empty array */
    int count = 1;
    int depth = 1;
    while (pos < jlen && depth > 0) {
        if (json[pos] == '"') { pos = json_skip_string(json, jlen, pos); continue; }
        if (json[pos] == '[' || json[pos] == '{') depth++;
        if (json[pos] == ']' || json[pos] == '}') depth--;
        if (depth == 1 && json[pos] == ',') count++;
        pos++;
    }
    return count;
}

/* Skip to the n-th element of a JSON array (0-indexed) */
int json_array_element(const char* json, int jlen, int pos, int index) {
    pos = json_skip_whitespace(json, jlen, pos);
    if (pos >= jlen || json[pos] != '[') return -1;
    pos++;
    pos = json_skip_whitespace(json, jlen, pos);
    if (pos < jlen && json[pos] == ']') return -1;
    for (int i = 0; i < index; i++) {
        pos = json_skip_value(json, jlen, pos);
        pos = json_skip_whitespace(json, jlen, pos);
        if (pos < jlen && json[pos] == ',') pos++;
        pos = json_skip_whitespace(json, jlen, pos);
    }
    return pos;
}
