/*
 * terra_mem.c -- Owned allocation domains, TxBuf, hand-rolled libc.
 *
 * Bridge allocations are individually owned by the JS caller. Native
 * allocations are roots tracked by a monotonic sequence and can be rewound.
 */
#include "terra_types.h"
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>

/* ---------- Hand-rolled libc ---------- */

void* memset(void* dst, int value, unsigned long n) {
    unsigned char* p = (unsigned char*)dst;
    for (unsigned long i = 0; i < n; i++) p[i] = (unsigned char)value;
    return dst;
}

void* memcpy(void* dst, const void* src, unsigned long n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (unsigned long i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

/* ---------- String utilities ---------- */

uint32_t tx_strlen(const char* s) {
    uint32_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

int tx_streq_c(const char* a, const char* b) {
    uint32_t i = 0;
    if (!a || !b) return 0;
    while (a[i] || b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return 1;
}

int tx_streq_n(const char* a, uint32_t alen, const char* b) {
    uint32_t blen = tx_strlen(b);
    if (alen != blen) return 0;
    for (uint32_t i = 0; i < alen; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

/* ---------- Owned allocator domains ---------- */

#define TX_ALLOC_MAGIC 0x54585254u
#define TX_DOMAIN_BRIDGE 0x42524447u
#define TX_DOMAIN_NATIVE 0x4e415456u

typedef union TxAllocHeader TxAllocHeader;
union TxAllocHeader {
    struct {
        uint32_t magic;
        uint32_t domain;
        uint32_t size;
        uint32_t sequence;
        uintptr_t self;
        TxAllocHeader* prev;
        TxAllocHeader* next;
        uintptr_t reserved;
    } root;
    max_align_t alignment;
};

static uint32_t tx_world_open_count = 0;
static TxAllocHeader* tx_bridge_head = NULL;
static TxAllocHeader* tx_bridge_tail = NULL;
static TxAllocHeader* tx_native_head = NULL;
static TxAllocHeader* tx_native_tail = NULL;
static uint32_t tx_native_sequence = 0;
static uint64_t tx_bridge_live_bytes = 0;
static uint64_t tx_native_live_bytes = 0;
static uint64_t tx_bridge_peak_bytes = 0;
static uint64_t tx_native_peak_bytes = 0;
static uint64_t tx_total_peak_bytes = 0;

/* Result pointers (set by operations, read by JS) */
uint32_t tx_last_ptr = 0;
uint32_t tx_last_len = 0;
uint32_t tx_last_width = 0;
uint32_t tx_last_height = 0;
uint32_t tx_last_stride = 0;

/* Error state */
int32_t tx_last_status = 0;
char tx_last_error[256];

/* Color tables (set by JS via txw_set_color_tables) */
static const uint8_t* g_tile_colors = NULL;
static uint32_t g_tile_color_count = 0;
static const uint8_t* g_wall_colors = NULL;
static uint32_t g_wall_color_count = 0;

const uint8_t* tx_get_tile_colors(void) { return g_tile_colors; }
uint32_t tx_get_tile_color_count(void) { return g_tile_color_count; }
const uint8_t* tx_get_wall_colors(void) { return g_wall_colors; }
uint32_t tx_get_wall_color_count(void) { return g_wall_color_count; }

static int tx_allocation_size(uint32_t payload_size, size_t* total) {
    if (!total || payload_size > UINT32_MAX - (uint32_t)sizeof(TxAllocHeader)) return 0;
    *total = sizeof(TxAllocHeader) + (size_t)payload_size;
    return *total >= sizeof(TxAllocHeader);
}

static TxAllocHeader* tx_find_root(void* payload, uint32_t domain) {
    TxAllocHeader* root = domain == TX_DOMAIN_BRIDGE ? tx_bridge_head : tx_native_head;
    while (root) {
        if ((uint8_t*)root + sizeof(TxAllocHeader) == payload &&
            root->root.magic == TX_ALLOC_MAGIC &&
            root->root.domain == domain &&
            root->root.self == (uintptr_t)root) return root;
        root = root->root.next;
    }
    return NULL;
}

static void tx_append_root(TxAllocHeader* header, uint32_t domain) {
    TxAllocHeader** head = domain == TX_DOMAIN_BRIDGE ? &tx_bridge_head : &tx_native_head;
    TxAllocHeader** tail = domain == TX_DOMAIN_BRIDGE ? &tx_bridge_tail : &tx_native_tail;
    header->root.prev = *tail;
    header->root.next = NULL;
    if (*tail) (*tail)->root.next = header;
    else *head = header;
    *tail = header;
}

static void tx_unlink_root(TxAllocHeader* header, uint32_t domain) {
    TxAllocHeader** head = domain == TX_DOMAIN_BRIDGE ? &tx_bridge_head : &tx_native_head;
    TxAllocHeader** tail = domain == TX_DOMAIN_BRIDGE ? &tx_bridge_tail : &tx_native_tail;
    if (header->root.prev) header->root.prev->root.next = header->root.next;
    else *head = header->root.next;
    if (header->root.next) header->root.next->root.prev = header->root.prev;
    else *tail = header->root.prev;
}

static void tx_release_root(TxAllocHeader* header, uint32_t domain) {
    if (!header) return;
    tx_unlink_root(header, domain);
    if (domain == TX_DOMAIN_NATIVE) tx_native_live_bytes -= header->root.size;
    else tx_bridge_live_bytes -= header->root.size;
    header->root.magic = 0;
    header->root.self = 0;
    free(header);
}

static void* tx_new_root(uint32_t size, uint32_t domain) {
    size_t total = 0;
    if (!tx_allocation_size(size, &total)) return NULL;
    if (domain == TX_DOMAIN_NATIVE && tx_native_sequence == UINT32_MAX) return NULL;
    TxAllocHeader* header = (TxAllocHeader*)malloc(total);
    if (!header) return NULL;
    header->root.magic = TX_ALLOC_MAGIC;
    header->root.domain = domain;
    header->root.size = size;
    header->root.sequence = 0;
    header->root.self = (uintptr_t)header;
    header->root.prev = NULL;
    header->root.next = NULL;
    header->root.reserved = 0;
    tx_append_root(header, domain);
    if (domain == TX_DOMAIN_NATIVE) {
        header->root.sequence = ++tx_native_sequence;
        tx_native_live_bytes += size;
        if (tx_native_live_bytes > tx_native_peak_bytes)
            tx_native_peak_bytes = tx_native_live_bytes;
    } else {
        tx_bridge_live_bytes += size;
        if (tx_bridge_live_bytes > tx_bridge_peak_bytes)
            tx_bridge_peak_bytes = tx_bridge_live_bytes;
    }
    if (tx_bridge_live_bytes + tx_native_live_bytes > tx_total_peak_bytes)
        tx_total_peak_bytes = tx_bridge_live_bytes + tx_native_live_bytes;
    return (uint8_t*)header + sizeof(TxAllocHeader);
}

uint32_t tx_malloc(uint32_t size) {
    void* payload = tx_new_root(size, TX_DOMAIN_BRIDGE);
    return (uint32_t)(uintptr_t)payload;
}

void tx_free(uint32_t ptr) {
    void* payload = (void*)(uintptr_t)ptr;
    tx_release_root(tx_find_root(payload, TX_DOMAIN_BRIDGE), TX_DOMAIN_BRIDGE);
}

uint32_t tx_bridge_allocation_size(uint32_t ptr) {
    void* payload = (void*)(uintptr_t)ptr;
    TxAllocHeader* header = tx_find_root(payload, TX_DOMAIN_BRIDGE);
    return header ? header->root.size : 0u;
}

int tx_bridge_range_is_valid(uint32_t ptr, uint32_t length) {
    if (!ptr) return length == 0u;
    uintptr_t address = (uintptr_t)ptr;
    TxAllocHeader* root = tx_bridge_head;
    while (root) {
        if (root->root.magic == TX_ALLOC_MAGIC &&
            root->root.domain == TX_DOMAIN_BRIDGE &&
            root->root.self == (uintptr_t)root) {
            uintptr_t payload = (uintptr_t)((uint8_t*)root + sizeof(TxAllocHeader));
            if (address >= payload) {
                uintptr_t offset = address - payload;
                if (offset <= root->root.size &&
                    length <= root->root.size - (uint32_t)offset) {
                    return 1;
                }
            }
        }
        root = root->root.next;
    }
    return 0;
}

uint8_t* tx_alloc(uint32_t size) {
    return (uint8_t*)tx_new_root(size ? size : 1u, TX_DOMAIN_NATIVE);
}

void tx_internal_free(void* payload) {
    tx_release_root(tx_find_root(payload, TX_DOMAIN_NATIVE), TX_DOMAIN_NATIVE);
}

void* tx_internal_realloc(void* payload, uint32_t size) {
    if (!payload) return tx_alloc(size);
    TxAllocHeader* old = tx_find_root(payload, TX_DOMAIN_NATIVE);
    if (!old) return NULL;
    size_t total = 0;
    if (!tx_allocation_size(size, &total)) return NULL;
    uint32_t old_size = old->root.size;
    TxAllocHeader* prev = old->root.prev;
    TxAllocHeader* next = old->root.next;
    TxAllocHeader* resized = (TxAllocHeader*)realloc(old, total);
    if (!resized) return NULL;
    resized->root.size = size;
    resized->root.self = (uintptr_t)resized;
    resized->root.prev = prev;
    resized->root.next = next;
    if (prev) prev->root.next = resized;
    else tx_native_head = resized;
    if (next) next->root.prev = resized;
    else tx_native_tail = resized;
    tx_native_live_bytes = tx_native_live_bytes - old_size + size;
    if (tx_native_live_bytes > tx_native_peak_bytes)
        tx_native_peak_bytes = tx_native_live_bytes;
    if (tx_bridge_live_bytes + tx_native_live_bytes > tx_total_peak_bytes)
        tx_total_peak_bytes = tx_bridge_live_bytes + tx_native_live_bytes;
    return (uint8_t*)resized + sizeof(TxAllocHeader);
}

uint32_t tx_heap_used(void) {
    uint64_t total = tx_bridge_live_bytes + tx_native_live_bytes;
    return total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
}

uint32_t tx_bridge_heap_used(void) {
    return tx_bridge_live_bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)tx_bridge_live_bytes;
}

uint32_t tx_native_heap_used(void) {
    return tx_native_live_bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)tx_native_live_bytes;
}

uint32_t tx_heap_peak(void) {
    return tx_total_peak_bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)tx_total_peak_bytes;
}

uint32_t tx_bridge_heap_peak(void) {
    return tx_bridge_peak_bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)tx_bridge_peak_bytes;
}

uint32_t tx_native_heap_peak(void) {
    return tx_native_peak_bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)tx_native_peak_bytes;
}

uint32_t tx_memory_used(void) {
#if defined(__wasm__)
    return (uint32_t)__builtin_wasm_memory_size(0) * TX_PAGE_SIZE;
#else
    return 0u;
#endif
}

uint32_t tx_mark(void) {
    return tx_native_sequence;
}

void tx_rewind(uint32_t mark) {
    while (tx_native_tail && tx_native_tail->root.sequence > mark) {
        TxAllocHeader* tail = tx_native_tail;
        tx_release_root(tail, TX_DOMAIN_NATIVE);
    }
    if (!tx_native_head && mark == 0u) tx_native_sequence = 0u;
}

void tx_reset_heap(void) {
    if (tx_world_open_count != 0u) return;
    tx_rewind(0);
    tx_last_ptr = 0;
    tx_last_len = 0;
    tx_last_width = 0;
    tx_last_height = 0;
    tx_last_stride = 0;
}

/* ---------- Heap mark management ---------- */

void tx_set_world_open_count(uint32_t count) { tx_world_open_count = count; }
uint32_t tx_get_world_open_count(void) { return tx_world_open_count; }


/* ---------- TxBuf dynamic buffer ---------- */

void buf_init(TxBuf* b, uint32_t cap) {
    b->len = 0;
    b->cap = cap ? cap : 128u;
    b->data = tx_alloc(b->cap);
    b->ok = b->data != NULL;
}

int buf_reserve(TxBuf* b, uint32_t extra) {
    if (!b->ok) return 0;
    if (extra > 0xffffffffu - b->len) { b->ok = 0; return 0; }
    uint32_t need = b->len + extra;
    if (need <= b->cap) return 1;
    uint32_t next = b->cap ? b->cap : 128u;
    while (next < need) {
        uint32_t old = next;
        next = next < 1048576u ? next * 2u : next + 1048576u;
        if (next < old) { next = need; break; }
    }

    uint8_t* p = (uint8_t*)tx_internal_realloc(b->data, next);
    if (!p) { b->ok = 0; return 0; }
    b->data = p;
    b->cap = next;
    return 1;
}

void buf_u8(TxBuf* b, uint8_t v) {
    if (buf_reserve(b, 1)) b->data[b->len++] = v;
}

void buf_bytes(TxBuf* b, const void* p, uint32_t n) {
    if (buf_reserve(b, n)) {
        memcpy(b->data + b->len, p, n);
        b->len += n;
    }
}

void buf_u16le(TxBuf* b, uint32_t v) {
    buf_u8(b, (uint8_t)v);
    buf_u8(b, (uint8_t)(v >> 8));
}

void buf_u32le(TxBuf* b, uint32_t v) {
    buf_u8(b, (uint8_t)v);
    buf_u8(b, (uint8_t)(v >> 8));
    buf_u8(b, (uint8_t)(v >> 16));
    buf_u8(b, (uint8_t)(v >> 24));
}

void buf_u64le(TxBuf* b, uint64_t v) {
    for (uint32_t i = 0; i < 8; i++) buf_u8(b, (uint8_t)(v >> (i * 8u)));
}

void buf_u16be(TxBuf* b, uint32_t v) {
    buf_u8(b, (uint8_t)(v >> 8));
    buf_u8(b, (uint8_t)v);
}

void buf_u32be(TxBuf* b, uint32_t v) {
    buf_u8(b, (uint8_t)(v >> 24));
    buf_u8(b, (uint8_t)(v >> 16));
    buf_u8(b, (uint8_t)(v >> 8));
    buf_u8(b, (uint8_t)v);
}

void buf_cstr(TxBuf* b, const char* s) {
    buf_bytes(b, s, tx_strlen(s));
}

/* ---------- Error state ---------- */

void tx_clear_error(void) {
    tx_last_status = 0;
    tx_last_error[0] = 0;
}

static int tx_error_append_raw(const char* text, uint32_t* position) {
    if (!text || !position) return 0;
    while (*text) {
        if (*position + 1u >= sizeof(tx_last_error)) return 0;
        tx_last_error[(*position)++] = *text++;
    }
    return 1;
}

static uint32_t tx_error_escape_char(unsigned char ch, char encoded[6]) {
    static const char hex[] = "0123456789abcdef";
    switch (ch) {
        case '"': encoded[0] = '\\'; encoded[1] = '"'; return 2u;
        case '\\': encoded[0] = '\\'; encoded[1] = '\\'; return 2u;
        case '\b': encoded[0] = '\\'; encoded[1] = 'b'; return 2u;
        case '\f': encoded[0] = '\\'; encoded[1] = 'f'; return 2u;
        case '\n': encoded[0] = '\\'; encoded[1] = 'n'; return 2u;
        case '\r': encoded[0] = '\\'; encoded[1] = 'r'; return 2u;
        case '\t': encoded[0] = '\\'; encoded[1] = 't'; return 2u;
        default:
            if (ch < 0x20u) {
                encoded[0] = '\\'; encoded[1] = 'u'; encoded[2] = '0'; encoded[3] = '0';
                encoded[4] = hex[ch >> 4u]; encoded[5] = hex[ch & 0x0fu];
                return 6u;
            }
            encoded[0] = (char)ch;
            return 1u;
    }
}

static void tx_error_append_json_text(const char* text, uint32_t* position, uint32_t reserve) {
    const unsigned char* cursor = (const unsigned char*)(text ? text : "");
    while (*cursor) {
        char encoded[6];
        uint32_t encoded_len = tx_error_escape_char(*cursor++, encoded);
        if (*position + encoded_len + reserve + 1u > sizeof(tx_last_error)) return;
        for (uint32_t i = 0; i < encoded_len; i++) tx_last_error[(*position)++] = encoded[i];
    }
}

void tx_set_error(const char* code, const char* message) {
    /* Preserve the public NOT_SUPPORTED status through operation dispatch. */
    tx_last_status = tx_streq_c(code, "TERRAX_NOT_SUPPORTED")
        ? TERRAX_WORLD_STATUS_NOT_SUPPORTED
        : -1;

    const char* prefix = "{\"code\":\"";
    const char* middle = "\",\"message\":\"";
    const char* suffix = "\"}";
    uint32_t position = 0;
    (void)tx_error_append_raw(prefix, &position);
    tx_error_append_json_text(
        code ? code : "TERRAX_WASM_ERROR",
        &position,
        tx_strlen(middle) + tx_strlen(suffix));
    (void)tx_error_append_raw(middle, &position);
    tx_error_append_json_text(message ? message : "error", &position, tx_strlen(suffix));
    (void)tx_error_append_raw(suffix, &position);
    tx_last_error[position] = 0;
}

/* ---------- Result helpers ---------- */

int set_result_buf(TxBuf* b) {
    if (!b || !b->ok) {
        tx_set_error("TERRAX_WASM_OOM", "out of memory");
        return -1;
    }
    tx_last_ptr = (uint32_t)(uintptr_t)b->data;
    tx_last_len = b->len;
    tx_clear_error();
    return (int)b->len;
}

int set_result_bytes(uint8_t* p, uint32_t len) {
    if (!p && len) {
        tx_set_error("TERRAX_WASM_OOM", "out of memory");
        return -1;
    }
    tx_last_ptr = (uint32_t)(uintptr_t)p;
    tx_last_len = len;
    tx_clear_error();
    return (int)len;
}

/* ---------- Color table registration ---------- */

void txw_set_color_tables(uint32_t tile_ptr, uint32_t tile_count,
                          uint32_t wall_ptr, uint32_t wall_count) {
    g_tile_colors = tile_ptr ? (const uint8_t*)(uintptr_t)tile_ptr : NULL;
    g_tile_color_count = tile_count;
    g_wall_colors = wall_ptr ? (const uint8_t*)(uintptr_t)wall_ptr : NULL;
    g_wall_color_count = wall_count;
}
