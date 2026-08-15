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

---

## 架构概览

```
┌─────────────────────────────────────────────────────┐
│  JS / Web / 小程序 宿主                              │
│    ↓ ccall / cwrap / 直接函数调用                    │
├─────────────────────────────────────────────────────┤
│  V2 API  (terra_world.h)                            │
│    terra_world_open / create / close / save          │
│    terra_info_*                                      │
│    terra_section_get_json / set_json                 │
│    terra_op_execute_json                             │
│    terra_op_get_preview_rgba / get_thumbnail_png     │
├──────────────┬──────────────────────────────────────┤
│  terra_api.c │  terra_ops.c (操作分发)               │
│              │  terra_mutators.c (安全 section 编码) │
│  terra_wld.c │  terra_render.c (PNG/RGBA渲染)       │
│  terra_map.c │  terra_update.c (批量修改)            │
├──────────────┴──────────────────────────────────────┤
│  terra_mem.c   (bump allocator)                     │
│  terra_json.c  (手写 JSON builder/parser)           │
│  terra_types.h (所有类型定义)                        │
└─────────────────────────────────────────────────────┘
```

**设计特点：**
- **双域跟踪分配**：bridge allocation 由调用者逐个 `tx_free`；native root 可逐个释放或按 sequence mark 回收
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
2. 解析 format section（版本、magic、指针表）
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

### `terra_op_get_preview_rgba`

获取最近一次 `render_preview_rgba` 操作的 RGBA 缓冲区数据。

```c
terrax_world_status terra_op_get_preview_rgba(
    TxWorld*  world,         // [in]  世界句柄
    uint8_t*  buffer,        // [out] 可选，接收 RGBA 像素
    uint64_t  buffer_size,
    uint64_t* required_size, // [out] 所需大小 (width * height * 4)
    uint32_t* width,         // [out] 图像宽度
    uint32_t* height,        // [out] 图像高度
    uint32_t* stride         // [out] 每行字节数 (通常 = width * 4)
);
```

**使用流程：**
1. 先调用 `terra_op_execute_json("render_preview_rgba", ...)` 渲染
2. 第一次调用获取 `required_size`、`width`、`height`
3. 分配缓冲区，第二次调用获取像素数据

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

#### `render_preview_rgba`

渲染世界为 RGBA 像素缓冲区。

**请求 JSON：**
```json
{
  "max_w": 0,   // 最大宽度 (0 = 原始尺寸)
  "max_h": 0    // 最大高度 (0 = 原始尺寸)
}
```

**响应 JSON：**
```json
{
  "status": "ok",
  "pixel_format": "rgba8",
  "width": 8400,
  "height": 2400,
  "stride": 33600,
  "buffer_size": 80640000
}
```

之后用 `terra_op_get_preview_rgba` 获取像素数据。

---

#### `render_preview_png`

渲染世界为 PNG 文件。

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

通过一个 `patch` 对象统一修改当前 WLD 的完整 header/format 可编码模型，不再按页面用途拆分字段类别。header 可写字段以当前版本 `terra_section_get_json("header")` 返回值为准，包括标量、布尔值、固定数组、动态字符串/数字数组、出生点列表和 `manifestJson`；派生的 `creationTimeDate`、`lastPlayedDate` 不单独写入。format 支持 `version`、`magic`、`type`、`revision`、`favoriteFlags`、`tileTypeCount` 和 `tileFrameImportantBitmap`；`pointerCount`、`positions` 由保存器重新计算。跨越 header 布局门槛的版本变更会被拒绝，修改 `tileTypeCount` 时必须同时提供等长位图。

`magic` 只能是 `relogic` 或 `xindong`，`uniqueId` 必须是标准 UUID，64 位整数可传 JSON 整数或十进制字符串，动态数组的 count 必须与数组长度一致，出生点必须位于目标世界边界内。未知字段、当前版本不存在的字段、重复字段和空 patch 都会原子拒绝。调用方可以只提交所需字段；页面层是否开放尺寸、种子和版本编辑不影响 WASM 接口能力。

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

