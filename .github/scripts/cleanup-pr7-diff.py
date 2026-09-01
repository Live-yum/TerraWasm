from pathlib import Path

WIDE = "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','stringToUTF8','lengthBytesUTF8','getValue','setValue','HEAPU8','HEAPU32','HEAP32','HEAPF32','HEAPF64','FS','stackAlloc','stackSave','stackRestore','wasmMemory']"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise SystemExit(f'{label} anchor not found')
    return text.replace(old, new, 1)


p = Path('CMakeLists.txt')
s = p.read_text()
s = replace_once(
    s,
    f'''    list(APPEND COMMON_LINK_OPTIONS\n        "-sALLOW_MEMORY_GROWTH=1"\n        "{WIDE}"\n        "-sMODULARIZE=1"\n        "-sERROR_ON_UNDEFINED_SYMBOLS=1"\n        "--no-entry"\n    )''',
    '''    list(APPEND COMMON_LINK_OPTIONS\n        "-sALLOW_MEMORY_GROWTH=1"\n        "-sMODULARIZE=1"\n        "-sERROR_ON_UNDEFINED_SYMBOLS=1"\n        "--no-entry"\n    )''',
    'CMake common runtime',
)
anchor = '''    if(TERRAX_ENABLE_LTO)\n        list(APPEND COMMON_COMPILE_OPTIONS -flto)\n        list(APPEND COMMON_LINK_OPTIONS -flto)\n        list(APPEND COMMON_MANIFEST_FLAGS -flto)\n    endif()\n\n'''
s = replace_once(
    s,
    anchor,
    anchor + f'''    set(WIDE_RUNTIME_METHODS "{WIDE}")\n    if(TERRAWASM_FEATURE_SET STREQUAL "wld")\n        set(WEB_RUNTIME_METHODS "-sEXPORTED_RUNTIME_METHODS=['HEAPU8','HEAPU32']")\n    else()\n        set(WEB_RUNTIME_METHODS "${{WIDE_RUNTIME_METHODS}}")\n    endif()\n\n''',
    'CMake LTO',
)
node = '''    set(NODE_LINK_OPTIONS\n        "SHELL:-sEXPORTED_FUNCTIONS=@${CMAKE_CURRENT_BINARY_DIR}/exported_functions_node.json"\n'''
s = replace_once(s, node, node + '        "${WIDE_RUNTIME_METHODS}"\n', 'CMake node flags')
web = '''    set(WEB_LINK_OPTIONS\n        "SHELL:-sEXPORTED_FUNCTIONS=@${CMAKE_CURRENT_BINARY_DIR}/exported_functions_web.json"\n'''
s = replace_once(s, web, web + '        "${WEB_RUNTIME_METHODS}"\n', 'CMake web flags')
p.write_text(s)

p = Path('build.ps1')
s = p.read_text()
s = replace_once(s, f'    "{WIDE}",\n', '', 'PowerShell common runtime')
anchor = '''if ($EnableLto) {\n    $CommonFlags += "-flto"\n}\n'''
s = replace_once(
    s,
    anchor,
    anchor + f'''$WideRuntimeMethods = "{WIDE}"\n$WebRuntimeMethods = if ($Features -eq "wld") {{ "-sEXPORTED_RUNTIME_METHODS=['HEAPU8','HEAPU32']" }} else {{ $WideRuntimeMethods }}\n''',
    'PowerShell LTO',
)
node = '''$NodeFlags = @(\n    "-sEXPORTED_FUNCTIONS=@exported_functions_node.json",\n'''
s = replace_once(s, node, node + '    $WideRuntimeMethods,\n', 'PowerShell node flags')
web = '''$WebFlags = @(\n    "-sEXPORTED_FUNCTIONS=@exported_functions_web.json",\n'''
s = replace_once(s, web, web + '    $WebRuntimeMethods,\n', 'PowerShell web flags')
p.write_text(s)
