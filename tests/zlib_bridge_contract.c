#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

#ifdef NDEBUG
#error "native contracts must be compiled with assertions enabled"
#endif

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "CHECK failed: %s at %s:%d\n", \
                    #condition, __FILE__, __LINE__); \
            return 1; \
        } \
    } while (0)

typedef struct TerraLegacyZStream {
    uint8_t* next_in;
    uint32_t avail_in;
    uint32_t total_in;
    uint8_t* next_out;
    uint32_t avail_out;
    uint32_t total_out;
    void* msg;
    void* state;
    void* zalloc;
    void* zfree;
    void* opaque;
    int32_t data_type;
    uint32_t adler;
    uint32_t reserved;
} TerraLegacyZStream;

extern int terra_inflateInit2_(
    TerraLegacyZStream* stream,
    int window_bits,
    const char* version,
    int stream_size);
extern int terra_inflate(TerraLegacyZStream* stream, int flush);
extern int terra_inflateEnd(TerraLegacyZStream* stream);

static int create_gzip(
    const uint8_t* source,
    uint32_t source_len,
    uint8_t* output,
    uint32_t output_len,
    uint32_t* compressed_len) {
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    stream.next_in = (Bytef*)source;
    stream.avail_in = source_len;
    stream.next_out = output;
    stream.avail_out = output_len;

    int status = deflateInit2(
        &stream,
        Z_BEST_COMPRESSION,
        Z_DEFLATED,
        MAX_WBITS + 16,
        8,
        Z_DEFAULT_STRATEGY);
    CHECK(status == Z_OK);

    status = deflate(&stream, Z_FINISH);
    CHECK(status == Z_STREAM_END);
    CHECK(stream.total_out > 0u);
    CHECK(stream.total_out <= UINT32_MAX);
    *compressed_len = (uint32_t)stream.total_out;

    status = deflateEnd(&stream);
    CHECK(status == Z_OK);
    return 0;
}

int main(void) {
    static const uint8_t source[] =
        "terra zlib bridge preserves native LP64 stream layout";
    uint8_t compressed[256];
    uint8_t restored[256];
    uint32_t compressed_len = 0u;

    int status = create_gzip(
        source,
        (uint32_t)sizeof(source),
        compressed,
        (uint32_t)sizeof(compressed),
        &compressed_len);
    CHECK(status == 0);
    CHECK(compressed_len > 0u);

    TerraLegacyZStream stream;
    memset(&stream, 0, sizeof(stream));
    memset(restored, 0, sizeof(restored));
    stream.next_in = compressed;
    stream.avail_in = compressed_len;
    stream.next_out = restored;
    stream.avail_out = (uint32_t)sizeof(restored);

    status = terra_inflateInit2_(
        &stream,
        MAX_WBITS + 16,
        ZLIB_VERSION,
        (int)sizeof(stream));
    CHECK(status == Z_OK);
    CHECK(stream.state != NULL);

    status = terra_inflate(&stream, Z_FINISH);
    CHECK(status == Z_STREAM_END);
    CHECK(stream.total_in == compressed_len);
    CHECK(stream.total_out == sizeof(source));
    CHECK(memcmp(restored, source, sizeof(source)) == 0);

    status = terra_inflateEnd(&stream);
    CHECK(status == Z_OK);
    CHECK(stream.state == NULL);

    puts("terra_zlib_bridge_contract: ok");
    return 0;
}
