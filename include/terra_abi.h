/*
 * terra_abi.h -- Stable artifact identity queries.
 *
 * These functions are intentionally tiny and allocation-free so the same
 * identity contract can be queried from Node, Web, and native test builds.
 */
#ifndef TERRA_ABI_H
#define TERRA_ABI_H

#include <stdint.h>
#include "terra_status.h"

uint32_t terra_abi_version(void);
const char* terra_capabilities(void);
const char* terra_build_info_json(void);

/* Common structured error query available in all/wld/plr feature builds. */
terrax_world_status terra_info_get_last_error_json(
    char* buffer,
    uint64_t buffer_size,
    uint64_t* required_size);

#endif /* TERRA_ABI_H */
