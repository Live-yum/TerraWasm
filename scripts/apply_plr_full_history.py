from pathlib import Path
import re

# One-shot transformation: removed by the apply workflow after generation.
ROOT = Path(__file__).resolve().parents[1]
PLR = ROOT / "src" / "terra_plr.c"
TEST = ROOT / "tests" / "test_plr.js"
README = ROOT / "README.md"
FIXTURE_README = ROOT / "tests" / "files" / "README.md"

text = PLR.read_text(encoding="utf-8")

VERSION_BLOCK = r'''#define PLR_CURRENT_KNOWN_VERSION 326

/* Player.Deserialize has no lower version rejection. Releases before 135 use
 * the legacy body without FileMetadata; releases 1-37 additionally use item
 * names instead of numeric item ids. Keep 326 as a known-layout marker only:
 * newer releases are attempted with the latest known layout and rejected only
 * when their bytes no longer match it. */
static int plr_version_supported(int32_t version) {
    return version > 0;
}

static int plr_version_has_metadata(int32_t version) { return version >= 135; }
static int plr_version_has_difficulty(int32_t version) { return version >= 10; }
static int plr_version_has_byte_difficulty(int32_t version) { return version >= 17; }
static int plr_version_has_play_time(int32_t version) { return version >= 138; }
static int plr_version_has_hair_dye(int32_t version) { return version >= 82; }
static int plr_version_has_team(int32_t version) { return version >= 283; }
static int plr_version_has_hide_lower(int32_t version) { return version >= 83; }
static int plr_version_has_hide_upper(int32_t version) { return version >= 124; }
static int plr_version_has_hide_misc(int32_t version) { return version >= 119; }
static int plr_version_has_skin_variant(int32_t version) { return version >= 107; }
static int plr_version_has_gender_bool(int32_t version) { return version >= 18 && version < 107; }
static int plr_version_has_extra_accessory(int32_t version) { return version >= 125; }
static int plr_version_has_biome_torches(int32_t version) { return version >= 229; }
static int plr_version_has_artisan_bread(int32_t version) { return version >= 256; }
static int plr_version_has_reserved_324(int32_t version) { return version >= 324; }
static int plr_version_has_permanent_upgrades(int32_t version) { return version >= 260; }
static int plr_version_has_dd2_flag(int32_t version) { return version >= 182; }
static int plr_version_has_tax_money(int32_t version) { return version >= 128; }
static int plr_version_has_death_counts(int32_t version) { return version >= 254; }
static int plr_version_has_numeric_items(int32_t version) { return version >= 38; }
static int plr_version_has_dyes(int32_t version) { return version >= 47; }
static int plr_version_has_inventory_favorites(int32_t version) { return version >= 114; }
static int plr_version_has_misc_equips(int32_t version) { return version >= 117; }
static int plr_version_has_forge(int32_t version) { return version >= 182; }
static int plr_version_has_void_vault(int32_t version) { return version >= 198; }
static int plr_version_has_void_info(int32_t version) { return version >= 199; }
static int plr_version_has_void_favorites(int32_t version) { return version >= 255; }
static int plr_version_has_buffs(int32_t version) { return version >= 11; }
static int plr_version_has_hb_locked(int32_t version) { return version >= 16; }
static int plr_version_has_hide_info(int32_t version) { return version >= 115; }
static int plr_version_has_angler(int32_t version) { return version >= 98; }
static int plr_version_has_dpad(int32_t version) { return version >= 162; }
static int plr_version_has_builder_status(int32_t version) { return version >= 164; }
static int plr_version_has_bartender(int32_t version) { return version >= 181; }
static int plr_version_has_death_metadata(int32_t version) { return version >= 200; }
static int plr_version_has_last_save(int32_t version) { return version >= 202; }
static int plr_version_has_golfer_score(int32_t version) { return version >= 206; }
static int plr_version_has_temporary_slots(int32_t version) { return version >= 214; }
static int plr_version_has_creative_tracker(int32_t version) { return version >= 218; }
static int plr_version_has_creative_powers(int32_t version) { return version >= 220; }
static int plr_version_has_super_cart(int32_t version) { return version >= 253; }
static int plr_version_has_loadouts(int32_t version) { return version >= 262; }
static int plr_version_has_voice_variant(int32_t version) { return version >= 280; }
static int plr_version_has_voice_pitch(int32_t version) { return version >= 281; }
static int plr_version_has_tracker_new_unlock_flag(int32_t version) { return version >= 282; }
static int plr_version_has_pending_refunds(int32_t version) { return version >= 300; }
static int plr_version_has_dialogues(int32_t version) { return version >= 310; }
static int plr_version_has_equipment_favorites(int32_t version) { return version >= 322; }

static uint32_t plr_version_armor_slots(int32_t version) {
    if (version < 38) return 0u;
    if (version < 81) return 11u;
    if (version < 124) return 16u;
    return 20u;
}
static uint32_t plr_version_dye_slots(int32_t version) {
    if (version < 47) return 0u;
    if (version < 81) return 3u;
    if (version < 124) return 8u;
    return 10u;
}
static uint32_t plr_version_inventory_disk_slots(int32_t version) {
    return version >= 58 ? 58u : 48u;
}
static uint32_t plr_version_bank_slots(int32_t version) {
    return version >= 58 ? 40u : 20u;
}
static uint32_t plr_version_buff_slots(int32_t version) {
    if (version < 11) return 0u;
    if (version < 74) return 10u;
    if (version < 252) return 22u;
    return 44u;
}
static uint32_t plr_version_builder_slots(int32_t version) {
    if (version < 164) return 0u;
    if (version < 167) return 8u;
    if (version < 197) return 10u;
    if (version < 230) return 11u;
    return 12u;
}
static int plr_skin_variant_is_male(uint8_t value) {
    return value == 0u || value == 1u || value == 2u || value == 3u ||
        value == 8u || value == 10u;
}
'''

text, n = re.subn(
    r'#define PLR_MIN_SUPPORTED_VERSION 280\n#define PLR_CURRENT_VERSION 326\n.*?(?=/\* -------------------------------------------------------------------------\n \* Semantic model readers)',
    VERSION_BLOCK + "\n", text, count=1, flags=re.S)
assert n == 1, "version helper block not found"

# Add legacy/default item helpers after the fixed item-array reader.
needle = '''static PlrJsonValue *plr_read_buffs(PlrReader *reader) {'''
HELPERS = r'''static PlrJsonValue *plr_make_default_item_array(uint32_t count) {
    PlrJsonValue *array = plr_json_array();
    if (!array) return NULL;
    for (uint32_t i = 0u; i < count; i++) {
        PlrJsonValue *item = plr_make_item(0, 0, 0u, 0);
        if (!item || !plr_json_array_push(array, item)) {
            plr_json_free(item); plr_json_free(array); return NULL;
        }
    }
    return array;
}

static int plr_array_replace_owned(
    PlrJsonValue *array, uint32_t index, PlrJsonValue *value) {
    if (!array || array->type != PLR_JSON_ARRAY || index >= array->as.array.count || !value) {
        plr_json_free(value); return 0;
    }
    plr_json_free(array->as.array.items[index]);
    array->as.array.items[index] = value;
    return 1;
}

static PlrJsonValue *plr_read_legacy_item(
    PlrReader *reader, int32_t version, int with_stack) {
    char *legacy_name = plr_read_string(reader);
    int32_t stack = with_stack ? plr_read_i32(reader) : 0;
    uint8_t prefix = version >= 36 ? plr_read_u8(reader) : 0u;
    if (!reader->ok || !legacy_name) { free(legacy_name); return NULL; }
    if (!with_stack) stack = legacy_name[0] ? 1 : 0;
    PlrJsonValue *item = plr_make_item(0, stack, prefix, 0);
    if (!item || !plr_json_object_put_string_owned(item, "legacyName", legacy_name)) {
        plr_json_free(item); return NULL;
    }
    return item;
}

static PlrJsonValue *plr_make_default_bool_array(uint32_t count) {
    PlrJsonValue *array = plr_json_array();
    if (!array) return NULL;
    for (uint32_t i = 0u; i < count; i++) {
        PlrJsonValue *value = plr_json_bool(0);
        if (!value || !plr_json_array_push(array, value)) {
            plr_json_free(value); plr_json_free(array); return NULL;
        }
    }
    return array;
}

static PlrJsonValue *plr_make_default_i32_array(uint32_t count) {
    PlrJsonValue *array = plr_json_array();
    if (!array) return NULL;
    for (uint32_t i = 0u; i < count; i++) {
        PlrJsonValue *value = plr_json_i64(0);
        if (!value || !plr_json_array_push(array, value)) {
            plr_json_free(value); plr_json_free(array); return NULL;
        }
    }
    return array;
}

static PlrJsonValue *plr_read_buffs(PlrReader *reader, uint32_t stored_count) {'''
text = text.replace(needle, HELPERS, 1)
# Pad historical buff counts to the unified 44-slot model.
text = text.replace(
    'for (uint32_t i = 0u; i < PLR_BUFF_SLOTS; i++) {\n        int32_t buff_type = plr_read_i32(reader);',
    'for (uint32_t i = 0u; i < PLR_BUFF_SLOTS; i++) {\n        int32_t buff_type = i < stored_count ? plr_read_i32(reader) : 0;', 1)
