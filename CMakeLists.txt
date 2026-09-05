cmake_minimum_required(VERSION 3.27)
project(TerraWasm C)

include(CTest)
option(TERRAWASM_NATIVE_TESTS "Build the host-native ABI smoke test" OFF)
option(TERRAWASM_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer for native targets" OFF)
option(TERRAWASM_FUZZ_SMOKE "Build bounded-reader fuzz smoke harnesses with native tests" ON)
option(TERRAWASM_VIEWER_WEB_PROFILE "Trim unused JSON operations from the viewer WLD Web target" OFF)

set(TERRAWASM_FEATURE_SET "all" CACHE STRING "Compiled capability set: all, wld, or plr")
set_property(CACHE TERRAWASM_FEATURE_SET PROPERTY STRINGS all wld plr)
string(TOLOWER "${TERRAWASM_FEATURE_SET}" TERRAWASM_FEATURE_SET)
if(NOT TERRAWASM_FEATURE_SET MATCHES "^(all|wld|plr)$")
    message(FATAL_ERROR "TERRAWASM_FEATURE_SET must be one of: all, wld, plr")
endif()
if(TERRAWASM_VIEWER_WEB_PROFILE AND NOT TERRAWASM_FEATURE_SET STREQUAL "wld")
    message(FATAL_ERROR "TERRAWASM_VIEWER_WEB_PROFILE requires TERRAWASM_FEATURE_SET=wld")
endif()
set(TERRAWASM_ENABLE_WLD OFF)
set(TERRAWASM_ENABLE_PLR OFF)
if(TERRAWASM_FEATURE_SET STREQUAL "all" OR TERRAWASM_FEATURE_SET STREQUAL "wld")
    set(TERRAWASM_ENABLE_WLD ON)
endif()
if(TERRAWASM_FEATURE_SET STREQUAL "all" OR TERRAWASM_FEATURE_SET STREQUAL "plr")
    set(TERRAWASM_ENABLE_PLR ON)
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(INC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Include headers from include/ directory
include_directories(${INC_DIR})

set(COMMON_SOURCES
    "${SRC_DIR}/terra_abi.c"
    "${SRC_DIR}/terra_reader.c"
    "${SRC_DIR}/terra_mem.c"
)
set(WLD_SOURCES
    "${SRC_DIR}/terra_commands.c"
    "${SRC_DIR}/terra_task.c"
    "${SRC_DIR}/terra_json.c"
    "${SRC_DIR}/terra_wld.c"
    "${SRC_DIR}/terra_wld_guard.c"
    "${SRC_DIR}/terra_api.c"
    "${SRC_DIR}/terra_commit.c"
    "${SRC_DIR}/terra_ops.c"
    "${SRC_DIR}/terra_mutators.c"
    "${SRC_DIR}/terra_render.c"
    "${SRC_DIR}/terra_render_png_lowmem.c"
    "${SRC_DIR}/terra_map.c"
    "${SRC_DIR}/terra_icon.c"
    "${SRC_DIR}/terra_update.c"
    "${SRC_DIR}/terra_txci.c"
    "${SRC_DIR}/terra_pixel_art.c"
    "${SRC_DIR}/terra_pixel_art_bulk_fast.c"
    "${SRC_DIR}/terra_hash.c"
)
set(PLR_SOURCES
    "${SRC_DIR}/terra_plr.c"
)
set(SOURCES ${COMMON_SOURCES})
if(TERRAWASM_ENABLE_WLD)
    list(APPEND SOURCES ${WLD_SOURCES})
    # Keep the established decoder internal and expose only the guarded entry point.
    set_source_files_properties(
        "${SRC_DIR}/terra_wld.c"
        PROPERTIES COMPILE_DEFINITIONS "parse_header=terra_parse_header_unchecked"
    )
    # The public unmarked PNG entry point is permanently the low-memory renderer.
endif()
if(TERRAWASM_ENABLE_PLR)
    list(APPEND SOURCES ${PLR_SOURCES})
endif()

set(TERRAX_BUILD_COMMIT "unknown" CACHE STRING "Source commit embedded in ABI identity")
set(TERRAX_BUILD_DIRTY "false" CACHE STRING "Dirty state embedded in ABI identity")
set(TERRAX_BUILD_COMPILER "native" CACHE STRING "Compiler identity embedded in ABI identity")
set(TERRAX_OPTIMIZE_FLAG "-O3" CACHE STRING "Optimization flag for release-style Wasm builds")
set(TERRAX_NODE_INITIAL_MEMORY "134217728" CACHE STRING "Node WASM initial linear memory in bytes")
set(TERRAX_NODE_MAXIMUM_MEMORY "536870912" CACHE STRING "Node WASM maximum linear memory in bytes")
set(TERRAX_WEB_INITIAL_MEMORY "67108864" CACHE STRING "Web/Mini Program WASM initial linear memory in bytes")
set(TERRAX_WEB_MAXIMUM_MEMORY "167772160" CACHE STRING "Web/Mini Program WASM maximum linear memory in bytes")
option(TERRAX_ENABLE_LTO "Enable link-time optimization for Wasm targets" OFF)

foreach(TERRAX_MEMORY_VALUE IN ITEMS
    TERRAX_NODE_INITIAL_MEMORY
    TERRAX_NODE_MAXIMUM_MEMORY
    TERRAX_WEB_INITIAL_MEMORY
    TERRAX_WEB_MAXIMUM_MEMORY
)
    if(NOT "${${TERRAX_MEMORY_VALUE}}" MATCHES "^[0-9]+$")
        message(FATAL_ERROR "${TERRAX_MEMORY_VALUE} must be an integer byte count")
    endif()
    math(EXPR TERRAX_MEMORY_REMAINDER "${${TERRAX_MEMORY_VALUE}} % 65536")
    if(NOT TERRAX_MEMORY_REMAINDER EQUAL 0)
        message(FATAL_ERROR "${TERRAX_MEMORY_VALUE} must be WebAssembly-page aligned (65536 bytes)")
    endif()
endforeach()
if(TERRAX_NODE_INITIAL_MEMORY GREATER TERRAX_NODE_MAXIMUM_MEMORY)
    message(FATAL_ERROR "TERRAX_NODE_INITIAL_MEMORY cannot exceed TERRAX_NODE_MAXIMUM_MEMORY")
endif()
if(TERRAX_WEB_INITIAL_MEMORY GREATER TERRAX_WEB_MAXIMUM_MEMORY)
    message(FATAL_ERROR "TERRAX_WEB_INITIAL_MEMORY cannot exceed TERRAX_WEB_MAXIMUM_MEMORY")
endif()

function(json_escape out_var value)
    string(REPLACE "\\" "\\\\" escaped "${value}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    set(${out_var} "${escaped}" PARENT_SCOPE)
endfunction()

function(list_to_json out_var)
    set(result "[")
    set(first_item TRUE)
    foreach(item IN LISTS ARGN)
        json_escape(escaped "${item}")
        if(first_item)
            set(first_item FALSE)
        else()
            string(APPEND result ",")
        endif()
        string(APPEND result "\"${escaped}\"")
    endforeach()
    string(APPEND result "]")
    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

function(configure_terra_identity target target_name initial_memory maximum_memory common_flags_text target_flags_text)
    target_compile_definitions(${target} PRIVATE
        TERRAX_BUILD_COMMIT="${TERRAX_BUILD_COMMIT}"
        TERRAX_BUILD_DIRTY=${TERRAX_BUILD_DIRTY}
        TERRAX_BUILD_COMPILER="${TERRAX_BUILD_COMPILER}"
        TERRAX_BUILD_TARGET="${target_name}"
        TERRAX_INITIAL_MEMORY=${initial_memory}
        TERRAX_MAXIMUM_MEMORY=${maximum_memory}
        TERRAX_BUILD_COMMON_FLAGS_TEXT="${common_flags_text}"
        TERRAX_BUILD_TARGET_FLAGS_TEXT="${target_flags_text}"
        TERRAWASM_FEATURE_SET="${TERRAWASM_FEATURE_SET}"
        TERRAWASM_FEATURE_WLD=$<BOOL:${TERRAWASM_ENABLE_WLD}>
        TERRAWASM_FEATURE_PLR=$<BOOL:${TERRAWASM_ENABLE_PLR}>
    )
endfunction()

if(EMSCRIPTEN)
    # Header/format and player patching are cold administrative paths. Keep
    # the global release profile at -O3, but size-optimize enabled schema-heavy
    # encoders so each feature set remains inside the Mini Program budget.
    if(TERRAWASM_ENABLE_WLD)
        set_source_files_properties(
            "${SRC_DIR}/terra_mutators.c"
            PROPERTIES COMPILE_OPTIONS "-Oz"
        )
    endif()
    if(TERRAWASM_ENABLE_PLR)
        set_source_files_properties(
            "${SRC_DIR}/terra_plr.c"
            PROPERTIES COMPILE_OPTIONS "-Oz"
        )
    endif()

    if(TERRAWASM_FEATURE_SET STREQUAL "all")
        set(NODE_EXPORTS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/exports.txt")
        set(WEB_EXPORTS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/exports.web.txt")
    elseif(TERRAWASM_FEATURE_SET STREQUAL "wld")
        set(NODE_EXPORTS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/exports.wld.txt")
        set(WEB_EXPORTS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/exports.wld.web.txt")
    else()
        set(NODE_EXPORTS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/exports.plr.txt")
        set(WEB_EXPORTS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/exports.plr.txt")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${NODE_EXPORTS_FILE}"
        "${WEB_EXPORTS_FILE}"
    )

    function(write_exports_json input_file output_file)
        file(STRINGS "${input_file}" export_lines)
        list(REMOVE_DUPLICATES export_lines)
        set(export_items)
        foreach(export_fn ${export_lines})
            list(APPEND export_items "\"${export_fn}\"")
        endforeach()
        list(JOIN export_items "," export_json_body)
        file(WRITE "${output_file}" "[${export_json_body}]")
    endfunction()

    write_exports_json(
        "${NODE_EXPORTS_FILE}"
        "${CMAKE_CURRENT_BINARY_DIR}/exported_functions_node.json"
    )
    write_exports_json(
        "${WEB_EXPORTS_FILE}"
        "${CMAKE_CURRENT_BINARY_DIR}/exported_functions_web.json"
    )

    set(COMMON_COMPILE_OPTIONS
        "${TERRAX_OPTIMIZE_FLAG}"
        -fno-exceptions
        -fno-rtti
    )
    set(COMMON_LINK_OPTIONS)
    if(TERRAWASM_ENABLE_WLD)
        list(APPEND COMMON_LINK_OPTIONS "-sUSE_ZLIB=1")
    endif()
    list(APPEND COMMON_LINK_OPTIONS
        "-sALLOW_MEMORY_GROWTH=1"
        "-sMODULARIZE=1"
        "-sERROR_ON_UNDEFINED_SYMBOLS=1"
        "--no-entry"
    )
    set(COMMON_MANIFEST_FLAGS
        "${TERRAX_OPTIMIZE_FLAG}"
        -fno-exceptions
        -fno-rtti
        ${COMMON_LINK_OPTIONS}
    )
    if(TERRAX_ENABLE_LTO)
        list(APPEND COMMON_COMPILE_OPTIONS -flto)
        list(APPEND COMMON_LINK_OPTIONS -flto)
        list(APPEND COMMON_MANIFEST_FLAGS -flto)
    endif()

    set(WIDE_RUNTIME_METHODS "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','stringToUTF8','lengthBytesUTF8','getValue','setValue','HEAPU8','HEAPU32','HEAP32','HEAPF32','HEAPF64','FS','stackAlloc','stackSave','stackRestore','wasmMemory']")
    if(TERRAWASM_FEATURE_SET STREQUAL "wld")
        set(WEB_RUNTIME_METHODS "-sEXPORTED_RUNTIME_METHODS=['HEAPU8','HEAPU32']")
    else()
        set(WEB_RUNTIME_METHODS "${WIDE_RUNTIME_METHODS}")
    endif()

    # The WLD-only Web target is buffer-only in the viewer. Keep the broader
    # all/plr Web targets unchanged so this experiment cannot alter Node or
    # the other capability sets.
    set(WEB_FILESYSTEM_FLAG "-sFILESYSTEM=1")
    if(TERRAWASM_FEATURE_SET STREQUAL "wld")
        set(WEB_FILESYSTEM_FLAG "-sFILESYSTEM=0")
    endif()

    set(NODE_LINK_OPTIONS
        "SHELL:-sEXPORTED_FUNCTIONS=@${CMAKE_CURRENT_BINARY_DIR}/exported_functions_node.json"
        "${WIDE_RUNTIME_METHODS}"
        "-sINITIAL_MEMORY=${TERRAX_NODE_INITIAL_MEMORY}"
        "-sMAXIMUM_MEMORY=${TERRAX_NODE_MAXIMUM_MEMORY}"
        "-sEXPORT_NAME='TerraWorldWasm'"
        "-sENVIRONMENT=node"
        "-sNODERAWFS=1"
        "-sFILESYSTEM=1"
    )
    set(WEB_LINK_OPTIONS
        "SHELL:-sEXPORTED_FUNCTIONS=@${CMAKE_CURRENT_BINARY_DIR}/exported_functions_web.json"
        "${WEB_RUNTIME_METHODS}"
        "-sINITIAL_MEMORY=${TERRAX_WEB_INITIAL_MEMORY}"
        "-sMAXIMUM_MEMORY=${TERRAX_WEB_MAXIMUM_MEMORY}"
        "-sEXPORT_NAME='TerraWorldWasmWeb'"
        "-sENVIRONMENT=web,worker"
        "${WEB_FILESYSTEM_FLAG}"
    )
    set(NODE_IDENTITY_FLAGS ${NODE_LINK_OPTIONS})
    list(POP_FRONT NODE_IDENTITY_FLAGS)
    list(PREPEND NODE_IDENTITY_FLAGS "-sEXPORTED_FUNCTIONS=@exported_functions_node.json")
    set(WEB_IDENTITY_FLAGS ${WEB_LINK_OPTIONS})
    list(POP_FRONT WEB_IDENTITY_FLAGS)
    list(PREPEND WEB_IDENTITY_FLAGS "-sEXPORTED_FUNCTIONS=@exported_functions_web.json")
    string(JOIN " @@ " COMMON_FLAGS_TEXT ${COMMON_MANIFEST_FLAGS})
    string(JOIN " @@ " NODE_FLAGS_TEXT ${NODE_IDENTITY_FLAGS})
    string(JOIN " @@ " WEB_FLAGS_TEXT ${WEB_IDENTITY_FLAGS})

    # ---- Node target ----
    add_executable(terrax_world_wasm ${SOURCES})
    configure_terra_identity(
        terrax_world_wasm "node"
        ${TERRAX_NODE_INITIAL_MEMORY} ${TERRAX_NODE_MAXIMUM_MEMORY}
        "${COMMON_FLAGS_TEXT}" "${NODE_FLAGS_TEXT}"
    )
    target_compile_options(terrax_world_wasm PRIVATE ${COMMON_COMPILE_OPTIONS})
    target_link_options(terrax_world_wasm PRIVATE
        "${TERRAX_OPTIMIZE_FLAG}"
        ${COMMON_LINK_OPTIONS}
        ${NODE_LINK_OPTIONS}
    )

    # ---- Web/MiniProgram target ----
    # 注意：不要用 SINGLE_FILE，小程序 WXWebAssembly.instantiate 只接受 .wasm 文件路径
    add_executable(terrax_world_wasm_web ${SOURCES})
    configure_terra_identity(
        terrax_world_wasm_web "web"
        ${TERRAX_WEB_INITIAL_MEMORY} ${TERRAX_WEB_MAXIMUM_MEMORY}
        "${COMMON_FLAGS_TEXT}" "${WEB_FLAGS_TEXT}"
    )
    if(TERRAWASM_VIEWER_WEB_PROFILE)
        target_compile_definitions(terrax_world_wasm_web PRIVATE TERRAWASM_VIEWER_WEB_PROFILE=1)
    endif()
    target_compile_options(terrax_world_wasm_web PRIVATE ${COMMON_COMPILE_OPTIONS})
    target_link_options(terrax_world_wasm_web PRIVATE
        "${TERRAX_OPTIMIZE_FLAG}"
        ${COMMON_LINK_OPTIONS}
        ${WEB_LINK_OPTIONS}
    )
else()
    set(NATIVE_SOURCES ${SOURCES})
    if(TERRAWASM_ENABLE_WLD)
        # Native LP64 zlib uses wider total counters than the legacy WASM-shaped
        # declaration in terra_txci.c. Compile native TXCI through the bridge.
        set_source_files_properties(
            "${SRC_DIR}/terra_txci.c"
            PROPERTIES COMPILE_DEFINITIONS "inflateInit2_=terra_inflateInit2_;inflate=terra_inflate;inflateEnd=terra_inflateEnd"
        )
        list(APPEND NATIVE_SOURCES "${SRC_DIR}/terra_zlib_bridge.c")
    endif()

    add_library(terrax_world_static STATIC ${NATIVE_SOURCES})
    configure_terra_identity(terrax_world_static "native" 0 0 "" "")
    target_compile_definitions(terrax_world_static PRIVATE TERRAX_STATIC)
    if(TERRAWASM_SANITIZERS)
        target_compile_options(terrax_world_static PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(terrax_world_static PRIVATE -fsanitize=address,undefined)
    endif()

    if(TERRAWASM_NATIVE_TESTS)
        target_compile_definitions(terrax_world_static PRIVATE TERRAX_TESTING)

        function(add_terra_contract target source)
            add_executable(${target} ${source})
            target_link_libraries(${target} PRIVATE terrax_world_static ${ARGN})
            target_compile_definitions(${target} PRIVATE TERRAX_STATIC)
            target_compile_options(${target} PRIVATE -UNDEBUG)
            if(TERRAWASM_SANITIZERS)
                target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
                target_link_options(${target} PRIVATE -fsanitize=address,undefined)
            endif()
            add_test(NAME ${target} COMMAND ${target})
        endfunction()

        if(TERRAWASM_ENABLE_WLD)
            find_package(ZLIB REQUIRED)
            add_terra_contract(terra_native_contract tests/native_contract.c ZLIB::ZLIB)
            target_compile_definitions(terra_native_contract PRIVATE
                TERRAX_TESTING
                TERRAX_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests"
            )
            add_terra_contract(terra_error_json_contract tests/error_json_contract.c ZLIB::ZLIB)
            add_terra_contract(terra_zlib_bridge_contract tests/zlib_bridge_contract.c ZLIB::ZLIB)
            add_terra_contract(terra_json_parser_contract tests/json_parser_contract.c ZLIB::ZLIB)
            add_terra_contract(terra_string_section_guard_contract tests/string_section_guard_contract.c ZLIB::ZLIB)
            add_terra_contract(terra_marker_color_index_contract tests/marker_color_index_contract.c ZLIB::ZLIB)

            if(TERRAWASM_FUZZ_SMOKE)
                add_executable(terra_fuzz_smoke
                    tests/fuzz/fuzz_smoke.c
                    tests/fuzz/fuzz_wld_open.c
                    tests/fuzz/fuzz_json.c
                )
                target_link_libraries(terra_fuzz_smoke PRIVATE terrax_world_static ZLIB::ZLIB)
                target_compile_definitions(terra_fuzz_smoke PRIVATE TERRAX_STATIC)
                target_compile_options(terra_fuzz_smoke PRIVATE -UNDEBUG)
                if(TERRAWASM_SANITIZERS)
                    target_compile_options(terra_fuzz_smoke PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
                    target_link_options(terra_fuzz_smoke PRIVATE -fsanitize=address,undefined)
                endif()
                add_test(NAME terra_fuzz_smoke COMMAND terra_fuzz_smoke)
            endif()
        endif()

        if(TERRAWASM_ENABLE_PLR)
            # Generate an encrypted PLR from semantic JSON so standalone CI
            # always exercises parsing, editing, encryption, and allocator isolation.
            set(TERRAWASM_PLR_JSON_FIXTURE_PATH
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/minimal-player.json"
                CACHE FILEPATH "Semantic JSON fixture used by the native PLR contract")
            if(NOT EXISTS "${TERRAWASM_PLR_JSON_FIXTURE_PATH}")
                message(FATAL_ERROR
                    "Native PLR contract fixture is missing: ${TERRAWASM_PLR_JSON_FIXTURE_PATH}")
            endif()
            file(TO_CMAKE_PATH
                "${TERRAWASM_PLR_JSON_FIXTURE_PATH}"
                TERRAWASM_PLR_JSON_FIXTURE_DEFINE)
            add_terra_contract(terra_plr_contract tests/plr_contract.c)
            target_compile_definitions(terra_plr_contract PRIVATE
                TERRAWASM_PLR_JSON_FIXTURE_PATH="${TERRAWASM_PLR_JSON_FIXTURE_DEFINE}"
            )

            set(TERRAWASM_PLR_REAL_FIXTURE_PATH
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/files/烟花.plr"
                CACHE FILEPATH "Real Terraria PLR fixture used by native compatibility tests")
            if(NOT EXISTS "${TERRAWASM_PLR_REAL_FIXTURE_PATH}")
                message(FATAL_ERROR
                    "Real PLR compatibility fixture is missing: ${TERRAWASM_PLR_REAL_FIXTURE_PATH}")
            endif()
            file(TO_CMAKE_PATH
                "${TERRAWASM_PLR_REAL_FIXTURE_PATH}"
                TERRAWASM_PLR_REAL_FIXTURE_DEFINE)
            add_terra_contract(terra_plr_real_fixture_contract tests/plr_real_fixture_contract.c)
            target_compile_definitions(terra_plr_real_fixture_contract PRIVATE
                TERRAWASM_PLR_REAL_FIXTURE_PATH="${TERRAWASM_PLR_REAL_FIXTURE_DEFINE}"
            )
        endif()
    endif()
endif()
