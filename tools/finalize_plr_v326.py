from pathlib import Path


def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one match, got {count}")
    return text.replace(old, new, 1)

source_path = Path("src/terra_plr.c")
src = source_path.read_text(encoding="utf-8")
src = replace_once(
    src,
    "    if (v326_layout) {\n"
    "        int extension_flag = plr_read_u8(&reader) != 0u;\n"
    "        PlrJsonValue *extensions = plr_json_object();\n"
    "        if (!reader.ok || !extensions ||\n"
    "            !plr_json_object_put_bool(extensions, \"v326PrefixFlag\", extension_flag) ||\n"
    "            !plr_root_put(root, \"formatExtensions\", extensions)) {\n"
    "            plr_json_free(extensions);\n"
    "            plr_json_free(root);\n"
    "            return NULL;\n"
    "        }\n"
    "        extensions = NULL;\n"
    "    }\n",
    "    if (v326_layout) {\n"
    "        int extension_flag = plr_read_u8(&reader) != 0u;\n"
    "        PlrJsonValue *extensions = plr_json_object();\n"
    "        if (!reader.ok || !extensions ||\n"
    "            !plr_json_object_put_bool(extensions, \"v326PrefixFlag\", extension_flag)) {\n"
    "            plr_json_free(extensions);\n"
    "            plr_json_free(root);\n"
    "            return NULL;\n"
    "        }\n"
    "        /* plr_root_put owns extensions on both success and insertion failure. */\n"
    "        if (!plr_root_put(root, \"formatExtensions\", extensions)) {\n"
    "            plr_json_free(root);\n"
    "            return NULL;\n"
    "        }\n"
    "        extensions = NULL;\n"
    "    }\n",
    "formatExtensions ownership",
)
source_path.write_text(src, encoding="utf-8")

real_test_path = Path("tests/test_plr_real_fixture.js")
test = real_test_path.read_text(encoding="utf-8")
test = replace_once(
    test,
    "    assertSemanticShape(original);\n\n"
    "    const clean = encode(module, handle);\n",
    "    assertSemanticShape(original);\n"
    "    assert.equal(original.version, 326, \"real fixture version changed unexpectedly\");\n"
    "    assert.equal(original.taxMoney, 113750, \"v326 prefix byte alignment regression\");\n"
    "    assert.equal(original.numberOfDeathsPve, 11, \"v326 death-counter alignment regression\");\n"
    "    assert.equal(original.voiceVariant, 2, \"v326 loadout alignment regression\");\n"
    "    assert.equal(original.voicePitchOffset, 0, \"v326 voice pitch alignment regression\");\n"
    "    assert.equal(original.formatExtensions?.v326PrefixFlag, true,\n"
    "      \"v326 body-prefix extension byte was not preserved\");\n"
    "    assert.equal(original.armor[0].itemType, 3381, \"v326 main armor layout regression\");\n\n"
    "    const clean = encode(module, handle);\n",
    "node real fixture semantic assertions",
)
test = replace_once(
    test,
    "    handle = openBuffer(module, source);\n"
    "    const originalName = getField(module, handle, \"/name\");\n",
    "    handle = openBuffer(module, source);\n"
    "    assert.equal(getField(module, handle, \"/version\"), 326);\n"
    "    assert.equal(getField(module, handle, \"/taxMoney\"), 113750);\n"
    "    assert.equal(getField(module, handle, \"/voiceVariant\"), 2);\n"
    "    assert.equal(getField(module, handle, \"/formatExtensions/v326PrefixFlag\"), true);\n"
    "    const originalName = getField(module, handle, \"/name\");\n",
    "web real fixture semantic assertions",
)
real_test_path.write_text(test, encoding="utf-8")

readme_path = Path("tests/files/README.md")
readme = readme_path.read_text(encoding="utf-8")
append = """

## v326 compatibility contract

The checked-in `烟花.plr` fixture is currently a Terraria player file with `version = 326`.
For v326 and later-known-layout parsing, TerraWasm preserves the additional body-prefix boolean as
`formatExtensions.v326PrefixFlag` and reads/writes the additional `favorited` byte on the main
armor/dye slots and all three loadout armor/dye sets. `miscEquips` and `miscDyes` remain on their
legacy type+prefix layout. Versions below 326 keep the legacy layout with no extra bytes.

Versions are not rejected by a numeric maximum. TerraWasm attempts the newest known layout for
higher version numbers and relies on bounded field parsing plus the strict plaintext EOF check to
reject files whose actual binary layout is incompatible.
"""
if "## v326 compatibility contract" not in readme:
    readme += append
readme_path.write_text(readme, encoding="utf-8")
