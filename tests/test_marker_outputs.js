"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const zlib = require("node:zlib");
const { getPrimaryWorldPath } = require("./helpers/fixtures");

const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));

const TEST_WLD = getPrimaryWorldPath();
const TEST_BYTES = fs.readFileSync(TEST_WLD);
const TXCI_GZ = fs.readFileSync(path.join(__dirname, "..", "data", "terraria_color_index.txci.gz"));
const PNG_SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

let modulePromise;
function loadModule() {
  modulePromise ||= TerraWorldWasm();
  return modulePromise;
}

function mustAlloc(M, size, label) {
  const ptr = M._tx_malloc(size);
  assert.notEqual(ptr, 0, `${label}: allocation failed`);
  return ptr;
}

function readU32(M, ptr) {
  return M.HEAPU32[ptr >>> 2] >>> 0;
}

function readU64(M, ptr) {
  return Number(new DataView(M.wasmMemory.buffer).getBigUint64(ptr, true));
}

function readRequiredSize(M, ptr) {
  return readU64(M, ptr);
}

function readLastErrorJson(M) {
  const sizePtr = mustAlloc(M, 8, "error required size");
  let outputPtr = 0;
  try {
    let status = M._terra_info_get_last_error_json(0, 0n, sizePtr);
    assert.equal(status, 0, "last-error probe must succeed");
    const required = readRequiredSize(M, sizePtr);
    if (!required) return {};
    outputPtr = mustAlloc(M, required, "error output");
    status = M._terra_info_get_last_error_json(outputPtr, BigInt(required), sizePtr);
    assert.equal(status, 0, "last-error copy must succeed");
    return JSON.parse(M.UTF8ToString(outputPtr));
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    M._tx_free(sizePtr);
  }
}

function operationFailureMessage(operationName, status, error) {
  return `${operationName} failed with status=${status}, error=${JSON.stringify(error)}`;
}

function allocCString(M, value) {
  const size = M.lengthBytesUTF8(value) + 1;
  const ptr = mustAlloc(M, size, "C string");
  M.stringToUTF8(value, ptr, size);
  return ptr;
}

function openWorld(M, bytes = TEST_BYTES) {
  const inputPtr = mustAlloc(M, bytes.length, "world input");
  const handlePtr = mustAlloc(M, 4, "world handle");
  M.HEAPU8.set(bytes, inputPtr);
  const status = M._terra_world_open_from_buffer(inputPtr, bytes.length, handlePtr);
  assert.equal(status, 0);
  return { handle: readU32(M, handlePtr), inputPtr, handlePtr };
}

function closeWorld(M, opened) {
  if (!opened) return;
  if (opened.handle) M._terra_world_close(opened.handle);
  M._tx_free(opened.handlePtr);
  M._tx_free(opened.inputPtr);
}

function installSolidMarkerIcon(M, handle, itemId, rgba = [12, 34, 56, 255]) {
  const iconSize = 4;
  const iconBytes = Buffer.alloc(iconSize * iconSize * 4);
  for (let offset = 0; offset < iconBytes.length; offset += 4) iconBytes.set(rgba, offset);
  const rgbaPtr = mustAlloc(M, iconBytes.length, "marker icon RGBA");
  const idsPtr = mustAlloc(M, 4, "marker icon ID");
  const xOffsetsPtr = mustAlloc(M, 4, "marker icon X offset");
  const yOffsetsPtr = mustAlloc(M, 4, "marker icon Y offset");
  M.HEAPU8.set(iconBytes, rgbaPtr);
  new DataView(M.HEAPU8.buffer).setInt32(idsPtr, itemId, true);
  new DataView(M.HEAPU8.buffer).setUint32(xOffsetsPtr, 0, true);
  new DataView(M.HEAPU8.buffer).setUint32(yOffsetsPtr, 0, true);
  try {
    const status = M._txw_set_icon_atlas(
      handle, rgbaPtr, iconSize, 1, iconSize, iconSize,
      idsPtr, xOffsetsPtr, yOffsetsPtr,
    );
    assert.equal(status, 1, "marker icon atlas must accept one icon");
  } finally {
    M._tx_free(yOffsetsPtr);
    M._tx_free(xOffsetsPtr);
    M._tx_free(idsPtr);
    M._tx_free(rgbaPtr);
  }
}

function installMarkerColorIndexBytes(M, handle, bytes) {
  const dataPtr = mustAlloc(M, bytes.length, "marker color index");
  M.HEAPU8.set(bytes, dataPtr);
  try {
    return M._txw_set_marker_color_index(handle, dataPtr, bytes.length);
  } finally {
    M._tx_free(dataPtr);
  }
}

function installMarkerColorIndex(M, handle) {
  const status = installMarkerColorIndexBytes(M, handle, TXCI_GZ);
  assert.equal(status, 0, "marker color index must load successfully");
}

function makeLongMarkerColorIndex() {
  const itemCount = 34;
  const brickCount = 512;
  const itemsOffset = 56;
  const directoryOffset = itemsOffset + itemCount * 6;
  const payloadOffset = directoryOffset + brickCount * 8;
  const bytes = Buffer.alloc(payloadOffset + 2);

  bytes.writeUInt32LE(0x49435854, 0);
  bytes.writeUInt16LE(3, 4);
  bytes.writeUInt16LE(32, 6);
  bytes.writeUInt32LE(1, 8);
  bytes.writeUInt32LE(itemCount, 12);
  bytes.writeUInt32LE(brickCount, 16);
  bytes.writeUInt32LE(44, 20);
  bytes.writeUInt32LE(48, 24);
  bytes.writeUInt32LE(itemsOffset, 28);
  bytes.writeUInt32LE(directoryOffset, 32);
  bytes.writeUInt32LE(payloadOffset, 36);
  bytes[44] = 0x11;
  bytes[45] = 0x22;
  bytes[46] = 0x33;
  bytes.writeUInt32LE(0, 48);
  bytes.writeUInt32LE(itemCount, 52);

  for (let item = 0; item < itemCount; item += 1) {
    const offset = itemsOffset + item * 6;
    const kindAndId = item === itemCount - 1 ? (0x8000 | 27) : 0;
    bytes.writeUInt16LE(kindAndId, offset);
    bytes.writeUInt16LE(0, offset + 2);
    bytes[offset + 4] = 0;
  }
  for (let brick = 0; brick < brickCount; brick += 1) {
    const offset = directoryOffset + brick * 8;
    bytes[offset] = 0;
    bytes.writeUInt32LE(0, offset + 4);
  }
  bytes.writeUInt16LE(0, payloadOffset);
  return bytes;
}

