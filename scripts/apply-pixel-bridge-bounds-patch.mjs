// Temporary exact-EOL patch helper; removed from the final PR tree.
import fs from 'node:fs'

function replaceOnce(source, oldText, newText, label) {
  const first = source.indexOf(oldText)
  if (first < 0) throw new Error(`${label}: source pattern not found`)
  if (source.indexOf(oldText, first + oldText.length) >= 0) {
    throw new Error(`${label}: source pattern is not unique`)
  }
  return source.slice(0, first) + newText + source.slice(first + oldText.length)
}

function patchMemory() {
  const file = 'src/terra_mem.c'
  let source = fs.readFileSync(file, 'utf8')
  const marker = `uint32_t tx_bridge_allocation_size(uint32_t ptr) {
    void* payload = (void*)(uintptr_t)ptr;
    TxAllocHeader* header = tx_find_root(payload, TX_DOMAIN_BRIDGE);
    return header ? header->root.size : 0u;
}
`
  const replacement = `${marker}
int tx_bridge_range_is_valid(uint32_t ptr, uint32_t length) {
    if (!ptr) return length == 0u;
    uintptr_t address = (uintptr_t)ptr;
    TxAllocHeader* root = tx_bridge_head;
    while (root) {
        if (root->root.magic == TX_ALLOC_MAGIC &&
            root->root.domain == TX_DOMAIN_BRIDGE &&
            root->root.self == (uintptr_t)root) {
            uintptr_t payload = (uintptr_t)((uint8_t*)root + sizeof(TxAllocHeader));
            if (address >= payload) {
                uintptr_t offset = address - payload;
                if (offset <= root->root.size &&
                    length <= root->root.size - (uint32_t)offset) {
                    return 1;
                }
            }
        }
        root = root->root.next;
    }
    return 0;
}
`
  source = replaceOnce(source, marker, replacement, 'bridge range helper')
  fs.writeFileSync(file, source)
}

