# TerraWasm

将 Terraria 世界文件（`.wld`）的解析、编辑、渲染能力编译为 WebAssembly 的纯 C17 库。通过 Emscripten 编译，提供 Node.js 和 Web 两个目标。

## 功能

- **世界解析**：版本 88-400，所有 11 个 section 的只读 JSON 序列化
- **渲染**：RGBA 预览、PNG 缩略图、.map 地图文件生成
- **编辑**：安全 header 布尔补丁、宝箱/图鉴二进制替换、批量方块更新、生物群系转换、可见性切换、电线移除
- **像素画映射**：将 RGBA/索引像素映射为 Terraria 方块（TXCI v3 色彩索引）
- **地图标记**：在 .map 文件中标记指定箱子和方块位置

## 项目结构

```
TerraWasm/
├── include/                # 头文件
│   ├── terra_types.h       # 核心类型定义（TxTile, TxWorld 等）
│   ├── terra_world.h       # V2 API 公开头文件
│   ├── terra_txci.h        # TXCI v3 色彩索引 API
│   └── terra_color_data.h  # 内置方块/墙壁颜色数据
├── src/                    # C 源文件
│   ├── terra_mem.c         # 桥接/原生双域跟踪分配器
│   ├── terra_json.c        # 手写 JSON 解析/构建
│   ├── terra_wld.c         # WLD 二进制解析（~2090 行）
│   ├── terra_api.c         # V2 API 实现
│   ├── terra_ops.c         # 操作分发器（14 个操作）
│   ├── terra_mutators.c    # header/chests/bestiary 验证与 WLD 二进制编码
│   ├── terra_render.c      # 渲染管线（颜色系统、PNG 编码）
│   ├── terra_map.c         # .map 文件生成（64x64 分块）
│   ├── terra_update.c      # 流式方块修改
│   ├── terra_txci.c        # TXCI v3 色彩索引加载器
│   └── terra_pixel_art.c   # 像素画映射实现
├── scripts/                # 构建/数据脚本
│   ├── terrax_color_index_v3_builder.py  # TXCI 生成器
│   ├── build_txci.py       # TXCI 构建流水线
│   └── ...
├── tests/                  # 测试文件
├── data/                   # 数据文件
│   ├── terraria_color_index.txci  # TXCI v3 色彩索引（~7MB）
│   └── extracted/          # 从游戏提取的颜色数据
├── docs/                   # 文档
│   └── API.md              # API 参考文档
├── build/                  # 构建输出（.wasm + .js）
├── CMakeLists.txt          # 构建配置
├── exports.txt             # WASM 导出函数列表
└── build.ps1               # 构建脚本
```

## 前置要求

