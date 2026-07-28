#include "terra_abi.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    const char* capabilities = terra_capabilities();
    const char* build = terra_build_info_json();
    if (terra_abi_version() != 1u || !capabilities || !build) return 1;
    if (!strstr(capabilities, "world-buffer-io") || !strstr(build, "compiler")) return 2;
    puts("native ABI contract: ok");
    return 0;
}
