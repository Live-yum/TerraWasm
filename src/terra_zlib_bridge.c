/*
 * terra_zlib_bridge.c -- Adapt the legacy WASM-shaped TXCI stream to the
 * platform zlib ABI. Native LP64 zlib uses wider total counters than WASM32,
 * so passing the hand-written struct directly can corrupt adjacent fields.
 */
#include <stdint.h>
#include <stdlib.h>
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

typedef struct TerraZlibContext {
    z_stream stream;
} TerraZlibContext;

static uint32_t terra_u32_counter(uLong value) {
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

static void terra_sync_to_zlib(TerraLegacyZStream* legacy, z_stream* stream) {
    stream->next_in = legacy->next_in;
    stream->avail_in = legacy->avail_in;
    stream->next_out = legacy->next_out;
    stream->avail_out = legacy->avail_out;
}

static void terra_sync_from_zlib(TerraLegacyZStream* legacy, z_stream* stream) {
    legacy->next_in = stream->next_in;
    legacy->avail_in = stream->avail_in;
    legacy->total_in = terra_u32_counter(stream->total_in);
    legacy->next_out = stream->next_out;
    legacy->avail_out = stream->avail_out;
    legacy->total_out = terra_u32_counter(stream->total_out);
    legacy->msg = stream->msg;
    legacy->data_type = stream->data_type;
    legacy->adler = terra_u32_counter(stream->adler);
}

int terra_inflateInit2_(
    TerraLegacyZStream* legacy,
    int window_bits,
    const char* ignored_version,
    int ignored_stream_size) {
    (void)ignored_version;
    (void)ignored_stream_size;
    if (!legacy || legacy->state) return Z_STREAM_ERROR;

    TerraZlibContext* context = (TerraZlibContext*)calloc(1u, sizeof(TerraZlibContext));
    if (!context) return Z_MEM_ERROR;
    terra_sync_to_zlib(legacy, &context->stream);

    int status = inflateInit2(&context->stream, window_bits);
    if (status != Z_OK) {
        free(context);
        return status;
    }

    legacy->state = context;
    terra_sync_from_zlib(legacy, &context->stream);
    return Z_OK;
}

int terra_inflate(TerraLegacyZStream* legacy, int flush) {
    if (!legacy || !legacy->state) return Z_STREAM_ERROR;
    TerraZlibContext* context = (TerraZlibContext*)legacy->state;
    terra_sync_to_zlib(legacy, &context->stream);
    int status = inflate(&context->stream, flush);
    terra_sync_from_zlib(legacy, &context->stream);
    return status;
}

int terra_inflateEnd(TerraLegacyZStream* legacy) {
    if (!legacy || !legacy->state) return Z_STREAM_ERROR;
    TerraZlibContext* context = (TerraZlibContext*)legacy->state;
    int status = inflateEnd(&context->stream);
    terra_sync_from_zlib(legacy, &context->stream);
    legacy->state = NULL;
    free(context);
    return status;
}
