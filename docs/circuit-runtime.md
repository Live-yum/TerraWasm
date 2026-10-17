# 电路原生源码与统一模块验证

## 唯一源码位置

电路实验室原来保存在 `viewer-app/features/circuit/native/` 的 overlay 已合入本仓库：

- `src/terra_circuit_{vm,world,compile,rules,devices,order}.c`
- `src/terra_circuit_projectiles.inc`、`src/terra_circuit_world_internal.h`
- `include/terra_circuit_vm.h`
- `tests/circuit/`：projectile、PixelBox 与 helper-level C 合约

迁移前逐文件核对 overlay 和合并结果（忽略 CRLF/LF 差异），没有替换上游其他源码。原 overlay 基线为 `81ad7d47988f8a4d375428ec4b7f73143108d2a8`；原电路产物 snapshot 为 `739e71254fb8e078d721b5cd9e662313e3a298f6`。这些是历史来源，不是当前工作树的构建身份。

以后直接修改这里的源码，不再复制 CMake、C 源文件或 patch 到 viewer-app。离线原生工具位于 `tools/`，其中 stable RGB 生成器仍由 viewer-app 的 `scripts/generate-stable-rgb.mjs` 调用。

## 保持的语义和边界

参考 TerrariaDecompiledSource `8255d34616c780af12079425ac92a0a7aed87d71`：

- 逻辑灯/门按 source FIFO 与接线盒路径恢复命中顺序，取消/重试保留原子恢复。普通设备副作用仍按编译网络端口顺序执行，不宣称完整原版顺序兼容。
- 原版 PixelBox 仅在同一次 TripWire 横纵同时命中时翻转；单轴不变，不同 TripWire 不配对。TWLD 模组规则保持独立。
- 支持 601 Portal Gun Station 电路 transport 和 186 Spear Trap style 4；支持 442 压板闭区间进入检测、冷却、取消恢复和 tick 顺序。
- 601 遇斜坡明确返回 unsupported。186 的 AI SolidCollision 不等同完整斜坡物理。
- 投射物最多 1000 槽，空槽优先，满池替换剩余寿命最短者，同值取低槽号。
- 未实现完整玩家/NPC/伤害、portal 实体、全部投射物与世界边缘行为。

## 本地验证

统一功能集已由 Emscripten 5.0.7（`D:\Tool\emsdk`）构建，输出在忽略的 `build-unified/` 和 `artifacts/unified/build/`。该试验产物为 `dirty: true`，不得当作可发布产物同步。

在 viewer-app 根目录（Bash）：

```bash
TCW_BUILD_DIR="$PWD/../TerraWasm/artifacts/unified/build" \
TERRA_CIRCUIT_BUILD_DIR="$PWD/../TerraWasm/artifacts/unified/build" \
node --test ../TerraWasm/tests/circuit/projectile-contract.cjs \
  ../TerraWasm/tests/circuit/pixelbox-contract.cjs \
  ../TerraWasm/tests/test_circuit_world.js \
  ../TerraWasm/tests/test_circuit_fragments_supports.js \
  ../TerraWasm/tests/test_hello_display.js ../TerraWasm/tests/test_circuit.js
```

47 项通过，包含真实 Node/Web WASM、电路百万格、世界保存重开、HELLO、取消恢复、601/186 与 PixelBox。`build.ps1 -Test` 也已纳入两个迁入的 JS 合约。helper C 合约需另行用 host GCC 编译，不能把上述结果当成 host C 验证。

## 客户端统一进展（已完成迁移）

后续已完成 all Web compact 导出、角色/电路/世界共享产物、小程序独立引擎分包及 H5 去重。clean 构建 `0ffa3e8d59ee5dc10a6bad3a51c133f1bda166bd` 的 Web WASM 为 453,818 bytes，原体积门禁通过。最新交付与验证记录见 `viewer-app/docs/wasm-workspace-artifact-sync.md`。

以下为第一阶段的历史状态，不再代表当前客户端：

## 第一阶段记录

viewer-app 仍使用原来三套已校验产物，未替换二进制、manifest 或运行实例。原电路同步脚本迁到 `viewer-app/scripts/sync-terrawasm-circuit.mjs`，行为不变。

统一 all 模块 Web WASM 为 467,803 字节，超出当前 all 门禁 458,752 字节；未放宽门禁。后续需要：

1. 收敛 all Web 导出和运行时 profile，在保留角色 ABI 的前提下通过体积门禁。
2. 同步修改浏览器 manifest 的 WLD-only 校验、角色 PLR-only 身份校验与 player workspace 声明。
3. 将三个加载器、Worker、微信分包、H5 离线缓存、复制/校验脚本和测试迁到同一产物路径。
4. 保留不同工作空间的实例隔离、内存记账、超时与未保存数据保护。
5. 使用真实 clean 源码提交重建，再原子同步产物；不得修改 dirty 字段掩盖来源。
6. 完成 H5/微信构建、分包体积、角色/世界 round-trip 和功能回归后，才删除旧产物。
