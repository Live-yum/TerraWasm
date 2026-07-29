#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

int terra_fuzz_wld_open(const uint8_t* data, size_t size);
int terra_fuzz_json(const uint8_t* data, size_t size);

int main(void) {
    uint8_t seed[1024];
    for (size_t i = 0; i < sizeof(seed); ++i) seed[i] = (uint8_t)((i * 73u) ^ (i >> 2));
    if (terra_fuzz_wld_open(seed, sizeof(seed)) != 0) return 1;
    if (terra_fuzz_json(seed, sizeof(seed)) != 0) return 2;
    puts("fuzz smoke: bounded reader harnesses completed");
    return 0;
}
