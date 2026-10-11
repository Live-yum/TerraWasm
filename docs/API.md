# TerraWasm API 文档

TerraWasm 是一个将 Terraria 世界文件（`.wld`）解析、编辑、渲染能力编译为 WebAssembly 的 C 库。通过 Emscripten 编译，提供 Node.js 和 Web 两个目标。

---

## 目录

1. [架构概览](#架构概览)
2. [构建系统](#构建系统)
3. [状态码枚举](#状态码枚举)
4. [核心数据结构](#核心数据结构)
5. [生命周期 API](#生命周期-api)
6. [信息查询 API](#信息查询-api)
7. [Section 读写 API](#section-读写-api)
8. [操作执行 API](#操作执行-api)
9. [内存管理 API](#内存管理-api)
10. [操作列表](#操作列表)
11. [Two-Call 模式](#two-call-模式)
12. [错误处理](#错误处理)
13. [使用示例（Node.js）](#使用示例nodejs)
14. [加密玩家文件 API](#加密玩家文件-api)

---

## 架构概览

```
┌─────────────────────────────────────────────────────┐
│  JS / Web / 小程序 宿主                              │
│    ↓ ccall / cwrap / 直接函数调用                    │
├─────────────────────────────────────────────────────┤
│  V2 API  (terra_world.h / terra_plr.h)              │
│    terra_world_open / create / close / save          │
│    terra_info_* / terra_plr_*                        │
│    terra_section_get_json / set_json                 │
│    terra_op_execute_json                             │
│    terra_op_get_thumbnail_png / terra_op_get_map    │
├──────────────┬──────────────────────────────────────┤
│  terra_api.c │  terra_ops.c (操作分发)               │
│              │  terra_mutators.c (安全 section 编码) │
│  terra_wld.c │  terra_render.c (PNG 渲染)           │
│  terra_map.c │  terra_update.c (批量修改)            │
├──────────────┴──────────────────────────────────────┤
│  terra_mem.c   (bump allocator)                     │
│  terra_json.c  (手写 JSON builder/parser)           │
│  terra_types.h (所有类型定义)                        │
└─────────────────────────────────────────────────────┘
```

**设计特点：**
- **三域跟踪分配**：bridge allocation 由调用者逐个 `tx_free`；native root 可逐个释放或按 sequence mark 回收；PLR DOM 使用独立 persistent 根
- **手写 libc**：`memset`/`memcpy`/`strlen`/`strcmp` 全部手写，无 libc 依赖
- **手写 JSON**：builder + parser 约 337 行 C，无外部 JSON 库
- **手写 PNG/zlib**：Fixed-Huffman deflate + LZ77 + CRC32，约 350 行
- **流式 tile 读取**：tile 按需从二进制流读取，不物化完整 tile 数组

---

## 构建系统

### 依赖

- **Emscripten SDK** (emsdk)
- **CMake** >= 3.27
- **zlib** — 通过 `-sUSE_ZLIB=1` (Emscripten port) 链接

### 构建命令

```powershell
# Node 目标
powershell -File TerraWasm/build-node.ps1

# Web 目标
powershell -File TerraWasm/build-web.ps1

# 两个目标
powershell -File TerraWasm/build.ps1
```

### 构建产物

| 目标 | 文件 | 初始内存 | 最大内存 |
|------|------|----------|----------|
| Node | `build/terrax_world_wasm.js` + `.wasm` | 128 MiB | 512 MiB |
| Web / Mini Program | `build/terrax_world_wasm_web.js` + `.wasm` | 64 MiB | 160 MiB |

内存值由 CMake 的 `TERRAX_*_INITIAL_MEMORY` / `TERRAX_*_MAXIMUM_MEMORY` cache 变量统一驱动，同时写入 linker flags、运行时 build identity 和 `terra.manifest.json`。`build.ps1` 会显式把同一组值传给 CMake 和 manifest 生成器，避免文档、WASM 制品与 manifest 漂移。Viewer 小程序的进程级可控内存预算是宿主侧独立限制，不等同于 WASM linear-memory 上限。

---

## 状态码枚举

```c
typedef enum terrax_world_status {
    TERRAX_WORLD_STATUS_OK                = 0,  // 成功
    TERRAX_WORLD_STATUS_INVALID_ARGUMENT  = 1,  // 无效参数
    TERRAX_WORLD_STATUS_BUFFER_TOO_SMALL  = 2,  // 缓冲区太小
    TERRAX_WORLD_STATUS_NOT_FOUND         = 3,  // 未找到 (section/operation)
    TERRAX_WORLD_STATUS_NOT_SUPPORTED     = 4,  // 不支持的操作
    TERRAX_WORLD_STATUS_PARSE_ERROR       = 5,  // 解析错误
    TERRAX_WORLD_STATUS_VALIDATION_ERROR  = 6,  // 验证错误
    TERRAX_WORLD_STATUS_IO_ERROR          = 7,  // I/O 错误
    TERRAX_WORLD_STATUS_STATE_ERROR       = 8,  // 状态错误
    TERRAX_WORLD_STATUS_INTERNAL_ERROR    = 9   // 内部错误
} terrax_world_status;
```

---

## 核心数据结构

### TxTile (65 bytes, 22 字段)

单个 tile 的完整表示，对应 Terraria 二进制格式：

```c
typedef struct TxTile {
    uint8_t  active;            // 是否有方块
    uint16_t type;              // 方块 ID (0-692+)
    int16_t  frame_x;           // U 纹理坐标
    int16_t  frame_y;           // V 纹理坐标
    uint16_t wall;              // 墙 ID
    uint8_t  liquid_amount;     // 液体量 (0-255)
    uint8_t  liquid_type;       // 0=无, 1=水, 2=岩浆, 3=蜂蜜, 4=微光
    uint8_t  brick_style;       // 0=满, 1=半, 2-4=斜面
    uint8_t  tile_color;        // 涂料 ID (1-30)
    uint8_t  wall_color;        // 墙涂料 ID
    uint8_t  wire_red;          // 红线 (0/1)
    uint8_t  wire_blue;         // 蓝线
    uint8_t  wire_green;        // 绿线
    uint8_t  wire_yellow;       // 黄线
    uint8_t  actuator;          // 致动器
    uint8_t  inactive;          // 非活跃 (致动器激活)
    uint8_t  invisible_block;   // 隐形方块
    uint8_t  invisible_wall;    // 隐形墙
    uint8_t  fullbright_block;  // 全亮方块
    uint8_t  fullbright_wall;   // 全亮墙
    uint16_t same;              // RLE 重复次数 - 1
} TxTile;
```

### TxWorld (~380 字段)

世界句柄，包含：
- **文件数据**：`file`, `file_len` — 原始 .wld 字节
- **Format 元数据**：`version`, `magic`, `pointer_count`, `positions[]`, `tile_type_count`
- **Header 元数据**：`worldName`, `seed`, `maxTilesX`, `maxTilesY`, `spawnTileX/Y`, `gameMode`, 等等
- **种子标志**：`drunkWorld`, `ftwWorld`, `remixWorld`, `zenithWorld`, 等等
- **Boss/事件进度**：`downedEye`, `downedPlantera`, `downedMoonlord`, 等等
- **Section 覆盖**：`section_overrides[11]` — `terra_section_set_json` 写入的修改
- **缓存状态**：preview 尺寸、operation 响应指针

### TxTileRule (批量更新规则)

```c
typedef struct TxTileRule {
    // Where 匹配条件 (-1 = 不匹配)
    int32_t is_active, type, wall, liquid_amount, liquid_type;
    int32_t brick_style, tile_color, wall_color;
    int32_t wire_red, wire_blue, wire_green, wire_yellow;
    int32_t actuator, inactive, invisible_block, invisible_wall;
    int32_t fullbright_block, fullbright_wall;
    // Patch 修改值 (-1 = 不修改)
    int32_t patch_is_active, patch_liquid_amount, patch_liquid_type;
    int32_t patch_brick_style, patch_tile_color, patch_wall_color;
    int32_t patch_wire_red, patch_wire_blue, patch_wire_green, patch_wire_yellow;
    int32_t patch_invisible_block, patch_invisible_wall;
    int32_t patch_fullbright_block, patch_fullbright_wall;
    int32_t patch_actuator, patch_inactive, patch_type, patch_wall;
    uint32_t limit;   // 限制匹配数 (0 = 无限)
    uint32_t matched; // 已匹配数 (输出)
    uint32_t updated; // 已更新数 (输出)
} TxTileRule;
```

### TxErrorState (结构化错误)

```c
typedef struct TxErrorState {
    int32_t status;        // 对应 terrax_world_status
    char    code[32];      // 如 "TERRAX_PARSE_ERROR"
    char    message[256];  // 人类可读描述
    char    operation[64]; // 出错的操作名
    char    section[32];   // 出错的 section
    char    field[64];     // 出错的字段
    char    detail[128];   // 额外详情
} TxErrorState;
```

---

## 生命周期 API

### `terra_world_open`

打开一个 `.wld` 文件，解析 format + header，返回世界句柄。

```c
terrax_world_status terra_world_open(
    const char* path_utf8,   // [in]  文件路径 (UTF-8)
    TxWorld**   out_world    // [out] 世界句柄指针
);
```

**行为：**
1. 读取文件到 bump allocator
2. 解析文件格式：88-326 读取 magic/指针表；1-87 按连续旧布局定位 header、瓦片和尾部
3. 解析 header section（世界名、尺寸、种子等）
4. 分配 `TxWorld` 结构体

**返回：** `OK` 成功，`IO_ERROR` 文件读取失败，`PARSE_ERROR` 解析失败

---

### `terra_world_create`

创建一个空的世界句柄（不关联文件）。

```c
terrax_world_status terra_world_create(
    TxWorld** out_world    // [out] 世界句柄指针
);
```

**返回：** `OK` 成功，`STATE_ERROR` 超过最大世界数 (8)

---

### `terra_world_close`

关闭世界句柄，释放关联的堆内存。

```c
terrax_world_status terra_world_close(
    TxWorld* world    // [in] 世界句柄
);
```

**行为：** 释放该世界的文件数据、section overrides、操作缓存。不影响其他世界。

---

### `terra_world_save`

将世界保存到文件（包含 section overrides 的修改）。

```c
terrax_world_status terra_world_save(
    TxWorld*    world,      // [in] 世界句柄
    const char* path_utf8   // [in] 输出文件路径 (UTF-8)
);
```

**行为：**
1. 从原始文件字节开始
2. 应用所有活跃的 section overrides
3. 写入目标路径

**返回：** `OK` 成功，`IO_ERROR` 写入失败

---

## 加密玩家文件 API

`terra_plr_*` 提供 Terraria 加密 `.plr` 的读写和语义编辑；`terra_player_*` 是同签名的兼容别名。文件使用 AES-128-CBC/PKCS#7，密钥和 IV 均为 UTF-16LE `h3y_gUyZ`。版本号不使用固定白名单：解析器保留 `version`，并依靠 metadata magic/type、字段边界、长度、计数和尾部完整性判断当前二进制布局是否兼容；布局不匹配时返回解析错误。

### 打开、保存和关闭

```c
terrax_world_status terra_plr_open_from_buffer(
    const uint8_t* buffer, uint32_t buffer_len, uint32_t* out_handle);
terrax_world_status terra_plr_open(
    const char* path_utf8, uint32_t* out_handle);
terrax_world_status terra_plr_open_json(
    const char* json_utf8, uint32_t* out_handle);
terrax_world_status terra_plr_close(uint32_t handle);
terrax_world_status terra_plr_save(
    uint32_t handle, const char* path_utf8);
terrax_world_status terra_plr_save_to_buffer(
    uint32_t handle, uint8_t* output, uint32_t capacity,
    uint32_t* out_required);
```

`terra_plr_save_to_buffer`/`terra_plr_encode` 支持 `output=NULL, capacity=0` 的 size probe。PLR 句柄使用独立 persistent 分配域；`tx_rewind`、`tx_reset_heap` 或 transient 回收不会释放活动玩家文档，调用方必须显式 `terra_plr_close`。

### JSON 查询和编辑

```c
terrax_world_status terra_plr_get_json(
    uint32_t handle, char* buffer, uint64_t buffer_size,
    uint32_t* required_size);
terrax_world_status terra_plr_get(
    uint32_t handle, const char* pointer_utf8, char* buffer,
    uint64_t buffer_size, uint32_t* required_size);
terrax_world_status terra_plr_set(
    uint32_t handle, const char* pointer_utf8,
    const char* value_json_utf8);
terrax_world_status terra_plr_set_many(
    uint32_t handle, const char* edits_json_utf8);
terrax_world_status terra_plr_apply_patch_json(
    uint32_t handle, const char* patch_json_utf8);
```

JSON 为 UTF-8、无 BOM；`get_json`/`get` 的 `required_size` 是 `uint32_t*`，包含末尾 NUL。查询路径是 RFC 6901 JSON Pointer，例如 `/name`、`/inventory/0/stack`、`/loadouts/1/armor/3/prefix`；字段名中的 `~` 和 `/` 必须分别写成 `~0` 和 `~1`。`set_many` 先克隆模型，任一指针或模型验证失败时不会提交部分修改。

结构化 patch 的顶层成员为：

- `fields`：按顶层 camelCase（也接受 snake_case）字段替换 JSON 值；
- `items`：`section`、`index`、`itemType`，以及可选 `stack`、`prefix`、`favorited`；
- `buffs`：`index`、`buffType`、`buffTime`；
- `loadoutSlots`：`loadoutIndex`、`slotKind`、`slotIndex`，其中 Armor/Dye 的 `itemType`、`stack`、`prefix` 和 Hide 的 `hide` 可为 JSON `null`，分别使用 `0`、`1`、`0`、`false` 默认值。

错误时使用 `TERRAX_PLR_PARSE_ERROR`、`TERRAX_PLR_JSON_ERROR`、`TERRAX_PLR_VALIDATION_ERROR`、`TERRAX_INVALID_HANDLE` 或 `TERRAX_WASM_OOM` 等结构化 code；原文错误仍通过 `terra_info_get_last_error_json` 查询。

---

## 信息查询 API

### `terra_info_list_sections_json`

列出所有 11 个 section 名称。

```c
terrax_world_status terra_info_list_sections_json(
    char*     buffer,        // [out] 可选，接收 JSON
    uint64_t  buffer_size,   // 缓冲区大小
    uint64_t* required_size  // [out] 所需大小
);
```

**返回 JSON 示例：**
```json
["format","header","chests","signs","npcs","tile_entities",
 "weighted_pressure_plates","town_manager","bestiary",
 "creative_powers","footer"]
```

---

### `terra_info_get_section_schema_json`

获取指定 section 的 JSON schema 描述。

```c
terrax_world_status terra_info_get_section_schema_json(
    const char* section_name,  // [in]  section 名称
    char*       buffer,        // [out] 可选
    uint64_t    buffer_size,
    uint64_t*   required_size
);
```

**参数 `section_name`：** 11 个合法值之一：`format`, `header`, `chests`, `signs`, `npcs`, `tile_entities`, `weighted_pressure_plates`, `town_manager`, `bestiary`, `creative_powers`, `footer`

---

### `terra_info_get_last_error_json`

获取最后一次错误的结构化信息。

```c
terrax_world_status terra_info_get_last_error_json(
    char*     buffer,
    uint64_t  buffer_size,
    uint64_t* required_size
);
```

**返回 JSON 示例：**
```json
{
  "status": 5,
  "code": "TERRAX_PARSE_ERROR",
  "message": "unsupported version",
  "operation": "terra_world_open",
  "section": "format",
  "field": "",
  "detail": ""
}
```

成功操作后返回 `{}` 或空字符串。

---

## Section 读写 API

### `terra_section_get_json`

读取指定 section 的数据为 JSON 字符串。

```c
terrax_world_status terra_section_get_json(
    TxWorld*    world,         // [in]  世界句柄
    const char* section_name,  // [in]  section 名称
    char*       buffer,        // [out] 可选
    uint64_t    buffer_size,
    uint64_t*   required_size
);
```

**支持的 section：**

| Section | 内容 | 可写 |
|---------|------|------|
| `format` | 版本、magic、指针表、tile_type_count | 只读 |
| `header` | 世界名、种子、尺寸、spawn、boss 进度等 | 通过 `header_patch` 修改白名单布尔字段 |
| `chests` | 宝箱列表及其物品 | 通过 `replace_chests` 修改 |
| `signs` | 标牌列表 | 只读 |
| `npcs` | NPC 列表 | 只读 |
| `tile_entities` | Tile 实体（逻辑传感器等） | 只读 |
| `weighted_pressure_plates` | 加重压力板 | 只读 |
| `town_manager` | 城镇 NPC 房间分配 | 只读 |
| `bestiary` | 怪物图鉴数据 | 通过 `replace_bestiary` 修改 |
| `creative_powers` | Journey 模式权限 | 只读 |
| `footer` | 世界 ID + 标记 | 只读 |

---

### `terra_section_set_json`

此入口不接受 JSON 原文作为 WLD section 字节。当前所有已知 section 都返回 `NOT_SUPPORTED`；写操作必须使用下文的显式 operation，由已验证的二进制 encoder 生成 section。

```c
terrax_world_status terra_section_set_json(
    TxWorld*    world,         // [in] 世界句柄
    const char* section_name,  // [in] section 名称
    const char* json_utf8      // [in] JSON 数据
);
```

**返回：** `NOT_FOUND`（未知 section）或 `NOT_SUPPORTED`（禁止不安全的 JSON→原始字节覆盖）。

---

## 操作执行 API

### `terra_op_execute_json`

执行命名操作，JSON 请求 → JSON 响应。

```c
terrax_world_status terra_op_execute_json(
    uint32_t    handle,             // [in]  不透明世界句柄
    const char* operation_name,     // [in]  操作名
    const char* request_json,       // [in]  请求 JSON
    char*       response_buffer,    // [out] 可选
    uint64_t    response_buffer_size,
    uint64_t*   required_size
);
```

**操作名列表见 [操作列表](#操作列表) 章节。**

`request_json` 最大为 1 MiB。实现直接读取调用者缓冲区，不在 WASM 栈上复制整份请求；two-call 探测只缓存一份精确请求键，第二次调用不会重复执行 mutation。

---

### `terra_op_get_thumbnail_png`

获取最近一次 `render_thumbnail_png` 操作的 PNG 数据。

```c
terrax_world_status terra_op_get_thumbnail_png(
    uint8_t*  buffer,        // [out] 可选，接收 PNG 字节
    uint64_t  buffer_size,
    uint64_t* required_size, // [out] PNG 字节数
    uint32_t* width,         // [out] 图像宽度
    uint32_t* height         // [out] 图像高度
);
```

**注意：** 此函数不需要世界句柄，PNG 数据存储在全局状态中。

---

## 内存管理 API

TerraWasm 使用 bridge/native 双域跟踪分配器。bridge 指针归 JS 调用者；world、section override 和 operation 临时结果属于 native 域。

| 函数 | 签名 | 说明 |
|------|------|------|
| `tx_malloc` | `uint32_t tx_malloc(uint32_t size)` | 从 bump allocator 分配内存 |
| `tx_free` | `void tx_free(uint32_t ptr)` | 释放一个合法的 bridge 根；重复、内部或偏移指针安全忽略 |
| `tx_heap_used` | `uint32_t tx_heap_used(void)` | 返回已用堆字节数 |
| `tx_bridge_heap_used` | `uint32_t tx_bridge_heap_used(void)` | 返回调用者 bridge 活跃字节数 |
| `tx_native_heap_used` | `uint32_t tx_native_heap_used(void)` | 返回 world/native 活跃字节数 |
| `tx_reset_heap` | `void tx_reset_heap(void)` | 重置 bump allocator（仅当无世界打开时） |
| `tx_memory_used` | `uint32_t tx_memory_used(void)` | 返回 WASM 线性内存总大小 |
| `tx_reclaim_transients` | `void tx_reclaim_transients(void)` | 回收临时分配 |
| `txw_set_color_tables` | `void txw_set_color_tables(uint32_t tile_ptr, uint32_t tile_count, uint32_t wall_ptr, uint32_t wall_count)` | 注册自定义颜色表 |
| `terra_world_open_from_buffer` | `terrax_world_status terra_world_open_from_buffer(const uint8_t* buffer, uint32_t buffer_len, TxWorld** out_world)` | 从内存缓冲区打开世界 |

---

## 操作列表

以下是 `terra_op_execute_json` 支持的所有操作：

### 渲染操作

#### `render_preview_png`

渲染世界为 PNG 文件。

全尺寸流式标记 PNG 在当前任务内使用至多 128 KiB 的可选宝箱首匹配缓存，避免每个行带重复解析库存；保持原箱顺序、首个匹配槽位、图标和透明混合结果，缓存分配不足时沿用原渲染路径，完成或取消均释放缓存。

**请求 JSON：**
```json
{
  "max_w": 400,              // 最大宽度 (0 = 原始)
  "max_h": 200,              // 最大高度 (0 = 原始)
  "output_path": "out.png"   // 可选，保存到文件
}
```

**响应 JSON：**
```json
{
  "status": "ok",
  "output_path": "out.png",
  "width": 400,
  "height": 114,
  "size": 28543
}
```

---

#### `render_thumbnail_png`

渲染缩略图 PNG（保持比例，默认 max_w=1920）。

**请求 JSON：**
```json
{
  "max_w": 1920   // 最大宽度 (默认 1920)
}
```

**响应 JSON：**
```json
{
  "status": "ok",
  "width": 1920,
  "height": 549,
  "size": 156789
}
```

之后用 `terra_op_get_thumbnail_png` 获取 PNG 字节。

---

#### `render_lit_map`

生成 `.map` 文件（Terraria 地图格式）。

带标记的 buffered 和 stream MAP 编码器，仅在最终命中点中存在可绘制图标时分配单任务颜色缓存（最多 4 MiB）：点的物品 ID 必须在当前图集中，半径及描边计算后的图标边长必须大于零。仅画圆环、半径为零、未知物品和没有命中点的请求不分配此缓存。宝箱使用实际匹配的库存物品 ID；描边宽度等于或超过半径时仍遵循原有的全半径图标绘制规则。缓存分配失败时继续使用原逐像素查色路径，任务完成、失败或取消后释放，输出字节和标记顺序保持一致。

**请求 JSON：**
```json
{
  "output_dir": "/path/to/dir"   // 输出目录
}
```

**响应 JSON：**
```json
{
  "status": "ok",
  "width": 8400,
  "height": 2400,
  "map_bytes": 523456,
  "file_written": true
}
```

---

### 批量修改操作

#### `header_patch`

通过一个 `patch` 对象统一修改当前 WLD 的完整 header/format 可编码模型，不再按页面用途拆分字段类别。header 可写字段以当前版本 `terra_section_get_json("header")` 返回值为准，包括标量、布尔值、固定数组、动态字符串/数字数组、出生点列表和 `manifestJson`；派生的 `creationTimeDate`、`lastPlayedDate` 不单独写入。format 支持 `version`、`magic`、`type`、`revision`、`favoriteFlags`、`tileTypeCount` 和 `tileFrameImportantBitmap`；`pointerCount`、`positions` 由保存器重新计算。跨越 header 或非 header section 布局门槛的版本变更会被拒绝，修改 `tileTypeCount` 时必须同时提供等长位图。

`magic` 只能是 `relogic` 或 `xindong`，`uniqueId` 必须是标准 UUID，64 位整数可传 JSON 整数或十进制字符串，动态数组的 count 必须与数组长度一致，出生点必须位于目标世界边界内。未知字段、当前版本不存在的字段、重复字段和空 patch 都会原子拒绝。调用方可以只提交所需字段；页面层是否开放尺寸、种子和版本编辑不影响 WASM 接口能力。

未修改的 `worldName`、字符串 `seed`、钓鱼任务完成者名称和 `manifestJson` 从当前 header 的有界原始范围复制，保留完整 UTF-8 内容及原长度前缀；不会经过内部 159 字节的名称/种子摘要。header JSON 和 footer 世界名也返回完整字符串。显式修改世界名、字符串种子或 manifest 时按实际 JSON 字段长度分配，仍受整份请求 1 MiB 上限约束。WLD 179 的数字种子继续使用原 UInt32 格式；修改世界名或世界 ID 时，footer 同步使用完整世界名。普通保存、重复 header override 和流式 `edit_plan` 遵循相同规则。

```json
{
  "patch": {
    "worldName": "新世界名称",
    "gameMode": 1,
    "spawnTileX": 4201,
    "spawnTileY": 2254,
    "crimson": true,
    "magic": "xindong",
    "drunkWorld": true,
    "downedEyeOfCthulhu": true,
    "savedGoblin": true,
    "hardMode": true
  }
}
```

响应中的 `updated` 是本次 patch 字段数；上例为 `{"status":"ok","updated":10}`。

#### `replace_chests`

宝箱数量按文件布局限制：WLD 88+ 的计数节支持 **8000** 个宝箱；WLD 1..87 的旧布局仍然写入恰好 **1000** 个存在标记（空位置补 `false`），因此最多 1000 个宝箱。这与 [指定 Terraria 源码的 `SaveChests` / `LoadChests` 和旧版加载器](https://github.com/Live-yum/TerrariaDecompiledSource/blob/8255d34616c780af12079425ac92a0a7aed87d71/Terraria.IO/WorldFile.cs) 一致。超过该布局上限时整次修改被拒绝，保留先前的世界和 section override。

WLD 294+ 每箱卡槽上限 504；WLD 88..293 必须保持源文件的共享卡槽数；更早版本固定为 20（版本 <58）或 40。`x/y` 必须位于世界边界内；`items` 长度必须严格等于 `maxItems`，空卡槽必须写为 `null`。名称最多 255 UTF-8 字节，版本 <85 必须为空。下面是现代版本的请求格式：物品限制为 `stack` 1..32767、`itemType` 1..1000000、`prefix` 0..255；历史版本继续使用其原有名称、堆叠数和前缀规则。每次请求的 JSON 上限仍为 1 MiB。

```json
{
  "chests": [
    {
      "x": 123,
      "y": 456,
      "name": "材料箱",
      "maxItems": 3,
      "items": [
        { "stack": 12, "itemType": 1, "prefix": 0 },
        null,
        { "stack": 1, "itemType": 50, "prefix": 2 }
      ]
    }
  ]
}
```

#### `replace_bestiary`

三个数组都必须存在，每个数组最多 4096 项，`persistentNpcId` 上限 255 UTF-8 字节，`killCount` 范围 0..1000000。未知或重复对象字段会被拒绝。

```json
{
  "kills": [{ "persistentNpcId": "Terraria.Zombie", "killCount": 50 }],
  "sightings": [{ "persistentNpcId": "Terraria.Bunny" }],
  "chats": [{ "persistentNpcId": "Terraria.Guide" }]
}
```

三个 mutator 都先完整验证并编码到临时原生缓冲区，成功后才原子发布 section override；解析、范围检查或 OOM 失败不会改变活动世界。可在一次 `terra_world_open*` 中连续执行三者，然后只调用一次 `terra_world_save_to_buffer`。

#### `batch_update_tiles`

批量修改 tile 属性。

**请求 JSON：**
```json
{
  "rules": [
    {
      "where": {
        "type": 1,           // 匹配方块类型
        "wall": 150,         // 匹配墙壁类型
        "tile_color": 5,     // 匹配方块涂料
        "wire_red": 1,       // 匹配有红线的
        "invisible_block": null  // null = 任意
      },
      "patch": {
        "type": 100,         // 替换为方块类型 100
        "wire_red": 0,       // 移除红线
        "invisible_block": 1 // 设为隐形
      },
      "limit": 1000          // 最多修改 1000 个
    }
  ]
}
```

**Where 字段（可选，-1 或 null = 不限制该条件）：**
- `is_active`, `type`, `wall`
- `liquid_amount`, `liquid_type`
- `brick_style`, `tile_color`, `wall_color`
- `wire_red`, `wire_blue`, `wire_green`, `wire_yellow`
- `actuator`, `inactive`, `invisible_block`, `invisible_wall`
- `fullbright_block`, `fullbright_wall`

**Patch 字段（可选，-1 或 null = 不修改）：**
在 JSON 的 `patch` 对象内使用与 Where 相同的字段名，例如：
- `is_active`, `type`, `wall`
- `liquid_amount`, `liquid_type`
- `brick_style`, `tile_color`, `wall_color`
- `wire_red`, `wire_blue`, `wire_green`, `wire_yellow`
- `actuator`, `inactive`, `invisible_block`, `invisible_wall`
- `fullbright_block`, `fullbright_wall`

基础字段会验证 JSON 类型和 WLD 可表示范围；字符串形式的数字、错误类型和越界值会拒绝整组规则，不会静默扩大匹配范围或截断数值。`rules` 的每一项必须是对象，`where` / `patch` 可省略或为 `null`。

| 基础字段 | 有效修改值 / 匹配值 |
|---|---|
| `type`, `wall` | 整数 `0..65535` |
| `liquid_amount`, `tile_color`, `wall_color` | 整数 `0..255` |
| `liquid_type` | 整数 `0..4`（无、水、岩浆、蜂蜜、微光） |
| `brick_style` | 整数 `0..7`（WLD 的 3 位编码范围） |
| 布尔基础字段（如 `is_active`, `wire_red`） | `true` / `false` 或整数 `0` / `1` |

这些基础字段仍接受 `-1` / `null` 作为原有哨兵：Where 不限制该条件，Patch 不修改该属性。`limit` 可省略或为 `null`，否则须为整数 `0..2147483647`，其中 `0` 表示不限数量。`platform_style`、坐标、材质和环境条件继续使用各自更严格的约束。

实现会按列顺序流式读取 tile RLE run 并重写 tile section，不会把全图 tile 展开成数组。

环境条件：`where.biome_region` 按修改前的环境判定，支持以下稳定编号；`where.has_wall` 为 `0/1`。两者省略时不限制，显式非法值会报错。

| ID | 环境 | ID | 环境 |
|---|---|---|---|
| 1 | 地牢（含地表入口） | 8 | 发光蘑菇 |
| 2 | 丛林（含神庙） | 9 | 海洋（含海滩） |
| 3 | 沙漠（含地下沙漠） | 10 | 地狱 |
| 4 | 雪地（含地下雪地） | 11 | 森林（普通地表） |
| 5 | 腐化 | 12 | 太空 |
| 6 | 猩红 | 13 | 地下土层 |
| 7 | 神圣 | 14 | 洞穴岩石层 |

地牢保留250块邻域阈值，覆盖各高度地牢砖、安全／危险地牢墙及墙前物件。其他材料环境覆盖整个达标邻域，包含其中建筑、家具、宝箱；沙漠计数排除游戏定义的海滩砂块，感染与神圣计数相互抵消，向日葵降低感染计数。双地牢种子按最近实体判定地下环境。空岛在全图有效物块低于10%时采用300块沙漠／雪地阈值，否则1500块。森林是排除主要环境、陨石和墓地的普通地表；海洋、高度层与材料环境可以重叠。

通用环境操作：`patch.terrain_theme`、`patch.wall_theme`、`patch.furniture_theme` 分别转换地形、背景墙、宝箱与家具，值为 `1`（沙漠）、`2`（雪地）、`3`（丛林）。可独立组合条件、涂料、透明等字段；没有依赖前端方案 ID 的专用执行路径。同条规则先执行环境操作，再执行显式属性写入；后续规则可继续覆盖结果。

- 地形覆盖普通实心方块、矿石、地牢／神庙砖、建筑材料及梁柱。沙漠用不坠落的砂岩，雪地用雪块，丛林用丛林草；梁柱保持非实心特性并使用对应材料。
- 背景墙保留是否可用于住房的属性。家具按 Terraria 1.4.5.8 的 `TileObjectData` 和物品放置样式转换，保留朝向、开关帧、机关类型和宝箱内容。无兼容款的特殊物件、上锁宝箱、锁住的神庙门和单行王朝桌保留原样。
- 家具支持普通／机关宝箱、开关门、平台、桌、椅、工作台、书架、床、钢琴、梳妆台、沙发、浴缸、吊灯、灯笼、蜡烛、落地灯、烛台、时钟、水槽、马桶和火把。
- 普通组合及纯海洋／高度条件共用一次 tile 扫描；其他环境组合共用额外一次区域分类，只保留169列邻域，按已选择环境打包成员位。后续预览和 MAP 可复用准备结果。

**响应 JSON：**
```json
{
  "status": "ok",
  "rules": [
    { "matched": 15234, "updated": 15234 }
  ],
  "total_matched": 15234,
  "total_updated": 15234
}
```

---

### 像素画映射


---

### 地图标记

#### `mark_tiles_and_chests_map`

生成 `.map` 文件，在匹配的箱子和方块位置上标记自定义颜色。

**请求 JSON：**
```json
{
  "output_dir": "/path/to/output",
  "chest_markers": [
    {
      "item_id": 49,              // 箱子中搜索的物品 ID
      "radius": 24,               // 标记半径（像素）
      "color": "#FF2020C8"        // 标记颜色 (#RRGGBBAA)
    }
  ],
  "tile_markers": [
    {
      "tile_type": 4,             // 方块类型 ID
      "tile_subid": 0,            // 方块子 ID
      "radius": 16,
      "color": "#20A0FFFF"
    }
  ]
}
```

**响应 JSON：**
```json
{
  "status": "ok",
  "matched_chest_count": 2,
  "matched_tile_count": 12,
  "map_bytes": 8849128,
  "file_written": true
}
```

---

未列出的 operation 名称会返回 `NOT_FOUND`，不会执行任何 WLD 修改。

---

## Two-Call 模式

大多数 API 使用 two-call 模式获取可变长度数据：

```javascript
// 1. 第一次调用：传 NULL/0 获取所需大小
const requiredSizePtr = malloc(8);
terra_info_list_sections_json(0, 0, requiredSizePtr);
const size = readU64(requiredSizePtr);

// 2. 分配缓冲区
const buffer = malloc(size);

// 3. 第二次调用：获取实际数据
terra_info_list_sections_json(buffer, BigInt(size), requiredSizePtr);
const json = UTF8ToString(buffer);
```

**Node.js 辅助函数：**
```javascript
function twoCall(fn) {
    const rp = M._tx_malloc(8);
    fn(0, 0n, rp);                    // 第一次：获取大小
    const sz = readU64(rp);
    if (sz === 0) return "";
    const buf = M._tx_malloc(sz);
    fn(buf, BigInt(sz), rp);          // 第二次：获取数据
    return UTF8ToString(buf);
}
```

**注意：** 操作名和请求 JSON 必须在两次调用期间保持有效。`stackAlloc` 和受调用者管理的 bridge allocation 都可以；原生 operation rewind 不会释放 bridge allocation。

---

## 错误处理

### 错误获取流程

```javascript
// 执行操作
const status = terra_op_execute_json(world, "render_thumbnail_png", "{\"max_w\":1920}", 0, 0n, rp);

if (status !== 0) {
    // 获取结构化错误信息
    const errJson = twoCall(M._terra_info_get_last_error_json);
    const err = JSON.parse(errJson);
    console.error(`[${err.code}] ${err.message}`);
}
```

### 常见错误场景

| 场景 | 状态码 | 错误码 |
|------|--------|--------|
| 文件不存在 | 7 | `TERRAX_IO_ERROR` |
| 文件格式错误 | 5 | `TERRAX_PARSE_ERROR` |
| 不支持的版本 | 5 | `TERRAX_PARSE_ERROR` |
| 未知 section | 3 | `TERRAX_NOT_FOUND` |
| 只读 section 写入 | 4 | `TERRAX_NOT_SUPPORTED` |
| 缓冲区太小 | 2 | `TERRAX_BUFFER_TOO_SMALL` |
| 空指针参数 | 1 | `TERRAX_INVALID_ARGUMENT` |
| 未知操作名 | 3 | `TERRAX_UNKNOWN_OPERATION` |

---

## 使用示例（Node.js）

```javascript
const path = require("path");
const TerraWorldWasm = require("./build/terrax_world_wasm.js");

async function main() {
    // 初始化 WASM 模块
    const M = await TerraWorldWasm();
    const { UTF8ToString, stringToUTF8, lengthBytesUTF8, getValue } = M;

    // 辅助函数
    function as(s) {
        const l = lengthBytesUTF8(s) + 1;
        const p = M._tx_malloc(l);
        stringToUTF8(s, p, l);
        return p;
    }
    function readU64(ptr) { return M.HEAPU32[ptr >> 2] >>> 0; }
    function twoCall(fn) {
        const rp = M._tx_malloc(8);
        fn(0, 0n, rp);
        const sz = readU64(rp);
        if (sz === 0) return "";
        const buf = M._tx_malloc(sz);
        fn(buf, BigInt(sz), rp);
        return UTF8ToString(buf);
    }

    // 打开世界
    const hp = M._tx_malloc(4);
    const wp = as("path/to/world.wld");
    const st = M._terra_world_open(wp, hp);
    const world = getValue(hp, "i32");

    if (st !== 0) {
        const err = JSON.parse(twoCall(M._terra_info_get_last_error_json));
        console.error("打开失败:", err);
        return;
    }

    // 读取 header
    const headerJson = /* twoCall with section_get_json */;
    const header = JSON.parse(headerJson);
    console.log("世界名:", header.worldName);
    console.log("尺寸:", header.maxTilesX, "x", header.maxTilesY);

    // 渲染缩略图
    const thumbResult = opExec(world, "render_thumbnail_png", '{"max_w":800}');
    console.log("缩略图:", JSON.parse(thumbResult.response));

    // 批量修改
    const updateResult = opExec(world, "batch_update_tiles", JSON.stringify({
        rules: [{ where: {}, patch: { wire_red: 0, wire_blue: 0, wire_green: 0, wire_yellow: 0 } }]
    }));

    // 保存
    M._terra_world_save(world, as("output.wld"));

    // 关闭
    M._terra_world_close(world);
}

main();
```

---

## 附录：Section Schema 示例

### format section
```json
{
  "version": 319,
  "magic": "relogic",
  "fileType": 2,
  "revision": 12345,
  "favorite": 0,
  "pointerCount": 11,
  "positions": [12, 100, ...],
  "tileTypeCount": 693
}
```

### header section (部分字段)
```json
{
  "worldName": "My World",
  "seed": "12345",
  "uuid": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
  "maxTilesX": 8400,
  "maxTilesY": 2400,
  "spawnTileX": 4200,
  "spawnTileY": 1200,
  "worldSurface": 600.0,
  "rockLayer": 1000.0,
  "gameMode": 0,
  "hardMode": true,
  "downedPlantera": false,
  ...
}
```


### 历史 WLD 兼容范围

WLD 文件格式 1-87 支持同步与任务式打开、header/宝箱/告示牌/NPC 读取、瓦片预览和原字节保存；旧格式不支持现代编辑编码器，编辑请求返回不支持错误。没有出现的可选 section 返回空集合（图鉴保持对象结构）。88-326 沿用分段模型。这里的数字是文件头 release 值，不是游戏展示版本号。详见 [多版本兼容说明](MULTI_VERSION_COMPATIBILITY.md)。


## Future-version read safety

Positive Int32 source releases are attempted without a known-maximum read gate. A structurally readable source newer than release 326 is read-only. WLD `format` JSON includes `originalVersion`, `readOnly`, `compatibility` (`future-layout-readonly`, `legacy-readonly`, or `known`) and `canExportOriginal`. Original byte export remains available. Mutations fail with `TERRAX_FUTURE_VERSION_READ_ONLY` / status `NOT_SUPPORTED`; this includes direct pixel, command and stream write entry points. A stream-backed source must be saved through stream `save`, which copies its immutable source ranges; buffer save never exports a compact metadata image as a world. PLR semantic JSON is unchanged; future sources retain the original encrypted bytes and cannot be changed through setters, replacement JSON, patches or aliases. Creation/conversion targets remain at proven layouts.
