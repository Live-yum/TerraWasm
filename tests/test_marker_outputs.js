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
function makeEntityWorld(customCells, options = {}) {
  const { makeSectionedWorld } = require('./helpers/sectioned-world');
  const base = makeSectionedWorld(128, { worldName: 'entity-fixture' });
  const latest = options.latest;
  const pointerOffset = latest ? 26 : 6;
  const version = latest ? TEST_BYTES.readUInt32LE(0) : 128;
  const header = latest ? Buffer.from(TEST_BYTES.subarray(TEST_BYTES.readUInt32LE(26), TEST_BYTES.readUInt32LE(30)))
    : Buffer.from(base.subarray(base.readUInt32LE(6)));
  let afterName = 1 + Buffer.byteLength('entity-fixture');
  if (latest) {
    let offset = 0;
    const skipString = () => { let length = 0, shift = 0, byte; do { byte = header[offset++]; length |= (byte & 127) << shift; shift += 7; } while(byte & 128); offset += length; };
    skipString(); skipString(); afterName = offset + 8 + 16;
  }
  const width = options.width || 128, height = options.height || 300;
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
  for(const type of [4,10,11,12,14,15,18,19,21,26,31,33,34,42,79,87,88,89,90,93,100,101,104,172,186,187,235,236,441,467,468,469,497]) important[type>>3] |= 1 << (type&7);
  const data=[];
  for(let x=0;x<width;x++) for(let y=0;y<height;) {
    const tile=cells.get(`${x},${y}`);
    let run=1;
    while(y+run<height && JSON.stringify(cells.get(`${x},${y+run}`))===JSON.stringify(tile)) run++;
    const rle=run>256?128:run>1?64:0;
    const active = tile?.type !== undefined;
    const f4 = (tile?.invisible_block ? 2 : 0) | (tile?.invisible_wall ? 4 : 0);
    const f3 = (f4 ? 1 : 0) | (tile?.color ? 8 : 0) | (tile?.wallColor ? 16 : 0);
    data.push(rle | (active ? 2 | (tile.type>255?32:0) : 0) | (tile?.wall ? 4 : 0) | (f3 ? 1 : 0));
    if(f3) data.push(1, f3);
    if(f4) data.push(f4);
    if(active) {
      data.push(tile.type&255); if(tile.type>255) data.push(tile.type>>8);
      if(important[tile.type>>3] & (1<<(tile.type&7))) data.push(tile.fx&255,tile.fx>>8,tile.fy&255,tile.fy>>8);
      if(tile.color) data.push(tile.color);
    }
    if(tile?.wall) { data.push(tile.wall); if(tile.wallColor) data.push(tile.wallColor); }
    if(rle) data.push((run-1)&255); if(rle===128) data.push((run-1)>>8);
    y+=run;
  }
  const sections = latest ? TEST_BYTES.readUInt16LE(24) : 7;
  const format=Buffer.alloc(pointerOffset+sections*4+2+important.length);
  format.writeUInt32LE(version);
  if(latest) TEST_BYTES.copy(format,4,4,24);
  format.writeUInt16LE(sections,pointerOffset-2);
  const tileStart=format.length+header.length, tileEnd=tileStart+data.length;
  for(let i=0;i<sections;i++) format.writeUInt32LE(i===0?format.length:i===1?tileStart:tileEnd,pointerOffset+i*4);
  format.writeUInt16LE(700,pointerOffset+sections*4); important.copy(format,pointerOffset+sections*4+2);
  return Buffer.concat([format,header,Buffer.from(data)]);
}

// Decode fixture tiles independently of the WASM decoder, ignoring RLE grouping.
test('1458 shadow swaps and negative wall half-invert match actual preview pixels', async () => {
  const M = await loadModule(), cells = new Map();
  for (let id = 1; id <= 100; id++) {
    for (const [row, paint] of [[0, 0], [1, 29], [2, 30]]) {
      cells.set(`${id},${row}`, { type: id, color: paint });
      cells.set(`${id},${row + 3}`, { wall: id, wallColor: paint });
    }
  }
  const opened = openWorld(M, makeEntityWorld(cells));
  try {
    const result = executeOperation(M, opened.handle, 'render_preview_png', { max_w: 0 });
    assert.equal(result.status, 0);
    const image = decodePngRgb(getThumbnailPng(M, opened.handle).png);
    const pixel = (x, y) => [...image.rgba.subarray((y * image.width + x) * 4, (y * image.width + x) * 4 + 3)];
    const badTiles = new Set([127, 135, 210, 428, 504, 541]);
    const badWalls = new Set([0, 21, 88, 89, 90, 91, 92, 93, 106, 107, 145, 150, 152, 168, 241, 318]);
    let shadowDifferences = 0, negativeDifferences = 0;
    for (let id = 1; id <= 100; id++) for (const wall of [false, true]) {
      if ((wall ? badWalls : badTiles).has(id)) continue;
      const row = wall ? 3 : 0, base = pixel(id, row);
      // Base includes the existing preview wall darkening. MapColor uses float32.
      const shadow = Math.min(base[2], Math.max(base[0], base[1]));
      const factor = Math.fround(Math.fround(shadow / 255) * Math.fround(0.3));
      const expectedShadow = Math.trunc(Math.fround(25 * factor));
      assert.deepEqual(pixel(id, row + 1), [expectedShadow, expectedShadow, expectedShadow], `shadow ${wall ? 'wall' : 'tile'} ${id}`);
      assert.deepEqual(pixel(id, row + 2), base.map(v => wall ? Math.floor((255 - v) / 2) : 255 - v), `negative ${wall ? 'wall' : 'tile'} ${id}`);
      if (expectedShadow !== Math.floor(25 * base[2] * 77 / 65025)) shadowDifferences++;
      if (wall && base.some(v => Math.floor((255 - v) / 2) !== Math.floor((255 - v) * 128 / 255))) negativeDifferences++;
    }
    assert.ok(shadowDifferences > 0, 'fixture must distinguish the old shadow formula');
    assert.ok(negativeDifferences > 0, 'fixture must distinguish the old negative wall formula');
  } finally { closeWorld(M, opened); }
});

