import os

path = r'C:\Users\depths\Desktop\Tdecoder\TerraWasm\src\terra_ops.c'
with open(path, 'r', encoding='utf-8', errors='replace') as f:
    content = f.read()

# Find the execute_render_lit_map function
marker = 'static int execute_render_lit_map'
start = content.find(marker)
if start < 0:
    print('ERROR: function not found')
    exit(1)

# Find the end of the function (next function starts with "static int execute_unlock")
end = content.find('static int execute_unlock_bestiary', start)
if end < 0:
    print('ERROR: end not found')
    exit(1)

new_func = """static int execute_render_lit_map(TxWorld* w, const char* request, int jlen,
                                  TxBuf* response) {
    extern int json_find_key(const char* json, int jlen, const char* key);
    extern int json_extract_str(const char* json, int jlen, int pos, char* out, int ocap);
    extern int write_file_from_heap(const char* path, const uint8_t* data, uint32_t len);

    int32_t result = terra_generate_map(w);
    if (result < 0) return -1;

    uint8_t* map_data = (uint8_t*)(uintptr_t)tx_last_ptr;
    uint32_t map_len = tx_last_len;
    uint32_t map_w = tx_last_width;
    uint32_t map_h = tx_last_height;

    int wrote_file = 0;
    int dir_pos = json_find_key(request, jlen, "output_dir");
    if (dir_pos >= 0 && map_data && map_len > 0) {
        char dir_buf[512];
        if (json_extract_str(request, jlen, dir_pos, dir_buf, sizeof(dir_buf))) {
            char map_path[768];
            uint32_t pos = 0;
            for (uint32_t i = 0; dir_buf[i] && pos < sizeof(map_path) - 32; i++)
                map_path[pos++] = dir_buf[i];
            if (pos > 0 && map_path[pos-1] != '/' && map_path[pos-1] != '\\\\')
                map_path[pos++] = '/';
            const char* fname = "world.map";
            for (uint32_t i = 0; fname[i] && pos < sizeof(map_path) - 1; i++)
                map_path[pos++] = fname[i];
            map_path[pos] = 0;
            wrote_file = write_file_from_heap(map_path, map_data, map_len);
        }
    }

    buf_cstr(response, "{\\"status\\":\\"ok\\",\\"width\\":");
    json_u32(response, map_w);
    buf_cstr(response, ",\\"height\\":");
    json_u32(response, map_h);
    buf_cstr(response, ",\\"map_bytes\\":");
    json_u32(response, map_len);
    buf_cstr(response, ",\\"file_written\\":");
    buf_cstr(response, wrote_file ? "true" : "false");
    buf_u8(response, '}');
    return set_result_buf(response);
}
"""

content = content[:start] + new_func + content[end:]
with open(path, 'w', encoding='utf-8', newline='\n') as f:
    f.write(content)
print('Fixed execute_render_lit_map')