function patchPixelArt() {
  const file = 'src/terra_pixel_art.c'
  let source = fs.readFileSync(file, 'utf8')

  source = replaceOnce(
    source,
    'extern uint32_t tx_bridge_allocation_size(uint32_t ptr);',
    'extern uint32_t tx_bridge_allocation_size(uint32_t ptr);\nextern int tx_bridge_range_is_valid(uint32_t ptr, uint32_t length);',
    'pixel bridge declaration',
  )

  source = replaceOnce(
    source,
    `    /* Copy RGBA pixels to bump allocator */
    uint8_t* pix = tx_alloc(pixels_len);
    if (!pix) {
        tx_set_error("TERRAX_WASM_OOM", "pixel art pixels allocation failed");
        return -1;
    }
    memcpy(pix, (const void*)(uintptr_t)pixels_ptr, pixels_len);

    /* Copy TxPixelMap array to bump allocator */
    uint32_t maps_size = map_count * sizeof(TxPixelMap);`,
    `    uint32_t expected_len = (uint32_t)expected_len64;
    uint32_t maps_size = map_count * sizeof(TxPixelMap);
    if (!tx_bridge_range_is_valid(pixels_ptr, expected_len) ||
        !tx_bridge_range_is_valid(map_ptr, maps_size)) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "pixel art payload exceeds its bridge allocation");
        return -1;
    }

    /* Only retain the bytes addressed by width*height. Callers may provide a
     * padded/backing buffer, but the unused suffix must not inflate native heap. */
    uint8_t* pix = tx_alloc(expected_len);
    if (!pix) {
        tx_set_error("TERRAX_WASM_OOM", "pixel art pixels allocation failed");
        return -1;
    }
    memcpy(pix, (const void*)(uintptr_t)pixels_ptr, expected_len);

    /* Copy TxPixelMap array to bump allocator */`,
    'queue pixel exact copy',
  )
  source = replaceOnce(
    source,
    '    w->pixel_art_pixels_len = pixels_len;',
    '    w->pixel_art_pixels_len = expected_len;',
    'queue pixel retained length',
  )

  source = replaceOnce(
    source,
    `    uint32_t maps_size = palette_count * sizeof(TxPixelMap);
    uint8_t* maps_buf = tx_alloc(maps_size);`,
    `    uint32_t palette_bytes = palette_count * 4u;
    uint32_t override_bytes = overrides_count * sizeof(TxPixelMap);
    if (!tx_bridge_range_is_valid(palette_ptr, palette_bytes) ||
        !tx_bridge_range_is_valid(txci_ptr, txci_len) ||
        (overrides_count > 0u && !tx_bridge_range_is_valid(overrides_ptr, override_bytes))) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "indexed pixel art payload exceeds its bridge allocation");
        return -1;
    }

    uint32_t maps_size = palette_count * sizeof(TxPixelMap);
    uint8_t* maps_buf = tx_alloc(maps_size);`,
    'indexed begin bridge validation',
  )

  source = replaceOnce(
    source,
    `    if (used > TX_PIXEL_ART_CHUNK_CELLS) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "chunk used count exceeds 4096");
        return -1;
    }
    if (used == 0u) {
        tx_clear_error();
        return 0;
    }

    uint32_t bytes = TX_PIXEL_ART_CHUNK_SIZE * TX_PIXEL_ART_CHUNK_SIZE * sizeof(uint16_t);`,
    `    if (used > TX_PIXEL_ART_CHUNK_CELLS) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "chunk used count exceeds 4096");
        return -1;
    }

    uint32_t bytes = TX_PIXEL_ART_CHUNK_SIZE * TX_PIXEL_ART_CHUNK_SIZE * sizeof(uint16_t);
    if (!tx_bridge_range_is_valid(indices_ptr, bytes)) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "chunk indices exceed their bridge allocation");
        return -1;
    }
    if (used == 0u) {
        tx_clear_error();
        return 0;
    }`,
    'indexed chunk bridge validation',
  )

  source = replaceOnce(
    source,
    `    uint32_t pixel_count = (uint32_t)pixel_count64;

    /* Load TXCI index from memory */`,
    `    uint32_t pixel_count = (uint32_t)pixel_count64;
    uint32_t expected_len = (uint32_t)expected_len64;
    uint32_t override_bytes = overrides_count * sizeof(TxPixelMap);
    if (!tx_bridge_range_is_valid(image_ptr, expected_len) ||
        !tx_bridge_range_is_valid(txci_ptr, txci_len) ||
        (overrides_count > 0u && !tx_bridge_range_is_valid(overrides_ptr, override_bytes))) {
        tx_set_error("TERRAX_INVALID_ARGUMENT", "pixel-art input exceeds its bridge allocation");
        return -1;
    }

    /* Load TXCI index from memory */`,
    'integrated pixel bridge validation',
  )

  source = replaceOnce(
    source,
    `    /* Copy RGBA pixels to bump allocator */
    uint8_t* pix = tx_alloc(image_len);
    if (!pix) {`,
    `    /* Retain only the RGBA prefix addressed by width*height. */
    uint8_t* pix = tx_alloc(expected_len);
    if (!pix) {`,
    'integrated pixel exact allocation',
  )
  source = replaceOnce(
    source,
    '    memcpy(pix, (const void*)(uintptr_t)image_ptr, image_len);',
    '    memcpy(pix, (const void*)(uintptr_t)image_ptr, expected_len);',
    'integrated pixel exact copy',
  )
  source = replaceOnce(
    source,
    '    w->pixel_art_pixels_len = image_len;',
    '    w->pixel_art_pixels_len = expected_len;',
    'integrated retained length',
  )

  fs.writeFileSync(file, source)
}