function fixtureCells(bytes, width, height) {
  const base = bytes.readUInt32LE(0) >= 135 ? 26 : 6;
  const sections = bytes.readUInt16LE(base - 2);
  const important = bytes.subarray(base + sections * 4 + 2);
  let offset = bytes.readUInt32LE(base + 4);
  const end = bytes.readUInt32LE(base + 8), cells = new Map();
  for (let x = 0; x < width; x++) for (let y = 0; y < height;) {
    const f1 = bytes[offset++], f2 = f1 & 1 ? bytes[offset++] : 0;
    const f3 = f2 & 1 ? bytes[offset++] : 0, f4 = f3 & 1 ? bytes[offset++] : 0;
    const tile = {};
    if (f1 & 2) {
      tile.type = bytes[offset++]; if (f1 & 32) tile.type |= bytes[offset++] << 8;
      if (important[tile.type >> 3] & (1 << (tile.type & 7))) {
        tile.fx = bytes.readInt16LE(offset); tile.fy = bytes.readInt16LE(offset + 2); offset += 4;
      }
      if (f3 & 8) tile.color = bytes[offset++];
    }
    if (f1 & 4) { tile.wall = bytes[offset++]; if (f3 & 16) tile.wallColor = bytes[offset++]; }
    if (f4 & 2) tile.invisible_block = 1;
    if (f4 & 4) tile.invisible_wall = 1;
    let run = 1;
    if (f1 >> 6 === 1) run += bytes[offset++];
    else if (f1 >> 6) { run += bytes.readUInt16LE(offset); offset += 2; }
    assert.ok(y + run <= height);
    if (Object.keys(tile).length) for (let dy = 0; dy < run; dy++) cells.set(`${x},${y+dy}`, tile);
    y += run;
  }
  assert.equal(offset, end);
  return cells;
}

function commitFixture(M, opened) {
  const sizePtr = mustAlloc(M, 4, 'size'), handlePtr = mustAlloc(M, 4, 'handle');
  let ptr = 0;
  try {
    assert.equal(M._terra_world_commit_to_buffer(opened.handle,0,0,sizePtr,handlePtr),0);
    const size = readU32(M,sizePtr); ptr = mustAlloc(M,size,'bytes');
    assert.equal(M._terra_world_commit_to_buffer(opened.handle,ptr,size,sizePtr,handlePtr),0);
    opened.handle = readU32(M,handlePtr);
    return Buffer.from(M.HEAPU8.subarray(ptr,ptr+size));
  } finally { if(ptr) M._tx_free(ptr); M._tx_free(handlePtr); M._tx_free(sizePtr); }
}

test('environment transparency matches game neighborhoods and shares final tiles with prepared outputs', async () => {
  const M = await loadModule(); M._tx_reset_heap();
  const baseline = M._tx_heap_used(), width = 220, height = 500, cells = new Map();
  for(let x=5;x<20;x++) for(let y=85;y<105;y++) cells.set(`${x},${y}`,{type:41,wall:7});
  for(let x=130;x<144;x++) for(let y=230;y<240;y++) cells.set(`${x},${y}`,{type:60});
  // Long runs cross both neighborhood boundaries and the underworld cutoff.
  for(let y=0;y<height;y++) {
    cells.set(`10,${y}`,{type:1,wall:7,color:3,wallColor:9});
    cells.set(`180,${y}`,{wall:4});
  }
  for(const [x,y] of [[30,90],[180,230],[219,499],[30,30]])
    cells.set(`${x},${y}`,{type:21,fx:108,fy:18,color:14,wall:x===30?7:4,wallColor:7});
  cells.set('219,499',{type:26,fx:54,fy:18,wall:87});
  cells.set('218,499',{type:226});
  const source = makeEntityWorld(cells,{width,height});
  const canonical = fixtureCells(source,width,height);
  const jungle = [], dungeon = [];
  for(const [key,t] of canonical) {
    if(t.type===60||t.type===226) jungle.push(key.split(',').map(Number));
    if(t.type===41) dungeon.push(key.split(',').map(Number));
  }
  const near = (points,x,y) => points.filter(([a,b])=>a>=x-84&&a<x+85&&b>=y-62&&b<y+62).length;
  for(const biome_region of [1,2]) {
    const expected = new Map([...canonical].map(([key,t])=>{
      const [x,y]=key.split(',').map(Number), next={...t};
      const matches = biome_region===1 ? near(dungeon,x,y)>=250&&(t.wall===7||t.type===41)
        : (near(jungle,x,y)>=140&&y<=height-200)||t.wall===87||t.type===226;
      if(matches) { if(t.type!==undefined) next.invisible_block=1; if(t.wall) next.invisible_wall=1; }
      return [key,next];
    }));
    const outputs=[];
    for(const prepare of [false,true]) {
      const opened=openWorld(M,source);
      const run=(name,request={})=>{
        const result=executeOperation(M,opened.handle,name,request);
        assert.equal(result.status,0,result.status ? JSON.stringify(readLastErrorJson(M)) : undefined);
        return JSON.parse(result.value);
      };
      try {
        if(prepare) run('begin_output_preparation',{map:true});
        run('batch_update_tiles',{rules:[
          {where:{biome_region,is_active:1},patch:{invisible_block:1}},
          {where:{biome_region,has_wall:1},patch:{invisible_wall:1}},
        ]});
        if(prepare) run('finish_output_preparation');
        const calls=run('get_output_preparation_stats').tile_decode_calls;
        assert.deepEqual(fixtureCells(commitFixture(M,opened),width,height),expected);
        installMarkerColorIndex(M,opened.handle);
        run('render_lit_map'); const map=getMapBytes(M,opened.handle).map;
        run('render_preview_png',{max_w:0}); const full=getThumbnailPng(M,opened.handle).png;
        run('render_preview_png',{max_w:256}); const list=getThumbnailPng(M,opened.handle).png;
        if(prepare) assert.equal(run('get_output_preparation_stats').tile_decode_calls,calls,'no output rescans after regional replacement');
        outputs.push({map,full,list});
      } finally {closeWorld(M,opened);}
      assert.equal(M._tx_heap_used(),baseline,'regional mask must be released');
    }
    assert.deepEqual(outputs[1],outputs[0]);
  }
});