text = text.replace(
    'int32_t buff_time = plr_read_i32(reader);\n        PlrJsonValue *buff = plr_json_object();',
    'int32_t buff_time = i < stored_count ? plr_read_i32(reader) : 0;\n        PlrJsonValue *buff = plr_json_object();', 1)

# Creative tracker gained its leading bool in release 282.
text = text.replace(
    'static PlrJsonValue *plr_read_sacrifices(PlrReader *reader) {\n    int has_new_unlocks = plr_read_u8(reader) != 0u;',
    'static PlrJsonValue *plr_read_sacrifices(PlrReader *reader, int32_t version) {\n    int has_new_unlocks = plr_version_has_tracker_new_unlock_flag(version) ?\n        (plr_read_u8(reader) != 0u) : 0;', 1)

PARSE = r'''static PlrJsonValue *plr_parse_plain(
    const uint8_t *plain, uint32_t plain_length) {
    PlrReader reader = {plain, plain_length, 0u, 1};
    int32_t version = plr_read_i32(&reader);
    if (!reader.ok || !plr_version_supported(version)) return NULL;

    PlrJsonValue *root = plr_json_object();
    if (!root) return NULL;
    if (!plr_json_object_put_i64(root, "version", version)) {
        plr_json_free(root); return NULL;
    }

    if (plr_version_has_metadata(version)) {
        uint64_t magic_and_type = plr_read_u64(&reader);
        uint32_t revision = plr_read_u32(&reader);
        uint64_t favorite_flags = plr_read_u64(&reader);
        if (!reader.ok || (magic_and_type & UINT64_C(0x00ffffffffffffff)) !=
            PLR_METADATA_MAGIC_LOW_56 ||
            ((magic_and_type >> 56u) & 0xffu) != PLR_PLAYER_FILE_TYPE) {
            plr_json_free(root); return NULL;
        }
        PlrJsonValue *metadata = plr_json_object();
        if (!metadata ||
            !plr_json_object_put_u64(metadata, "favoriteFlags", favorite_flags) ||
            !plr_json_object_put_u64(metadata, "magicAndType", magic_and_type) ||
            !plr_json_object_put_u64(metadata, "revision", revision) ||
            !plr_root_put(root, "metadata", metadata)) {
            plr_json_free(metadata); plr_json_free(root); return NULL;
        }
    } else if (!plr_root_put(root, "metadata", plr_json_null())) {
        plr_json_free(root); return NULL;
    }

    char *name = plr_read_string(&reader);
    if (!name || !plr_json_object_put_string_owned(root, "name", name)) {
        free(name); plr_json_free(root); return NULL;
    }

    uint8_t difficulty = 0u;
    if (plr_version_has_difficulty(version)) {
        if (plr_version_has_byte_difficulty(version)) difficulty = plr_read_u8(&reader);
        else difficulty = plr_read_u8(&reader) ? 2u : 0u;
    }
    int64_t play_time = plr_version_has_play_time(version) ? plr_read_i64(&reader) : 0;
    int32_t hair = plr_read_i32(&reader);
    uint8_t hair_dye = plr_version_has_hair_dye(version) ? plr_read_u8(&reader) : 0u;
    uint8_t team = plr_version_has_team(version) ? plr_read_u8(&reader) : 0u;
    if (!reader.ok ||
        !plr_json_object_put_u64(root, "difficulty", difficulty) ||
        !plr_json_object_put_i64(root, "playTimeTicks", play_time) ||
        !plr_json_object_put_i64(root, "hair", hair) ||
        !plr_json_object_put_u64(root, "hairDye", hair_dye) ||
        !plr_json_object_put_u64(root, "team", team)) {
        plr_json_free(root); return NULL;
    }

    uint8_t hide_lower = plr_version_has_hide_lower(version) ? plr_read_u8(&reader) : 0u;
    uint8_t hide_upper = plr_version_has_hide_upper(version) ? plr_read_u8(&reader) : 0u;
    PlrJsonValue *hide_accessory = plr_json_array();
    if (!hide_accessory) { plr_json_free(root); return NULL; }
    for (uint32_t i = 0u; i < PLR_DYE_SLOTS; i++) {
        int value = i < 8u ? ((hide_lower >> i) & 1u) != 0u :
            ((hide_upper >> (i - 8u)) & 1u) != 0u;
        PlrJsonValue *boolean = plr_json_bool(value);
        if (!boolean || !plr_json_array_push(hide_accessory, boolean)) {
            plr_json_free(boolean); plr_json_free(hide_accessory); plr_json_free(root); return NULL;
        }
    }
    if (!plr_root_put(root, "hideVisibleAccessory", hide_accessory)) {
        plr_json_free(root); return NULL;
    }
    uint8_t hide_misc = plr_version_has_hide_misc(version) ? plr_read_u8(&reader) : 0u;
    uint8_t skin_variant = 0u;
    if (plr_version_has_skin_variant(version)) skin_variant = plr_read_u8(&reader);
    else if (plr_version_has_gender_bool(version)) skin_variant = plr_read_u8(&reader) ? 0u : 4u;
    else skin_variant = (hair == 5 || hair == 6 || hair == 9 || hair == 11) ? 4u : 0u;
    if (!reader.ok ||
        !plr_json_object_put_u64(root, "hideMisc", hide_misc) ||
        !plr_json_object_put_u64(root, "skinVariant", skin_variant)) {
        plr_json_free(root); return NULL;
    }

    int32_t stat_life = plr_read_i32(&reader);
    int32_t stat_life_max = plr_read_i32(&reader);
    int32_t stat_mana = plr_read_i32(&reader);
    int32_t stat_mana_max = plr_read_i32(&reader);
    int extra_accessory = plr_version_has_extra_accessory(version) ? (plr_read_u8(&reader) != 0u) : 0;
    int unlocked_torches = 0, using_torches = 0, artisan_bread = 0;
    int upgrades[6] = {0, 0, 0, 0, 0, 0};
    if (plr_version_has_biome_torches(version)) {
        unlocked_torches = plr_read_u8(&reader) != 0u;
        using_torches = plr_read_u8(&reader) != 0u;
        if (plr_version_has_artisan_bread(version)) artisan_bread = plr_read_u8(&reader) != 0u;
        if (plr_version_has_reserved_324(version)) (void)plr_read_u8(&reader);
        if (plr_version_has_permanent_upgrades(version))
            for (uint32_t i = 0u; i < 6u; i++) upgrades[i] = plr_read_u8(&reader) != 0u;
    }
    int dd2 = plr_version_has_dd2_flag(version) ? (plr_read_u8(&reader) != 0u) : 0;
    int32_t tax_money = plr_version_has_tax_money(version) ? plr_read_i32(&reader) : 0;
    int32_t deaths_pve = plr_version_has_death_counts(version) ? plr_read_i32(&reader) : 0;
    int32_t deaths_pvp = plr_version_has_death_counts(version) ? plr_read_i32(&reader) : 0;
    if (!reader.ok ||
        !plr_json_object_put_i64(root, "statLife", stat_life) ||
        !plr_json_object_put_i64(root, "statLifeMax", stat_life_max) ||
        !plr_json_object_put_i64(root, "statMana", stat_mana) ||
        !plr_json_object_put_i64(root, "statManaMax", stat_mana_max) ||
        !plr_json_object_put_bool(root, "extraAccessory", extra_accessory) ||
        !plr_json_object_put_bool(root, "unlockedBiomeTorches", unlocked_torches) ||
        !plr_json_object_put_bool(root, "usingBiomeTorches", using_torches) ||
        !plr_json_object_put_bool(root, "ateArtisanBread", artisan_bread) ||
        !plr_json_object_put_bool(root, "usedAegisCrystal", upgrades[0]) ||
        !plr_json_object_put_bool(root, "usedAegisFruit", upgrades[1]) ||
        !plr_json_object_put_bool(root, "usedArcaneCrystal", upgrades[2]) ||
        !plr_json_object_put_bool(root, "usedGalaxyPearl", upgrades[3]) ||
        !plr_json_object_put_bool(root, "usedGummyWorm", upgrades[4]) ||
        !plr_json_object_put_bool(root, "usedAmbrosia", upgrades[5]) ||
        !plr_json_object_put_bool(root, "downedDd2EventAnyDifficulty", dd2) ||
        !plr_json_object_put_i64(root, "taxMoney", tax_money) ||
        !plr_json_object_put_i64(root, "numberOfDeathsPve", deaths_pve) ||
        !plr_json_object_put_i64(root, "numberOfDeathsPvp", deaths_pvp)) {
        plr_json_free(root); return NULL;
    }

    const char *colors[] = {"hairColor", "skinColor", "eyeColor", "shirtColor",
        "underShirtColor", "pantsColor", "shoeColor"};
    for (uint32_t i = 0u; i < 7u; i++) {
        PlrJsonValue *color = plr_read_color(&reader);
        if (!color || !plr_root_put(root, colors[i], color)) {
            plr_json_free(color); plr_json_free(root); return NULL;
        }
    }

    PlrJsonValue *armor = plr_make_default_item_array(PLR_ARMOR_SLOTS);
    PlrJsonValue *dyes = plr_make_default_item_array(PLR_DYE_SLOTS);
    PlrJsonValue *inventory = plr_make_default_item_array(PLR_INVENTORY_SLOTS);
    PlrJsonValue *misc_equips = plr_make_default_item_array(PLR_MISC_SLOTS);
    PlrJsonValue *misc_dyes = plr_make_default_item_array(PLR_MISC_SLOTS);
    PlrJsonValue *piggy = plr_make_default_item_array(PLR_BANK_SLOTS);
    PlrJsonValue *safe = plr_make_default_item_array(PLR_BANK_SLOTS);
    PlrJsonValue *forge = plr_make_default_item_array(PLR_BANK_SLOTS);
    PlrJsonValue *vault = plr_make_default_item_array(PLR_BANK_SLOTS);
    if (!armor || !dyes || !inventory || !misc_equips || !misc_dyes ||
        !piggy || !safe || !forge || !vault) goto parse_items_fail;

    if (plr_version_has_numeric_items(version)) {
        uint32_t armor_count = plr_version_armor_slots(version);
        for (uint32_t i = 0u; i < armor_count; i++)
            if (!plr_array_replace_owned(armor, i,
                    plr_read_type_prefix_item(&reader, plr_version_has_equipment_favorites(version))))
                goto parse_items_fail;
        uint32_t dye_count = plr_version_dye_slots(version);
        for (uint32_t i = 0u; i < dye_count; i++)
            if (!plr_array_replace_owned(dyes, i,
                    plr_read_type_prefix_item(&reader, plr_version_has_equipment_favorites(version))))
                goto parse_items_fail;
        uint32_t inventory_count = plr_version_inventory_disk_slots(version);
        for (uint32_t disk = 0u; disk < inventory_count; disk++) {
            uint32_t model_index = version < 58 && disk >= 40u ? disk + 10u : disk;
            if (!plr_array_replace_owned(inventory, model_index,
                    plr_read_full_item(&reader, plr_version_has_inventory_favorites(version))))
                goto parse_items_fail;
        }
        if (plr_version_has_misc_equips(version)) {
            for (uint32_t i = 0u; i < PLR_MISC_SLOTS; i++) {
                if (version < 136 && i == 1u) continue;
                if (!plr_array_replace_owned(misc_equips, i, plr_read_type_prefix_item(&reader, 0)) ||
                    !plr_array_replace_owned(misc_dyes, i, plr_read_type_prefix_item(&reader, 0)))
                    goto parse_items_fail;
            }
        }
        uint32_t bank_count = plr_version_bank_slots(version);
        for (uint32_t i = 0u; i < bank_count; i++)
            if (!plr_array_replace_owned(piggy, i, plr_read_full_item(&reader, 0))) goto parse_items_fail;
        for (uint32_t i = 0u; i < bank_count; i++)
            if (!plr_array_replace_owned(safe, i, plr_read_full_item(&reader, 0))) goto parse_items_fail;
        if (plr_version_has_forge(version))
            for (uint32_t i = 0u; i < PLR_BANK_SLOTS; i++)
                if (!plr_array_replace_owned(forge, i, plr_read_full_item(&reader, 0))) goto parse_items_fail;
        if (plr_version_has_void_vault(version))
            for (uint32_t i = 0u; i < PLR_BANK_SLOTS; i++)
                if (!plr_array_replace_owned(vault, i,
                        plr_read_full_item(&reader, plr_version_has_void_favorites(version))))
                    goto parse_items_fail;
    } else {
        for (uint32_t i = 0u; i < 8u; i++)
            if (!plr_array_replace_owned(armor, i, plr_read_legacy_item(&reader, version, 0))) goto parse_items_fail;
        if (version >= 6)
            for (uint32_t i = 10u; i < 13u; i++)
                if (!plr_array_replace_owned(armor, i, plr_read_legacy_item(&reader, version, 0))) goto parse_items_fail;
        uint32_t legacy_inventory = version >= 15 ? 48u : 44u;
        for (uint32_t disk = 0u; disk < legacy_inventory; disk++) {
            uint32_t model_index = disk >= 40u ? disk + 10u : disk;
            if (!plr_array_replace_owned(inventory, model_index,
                    plr_read_legacy_item(&reader, version, 1))) goto parse_items_fail;
        }
        for (uint32_t i = 0u; i < 20u; i++)
            if (!plr_array_replace_owned(piggy, i, plr_read_legacy_item(&reader, version, 1))) goto parse_items_fail;
        if (version >= 20)
            for (uint32_t i = 0u; i < 20u; i++)
                if (!plr_array_replace_owned(safe, i, plr_read_legacy_item(&reader, version, 1))) goto parse_items_fail;
    }
    if (!reader.ok) goto parse_items_fail;
    if (!plr_root_put(root, "armor", armor) || !plr_root_put(root, "dyes", dyes) ||
        !plr_root_put(root, "inventory", inventory) ||
        !plr_root_put(root, "miscEquips", misc_equips) ||
        !plr_root_put(root, "miscDyes", misc_dyes) ||
        !plr_root_put(root, "piggyBank", piggy) || !plr_root_put(root, "safe", safe) ||
        !plr_root_put(root, "defendersForge", forge) || !plr_root_put(root, "voidVault", vault)) {
        plr_json_free(root); return NULL;
    }
    armor = dyes = inventory = misc_equips = misc_dyes = piggy = safe = forge = vault = NULL;
    if (!plr_json_object_put_u64(root, "voidVaultInfo",
            plr_version_has_void_info(version) ? plr_read_u8(&reader) : 0u) || !reader.ok) {
        plr_json_free(root); return NULL;
    }

    PlrJsonValue *buffs = plr_read_buffs(&reader, plr_version_buff_slots(version));
    if (!buffs || !plr_root_put(root, "buffs", buffs)) {
        plr_json_free(buffs); plr_json_free(root); return NULL;
    }
    PlrJsonValue *spawn_points = plr_read_spawn_points(&reader);
    if (!spawn_points || !plr_root_put(root, "spawnPoints", spawn_points)) {
        plr_json_free(spawn_points); plr_json_free(root); return NULL;
    }
    int hb_locked = plr_version_has_hb_locked(version) ? (plr_read_u8(&reader) != 0u) : 0;
    PlrJsonValue *hide_info = plr_version_has_hide_info(version) ?
        plr_read_bool_array(&reader, PLR_HIDE_INFO_SLOTS) :
        plr_make_default_bool_array(PLR_HIDE_INFO_SLOTS);
    int32_t angler = plr_version_has_angler(version) ? plr_read_i32(&reader) : 0;
    PlrJsonValue *dpad = plr_version_has_dpad(version) ?
        plr_read_i32_array(&reader, PLR_DPAD_SLOTS) : plr_make_default_i32_array(PLR_DPAD_SLOTS);
    uint32_t builder_count = plr_version_builder_slots(version);
    PlrJsonValue *builder = builder_count ? plr_read_i32_array(&reader, builder_count) :
        plr_make_default_i32_array(PLR_BUILDER_STATUS_SLOTS);
    int32_t bartender = plr_version_has_bartender(version) ? plr_read_i32(&reader) : 0;
    int dead = plr_version_has_death_metadata(version) ? (plr_read_u8(&reader) != 0u) : 0;
    PlrJsonValue *respawn = dead ? plr_json_i64(plr_read_i32(&reader)) : plr_json_null();
    int64_t last_save = plr_version_has_last_save(version) ? plr_read_i64(&reader) : 0;
    int32_t golfer = plr_version_has_golfer_score(version) ? plr_read_i32(&reader) : 0;
    if (!hide_info || !dpad || !builder || !respawn || !reader.ok ||
        !plr_json_object_put_bool(root, "hbLocked", hb_locked) ||
        !plr_root_put(root, "hideInfo", hide_info) ||
        !plr_json_object_put_i64(root, "anglerQuestsFinished", angler) ||
        !plr_root_put(root, "dpadRadialBindings", dpad) ||
        !plr_root_put(root, "builderAccStatus", builder) ||
        !plr_json_object_put_i64(root, "bartenderQuestLog", bartender) ||
        !plr_json_object_put_bool(root, "dead", dead) ||
        !plr_root_put(root, "respawnTimer", respawn) ||
        !plr_json_object_put_i64(root, "lastSaveUtcTicks", last_save) ||
        !plr_json_object_put_i64(root, "golferScoreAccumulated", golfer)) {
        plr_json_free(hide_info); plr_json_free(dpad); plr_json_free(builder);
        plr_json_free(respawn); plr_json_free(root); return NULL;
    }

    PlrJsonValue *sacrifices = plr_version_has_creative_tracker(version) ?
        plr_read_sacrifices(&reader, version) : NULL;
    PlrJsonValue *sacrifice_items = sacrifices ? plr_json_object_get(sacrifices, "items") : NULL;
    PlrJsonValue *sacrifice_flag = sacrifices ? plr_json_object_get(sacrifices, "hasNewUnlocks") : NULL;
    if (sacrifices) {
        if (!sacrifice_items || !sacrifice_flag ||
            !plr_root_put(root, "creativeItemSacrifices", plr_json_clone(sacrifice_items)) ||
            !plr_root_put(root, "creativeTrackerHasNewUnlocks", plr_json_clone(sacrifice_flag))) {
            plr_json_free(sacrifices); plr_json_free(root); return NULL;
        }
        plr_json_free(sacrifices);
    } else {
        if (!plr_root_put(root, "creativeItemSacrifices", plr_json_array()) ||
            !plr_json_object_put_bool(root, "creativeTrackerHasNewUnlocks", 0)) {
            plr_json_free(root); return NULL;
        }
    }

    PlrJsonValue *temporary = plr_version_has_temporary_slots(version) ?
        plr_read_temporary_slots(&reader) : plr_json_array();
    if (!temporary) { plr_json_free(root); return NULL; }
    if (!plr_version_has_temporary_slots(version)) {
        for (uint32_t i = 0u; i < PLR_TEMPORARY_SLOTS; i++)
            if (!plr_json_array_push(temporary, plr_json_null())) {
                plr_json_free(temporary); plr_json_free(root); return NULL;
            }
    }
    PlrJsonValue *powers = plr_version_has_creative_powers(version) ?
        plr_read_creative_powers(&reader) : plr_json_object();
    if (!powers) { plr_json_free(temporary); plr_json_free(root); return NULL; }
    if (!plr_version_has_creative_powers(version) &&
        (!plr_json_object_put_bool(powers, "farPlacementEnabled", 0) ||
         !plr_json_object_put_bool(powers, "godmodeEnabled", 0) ||
         !plr_json_object_put_float(powers, "spawnRateSlider", 0.0))) {
        plr_json_free(temporary); plr_json_free(powers); plr_json_free(root); return NULL;
    }
    uint8_t super_flags = plr_version_has_super_cart(version) ? plr_read_u8(&reader) : 0u;
    int32_t loadout_index = plr_version_has_loadouts(version) ? plr_read_i32(&reader) : 0;
    PlrJsonValue *loadouts = NULL;
    if (plr_version_has_loadouts(version)) loadouts = plr_read_loadouts(
        &reader, plr_version_has_equipment_favorites(version));
    else {
        loadouts = plr_json_array();
        for (uint32_t i = 0u; loadouts && i < PLR_LOADOUTS; i++) {
            PlrJsonValue *loadout = plr_json_object();
            PlrJsonValue *a = plr_make_default_item_array(PLR_ARMOR_SLOTS);
            PlrJsonValue *d = plr_make_default_item_array(PLR_DYE_SLOTS);
            PlrJsonValue *h = plr_make_default_bool_array(PLR_DYE_SLOTS);
            if (!loadout || !a || !d || !h || !plr_root_put(loadout, "armor", a) ||
                !plr_root_put(loadout, "dyes", d) || !plr_root_put(loadout, "hide", h) ||
                !plr_json_array_push(loadouts, loadout)) {
                plr_json_free(loadout); plr_json_free(a); plr_json_free(d); plr_json_free(h);
                plr_json_free(loadouts); loadouts = NULL; break;
            }
        }
    }
    uint8_t voice_variant = plr_version_has_voice_variant(version) ? plr_read_u8(&reader) :
        (plr_skin_variant_is_male(skin_variant) ? 1u : 2u);
    float voice_pitch = plr_version_has_voice_pitch(version) ? plr_read_f32(&reader) : 0.0f;
    if (!temporary || !powers || !loadouts || !reader.ok ||
        !plr_root_put(root, "temporarySlots", temporary) ||
        !plr_root_put(root, "creativePowers", powers) ||
        !plr_json_object_put_bool(root, "unlockedSuperCart", (super_flags & 1u) != 0u) ||
        !plr_json_object_put_bool(root, "enabledSuperCart", (super_flags & 2u) != 0u) ||
        !plr_json_object_put_i64(root, "currentLoadoutIndex", loadout_index) ||
        !plr_root_put(root, "loadouts", loadouts) ||
        !plr_json_object_put_u64(root, "voiceVariant", voice_variant) ||
        !plr_json_object_put_float(root, "voicePitchOffset", voice_pitch)) {
        plr_json_free(temporary); plr_json_free(powers); plr_json_free(loadouts);
        plr_json_free(root); return NULL;
    }

    uint32_t pending_count = 0u;
    if (plr_version_has_pending_refunds(version) &&
        !plr_read_count(&reader, PLR_MAX_PENDING_REFUNDS, 9u, &pending_count)) {
        plr_json_free(root); return NULL;
    }
    PlrJsonValue *pending = plr_read_item_array(&reader, pending_count, 0, 0);
    PlrJsonValue *dialogues = plr_version_has_dialogues(version) ?
        plr_read_dialogues(&reader) : plr_json_array();
    PlrJsonValue *layout = plr_json_object();
    if (!pending || !dialogues || !layout ||
        !plr_root_put(root, "pendingRefunds", pending) ||
        !plr_root_put(root, "oneTimeDialoguesSeen", dialogues) ||
        !plr_json_object_put_u64(layout, "builderAccStatusCount", builder_count) ||
        !plr_json_object_put_bool(layout, "includesDeathMetadata", plr_version_has_death_metadata(version)) ||
        !plr_root_put(root, "tailLayout", layout)) {
        plr_json_free(pending); plr_json_free(dialogues); plr_json_free(layout);
        plr_json_free(root); return NULL;
    }
    if (!reader.ok || reader.offset != reader.length) {
        plr_json_free(root); return NULL;
    }
    return root;

parse_items_fail:
    plr_json_free(armor); plr_json_free(dyes); plr_json_free(inventory);
    plr_json_free(misc_equips); plr_json_free(misc_dyes); plr_json_free(piggy);
    plr_json_free(safe); plr_json_free(forge); plr_json_free(vault);
    plr_json_free(root);
    return NULL;
}
'''
text, n = re.subn(
    r'static PlrJsonValue \*plr_parse_plain\(.*?(?=/\* -------------------------------------------------------------------------\n \* Semantic model validation)',
    PARSE + "\n", text, count=1, flags=re.S)
