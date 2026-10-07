/*
 * terra_abi.c -- Stable artifact identity queries.
 */
#include "terra_abi.h"

extern char tx_last_error[256];
extern uint32_t tx_strlen(const char *s);

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

#ifndef TERRAWASM_FEATURE_SET
#define TERRAWASM_FEATURE_SET "all"
#endif
#ifndef TERRAWASM_FEATURE_WLD
#define TERRAWASM_FEATURE_WLD 1
#endif
#ifndef TERRAWASM_FEATURE_PLR
#define TERRAWASM_FEATURE_PLR 1
#endif
#ifndef TERRAWASM_VIEWER_WEB_PROFILE
#define TERRAWASM_VIEWER_WEB_PROFILE 0
#endif

#if TERRAWASM_VIEWER_WEB_PROFILE
#define TERRAX_VIEWER_WEB_PROFILE_JSON "true"
#else
#define TERRAX_VIEWER_WEB_PROFILE_JSON "false"
#endif

#define TERRAX_STRINGIFY_VALUE(value) #value
#define TERRAX_STRINGIFY(value) TERRAX_STRINGIFY_VALUE(value)

#if TERRAWASM_FEATURE_WLD && TERRAWASM_FEATURE_PLR
static const char g_capabilities[] =
    "{\"version\":1,\"features\":["
    "\"world-buffer-io\",\"json-sections\",\"preview-rgba\","
    "\"thumbnail-png\",\"map-output\",\"pixel-art\",\"sha256\","
    "\"plr-read-write\",\"circuit-traversal\",\"circuit-world\"]}";
#elif TERRAWASM_FEATURE_WLD
static const char g_capabilities[] =
    "{\"version\":1,\"features\":["
    "\"world-buffer-io\",\"json-sections\",\"preview-rgba\","
    "\"thumbnail-png\",\"map-output\",\"pixel-art\",\"sha256\",\"circuit-traversal\",\"circuit-world\"]}";
#else
static const char g_capabilities[] =
    "{\"version\":1,\"features\":[\"plr-read-write\"]}";
#endif

#if TERRAWASM_FEATURE_WLD
#define TERRAX_STREAM_IDENTITY_JSON ",\"stream\":{\"version\":2,\"inputLease\":true,\"editPlan\":true,\"pngColumnCursors\":true,\"stampTiles\":true}"
#else
#define TERRAX_STREAM_IDENTITY_JSON ""
#endif

static const char g_build_info_json[] =
    "{\"abiVersion\":" TERRAX_STRINGIFY(TERRAX_ABI_VERSION)
    TERRAX_STREAM_IDENTITY_JSON
    ",\"worldWorkspaceAbiVersion\":" TERRAX_STRINGIFY(TERRAWASM_FEATURE_WLD)
    ",\"pixelWorkspaceAbiVersion\":" TERRAX_STRINGIFY(TERRAWASM_FEATURE_WLD)
    ",\"circuitAbiVersion\":" TERRAX_STRINGIFY(TERRAWASM_FEATURE_WLD)
    ",\"circuitWorldAbiVersion\":" TERRAX_STRINGIFY(TERRAWASM_FEATURE_WLD)
    ",\"playerWorkspaceAbiVersion\":" TERRAX_STRINGIFY(TERRAWASM_FEATURE_PLR)
    ",\"sourceCommit\":\"" TERRAX_BUILD_COMMIT "\""
    ",\"dirty\":" TERRAX_STRINGIFY(TERRAX_BUILD_DIRTY)
    ",\"compiler\":\"" TERRAX_BUILD_COMPILER "\""
    ",\"target\":\"" TERRAX_BUILD_TARGET "\""
    ",\"featureSet\":\"" TERRAWASM_FEATURE_SET "\""
    ",\"viewerWebProfile\":" TERRAX_VIEWER_WEB_PROFILE_JSON
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

terrax_world_status terra_info_get_last_error_json(
    char* buffer,
    uint64_t buffer_size,
    uint64_t* required_size) {
    uint32_t len = tx_strlen(tx_last_error);
    uint64_t needed = (uint64_t)len + 1u;
    if (required_size) *required_size = needed;
    if (!buffer || buffer_size == 0u) return TERRAX_WORLD_STATUS_OK;
    if (buffer_size < needed) return TERRAX_WORLD_STATUS_BUFFER_TOO_SMALL;
    for (uint32_t i = 0u; i <= len; i++) buffer[i] = tx_last_error[i];
    return TERRAX_WORLD_STATUS_OK;
}
