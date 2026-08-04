#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

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

static uint32_t create_gzip(
    const uint8_t* source,
    uint32_t source_len,
    uint8_t* output,
    uint32_t output_len) {
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    stream.next_in = (Bytef*)source;
    stream.avail_in = source_len;
    stream.next_out = output;
    stream.avail_out = output_len;

    assert(deflateInit2(
        &stream,
        Z_BEST_COMPRESSION,
        Z_DEFLATED,
        MAX_WBITS + 16,
        8,
        Z_DEFAULT_STRATEGY) == Z_OK);
    assert(deflate(&stream, Z_FINISH) == Z_STREAM_END);
    uint32_t compressed_len = (uint32_t)stream.total_out;
    assert(deflateEnd(&stream) == Z_OK);
    return compressed_len;
}

int main(void) {
    static const uint8_t source[] =
        "terra zlib bridge preserves native LP64 stream layout";
    uint8_t compressed[256];
    uint8_t restored[256];
    uint32_t compressed_len = create_gzip(
        source,
        (uint32_t)sizeof(source),
        compressed,
        (uint32_t)sizeof(compressed));

    TerraLegacyZStream stream;
    memset(&stream, 0, sizeof(stream));
    stream.next_in = compressed;
    stream.avail_in = compressed_len;
    stream.next_out = restored;
    stream.avail_out = (uint32_t)sizeof(restored);

    assert(terra_inflateInit2_(
        &stream,
        MAX_WBITS + 16,
        ZLIB_VERSION,
        (int)sizeof(stream)) == Z_OK);
    assert(terra_inflate(&stream, Z_FINISH) == Z_STREAM_END);
    assert(stream.total_in == compressed_len);
    assert(stream.total_out == sizeof(source));
    assert(memcmp(restored, source, sizeof(source)) == 0);
    assert(terra_inflateEnd(&stream) == Z_OK);
    assert(stream.state == NULL);

    puts("terra_zlib_bridge_contract: ok");
    return 0;
}