assert n == 1, "parse function not replaced"

text = text.replace(
    'if (!plr_version_supported(model_version))\n        return plr_model_error("PLR version must be within TerraWasm\'s modern Terraria 280-326 range");',
    'if (!plr_version_supported(model_version))\n        return plr_model_error("PLR version must be a positive Terraria release number");', 1)

# Add legacy item writer and make creative tracker version-aware.
insert_at = 'static void plr_writer_item_array(\n'
LEGACY_WRITER = r'''static void plr_writer_legacy_item(
    PlrWriter *writer, const PlrJsonValue *item, int32_t version, int with_stack) {
    int32_t item_type = 0, stack = 0;
    uint8_t prefix = 0u;
    const PlrJsonValue *legacy = plr_field(item, "legacyName");
    const char *legacy_name = NULL;
    if (!plr_field_i32(item, "itemType", &item_type) ||
        !plr_field_i32(item, "stack", &stack) || !plr_field_u8(item, "prefix", &prefix)) {
        writer->ok = 0; return;
    }
    if (legacy && !plr_value_string(legacy, &legacy_name)) { writer->ok = 0; return; }
    if (!legacy_name) {
        if (item_type != 0) {
            tx_set_error("TERRAX_PLR_LEGACY_ITEM_NAME_REQUIRED",
                "release 1-37 item edits require legacyName because the file stores item names, not ids");
            writer->ok = 0; return;
        }
        legacy_name = "";
    }
    plr_writer_string(writer, legacy_name);
    if (with_stack) plr_writer_i32(writer, stack);
    if (version >= 36) plr_writer_u8(writer, prefix);
}

'''
text = text.replace(insert_at, LEGACY_WRITER + insert_at, 1)
text = text.replace(
    'static void plr_writer_sacrifices(PlrWriter *writer, const PlrJsonValue *root) {',
    'static void plr_writer_sacrifices(PlrWriter *writer, const PlrJsonValue *root, int32_t version) {', 1)