function callStringApi(M, invoke) {
  const sizePtr = mustAlloc(M, 8, "required size");
  let outputPtr = 0;
  try {
    let status = invoke(0, 0n, sizePtr);
    if (status !== 0) return { status, value: null };
    const required = readRequiredSize(M, sizePtr);
    outputPtr = mustAlloc(M, required, "string output");
    status = invoke(outputPtr, BigInt(required), sizePtr);
    return { status, value: status === 0 ? M.UTF8ToString(outputPtr) : null };
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    M._tx_free(sizePtr);
  }
}

function readSection(M, handle, name) {
  const namePtr = allocCString(M, name);
  try {
    const result = callStringApi(M, (buffer, size, required) =>
      M._terra_section_get_json(handle, namePtr, buffer, size, required));
    assert.equal(result.status, 0);
    return JSON.parse(result.value);
  } finally {
    M._tx_free(namePtr);
  }
}

function computePreviewSize(worldWidth, worldHeight, maxW, maxH) {
  let width = worldWidth;
  let height = worldHeight;
  if (maxW || maxH) {
    let scaleNum = maxW || worldWidth;
    let scaleDen = worldWidth;
    const yNum = maxH || worldHeight;
    const yDen = worldHeight;
    if (yNum * scaleDen < scaleNum * yDen) {
      scaleNum = yNum;
      scaleDen = yDen;
    }
    if (scaleNum > scaleDen) scaleNum = scaleDen;
    width = Math.floor((worldWidth * scaleNum) / scaleDen);
    height = Math.floor((worldHeight * scaleNum) / scaleDen);
  }
  return { width, height };
}

function readU16LE(buffer, state) {
  const value = buffer.readUInt16LE(state.offset);
  state.offset += 2;
  return value;
}

function listTileRuns(format, header, tileType, previewWidth, previewHeight, minimumPreviewRows = 2) {
  const tileStart = format.positions[1];
  const tileEnd = format.positions[2];
  const important = format.tileFrameImportantBitmap || [];
  const state = { offset: tileStart };
  const runs = [];
  for (let x = 0; x < header.maxTilesX; x += 1) {
    for (let y = 0; y < header.maxTilesY;) {
      assert.ok(state.offset < tileEnd, "tile stream ended early while searching for a long run");
      const f1 = TEST_BYTES[state.offset];
      state.offset += 1;
      let f2 = 0;
      let f3 = 0;
      if (f1 & 1) {
        f2 = TEST_BYTES[state.offset];
        state.offset += 1;
      }
      if (f2 & 1) {
        f3 = TEST_BYTES[state.offset];
        state.offset += 1;
      }
      if (f3 & 1) state.offset += 1;

      const active = (f1 >> 1) & 1;
      let type = -1;
      if (active) {
        type = f1 & 32 ? readU16LE(TEST_BYTES, state) : TEST_BYTES[state.offset++];
        if (important[type]) state.offset += 4;
      }
      if (f3 & 8) state.offset += 1;
      if (f1 & 4) state.offset += f3 & 64 ? 2 : 1;
      if (f3 & 16) state.offset += 1;
      if ((f1 >> 3) & 3) state.offset += 1;

      let same = 0;
      const rle = (f1 >> 6) & 3;
      if (rle === 1) same = TEST_BYTES[state.offset++];
      else if (rle) same = readU16LE(TEST_BYTES, state);

      const run = same + 1;
      if (active && type === tileType) {
        const px = Math.min(previewWidth - 1, Math.floor((x * previewWidth) / header.maxTilesX));
        const py0 = Math.floor((y * previewHeight) / header.maxTilesY);
        let py1 = Math.floor(((y + run) * previewHeight) / header.maxTilesY);
        if (py1 <= py0) py1 = py0 + 1;
        if (py1 - py0 >= minimumPreviewRows) {
          const midpoint = Math.min(previewHeight - 1, Math.floor((((y + Math.floor(run / 2)) * previewHeight) / header.maxTilesY)));
          runs.push({ x, y, run, px, py0, py1, midpoint });
        }
      }
      y += run;
    }
  }
  return runs;
}

function executeOperation(M, handle, name, request) {
  const namePtr = allocCString(M, name);
  const requestPtr = allocCString(M, JSON.stringify(request));
  try {
    return callStringApi(M, (buffer, size, required) =>
      M._terra_op_execute_json(handle, namePtr, requestPtr, buffer, size, required));
  } finally {
    M._tx_free(requestPtr);
    M._tx_free(namePtr);
  }
}

function firstStoredItem(chests) {
  for (const chest of chests) {
    for (const item of chest.items || []) {
      if (item && item.stack > 0) return item.itemType;
    }
  }
  assert.fail("fixture must contain at least one stored chest item");
}

function countChestsWithItem(chests, itemType) {
  return chests.filter((chest) =>
    (chest.items || []).some((item) => item && item.stack > 0 && item.itemType === itemType)).length;
}

function firstStoredChest(chests) {
  for (const chest of chests) {
    if ((chest.items || []).some((item) => item && item.stack > 0)) return chest;
  }
  assert.fail("fixture must contain at least one chest with stored items");
}

function getThumbnailPng(M, handle) {
  assert.equal(typeof M._terra_op_get_thumbnail_png, "function", "thumbnail getter must be exported");
  const sizePtr = mustAlloc(M, 8, "thumbnail size");
  const widthPtr = mustAlloc(M, 4, "thumbnail width");
  const heightPtr = mustAlloc(M, 4, "thumbnail height");
  let outputPtr = 0;
  try {
    let status = M._terra_op_get_thumbnail_png(handle, 0, 0n, sizePtr, widthPtr, heightPtr);
    assert.equal(status, 2, "thumbnail probe must report buffer-too-small");
    const required = readRequiredSize(M, sizePtr);
    assert.ok(required > PNG_SIGNATURE.length, "thumbnail PNG must report a non-empty payload");
    outputPtr = mustAlloc(M, required, "thumbnail output");
    status = M._terra_op_get_thumbnail_png(
      handle, outputPtr, BigInt(required), sizePtr, widthPtr, heightPtr,
    );
    assert.equal(status, 0, `thumbnail copy failed with status ${status}`);
    return {
      png: Buffer.from(M.HEAPU8.slice(outputPtr, outputPtr + required)),
      width: readU32(M, widthPtr),
      height: readU32(M, heightPtr),
      reportedSize: required,
    };
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    M._tx_free(heightPtr);
    M._tx_free(widthPtr);
    M._tx_free(sizePtr);
  }
}

