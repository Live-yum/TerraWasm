#ifndef TERRA_CIRCUIT_MECHANISM_TILES_H
#define TERRA_CIRCUIT_MECHANISM_TILES_H
/* Wiring.HitSwitch/HitWireSingle at 8255d34616c780af12079425ac92a0a7aed87d71.
 * Wiring.cs SHA-256: c05fac30c1e1d13720a30be89c957b4ea5cc84be18e44e4a2c44ea532ab89832.
 * Natural decorations merely crossed by a wire are not circuit devices. */
static const unsigned char cx_wire_object_bits[95] = {
    16,140,32,0,14,4,2,0,0,0,0,176,16,2,0,64,156,235,33,0,0,96,0,0,
    0,128,150,9,0,8,16,128,255,31,0,0,0,0,0,4,0,128,4,32,16,0,16,0,
    60,0,96,12,248,51,0,47,144,0,24,16,1,0,2,36,0,0,8,0,4,32,48,0,
    0,0,6,0,0,0,0,0,68,0,134,0,0,0,0,0,0,0,35,32,0,0,0,
};
static int cx_wire_object_type(unsigned type) {
    return type < 754u && (cx_wire_object_bits[type >> 3] & (1u << (type & 7u)));
}
#endif