text = text.replace(
    '    plr_writer_u8(writer, has_new_unlocks ? 1u : 0u);\n    plr_writer_i32(writer, (int32_t)array->as.array.count);',
    '    if (plr_version_has_tracker_new_unlock_flag(version))\n        plr_writer_u8(writer, has_new_unlocks ? 1u : 0u);\n    plr_writer_i32(writer, (int32_t)array->as.array.count);', 1)

ENCODE = r'''static uint8_t *plr_encode_plain(
    const PlrJsonValue *root, uint32_t *out_length) {
    if (out_length) *out_length = 0u;
    if (!plr_validate_model(root)) return NULL;
    PlrWriter writer = {NULL, 0u, 0u, 1};
    int32_t version = 0;
    if (!plr_field_i32(root, "version", &version) || !plr_version_supported(version)) return NULL;
    plr_writer_i32(&writer, version);

    const PlrJsonValue *metadata = plr_field(root, "metadata");
    if (plr_version_has_metadata(version)) {
        uint64_t magic_and_type = PLR_DEFAULT_MAGIC_AND_TYPE, favorite_flags = 0u;
        uint32_t revision = 0u;
        if (metadata && metadata->type != PLR_JSON_NULL &&
            (!plr_field_u64(metadata, "magicAndType", &magic_and_type) ||
             !plr_field_u32(metadata, "revision", &revision) ||
             !plr_field_u64(metadata, "favoriteFlags", &favorite_flags))) writer.ok = 0;
        plr_writer_u64(&writer, magic_and_type);
        plr_writer_u32(&writer, revision);
        plr_writer_u64(&writer, favorite_flags);
    }

    const char *name = NULL;
    int32_t i32 = 0;
    int64_t i64 = 0;
    uint8_t u8 = 0;
    int boolean = 0;
    if (!plr_field_string(root, "name", &name)) writer.ok = 0;
    plr_writer_string(&writer, name ? name : "");
    if (!plr_field_u8(root, "difficulty", &u8)) writer.ok = 0;
    if (plr_version_has_difficulty(version)) {
        if (plr_version_has_byte_difficulty(version)) plr_writer_u8(&writer, u8);
        else plr_writer_u8(&writer, u8 == 2u ? 1u : 0u);
    }
    if (!plr_field_i64(root, "playTimeTicks", &i64)) writer.ok = 0;
    if (plr_version_has_play_time(version)) plr_writer_i64(&writer, i64);
    if (!plr_field_i32(root, "hair", &i32)) writer.ok = 0;
    plr_writer_i32(&writer, i32);
    if (!plr_field_u8(root, "hairDye", &u8)) writer.ok = 0;
    if (plr_version_has_hair_dye(version)) plr_writer_u8(&writer, u8);
    if (!plr_field_u8(root, "team", &u8)) writer.ok = 0;
    if (plr_version_has_team(version)) plr_writer_u8(&writer, u8);

    const PlrJsonValue *hide = plr_field_array(root, "hideVisibleAccessory");
    uint8_t hide_lower = 0u, hide_upper = 0u;
    if (!hide || hide->as.array.count != PLR_DYE_SLOTS) writer.ok = 0;
    else for (uint32_t index = 0u; index < PLR_DYE_SLOTS; index++) {
        if (!plr_value_bool(hide->as.array.items[index], &boolean)) writer.ok = 0;
        else if (boolean && index < 8u) hide_lower |= (uint8_t)(1u << index);
        else if (boolean) hide_upper |= (uint8_t)(1u << (index - 8u));
    }
    if (plr_version_has_hide_lower(version)) plr_writer_u8(&writer, hide_lower);
    if (plr_version_has_hide_upper(version)) plr_writer_u8(&writer, hide_upper);
    if (!plr_field_u8(root, "hideMisc", &u8)) writer.ok = 0;
    if (plr_version_has_hide_misc(version)) plr_writer_u8(&writer, u8);
    uint8_t skin_variant = 0u;
    if (!plr_field_u8(root, "skinVariant", &skin_variant)) writer.ok = 0;
    if (plr_version_has_skin_variant(version)) plr_writer_u8(&writer, skin_variant);
    else if (plr_version_has_gender_bool(version))
        plr_writer_u8(&writer, plr_skin_variant_is_male(skin_variant) ? 1u : 0u);

    const char *stats[] = {"statLife", "statLifeMax", "statMana", "statManaMax"};
    for (uint32_t i = 0u; i < 4u; i++) {
        if (!plr_field_i32(root, stats[i], &i32)) writer.ok = 0;
        plr_writer_i32(&writer, i32);
    }
    if (!plr_field_bool(root, "extraAccessory", &boolean)) writer.ok = 0;
    if (plr_version_has_extra_accessory(version)) plr_writer_u8(&writer, boolean ? 1u : 0u);
    if (plr_version_has_biome_torches(version)) {
        if (!plr_field_bool(root, "unlockedBiomeTorches", &boolean)) writer.ok = 0;
        plr_writer_u8(&writer, boolean ? 1u : 0u);
        if (!plr_field_bool(root, "usingBiomeTorches", &boolean)) writer.ok = 0;
        plr_writer_u8(&writer, boolean ? 1u : 0u);
        if (plr_version_has_artisan_bread(version)) {
            if (!plr_field_bool(root, "ateArtisanBread", &boolean)) writer.ok = 0;
            plr_writer_u8(&writer, boolean ? 1u : 0u);
        }
        if (plr_version_has_reserved_324(version)) plr_writer_u8(&writer, 0u);
        if (plr_version_has_permanent_upgrades(version)) {
            const char *upgrades[] = {"usedAegisCrystal", "usedAegisFruit", "usedArcaneCrystal",
                "usedGalaxyPearl", "usedGummyWorm", "usedAmbrosia"};
            for (uint32_t i = 0u; i < 6u; i++) {
                if (!plr_field_bool(root, upgrades[i], &boolean)) writer.ok = 0;
                plr_writer_u8(&writer, boolean ? 1u : 0u);
            }
        }
    }
    if (!plr_field_bool(root, "downedDd2EventAnyDifficulty", &boolean)) writer.ok = 0;
    if (plr_version_has_dd2_flag(version)) plr_writer_u8(&writer, boolean ? 1u : 0u);
    if (!plr_field_i32(root, "taxMoney", &i32)) writer.ok = 0;
    if (plr_version_has_tax_money(version)) plr_writer_i32(&writer, i32);
    if (!plr_field_i32(root, "numberOfDeathsPve", &i32)) writer.ok = 0;
    if (plr_version_has_death_counts(version)) plr_writer_i32(&writer, i32);
    if (!plr_field_i32(root, "numberOfDeathsPvp", &i32)) writer.ok = 0;
    if (plr_version_has_death_counts(version)) plr_writer_i32(&writer, i32);

    const char *colors[] = {"hairColor", "skinColor", "eyeColor", "shirtColor",
        "underShirtColor", "pantsColor", "shoeColor"};
    for (uint32_t i = 0u; i < 7u; i++) plr_writer_color(&writer, root, colors[i]);

    const PlrJsonValue *armor = plr_field_array(root, "armor");
    const PlrJsonValue *dyes = plr_field_array(root, "dyes");
    const PlrJsonValue *inventory = plr_field_array(root, "inventory");
    const PlrJsonValue *misc_equips = plr_field_array(root, "miscEquips");
    const PlrJsonValue *misc_dyes = plr_field_array(root, "miscDyes");
    const PlrJsonValue *piggy = plr_field_array(root, "piggyBank");
    const PlrJsonValue *safe = plr_field_array(root, "safe");
    const PlrJsonValue *forge = plr_field_array(root, "defendersForge");
    const PlrJsonValue *vault = plr_field_array(root, "voidVault");
    if (!armor || !dyes || !inventory || !misc_equips || !misc_dyes || !piggy || !safe || !forge || !vault)
        writer.ok = 0;
    else if (plr_version_has_numeric_items(version)) {
        for (uint32_t i = 0u; i < plr_version_armor_slots(version); i++)
            plr_writer_type_prefix_item(&writer, armor->as.array.items[i], plr_version_has_equipment_favorites(version));
        for (uint32_t i = 0u; i < plr_version_dye_slots(version); i++)
            plr_writer_type_prefix_item(&writer, dyes->as.array.items[i], plr_version_has_equipment_favorites(version));
        for (uint32_t disk = 0u; disk < plr_version_inventory_disk_slots(version); disk++) {
            uint32_t model_index = version < 58 && disk >= 40u ? disk + 10u : disk;
            plr_writer_item(&writer, inventory->as.array.items[model_index], plr_version_has_inventory_favorites(version));
        }
        if (plr_version_has_misc_equips(version))
            for (uint32_t i = 0u; i < PLR_MISC_SLOTS; i++) {
                if (version < 136 && i == 1u) continue;
                plr_writer_type_prefix_item(&writer, misc_equips->as.array.items[i], 0);
                plr_writer_type_prefix_item(&writer, misc_dyes->as.array.items[i], 0);
            }
        for (uint32_t i = 0u; i < plr_version_bank_slots(version); i++)
            plr_writer_item(&writer, piggy->as.array.items[i], 0);
        for (uint32_t i = 0u; i < plr_version_bank_slots(version); i++)
            plr_writer_item(&writer, safe->as.array.items[i], 0);
        if (plr_version_has_forge(version))
            for (uint32_t i = 0u; i < PLR_BANK_SLOTS; i++) plr_writer_item(&writer, forge->as.array.items[i], 0);
        if (plr_version_has_void_vault(version))
            for (uint32_t i = 0u; i < PLR_BANK_SLOTS; i++)
                plr_writer_item(&writer, vault->as.array.items[i], plr_version_has_void_favorites(version));
    } else {
        for (uint32_t i = 0u; i < 8u; i++) plr_writer_legacy_item(&writer, armor->as.array.items[i], version, 0);
        if (version >= 6)
            for (uint32_t i = 10u; i < 13u; i++) plr_writer_legacy_item(&writer, armor->as.array.items[i], version, 0);
        uint32_t legacy_inventory = version >= 15 ? 48u : 44u;
        for (uint32_t disk = 0u; disk < legacy_inventory; disk++) {
            uint32_t model_index = disk >= 40u ? disk + 10u : disk;
            plr_writer_legacy_item(&writer, inventory->as.array.items[model_index], version, 1);
        }
        for (uint32_t i = 0u; i < 20u; i++) plr_writer_legacy_item(&writer, piggy->as.array.items[i], version, 1);
        if (version >= 20)
            for (uint32_t i = 0u; i < 20u; i++) plr_writer_legacy_item(&writer, safe->as.array.items[i], version, 1);
    }
    if (!plr_field_u8(root, "voidVaultInfo", &u8)) writer.ok = 0;
    if (plr_version_has_void_info(version)) plr_writer_u8(&writer, u8);

    const PlrJsonValue *buffs = plr_field_array(root, "buffs");
    uint32_t buff_count = plr_version_buff_slots(version);
    if (!buffs || buffs->as.array.count != PLR_BUFF_SLOTS) writer.ok = 0;
    else for (uint32_t i = 0u; i < buff_count; i++) {
        if (!plr_field_i32(buffs->as.array.items[i], "buffType", &i32)) writer.ok = 0;
        plr_writer_i32(&writer, i32);
        if (!plr_field_i32(buffs->as.array.items[i], "buffTime", &i32)) writer.ok = 0;
        plr_writer_i32(&writer, i32);
    }
    plr_writer_spawn_points(&writer, root);
    if (!plr_field_bool(root, "hbLocked", &boolean)) writer.ok = 0;
    if (plr_version_has_hb_locked(version)) plr_writer_u8(&writer, boolean ? 1u : 0u);
    if (plr_version_has_hide_info(version)) plr_writer_bool_array(&writer, root, "hideInfo", PLR_HIDE_INFO_SLOTS);
    if (!plr_field_i32(root, "anglerQuestsFinished", &i32)) writer.ok = 0;
    if (plr_version_has_angler(version)) plr_writer_i32(&writer, i32);
    if (plr_version_has_dpad(version)) plr_writer_i32_array(&writer, root, "dpadRadialBindings", PLR_DPAD_SLOTS);
    uint32_t builder_count = plr_version_builder_slots(version);
    if (builder_count) plr_writer_i32_array(&writer, root, "builderAccStatus", builder_count);
    if (!plr_field_i32(root, "bartenderQuestLog", &i32)) writer.ok = 0;
    if (plr_version_has_bartender(version)) plr_writer_i32(&writer, i32);
    if (!plr_field_bool(root, "dead", &boolean)) writer.ok = 0;
    if (plr_version_has_death_metadata(version)) {
        plr_writer_u8(&writer, boolean ? 1u : 0u);
        if (boolean) {
            const PlrJsonValue *respawn = plr_field(root, "respawnTimer");
            if (!respawn || (respawn->type != PLR_JSON_NULL && !plr_value_i32(respawn, &i32))) writer.ok = 0;
            if (respawn && respawn->type == PLR_JSON_NULL) i32 = 0;
            plr_writer_i32(&writer, i32);
        }
    }
    if (!plr_field_i64(root, "lastSaveUtcTicks", &i64)) writer.ok = 0;
    if (plr_version_has_last_save(version)) plr_writer_i64(&writer, i64);
    if (!plr_field_i32(root, "golferScoreAccumulated", &i32)) writer.ok = 0;
    if (plr_version_has_golfer_score(version)) plr_writer_i32(&writer, i32);
    if (plr_version_has_creative_tracker(version)) plr_writer_sacrifices(&writer, root, version);
    if (plr_version_has_temporary_slots(version)) plr_writer_temporary_slots(&writer, root);
    if (plr_version_has_creative_powers(version)) plr_writer_creative_powers(&writer, root);
    if (plr_version_has_super_cart(version)) {
        int unlocked = 0, enabled = 0;
        if (!plr_field_bool(root, "unlockedSuperCart", &unlocked) ||
            !plr_field_bool(root, "enabledSuperCart", &enabled)) writer.ok = 0;
        plr_writer_u8(&writer, (uint8_t)((unlocked ? 1u : 0u) | (enabled ? 2u : 0u)));
    }
    if (plr_version_has_loadouts(version)) {
        if (!plr_field_i32(root, "currentLoadoutIndex", &i32)) writer.ok = 0;
        plr_writer_i32(&writer, i32);
        plr_writer_loadouts(&writer, root, plr_version_has_equipment_favorites(version));
    }
    if (!plr_field_u8(root, "voiceVariant", &u8)) writer.ok = 0;
    if (plr_version_has_voice_variant(version)) plr_writer_u8(&writer, u8);
    float voice_pitch = 0.0f;
    if (!plr_field_float(root, "voicePitchOffset", &voice_pitch)) writer.ok = 0;
    if (plr_version_has_voice_pitch(version)) plr_writer_f32(&writer, voice_pitch);
    const PlrJsonValue *pending = plr_field_array(root, "pendingRefunds");
    if (!pending || pending->as.array.count > PLR_MAX_PENDING_REFUNDS || pending->as.array.count > INT32_MAX)
        writer.ok = 0;
    else if (plr_version_has_pending_refunds(version)) {
        plr_writer_i32(&writer, (int32_t)pending->as.array.count);
        for (uint32_t i = 0u; i < pending->as.array.count; i++) plr_writer_item(&writer, pending->as.array.items[i], 0);
    }
    const PlrJsonValue *dialogues = plr_field_array(root, "oneTimeDialoguesSeen");
    if (!dialogues || dialogues->as.array.count > PLR_MAX_DIALOGUES || dialogues->as.array.count > INT32_MAX)
        writer.ok = 0;
    else if (plr_version_has_dialogues(version)) {
        plr_writer_i32(&writer, (int32_t)dialogues->as.array.count);
        for (uint32_t i = 0u; i < dialogues->as.array.count; i++) {
            const char *dialogue = NULL;
            if (!plr_value_string(dialogues->as.array.items[i], &dialogue)) writer.ok = 0;
            plr_writer_string(&writer, dialogue ? dialogue : "");
        }
    }
    if (!writer.ok || writer.length == 0u) { free(writer.data); return NULL; }
    if (out_length) *out_length = writer.length;
    return writer.data;
}
'''
text, n = re.subn(
    r'static uint8_t \*plr_encode_plain\(.*?(?=/\* -------------------------------------------------------------------------\n \* RFC 6901 JSON Pointer editing)',
    ENCODE + "\n", text, count=1, flags=re.S)
