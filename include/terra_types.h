/*
 * terra_types.h -- Internal type definitions for TerraWasm.
 *
 * All structs are plain C, no dynamic allocation in constructors.
 * TxWorld holds raw .wld bytes plus parsed format/header metadata.
 * Tiles are read on-demand from the binary stream (never materialized).
 */
#ifndef TERRA_TYPES_H
#define TERRA_TYPES_H

#include <stdint.h>
#include "terra_status.h"

#ifndef NULL
#define NULL ((void*)0)
#endif

/* ---------- Constants ---------- */

#define TX_MAX_SECTIONS        16u
#define TX_MAX_WORLDS          8u
#define TX_MAX_NAME            160u
#define TX_MAX_SECTION_OVERRIDES 11u
#define TX_MAX_OPS             128u
#define TX_MAX_HEADER_BOOL_FIELDS 128u
#define TX_PAGE_SIZE           65536u
#define TX_ICON_ATLAS_MAX      256u

/* ---------- Tile (22 fields, matches Terraria tile binary format) ---------- */

typedef struct TxTile {
    uint8_t  active;
    uint16_t type;
    int16_t  frame_x;
    int16_t  frame_y;
    uint16_t wall;
    uint8_t  liquid_amount;
    uint8_t  liquid_type;       /* 0=none, 1=water, 2=lava, 3=honey, 4=shimmer */
    uint8_t  brick_style;       /* 0=full, 1=half, 2-4=slopes */
    uint8_t  tile_color;        /* paint */
    uint8_t  wall_color;
    uint8_t  wire_red;
    uint8_t  wire_blue;
    uint8_t  wire_green;
    uint8_t  wire_yellow;
    uint8_t  actuator;
    uint8_t  inactive;
    uint8_t  invisible_block;
    uint8_t  invisible_wall;
    uint8_t  fullbright_block;
    uint8_t  fullbright_wall;
    uint16_t same;              /* RLE count - 1 */
} TxTile;

/* ---------- Section override (raw bytes in bump allocator) ---------- */

typedef struct TxSectionOverride {
    uint8_t* data;
    uint32_t len;
    uint8_t  active;
} TxSectionOverride;

typedef struct TxHeaderBoolField {
    const char* json_name;
    uint32_t section_offset;
    uint32_t world_member_offset;
} TxHeaderBoolField;

typedef struct TxPixelArtChunk TxPixelArtChunk;

typedef struct TxIconAtlas {
    uint8_t* rgba;
    uint32_t icon_size;
    uint32_t icon_count;
    uint32_t atlas_width;
    uint32_t atlas_height;
    uint32_t item_ids[TX_ICON_ATLAS_MAX];
    uint32_t x_offsets[TX_ICON_ATLAS_MAX];
    uint32_t y_offsets[TX_ICON_ATLAS_MAX];
} TxIconAtlas;

struct TxPixelArtChunk {
    int32_t cx;
    int32_t cy;
    uint32_t used;
    const uint16_t* indices;
    TxPixelArtChunk* next;
};

/* ---------- Dynamic buffer ---------- */

typedef struct TxBuf {
    uint8_t* data;
    uint32_t len;
    uint32_t cap;
    int      ok;
} TxBuf;

/* ---------- Error state (V2 API structured error) ---------- */

typedef struct TxErrorState {
    int32_t status;
    char    code[32];
    char    message[256];
    char    operation[64];
    char    section[32];
    char    field[64];
    char    detail[128];
} TxErrorState;

/* ---------- World handle ---------- */

