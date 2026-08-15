#include "terra_hash.h"

#include <string.h>

typedef struct {
    uint32_t state[8];
    uint64_t bit_length;
    uint8_t block[64];
    uint32_t block_length;
} TxSha256;

#define TX_SHA256_MAX_CONTEXTS 4u
#define TX_SHA256_HANDLE_SLOT_BITS 3u
#define TX_SHA256_HANDLE_SLOT_MASK ((1u << TX_SHA256_HANDLE_SLOT_BITS) - 1u)
#define TX_SHA256_HANDLE_GENERATION_MASK ((1u << (32u - TX_SHA256_HANDLE_SLOT_BITS)) - 1u)

typedef struct {
    TxSha256 context;
    uint8_t digest[32];
    uint32_t handle;
    uint8_t active;
    uint8_t finalized;
} TxSha256Slot;

static TxSha256Slot tx_sha256_slots[TX_SHA256_MAX_CONTEXTS];
static uint32_t tx_sha256_next_generation = 1u;

static const uint32_t tx_sha256_round_constants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotate_right(uint32_t value, uint32_t count) {
    return (value >> count) | (value << (32u - count));
}

static uint32_t read_be32(const uint8_t* value) {
    return ((uint32_t)value[0] << 24u) |
           ((uint32_t)value[1] << 16u) |
           ((uint32_t)value[2] << 8u) |
           (uint32_t)value[3];
}

static void write_be32(uint8_t* output, uint32_t value) {
    output[0] = (uint8_t)(value >> 24u);
    output[1] = (uint8_t)(value >> 16u);
    output[2] = (uint8_t)(value >> 8u);
    output[3] = (uint8_t)value;
}