assert n == 1, "encode function not replaced"

# Future-version parse errors are diagnostic rather than a numeric rejection.
text = text.replace(
'''static terrax_world_status plr_parse_error(void) {
    return plr_status_error(
        g_plr_oom ? TERRAX_WORLD_STATUS_INTERNAL_ERROR : TERRAX_WORLD_STATUS_PARSE_ERROR,
        g_plr_oom ? "TERRAX_WASM_OOM" : "TERRAX_PLR_PARSE_ERROR",
        g_plr_oom ? "out of memory while parsing PLR" :
            "PLR payload is invalid or its version uses an incompatible binary layout");
}''',
'''static terrax_world_status plr_parse_error_for_version(int32_t version) {
    if (g_plr_oom) return plr_status_error(
        TERRAX_WORLD_STATUS_INTERNAL_ERROR, "TERRAX_WASM_OOM", "out of memory while parsing PLR");
    if (version > PLR_CURRENT_KNOWN_VERSION) return plr_status_error(
        TERRAX_WORLD_STATUS_PARSE_ERROR, "TERRAX_PLR_NEWER_LAYOUT_ERROR",
        "PLR release is newer than the known release 326 layout and could not be parsed with that layout");
    return plr_status_error(
        TERRAX_WORLD_STATUS_PARSE_ERROR, "TERRAX_PLR_PARSE_ERROR",
        "PLR payload is invalid or its historical binary layout could not be parsed");
}

static terrax_world_status plr_parse_error(void) {
    return plr_parse_error_for_version(0);
}''', 1)
text = text.replace(
'''    PlrJsonValue *root = plr_parse_plain(plain, plain_length);
    free(plain);
    if (!root) {
        free(original);
        return plr_parse_error();
    }''',
'''    int32_t detected_version = 0;
    if (plain_length >= 4u) detected_version = (int32_t)(
        (uint32_t)plain[0] | ((uint32_t)plain[1] << 8u) |
        ((uint32_t)plain[2] << 16u) | ((uint32_t)plain[3] << 24u));
    PlrJsonValue *root = plr_parse_plain(plain, plain_length);
    free(plain);
    if (!root) {
        free(original);
        return plr_parse_error_for_version(detected_version);
    }''', 1)