typedef struct TxWorld {
    uint8_t  active;
    uint32_t handle;            /* packed generation + slot token exposed to JS */
    uint32_t allocation_mark;   /* native sequence before the first owned root */

    /* Raw .wld file bytes (bump-allocated copy) */
    uint8_t* file;
    uint32_t file_len;

    /* Format metadata (parsed from binary) */
    uint32_t version;
    char     magic[8];          /* "relogic" or "xindong" */
    uint8_t  format_dirty;      /* version/magic changed and must be serialized */
    uint8_t  file_type;
    uint32_t revision;
    uint64_t favorite;
    uint16_t pointer_count;
    uint32_t positions[TX_MAX_SECTIONS];  /* section pointer table */
    uint32_t starts[TX_MAX_SECTIONS];     /* section byte start */
    uint32_t ends[TX_MAX_SECTIONS];       /* section byte end */
    uint32_t format_len;                  /* byte length of format section */
    uint16_t tile_type_count;
    uint8_t* important;         /* pointer into file[] for tile importance bitmap */
    uint8_t* important_override;/* owned replacement for a patched format bitmap */
    uint32_t important_len;

    /* Header metadata (parsed from binary) */
    char     worldName[TX_MAX_NAME];
    char     seed[TX_MAX_NAME];
    char     uuid[40];
    uint64_t worldGeneratorVersion;
    int32_t  worldId;
    int32_t  maxTilesX;
    int32_t  maxTilesY;
    int32_t  spawnTileX;
    int32_t  spawnTileY;
    double   worldSurface;     /* ground level (double for precision) */
    double   rockLayer;        /* rock layer level */
    int32_t  gameMode;
    int32_t  leftWorld;
    int32_t  rightWorld;
    int32_t  topWorld;
    int32_t  bottomWorld;

    /* ---- Seed flags (version-gated booleans) ---- */
    uint8_t  drunkWorld;           /* >=222 */
    uint8_t  ftwWorld;             /* >=227 */
    uint8_t  tenthAnniversaryWorld; /* >=238 */
    uint8_t  dontStarveWorld;     /* >=239 */
    uint8_t  notTheBeesWorld;    /* >=241 */
    uint8_t  remixWorld;           /* >=249 */
    uint8_t  noTrapsWorld;        /* >=266 */
    uint8_t  zenithWorld;          /* >=267; derived as remix&&drunk for <267 */
    uint8_t  skyblockWorld;        /* >=302 */

    /* ---- Timestamps ---- */
    uint64_t creationTime;         /* >=141, ticks */
    uint64_t lastPlayed;           /* >=284, ticks */

    /* ---- Terrain / environment ---- */
    uint8_t  moonType;
    uint32_t treeX[3];
    uint32_t treeStyle[4];
    uint32_t caveBackX[3];
    uint32_t caveBackStyle[4];
    uint32_t iceBackStyle;
    uint32_t jungleBackStyle;
    uint32_t hellBackStyle;

    /* ---- Time / world state ---- */
    double   gameTime;
    uint8_t  isDayTime;
    uint32_t moonPhase;
    uint8_t  isBloodMoon;
    uint8_t  isEclipse;
    int32_t  dungeonX;
    int32_t  dungeonY;
    uint8_t  isCrimson;

    /* ---- Boss / event progress ---- */
    uint8_t  downedEye;
    uint8_t  downedEaterBrain;
    uint8_t  downedSkeletron;
    uint8_t  downedQueenBee;
    uint8_t  downedDestroyer;
    uint8_t  downedTwins;
    uint8_t  downedSkeletronPrime;
    uint8_t  downedAnyMech;
    uint8_t  downedPlantera;
    uint8_t  downedGolem;
    uint8_t  downedKingSlime;     /* >=118 */

    /* ---- Saved NPCs ---- */
    uint8_t  savedGoblin;
    uint8_t  savedWizard;
    uint8_t  savedMech;
    uint8_t  downedGoblins;
    uint8_t  downedClown;
    uint8_t  downedFrost;
    uint8_t  downedPirates;

    /* ---- World state ---- */
    uint8_t  shadowOrbSmashed;
    uint8_t  spawnMeteor;
    uint8_t  shadowOrbCount;
    uint32_t altarCount;
    uint8_t  hardMode;
    uint8_t  afterPartyOfDoom;   /* >=257 */

    /* ---- Invasion ---- */
    uint32_t invasionDelay;
    uint32_t invasionSize;
    uint32_t invasionType;
    double   invasionX;
    double   slimeRainTime;       /* >=118 */
    uint8_t  sundialCooldown;      /* >=113 */

    /* ---- Weather ---- */
    uint8_t  isRaining;
    uint32_t rainTime;
    float    maxRain;

    /* ---- Ore tiers ---- */
    int32_t  oreTierCobalt;
    int32_t  oreTierMythril;
    int32_t  oreTierAdamantite;

    /* ---- Backgrounds ---- */
    uint8_t  bgTree;
    uint8_t  bgCorruption;
    uint8_t  bgJungle;
    uint8_t  bgSnow;
    uint8_t  bgHallow;
    uint8_t  bgCrimson;
    uint8_t  bgDesert;
    uint8_t  bgOcean;
    int32_t  cloudBgActive;
    uint16_t numClouds;
    float    windSpeedSet;

    /* ---- Angler (dynamic: file offset for string array) ---- */
    uint32_t anglerFinishedSize;          /* >=95 */
    uint32_t anglersOff;           /* file offset to re-read angler strings */
    uint8_t  savedAngler;          /* >=99 */
    uint32_t anglerQuest;          /* >=101 */
    uint8_t  savedStylist;         /* >=104 */
    uint8_t  savedTaxCollector;   /* >=140 */
    uint8_t  savedGolfer;          /* >=201 */

    /* ---- Misc progression ---- */
    uint32_t invasionSizeStart;   /* >=107 */
    uint32_t cultistDelay;         /* >=108 */

    /* ---- Kill counts (dynamic: file offset for u32 array) ---- */
    uint16_t numMobs;             /* >=109 */
    uint32_t mobsOff;

    /* ---- Banners (dynamic: file offset for u16 array) ---- */
    uint8_t  claimableBannersPresent;
    uint16_t numClaimableBanners;
    uint32_t claimableBannersOff;

    /* ---- Late events ---- */
    uint8_t  fastForwardTime;     /* >=140 */
    uint8_t  downedFishron;        /* >=131 */
    uint8_t  downedMartians;       /* >=140 */
    uint8_t  downedLunaticCultist; /* >=140 */
    uint8_t  downedMoonlord;       /* >=140 */
    uint8_t  downedHalloweenKing; /* >=131 */
    uint8_t  downedHalloweenTree; /* >=131 */
    uint8_t  downedChristmasIceQueen; /* >=131 */
    uint8_t  downedSanta;   /* >=131 */
    uint8_t  downedChristmasTree;      /* >=131 */

    /* ---- Celestial towers (>=140) ---- */
    uint8_t  downedCelestialSolar;
    uint8_t  downedCelestialVortex;
    uint8_t  downedCelestialNebula;
    uint8_t  downedCelestialStardust;
    uint8_t  downedTowerSolar;
    uint8_t  downedTowerVortex;
    uint8_t  downedTowerNebula;
    uint8_t  downedTowerStardust;
    uint8_t  downedTowerAncient;

    /* ---- Party (>=170) ---- */
    uint8_t  partyManual;
    uint8_t  partyGenuine;
    uint32_t partyCooldown;
    uint32_t partyCelebratingNPCSize;
    uint32_t partyCelebratingNPCsOff;        /* file offset for i32 array */

    /* ---- Sandstorm (>=174) ---- */
    uint8_t  sandstormHappening;
    uint32_t sandStormTime;
    float    sandStormSeverity;
    float    sandstormIntendedSeverity;

    /* ---- DD2 (>=178) ---- */
    uint8_t  savedBartender;
    uint8_t  downedInvasionT1;
    uint8_t  downedInvasionT2;
    uint8_t  downedInvasionT3;

    /* ---- More backgrounds ---- */
    uint8_t  mushroomBg;           /* >194 */
    uint8_t  undergroundDesertBg;         /* >=215 */
    uint8_t  bgTree2;              /* >=195 */
    uint8_t  bgTree3;
    uint8_t  bgTree4;

    /* ---- 1.4+ fields ---- */
    uint8_t  combatBookUsed;           /* >=204 */
    uint32_t lanternNightCooldown;      /* >=207 */
    uint8_t  lanternNightGenuine;
    uint8_t  lanternNightManual;
    uint8_t  lanternNightNextNightIsGenuine;
    uint32_t treetopSize;         /* >=211 */
    uint32_t treeTopVariationsOff;          /* file offset for i32 array */
    uint8_t  forceHalloweenForToday;       /* >=212 */
    uint8_t  forceXMasForToday;
    uint32_t savedOreTiersCopper;       /* >=216 */
    uint32_t savedOreTiersIron;
    uint32_t savedOreTiersSilver;
    uint32_t savedOreTiersGold;
    uint8_t  boughtCat;            /* >=217 */
    uint8_t  boughtDog;
    uint8_t  boughtBunny;
    uint8_t  downedEmpressOfLight;        /* >=223 */
    uint8_t  downedQueenSlime;
    uint8_t  downedDeerclops;      /* >=240 */

    /* ---- Unlocked spawns (>=250-261) ---- */
    uint8_t  unlockedSlimeBlueSpawn;
    uint8_t  unlockedMerchantSpawn;
    uint8_t  unlockedDemolitionistSpawn;
    uint8_t  unlockedPartyGirlSpawn;
    uint8_t  unlockedDyeTraderSpawn;
    uint8_t  unlockedTruffleSpawn;
    uint8_t  unlockedArmsDealerSpawn;
    uint8_t  unlockedNurseSpawn;
    uint8_t  unlockedPrincessSpawn;
    uint8_t  combatBookVolumeTwoWasUsed;      /* >=259 */
    uint8_t  peddlersSatchelWasUsed;      /* >=260 */
    uint8_t  unlockedSlimeGreenSpawn;  /* >=261 */
    uint8_t  unlockedSlimeOldSpawn;
    uint8_t  unlockedSlimePurpleSpawn;
    uint8_t  unlockedSlimeRainbowSpawn;
    uint8_t  unlockedSlimeRedSpawn;
    uint8_t  unlockedSlimeYellowSpawn;
    uint8_t  unlockedSlimeCopperSpawn;

    /* ---- Late 1.4+ fields ---- */
    uint8_t  fastForwardTimeToDusk;     /* >=264 */
    uint8_t  moondialCooldown;     /* >=264 */
    uint8_t  forceHalloweenForever; /* >=287 */
    uint8_t  forcexmasForever;    /* >=287 */
    uint8_t  vampireSeed;          /* >=288 */
    uint8_t  infectedSeed;         /* >=296 */
    uint32_t tempmeteorShowerCount;   /* >=291 */
    uint32_t tempcoinRain;             /* >=291 */
    uint8_t  teambasedSpawnsSeed;      /* >=297 */
    uint8_t  numExtradSpawnPointManager;     /* >=297 */
    uint32_t extradSpawnPointManagerOff;      /* file offset for i32 array */
    uint8_t  dualdungeonsSeed;    /* >=304 */
    uint32_t legacySkip;           /* >=299 && <313 */
    uint32_t maniFestOff;          /* >=299, file offset for 7bit string */
    uint32_t maniFestLen;          /* >=299, byte length of string at maniFestOff */

    /* Exact binary locations for the explicitly mutable header booleans. */
    TxHeaderBoolField header_bool_fields[TX_MAX_HEADER_BOOL_FIELDS];
    uint32_t header_bool_field_count;

    /* Section overrides (for terra_section_set_json) */
    TxSectionOverride section_overrides[TX_MAX_SECTION_OVERRIDES];
    uint32_t heap_mark;
    uint32_t override_heap_mark;

    /* Marker item thumbnails owned by the active world session. */
    TxIconAtlas icon_atlas;

    /* Cached preview state */
    uint32_t preview_width;
    uint32_t preview_height;
    uint32_t preview_stride;

    /* Operation heap tracking -- end-of-previous-operation heap position.
     * Used by terra_op_execute_json to release only the previous operation's
     * transient allocations without corrupting caller-allocated data. */
    uint32_t last_op_heap_end;
    /* Exact-keyed, single-use operation response for the two-call pattern. */
    uint32_t op_response_ptr;
    uint32_t op_response_len;
    uint8_t* op_request_key;
    uint32_t op_request_key_len;

    /* Exact section-indexed, single-use response for section two-call reads. */
    uint8_t* section_response;
    uint32_t section_response_len;
    int32_t  section_response_index;

    /* World-owned media result. Global tx_last_* values are scratch only. */
    uint8_t* media_result;
    uint32_t media_result_len;
    uint32_t media_result_width;
    uint32_t media_result_height;
    uint32_t media_result_stride;
    uint8_t  media_result_kind;  /* 1 = RGBA, 2 = PNG, 3 = MAP */

    /* Pixel art queue (applied during save) */
    uint8_t* pixel_art_pixels;     /* RGBA pixel data */
    uint32_t pixel_art_pixels_len;
    uint8_t* pixel_art_maps;       /* TxPixelMap array (12 bytes each) */
    uint32_t pixel_art_map_count;
    int32_t  pixel_art_start_x;
    int32_t  pixel_art_start_y;
    uint32_t pixel_art_width;
    uint32_t pixel_art_height;
    int32_t  pixel_art_skip_transparent;
    uint8_t  pixel_art_indexed;
    uint32_t pixel_art_default_index;
    uint32_t pixel_art_chunk_cols;
    uint32_t pixel_art_chunk_rows;
    TxPixelArtChunk** pixel_art_chunk_table;
    TxPixelArtChunk* pixel_art_chunks;
} TxWorld;

