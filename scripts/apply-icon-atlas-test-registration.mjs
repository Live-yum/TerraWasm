import fs from 'node:fs'

const file = 'build.ps1'
let source = fs.readFileSync(file, 'utf8')
const needle = 'tests/test_manifest_contract.js tests/test_marker_outputs.js tests/test_open_task.js'
const replacement = 'tests/test_manifest_contract.js tests/test_marker_outputs.js tests/test_icon_atlas_bridge.js tests/test_open_task.js'
const first = source.indexOf(needle)
if (first < 0) throw new Error('Node regression test list marker not found')
if (source.indexOf(needle, first + needle.length) >= 0) throw new Error('Node regression test list marker is not unique')
source = source.slice(0, first) + replacement + source.slice(first + needle.length)
fs.writeFileSync(file, source)