test('dungeon transparency includes surface entrances, safe walls and wall-less outer bricks', async () => {
  const M=await loadModule(); M._tx_reset_heap();
  const baseline=M._tx_heap_used(), width=360, height=500;
  for(const [brick,unsafe,safe,slab] of [[41,7,17,100],[43,8,18,104],[44,9,19,102]]) {
    const cells=new Map();
    for(const top of [20,180]) {
      for(let x=10;x<30;x++) for(let y=top;y<top+20;y++) cells.set(`${x},${y}`,{type:brick});
      cells.set(`9,${top-1}`,{type:brick}); // Exposed roof / outer wall has no background wall.
      cells.set(`35,${top}`,{type:21,fx:108,fy:18,wall:safe,color:14});
      cells.set(`36,${top}`,{type:15,fx:0,fy:18,wall:unsafe});
      cells.set(`37,${top}`,{type:1,wall:slab,wallColor:3});
      cells.set(`38,${top}`,{type:1,wall:4}); // Nearby ordinary terrain is not dungeon structure.
    }
    cells.set('300,20',{type:brick,wall:safe}); // Isolated material elsewhere is not a dungeon.
    const source=makeEntityWorld(cells,{width,height,latest:true});
    const expected=fixtureCells(source,width,height);
    for(const [key,tile] of expected) {
      if(key==='300,20'||key.startsWith('38,')) continue;
      tile.invisible_block=1;
      if(tile.wall) tile.invisible_wall=1;
    }
    for(const flags of [{},{drunkWorld:true},{dualDungeonsSeed:true},{worldSurface:30}]) {
      const opened=openWorld(M,source);
      const run=(name,request)=>{
        const result=executeOperation(M,opened.handle,name,request);
        assert.equal(result.status,0,result.status ? JSON.stringify(readLastErrorJson(M)) : undefined);
      };
      try {
        run('header_patch',{patch:{spawnTileX:20,spawnTileY:60,worldSurface:80,rockLayer:120,dungeonX:20,dungeonY:100,
          drunkWorld:false,dualDungeonsSeed:false,remixWorld:false,...flags}});
        run('batch_update_tiles',{rules:[
          {where:{biome_region:1,is_active:1},patch:{invisible_block:1}},
          {where:{biome_region:1,has_wall:1},patch:{invisible_wall:1}},
        ]});
        assert.deepEqual(fixtureCells(commitFixture(M,opened),width,height),expected);
      } finally {closeWorld(M,opened);}
      assert.equal(M._tx_heap_used(),baseline);
    }
  }
});

test('fourteen environment predicates cover furniture and compose without extra scans', async () => {
  const M=await loadModule(); M._tx_reset_heap();
  const baseline=M._tx_heap_used(), width=3200, height=900, cells=new Map(), probes=[];
  for(const [id,type,count,x] of [[3,53,1500,450],[4,147,1500,750],[5,25,300,1050],
    [6,203,300,1350],[7,117,125,1650],[8,70,100,1950],[2,60,140,2250],[1,41,300,2550]]) {
    for(let i=0;i<count;i++) cells.set(`${x+i%75},${250+Math.floor(i/75)}`,{type});
    probes.push({x:x+35,y:230,biomes:[id]},{x:x+35,y:390,biomes:[]});
  }
  for(const y of [69,70,71,200,201,400,401,700,701,899]) probes.push({x:2100,y,biomes:[]});
  for(const x of [0,379,380,width-380,width-379,width-1])
    for(const y of [340,341]) probes.push({x,y,biomes:[]});
  const regionsAt = ({x,y,biomes}) => {
    const ids=[...biomes];
    if(y<=340&&(x<380||x>width-380)) ids.push(9);
    if(y>700) ids.push(10);
    if(y<=200*0.3499999940395355) ids.push(12);
    if(y>200&&y<=400) ids.push(13);
    if(y>400&&y<=700) ids.push(14);
    if(y<=200&&!ids.length) ids.push(11);
    return ids;
  };
  for(const {x,y,biomes} of probes) cells.set(`${x},${y}`,{type:21,fx:108,fy:18,wall:biomes.includes(1)?7:4,color:13,wallColor:9});
  const source=makeEntityWorld(cells,{width,height,latest:true});
  const original=fixtureCells(source,width,height), single=new Map(), calls=new Map();
  const cases=[...Array.from({length:14},(_,i)=>[i+1]),[1,2],[3,4,5],[3,4,5,6,7],
    ...[9,10,11,12,13,14].map(length=>Array.from({length},(_,i)=>i+1))];
  for(const ids of cases) {
    const opened=openWorld(M,source);
    const run=(name,request={})=>{
      const result=executeOperation(M,opened.handle,name,request);
      assert.equal(result.status,0,result.status?JSON.stringify(readLastErrorJson(M)):undefined);
      return JSON.parse(result.value);
    };
    try {
      run('header_patch',{patch:{spawnTileX:1000,spawnTileY:100,worldSurface:200,rockLayer:400,dualDungeonsSeed:false,skyblockWorld:false}});
      if(ids.length>1) run('begin_output_preparation',{map:true});
      run('batch_update_tiles',{rules:ids.flatMap(biome_region=>[
        {where:{biome_region,is_active:1},patch:{invisible_block:1}},
        {where:{biome_region,has_wall:1},patch:{invisible_wall:1}},
      ])});
      if(ids.length>1) run('finish_output_preparation');
      const actual=fixtureCells(commitFixture(M,opened),width,height);
      for(const probe of probes) {
        const key=`${probe.x},${probe.y}`, expected={...original.get(key)};
        if(regionsAt(probe).some(id=>ids.includes(id))) Object.assign(expected,{invisible_block:1,invisible_wall:1});
        assert.deepEqual(actual.get(key),expected,`regions ${ids} at ${key}`);
      }
      if(ids.length===1) {single.set(ids[0],actual);calls.set(ids[0],run('get_output_preparation_stats').tile_decode_calls);}
      else {
        const expected=new Map([...original].map(([key,tile])=>[key,{...tile}]));
        for(const id of ids) for(const [key,tile] of single.get(id)) {
          if(tile.invisible_block) expected.get(key).invisible_block=1;
          if(tile.invisible_wall) expected.get(key).invisible_wall=1;
        }
        assert.deepEqual(actual,expected,`packed combined regions ${ids}`);
        assert.equal(run('get_output_preparation_stats').tile_decode_calls,calls.get(1),'one shared classification pass');
      }
    } finally {closeWorld(M,opened);}
    assert.equal(M._tx_heap_used(),baseline);
  }
  for(let id=1;id<=14;id++) assert.equal(calls.get(id),calls.get(10)*([9,10,12,13,14].includes(id)?1:2),`source passes for ${id}`);
});

