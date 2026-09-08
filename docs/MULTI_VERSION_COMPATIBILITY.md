# 多版本兼容

本文的版本号是 `.wld` / `.plr` 文件头中的 release 整数，不是游戏展示版本号。实现依据仓库 `code/` 中的 `WorldFile.cs`、`Player.cs`、`BannerSystem.cs`、实体类和 `NPCID.cs`；此次核对的源码子模块提交为 `8255d34616c780af12079425ac92a0a7aed87d71`。

| 文件与格式版本 | 支持范围 | 验证方式 |
| --- | --- | --- |
| WLD 1–87 | 连续旧布局读取、header/瓦片/宝箱/告示牌/NPC/footer、预览、原字节保存 | 独立构造的全部 87 个 release；Node/Web 同步和任务式打开；截断样本与特殊帧门槛 |
| WLD 88–326 | 分段读取和现有编辑能力；根据 release 解释可选段 | 已有真实世界回归；128/129/139/195/196 独立 header 样本及保存重开；原生 NPC/实体/footer/瓦片契约 |
| PLR 1–326 | 加密读取、语义编辑、保存重开，按 Player.Deserialize 归一化 | 全 release 矩阵；1/37/38/58/59 独立编码样本；真实 v326 玩家文件 |
| PLR 327+ | 沿用已有策略，仅尝试最新已知布局；完整解析失败时报 newer-layout 错误 | 327/400 同布局与新增未知字节反例，不代表支持未知的未来布局 |

## 边界处理

- WLD 1–87 尚无旧格式编辑编码器，现代编辑操作返回不支持；读取后保存保留输入全部字节。任务式打开接口可用，但连续旧布局的扫描在一次解析阶段完成，不保证每个 step 都满足耗时预算。
- WLD 1–37 宝箱物品保留 `legacyName`，`itemType` 为 0。旧 NPC 名称转换为数字 `npcNetId`，同时保留 `legacyTypeName`；31–83 的独立 NPC 命名表也会读取。不存在的可选段返回空集合，图鉴保持对象结构；1–6 没有 footer，其 JSON 标记 `present:false`。
- 现代头字段修正了 v128 快进、v129 税收官、v131 晚期事件、v196 背景门槛；旧 NPC 类型字符串、v140 常驻 NPC 列表、v213 variation 位均按版本读取。
- 实体数量使用 Int32，DisplayDoll 的 307/308/311/312 布局分开处理。瓦片遵循墙低位、墙漆、液体、墙高位、RLE 的顺序，拒绝截断和非法重复长度。
- footer 使用实际版本对应的物理段；修改世界名称或 ID 时同步更新 footer。格式版本 patch 不能跨越其它段的布局门槛，也不能超过 WLD 326 上限。
- PLR 未编辑保存保持原始密文；编辑后按游戏加载规则归一化外观、生命/魔力和复活时间等字段。1–37 的名称物品不会静默接受不可表示的数值 ID、装备数量或早期 prefix 写入。

## 验证与限制

从根目录运行 `TerraWasm/build.ps1 -Features all -Target all -AllowDirty -Test` 可构建并运行 Node/Web 回归。原生测试通过 CMake 的 `TERRAWASM_NATIVE_TESTS=ON` 启用；`TERRAWASM_SANITIZERS=ON` 启用 ASan/UBSan。`scripts/generate_legacy_npc_names.py --check` 检查 473 条名称映射与源码一致。

独立历史样本按源码字段顺序构造，不调用待测编码器生成输入；PLR 全 release 矩阵另有编码/解码往返检查。这些测试不能替代所有历史游戏版本生成的真实存档，目前没有外部真实 WLD 1–87 或 PLR 1–37 样本。低版本新增路径保留原文件且限制编辑，便于进一步用真实样本验证。

### 本次验证结果

- Node/Web 完整回归：107/107；原有操作套件：38/38。
- 原生 Release 契约：12/12，包含旧 NPC、实体、footer、瓦片和增量打开检查。
- 最终 Web WASM：322,219 字节，低于既有 327,680 字节上限；8400×2400 MAP 基准通过。
- ASan/UBSan 联合构建：10/12 通过；`terra_error_json_contract`、`terra_json_parser_contract` 出现间歇性 SIGSEGV。关闭 ASan 自身 SIGSEGV 处理后仍可复现；单独重复 JSON 测试曾出现 4 次成功、1 次失败，尚未定位原因。这项检查未计为通过，不据此声称内存检查全部完成。

构建及测试的完整日志保存在本地 `build/compatibility-validation.log`。产物清单标记为 dirty 本地验证构建，未提交或发布。
