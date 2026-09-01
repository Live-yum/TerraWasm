/*
 * terra_abi.c -- Stable artifact identity queries.
 */
#include "terra_abi.h"

#ifndef TERRAX_ABI_VERSION
#define TERRAX_ABI_VERSION 1
#endif

#ifndef TERRAX_BUILD_COMMIT
#define TERRAX_BUILD_COMMIT "unknown"
#endif

#ifndef TERRAX_BUILD_DIRTY
#define TERRAX_BUILD_DIRTY false
#endif

#ifndef TERRAX_BUILD_COMPILER
#define TERRAX_BUILD_COMPILER "unknown"
#endif

#ifndef TERRAX_BUILD_COMMON_FLAGS_TEXT
#define TERRAX_BUILD_COMMON_FLAGS_TEXT ""
#endif

#ifndef TERRAX_BUILD_TARGET_FLAGS_TEXT
#define TERRAX_BUILD_TARGET_FLAGS_TEXT ""
#endif

#ifndef TERRAX_BUILD_TARGET
#define TERRAX_BUILD_TARGET "unknown"
#endif

#ifndef TERRAX_INITIAL_MEMORY
#define TERRAX_INITIAL_MEMORY 0
#endif

#ifndef TERRAX_MAXIMUM_MEMORY
#define TERRAX_MAXIMUM_MEMORY 0
#endif

#define TERRAX_STRINGIFY_VALUE(value) #value
#define TERRAX_STRINGIFY(value) TERRAX_STRINGIFY_VALUE(value)

static const char g_capabilities[] =
    "{\"version\":1,\"features\":["
    "\"world-buffer-io\","
    "\"json-sections\","
    "\"preview-rgba\","
    "\"thumbnail-png\","
    "\"map-output\","
    "\"pixel-art\","
    "\"sha256\","
    "\"plr-read-write\""
    "]}";

static const char g_build_info_json[] =
    "{\"abiVersion\":" TERRAX_STRINGIFY(TERRAX_ABI_VERSION)
    ",\"sourceCommit\":\"" TERRAX_BUILD_COMMIT "\""
    ",\"dirty\":" TERRAX_STRINGIFY(TERRAX_BUILD_DIRTY)
    ",\"compiler\":\"" TERRAX_BUILD_COMPILER "\""
    ",\"target\":\"" TERRAX_BUILD_TARGET "\""
    ",\"initialMemory\":" TERRAX_STRINGIFY(TERRAX_INITIAL_MEMORY)
    ",\"maxMemory\":" TERRAX_STRINGIFY(TERRAX_MAXIMUM_MEMORY)
    ",\"commonFlagsText\":\"" TERRAX_BUILD_COMMON_FLAGS_TEXT "\""
    ",\"targetFlagsText\":\"" TERRAX_BUILD_TARGET_FLAGS_TEXT "\""
    "}";

uint32_t terra_abi_version(void) {
    return TERRAX_ABI_VERSION;
}

const char* terra_capabilities(void) {
    return g_capabilities;
}

const char* terra_build_info_json(void) {
    return g_build_info_json;
}
