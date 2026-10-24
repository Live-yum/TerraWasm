# refactor 交接与实际构建证据（2026-10-24）

本轮只提交 `refactor`，已建立 [PR #44](https://github.com/Live-yum/TerraWasm/pull/44)，未修改或合并到 `master`。下面是本轮完整 producer 验收；viewer 的整体重构结论仍由其最终集成结果决定。

## 当前实现与来源

- `610ae8e43ed845d23ecd5aced5524a49770683a0`：取得资源后禁止后续分配的原生清理回归；完整 Node/Web 四产物校验；compiled workspace ABI 与 manifest 对齐；补齐全部 feature profile 的构建验证。
- `b4699e070345b7e343a53df5f5e8376306f388bd`：Windows PLR 严格 UTF-8 转 UTF-16，候选创建、读回、替换和删除全部使用 wide API；加入真实文件替换失败及中文、emoji 路径测试。
- `4d81e63cb4e8ca08c1c1b65b4c3a7977429b8928`：修复实际 Windows CI 发现的 MSVC C `max_align_t` 缺失，保留非 MSVC 布局，并验证实际返回地址的对齐。

最终 all/WLD/PLR 均从已经发布的 **`4d81e63cb4e8ca08c1c1b65b4c3a7977429b8928`** 干净源码实际编译；Emscripten 固定为 **5.0.7**。此后的文档提交不改变这批产物的来源。每个 profile 保留 Node wrapper/Wasm、Web wrapper/Wasm 和完整 manifest 五个文件；校验器要求显式的 40 位预期 SHA，并同时验证字节、编译来源、能力、ABI、导出及内存配置。

配套 [viewer-app refactor](https://github.com/Live-yan/viewer-app/tree/refactor) 已通过其同步脚本接入这批 **all** Web wrapper/Wasm、完整 manifest 和浏览器投影；未把独立 WLD/PLR 替换进 unified consumer。

## 已完成的验收

| 验收 | 结果 |
| --- | --- |
| 原生 CTest | 56/56 |
| ASan、UBSan、LeakSanitizer CTest | 56/56 |
| all Node/Web | 242/242，零跳过、取消 |
| WLD Node/Web | 218/218，零跳过、取消 |
| PLR Node/Web | 69/69，零跳过、取消 |
| all、WLD legacy | 各 38/38 |
| all、WLD 8400×2400 MAP benchmark 与各 profile 预览 | 通过 |
| 全四产物校验、原有包体上限 | 三个 profile 均通过 |
| Windows 原生 PLR 文件、allocator、native span | 实际 CI 3/3 |
| 最终 push 和 PR quality workflow | 各 7/7 job success，包括公开大世界 Circuit acceptance |

实际 `all` Web wrapper 为 **24,469 bytes**，Web Wasm 为 **474,863 bytes**，后者 SHA-256 为 **`07f00c021de9f4531bfa2cff537f3de95694f08c1c48186b961531e1be6e32f1`**。旧 `4a8bde`/`5e3f86` 产物不再作为本轮通过证据。详细哈希、运行链接和边界见 [完整验收记录](docs/acceptance/refactor-final-validation-2026-10-24.md) 及 [机器可读记录](docs/acceptance/refactor-final-validation-2026-10-24.json)。

## 验收边界与后续消费要求

1. viewer 的 identity、READY、journal、累计内存及 H5/小程序完整测试需使用上述最终产物独立验收；不能由这里的 producer 通过替代。前端结果见 [viewer-app 交接文档](https://github.com/Live-yan/viewer-app/blob/refactor/REFACTOR_HANDOFF.md)。
2. Windows 已实际覆盖 PLR 本地 Unicode 路径、候选失败清理和禁止 delete sharing 的替换失败；不将其扩大为所有 WLD 路径、远程文件系统或断电持久性结论。详见 [PLR 候选验证边界](docs/plr-candidate-validation.md)。
3. 完整私有游戏语料、真实游戏客户端载入和真机物理内存峰值仍需要相应环境。公开语料、逻辑内存账和无设备参与的测试不能替代这些证据。
4. 后续生产代码改动须重新编译并重新同步完整 manifest 与对应字节；不得手改 BASIC hash 或混用旧 Wasm。本轮没有提高任何既有预算，也没有用重建 owner/Worker 掩盖连续清理失败。
5. 未发布二进制 release、未部署、未改 backend；v3 migration 仍关闭。

主分支核对点：`1181281d7e9f78f547191f529fb57bf95925c265`。GitHub PR 的临时合并检查不表示向主分支合并。原始用户私有世界与资源路径未添加到 Git。