test('environment thresholds handle sparse skyblocks, sunflowers, hallow cancellation and ocean sand', async () => {
  const M=await loadModule(), width=1200,height=700;
  for(const spec of [
    {id:3,type:53,count:1499,match:false},{id:3,type:53,count:1500,match:true},
    {id:4,type:147,count:1499,match:false},{id:4,type:147,count:1500,match:true},
    {id:5,type:25,count:300,flowers:1,match:false},{id:5,type:25,count:330,flowers:1,infectedSeed:true,match:true},
    {id:5,type:25,count:300,holy:125,match:false},{id:7,type:117,count:125,evil:1,match:false},
    {id:6,type:203,count:300,match:true},{id:8,type:70,count:99,match:false},{id:8,type:70,count:100,match:true},
    {id:3,type:53,count:300,skyblockWorld:true,match:true},{id:4,type:147,count:300,skyblockWorld:true,match:true},
    {id:3,type:53,count:300,skyblockWorld:true,dense:true,match:false},
    {id:3,type:53,count:1500,ocean:true,match:false},
  ]) {
    const cells=new Map(), start=spec.ocean?100:500, px=start+35, py=230;
    for(let i=0;i<spec.count;i++) cells.set(`${start+i%75},${250+Math.floor(i/75)}`,{type:spec.type});
    for(const [count,type,y] of [[spec.flowers,27,210],[spec.holy,117,211],[spec.evil,25,214]])
      for(let i=0;i<(count||0);i++) cells.set(`${start+i%75},${y+Math.floor(i/75)}`,{type});
    if(spec.dense) for(let x=0;x<300;x++) for(let y=0;y<350;y++) cells.set(`${x},${y}`,{type:1});
    cells.set(`${px},${py}`,{type:21,fx:0,fy:0,wall:4});
    const opened=openWorld(M,makeEntityWorld(cells,{width,height,latest:true}));
    try {
      let result=executeOperation(M,opened.handle,'header_patch',{patch:{spawnTileX:600,spawnTileY:100,
        worldSurface:250,rockLayer:350,skyblockWorld:!!spec.skyblockWorld,infectedSeed:!!spec.infectedSeed,dualDungeonsSeed:false}});
      assert.equal(result.status,0);
      result=executeOperation(M,opened.handle,'batch_update_tiles',{rules:[{where:{biome_region:spec.id,type:21},patch:{invisible_block:1}}]});
      assert.equal(result.status,0);
      assert.equal(!!fixtureCells(commitFixture(M,opened),width,height).get(`${px},${py}`).invisible_block,spec.match,JSON.stringify(spec));
    } finally {closeWorld(M,opened);}
  }
});

test('dual dungeon floor biomes support desert, snow, infections, hallow and mushroom together', async () => {
  const M=await loadModule(), width=128,height=800,cells=new Map();
  const floors=[[10,396,[3]],[25,147,[4]],[40,25,[5]],[55,203,[6]],[70,117,[7]],[85,70,[8]],[100,112,[3,5]]];
  for(const [x,type] of floors) {
    for(let y=60;y<=200;y++) cells.set(`${x},${y}`,{wall:187});
    cells.set(`${x},200`,{type,wall:187});
    cells.set(`${x},80`,{type:21,fx:0,fy:0,wall:187});
  }
  const source=makeEntityWorld(cells,{width,height,latest:true});
  for(let id=3;id<=8;id++) {
    const opened=openWorld(M,source);
    try {
      let result=executeOperation(M,opened.handle,'header_patch',{patch:{spawnTileX:20,spawnTileY:60,
        worldSurface:50,rockLayer:120,dualDungeonsSeed:true,skyblockWorld:false}});
      assert.equal(result.status,0);
      result=executeOperation(M,opened.handle,'batch_update_tiles',{rules:[{where:{biome_region:id,type:21},patch:{invisible_block:1}}]});
      assert.equal(result.status,0);
      const actual=fixtureCells(commitFixture(M,opened),width,height);
      for(const [x,,ids] of floors) assert.equal(!!actual.get(`${x},80`).invisible_block,ids.includes(id),`region ${id}, floor at ${x}`);
    } finally {closeWorld(M,opened);}
  }
});

test('invalid region selectors fail without silently modifying the entire world', async () => {
  const M=await loadModule(), opened=openWorld(M,makeEntityWorld());
  try {
    for(const biome_region of [null,true,'1',0,-1,15,1.5]) {
      const result=executeOperation(M,opened.handle,'batch_update_tiles',{rules:[{where:{biome_region},patch:{invisible_block:1}}]});
      assert.notEqual(result.status,0);
      const excluded=executeOperation(M,opened.handle,'batch_update_tiles',{rules:[{where:{exclude_biome_region:biome_region},patch:{invisible_block:1}}]});
      assert.notEqual(excluded.status,0);
    }
    assert.deepEqual(fixtureCells(commitFixture(M,opened),128,300),fixtureCells(makeEntityWorld(),128,300));
  } finally {closeWorld(M,opened);}
});

