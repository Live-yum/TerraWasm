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
static const unsigned char cx_support_property_values[10] = {
    0,2,3,4,6,7,8,23,32,64
};
static const unsigned char cx_support_materials[377] = {
    0,48,3,0,0,3,48,53,52,117,51,0,0,48,0,0,0,0,0,0,0,0,0,0,0,3,144,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,83,53,51,51,51,51,51,80,3,0,0,0,3,9,5,9,0,0,144,6,0,
    0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,128,136,136,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,9,0,0,0,0,9,0,16,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,34,34,34,0,32,2,0,0,0,0,34,34,0,0,0,0,32,2,0,0,0,0,
    0,0,0,0,0,153,153,0,0,32,0,0,0,0,0,0,0,0,0,34,32,34,2,0,0,0,0,0,2,0,130,0,
    0,48,3,35,34,2,0,0,0,0,32,0,0,0,32,2,0,0,0,0,0,112,0,0,0,112,119,119,48,0,0,0,
    0,0,0,0,0,0,0,0,0,48,83,0,0,0,0,0,0,0,0,51,51,3,0,144,48,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,34,8,8,130,0,2,2,0,0,34,32,34,2,34,96,0,51,0,51,3,3,102,
    102,6,3,2,0,0,0,3,48,51,0,32,34,34,34,34,34,34,2,48,0,32,3,0,128,128,32,0,2,0,0,0,
    2,32,34,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    3,48,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};
static unsigned cx_support_properties(unsigned type) {
    return type < 754u ? cx_support_property_values[(cx_support_materials[type >> 1] >> ((type & 1u) * 4u)) & 15u] : 0;
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
