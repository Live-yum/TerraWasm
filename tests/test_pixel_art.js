"use strict";
const path = require("path");
const fs = require("fs");
const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));
const { getPrimaryWorldPath } = require("./helpers/fixtures");

const WLD_PATH = getPrimaryWorldPath();
const RGBA_SMALL = path.join(__dirname, "test_small.rgba");
const RGBA_FULL = path.join(__dirname, "test_quantized.rgba");
const TXCI_PATH = path.resolve(__dirname, "..", "data", "terraria_color_index.txci");

let passed = 0, failed = 0;
const failures = [];
function ok(n) { passed++; console.log("  PASS: " + n); }
function fail(n, m) { failed++; failures.push(n); console.log("  FAIL: " + n + " - " + m); }

async function main() {
    console.log("Loading WASM...");
    const t0 = Date.now();
    const M = await TerraWorldWasm();
    const { UTF8ToString, stringToUTF8, lengthBytesUTF8, getValue } = M;
    console.log("  WASM loaded in " + (Date.now() - t0) + "ms");

    function as(s) { const l = lengthBytesUTF8(s) + 1; const p = M._tx_malloc(l); stringToUTF8(s, p, l); return p; }
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

    function loadRGBA(filePath) {
        const buf = fs.readFileSync(filePath);
        const ptr = M._tx_malloc(buf.length);
        M.HEAPU8.set(buf, ptr);
        return { buf, ptr };
    }

    function loadTxci() {
        const buf = fs.readFileSync(TXCI_PATH);
        const ptr = M._tx_malloc(buf.length);
        M.HEAPU8.set(buf, ptr);
        return { buf, ptr };
    }

    function openWorld(wldPath) {
        const hp = M._tx_malloc(4);
        const wp = as(wldPath);
        const st = M._terra_world_open(wp, hp);
        const h = getValue(hp, "i32");
        return { st, h };
    }

    // === 1. Small image (16x16, 4 solid colors) ===
    console.log("\n=== 1. Small image (16x16, 4 colors) ===");
    {
        const { st, h } = openWorld(WLD_PATH);
        if (st !== 0) { fail("open world", "st=" + st); process.exit(1); }
        ok("open world");

        const { buf: rgbaBuf, ptr: rgbaPtr } = loadRGBA(RGBA_SMALL);
        const { buf: txciBuf, ptr: txciPtr } = loadTxci();
        ok("load RGBA + TXCI into WASM memory");

        const t1 = Date.now();
        const ret = M._txw_apply_pixel_art(
            h, rgbaPtr, rgbaBuf.length, 16, 16,
            txciPtr, txciBuf.length, 100, 200, 0, 0
        );
        const dt = Date.now() - t1;
        if (ret === 0) ok("txw_apply_pixel_art(16x16) -> 0 (" + dt + "ms)");
        else fail("txw_apply_pixel_art(16x16)", "returned " + ret + ": " + getError());

        const savePath = path.join(__dirname, "tmp_pixel_small.wld");
        const t2 = Date.now();
        const st2 = M._terra_world_save(h, as(savePath));
        const dt2 = Date.now() - t2;
        if (st2 === 0) ok("save with pixel art (" + dt2 + "ms)");
        else fail("save with pixel art", "st=" + st2);

        if (st2 === 0) {
            const { st: st3, h: h2 } = openWorld(savePath);
            if (st3 === 0 && h2) ok("reopen saved world"); else fail("reopen saved world", "st=" + st3);
            if (h2) M._terra_world_close(h2);
            if (fs.existsSync(savePath)) fs.unlinkSync(savePath);
        }
        M._terra_world_close(h);
    }

    // === 2. Full-size image (768x768, quantized to 256 colors) ===
    console.log("\n=== 2. Full-size image (768x768, 256 colors) ===");
    {
        const { st, h } = openWorld(WLD_PATH);
        if (st !== 0) { fail("open world for full test", "st=" + st); process.exit(1); }

        const { buf: rgbaBuf, ptr: rgbaPtr } = loadRGBA(RGBA_FULL);
        const { buf: txciBuf, ptr: txciPtr } = loadTxci();

        const t1 = Date.now();
        const ret = M._txw_apply_pixel_art(
            h, rgbaPtr, rgbaBuf.length, 768, 768,
            txciPtr, txciBuf.length, 50, 50, 1, 0
        );
        const dt = Date.now() - t1;
        if (ret === 0) ok("txw_apply_pixel_art(768x768) -> 0 (" + dt + "ms)");
        else fail("txw_apply_pixel_art(768x768)", "returned " + ret + ": " + getError());

        const savePath = path.join(__dirname, "tmp_pixel_full.wld");
        const t2 = Date.now();
        const st2 = M._terra_world_save(h, as(savePath));
        const dt2 = Date.now() - t2;
        if (st2 === 0) ok("save full pixel art (" + dt2 + "ms)");
        else fail("save full pixel art", "st=" + st2);

        if (st2 === 0) {
            const { st: st3, h: h2 } = openWorld(savePath);
            if (st3 === 0 && h2) ok("reopen full pixel art world"); else fail("reopen full pixel art world", "st=" + st3);
            if (h2) M._terra_world_close(h2);
            if (fs.existsSync(savePath)) fs.unlinkSync(savePath);
        }
        M._terra_world_close(h);
    }

    // === 3. prefer_wall=1 test ===
    console.log("\n=== 3. prefer_wall=1 ===");
    {
        const { st, h } = openWorld(WLD_PATH);
        if (st !== 0) { fail("open world for wall test", "st=" + st); process.exit(1); }

        const { buf: rgbaBuf, ptr: rgbaPtr } = loadRGBA(RGBA_SMALL);
        const { buf: txciBuf, ptr: txciPtr } = loadTxci();

        const ret = M._txw_apply_pixel_art(
            h, rgbaPtr, rgbaBuf.length, 16, 16,
            txciPtr, txciBuf.length, 10, 10, 1, 0
        );
        if (ret === 0) ok("txw_apply_pixel_art(prefer_wall=1) -> 0");
        else fail("txw_apply_pixel_art(prefer_wall=1)", "returned " + ret + ": " + getError());

        const savePath = path.join(__dirname, "tmp_pixel_wall.wld");
        const st2 = M._terra_world_save(h, as(savePath));
        if (st2 === 0) ok("save with prefer_wall"); else fail("save with prefer_wall", "st=" + st2);

        if (h) M._terra_world_close(h);
        if (fs.existsSync(savePath)) fs.unlinkSync(savePath);
    }

    // === 4. block_inactive=1 test ===
    console.log("\n=== 4. block_inactive=1 ===");
    {
        const { st, h } = openWorld(WLD_PATH);
        if (st !== 0) { fail("open world for inactive test", "st=" + st); process.exit(1); }

        const { buf: rgbaBuf, ptr: rgbaPtr } = loadRGBA(RGBA_SMALL);
        const { buf: txciBuf, ptr: txciPtr } = loadTxci();

        const ret = M._txw_apply_pixel_art(
            h, rgbaPtr, rgbaBuf.length, 16, 16,
            txciPtr, txciBuf.length, 10, 10, 0, 1
        );
        if (ret === 0) ok("txw_apply_pixel_art(block_inactive=1) -> 0");
        else fail("txw_apply_pixel_art(block_inactive=1)", "returned " + ret + ": " + getError());

        const savePath = path.join(__dirname, "tmp_pixel_inactive.wld");
        const st2 = M._terra_world_save(h, as(savePath));
        if (st2 === 0) ok("save with block_inactive"); else fail("save with block_inactive", "st=" + st2);

        if (h) M._terra_world_close(h);
        if (fs.existsSync(savePath)) fs.unlinkSync(savePath);
    }

    // === 5. Negative start coordinates ===
    console.log("\n=== 5. Negative start coordinates ===");
    {
        const { st, h } = openWorld(WLD_PATH);
        if (st !== 0) { fail("open world for negative coords", "st=" + st); process.exit(1); }

        const { buf: rgbaBuf, ptr: rgbaPtr } = loadRGBA(RGBA_SMALL);
        const { buf: txciBuf, ptr: txciPtr } = loadTxci();

        const ret = M._txw_apply_pixel_art(
            h, rgbaPtr, rgbaBuf.length, 16, 16,
            txciPtr, txciBuf.length, -100, -200, 0, 0
        );
        if (ret === 0) ok("txw_apply_pixel_art(negative coords) -> 0");
        else fail("txw_apply_pixel_art(negative coords)", "returned " + ret + ": " + getError());

        if (h) M._terra_world_close(h);
    }

    // === 6. Error cases ===
    console.log("\n=== 6. Error cases ===");
    {
        const { st, h } = openWorld(WLD_PATH);
        const { buf: rgbaBuf, ptr: rgbaPtr } = loadRGBA(RGBA_SMALL);
        const { buf: txciBuf, ptr: txciPtr } = loadTxci();

        const err1 = M._txw_apply_pixel_art(0, rgbaPtr, rgbaBuf.length, 16, 16, txciPtr, txciBuf.length, 0, 0, 0, 0);
        if (err1 === -1) ok("null world -> -1"); else fail("null world", "expected -1, got " + err1);

        const err2 = M._txw_apply_pixel_art(h, rgbaPtr, rgbaBuf.length, 0, 0, txciPtr, txciBuf.length, 0, 0, 0, 0);
        if (err2 === -1) ok("zero dimensions -> -1"); else fail("zero dimensions", "expected -1, got " + err2);

        const err3 = M._txw_apply_pixel_art(h, rgbaPtr, 10, 16, 16, txciPtr, txciBuf.length, 0, 0, 0, 0);
        if (err3 === -1) ok("image data too short -> -1"); else fail("image data too short", "expected -1, got " + err3);

        const err4 = M._txw_apply_pixel_art(h, rgbaPtr, rgbaBuf.length, 16, 16, 0, txciBuf.length, 0, 0, 0, 0);
        if (err4 === -1) ok("null TXCI ptr -> -1"); else fail("null TXCI ptr", "expected -1, got " + err4);

        const err5 = M._txw_apply_pixel_art(h, rgbaPtr, rgbaBuf.length, 16, 16, txciPtr, 10, 0, 0, 0, 0);
        if (err5 === -1) ok("TXCI data too small -> -1"); else fail("TXCI data too small", "expected -1, got " + err5);

        if (h) M._terra_world_close(h);
    }

    // === 7. Re-apply (overwrite previous pixel art) ===
    console.log("\n=== 7. Re-apply pixel art ===");
    {
        const { st, h } = openWorld(WLD_PATH);
        if (st !== 0) { fail("open world for re-apply", "st=" + st); process.exit(1); }

        const { buf: rgbaBuf, ptr: rgbaPtr } = loadRGBA(RGBA_SMALL);
        const { buf: txciBuf, ptr: txciPtr } = loadTxci();

        // First apply
        let ret = M._txw_apply_pixel_art(h, rgbaPtr, rgbaBuf.length, 16, 16, txciPtr, txciBuf.length, 0, 0, 0, 0);
        if (ret !== 0) { fail("first apply", "returned " + ret); }
        else {
            // Second apply (should overwrite)
            ret = M._txw_apply_pixel_art(h, rgbaPtr, rgbaBuf.length, 16, 16, txciPtr, txciBuf.length, 500, 500, 0, 0);
            if (ret === 0) ok("re-apply pixel art -> 0");
            else fail("re-apply pixel art", "returned " + ret + ": " + getError());
        }

        const savePath = path.join(__dirname, "tmp_pixel_reapply.wld");
        const st2 = M._terra_world_save(h, as(savePath));
        if (st2 === 0) ok("save after re-apply"); else fail("save after re-apply", "st=" + st2);

        if (h) M._terra_world_close(h);
        if (fs.existsSync(savePath)) fs.unlinkSync(savePath);
    }

    // === Results ===
    console.log("\n=== Results ===");
    console.log("  Passed: " + passed);
    console.log("  Failed: " + failed);
    if (failures.length > 0) {
        console.log("  Failures:");
        failures.forEach(f => console.log("    - " + f));
    }
    process.exit(failed > 0 ? 1 : 0);
}
main().catch(e => { console.error("FATAL:", e); process.exit(1); });
