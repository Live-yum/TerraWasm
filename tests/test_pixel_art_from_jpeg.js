"use strict";
const path = require("path");
const fs = require("fs");
const sharp = require("sharp");

const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));

const WLD_PATH = path.resolve(__dirname, "..", "..", "TerraX", "wld", "1.wld");
const JPEG_PATH = path.resolve(__dirname, "..", "..", "TerraX", "img", "test.jpeg");
const TXCI_PATH = path.join(__dirname, "..", "data", "terraria_color_index.txci");
const OUTPUT_PATH = path.join(__dirname, "pixel_art_from_jpeg.wld");

async function main() {
    console.log("Loading WASM...");
    const t0 = Date.now();
    const M = await TerraWorldWasm();
    const { UTF8ToString, stringToUTF8, lengthBytesUTF8, getValue } = M;
    console.log("  WASM loaded in " + (Date.now() - t0) + "ms");

    function as(s) {
        const l = lengthBytesUTF8(s) + 1;
        const p = M._tx_malloc(l);
        stringToUTF8(s, p, l);
        return p;
    }

    function getError() {
        const rp = M._tx_malloc(8);
        M._terra_info_get_last_error_json(0, 0n, rp);
        const sz = Number(M.HEAPU32[rp >> 2]);
        if (sz > 0) {
            const buf = M._tx_malloc(sz);
            M._terra_info_get_last_error_json(buf, BigInt(sz), rp);
            return UTF8ToString(buf);
        }
        return "(no error)";
    }

    // === 1. Decode JPEG to RGBA ===
    console.log("\n=== 1. Decode JPEG ===");
    console.log("  Input: " + JPEG_PATH);
    const t1 = Date.now();
    const { data: rgbaBuf, info: imgInfo } = await sharp(JPEG_PATH)
        .ensureAlpha()
        .raw()
        .toBuffer({ resolveWithObject: true });
    console.log("  Decoded: " + imgInfo.width + "x" + imgInfo.height + " (" + rgbaBuf.length + " bytes) in " + (Date.now() - t1) + "ms");

    // Copy RGBA to WASM memory
    const rgbaPtr = M._tx_malloc(rgbaBuf.length);
    M.HEAPU8.set(rgbaBuf, rgbaPtr);
    console.log("  RGBA copied to WASM memory at ptr=" + rgbaPtr);

    // === 2. Load TXCI (uncompressed) ===
    console.log("\n=== 2. Load TXCI ===");
    const tciBuf = fs.readFileSync(TXCI_PATH);
    const tciPtr = M._tx_malloc(tciBuf.length);
    M.HEAPU8.set(tciBuf, tciPtr);
    console.log("  TXCI: " + tciBuf.length + " bytes at ptr=" + tciPtr);

    // === 3. Open world ===
    console.log("\n=== 3. Open world ===");
    const hp = M._tx_malloc(4);
    const wp = as(WLD_PATH);
    const st = M._terra_world_open(wp, hp);
    const h = getValue(hp, "i32");
    if (st !== 0) {
        console.error("  FAIL: open world returned " + st + ": " + getError());
        process.exit(1);
    }
    console.log("  World opened: handle=" + h);

    // === 4. Apply pixel art ===
    console.log("\n=== 4. Apply pixel art ===");
    const startX = 100;
    const startY = 200;
    const preferWall = 0;
    const blockInactive = 0;
    console.log("  Params: " + imgInfo.width + "x" + imgInfo.height +
                " at (" + startX + ", " + startY + ")" +
                " prefer_wall=" + preferWall + " block_inactive=" + blockInactive);
    console.log("  Calling txw_apply_pixel_art...");

    const t2 = Date.now();
    const ret = M._txw_apply_pixel_art(
        h,
        rgbaPtr, rgbaBuf.length,
        imgInfo.width, imgInfo.height,
        tciPtr, tciBuf.length,
        startX, startY,
        preferWall, blockInactive
    );
    const dt = Date.now() - t2;
    if (ret === 0) {
        console.log("  txw_apply_pixel_art -> 0 (" + dt + "ms) OK");
    } else {
        console.error("  FAIL: txw_apply_pixel_art returned " + ret + ": " + getError());
        process.exit(1);
    }

    // === 5. Save world ===
    console.log("\n=== 5. Save world ===");
    const t3 = Date.now();
    const st2 = M._terra_world_save(h, as(OUTPUT_PATH));
    const dt2 = Date.now() - t3;
    if (st2 === 0) {
        console.log("  Saved to: " + OUTPUT_PATH + " (" + dt2 + "ms)");
        const stat = fs.statSync(OUTPUT_PATH);
        console.log("  Output size: " + (stat.size / 1024 / 1024).toFixed(2) + " MB");
    } else {
        console.error("  FAIL: save returned " + st2 + ": " + getError());
        process.exit(1);
    }

    // === 6. Verify by reopening ===
    console.log("\n=== 6. Verify ===");
    const hp2 = M._tx_malloc(4);
    const wp2 = as(OUTPUT_PATH);
    const st3 = M._terra_world_open(wp2, hp2);
    const h2 = getValue(hp2, "i32");
    if (st3 === 0 && h2) {
        console.log("  Re-opened saved world OK (handle=" + h2 + ")");
        M._terra_world_close(h2);
    } else {
        console.error("  FAIL: reopen returned " + st3 + ": " + getError());
    }

    M._terra_world_close(h);

    console.log("\n=== DONE ===");
    console.log("Output: " + OUTPUT_PATH);
    console.log("You can open this .wld in Terraria or a world viewer to check the pixel art.");
}

main().catch(e => { console.error("FATAL:", e); process.exit(1); });
