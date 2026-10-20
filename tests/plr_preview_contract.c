#include "terra_plr.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Optional external corpus is never copied into the source tree. */
int main(int argc, char **argv) {
    assert(argc > 1);
    for (int i = 1; i < argc; i++) {
        FILE *file = fopen(argv[i], "rb"); assert(file);
        assert(fseek(file, 0, SEEK_END) == 0);
        long length = ftell(file); assert(length > 0 && length <= 2 * 1024 * 1024);
        rewind(file);
        unsigned char *bytes = malloc((size_t)length); assert(bytes);
        assert(fread(bytes, 1, (size_t)length, file) == (size_t)length); fclose(file);
        uint32_t required = 0;
        int status = terra_plr_preview_json_from_buffer(bytes, (uint32_t)length, NULL, 0, &required);
        if (status != 0) fprintf(stderr, "Preview failed: %s (%d)\n", argv[i], status);
        assert(status == 0 && required > 1 && required < 4 * 1024 * 1024);
        char *json = malloc(required); assert(json);
        assert(terra_plr_preview_json_from_buffer(bytes, (uint32_t)length, json, required, &required) == 0);
        assert(strstr(json, "\"armor\"") && strstr(json, "\"name\""));
        assert(terra_plr_preview_json_from_buffer(bytes, 1, NULL, 0, &required) != 0);
        assert(terra_plr_preview_json_from_buffer(bytes, 16, NULL, 0, &required) != 0);
        free(json); free(bytes);
    }
    printf("Read-only player preview: %d files passed, incomplete fields rejected\n", argc - 1);
    return 0;
}
