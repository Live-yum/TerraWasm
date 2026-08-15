"use strict";
const fs = require("fs");
const path = require("path");

const src = fs.readFileSync(path.resolve(__dirname, "..", "..", "TerraX", "include", "color_data.h"), "utf8");

// Parse Tile COLORS
const tileMatch = src.match(/namespace\s+Tile\s*\{[\s\S]*?constexpr\s+TileColor\s+COLORS\s*\[\]\s*=\s*\{([\s\S]*?)\}\s*;/);
if (!tileMatch) { console.error("Failed to find Tile::COLORS"); process.exit(1); }

const tileEntries = [];
const re = /\{(\d+),\s*(\d+),\s*\{(\d+),\s*(\d+),\s*(\d+)\}\}/g;
let m;
while ((m = re.exec(tileMatch[1])) !== null) {
    tileEntries.push({ id: +m[1], variant: +m[2], r: +m[3], g: +m[4], b: +m[5] });
}

// Parse Wall COLORS
const wallMatch = src.match(/namespace\s+Wall\s*\{[\s\S]*?constexpr\s+WallColor\s+COLORS\s*\[\]\s*=\s*\{([\s\S]*?)\}\s*;/);
if (!wallMatch) { console.error("Failed to find Wall::COLORS"); process.exit(1); }

const wallEntries = [];
const re2 = /\{(\d+),\s*(\d+),\s*\{(\d+),\s*(\d+),\s*(\d+)\}\}/g;
while ((m = re2.exec(wallMatch[1])) !== null) {
    wallEntries.push({ id: +m[1], variant: +m[2], r: +m[3], g: +m[4], b: +m[5] });
}

console.log("Tile entries:", tileEntries.length, "Wall entries:", wallEntries.length);

const MAX_IDS = 1024;
const MAX_VARS = 16;
const tileTable = new Uint8Array(MAX_IDS * MAX_VARS * 4);
const wallTable = new Uint8Array(MAX_IDS * MAX_VARS * 4);

for (const e of tileEntries) {
    if (e.id >= MAX_IDS || e.variant >= MAX_VARS) continue;
    const idx = (e.id * MAX_VARS + e.variant) * 4;
    tileTable[idx] = e.r; tileTable[idx+1] = e.g; tileTable[idx+2] = e.b; tileTable[idx+3] = 255;
}
for (const e of wallEntries) {
    if (e.id >= MAX_IDS || e.variant >= MAX_VARS) continue;
    const idx = (e.id * MAX_VARS + e.variant) * 4;
    wallTable[idx] = e.r; wallTable[idx+1] = e.g; wallTable[idx+2] = e.b; wallTable[idx+3] = 255;
}

function emitArray(name, table, rows) {
    let s = "static const uint8_t " + name + "[" + rows + " * TX_COLOR_MAX_VARIANTS * 4] = {\n";
    for (let id = 0; id < rows; id++) {
        const parts = [];
        for (let v = 0; v < MAX_VARS; v++) {
            const idx = (id * MAX_VARS + v) * 4;
            parts.push(table[idx], table[idx+1], table[idx+2], table[idx+3]);
        }
        s += "    " + parts.join(", ") + ",\n";
    }
    s += "};\n";
    return s;
}

let out = "#ifndef TERRA_COLOR_DATA_H\n#define TERRA_COLOR_DATA_H\n\n#include <stdint.h>\n\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";
out += "#define TX_COLOR_MAX_IDS 1024\n#define TX_COLOR_MAX_VARIANTS 16\n\n";
out += "/* Index: (id * TX_COLOR_MAX_VARIANTS + variant) * 4 -> {R, G, B, A} */\n";
out += "/* A == 255 means valid; A == 0 means undefined. */\n\n";
out += emitArray("TX_BUILTIN_TILE_COLORS", tileTable, MAX_IDS);
out += "\n";
out += emitArray("TX_BUILTIN_WALL_COLORS", wallTable, MAX_IDS);

// Paint colors (from TerraX ColorData if available, else hardcoded from Terraria wiki)
out += "\n/* Paint colors [30 * 3 bytes RGB]. Index: (paint_id - 1) * 3. */\n";
out += "static const uint8_t TX_PAINT_COLORS[30 * 3] = {\n";
out += "    255, 0, 0,       /* 1: Red */\n";
out += "    255, 150, 0,     /* 2: Orange */\n";
out += "    255, 255, 0,     /* 3: Yellow */\n";
out += "    0, 255, 0,       /* 4: Lime */\n";
out += "    0, 255, 0,       /* 5: Green */\n";
out += "    0, 255, 200,     /* 6: Teal */\n";
out += "    0, 200, 255,     /* 7: Cyan */\n";
out += "    0, 150, 255,     /* 8: Sky Blue */\n";
out += "    0, 0, 255,       /* 9: Blue */\n";
out += "    150, 0, 255,     /* 10: Purple */\n";
out += "    200, 0, 255,     /* 11: Violet */\n";
out += "    255, 0, 200,     /* 12: Pink */\n";
out += "    255, 255, 255,   /* 13: White */\n";
out += "    175, 175, 175,   /* 14: Gray */\n";
out += "    100, 100, 100,   /* 15: Dark Gray */\n";
out += "    255, 125, 0,     /* 16: Deep Orange */\n";
out += "    175, 0, 0,       /* 17: Crimson */\n";
out += "    0, 0, 175,       /* 18: Deep Blue */\n";
out += "    175, 125, 0,     /* 19: Brown */\n";
out += "    255, 255, 255,   /* 20: Illuminant */\n";
out += "    255, 175, 0,     /* 21: Deep Orange */\n";
out += "    255, 0, 255,     /* 22: Deep Violet */\n";
out += "    200, 0, 0,       /* 23: Blood */\n";
out += "    200, 175, 0,     /* 24: Amber */\n";
out += "    255, 255, 255,   /* 25: Bright White */\n";
out += "    175, 175, 175,   /* 26: Silver */\n";
out += "    0, 175, 175,     /* 27: Deep Teal */\n";
out += "    60, 0, 60,       /* 28: Echo/Void */\n";
out += "    0, 0, 0,         /* 29: Shadow (special blend) */\n";
out += "    0, 0, 0,         /* 30: Negative (special blend) */\n";
out += "};\n\n";
out += "#ifdef __cplusplus\n}\n#endif\n\n#endif /* TERRA_COLOR_DATA_H */\n";

fs.writeFileSync(path.resolve(__dirname, "..", "src", "terra_color_data.h"), out);
console.log("Generated terra_color_data.h (" + out.length + " bytes)");