function getMapBytes(M, handle) {
  assert.equal(typeof M._terra_op_get_map, "function", "map output getter must be exported");
  const sizePtr = mustAlloc(M, 8, "map size");
  const widthPtr = mustAlloc(M, 4, "map width");
  const heightPtr = mustAlloc(M, 4, "map height");
  let outputPtr = 0;
  try {
    let status = M._terra_op_get_map(handle, 0, 0n, sizePtr, widthPtr, heightPtr);
    assert.equal(status, 2, "map probe must report buffer-too-small");
    const required = readRequiredSize(M, sizePtr);
    outputPtr = mustAlloc(M, required, "map output");
    status = M._terra_op_get_map(handle, outputPtr, BigInt(required), sizePtr, widthPtr, heightPtr);
    assert.equal(status, 0, `map copy failed with status ${status}`);
    return {
      map: Buffer.from(M.HEAPU8.slice(outputPtr, outputPtr + required)),
      width: readU32(M, widthPtr),
      height: readU32(M, heightPtr),
      reportedSize: required,
    };
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    M._tx_free(heightPtr);
    M._tx_free(widthPtr);
    M._tx_free(sizePtr);
  }
}

function paethPredictor(a, b, c) {
  const p = a + b - c;
  const pa = Math.abs(p - a);
  const pb = Math.abs(p - b);
  const pc = Math.abs(p - c);
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

function decodePngRgba(png) {
  assert.deepEqual(png.subarray(0, PNG_SIGNATURE.length), PNG_SIGNATURE, "PNG signature must match");
  let width = 0;
  let height = 0;
  let bitDepth = 0;
  let colorType = 0;
  let interlace = 0;
  const idatChunks = [];
  let offset = PNG_SIGNATURE.length;
  while (offset + 12 <= png.length) {
    const length = png.readUInt32BE(offset);
    offset += 4;
    const type = png.toString("ascii", offset, offset + 4);
    offset += 4;
    const dataEnd = offset + length;
    const data = png.subarray(offset, dataEnd);
    offset = dataEnd + 4;
    if (type === "IHDR") {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      bitDepth = data[8];
      colorType = data[9];
      interlace = data[12];
    } else if (type === "IDAT") {
      idatChunks.push(data);
    } else if (type === "IEND") {
      break;
    }
  }

  assert.ok(width > 0 && height > 0, "PNG IHDR must describe non-zero dimensions");
  // TerraWasm's current preview encoder writes non-interlaced RGBA8 PNGs.
  assert.equal(bitDepth, 8, "tests rely on the project's RGBA8 preview PNG contract");
  assert.equal(colorType, 6, "tests rely on the project's RGBA preview PNG contract");
  assert.equal(interlace, 0, "tests rely on the project's non-interlaced preview PNG contract");

  const inflated = zlib.inflateSync(Buffer.concat(idatChunks));
  const stride = width * 4;
  const rgba = Buffer.alloc(stride * height);
  let inOffset = 0;
  let outOffset = 0;
  for (let row = 0; row < height; row += 1) {
    const filter = inflated[inOffset];
    inOffset += 1;
    for (let column = 0; column < stride; column += 1) {
      const raw = inflated[inOffset];
      inOffset += 1;
      const left = column >= 4 ? rgba[outOffset + column - 4] : 0;
      const up = row > 0 ? rgba[outOffset + column - stride] : 0;
      const upLeft = row > 0 && column >= 4 ? rgba[outOffset + column - stride - 4] : 0;
      let value = raw;
      switch (filter) {
        case 0:
          break;
        case 1:
          value = (raw + left) & 0xff;
          break;
        case 2:
          value = (raw + up) & 0xff;
          break;
        case 3:
          value = (raw + Math.floor((left + up) / 2)) & 0xff;
          break;
        case 4:
          value = (raw + paethPredictor(left, up, upLeft)) & 0xff;
          break;
        default:
          assert.fail(`unsupported PNG filter ${filter}`);
      }
      rgba[outOffset + column] = value;
    }
    outOffset += stride;
  }

  return { width, height, rgba };
}

function decodePngRgb(png) {
  assert.deepEqual(png.subarray(0, PNG_SIGNATURE.length), PNG_SIGNATURE, "PNG signature must match");
  let width = 0;
  let height = 0;
  let bitDepth = 0;
  let colorType = 0;
  let interlace = 0;
  const idatChunks = [];
  let offset = PNG_SIGNATURE.length;
  while (offset + 12 <= png.length) {
    const length = png.readUInt32BE(offset);
    offset += 4;
    const type = png.toString("ascii", offset, offset + 4);
    offset += 4;
    const dataEnd = offset + length;
    const data = png.subarray(offset, dataEnd);
    offset = dataEnd + 4;
    if (type === "IHDR") {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      bitDepth = data[8];
      colorType = data[9];
      interlace = data[12];
    } else if (type === "IDAT") {
      idatChunks.push(data);
    } else if (type === "IEND") {
      break;
    }
  }

  assert.ok(width > 0 && height > 0, "PNG IHDR must describe non-zero dimensions");
  assert.equal(bitDepth, 8, "native marked previews must use 8-bit RGB");
  assert.equal(colorType, 2, "native marked previews must use RGB PNGs");
  assert.equal(interlace, 0, "native marked previews must be non-interlaced");

  const inflated = zlib.inflateSync(Buffer.concat(idatChunks));
  const stride = width * 3;
  const rgb = Buffer.alloc(stride * height);
  let inOffset = 0;
  let outOffset = 0;
  for (let row = 0; row < height; row += 1) {
    const filter = inflated[inOffset++];
    for (let column = 0; column < stride; column += 1) {
      const raw = inflated[inOffset++];
      const left = column >= 3 ? rgb[outOffset + column - 3] : 0;
      const up = row > 0 ? rgb[outOffset + column - stride] : 0;
      const upLeft = row > 0 && column >= 3 ? rgb[outOffset + column - stride - 3] : 0;
      let value = raw;
      switch (filter) {
        case 0:
          break;
        case 1:
          value = (raw + left) & 0xff;
          break;
        case 2:
          value = (raw + up) & 0xff;
          break;
        case 3:
          value = (raw + Math.floor((left + up) / 2)) & 0xff;
          break;
        case 4:
          value = (raw + paethPredictor(left, up, upLeft)) & 0xff;
          break;
        default:
          assert.fail(`unsupported PNG filter ${filter}`);
      }
      rgb[outOffset + column] = value;
    }
    outOffset += stride;
  }

  const rgba = Buffer.alloc(width * height * 4);
  for (let index = 0; index < width * height; index += 1) {
    rgba[index * 4] = rgb[index * 3];
    rgba[index * 4 + 1] = rgb[index * 3 + 1];
    rgba[index * 4 + 2] = rgb[index * 3 + 2];
    rgba[index * 4 + 3] = 255;
  }
  return { width, height, rgba };
}

function pixelAt(decoded, x, y) {
  const offset = (y * decoded.width + x) * 4;
  return decoded.rgba.subarray(offset, offset + 4);
}

function assertRequestedColorNearChest(decoded, worldWidth, worldHeight, chest, expectedRgb, radius = 3) {
  const centerX = Math.min(decoded.width - 1, Math.floor((chest.x * decoded.width) / worldWidth));
  const centerY = Math.min(decoded.height - 1, Math.floor((chest.y * decoded.height) / worldHeight));
  for (let y = Math.max(0, centerY - radius); y <= Math.min(decoded.height - 1, centerY + radius); y += 1) {
    for (let x = Math.max(0, centerX - radius); x <= Math.min(decoded.width - 1, centerX + radius); x += 1) {
      const pixel = pixelAt(decoded, x, y);
      if (pixel[0] === expectedRgb[0] && pixel[1] === expectedRgb[1] && pixel[2] === expectedRgb[2]) return;
    }
  }

  const sampledPixels = [];
  for (let y = Math.max(0, centerY - radius); y <= Math.min(decoded.height - 1, centerY + radius); y += 1) {
    for (let x = Math.max(0, centerX - radius); x <= Math.min(decoded.width - 1, centerX + radius); x += 1) {
      sampledPixels.push({ x, y, rgba: [...pixelAt(decoded, x, y)] });
    }
  }
  assert.fail(
    `expected RGB ${expectedRgb.join(",")} near chest (${chest.x},${chest.y}) at preview pixel ` +
      `(${centerX},${centerY}); sampled=${JSON.stringify(sampledPixels)}`,
  );
}

function assertRequestedColorNearPreviewPoint(decoded, centerX, centerY, expectedRgb, radius = 1) {
  for (let y = Math.max(0, centerY - radius); y <= Math.min(decoded.height - 1, centerY + radius); y += 1) {
    for (let x = Math.max(0, centerX - radius); x <= Math.min(decoded.width - 1, centerX + radius); x += 1) {
      const pixel = pixelAt(decoded, x, y);
      if (pixel[0] === expectedRgb[0] && pixel[1] === expectedRgb[1] && pixel[2] === expectedRgb[2]) return;
    }
  }
  assert.fail(`expected RGB ${expectedRgb.join(",")} near preview pixel (${centerX},${centerY})`);
}

test("marker preview returns a PNG thumbnail and paints the requested chest color near the matched chest", async () => {
  const M = await loadModule();
  let opened;
  try {
    opened = openWorld(M);
    const header = readSection(M, opened.handle, "header");
    const chests = readSection(M, opened.handle, "chests");
    const chest = firstStoredChest(chests);
    const markerItem = firstStoredItem([chest]);

    const result = executeOperation(M, opened.handle, "mark_tiles_and_chests_preview", {
      chest_markers: [{ item_id: markerItem, color: "#11CC44FF" }],
      max_w: 512,
      max_h: 256,
    });
    if (result.status !== 0) {
      const error = readLastErrorJson(M);
      assert.equal(result.status, 0, operationFailureMessage("mark_tiles_and_chests_preview", result.status, error));
    }

    const response = JSON.parse(result.value);
    assert.equal(response.status, "ok");
    assert.ok(response.thumbnail_png_bytes > PNG_SIGNATURE.length);
    assert.ok(response.matched_chest_count > 0);
    assert.ok(response.width > 0);
    assert.ok(response.height > 0);

    const thumbnail = getThumbnailPng(M, opened.handle);
    assert.equal(thumbnail.reportedSize, response.thumbnail_png_bytes);
    assert.ok(thumbnail.width > 0);
    assert.ok(thumbnail.height > 0);
    assert.equal(thumbnail.width, response.width);
    assert.equal(thumbnail.height, response.height);
    assert.deepEqual(thumbnail.png.subarray(0, PNG_SIGNATURE.length), PNG_SIGNATURE);

    const decoded = decodePngRgba(thumbnail.png);
    assert.equal(decoded.width, thumbnail.width);
    assert.equal(decoded.height, thumbnail.height);
    assertRequestedColorNearChest(decoded, header.maxTilesX, header.maxTilesY, chest, [0x11, 0xcc, 0x44]);
  } finally {
    closeWorld(M, opened);
  }
});

test("native-size marker preview scans the tile stream once and keeps the original dimensions", async () => {
  const M = await loadModule();
  let opened;
  try {
    opened = openWorld(M);
    const header = readSection(M, opened.handle, "header");
    const chests = readSection(M, opened.handle, "chests");
    const chest = firstStoredChest(chests);
    const markerItem = firstStoredItem([chest]);

    const result = executeOperation(M, opened.handle, "mark_tiles_and_chests_preview", {
      chest_markers: [{ item_id: markerItem, color: "#11CC44FF" }],
      max_w: 0,
      max_h: 0,
    });
    let nativeSizeError = {};
    try {
      nativeSizeError = readLastErrorJson(M);
    } catch (error) {
      nativeSizeError = { message: error.message };
    }
    assert.equal(result.status, 0, operationFailureMessage(
      "native-size mark_tiles_and_chests_preview",
      result.status,
      nativeSizeError,
    ));

    const response = JSON.parse(result.value);
    assert.equal(response.width, header.maxTilesX);
    assert.equal(response.height, header.maxTilesY);
    assert.ok(response.matched_chest_count > 0);

    const thumbnail = getThumbnailPng(M, opened.handle);
    assert.equal(thumbnail.width, header.maxTilesX);
    assert.equal(thumbnail.height, header.maxTilesY);
    const linearMemoryBytes = M.wasmMemory.buffer.byteLength;
    const nativePeakBytes = M._tx_native_heap_peak() >>> 0;
    // Native allocations are backed by Emscripten's linear memory; adding both
    // counters would double-count the same WASM surface. The JS PNG copy is
    // the extra process-side allocation that remains live across the bridge.
    assert.ok(nativePeakBytes <= linearMemoryBytes, "native allocations must fit the linear heap");
    const conservativePeakBytes = linearMemoryBytes + thumbnail.png.length;
    assert.ok(
      conservativePeakBytes <= 200_000_000,
      `native-size preview peak estimate exceeded 200 MB: linear=${linearMemoryBytes}, ` +
        `nativePeak=${nativePeakBytes}, png=${thumbnail.png.length}`,
    );
    const decoded = decodePngRgb(thumbnail.png);
    assertRequestedColorNearChest(
      decoded,
      header.maxTilesX,
      header.maxTilesY,
      chest,
      [0x11, 0xcc, 0x44],
      35,
    );
  } finally {
    closeWorld(M, opened);
  }
});

test("marker preview composites the matched item's RGBA thumbnail inside its configured radius", async () => {
  const M = await loadModule();
  let opened;
  try {
    opened = openWorld(M);
    const header = readSection(M, opened.handle, "header");
    const chests = readSection(M, opened.handle, "chests");
    const chest = firstStoredChest(chests);
    const markerItem = firstStoredItem([chest]);
    installSolidMarkerIcon(M, opened.handle, markerItem);

    const result = executeOperation(M, opened.handle, "mark_tiles_and_chests_preview", {
      chest_markers: [{ item_id: markerItem, color: "#FF2020FF", radius: 60, line_width: 1 }],
      max_w: 512,
      max_h: 256,
    });
    if (result.status !== 0) {
      assert.equal(result.status, 0, operationFailureMessage("marker icon preview", result.status, readLastErrorJson(M)));
    }
    const response = JSON.parse(result.value);
    assert.ok(response.matched_chest_count > 0);
    const thumbnail = getThumbnailPng(M, opened.handle);
    const decoded = decodePngRgba(thumbnail.png);
    assertRequestedColorNearChest(decoded, header.maxTilesX, header.maxTilesY, chest, [12, 34, 56], 5);
  } finally {
    closeWorld(M, opened);
  }
});

test("marked map output changes when the matched item thumbnail is installed", async () => {
  const M = await loadModule();
  let opened;
  try {
    opened = openWorld(M);
    const chests = readSection(M, opened.handle, "chests");
    const markerItem = firstStoredItem(chests);
    const request = {
      chest_markers: [{ item_id: markerItem, color: "#FF2020FF", radius: 60, line_width: 1 }],
    };

    let result = executeOperation(M, opened.handle, "mark_tiles_and_chests_map", request);
    assert.equal(result.status, 0);
    const withoutIcon = getMapBytes(M, opened.handle).map;

    installSolidMarkerIcon(M, opened.handle, markerItem, [0, 255, 0, 255]);
    result = executeOperation(M, opened.handle, "mark_tiles_and_chests_map", request);
    assert.equal(result.status, 0);
    const withIcon = getMapBytes(M, opened.handle).map;
    assert.notDeepEqual(withIcon, withoutIcon);
  } finally {
    closeWorld(M, opened);
  }
});

test("marked MAP preserves configured marker RGB and item icon RGB", async () => {
  const M = await loadModule();
  let opened;
  try {
    opened = openWorld(M);
    const chests = readSection(M, opened.handle, "chests");
    const markerItem = firstStoredItem(chests);
    installMarkerColorIndex(M, opened.handle);

    const request = (color) => ({
      chest_markers: [{ item_id: markerItem, color, radius: 60, line_width: 1 }],
    });
    let result = executeOperation(M, opened.handle, "mark_tiles_and_chests_map", request("#FF2020FF"));
    assert.equal(result.status, 0);
    const redMarker = getMapBytes(M, opened.handle).map;

    result = executeOperation(M, opened.handle, "mark_tiles_and_chests_map", request("#20A020FF"));
    assert.equal(result.status, 0);
    const greenMarker = getMapBytes(M, opened.handle).map;
    assert.notDeepEqual(greenMarker, redMarker, "marker RGB must affect MAP tile values");

    installSolidMarkerIcon(M, opened.handle, markerItem, [255, 0, 0, 255]);
    result = executeOperation(M, opened.handle, "mark_tiles_and_chests_map", request("#FF2020FF"));
    assert.equal(result.status, 0);
    const redIcon = getMapBytes(M, opened.handle).map;

    installSolidMarkerIcon(M, opened.handle, markerItem, [0, 0, 255, 255]);
    result = executeOperation(M, opened.handle, "mark_tiles_and_chests_map", request("#FF2020FF"));
    assert.equal(result.status, 0);
    const blueIcon = getMapBytes(M, opened.handle).map;
    assert.notDeepEqual(blueIcon, redIcon, "icon RGB must affect MAP tile values");
  } finally {
    closeWorld(M, opened);
  }
});

test("marked MAP scans TXCI candidates beyond the legacy 32-entry limit", async () => {
  const M = await loadModule();
  let opened;
  try {
    opened = openWorld(M);
    const chests = readSection(M, opened.handle, "chests");
    const markerItem = firstStoredItem(chests);
    const request = {
      chest_markers: [{ item_id: markerItem, color: "#112233FF", radius: 1, line_width: 1 }],
    };

    assert.equal(
      installMarkerColorIndexBytes(M, opened.handle, makeLongMarkerColorIndex()),
      0,
      "synthetic long TXCI group must load successfully",
    );
    let result = executeOperation(M, opened.handle, "mark_tiles_and_chests_map", request);
    assert.equal(result.status, 0);
    const withLongGroup = getMapBytes(M, opened.handle).map;

    assert.equal(M._txw_set_marker_color_index(opened.handle, 0, 0), 0);
    result = executeOperation(M, opened.handle, "mark_tiles_and_chests_map", request);
    assert.equal(result.status, 0);
    const withoutIndex = getMapBytes(M, opened.handle).map;
    assert.notDeepEqual(
      withLongGroup,
      withoutIndex,
      "MAP generation must use a valid candidate after the first 32 entries",
    );
  } finally {
    closeWorld(M, opened);
  }
});

test("failed marker color index replacement preserves the previous MAP lookup", async () => {
  const M = await loadModule();
  let opened;
  try {
    opened = openWorld(M);
    const chests = readSection(M, opened.handle, "chests");
    const markerItem = firstStoredItem(chests);
    const request = {
      chest_markers: [{ item_id: markerItem, color: "#FF2020FF", radius: 60, line_width: 1 }],
    };

    installMarkerColorIndex(M, opened.handle);
    let result = executeOperation(M, opened.handle, "mark_tiles_and_chests_map", request);
    assert.equal(result.status, 0);
    const beforeFailedReplacement = getMapBytes(M, opened.handle).map;

    const malformed = Buffer.alloc(TXCI_GZ.length, 0);
    assert.equal(
      installMarkerColorIndexBytes(M, opened.handle, malformed),
      -1,
      "malformed TXCI replacement must fail",
    );
    assert.equal(readLastErrorJson(M).code, "TERRAX_PARSE_ERROR");

    result = executeOperation(M, opened.handle, "mark_tiles_and_chests_map", request);
    assert.equal(result.status, 0);
    const afterFailedReplacement = getMapBytes(M, opened.handle).map;
    assert.deepEqual(
      afterFailedReplacement,
      beforeFailedReplacement,
      "a failed replacement must keep the last known-good marker color lookup",
    );
  } finally {
    closeWorld(M, opened);
  }
});

test("marker icon atlas is released when its world closes", async () => {
  const M = await loadModule();
  const baseline = M._tx_native_heap_used() >>> 0;
  let opened;
  try {
    opened = openWorld(M);
    const chests = readSection(M, opened.handle, "chests");
    installSolidMarkerIcon(M, opened.handle, firstStoredItem(chests));
    assert.ok((M._tx_native_heap_used() >>> 0) > baseline);
  } finally {
    closeWorld(M, opened);
  }
  assert.equal(M._tx_native_heap_used() >>> 0, baseline);
});

test("marked map reports scanned matches, dimensions, and a v33083 single-use payload", async () => {
  const M = await loadModule();
  let opened;
  let sizePtr = 0;
  let widthPtr = 0;
  let heightPtr = 0;
  let shortPtr = 0;
  let outputPtr = 0;
  try {
    opened = openWorld(M);
    const header = readSection(M, opened.handle, "header");
    const chests = readSection(M, opened.handle, "chests");
    const itemType = firstStoredItem(chests);
    const expectedChests = countChestsWithItem(chests, itemType);
    assert.ok(expectedChests > 0);

    const duplicateRuleCount = expectedChests + 5;
    let result = executeOperation(M, opened.handle, "mark_tiles_and_chests_map", {
      chest_markers: Array.from({ length: duplicateRuleCount }, () => ({
        item_id: itemType,
        color: "#FF2020C8",
      })),
      tile_markers: [{ tile_type: -1, color: "#20A0FFFF" }],
    });
    assert.equal(result.status, 0);
    let response = JSON.parse(result.value);
    assert.equal(response.matched_chest_count, expectedChests);
    assert.equal(response.width, header.maxTilesX);
    assert.equal(response.height, header.maxTilesY);
    assert.equal(response.matched_tile_count, 0);
    assert.ok(response.map_bytes > 0);

    result = executeOperation(M, opened.handle, "mark_tiles_and_chests_map", {
      chest_markers: Array.from({ length: duplicateRuleCount }, () => ({
        item_id: itemType,
        color: "#FF2020C8",
      })),
      tile_markers: [{ tile_type: -1, color: "#20A0FFFF" }],
    });
    assert.equal(result.status, 0);
    response = JSON.parse(result.value);
    assert.equal(response.matched_chest_count, expectedChests);
    assert.equal(response.width, header.maxTilesX);
    assert.equal(response.height, header.maxTilesY);
    assert.equal(response.matched_tile_count, 0);
    assert.ok(response.map_bytes > 0);

    assert.equal(typeof M._terra_op_get_map, "function", "map output getter must be exported");
    sizePtr = mustAlloc(M, 8, "map size");
    widthPtr = mustAlloc(M, 4, "map width");
    heightPtr = mustAlloc(M, 4, "map height");

    let status = M._terra_op_get_map(
      opened.handle, 0, 0n, sizePtr, widthPtr, heightPtr,
    );
    assert.equal(status, 2);
    const required = readRequiredSize(M, sizePtr);
    assert.equal(required, response.map_bytes);
    assert.ok(readU32(M, widthPtr) > 0);
    assert.ok(readU32(M, heightPtr) > 0);

    shortPtr = mustAlloc(M, required - 1, "short map output");
    status = M._terra_op_get_map(
      opened.handle, shortPtr, BigInt(required - 1), sizePtr, widthPtr, heightPtr,
    );
    assert.equal(status, 2, "short buffer must not consume map output");

    outputPtr = mustAlloc(M, required, "map output");
    status = M._terra_op_get_map(
      opened.handle, outputPtr, BigInt(required), sizePtr, widthPtr, heightPtr,
    );
    assert.equal(status, 0);
    const mapBytes = Buffer.from(M.HEAPU8.slice(outputPtr, outputPtr + required));
    assert.ok(mapBytes.some((value) => value !== 0));
    assert.equal(mapBytes.readUInt16LE(0), 33083, "marked map must keep the v33083 map header");
    status = M._terra_op_get_map(
      opened.handle, 0, 0n, sizePtr, widthPtr, heightPtr,
    );
    assert.equal(status, 8, "successful copy must consume world-owned map output");
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    if (shortPtr) M._tx_free(shortPtr);
    if (heightPtr) M._tx_free(heightPtr);
    if (widthPtr) M._tx_free(widthPtr);
    if (sizePtr) M._tx_free(sizePtr);
    closeWorld(M, opened);
  }
});

test("tile marker maps report actual matched tile counts instead of the marker rule count", async () => {
  const M = await loadModule();
  let opened;
  try {
    opened = openWorld(M);
    const result = executeOperation(M, opened.handle, "mark_tiles_and_chests_map", {
      tile_markers: [{ tile_type: 0, color: "#20A0FFFF" }],
    });
    assert.equal(result.status, 0);
    const response = JSON.parse(result.value);
    assert.ok(
      response.matched_tile_count > 1,
      "fixture dirt tiles must report matched tile runs, not the one marker rule",
    );
  } finally {
    closeWorld(M, opened);
  }
});

test("mark_tiles_and_chests_map reads updated tile overrides from batch_update_tiles", async () => {
  const M = await loadModule();
  let opened;
  try {
    opened = openWorld(M);
    const update = executeOperation(M, opened.handle, "batch_update_tiles", {
      rules: [{ where: { type: 0 }, patch: { is_active: false } }],
    });
    assert.equal(update.status, 0);

    const result = executeOperation(M, opened.handle, "mark_tiles_and_chests_map", {
      tile_markers: [{ tile_type: 0, color: "#20A0FFFF" }],
    });
    assert.equal(result.status, 0);
    const response = JSON.parse(result.value);
    assert.equal(response.status, "ok");
    assert.equal(
      response.matched_tile_count,
      0,
      "tile marker matching must read the updated tile section override",
    );
  } finally {
    closeWorld(M, opened);
  }
});

test("tile marker preview covers every preview row touched by a long matched run, not just the run midpoint", async () => {
  const M = await loadModule();
  let opened;
  try {
    opened = openWorld(M);
    const header = readSection(M, opened.handle, "header");
    const format = readSection(M, opened.handle, "format");
    const preview = computePreviewSize(header.maxTilesX, header.maxTilesY, 2048, 0);
    const candidateRuns = listTileRuns(format, header, 0, preview.width, preview.height, 4);
    assert.ok(candidateRuns.length > 0, "fixture must contain at least one long tile_type=0 run");

    const result = executeOperation(M, opened.handle, "mark_tiles_and_chests_preview", {
      max_w: 2048,
      tile_markers: [{ tile_type: 0, color: "#20A0FFFF", radius: 1, line_width: 1 }],
    });
    assert.equal(result.status, 0);
    const response = JSON.parse(result.value);
    assert.equal(response.status, "ok");
    assert.ok(response.matched_tile_count > 1);

    const thumbnail = getThumbnailPng(M, opened.handle);
    const decoded = decodePngRgba(thumbnail.png);
    let uncovered = null;
    for (const run of candidateRuns) {
      const missingRows = [];
      for (let previewY = run.py0; previewY < run.py1; previewY += 1) {
        try {
          assertRequestedColorNearPreviewPoint(decoded, run.px, previewY, [0x20, 0xa0, 0xff], 1);
        } catch {
          missingRows.push(previewY);
        }
      }
      if (missingRows.length > 0) {
        uncovered = { run, missingRows };
        break;
      }
    }
    if (uncovered) {
      assert.fail(
        `tile preview missed preview rows for run x=${uncovered.run.x}, y=${uncovered.run.y}, ` +
        `run=${uncovered.run.run}, px=${uncovered.run.px}, py0=${uncovered.run.py0}, py1=${uncovered.run.py1}, ` +
        `missing=${uncovered.missingRows.join(",")}`,
      );
    }
  } finally {
    closeWorld(M, opened);
  }
});

test("marker maps still return a valid v33083 payload when no chest or tile marker matches", async () => {
  const M = await loadModule();
  let opened;
  try {
    opened = openWorld(M);
    const header = readSection(M, opened.handle, "header");
    const result = executeOperation(M, opened.handle, "mark_tiles_and_chests_map", {
      chest_markers: [{ item_id: 2147483647, color: "#AA5500FF" }],
      tile_markers: [{ tile_type: -1, color: "#0055AAFF" }],
    });
    assert.equal(result.status, 0);
    const response = JSON.parse(result.value);
    assert.equal(response.status, "ok");
    assert.equal(response.matched_chest_count, 0);
    assert.equal(response.matched_tile_count, 0);
    assert.equal(response.width, header.maxTilesX);
    assert.equal(response.height, header.maxTilesY);
    assert.ok(response.map_bytes > 0);

    const mapOutput = getMapBytes(M, opened.handle);
    assert.equal(mapOutput.reportedSize, response.map_bytes);
    assert.equal(mapOutput.width, header.maxTilesX);
    assert.equal(mapOutput.height, header.maxTilesY);
    assert.equal(mapOutput.map.readUInt16LE(0), 33083);
  } finally {
    closeWorld(M, opened);
  }
});

test("next operation and close reclaim unconsumed map output", async () => {
  const M = await loadModule();
  M._tx_reset_heap();
  const baseline = M._tx_native_heap_used() >>> 0;
  let opened;
  const sizePtr = mustAlloc(M, 8, "map size");
  const widthPtr = mustAlloc(M, 4, "map width");
  const heightPtr = mustAlloc(M, 4, "map height");
  try {
    opened = openWorld(M);
    let result = executeOperation(M, opened.handle, "mark_tiles_and_chests_map", {
      tile_markers: [{ tile_type: -1, color: "#20A0FFFF" }],
    });
    assert.equal(result.status, 0);
    assert.equal(M._terra_op_get_map(opened.handle, 0, 0n, sizePtr, widthPtr, heightPtr), 2);
    const withMap = M._tx_native_heap_used() >>> 0;

    result = executeOperation(M, opened.handle, "render_thumbnail_png", { max_w: 32 });
    assert.equal(result.status, 0);
    assert.equal(M._terra_op_get_map(opened.handle, 0, 0n, sizePtr, widthPtr, heightPtr), 8);
    assert.ok((M._tx_native_heap_used() >>> 0) < withMap, "next operation must reclaim map bytes");

    result = executeOperation(M, opened.handle, "mark_tiles_and_chests_map", {
      tile_markers: [{ tile_type: -1, color: "#20A0FFFF" }],
    });
    assert.equal(result.status, 0);
    const staleHandle = opened.handle;
    assert.equal(M._terra_world_close(staleHandle), 0);
    opened.handle = 0;
    assert.equal(M._terra_op_get_map(staleHandle, 0, 0n, sizePtr, widthPtr, heightPtr), 8);
  } finally {
    closeWorld(M, opened);
    M._tx_free(heightPtr);
    M._tx_free(widthPtr);
    M._tx_free(sizePtr);
  }
  assert.equal(M._tx_native_heap_used() >>> 0, baseline);
});

// Independent RLE fixture: two diagonal/bridged veins, adjacent different ore,
// all frames of multi-tile objects, and both sword styles.
function makeEntityWorld(customCells) {
  const { makeSectionedWorld } = require('./helpers/sectioned-world');
  const base = makeSectionedWorld(128, { worldName: 'entity-fixture' });
  const header = Buffer.from(base.subarray(base.readUInt32LE(6)));
  const afterName = 1 + Buffer.byteLength('entity-fixture');
  const width = 128, height = 300;
  header.writeInt32LE(height, afterName + 20);
  header.writeInt32LE(width, afterName + 24);
  const cells = new Map();
  const put = (x,y,type,fx=0,fy=0) => cells.set(`${x},${y}`, {type,fx,fy});
  // The lower branch joins a prior component late; union must retire only once.
  for (const [x,y] of [[10,20],[11,21],[10,24],[11,23],[12,22],[30,30],[31,31]]) put(x,y,8);
  put(12,21,7);
  const object = (x,y,type,w,h,fx=0,fy=0) => {
    for(let dx=0;dx<w;dx++) for(let dy=0;dy<h;dy++) put(x+dx,y+dy,type,fx+18*dx,fy+18*dy);
  };
  object(50,40,12,2,2); object(60,60,236,2,2,72);
  object(70,80,187,3,2,918); object(80,80,187,3,2,864);
  object(90,100,186,3,2,810);
  object(30,120,31,2,2); object(40,120,31,2,2,36);
  object(50,140,26,3,2); object(60,140,26,3,2,54);
  if (customCells) { cells.clear(); for (const [key,value] of customCells) cells.set(key,value); }
  const important = Buffer.alloc(88);
  for(const type of [12,236,187,186,31,26]) important[type>>3] |= 1 << (type&7);
  const data=[];
  for(let x=0;x<width;x++) for(let y=0;y<height;) {
    const tile=cells.get(`${x},${y}`);
    let run=1;
    while(y+run<height && JSON.stringify(cells.get(`${x},${y+run}`))===JSON.stringify(tile)) run++;
    const rle=run>256?128:run>1?64:0;
    data.push(rle | (tile ? 2 | (tile.type>255?32:0) : 0));
    if(tile) {
      data.push(tile.type&255); if(tile.type>255) data.push(tile.type>>8);
      if(important[tile.type>>3] & (1<<(tile.type&7))) data.push(tile.fx&255,tile.fx>>8,tile.fy&255,tile.fy>>8);
    }
    if(rle) data.push((run-1)&255); if(rle===128) data.push((run-1)>>8);
    y+=run;
  }
  const format=Buffer.alloc(4+2+7*4+2+important.length);
  format.writeUInt32LE(128); format.writeUInt16LE(7,4);
  const tileStart=format.length+header.length, tileEnd=tileStart+data.length;
  for(let i=0;i<7;i++) format.writeUInt32LE(i===0?format.length:i===1?tileStart:tileEnd,6+i*4);
  format.writeUInt16LE(700,34); important.copy(format,36);
  return Buffer.concat([format,header,Buffer.from(data)]);
}

test('entity points share PNG/MAP anchors, deduplicate frames, and merge diagonal veins', async () => {
  const M=await loadModule(); M._tx_reset_heap();
  const opened=openWorld(M,makeEntityWorld());
  const selectors=[
    {tile_type:8,locate:2}, {tile_type:7,locate:2},
    {tile_type:12,locate:1,frame_x:0,frame_y:0},
    {tile_type:236,locate:1,frame_x:0,frame_y:0,frame_x_mod:36,frame_y_mod:36},
    {tile_type:187,locate:1,frame_x:918,frame_y:0},
    {tile_type:186,locate:1,frame_x:810,frame_y:0},
    {tile_type:31,locate:1,frame_x:0,frame_y:0},
    {tile_type:31,locate:1,frame_x:36,frame_y:0},
    {tile_type:26,locate:1,frame_x:0,frame_y:0},
    {tile_type:26,locate:1,frame_x:54,frame_y:0},
  ].map(row=>({...row,radius:4,line_width:1,color:'#FF00FFFF'}));
  try {
    for(const max_w of [0,64,128]) {
      const result=executeOperation(M,opened.handle,'mark_tiles_and_chests_preview',{tile_markers:selectors,max_w});
      assert.equal(result.status,0,result.status ? JSON.stringify(readLastErrorJson(M)) : undefined);
      assert.equal(JSON.parse(result.value).matched_tile_count,11);
      const png=getThumbnailPng(M,opened.handle).png;
      if(max_w===0) {
        const image=decodePngRgb(png);
        assert.deepEqual(Array.from(pixelAt(image,14,20).slice(0,3)),[255,0,255], 'vein uses a configured radius ring');
        assert.deepEqual(Array.from(pixelAt(image,54,40).slice(0,3)),[255,0,255], 'object uses one top-left frame');
      }
    }
    installMarkerColorIndex(M,opened.handle);
    let result=executeOperation(M,opened.handle,'mark_tiles_and_chests_map',{tile_markers:selectors});
    assert.equal(result.status,0,result.status ? JSON.stringify(readLastErrorJson(M)) : undefined);
    assert.equal(JSON.parse(result.value).matched_tile_count,11);
    const first=getMapBytes(M,opened.handle).map;
    result=executeOperation(M,opened.handle,'mark_tiles_and_chests_map',{tile_markers:selectors.map(row=>({...row,radius:9,color:'#00FFFFFF'}))});
    assert.equal(result.status,0);
    assert.notDeepEqual(getMapBytes(M,opened.handle).map,first,'MAP responds to entity radius/color');
    result=executeOperation(M,opened.handle,'mark_tiles_and_chests_preview',{tile_markers:[{tile_type:8,locate:2},{tile_type:8,locate:2}]});
    assert.notEqual(result.status,0,'duplicate cluster selectors cannot overflow frontier arrays');
  } finally { closeWorld(M,opened); }
});


test('streaming veins match an independent eight-neighbour flood fill and release scratch memory', async () => {
  const M=await loadModule(); M._tx_reset_heap();
  let seed=12345;
  const random=()=>{ seed=(Math.imul(seed,1664525)+1013904223)>>>0; return seed/4294967296; };
  for(let trial=0;trial<20;trial++) {
    const cells=new Map();
    for(let x=0;x<24;x++) for(let y=0;y<40;y++) {
      if(random()<trial/24) cells.set(`${x},${y}`,{type:random()<.7?8:7,fx:0,fy:0});
    }
    const visited=new Set(); let expected=0;
    for(const [key,tile] of cells) {
      if(visited.has(key)) continue;
      expected++; const pending=[key]; visited.add(key);
      while(pending.length) {
        const [x,y]=pending.pop().split(',').map(Number);
        for(let dx=-1;dx<=1;dx++) for(let dy=-1;dy<=1;dy++) {
          const neighbour=`${x+dx},${y+dy}`;
          if(!visited.has(neighbour) && cells.get(neighbour)?.type===tile.type) {visited.add(neighbour);pending.push(neighbour);}
        }
      }
    }
    const baseline=M._tx_native_heap_used()>>>0;
    const opened=openWorld(M,makeEntityWorld(cells));
    try {
      const result=executeOperation(M,opened.handle,'mark_tiles_and_chests_preview',{max_w:64,tile_markers:[{tile_type:8,locate:2},{tile_type:7,locate:2}]});
      assert.equal(result.status,0);
      assert.equal(JSON.parse(result.value).matched_tile_count,expected,`trial ${trial}`);
      getThumbnailPng(M,opened.handle);
    } finally {closeWorld(M,opened);}
    assert.equal(M._tx_native_heap_used()>>>0,baseline, 'all location buffers released');
  }
});