适用于 WLD 294+。宝箱上限 1000，每箱卡槽上限 504，名称上限 255 UTF-8 字节。`x/y` 必须位于世界边界内；`items` 长度必须严格等于 `maxItems`，空卡槽必须写为 `null`。物品限制：`stack` 1..32767、`itemType` 1..1000000、`prefix` 0..255。

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

**Where 字段（可选，-1 或 null = 不匹配）：**
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

实现会按列顺序流式读取 tile RLE run 并重写 tile section，不会把全图 tile 展开成数组。

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

#### `convert_world_biome`

生物群系转换（内部调用 batch_update_tiles）。

**请求 JSON：**
```json
{
  "mode": "purify"     // "purify" | "corruption" | "crimson" | "hallow"
}
```

| 模式 | 效果 |
|------|------|
| `purify` | 净化（移除腐化/猩红/神圣） |
| `corruption` | 转为腐化 |
| `crimson` | 转为猩红 |
| `hallow` | 转为神圣 |

---

#### `set_visibility`

切换方块/墙的隐形状态。

**请求 JSON：**
```json
{
  "invisible_block": 1,   // 1=隐形, 0=显示, null=不改
  "invisible_wall": 1
}
```

---

#### `remove_all_wires`

移除所有电线。

**请求 JSON：** `{}`（无参数）

---

### 像素画映射

#### `apply_pixel_art_mapping`

将图片文件（JPEG/PNG）映射为 Terraria 方块，放置到世界指定位置。WASM 内部加载和解码图片，使用 TXCI v3 色彩索引进行 O(1) 颜色查找。内存由 WASM 内部管理，无需手动释放。

**请求 JSON：**
```json
{
  "image_path": "/path/to/image.png",  // 图片文件路径（必需，支持 JPEG/PNG）
  "lut_path": "/path/to/index.txci",   // TXCI v3 文件路径（必需，也接受 txci_path）
  "start_x": 3816,                     // 世界中放置起始 X 坐标
  "start_y": 816,                      // 世界中放置起始 Y 坐标
  "prefer_wall": false,                // 优先使用墙壁而非方块
  "block_inactive": false,             // 放置的方块设为非激活状态
  "mapping_json": {                    // 颜色覆盖映射（可选）
    "mappings": [
      { "color": "#FF0000", "blockId": 166, "blockType": "blocks", "paintId": 0 },
      { "color": "#0000FF", "blockId": 54,  "blockType": "walls",  "paintId": 0 },
      { "color": "#000000", "blockId": 0,   "blockType": "empty",  "paintId": 0 }
    ]
  },
  "output_dir": "/path/to/output",     // 输出目录（可选，保存修改后的 .wld）
  "output_filename": "pixel_art.wld"   // 输出文件名（可选）
}
```

**使用示例：**
```javascript
const req = JSON.stringify({
    image_path: '/path/to/pixel_art.png',
    lut_path: 'data/terraria_color_index.txci',
    start_x: 100, start_y: 200,
    output_dir: 'output', output_filename: 'result.wld'
});
// 调用: terra_op_execute_json(world, "apply_pixel_art_mapping", req)
```

**响应 JSON：**
```json
{
  "status": "ok",
  "replaced_tiles": 589824,
  "total_tiles": 20160000
}
```

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

### 占位操作（未完全实现）

| 操作名 | 状态 | 说明 |
|--------|------|------|
| `mark_chest_items_preview` | 占位 | 返回空标记 |
| `mark_chest_items_map` | 占位 | 返回空标记 |
| `mark_tiles_and_chests_preview` | 占位 | 返回空标记 |
| `unlock_bestiary` | 不支持 | 返回 `TERRAX_NOT_SUPPORTED`，避免未修改字节却报告成功；请使用 `replace_bestiary` |

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
const status = terra_op_execute_json(world, "render_preview_rgba", "{}", 0, 0n, rp);

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
