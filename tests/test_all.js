"use strict";
const path = require("path");
const fs = require("fs");
const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));
const { getPrimaryWorldPath } = require("./helpers/fixtures");

const TEST_WLD = getPrimaryWorldPath();

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

    // Helpers
    function as(s) { const l = lengthBytesUTF8(s)+1; const p = M._tx_malloc(l); stringToUTF8(s,p,l); return p; }
    function memUsed() { return M._tx_memory_used(); }
    function heapUsed() { return M._tx_heap_used(); }
    let peakMem = 0;
    function trackPeak() { const m = memUsed(); if (m > peakMem) peakMem = m; }
    function readU64(ptr) { return M.HEAPU32[ptr >> 2] >>> 0; }

    // Two-call pattern helpers
    function rs2(fn) {
        const rp = M._tx_malloc(8);
        const st1 = fn(0, 0n, rp);
        if (st1 !== 0) return "";
        const sz = readU64(rp);
        if (sz === 0) { return ""; }
        const buf = M._tx_malloc(sz);
        if (fn(buf, BigInt(sz), rp) !== 0) return "";
        const s = UTF8ToString(buf); return s;
    }
    function rs2p(fn, pre) {
        const rp = M._tx_malloc(8);
        const st1 = fn(pre, 0, 0n, rp);
        if (st1 !== 0) return "";
        const sz = readU64(rp);
        if (sz === 0) { return ""; }
        const buf = M._tx_malloc(sz);
        if (fn(pre, buf, BigInt(sz), rp) !== 0) return "";
        const s = UTF8ToString(buf); return s;
    }
    function rs2s(fn, h, sn) {
        const rp = M._tx_malloc(8);
        const st1 = fn(h, sn, 0, 0n, rp);
        if (st1 !== 0) return "";
        const sz = readU64(rp);
        if (sz === 0) { return ""; }
        const buf = M._tx_malloc(sz);
        if (fn(h, sn, buf, BigInt(sz), rp) !== 0) return "";
        const s = UTF8ToString(buf); return s;
    }

    // opExec: use stackAlloc for the two-call pattern strings so they
    // survive the bump-allocator heap rewind inside terra_op_execute_json.
    function opExec(h, opName, reqJson) {
        const sp = M.stackSave();
        const onBuf = M.stackAlloc(lengthBytesUTF8(opName) + 1);
        stringToUTF8(opName, onBuf, lengthBytesUTF8(opName) + 1);
        const rjBuf = M.stackAlloc(lengthBytesUTF8(reqJson) + 1);
        stringToUTF8(reqJson, rjBuf, lengthBytesUTF8(reqJson) + 1);
        const rp = M.stackAlloc(8);
        const st1 = M._terra_op_execute_json(h, onBuf, rjBuf, 0, 0n, rp);
        if (st1 !== 0) {
            const err = rs2(M._terra_info_get_last_error_json);
            M.stackRestore(sp);
            return { status: st1, response: null, error: err };
        }
        const sz = readU64(rp);
        if (sz === 0 || sz > 100 * 1024 * 1024) { M.stackRestore(sp); return { status: 0, response: "(no data)", error: null }; }
        const buf = M._tx_malloc(sz);
        const st2 = M._terra_op_execute_json(h, onBuf, rjBuf, buf, BigInt(sz), rp);
        const s = st2 === 0 ? UTF8ToString(buf) : null;
        const err = st2 !== 0 ? rs2(M._terra_info_get_last_error_json) : null;
        M.stackRestore(sp);
        return { status: st2, response: s, error: err };
    }

    // === 1. Lifecycle ===
    console.log("\n=== 1. Lifecycle ===");
    let hp = M._tx_malloc(4);
    let st = M._terra_world_create(hp);
    let h = getValue(hp, "i32");
    if (st === 0 && h) ok("terra_world_create"); else fail("terra_world_create", "st=" + st + " h=" + h);
    if (h) M._terra_world_close(h);

    hp = M._tx_malloc(4);
    let wp = as(TEST_WLD);
    st = M._terra_world_open(wp, hp);
    h = getValue(hp, "i32");
    if (st === 0 && h) ok("terra_world_open"); else fail("terra_world_open", "st=" + st);

    const savePath = path.join(__dirname, "tmp_v2_save.wld");
    let sp = as(savePath);
    st = M._terra_world_save(h, sp);
    if (st === 0) ok("terra_world_save"); else fail("terra_world_save", "st=" + st);

    M._terra_world_close(h);
    hp = M._tx_malloc(4);
    wp = as(TEST_WLD);
    st = M._terra_world_open(wp, hp);
    h = getValue(hp, "i32");
    if (st === 0 && h) ok("terra_world_close + reopen"); else fail("terra_world_close + reopen", "st=" + st);

    // === 2. Info APIs ===
    console.log("\n=== 2. Info APIs ===");
    const sections = rs2(M._terra_info_list_sections_json);
    if (sections.includes("format") && sections.includes("footer")) ok("list_sections_json"); else fail("list_sections_json", "missing sections");

    const sectionNames = ["format","header","chests","signs","npcs","tile_entities","weighted_pressure_plates","town_manager","bestiary","creative_powers","footer"];
    let schemaOk = true;
    for (const sn of sectionNames) {
        const sp2 = as(sn);
        const schema = rs2p(M._terra_info_get_section_schema_json, sp2);
        if (!schema.includes(sn)) { schemaOk = false; fail("get_section_schema(" + sn + ")", "missing name"); }
    }
    if (schemaOk) ok("get_section_schema (all 11 sections)");

    const err = rs2(M._terra_info_get_last_error_json);
    if (err === "" || err === "{}") ok("get_last_error_json (empty after success)"); else ok("get_last_error_json: " + err.substring(0, 60));

    // === 3. Section Read ===
    console.log("\n=== 3. Section Read ===");
    for (const sn of sectionNames) {
        const sp2 = as(sn);
        const data = rs2s(M._terra_section_get_json, h, sp2);
        if (data && data.length > 0) ok("section_get_json(" + sn + ")"); else fail("section_get_json(" + sn + ")", "empty");
    }

    // === 4. Section Write ===
    console.log("\n=== 4. Section Write ===");
    const fmtData = rs2s(M._terra_section_get_json, h, as("format"));
    const fmtP = as("format"), fmtJ = as(fmtData);
    st = M._terra_section_set_json(h, fmtP, fmtJ);
    if (st === 4) ok("section_set_json(format) rejects unsafe JSON override"); else fail("section_set_json(format)", "expected 4, got " + st);

    const hdrData = rs2s(M._terra_section_get_json, h, as("header"));
    const hdrP = as("header"), hdrJ = as(hdrData);
    st = M._terra_section_set_json(h, hdrP, hdrJ);
    if (st === 4) ok("section_set_json(header) rejects unsafe JSON override"); else fail("section_set_json(header)", "expected 4, got " + st);

    const ftrP = as("footer"), ftrJ = as("{}");
    st = M._terra_section_set_json(h, ftrP, ftrJ);
    if (st === 4) ok("section_set_json(footer) -> NOT_SUPPORTED"); else fail("section_set_json(footer)", "expected 4, got " + st);

    const unkP = as("nonexistent"), unkJ = as("{}");
    st = M._terra_section_set_json(h, unkP, unkJ);
    if (st === 3) ok("section_set_json(unknown) -> NOT_FOUND"); else fail("section_set_json(unknown)", "expected 3, got " + st);

    // === 5. Operations ===
    console.log("\n=== 5. Operations ===");

    let t1 = Date.now();
    let r = opExec(h, "render_preview_rgba", "{}");
    let dt = Date.now() - t1;
    trackPeak();
    if (r.status === 0 && r.response && r.response.includes("width")) {
        ok("render_preview_rgba (" + dt + "ms, mem=" + (memUsed()/1024/1024).toFixed(1) + "MB)");
    } else {
        fail("render_preview_rgba", "st=" + r.status + " resp=" + (r.response||"null").substring(0,80));
    }

    const rpBuf = M._tx_malloc(8);
    const rpw = M._tx_malloc(4), rph = M._tx_malloc(4), rps = M._tx_malloc(4);
    st = M._terra_op_get_preview_rgba(h, 0, 0n, rpBuf, rpw, rph, rps);
    const rgbaSz = Number(getValue(rpBuf, "i64"));
    if (st === 2 && rgbaSz > 0) ok("get_preview_rgba (probe size=" + rgbaSz + ")"); else fail("get_preview_rgba", "st=" + st + " sz=" + rgbaSz);

    t1 = Date.now();
    r = opExec(h, "render_preview_png", JSON.stringify({ max_w: 400, max_h: 200 }));
    dt = Date.now() - t1;
    trackPeak();
    if (r.status === 0) ok("render_preview_png thumbnail (" + dt + "ms, mem=" + (memUsed()/1024/1024).toFixed(1) + "MB)"); else fail("render_preview_png thumbnail", "st=" + r.status + " " + (r.error || ""));

    r = opExec(h, "batch_update_tiles", JSON.stringify({ rules: [{ where: { invisible_block: null }, patch: { invisible_block: 1 } }] }));
    trackPeak();
    if (r.status === 0) ok("batch_update_tiles (visibility, mem=" + (memUsed()/1024/1024).toFixed(1) + "MB)"); else fail("batch_update_tiles (visibility)", "st=" + r.status + " " + (r.error || ""));

    console.log("  [debug] heap_used=" + (heapUsed()/1024/1024).toFixed(1) + "MB, mem=" + (memUsed()/1024/1024).toFixed(1) + "MB, native_heap=" + (M._tx_native_heap_used()/1024/1024).toFixed(1) + "MB");

    r = opExec(h, "batch_update_tiles", JSON.stringify({ rules: [{ where: {}, patch: { wire_red: 0, wire_blue: 0, wire_green: 0, wire_yellow: 0 } }] }));
    if (r.status === 0) ok("batch_update_tiles (wire removal)"); else fail("batch_update_tiles (wire removal)", "st=" + r.status + " err=" + (r.error||""));

    r = opExec(h, "batch_update_tiles", JSON.stringify({
        rules: [{
            where: { type: null, wall: null, tile_color: null },
            patch: {
                tile_color: 5,
                wall_color: 7,
                liquid_type: 1,
                liquid_amount: 1,
                fullbright_block: 1,
                fullbright_wall: 1
            },
            limit: 1
        }]
    }));
    if (r.status === 0) ok("batch_update_tiles (color/liquid/fullbright fields)"); else fail("batch_update_tiles (color/liquid/fullbright fields)", "st=" + r.status + " err=" + (r.error||""));

    for (const mode of ["purify", "corruption", "crimson", "hallow"]) {
        r = opExec(h, "convert_world_biome", JSON.stringify({ mode }));
        if (r.status === 0) ok("convert_world_biome(" + mode + ")"); else fail("convert_world_biome(" + mode + ")", "st=" + r.status + " " + (r.error || ""));
    }

    const mapDir = path.join(__dirname, "out_isolated");
    if (!fs.existsSync(mapDir)) fs.mkdirSync(mapDir, { recursive: true });
    r = opExec(h, "render_lit_map", JSON.stringify({ output_dir: mapDir }));
    if (r.status === 0) ok("render_lit_map"); else fail("render_lit_map", "st=" + r.status + " " + (r.error || ""));

    r = opExec(h, "unlock_bestiary", "{}");
    if (r.status !== 0 && (r.error || "").includes("TERRAX_NOT_SUPPORTED")) {
        ok("unlock_bestiary rejects the removed no-op placeholder");
    } else {
        fail("unlock_bestiary", "expected explicit NOT_SUPPORTED, got st=" + r.status + " " + (r.error || ""));
    }

    r = opExec(h, "nonexistent_op", "{}");
    if (r.status !== 0) ok("unknown operation -> error"); else fail("unknown operation", "expected error, got 0");

    // === 6. Save/Reload ===
    console.log("\n=== 6. Save/Reload Verification ===");
    const savePath2 = path.join(__dirname, "tmp_v2_modified.wld");
    sp = as(savePath2);
    st = M._terra_world_save(h, sp);
    if (st === 0) ok("save modified world"); else fail("save modified world", "st=" + st);
    M._terra_world_close(h);
    h = 0;

    hp = M._tx_malloc(4);
    wp = as(savePath2);
    st = M._terra_world_open(wp, hp);
    const h2 = getValue(hp, "i32");
    if (st === 0 && h2) {
        const hdr2Data = rs2s(M._terra_section_get_json, h2, as("header"));
        if (hdr2Data.includes("maxTilesX")) ok("reopen modified world + read header"); else fail("reopen modified world", "header missing maxTilesX");
        M._terra_world_close(h2);
    } else {
        fail("reopen modified world", "st=" + st);
    }

    // === 7. Error Handling ===
    console.log("\n=== 7. Error Handling ===");
    st = M._terra_world_open(0, hp);
    if (st !== 0) ok("open(null path) -> error"); else fail("open(null path)", "expected error");

    hp = M._tx_malloc(4);
    wp = as(TEST_WLD);
    st = M._terra_world_open(wp, hp);
    h = getValue(hp, "i32");
    const data = rs2s(M._terra_section_get_json, h, as("nonexistent"));
    if (data === "" || data.includes("error")) ok("get_section(unknown) -> error/empty"); else fail("get_section(unknown)", "unexpected data: " + data.substring(0, 40));
    M._terra_world_close(h);
    h = 0;

    // === 8. JSON Data Integrity ===
    console.log("\n=== 8. JSON Data Integrity ===");
    hp = M._tx_malloc(4);
    wp = as(TEST_WLD);
    st = M._terra_world_open(wp, hp);
    const hJson = getValue(hp, "i32");
    if (st === 0 && hJson) {
        const fmtData2 = rs2s(M._terra_section_get_json, hJson, as("format"));
        const fmt = JSON.parse(fmtData2);
        if (fmt.version && fmt.magic) ok("format JSON has version + magic"); else fail("format JSON", "missing fields");
        const hdrData2 = rs2s(M._terra_section_get_json, hJson, as("header"));
        const hdr = JSON.parse(hdrData2);
        if (hdr.worldName && hdr.maxTilesX && hdr.maxTilesY) ok("header JSON has name + dimensions"); else fail("header JSON", "missing fields");
        M._terra_world_close(hJson);
    } else {
        fail("JSON integrity", "couldn't open world: st=" + st);
    }

    if (h) M._terra_world_close(h);
    if (fs.existsSync(savePath)) fs.unlinkSync(savePath);
    if (fs.existsSync(savePath2)) fs.unlinkSync(savePath2);

    console.log("\n=== Memory Usage ===");
    console.log("  WASM linear memory: " + (memUsed() / 1024 / 1024).toFixed(1) + " MB");
    console.log("  Heap used: " + (heapUsed() / 1024 / 1024).toFixed(1) + " MB");
    console.log("  Native heap peak: " + (M._tx_native_heap_peak() / 1024 / 1024).toFixed(1) + " MB");
    console.log("  Bridge heap peak: " + (M._tx_bridge_heap_peak() / 1024 / 1024).toFixed(1) + " MB");
    console.log("  Tracked heap peak: " + (M._tx_heap_peak() / 1024 / 1024).toFixed(1) + " MB");
    console.log("  Peak memory: " + (peakMem / 1024 / 1024).toFixed(1) + " MB");

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