PLR.write_text(text, encoding="utf-8")

# Replace the narrow modern-history test with exhaustive history + future-layout checks.
t = TEST.read_text(encoding="utf-8")
if 'const crypto = require("node:crypto");' not in t:
    t = t.replace('const assert = require("node:assert/strict");',
                  'const assert = require("node:assert/strict");\nconst crypto = require("node:crypto");', 1)

NEW_TESTS = r'''function makeAirItem() {
  return { itemType: 0, stack: 0, prefix: 0, favorited: false };
}

function historicalModel(version) {
  const model = JSON.parse(fs.readFileSync(MODEL_FIXTURE, "utf8"));
  model.version = version;
  if (version < 135) model.metadata = null;
  const clearItems = (items) => items.map(() => makeAirItem());
  model.armor = clearItems(model.armor);
  model.dyes = clearItems(model.dyes);
  model.inventory = clearItems(model.inventory);
  model.miscEquips = clearItems(model.miscEquips);
  model.miscDyes = clearItems(model.miscDyes);
  model.piggyBank = clearItems(model.piggyBank);
  model.safe = clearItems(model.safe);
  model.defendersForge = clearItems(model.defendersForge);
  model.voidVault = clearItems(model.voidVault);
  model.pendingRefunds = [];
  model.oneTimeDialoguesSeen = [];
  model.creativeItemSacrifices = [];
  model.creativeTrackerHasNewUnlocks = false;
  model.temporarySlots = [null, null, null, null];
  for (const loadout of model.loadouts) {
    loadout.armor = clearItems(loadout.armor);
    loadout.dyes = clearItems(loadout.dyes);
    loadout.hide = loadout.hide.map(() => false);
  }
  model.buffs = model.buffs.map(() => ({ buffType: 0, buffTime: 0 }));
  const builderCount = version < 164 ? 0 : version < 167 ? 8 : version < 197 ? 10 : version < 230 ? 11 : 12;
  model.tailLayout.builderAccStatusCount = builderCount;
  model.tailLayout.includesDeathMetadata = version >= 200;
  model.builderAccStatus = Array(builderCount || 12).fill(0);
  return model;
}

function decryptPlr(bytes) {
  const key = Buffer.from([104,0,51,0,121,0,95,0,103,0,85,0,121,0,90,0]);
  const decipher = crypto.createDecipheriv("aes-128-cbc", key, key);
  return Buffer.concat([decipher.update(bytes), decipher.final()]);
}

function encryptPlr(plain) {
  const key = Buffer.from([104,0,51,0,121,0,95,0,103,0,85,0,121,0,90,0]);
  const cipher = crypto.createCipheriv("aes-128-cbc", key, key);
  return Buffer.concat([cipher.update(plain), cipher.final()]);
}

function lastError(module) {
  return twoCallJson(module, module._terra_info_get_last_error_json, []);
}

test("PLR recognizes every historical Terraria release 1-326", async () => {
  const module = await TerraWorldWasm();
  const baseline = module._tx_heap_used();
  for (let version = 1; version <= 326; version++) {
    let handle = 0;
    let reopened = 0;
    try {
      handle = openJson(module, historicalModel(version));
      const encoded = encode(module, handle);
      reopened = openBuffer(module, encoded);
      assert.equal(getField(module, reopened, "/version"), version, `release ${version}`);
      assert.equal(typeof getField(module, reopened, "/name"), "string", `release ${version}`);
    } finally {
      if (reopened) assert.equal(module._terra_player_close(reopened), 0);
      if (handle) assert.equal(module._terra_player_close(handle), 0);
    }
    assert.equal(module._tx_heap_used(), baseline, `release ${version} leaked tracked allocations`);
  }
});

test("PLR historical source gates keep old layouts aligned", async () => {
  const module = await TerraWorldWasm();
  const baseline = module._tx_heap_used();
  const versions = [
    9,10,16,17,37,38,46,47,57,58,73,74,80,81,82,83,97,98,106,107,
    113,114,116,117,118,119,123,124,125,127,128,134,135,137,138,
    161,162,163,164,166,167,180,181,182,196,197,198,199,200,201,202,
    205,206,213,214,217,218,219,220,228,229,230,251,252,253,254,255,
    256,259,260,261,262,279,280,281,282,283,299,300,309,310,321,322,323,324,325,326,
  ];
  for (const version of versions) {
    const model = historicalModel(version);
    model.difficulty = 2;
    model.playTimeTicks = 123456;
    model.hairDye = 7;
    model.team = 5;
    model.hideVisibleAccessory[0] = true;
    model.hideVisibleAccessory[8] = true;
    model.hideMisc = 3;
    model.skinVariant = 4;
    model.extraAccessory = true;
    model.unlockedBiomeTorches = true;
    model.usingBiomeTorches = true;
    model.ateArtisanBread = true;
    model.usedAegisCrystal = true;
    model.downedDd2EventAnyDifficulty = true;
    model.taxMoney = 99;
    model.numberOfDeathsPve = 7;
    model.numberOfDeathsPvp = 8;
    model.hbLocked = true;
    model.anglerQuestsFinished = 12;
    model.bartenderQuestLog = 13;
    model.dead = true;
    model.respawnTimer = 42;
    model.lastSaveUtcTicks = 987654;
    model.golferScoreAccumulated = 55;
    model.unlockedSuperCart = true;
    model.enabledSuperCart = true;
    model.currentLoadoutIndex = 2;
    model.voiceVariant = 3;
    model.voicePitchOffset = 0.25;
    model.pendingRefunds = version >= 300 ? [{ itemType: 1, stack: 2, prefix: 0, favorited: false }] : [];
    model.oneTimeDialoguesSeen = version >= 310 ? ["gate"] : [];
    let handle = 0, reopened = 0;
    try {
      handle = openJson(module, model);
      reopened = openBuffer(module, encode(module, handle));
      assert.equal(getField(module, reopened, "/difficulty"), version >= 10 ? 2 : 0);
      assert.equal(getField(module, reopened, "/playTimeTicks"), version >= 138 ? 123456 : 0);
      assert.equal(getField(module, reopened, "/hairDye"), version >= 82 ? 7 : 0);
      assert.equal(getField(module, reopened, "/team"), version >= 283 ? 5 : 0);
      assert.equal(getField(module, reopened, "/hideVisibleAccessory/0"), version >= 83);
      assert.equal(getField(module, reopened, "/hideVisibleAccessory/8"), version >= 124);
      assert.equal(getField(module, reopened, "/hideMisc"), version >= 119 ? 3 : 0);
      assert.equal(getField(module, reopened, "/extraAccessory"), version >= 125);
      assert.equal(getField(module, reopened, "/taxMoney"), version >= 128 ? 99 : 0);
      assert.equal(getField(module, reopened, "/downedDd2EventAnyDifficulty"), version >= 182);
      assert.equal(getField(module, reopened, "/numberOfDeathsPve"), version >= 254 ? 7 : 0);
      assert.equal(getField(module, reopened, "/hbLocked"), version >= 16);
      assert.equal(getField(module, reopened, "/anglerQuestsFinished"), version >= 98 ? 12 : 0);
      assert.equal(getField(module, reopened, "/bartenderQuestLog"), version >= 181 ? 13 : 0);
      assert.equal(getField(module, reopened, "/dead"), version >= 200);
      assert.equal(getField(module, reopened, "/lastSaveUtcTicks"), version >= 202 ? 987654 : 0);
      assert.equal(getField(module, reopened, "/golferScoreAccumulated"), version >= 206 ? 55 : 0);
      assert.equal(getField(module, reopened, "/unlockedSuperCart"), version >= 253);
      assert.equal(getField(module, reopened, "/currentLoadoutIndex"), version >= 262 ? 2 : 0);
      assert.equal(getField(module, reopened, "/voiceVariant"), version >= 280 ? 3 : 2);
      assert.equal(getField(module, reopened, "/voicePitchOffset"), version >= 281 ? 0.25 : 0);
      assert.equal(getField(module, reopened, "/pendingRefunds").length, version >= 300 ? 1 : 0);
      assert.equal(getField(module, reopened, "/oneTimeDialoguesSeen").length, version >= 310 ? 1 : 0);
    } finally {
      if (reopened) module._terra_player_close(reopened);
      if (handle) module._terra_player_close(handle);
    }
    assert.equal(module._tx_heap_used(), baseline, `gate release ${version} leaked`);
  }
});

test("PLR accepts newer releases when the 326 layout is unchanged and diagnoses changed layouts", async () => {
  const module = await TerraWorldWasm();
  const baseline = module._tx_heap_used();
  for (const version of [327, 400]) {
    let handle = 0, reopened = 0;
    try {
      handle = openJson(module, historicalModel(version));
      const encoded = encode(module, handle);
      reopened = openBuffer(module, encoded);
      assert.equal(getField(module, reopened, "/version"), version);
    } finally {
      if (reopened) module._terra_player_close(reopened);
      if (handle) module._terra_player_close(handle);
    }
  }

  let handle = 0;
  try {
    handle = openJson(module, historicalModel(327));
    const encoded = encode(module, handle);
    const plain = decryptPlr(encoded);
    const changedLayout = encryptPlr(Buffer.concat([plain, Buffer.from([0x7f])]));
    const input = allocBytes(module, changedLayout);
    const output = module._tx_malloc(4);
    try {
      assert.notEqual(module._terra_player_open_from_buffer(input, changedLayout.length, output), 0);
      const error = lastError(module);
      assert.equal(error.code, "TERRAX_PLR_NEWER_LAYOUT_ERROR");
      assert.match(error.message, /newer.*326.*layout/i);
    } finally {
      module._tx_free(output);
      module._tx_free(input);
    }
  } finally {
    if (handle) module._terra_player_close(handle);
  }
  assert.equal(module._tx_heap_used(), baseline);
});
'''

t, n = re.subn(r'test\("PLR releases 280-326 follow Terraria\'s modern history gates".*\Z', NEW_TESTS + "\n", t, count=1, flags=re.S)
assert n == 1, "old history test not found"
TEST.write_text(t, encoding="utf-8")

# Keep public compatibility text aligned with the new policy.
for path in (README, FIXTURE_README):
    if not path.exists():
        continue
    s = path.read_text(encoding="utf-8")
    s = s.replace("modern PLR 280-326", "historical PLR 1-326 plus best-effort newer-layout compatibility")
    s = s.replace("PLR 280-326", "PLR 1-326, plus newer releases when the 326 layout is unchanged")
    s = s.replace("280-326", "1-326")
    s = s.replace("327+ are rejected", "newer releases are attempted with the latest known layout and fail only on layout incompatibility")
    path.write_text(s, encoding="utf-8")

print("Applied full PLR history + forward-compatible layout probing")
