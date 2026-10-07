#ifndef TERRA_CIRCUIT_SUPPORT_MATERIALS_H
#define TERRA_CIRCUIT_SUPPORT_MATERIALS_H
/* Generated from pinned Terraria source 8255d34616c780af12079425ac92a0a7aed87d71.
 * Main.cs SHA-256: daae97ba9e9a11c457e40cb2001657b08d4611ee1ee6ba9e446bced606e7d5f5.
 * TileID.cs SHA-256: 9e766249c1a4ea56bc10dd1019a0502243be12177defc50c7febb6e4c1615807.
 * WorldGen.cs SHA-256: 8de656a227fe6d250438078e865be97bae1c1819d77a8a5cd5c1b651c6e6fef5 */
#define CX_SUPPORT_TABLE 1u
#define CX_SUPPORT_SOLID_TOP 2u
#define CX_SUPPORT_NO_ATTACH 4u
#define CX_SUPPORT_BEAM 8u
#define CX_SUPPORT_PLATFORM 16u
#define CX_SUPPORT_MOSS 32u
#define CX_SUPPORT_FALLING 64u
static const unsigned char cx_support_materials[754] = {
    0,0,0,4,4,0,0,0,0,0,4,0,0,4,7,4,6,4,7,23,4,4,0,0,0,0,0,4,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,0,0,64,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,7,7,4,4,4,4,4,4,4,
    4,4,4,4,0,7,4,0,0,0,0,0,0,0,4,0,64,0,7,0,64,0,0,0,0,0,0,64,8,0,0,0,
    0,0,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,32,32,32,32,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    64,0,0,0,0,0,0,0,0,0,64,0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,3,3,3,3,3,0,0,0,3,3,0,
    0,0,0,0,0,0,0,0,3,3,3,3,0,0,0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,64,64,64,64,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,3,3,0,3,3,3,3,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,3,32,0,0,
    0,0,0,4,4,0,4,3,3,3,3,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,3,3,0,
    0,0,0,0,0,0,0,0,0,0,0,23,0,0,0,0,0,0,0,23,23,23,23,23,0,4,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,7,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,4,4,4,4,4,0,0,0,0,64,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,32,0,32,0,3,32,0,0,3,0,
    3,0,0,0,0,0,3,3,0,3,3,3,3,0,3,3,0,8,0,0,4,4,0,0,4,4,4,0,4,0,8,8,
    8,8,8,0,4,0,3,0,0,0,0,0,0,0,4,0,0,4,4,4,0,0,0,3,3,3,3,3,3,3,3,3,
    3,3,3,3,3,0,0,4,0,0,0,3,4,0,0,0,0,32,0,32,0,3,0,0,3,0,0,0,0,0,0,0,
    3,0,0,3,3,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    4,0,0,4,0,0,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};
static unsigned cx_support_properties(unsigned type) {
    return type < sizeof(cx_support_materials) ? cx_support_materials[type] : 0;
}
/* WorldGen.GetDesiredStalagtiteStyle: small coral alone accepts coralstone. */
static int cx_stalactite_material(unsigned type, unsigned height) {
    if (cx_support_properties(type) & CX_SUPPORT_MOSS) return 1;
    if (type == 225u) return height == 1u;
    switch (type) {
        case 1: case 200: case 164: case 163: case 117: case 402: case 403:
        case 25: case 398: case 400: case 203: case 399: case 401:
        case 396: case 397: case 367: case 368: case 147: case 161: return 1;
        default: return 0;
    }
}
#endif
