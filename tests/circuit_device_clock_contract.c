/* Standalone device registration contract; no fixture, test hook, or ABI export
 * is linked into the release module. Run from the repository root with:
 * cc -std=c17 -O2 -ffunction-sections -fdata-sections -Iinclude -Isrc \
 *   src/terra_circuit_devices.c tests/circuit_device_clock_contract.c \
 *   -Wl,--gc-sections -o /tmp/circuit-device-clock && /tmp/circuit-device-clock
 * Source: Wiring.CheckMech / HitSwitch at
 * 8255d34616c780af12079425ac92a0a7aed87d71, Terraria/Wiring.cs:455-474.
 */
#include "terra_circuit_world_internal.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { COUNT = 1001, SOURCE_LIMIT = 999 };
static CxDevice devices[COUNT];
static uint32_t mechanics[COUNT];

static void reset(CxWorld* world) {
    memset(world, 0, sizeof(*world));
    memset(devices, 0, sizeof(devices));
    memset(mechanics, 0, sizeof(mechanics));
    world->devices = devices;
    world->device_count = COUNT;
    world->mechs = mechanics;
    world->mech_capacity = COUNT;
    for (uint32_t i = 0; i < COUNT; ++i) {
        devices[i].x = i + 2;
        devices[i].y = 4;
        devices[i].tile.active = 1;
        devices[i].tile.type = 144;
        devices[i].tile.frame_x = 72;
    }
}

static int interact(CxWorld* world, uint32_t index) {
    world->command.x = devices[index].x;
    world->command.y = devices[index].y;
    return cx_interact(world);
}

int main(void) {
    CxWorld world;
    reset(&world);
    for (uint32_t i = 0; i < COUNT; ++i) {
        assert(interact(&world, i) == 0);
        assert(devices[i].tile.frame_y == 18);
        assert(devices[i].cooldown == (i < SOURCE_LIMIT ? 18000u : 0u));
    }
    assert(world.mech_count == SOURCE_LIMIT);
    for (uint32_t i = 0; i < SOURCE_LIMIT; ++i) assert(mechanics[i] == i);

    /* At capacity HitSwitch still changes the frame, but it cannot create or
     * restart a clock. A duplicate position retains its original deadline. */
    devices[0].cooldown = 17777;
    assert(interact(&world, 0) == 0 && devices[0].tile.frame_y == 0);
    assert(interact(&world, 0) == 0 && devices[0].tile.frame_y == 18);
    assert(devices[0].cooldown == 17777 && world.mech_count == SOURCE_LIMIT);
    assert(interact(&world, SOURCE_LIMIT) == 0 && devices[SOURCE_LIMIT].tile.frame_y == 0);
    assert(interact(&world, SOURCE_LIMIT) == 0 && devices[SOURCE_LIMIT].tile.frame_y == 18);
    assert(devices[SOURCE_LIMIT].cooldown == 0 && world.mech_count == SOURCE_LIMIT);

    /* A momentary switch shares CheckMech's same world-wide capacity. Its
     * explicit HitSwitch pulse/frame change is still accepted when full. */
    devices[COUNT - 1].tile.type = 411;
    devices[COUNT - 1].tile.frame_x = devices[COUNT - 1].tile.frame_y = 0;
    assert(interact(&world, COUNT - 1) == 1);
    assert(devices[COUNT - 1].tile.frame_x == 36);
    assert(devices[COUNT - 1].cooldown == 0 && world.mech_count == SOURCE_LIMIT);
    puts("circuit devices: 999 shared mechanic slots, rejected registration and duplicate deadlines passed");
    return 0;
}