- [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)（`D:\Tool\emsdk`）
- [CMake](https://cmake.org/) 3.27+
- [Node.js](https://nodejs.org/) 18+
- Python 3.8+（仅用于 TXCI 数据生成）

## 构建

### 1. 生成 TXCI 色彩索引（一次性）

```powershell
pip install numpy scipy
python scripts/build_txci.py
```

产出：`data/terraria_color_index.txci`（~7MB）

### 2. 编译 WASM

```powershell
.\build.ps1                          # 编译 node + web 两个目标
.\build.ps1 -Target node             # 仅编译 Node.js 目标
.\build.ps1 -Target web              # 仅编译 Web 目标
.\build.ps1 -Quick                   # 跳过 CMake configure（增量编译）
.\build.ps1 -Test                    # 编译后运行测试
```

产出：
- `build/terrax_world_wasm.js` + `.wasm`（Node.js 目标，128 MiB 初始内存，512 MiB 最大内存）
- `build/terrax_world_wasm_web.js` + `.wasm`（Web 目标，32 MiB 初始内存，96 MiB 最大内存）

编译完成后会在 `build/terra.manifest.json` 写入 ABI、源码 commit、dirty 状态、Node/Web 导出集合与导出哈希、公共与目标专属构建 flags、内存预算，以及每个交付产物的 byte size 和 SHA-256。构建不会自动修改其他仓库。

只有明确传入 `-DeployDir` 才会部署 Web manifest 白名单中的 wrapper 和 `.wasm` 两个文件；脏工作树默认拒绝部署。`-AllowDirty` 仅用于本地诊断，不能产生可发布部署。

发布默认仍使用 `-O3`。如需比较更激进的体积优化配置，可在不改源码的前提下执行：

```powershell
.\build.ps1 -Target all -OptimizeFlag '-Oz' -EnableLto
```

这条命令只用于对比正确性、体积和运行表现；当前仓库不会在没有重新验证的情况下把发布默认值从 `-O3` 改成 `-Oz + LTO`。

2026-07-29 使用 Emscripten 5.0.7 的实测结果：

| 配置 | Web wrapper | Web Wasm | 回归结果 |
|---|---:|---:|---|
| `-O3` | 64,184 B | 204,336 B | Node 48/48、兼容套件 41/41 通过 |
| `-Oz + LTO` | 65,094 B | 150,439 B | Node 48/50；两项多实例化内存回归失败，兼容套件未继续 |

因此发布配置保留 `-O3`。`-Oz + LTO` 虽减少约 53 KiB Wasm，但未通过正确性门禁；在该问题解决前不比较或采用其运行时性能数据。

Web 交付包有显式体积门禁：

- wrapper 上限：`128 KiB`
- wasm 上限：`256 KiB`

可以单独运行：

```powershell
node scripts/check-artifact-size.mjs build/terra.manifest.json
```

该门禁会同时校验 manifest 记录的 byte size 与磁盘实际文件是否一致。

CI 工作流位于 `.github/workflows/quality.yml`，当前包含：

- 原生 `cmake + ctest` 合约门禁
- `ASan/UBSan + fuzz smoke` 门禁
- 固定 `Emscripten 5.0.7` 的 Node/Web 发布构建门禁
- manifest 产物大小门禁与可追溯 artifact 上传

### 3. 运行测试

```powershell
.\build.ps1 -Test                    # 编译并运行缩略图测试
node tests/test_all.js               # 综合兼容测试
node --test tests/test_section_mutators.js # 安全 section mutator/内存测试
node tests/test_thumbnail.js         # 缩略图渲染
node tests/test_pixel_art_mapping.js # 像素画映射（13 项）
node tests/test_mark_tiles_map.js    # 地图标记（13 项）
```

## API 使用

### 像素画映射

#### 高级 API：`applyPixelArt`

一体化像素画写入，WASM 内部完成 TXCI 加载、颜色匹配、像素画排队。

```javascript
const tx = useTerrax()
const world = tx.openWorld(wldBuffer)

tx.applyPixelArt(world, {
  pixels: rgbaUint8Array,      // RGBA 像素数据（4 字节/像素）
  width: 768,                  // 图片宽度
  height: 768,                 // 图片高度
  txciGz: txciGzBuffer,        // TXCI gzip 数据（.txci.gz 文件内容）
  startX: 100,                 // 世界坐标 X
  startY: 200,                 // 世界坐标 Y
  preferWall: false,           // 优先使用墙壁匹配
  blockInactive: false,        // 方块虚化
  overrides: [...]             // 可选：颜色覆盖映射（见下文）
})

const result = tx.saveWorld(world)  // 保存时流式应用像素画
```

#### 颜色覆盖映射（overrides）

`overrides` 数组允许对特定颜色自定义映射，优先于 TXCI 自动匹配。

**格式：**

| 类型 | 字段 | 说明 |
|------|------|------|
| 空方块 | `{ r, g, b, a, active: false }` | 清除一切（无 tile、无 wall、无液体） |
| 方块 | `{ r, g, b, a, tileType, tileColor?, blockInactive? }` | 放置方块，可选油漆和虚化 |
| 墙壁 | `{ r, g, b, a, wallType, wallColor? }` | 放置墙壁（自动清除原方块），可选油漆 |
| 保留原样 | 不放入 overrides | 该颜色使用 TXCI 自动匹配 |

**示例：**

```javascript
overrides: [
  // 白色 → 空方块（挖空）
  { r: 255, g: 255, b: 255, a: 255, active: false },

  // 红色 → 方块 166 + 红色油漆
  { r: 255, g: 0, b: 0, a: 255, tileType: 166, tileColor: 1 },

  // 蓝色 → 墙壁 4 + 蓝色油漆
  { r: 0, g: 0, b: 255, a: 255, wallType: 4, wallColor: 9 },

  // 灰色 → 虚化方块
  { r: 128, g: 128, b: 128, a: 255, tileType: 1, blockInactive: true },

  // 绿色 → 不放入 overrides，使用 TXCI 自动匹配
]
```

**油漆 ID 参考：**

| ID | 颜色 | ID | 颜色 |
|----|------|----|------|
| 0 | 无油漆 | 16 | 淡绿 |
| 1 | 红色 | 17 | 绿色 |
| 2 | 橙色 | 18 | 淡蓝 |
| 3 | 黄色 | 19 | 青色 |
| 4 | 淡黄绿 | 20 | 蓝色 |
| 5 | 绿色 | 21 | 紫色 |
| 6 | 青绿 | 22 | 品红 |
| 7 | 青色 | 23 | 粉红 |
| 8 | 淡蓝 | 24 | 淡粉 |
| 9 | 蓝色 | 25 | 暗影 |
| 10 | 紫色 | 26 | 白色 |
| 11 | 品红 | 27 | 灰色 |
| 12 | 粉红 | 28 | 棕灰 |
| 13-25 | 同 1-12（深色变体） | 29 | 暗黑 |

#### 低级 API：`queuePixelArt`

完全自定义映射数组，无需 TXCI：

```javascript
tx.queuePixelArt(world, {
  pixels: rgbaUint8Array,
  width: 768,
  height: 768,
  startX: 100,
  startY: 200,
  skipTransparent: true,
  mappings: [
    { r: 255, g: 0, b: 0, a: 255, tile_type: 166, tile_color: 1, active_mode: 1 },
    { r: 0, g: 0, b: 255, a: 255, wall_type: 4, wall_color: 9, active_mode: 2 },
    { r: 255, g: 255, b: 255, a: 255, active_mode: 0 },  // 空方块
  ]
})
```

#### TXCI 色彩索引构建

```powershell
# 使用自定义 tile 白名单构建 TXCI
python scripts/terrax_color_index_v3_builder.py \
  --colors data/extracted/colors.generated.json \
  --out-dir data \
  --name terraria_color_index \
  --tile-whitelist "0,1,6,7,8,9,22,25,..." \
  --variant-mode zero \
  --brick-size 8

# 压缩为 gzip（用于 WASM 加载）
gzip -k data/terraria_color_index.txci
```

### 地图标记

```javascript
const req = JSON.stringify({
    output_dir: "output",
    chest_markers: [
        { item_id: 49, color: "#FF2020C8" }  // 标记含有生命水晶的箱子
    ],
    tile_markers: [
        { tile_type: 4, color: "#20A0FFFF" }  // 标记火把
    ]
});
// 调用: terra_op_execute_json(world, "mark_tiles_and_chests_map", req)
```

## 设计原则

- **纯 C17**：无 C++ 运行时、无 libc 依赖（手写 memset/memcpy/strlen）
- **流式处理**：方块从不完整物化为数组，逐个读取处理
- **双域跟踪分配器**：bridge 指针逐个释放，native root 按 world/operation 生命周期回收
- **手写 PNG/zlib**：固定 Huffman + LZ77，无外部依赖
- **stb_image**：JPEG/PNG 图片解码（唯一的外部头文件库）
- **Section Override**：修改存储为覆盖层，保存时重建文件
