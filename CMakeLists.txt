cmake_minimum_required(VERSION 3.27)
project(TerraWasm C)

include(CTest)
option(TERRAWASM_NATIVE_TESTS "Build the host-native ABI smoke test" OFF)
option(TERRAWASM_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer for native targets" OFF)
option(TERRAWASM_FUZZ_SMOKE "Build bounded-reader fuzz smoke harnesses with native tests" ON)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(INC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Include headers from include/ directory
include_directories(${INC_DIR})

set(SOURCES
    "${SRC_DIR}/terra_abi.c"
    "${SRC_DIR}/terra_reader.c"
    "${SRC_DIR}/terra_commands.c"
    "${SRC_DIR}/terra_task.c"
    "${SRC_DIR}/terra_mem.c"
    "${SRC_DIR}/terra_json.c"
    "${SRC_DIR}/terra_wld.c"
    "${SRC_DIR}/terra_api.c"
    "${SRC_DIR}/terra_ops.c"
    "${SRC_DIR}/terra_mutators.c"
    "${SRC_DIR}/terra_render.c"
    "${SRC_DIR}/terra_map.c"
    "${SRC_DIR}/terra_update.c"
    "${SRC_DIR}/terra_txci.c"
    "${SRC_DIR}/terra_pixel_art.c"
    "${SRC_DIR}/terra_hash.c"
)

set(TERRAX_BUILD_COMMIT "unknown" CACHE STRING "Source commit embedded in ABI identity")
set(TERRAX_BUILD_DIRTY "unknown" CACHE STRING "Dirty state embedded in ABI identity")
set(TERRAX_BUILD_COMPILER "native" CACHE STRING "Compiler identity embedded in ABI identity")
set(TERRAX_BUILD_FLAGS "-O3" CACHE STRING "Build flags embedded in ABI identity")

function(configure_terra_identity target target_name initial_memory maximum_memory)
    target_compile_definitions(${target} PRIVATE
        TERRAX_BUILD_COMMIT="${TERRAX_BUILD_COMMIT}"
        TERRAX_BUILD_DIRTY="${TERRAX_BUILD_DIRTY}"
        TERRAX_BUILD_COMPILER="${TERRAX_BUILD_COMPILER}"
        TERRAX_BUILD_FLAGS="${TERRAX_BUILD_FLAGS}"
        TERRAX_BUILD_TARGET="${target_name}"
        TERRAX_INITIAL_MEMORY=${initial_memory}
        TERRAX_MAXIMUM_MEMORY=${maximum_memory}
    )
endfunction()

if(EMSCRIPTEN)
    set(NODE_EXPORTS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/exports.txt")
    set(WEB_EXPORTS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/exports.web.txt")
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
        -fno-exceptions
        -fno-rtti
        -O3
        "-sUSE_ZLIB=1"
    )

    set(COMMON_LINK_OPTIONS
        "-sUSE_ZLIB=1"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','stringToUTF8','lengthBytesUTF8','getValue','setValue','HEAPU8','HEAPU32','HEAP32','HEAPF32','HEAPF64','FS','stackAlloc','stackSave','stackRestore','wasmMemory']"
        "-sMODULARIZE=1"
        "-sERROR_ON_UNDEFINED_SYMBOLS=1"
        "--no-entry"
    )

    # ---- Node target ----
    add_executable(terrax_world_wasm ${SOURCES})
    configure_terra_identity(terrax_world_wasm "node" 134217728 536870912)
    target_compile_options(terrax_world_wasm PRIVATE ${COMMON_COMPILE_OPTIONS})
    target_link_options(terrax_world_wasm PRIVATE
        ${COMMON_LINK_OPTIONS}
        "SHELL:-sEXPORTED_FUNCTIONS=@${CMAKE_CURRENT_BINARY_DIR}/exported_functions_node.json"
        "-sINITIAL_MEMORY=134217728"
        "-sMAXIMUM_MEMORY=536870912"
        "-sEXPORT_NAME='TerraWorldWasm'"
        "-sENVIRONMENT=node"
        "-sNODERAWFS=1"
        "-sFILESYSTEM=1"
    )

    # ---- Web/MiniProgram target ----
    # 注意：不要用 SINGLE_FILE，小程序 WXWebAssembly.instantiate 只接受 .wasm 文件路径
    add_executable(terrax_world_wasm_web ${SOURCES})
    configure_terra_identity(terrax_world_wasm_web "web" 33554432 100663296)
    target_compile_options(terrax_world_wasm_web PRIVATE ${COMMON_COMPILE_OPTIONS})
    target_link_options(terrax_world_wasm_web PRIVATE
        ${COMMON_LINK_OPTIONS}
        "SHELL:-sEXPORTED_FUNCTIONS=@${CMAKE_CURRENT_BINARY_DIR}/exported_functions_web.json"
        "-sINITIAL_MEMORY=33554432"
        "-sMAXIMUM_MEMORY=100663296"
        "-sEXPORT_NAME='TerraWorldWasmWeb'"
        "-sENVIRONMENT=web,worker"
        "-sFILESYSTEM=1"
    )
else()
    add_library(terrax_world_static STATIC ${SOURCES})
    configure_terra_identity(terrax_world_static "native" 0 0)
    target_compile_definitions(terrax_world_static PRIVATE TERRAX_STATIC)
    if(TERRAWASM_SANITIZERS)
        target_compile_options(terrax_world_static PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(terrax_world_static PRIVATE -fsanitize=address,undefined)
    endif()
    if(TERRAWASM_NATIVE_TESTS)
        add_executable(terra_native_contract tests/native_contract.c)
        target_link_libraries(terra_native_contract PRIVATE terrax_world_static)
        target_compile_definitions(terra_native_contract PRIVATE TERRAX_STATIC)
        if(TERRAWASM_SANITIZERS)
            target_compile_options(terra_native_contract PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(terra_native_contract PRIVATE -fsanitize=address,undefined)
        endif()
        add_test(NAME terra_native_contract COMMAND terra_native_contract)
        if(TERRAWASM_FUZZ_SMOKE)
            add_executable(terra_fuzz_smoke
                tests/fuzz/fuzz_smoke.c
                tests/fuzz/fuzz_wld_open.c
                tests/fuzz/fuzz_json.c
            )
            target_link_libraries(terra_fuzz_smoke PRIVATE terrax_world_static)
            target_compile_definitions(terra_fuzz_smoke PRIVATE TERRAX_STATIC)
            if(TERRAWASM_SANITIZERS)
                target_compile_options(terra_fuzz_smoke PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
                target_link_options(terra_fuzz_smoke PRIVATE -fsanitize=address,undefined)
            endif()
            add_test(NAME terra_fuzz_smoke COMMAND terra_fuzz_smoke)
        endif()
    endif()
endif()