test('desert conversion preserves the original desert and lays sand over new surface terrain', async () => {
  const M=await loadModule(), width=1200, height=700, cells=new Map();
  for(let i=0;i<1500;i++) cells.set(`${500+i%75},${250+Math.floor(i/75)}`,{type:53});
  cells.set('535,230',{type:21,fx:108,fy:18,wall:7});
  cells.set('630,230',{type:21,fx:108,fy:18,wall:7}); // Fringe sees <1500 desert cells.
  cells.set('630,250',{type:397,wall:187});
  for(let y=180;y<=220;y++) cells.set(`800,${y}`,{type:0}); // One RLE run crosses worldSurface.
  cells.set('801,190',{type:1}); cells.set('802,190',{type:53});
  const source=makeEntityWorld(cells,{width,height,latest:true}), outputs=[];
  for(const prepare of [false,true]) {
    const opened=openWorld(M,source);
    const run=(name,request={})=>{const result=executeOperation(M,opened.handle,name,request);assert.equal(result.status,0,result.status?JSON.stringify(readLastErrorJson(M)):undefined);};
    try {
      run('header_patch',{patch:{spawnTileX:600,spawnTileY:100,worldSurface:200,rockLayer:400}});
      if(prepare)run('begin_output_preparation',{map:true});
      run('batch_update_tiles',{rules:[
        {where:{exclude_biome_region:3,is_active:1},patch:{terrain_theme:1}},
        {where:{exclude_biome_region:3,has_wall:1},patch:{wall_theme:1}},
        {where:{exclude_biome_region:3,is_active:1},patch:{furniture_theme:1}},
      ]});
      if(prepare)run('finish_output_preparation');
      const actual=fixtureCells(commitFixture(M,opened),width,height);
      assert.deepEqual(actual.get('535,230'),{type:21,fx:108,fy:18,wall:7});
      assert.deepEqual(actual.get('535,250'),{type:53});
      assert.deepEqual(actual.get('630,230'),{type:21,fx:108,fy:18,wall:7});
      assert.deepEqual(actual.get('630,250'),{type:397,wall:187});
      assert.equal(actual.get('800,200').type,53);
      assert.equal(actual.get('800,201').type,396);
      assert.equal(actual.get('801,190').type,396);
      assert.equal(actual.get('802,190').type,53);
      outputs.push(actual);
    } finally {closeWorld(M,opened);}
  }
  assert.deepEqual(outputs[1],outputs[0]);
});

test('dual dungeon environments use the nearest solid biome and respect depth and the 300-tile limit', async () => {
  const M=await loadModule(); M._tx_reset_heap();
  const width=128,height=800,cells=new Map();
  for(const x of [20,30,40,50,60,70]) for(let y=60;y<=500;y++) cells.set(`${x},${y}`,{wall:7});
  cells.set('20,360',{type:60,wall:7});
  cells.set('30,200',{type:41,wall:7});
  cells.set('40,100',{type:41,wall:7}); // Above rock layer: stops search without dungeon.
  cells.set('50,150',{type:147,wall:7}); cells.set('50,160',{type:60,wall:7});
  cells.set('60,149',{wall:64}); cells.set('60,150',{type:59,wall:7});
  cells.set('70,100',{type:61,wall:7}); cells.set('70,180',{type:226,wall:7}); // Non-solid jungle plant does not stop search.
  const source=makeEntityWorld(cells,{width,height,latest:true});
  for(const biome_region of [1,2]) {
    const opened=openWorld(M,source);
    const run=(name,request)=>{
      const result=executeOperation(M,opened.handle,name,request);
      assert.equal(result.status,0,result.status ? JSON.stringify(readLastErrorJson(M)) : undefined);
    };
    try {
      run('header_patch',{patch:{spawnTileX:20,spawnTileY:60,dungeonX:40,dungeonY:50,dualDungeonsSeed:true,worldSurface:50,rockLayer:120,drunkWorld:false,remixWorld:false}});
      run('batch_update_tiles',{rules:[{where:{biome_region,has_wall:1},patch:{invisible_wall:1}}]});
      const actual=fixtureCells(commitFixture(M,opened),width,height);
      const coated=(x,y)=>actual.get(`${x},${y}`)?.invisible_wall||0;
      if(biome_region===1) {
        assert.equal(coated(30,60),1); assert.equal(coated(30,200),1); assert.equal(coated(30,201),0);
        assert.equal(coated(40,60),0); assert.equal(coated(20,100),0);
      } else {
        assert.equal(coated(20,60),0); assert.equal(coated(20,61),1); assert.equal(coated(20,361),0);
        assert.equal(coated(50,100),0); assert.equal(coated(50,151),1);
        assert.equal(coated(60,100),1); assert.equal(coated(60,151),0);
        assert.equal(coated(70,90),1); assert.equal(coated(30,100),0);
      }
    } finally {closeWorld(M,opened);}
  }
});

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

