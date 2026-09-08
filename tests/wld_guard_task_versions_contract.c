#include "terra_wld_guard_task.h"

#include <assert.h>
#include <stdint.h>

/* Exercise the incremental section cursor after header decoding, with the
 * byte layouts defined by WorldFile.LoadNPCs rather than a TerraWasm writer. */
static int validate_sections(uint32_t version, uint32_t section,
                             uint8_t *bytes, uint32_t length) {
    TxWorld world = {0};
    TxWldGuardTask task = {0};
    world.version = version;
    world.pointer_count = 11;
    world.file = bytes;
    world.file_len = length;
    world.ends[section] = length;
    task.initialized = 1;
    for (unsigned step = 0; step < 100; step++) {
        int result = tx_wld_guard_task_step(&world, &task, 1);
        if (result != 0) return result;
    }
    assert(!"section validation failed to finish within bounded work units");
    return 0;
}

int main(void) {
    uint8_t town139[] = {
        1, 5, 'G', 'u', 'i', 'd', 'e', 3, 'B', 'o', 'b',
        0, 0, 32, 65, 0, 0, 0, 0, /* X=10, Y=0 */
        0, 10, 0, 0, 0, 20, 0, 0, 0, /* homeless, home X/Y */
        0 /* no persistent list before 140 */
    };
    assert(validate_sections(139, 4, town139, sizeof(town139)) == 1);

    uint8_t persistent140[] = {
        0, 1, 5, 'G', 'u', 'i', 'd', 'e',
        0, 0, 32, 65, 0, 0, 0, 0, 0
    };
    assert(validate_sections(140, 4, persistent140, sizeof(persistent140)) == 1);
    assert(validate_sections(140, 4, persistent140, sizeof(persistent140) - 1) == -1);
    uint8_t truncated140[] = {0, 1};
    assert(validate_sections(140, 4, truncated140, sizeof(truncated140)) == -1);

    uint8_t variation213[] = {
        1, 22, 0, 0, 0, 0, /* NPC ID + empty given name */
        0, 0, 32, 65, 0, 0, 0, 0,
        0, 10, 0, 0, 0, 20, 0, 0, 0,
        2, /* reserved bit set; variation is controlled only by bit zero */
        0, 0
    };
    assert(validate_sections(213, 4, variation213, sizeof(variation213)) == 1);

    uint8_t footer[] = {1, 1, 'W', 7, 0, 0, 0};
    assert(validate_sections(209, 8, footer, sizeof(footer)) == 1);
    assert(validate_sections(210, 8, footer, sizeof(footer)) == -1);
    return 0;
}
