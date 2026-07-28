cmake_minimum_required(VERSION 3.27)
project(TerraWasm C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)

set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(INC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
include_directories(${INC_DIR})

set(SOURCES
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

if(EMSCRIPTEN)
    set(NODE_EXPORTS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/exports.txt")
    set(WEB_EXPORTS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/exports.web.txt")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${NODE_EXPORTS_FILE}"
        "${WEB_EXPORTS_FILE}"
    )

    function(write_exports_json input_file output_file)
        file(STRINGS "${input_file}" export_lines)
        list(FILTER export_lines EXCLUDE REGEX "^[ \\t]*#")
        list(FILTER export_lines EXCLUDE REGEX "^[ \\t]*$")
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
        -Oz
        -flto
        -fno-exceptions
        -fno-rtti
        -ffunction-sections
        -fdata-sections
        "-sUSE_ZLIB=1"
    )

    set(COMMON_LINK_OPTIONS
        -Oz
        -flto
        "-sUSE_ZLIB=1"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sMALLOC=emmalloc"
        "-sMODULARIZE=1"
        "-sASSERTIONS=0"
        "-sSAFE_HEAP=0"
        "-sSTACK_OVERFLOW_CHECK=0"
        "-sERROR_ON_UNDEFINED_SYMBOLS=1"
        "-sDISABLE_EXCEPTION_CATCHING=1"
        "-sSUPPORT_LONGJMP=0"
        "--no-entry"
    )

    # Node target retains filesystem compatibility for CLI tools and existing tests.
    add_executable(terrax_world_wasm ${SOURCES})
    target_compile_options(terrax_world_wasm PRIVATE ${COMMON_COMPILE_OPTIONS})
    target_link_options(terrax_world_wasm PRIVATE
        ${COMMON_LINK_OPTIONS}
        "SHELL:-sEXPORTED_FUNCTIONS=@${CMAKE_CURRENT_BINARY_DIR}/exported_functions_node.json"
        "-sEXPORTED_RUNTIME_METHODS=['HEAPU8','HEAPU32','FS']"
        "-sINITIAL_MEMORY=67108864"
        "-sMAXIMUM_MEMORY=536870912"
        "-sEXPORT_NAME='TerraWorldWasm'"
        "-sENVIRONMENT=node"
        "-sNODERAWFS=1"
        "-sFILESYSTEM=1"
    )

    # Web/MiniProgram target is buffer-only. Removing MEMFS avoids a second WLD copy
    # and drops the filesystem runtime from both glue JS and WASM.
    add_executable(terrax_world_wasm_web ${SOURCES})
    target_compile_options(terrax_world_wasm_web PRIVATE ${COMMON_COMPILE_OPTIONS})
    target_link_options(terrax_world_wasm_web PRIVATE
        ${COMMON_LINK_OPTIONS}
        "SHELL:-sEXPORTED_FUNCTIONS=@${CMAKE_CURRENT_BINARY_DIR}/exported_functions_web.json"
        "-sEXPORTED_RUNTIME_METHODS=['HEAPU8','HEAPU32']"
        "-sINITIAL_MEMORY=16777216"
        "-sMAXIMUM_MEMORY=134217728"
        "-sEXPORT_NAME='TerraWorldWasmWeb'"
        "-sENVIRONMENT=web,worker"
        "-sFILESYSTEM=0"
    )
else()
    add_library(terrax_world_static STATIC ${SOURCES})
    target_compile_definitions(terrax_world_static PRIVATE TERRAX_STATIC)
endif()
