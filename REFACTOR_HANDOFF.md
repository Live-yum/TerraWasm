# refactor 线上续做交接（2026-10-24）

用户要求把当前检查点提交并推送到 `refactor`，随后在线上继续。未修改或合并到 `master`。

## 本次原生提交

- `03f66ff`：非法 tile 及完整候选 tile/RLE/footer 验证；Windows exclusive temp 写入；Win64 内存函数声明等修复。
- `e002243639d9d7efa3f0cf106d99057b658ebf28`：有效测试世界 footer 和规范 LF HELLO fixture。
- `4a8bde75d1307223aaadd404bc3380943537955f`：把 BASIC wrapper 的 memory history/handoff/growth fence 纳入可复现 producer 构建，模板漂移时拒绝生成。

配套 [viewer-app refactor](https://github.com/Live-yan/viewer-app/tree/refactor) 已同步 `4a8bde` 的 Web wrapper、Wasm、完整 manifest 和浏览器 manifest；BASIC 二进制大小/hash pin 也由相同同步步骤生成。此后的文档提交没有修改产物来源。

## 已验证

- 最新 producer Node/Web 测试 233/233；legacy 38/38。
- 此前 native profiles CTest 54/54。
- 真实大世界 checkpoint、OOM rollback、候选读回测试通过。
- Web Wasm：474,863 bytes；SHA-256 `6905d558b4a302f2b83c5b31f8fa667d296dcd2de97a0ce9891d302bb4068027`。

## 未完成与后续检查

1. 前端完整重构尚未完成。具体 Circuit/Fusion writer、v3 准备、runtime 压力和测试失败见 [viewer-app 交接文档](https://github.com/Live-yan/viewer-app/blob/refactor/REFACTOR_HANDOFF.md)。
2. viewer 大世界 20 次编辑生命周期测试曾在 300 秒超时；需要单独复现，确定完整候选校验的成本或前端等待点。不能把原生定向通过等同该集成测试通过。
3. Windows Unicode 文件路径、完整私有游戏语料、真实游戏客户端载入和真机物理峰值没有完成全量验收。现有 ASCII corpus 的通过不能替代这些边界。
4. 前端最终合并树必须重新运行 artifact/manifest 和 native streaming 集成检查。不要复制未验证 wrapper 或手工改 BASIC hash 绕过来源校验。
5. 本次未发布二进制 release、未部署、未改 backend。保持 v3 migration 关闭。

主分支核对点：`1181281d7e9f78f547191f529fb57bf95925c265`。原始用户世界与资源路径未添加到 Git。
