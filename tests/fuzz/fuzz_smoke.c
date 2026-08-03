#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

int terra_fuzz_wld_open(const uint8_t* data, size_t size);
int terra_fuzz_json(const uint8_t* data, size_t size);

static void write_u16le(uint8_t* data, size_t offset, uint16_t value) {
    data[offset] = (uint8_t)value;
    data[offset + 1u] = (uint8_t)(value >> 8u);
}

static void write_u32le(uint8_t* data, size_t offset, uint32_t value) {
    data[offset] = (uint8_t)value;
    data[offset + 1u] = (uint8_t)(value >> 8u);
    data[offset + 2u] = (uint8_t)(value >> 16u);
    data[offset + 3u] = (uint8_t)(value >> 24u);
}

int main(void) {
    uint8_t seed[1024];
    for (size_t i = 0; i < sizeof(seed); ++i) seed[i] = (uint8_t)((i * 73u) ^ (i >> 2));
    if (terra_fuzz_wld_open(seed, sizeof(seed)) != 0) return 1;
    if (terra_fuzz_json(seed, sizeof(seed)) != 0) return 2;

    /* Version 88 with a valid format table and an overlong header string. */
    uint8_t overlong_header[16] = {0};
    write_u32le(overlong_header, 0u, 88u);
    write_u16le(overlong_header, 4u, 1u);
    write_u32le(overlong_header, 6u, 12u);
    write_u16le(overlong_header, 10u, 0u);
    overlong_header[12] = 0x7fu;
    if (terra_fuzz_wld_open(overlong_header, sizeof(overlong_header)) != 0) return 3;

    puts("fuzz smoke: parser and JSON harnesses completed");
    return 0;
}