function patchTests() {
  const file = 'tests/test_pixel_art_bulk.js'
  let source = fs.readFileSync(file, 'utf8')
  const marker = 'test("generic operation output paths cannot escape the working directory", async () => {'
  const testBlock = `test("direct pixel-art ABI validates bridge ranges and retains only addressed RGBA bytes", async () => {
  const M = await TerraWorldWasm();
  const worldBytes = fs.readFileSync(WLD_PATH);
  const txci = fs.readFileSync(TXCI_PATH);

  function alloc(value) {
    const bytes = typeof value === "number" ? new Uint8Array(value) : new Uint8Array(value);
    const ptr = M._tx_malloc(bytes.byteLength || 1);
    assert.notEqual(ptr, 0);
    if (bytes.byteLength) M.HEAPU8.set(bytes, ptr);
    return ptr;
  }

  const inputPtr = alloc(worldBytes);
  const handlePtr = M._tx_malloc(4);
  assert.notEqual(handlePtr, 0);
  assert.equal(M._terra_world_open_from_buffer(inputPtr, worldBytes.byteLength, handlePtr), 0);
  const handle = M.HEAPU32[handlePtr >>> 2] >>> 0;
  assert.notEqual(handle, 0);
  M._tx_free(inputPtr);
  M._tx_free(handlePtr);

  try {
    const mapBytes = new Uint8Array(12);
    mapBytes[10] = 1;
    const mapPtr = alloc(mapBytes);
    const tinyPixelsPtr = alloc(Uint8Array.of(1, 2, 3, 255));
    const nativeBeforeShortQueue = M._tx_native_heap_used();
    assert.ok(M._txw_queue_pixel_art(
      handle, 0, 0, 2, 2,
      tinyPixelsPtr, 16,
      mapPtr, 1, 1,
    ) < 0, "short bridge allocation must be rejected before memcpy");
    assert.equal(M._tx_native_heap_used(), nativeBeforeShortQueue);
    M._tx_free(tinyPixelsPtr);

    const oversized = new Uint8Array(1024 * 1024);
    oversized.set([1, 2, 3, 255]);
    const oversizedPtr = alloc(oversized);
    const nativeBeforeOversizedQueue = M._tx_native_heap_used();
    assert.equal(M._txw_queue_pixel_art(
      handle, 0, 0, 1, 1,
      oversizedPtr, oversized.byteLength,
      mapPtr, 1, 1,
    ), 0);
    const retainedQueueBytes = M._tx_native_heap_used() - nativeBeforeOversizedQueue;
    assert.ok(
      retainedQueueBytes < 1024,
      \`1x1 queue retained \${retainedQueueBytes} bytes instead of only its addressed RGBA/maps prefix\`,
    );
    M._tx_free(oversizedPtr);
    M._tx_free(mapPtr);

    const txciPtr = alloc(txci);
    const palettePtr = alloc(Uint8Array.of(0, 0, 0, 0, 255, 0, 0, 255));
    const tinyPalettePtr = alloc(Uint8Array.of(0, 0, 0, 0));
    const nativeBeforeBadBegin = M._tx_native_heap_used();
    assert.ok(M._txw_begin_pixel_art_indexed(
      handle, 0, 0, 64, 64,
      tinyPalettePtr, 2,
      txciPtr, txci.byteLength,
      0, 0, 0, 0, 0,
    ) < 0, "palette count may not exceed the bridge allocation");
    assert.equal(M._tx_native_heap_used(), nativeBeforeBadBegin);
    M._tx_free(tinyPalettePtr);

    assert.equal(M._txw_begin_pixel_art_indexed(
      handle, 0, 0, 64, 64,
      palettePtr, 2,
      txciPtr, txci.byteLength,
      0, 0, 0, 0, 0,
    ), 0);
    const tinyIndicesPtr = alloc(new Uint8Array(2));
    const nativeBeforeBadChunk = M._tx_native_heap_used();
    assert.ok(M._txw_add_pixel_art_chunk(
      handle, 0, 0, tinyIndicesPtr, 4096, 1,
    ) < 0, "chunk count may not outgrow the bridge allocation");
    assert.equal(M._tx_native_heap_used(), nativeBeforeBadChunk);
    M._tx_free(tinyIndicesPtr);

    const tinyImagePtr = alloc(Uint8Array.of(1, 2, 3, 255));
    const nativeBeforeBadApply = M._tx_native_heap_used();
    assert.ok(M._txw_apply_pixel_art(
      handle,
      tinyImagePtr, 16, 2, 2,
      txciPtr, txci.byteLength,
      0, 0, 0, 0, 0, 0,
    ) < 0, "integrated pixel-art must reject a claimed image length beyond its bridge allocation");
    assert.equal(M._tx_native_heap_used(), nativeBeforeBadApply);
    M._tx_free(tinyImagePtr);

    M._tx_free(palettePtr);
    M._tx_free(txciPtr);
    assert.equal(M._tx_bridge_heap_used(), 0);
  } finally {
    assert.equal(M._terra_world_close(handle), 0);
    assert.equal(M._tx_bridge_heap_used(), 0);
    assert.equal(M._tx_native_heap_used(), 0);
  }
});

`
  source = replaceOnce(source, marker, testBlock + marker, 'pixel bridge regression test')
  fs.writeFileSync(file, source)
}

patchMemory()
patchPixelArt()
patchTests()