function decodeMapCells(bytes) {
  assert.equal(bytes.readUInt32LE(0), 33083);
  let offset = 24, nameLength = 0, shift = 0, byte;
  do { byte = bytes[offset++]; nameLength |= (byte & 127) << shift; shift += 7; } while (byte & 128);
  offset += nameLength + 4;
  const height = bytes.readUInt32LE(offset), width = bytes.readUInt32LE(offset + 4);
  offset += 8;
  const tileCount = bytes.readUInt16LE(offset), wallCount = bytes.readUInt16LE(offset + 2);
  offset += 12;
  let typeCounts = 0;
  for (const count of [tileCount, wallCount]) {
    for (let i = 0; i < count; i++) typeCounts += (bytes[offset + (i >> 3)] >> (i & 7)) & 1;
    offset += Math.ceil(count / 8);
  }
  offset += typeCounts;
  const cells = new Uint32Array(width * height);
  for (let cy = 0; cy < Math.ceil(height / 64); cy++) for (let cx = 0; cx < Math.ceil(width / 64); cx++) {
    const length = bytes.readUInt32LE(offset); offset += 4;
    const chunk = zlib.inflateSync(bytes.subarray(offset, offset + length)); offset += length;
    assert.equal(chunk.length, 4096 * 4);
    for (let y = 0; y < 64 && cy * 64 + y < height; y++) for (let x = 0; x < 64 && cx * 64 + x < width; x++) {
      cells[(cy * 64 + y) * width + cx * 64 + x] = chunk.readUInt32LE((y * 64 + x) * 4);
    }
  }
  assert.equal(offset, bytes.length);
  return (x, y) => cells[y * width + x];
}

test('one tile scan survives commit and produces byte-identical MAP/full/list outputs', async () => {
  const M = await loadModule(); M._tx_reset_heap();
  const baseline = M._tx_heap_used();
  const marker = {tile_type:7,locate:2,icon_id:1000000,radius:12,line_width:2,color:'#FF00FFFF'};
  for (const replace of [false, true]) for (const markers of [[], [marker]]) for (const previewWidth of [0,960]) {
    const results = [];
    for (const prepare of [false, true]) {
      const opened = openWorld(M, makeEntityWorld());
      const run = (op, request = {}) => {
        const result = executeOperation(M, opened.handle, op, request);
        if (result.status !== 0) assert.fail(`${op}: ${JSON.stringify(readLastErrorJson(M))}`);
        return JSON.parse(result.value);
      };
      try {
        const before = run('get_output_preparation_stats').tile_decode_calls;
        if (prepare) run('begin_output_preparation', {tile_markers:markers,map:true,preview_width:previewWidth});
        if (replace) run('batch_update_tiles', {rules:[{where:{type:8},patch:{type:7}}]});
        if (prepare) run('finish_output_preparation');
        const scanned = run('get_output_preparation_stats');
        if (prepare) {
          assert.ok(scanned.source_runs > 0);
          assert.equal(scanned.tile_decode_calls - before, scanned.source_runs, 'exactly one source traversal');
        }
        const sizePtr = mustAlloc(M, 4, 'commit size');
        const handlePtr = mustAlloc(M, 4, 'commit handle');
        let ptr = 0, wld;
        try {
          assert.equal(M._terra_world_commit_to_buffer(opened.handle,0,0,sizePtr,handlePtr),0);
          const size = readU32(M,sizePtr); ptr = mustAlloc(M,size,'commit bytes');
          assert.equal(M._terra_world_commit_to_buffer(opened.handle,ptr,size,sizePtr,handlePtr),0);
          opened.handle = readU32(M,handlePtr);
          wld = Buffer.from(M.HEAPU8.subarray(ptr,ptr+size));
        } finally { if(ptr) M._tx_free(ptr); M._tx_free(sizePtr); M._tx_free(handlePtr); }
        M._tx_reclaim_transients();
        installSolidMarkerIcon(M,opened.handle,marker.icon_id,[0,255,0,255]);
        installMarkerColorIndex(M,opened.handle);
        run(markers.length ? 'mark_tiles_and_chests_map' : 'render_lit_map',{tile_markers:markers});
        const map = getMapBytes(M,opened.handle).map;
        const images = [];
        for (const max_w of [previewWidth,256]) {
          run(markers.length ? 'mark_tiles_and_chests_preview' : 'render_preview_png',{tile_markers:markers,max_w});
          images.push(getThumbnailPng(M,opened.handle).png);
          M._tx_reclaim_transients();
        }
        if (prepare) assert.equal(run('get_output_preparation_stats').tile_decode_calls, scanned.tile_decode_calls,
          'commit, MAP, full PNG and list PNG must not decode another tile');
        results.push({wld,map,images});
        run('batch_update_tiles', {rules:[{where:{type:7},patch:{type:8}}]});
        assert.equal(run('get_output_preparation_stats').ready,0,'subsequent mutation invalidates prepared outputs');
      } finally { closeWorld(M, opened); }
      assert.equal(M._tx_heap_used(),baseline,'persistent preparation is released at close');
    }
    assert.deepEqual(results[1],results[0],`preserve bytes: replace=${replace}, markers=${markers.length}`);
  }
});

test('entity PNG and MAP use icon pixels, retain rings, and invalidate locations after replacement', async () => {
  const M = await loadModule(); M._tx_reset_heap();
  const baseline = M._tx_heap_used();
  const opened = openWorld(M, makeEntityWorld());
  const marker = {tile_type:12,locate:1,frame_x:0,frame_y:0,icon_id:1000000,radius:12,line_width:2,color:'#FF00FFFF'};
  const run = (name, request) => {
    const result = executeOperation(M, opened.handle, name, request);
    assert.equal(result.status, 0);
    return JSON.parse(result.value);
  };
  try {
    installSolidMarkerIcon(M, opened.handle, marker.icon_id, [0, 255, 0, 255]);
    for (const max_w of [0,64,128]) {
      assert.equal(run('mark_tiles_and_chests_preview', {tile_markers:[marker],max_w}).matched_tile_count, 1);
      const png = getThumbnailPng(M, opened.handle).png;
      const image = max_w === 0 ? decodePngRgb(png) : decodePngRgba(png);
      assert.deepEqual(Array.from(pixelAt(image, Math.floor(50*image.width/128), Math.floor(40*image.height/300)).slice(0,3)), [0,255,0]);
      if (max_w === 0) assert.deepEqual(Array.from(pixelAt(image,62,40).slice(0,3)), [255,0,255]);
      M._tx_reclaim_transients();
    }
    installMarkerColorIndex(M, opened.handle);
    run('mark_tiles_and_chests_map', {tile_markers:[marker]});
    const withIcon = getMapBytes(M, opened.handle).map;
    const iconCells = decodeMapCells(withIcon);
    M._txw_clear_icon_atlas(opened.handle);
    run('mark_tiles_and_chests_map', {tile_markers:[marker]});
    const plainCells = decodeMapCells(getMapBytes(M, opened.handle).map);
    assert.notEqual(iconCells(50,40), plainCells(50,40), 'icon center must overwrite the actual map tile');
    assert.equal(iconCells(62,40), plainCells(62,40), 'circle remains unchanged');
    assert.equal(iconCells(80,40), plainCells(80,40), 'outside stays unchanged');
    run('mark_tiles_and_chests_map', {tile_markers:[{...marker,color:'#00FF00FF'}]});
    const greenCells = decodeMapCells(getMapBytes(M, opened.handle).map);
    assert.equal(iconCells(50,40), greenCells(62,40), 'green image pixel uses the green palette map tile');
    run('batch_update_tiles', {rules:[{where:{is_active:true,type:12},patch:{type:1}}]});
    assert.equal(run('mark_tiles_and_chests_preview', {tile_markers:[marker],max_w:64}).matched_tile_count, 0);
    getThumbnailPng(M, opened.handle);
    assert.equal(run('mark_tiles_and_chests_map', {tile_markers:[marker]}).matched_tile_count, 0);
    getMapBytes(M, opened.handle);
  } finally { closeWorld(M, opened); }
  assert.equal(M._tx_heap_used(), baseline, 'retained locations and atlas must be released on close');
});