static void sha256_transform(TxSha256* context, const uint8_t block[64]) {
    uint32_t schedule[64];
    for (uint32_t index = 0; index < 16u; ++index) {
        schedule[index] = read_be32(block + index * 4u);
    }
    for (uint32_t index = 16u; index < 64u; ++index) {
        uint32_t previous_15 = schedule[index - 15u];
        uint32_t previous_2 = schedule[index - 2u];
        uint32_t sigma0 = rotate_right(previous_15, 7u) ^
                          rotate_right(previous_15, 18u) ^
                          (previous_15 >> 3u);
        uint32_t sigma1 = rotate_right(previous_2, 17u) ^
                          rotate_right(previous_2, 19u) ^
                          (previous_2 >> 10u);
        schedule[index] = schedule[index - 16u] + sigma0 +
                          schedule[index - 7u] + sigma1;
    }

    uint32_t a = context->state[0];
    uint32_t b = context->state[1];
    uint32_t c = context->state[2];
    uint32_t d = context->state[3];
    uint32_t e = context->state[4];
    uint32_t f = context->state[5];
    uint32_t g = context->state[6];
    uint32_t h = context->state[7];

    for (uint32_t index = 0; index < 64u; ++index) {
        uint32_t sum1 = rotate_right(e, 6u) ^ rotate_right(e, 11u) ^ rotate_right(e, 25u);
        uint32_t choice = (e & f) ^ ((~e) & g);
        uint32_t temporary1 = h + sum1 + choice +
                              tx_sha256_round_constants[index] + schedule[index];
        uint32_t sum0 = rotate_right(a, 2u) ^ rotate_right(a, 13u) ^ rotate_right(a, 22u);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temporary2 = sum0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }

    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

static void sha256_init(TxSha256* context) {
    static const uint32_t initial_state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    memcpy(context->state, initial_state, sizeof(initial_state));
    context->bit_length = 0u;
    context->block_length = 0u;
}

static void sha256_update(TxSha256* context, const uint8_t* input, uint32_t input_len) {
    uint32_t offset = 0u;
    if (context->block_length != 0u) {
        uint32_t available = 64u - context->block_length;
        uint32_t take = input_len < available ? input_len : available;
        memcpy(context->block + context->block_length, input, take);
        context->block_length += take;
        offset += take;
        if (context->block_length == 64u) {
            sha256_transform(context, context->block);
            context->bit_length += 512u;
            context->block_length = 0u;
        }
    }
    while (input_len - offset >= 64u) {
        sha256_transform(context, input + offset);
        context->bit_length += 512u;
        offset += 64u;
    }
    if (offset < input_len) {
        context->block_length = input_len - offset;
        memcpy(context->block, input + offset, context->block_length);
    }
}

static void sha256_finish(TxSha256* context, uint8_t output[32]) {
    uint32_t index = context->block_length;
    context->block[index++] = 0x80u;
    if (index > 56u) {
        while (index < 64u) context->block[index++] = 0u;
        sha256_transform(context, context->block);
        index = 0u;
    }
    while (index < 56u) context->block[index++] = 0u;

    context->bit_length += (uint64_t)context->block_length * 8u;
    for (uint32_t offset = 0; offset < 8u; ++offset) {
        context->block[63u - offset] =
            (uint8_t)(context->bit_length >> (offset * 8u));
    }
    sha256_transform(context, context->block);
    for (uint32_t word = 0; word < 8u; ++word) {
        write_be32(output + word * 4u, context->state[word]);
    }
}

static int sha256_memory_range_is_valid(const void* pointer, uint32_t length) {
    if (!pointer) return length == 0u;
#if defined(__wasm__)
    uint64_t start = (uint64_t)(uintptr_t)pointer;
    uint64_t memory_size = (uint64_t)__builtin_wasm_memory_size(0) * 65536u;
    return start <= memory_size && (uint64_t)length <= memory_size - start;
#else
    (void)length;
    return 1;
#endif
}

static uint32_t sha256_next_handle(uint32_t slot_index) {
    uint32_t generation =
        tx_sha256_next_generation++ & TX_SHA256_HANDLE_GENERATION_MASK;
    if (generation == 0u) {
        generation = 1u;
        tx_sha256_next_generation = 2u;
    }
    return (generation << TX_SHA256_HANDLE_SLOT_BITS) | (slot_index + 1u);
}

static TxSha256Slot* sha256_get_slot(uint32_t handle) {
    uint32_t encoded_slot = handle & TX_SHA256_HANDLE_SLOT_MASK;
    if (!handle || encoded_slot == 0u || encoded_slot > TX_SHA256_MAX_CONTEXTS) {
        return NULL;
    }
    TxSha256Slot* slot = &tx_sha256_slots[encoded_slot - 1u];
    return slot->active && slot->handle == handle ? slot : NULL;
}

terrax_world_status terra_sha256_create(uint32_t* out_handle) {
    if (!sha256_memory_range_is_valid(out_handle, sizeof(*out_handle))) {
        return TERRAX_WORLD_STATUS_INVALID_ARGUMENT;
    }
    *out_handle = 0u;
    for (uint32_t index = 0u; index < TX_SHA256_MAX_CONTEXTS; ++index) {
        TxSha256Slot* slot = &tx_sha256_slots[index];
        if (slot->active) continue;
        memset(slot, 0, sizeof(*slot));
        sha256_init(&slot->context);
        slot->handle = sha256_next_handle(index);
        slot->active = 1u;
        *out_handle = slot->handle;
        return TERRAX_WORLD_STATUS_OK;
    }
    return TERRAX_WORLD_STATUS_STATE_ERROR;
}

terrax_world_status terra_sha256_update(
    uint32_t handle,
    const uint8_t* input,
    uint32_t input_len) {
    TxSha256Slot* slot = sha256_get_slot(handle);
    if (!slot) return TERRAX_WORLD_STATUS_STATE_ERROR;
    if (slot->finalized) return TERRAX_WORLD_STATUS_STATE_ERROR;
    if (!sha256_memory_range_is_valid(input, input_len)) {
        return TERRAX_WORLD_STATUS_INVALID_ARGUMENT;
    }
    if (input_len != 0u) sha256_update(&slot->context, input, input_len);
    return TERRAX_WORLD_STATUS_OK;
}

terrax_world_status terra_sha256_final(
    uint32_t handle,
    uint8_t* output_32) {
    TxSha256Slot* slot = sha256_get_slot(handle);
    if (!slot) return TERRAX_WORLD_STATUS_STATE_ERROR;
    if (!sha256_memory_range_is_valid(output_32, 32u)) {
        return TERRAX_WORLD_STATUS_INVALID_ARGUMENT;
    }
    if (!slot->finalized) {
        sha256_finish(&slot->context, slot->digest);
        slot->finalized = 1u;
    }
    memcpy(output_32, slot->digest, 32u);
    return TERRAX_WORLD_STATUS_OK;
}

terrax_world_status terra_sha256_destroy(uint32_t handle) {
    TxSha256Slot* slot = sha256_get_slot(handle);
    if (!slot) return TERRAX_WORLD_STATUS_STATE_ERROR;
    memset(slot, 0, sizeof(*slot));
    return TERRAX_WORLD_STATUS_OK;
}

terrax_world_status terra_sha256(
    const uint8_t* input,
    uint32_t input_len,
    uint8_t* output_32) {
    if (!sha256_memory_range_is_valid(input, input_len) ||
        !sha256_memory_range_is_valid(output_32, 32u)) {
        return TERRAX_WORLD_STATUS_INVALID_ARGUMENT;
    }
    TxSha256 context;
    sha256_init(&context);
    if (input_len != 0u) sha256_update(&context, input, input_len);
    sha256_finish(&context, output_32);
    return TERRAX_WORLD_STATUS_OK;
}
