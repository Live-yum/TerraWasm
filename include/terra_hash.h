#ifndef TERRA_HASH_H
#define TERRA_HASH_H

#include <stdint.h>
#include "terra_status.h"

terrax_world_status terra_sha256(
    const uint8_t* input,
    uint32_t input_len,
    uint8_t* output_32);

/* Incremental hashing uses generation-tagged opaque handles. */
terrax_world_status terra_sha256_create(uint32_t* out_handle);
terrax_world_status terra_sha256_update(
    uint32_t handle,
    const uint8_t* input,
    uint32_t input_len);
terrax_world_status terra_sha256_final(
    uint32_t handle,
    uint8_t* output_32);
terrax_world_status terra_sha256_destroy(uint32_t handle);

#endif