test('transparent presets preserve every chest frame and color and never coat absent surfaces', async () => {
  const presets=[
    {id:'transparent-chests',rules:[21,467,441,468].map(type=>({where:{is_active:1,type},patch:{invisible_block:1}}))},
    {id:'transparent-world',rules:[{where:{is_active:1},patch:{invisible_block:1}},{where:{has_wall:1},patch:{invisible_wall:1}}]},
  ];
  const cells=new Map();
  for (const [i,type] of [21,467,441,468].entries()) for(let dx=0;dx<2;dx++) for(let dy=0;dy<2;dy++)
    cells.set(`${10+i*8+dx},${40+dy}`,{type,fx:108+dx*18,fy:dy*18,color:14,wall:87,wallColor:7});
  cells.set('50,60',{type:1,wall:7,color:3,wallColor:9});
  cells.set('51,60',{wall:7,wallColor:9});
  cells.set('52,60',{type:8});
  const M=await loadModule();
  const tileBytes=b=>b.subarray(b.readUInt32LE(10),b.readUInt32LE(14));
  for(const preset of presets) {
    const opened=openWorld(M,makeEntityWorld(cells));
    try {
      const result=executeOperation(M,opened.handle,'batch_update_tiles',{rules:preset.rules});
      assert.equal(result.status,0,result.status?JSON.stringify(readLastErrorJson(M)):undefined);
      const expected=new Map([...cells].map(([key,tile])=>{
        const next={...tile};
        if(preset.id==='transparent-world') {
          if(tile.type!==undefined) next.invisible_block=1;
          if(tile.wall) next.invisible_wall=1;
        } else if([21,467,441,468].includes(tile.type)) next.invisible_block=1;
        return [key,next];
      }));
      const sizePtr=mustAlloc(M,4,'size'), handlePtr=mustAlloc(M,4,'handle');
      let ptr=0;
      try {
        assert.equal(M._terra_world_commit_to_buffer(opened.handle,0,0,sizePtr,handlePtr),0);
        const size=readU32(M,sizePtr); ptr=mustAlloc(M,size,'bytes');
        assert.equal(M._terra_world_commit_to_buffer(opened.handle,ptr,size,sizePtr,handlePtr),0);
        opened.handle=readU32(M,handlePtr);
        assert.deepEqual(tileBytes(Buffer.from(M.HEAPU8.subarray(ptr,ptr+size))),tileBytes(makeEntityWorld(expected)),preset.id);
      } finally {if(ptr)M._tx_free(ptr);M._tx_free(handlePtr);M._tx_free(sizePtr);}
    } finally {closeWorld(M,opened);}
  }
  const opened=openWorld(M,makeEntityWorld(cells));
  try {
    for(const has_wall of ['bad',2,-1,null]) {
      const result=executeOperation(M,opened.handle,'batch_update_tiles',{rules:[{where:{has_wall},patch:{invisible_wall:1}}]});
      assert.notEqual(result.status,0,'invalid has_wall must not silently paint all tiles');
    }
  } finally {closeWorld(M,opened);}
});

