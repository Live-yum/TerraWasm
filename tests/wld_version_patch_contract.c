/* Exercise the internal header-only migration policy without constructing
 * unrelated header/tile payloads. The boundaries come from WorldFile and
 * BannerSystem loaders, not from the table under test. */
#include "../src/terra_mutators.c"
#include <assert.h>

int main(void) {
    const uint32_t boundaries[][2]={{194,195},{195,196},{267,268},{288,289},{311,312},{322,323}};
    for (uint32_t i=0;i<sizeof(boundaries)/sizeof(boundaries[0]);i++) {
        assert(!header_versions_compatible(boundaries[i][0],boundaries[i][1]));
        assert(!header_versions_compatible(boundaries[i][1],boundaries[i][0]));
    }
    assert(header_versions_compatible(196,197));
    assert(header_versions_compatible(315,322));
    assert(header_versions_compatible(323,326));
    assert(!header_versions_compatible(326,327));
    return 0;
}
