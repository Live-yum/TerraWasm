// Temporary exact-EOL patch helper; removed from the final PR tree.
import fs from 'node:fs'

function replaceRegion(source, startMarker, endMarker, replacement, label) {
  const start = source.indexOf(startMarker)
  if (start < 0) throw new Error(`${label}: start marker not found`)
  const end = source.indexOf(endMarker, start)
  if (end < 0) throw new Error(`${label}: end marker not found`)
  return source.slice(0, start) + replacement + source.slice(end)
}

function replaceOnce(source, oldText, newText, label) {
  const first = source.indexOf(oldText)
  if (first < 0) throw new Error(`${label}: source pattern not found`)
  if (source.indexOf(oldText, first + oldText.length) >= 0) {
    throw new Error(`${label}: source pattern is not unique`)
  }
  return source.slice(0, first) + newText + source.slice(first + oldText.length)
}

function patchReader() {
  const file = 'src/terra_wld.c'
  let source = fs.readFileSync(file, 'utf8')
  const eol = source.includes('\r\n') ? '\r\n' : '\n'
  const replacement = [
    'void rd_string_copy(const uint8_t *p,uint32_t len,uint32_t *off,char *out,uint32_t cap){',
    '    if (!off){',
    '        if (out&&cap)out[0]=0;',
    '        return;',
    '        }',
    '    if (!p){',
    '        if (out&&cap)out[0]=0;',
    '        *off=len;',
    '        return;',
    '        }',
    '    int ok=0;',
    '    uint32_t slen=rd_7bit(p,len,off,&ok);',
    '    if (!ok||!terra_reader_has(*off,slen,len)){',
    '        if (out&&cap)out[0]=0;',
    '        *off=len;',
    '        return;',
    '        }',
    '    uint32_t n=slen;',
    '    if (!out||cap==0u)n=0u;',
    '    else if (n>=cap)n=cap-1u;',
    '    for (uint32_t i=0;',
    '    i<n;',
    '    i++)out[i]=(char)p[*off+i];',
    '    if (out&&cap)out[n]=0;',
    '    (void)terra_reader_take(off,slen,len);',
    '    }',
  ].join(eol) + eol
  source = replaceRegion(
    source,
    'void rd_string_copy(const uint8_t *p,uint32_t len,uint32_t *off,char *out,uint32_t cap){',
    'void uuid_to_string(const uint8_t *p,char *out){',
    replacement,
    'rd_string_copy',
  )

  const oldSkip = [
    'void rd_skip_string_value(const uint8_t *p,uint32_t len,uint32_t *off){',
    '    int ok=0;',
    '    uint32_t slen=rd_7bit(p,len,off,&ok);',
    '    if (!ok||!terra_reader_has(*off,slen,len)){',
    '        *off=len;',
    '        ;',
    '        }',
    '    (void)terra_reader_take(off,slen,len);',
    '    }',
  ].join(eol)
  const newSkip = [
    'void rd_skip_string_value(const uint8_t *p,uint32_t len,uint32_t *off){',
    '    if (!off)return;',
    '    if (!p){',
    '        *off=len;',
    '        return;',
    '        }',
    '    int ok=0;',
    '    uint32_t slen=rd_7bit(p,len,off,&ok);',
    '    if (!ok||!terra_reader_has(*off,slen,len)){',
    '        *off=len;',
    '        return;',
    '        }',
    '    (void)terra_reader_take(off,slen,len);',
    '    }',
  ].join(eol)
  source = replaceOnce(source, oldSkip, newSkip, 'rd_skip_string_value')
  fs.writeFileSync(file, source)
}

function patchNativeContract() {
  const file = 'tests/native_contract.c'
  let source = fs.readFileSync(file, 'utf8')

  const declarationAnchor = 'static int read_file_alloc'
  const declarations = [
    'void rd_string_copy(const uint8_t* p, uint32_t len, uint32_t* off, char* out, uint32_t cap);',
    'void rd_skip_string_value(const uint8_t* p, uint32_t len, uint32_t* off);',
    '',
    'static int read_file_alloc',
  ].join('\n')
  source = replaceOnce(source, declarationAnchor, declarations, 'reader declarations')

  const testBlock = [
    'static int test_string_reader_bounds(void) {',
    '    static const unsigned char truncated[] = {5u, \'A\'};',
    '    static const unsigned char valid[] = {5u, \'h\', \'e\', \'l\', \'l\', \'o\'};',
    '    static const unsigned char malformed_7bit[] = {0x80u, 0x80u, 0x80u, 0x80u, 0x80u};',
    '    char output[8] = "sentinel";',
    '    uint32_t off = 0u;',
    '',
    '    rd_string_copy(truncated, (uint32_t)sizeof(truncated), &off, output, sizeof(output));',
    '    if (!expect(off == sizeof(truncated), "native reader contract: truncated string offset did not clamp")) return 0;',
    '    if (!expect(output[0] == \'\\0\', "native reader contract: truncated string output was not cleared")) return 0;',
    '',
    '    off = 0u;',
    '    memset(output, 0x7f, sizeof(output));',
    '    rd_string_copy(valid, (uint32_t)sizeof(valid), &off, output, 4u);',
    '    if (!expect(off == sizeof(valid), "native reader contract: valid string offset did not advance")) return 0;',
    '    if (!expect(strcmp(output, "hel") == 0, "native reader contract: bounded copy did not terminate correctly")) return 0;',
    '',
    '    off = 0u;',
    '    rd_string_copy(valid, (uint32_t)sizeof(valid), &off, NULL, 0u);',
    '    if (!expect(off == sizeof(valid), "native reader contract: zero-capacity copy did not consume the string")) return 0;',
    '',
    '    off = 0u;',
    '    output[0] = \'X\';',
    '    rd_string_copy(malformed_7bit, (uint32_t)sizeof(malformed_7bit), &off, output, sizeof(output));',
    '    if (!expect(off == sizeof(malformed_7bit), "native reader contract: malformed 7-bit length did not clamp")) return 0;',
    '    if (!expect(output[0] == \'\\0\', "native reader contract: malformed 7-bit output was not cleared")) return 0;',
    '',
    '    off = 0u;',
    '    rd_skip_string_value(truncated, (uint32_t)sizeof(truncated), &off);',
    '    if (!expect(off == sizeof(truncated), "native reader contract: truncated skipped string did not clamp")) return 0;',
    '',
    '    puts("native reader contract: bounded string readers enforced");',
    '    return 1;',
    '}',
    '',
  ].join('\n')
  source = replaceOnce(source, 'int main(void) {', testBlock + 'int main(void) {', 'reader test insertion')
  source = replaceOnce(
    source,
    '    if (!test_native_terraria_header_layout()) return 3;',
    '    if (!test_native_terraria_header_layout()) return 3;\n    if (!test_string_reader_bounds()) return 4;',
    'reader test main insertion',
  )
  source = replaceOnce(
    source,
    '    if (!test_failed_save_preserves_destination()) return 4;',
    '    if (!test_failed_save_preserves_destination()) return 5;',
    'reader test main renumber',
  )
  fs.writeFileSync(file, source)
}

patchReader()
patchNativeContract()