test('composable themes preserve object frames, locks, contents and all prepared outputs', async () => {
  // Expected placement frames from ContentSamples / TileObjectData in Terraria 1.4.5.8.
  const furniture = [
    [21,0,0,[[467,360,0],[21,396,0],[21,288,0]]],
    [467,72,0,[[467,360,0],[21,396,0],[21,288,0]]],
    [441,0,0,[[468,360,0],[441,396,0],[441,288,0]]],
    [468,72,0,[[468,360,0],[441,396,0],[441,288,0]]],
    [10,18,54,[[10,72,378],[10,18,1458],[10,18,108]]],
    [11,108,72,[[11,108,396],[11,36,1476],[11,36,126]]],
    [19,216,18,[[19,216,756],[19,216,630],[19,216,36]]],
    [34,162,72,[[34,162,396],[34,54,612],[34,54,666]]],
    [42,18,54,[[42,18,1602],[42,18,666],[42,18,594]]],
    [14,54,18,[[469,378,18],[14,1296,18],[14,108,18]]],
    [469,54,18,[[469,378,18],[14,1296,18],[14,108,18]]],
    [18,36,0,[[18,1404,0],[18,720,0],[18,72,0]]],
    [33,18,22,[[33,18,814],[33,18,176],[33,18,198]]],
    [101,54,18,[[101,2106,18],[101,918,18],[101,648,18]]],
    [15,18,58,[[15,18,1738],[15,18,1138],[15,18,138]]],
    [79,90,54,[[79,90,1386],[79,90,558],[79,90,90]]],
    [87,54,18,[[87,2052,18],[87,378,18],[87,108,18]]],
    [88,54,18,[[88,2052,18],[88,1620,18],[88,108,18]]],
    [89,54,18,[[89,2214,18],[89,1458,18],[89,162,18]]],
    [90,90,54,[[90,90,1386],[90,90,198],[90,90,234]]],
    [93,18,72,[[93,18,2070],[93,18,288],[93,18,342]]],
    [100,36,54,[[100,36,1386],[100,36,342],[100,36,270]]],
    [104,36,18,[[104,1404,18],[104,396,18],[104,504,18]]],
    [172,18,56,[[172,18,1500],[172,18,816],[172,18,94]]],
    [497,18,58,[[497,18,1498],[497,18,618],[497,18,58]]],
    [4,88,22,[[4,88,352],[4,88,198],[4,88,462]]],
  ];
  const cells=new Map(furniture.map(([type,fx,fy],i)=>[`${10+i},40`,{type,fx,fy,color:14}]));
  const oresAndBuildings=[0,1,7,8,9,22,41,43,44,56,58,59,107,108,111,166,167,168,211,221,222,223,226,158,321,481];
  oresAndBuildings.forEach((type,i)=>cells.set(`${10+i},60`,{type,color:7}));
  [124,561,574,575,576,577,578].forEach((type,i)=>cells.set(`${10+i},70`,{type}));
  const protectedTiles=[[21,72],[21,144],[21,828],[21,864],[21,900],[21,936],[21,972],[21,1296],[21,1368],[21,1440],[467,468],[10,0,594],[14,1350],[26,0],[235,0]];
  protectedTiles.forEach(([type,fx,fy=0],i)=>cells.set(`${10+i},80`,{type,fx,fy}));
  cells.set('10,90',{wall:1,wallColor:8}); cells.set('11,90',{wall:7,wallColor:9});
  const source=makeEntityWorld(cells), canonical=fixtureCells(source,128,300), M=await loadModule();
  for(let theme=1;theme<=3;theme++) {
    const expected=new Map(canonical);
    furniture.forEach(([, , ,targets],i)=>{const [type,fx,fy]=targets[theme-1];expected.set(`${10+i},40`,{type,fx,fy,color:14});});
    oresAndBuildings.forEach((_,i)=>expected.set(`${10+i},60`,{type:[396,147,60][theme-1],color:7}));
    [124,561,574,575,576,577,578].forEach((_,i)=>expected.set(`${10+i},70`,{type:[577,574,575][theme-1]}));
    expected.set('10,90',{wall:[34,31,42][theme-1],wallColor:8});expected.set('11,90',{wall:[187,71,64][theme-1],wallColor:9});
    const outputs=[];
    for(const prepare of [false,true]) {
      const opened=openWorld(M,source);
      const run=(name,request={})=>{const r=executeOperation(M,opened.handle,name,request);assert.equal(r.status,0,r.status?JSON.stringify(readLastErrorJson(M)):undefined);return JSON.parse(r.value);};
      try {
        if(prepare)run('begin_output_preparation',{map:true});
        run('batch_update_tiles',{rules:[{where:{is_active:1},patch:{terrain_theme:theme,furniture_theme:theme}},{where:{has_wall:1},patch:{wall_theme:theme}}]});
        if(prepare)run('finish_output_preparation');
        const calls=run('get_output_preparation_stats').tile_decode_calls;
        assert.deepEqual(fixtureCells(commitFixture(M,opened),128,300),expected);
        installMarkerColorIndex(M,opened.handle);
        run('render_lit_map');const map=getMapBytes(M,opened.handle).map;
        run('render_preview_png',{max_w:0});const full=getThumbnailPng(M,opened.handle).png;
        run('render_preview_png',{max_w:256});const list=getThumbnailPng(M,opened.handle).png;
        if(prepare)assert.equal(run('get_output_preparation_stats').tile_decode_calls,calls);
        outputs.push({map,full,list});
      } finally {closeWorld(M,opened);}
    }
    assert.deepEqual(outputs[1],outputs[0]);
  }
  const opened=openWorld(M);
  try {
    const before=readSection(M,opened.handle,'chests');
    assert.equal(executeOperation(M,opened.handle,'batch_update_tiles',{rules:[{where:{is_active:1},patch:{furniture_theme:1}}]}).status,0);
    commitFixture(M,opened);
    assert.deepEqual(readSection(M,opened.handle,'chests'),before,'chest coordinates, names, all slots and item prefixes survive');
  } finally {closeWorld(M,opened);}
});

test('theme patches validate enums and obey arbitrary user conditions and later overrides', async () => {
  const M=await loadModule(),cells=new Map([['10,40',{type:21,fx:0,fy:0}],['20,40',{type:15,fx:18,fy:18}],['30,40',{type:1}],['40,40',{type:7}]]);
  const opened=openWorld(M,makeEntityWorld(cells));
  try {
    for(const field of ['terrain_theme','wall_theme','furniture_theme']) for(const value of [0,4,-1,1.5,true,null,'1','bad'])
      assert.notEqual(executeOperation(M,opened.handle,'batch_update_tiles',{rules:[{patch:{[field]:value}}]}).status,0);
    assert.deepEqual(fixtureCells(commitFixture(M,opened),128,300),fixtureCells(makeEntityWorld(cells),128,300));
    assert.equal(executeOperation(M,opened.handle,'batch_update_tiles',{rules:[
      {where:{type:21},patch:{furniture_theme:3,invisible_block:1}},
      {where:{type:7},patch:{terrain_theme:2,type:8}},
    ]}).status,0);
    cells.set('10,40',{type:21,fx:288,fy:0,invisible_block:1});cells.set('40,40',{type:8});
    assert.deepEqual(fixtureCells(commitFixture(M,opened),128,300),fixtureCells(makeEntityWorld(cells),128,300));
  } finally {closeWorld(M,opened);}
});