/* ---------- Pixel art color mapping entry (12 bytes, matches JS DataView layout) ---------- */

typedef struct TxPixelMap {
    uint8_t  r, g, b, a;       /* RGBA color to match */
    uint16_t tile_type;         /* tile type (0 = no tile) */
    uint16_t wall_type;         /* wall type (0 = no wall) */
    uint8_t  tile_color;        /* paint ID for tile */
    uint8_t  wall_color;        /* paint ID for wall */
    uint8_t  active_mode;       /* 0=empty, 1=tile, 2=wall, 3=skip/no-op */
    uint8_t  block_inactive;    /* set inactive flag */
} TxPixelMap;

/* ---------- Color table reference (set by JS) ---------- */

typedef struct TxColorTables {
    const uint8_t* tile_colors;
    uint32_t       tile_color_count;
    const uint8_t* wall_colors;
    uint32_t       wall_color_count;
} TxColorTables;

/* ---------- Biome conversion mode ---------- */

typedef enum TxBiomeMode {
    TX_BIOME_PURIFY    = 0,
    TX_BIOME_CORRUPTION = 1,
    TX_BIOME_CRIMSON   = 2,
    TX_BIOME_HALLOW    = 3
} TxBiomeMode;

/* ---------- Batch update rule ---------- */

typedef struct TxTileRule {
    /* Where clause (optional fields, -1 = don't match) */
    int32_t  is_active;     /* -1=any, 0=inactive, 1=active */
    int32_t  type;          /* -1=any */
    int32_t  wall;          /* -1=any */
    int32_t  liquid_amount; /* -1=any */
    int32_t  liquid_type;   /* -1=any */
    int32_t  brick_style;   /* -1=any */
    int32_t  tile_color;    /* -1=any */
    int32_t  wall_color;    /* -1=any */
    int32_t  wire_red;      /* -1=any */
    int32_t  wire_blue;     /* -1=any */
    int32_t  wire_green;    /* -1=any */
    int32_t  wire_yellow;   /* -1=any */
    int32_t  actuator;      /* -1=any */
    int32_t  inactive;      /* -1=any */
    int32_t  invisible_block; /* -1=any */
    int32_t  invisible_wall;  /* -1=any */
    int32_t  fullbright_block; /* -1=any */
    int32_t  fullbright_wall;  /* -1=any */

    /* Patch clause (fields to change, -1 = no change) */
    int32_t  patch_is_active;
    int32_t  patch_liquid_amount;
    int32_t  patch_liquid_type;
    int32_t  patch_brick_style;
    int32_t  patch_tile_color;
    int32_t  patch_wall_color;
    int32_t  patch_wire_red;
    int32_t  patch_wire_blue;
    int32_t  patch_wire_green;
    int32_t  patch_wire_yellow;
    int32_t  patch_invisible_block;
    int32_t  patch_invisible_wall;
    int32_t  patch_fullbright_block;
    int32_t  patch_fullbright_wall;
    int32_t  patch_actuator;
    int32_t  patch_inactive;
    int32_t  patch_type;         /* -1=no change, >=0 set tile type */
    int32_t  patch_wall;         /* -1=no change, >=0 set wall id */

    /* Limit (0 = unlimited) */
    uint32_t limit;

    /* Stats */
    uint32_t matched;
    uint32_t updated;
} TxTileRule;

/* ---------- Chest marker ---------- */

typedef struct TxChestMarker {
    int32_t  item_id;
    uint8_t  r, g, b, a;
    int32_t  radius;
    int32_t  thickness;
} TxChestMarker;

/* ---------- Tile marker ---------- */

typedef struct TxTileMarker {
    uint32_t tile_type;
    uint16_t tile_subid;
    uint8_t  r, g, b, a;
    int32_t  radius;
    int32_t  thickness;
} TxTileMarker;

#endif /* TERRA_TYPES_H */
