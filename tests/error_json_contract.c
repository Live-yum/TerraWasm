#include "terra_world.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void tx_set_error(const char* code, const char* message);

int main(void) {
    uint64_t required = 0;
    char output[256];

    tx_set_error("BAD\"CODE", "line one\nline two\t\\done");
    if (terra_info_get_last_error_json(NULL, 0u, &required) != TERRAX_WORLD_STATUS_OK) return 1;
    if (required == 0u || required > sizeof(output)) return 2;
    if (terra_info_get_last_error_json(output, sizeof(output), &required) != TERRAX_WORLD_STATUS_OK) return 3;
    if (strchr(output, '\n') || strchr(output, '\t')) return 4;
    if (!strstr(output, "BAD\\\"CODE")) return 5;
    if (!strstr(output, "line one\\nline two\\t\\\\done")) return 6;
    if (output[0] != '{' || output[strlen(output) - 1u] != '}') return 7;

    puts("error JSON contract: escaped and bounded");
    return 0;
}
