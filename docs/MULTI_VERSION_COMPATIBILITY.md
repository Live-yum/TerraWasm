# 多版本兼容

本文的版本号是 `.wld` / `.plr` 文件头中的 release 整数，不是游戏展示版本号。实现依据仓库 `code/` 中的 `WorldFile.cs`、`Player.cs`、`BannerSystem.cs`、实体类和 `NPCID.cs`；此次核对的源码子模块提交为 `8255d34616c780af12079425ac92a0a7aed87d71`。

| 文件与格式版本 | 支持范围 | 验证方式 |
| --- | --- | --- |
| WLD 1–87 | 连续旧布局读取、header/瓦片/宝箱/告示牌/NPC/footer、预览、原字节保存 | 独立构造的全部 87 个 release；Node/Web 同步和任务式打开；截断样本与特殊帧门槛 |
| WLD 88–326 | 分段读取和现有编辑能力；根据 release 解释可选段 | 已有真实世界回归；128/129/139/195/196 独立 header 样本及保存重开；原生 NPC/实体/footer/瓦片契约 |
| PLR 1–326 | 加密读取、语义编辑、保存重开，按 Player.Deserialize 归一化 | 全 release 矩阵；1/37/38/58/59 独立编码样本；真实 v326 玩家文件 |
| WLD / PLR 327+ | 尝试已知布局；完整结构验证成功后只读，原字节导出；结构不符时报错 | WLD 326→328 / INT32_MAX、PLR 326→328，以及错误结构反例；不保证任意未来布局 |

## 边界处理

- WLD 1–87 尚无旧格式编辑编码器，现代编辑操作返回不支持；读取后保存保留输入全部字节。任务式打开接口可用，但连续旧布局的扫描在一次解析阶段完成，不保证每个 step 都满足耗时预算。
- WLD 1–37 宝箱物品保留 `legacyName`，`itemType` 为 0。旧 NPC 名称转换为数字 `npcNetId`，同时保留 `legacyTypeName`；31–83 的独立 NPC 命名表也会读取。不存在的可选段返回空集合，图鉴保持对象结构；1–6 没有 footer，其 JSON 标记 `present:false`。
- 现代头字段修正了 v128 快进、v129 税收官、v131 晚期事件、v196 背景门槛；旧 NPC 类型字符串、v140 常驻 NPC 列表、v213 variation 位均按版本读取。
- 实体数量使用 Int32，DisplayDoll 的 307/308/311/312 布局分开处理。瓦片遵循墙低位、墙漆、液体、墙高位、RLE 的顺序，拒绝截断和非法重复长度。
- footer 使用实际版本对应的物理段；修改世界名称或 ID 时同步更新 footer。格式版本 patch 不能跨越其它段的布局门槛（包括 195/196 背景、268 NPC 前缀和 289 旗帜数据），写入目标仍不能超过已验证的 WLD 326 布局。
- PLR 未编辑保存保持原始密文；编辑后按游戏加载规则归一化外观、生命/魔力和复活时间等字段。1–37 的名称物品不会静默接受不可表示的数值 ID、装备数量或早期 prefix 写入。

## 验证与限制

从根目录运行 `TerraWasm/build.ps1 -Features all -Target all -AllowDirty -Test` 可构建并运行 Node/Web 回归。原生测试通过 CMake 的 `TERRAWASM_NATIVE_TESTS=ON` 启用；`TERRAWASM_SANITIZERS=ON` 启用 ASan/UBSan。`scripts/generate_legacy_npc_names.py --check` 检查 473 条名称映射与源码一致。

独立历史样本按源码字段顺序构造，不调用待测编码器生成输入；PLR 全 release 矩阵另有编码/解码往返检查。这些测试不能替代所有历史游戏版本生成的真实存档，目前没有外部真实 WLD 1–87 或 PLR 1–37 样本。低版本新增路径保留原文件且限制编辑，便于进一步用真实样本验证。

### 本次验证结果

- Node/Web 完整回归：107/107；原有操作套件：38/38。
- 原生 Release 契约：13/13，包含旧 NPC、实体、footer、瓦片和增量打开检查。
- 最终 Web WASM：322,219 字节，低于既有 327,680 字节上限；8400×2400 MAP 基准通过。
- ASan/UBSan 联合构建：10/12 通过；`terra_error_json_contract`、`terra_json_parser_contract` 出现间歇性 SIGSEGV。关闭 ASan 自身 SIGSEGV 处理后仍可复现；单独重复 JSON 测试曾出现 4 次成功、1 次失败，尚未定位原因。这项检查未计为通过，不据此声称内存检查全部完成。

构建及测试的完整日志保存在本地 `build/compatibility-validation.log`。产物清单标记为 dirty 本地验证构建，未提交或发布。


## 未知新版本安全读取（2026-10-02）

读取接受正 Int32 release，不先按 326 拒绝。326 仅是已验证的写入布局边界；不把来源版本改为 326。参考源固定为 `Live-yum/TerrariaDecompiledSource@8255d34616c780af12079425ac92a0a7aed87d71`，尤其 `WorldFile.LoadWorld_Version2` 的逐段精确消费及 `FileMetadata.Read` 的签名/类型验证；保留已有 xindong 分支。

- WLD 格式 JSON 增加 `originalVersion`、`readOnly`、`compatibility` 和 `canExportOriginal`；future 成功读取时兼容性为 `future-layout-readonly`。不增加导出函数、不更改 ABI 版本。
- 未来 WLD 校验完整 section table、首段精确起点、已知 11 段、所有已知段完整消费、footer 与头名称/ID一致、tile bitmap/type、RLE 不跨列、未知实体/creative payload。未知字段、未知段、未消费尾字节不会被当作解析成功。
- 元数据保留流式路径相同的 16 MiB 预算；来源文件至多 200,000,000 bytes，世界至多 200,000,000 tiles。限制属于资源保护，非 release 白名单。原有历史分支保持不变。
- core 层拒绝未来文档的 header/chest/bestiary/tile、binary command、pixel 和 stream mutation，返回 `TERRAX_FUTURE_VERSION_READ_ONLY`。同步存档直接复制原始文件；stream `save` 原样转发来源范围，不重写头、pointer 或 tiles。stream 调用方必须保持来源 ID 对应的字节不可变，直至 close。
- PLR 来源密文与版本保持不变；setter、set-many、replace、patch及旧拼写别名不能解锁未来版本。JSON 创建/转换目标保留已验证的 1–326 范围。PLR 语义 JSON 结构不增加自定义字段。
- 326→328 是同布局测试；238→240 跨真实 dontStarve/deerclops 条件，不能作为“只改版本”的兼容证明。

本地验证：新增原生 future WLD 合约及 PLR 来源只读合约通过；15 项可运行 Release 与 ASan/UBSan 测试通过（包括全部 87 个旧版合成布局的完整打开/原样保存）。现环境缺少仓库 LFS 的 `native-terraria-header.wld`，对应旧测试未计入通过；LeakSanitizer 在此环境因 ptrace 不可用。私有 CI 保持完整 native、ASan/UBSan/LSan、Node/Web及feature matrix验证，并上传可追溯 WLD/PLR 产物。上述核心测试不代表用户端完整工作流已达到 200,000,000 bytes / 8 秒目标，仍需用最终 viewer 验证。

审查发现并修复了 GCC -O3 将自实现 memset 字节循环优化成自身递归的问题；memset/memcpy 均改用 native/Emscripten 工具链 libc。原生 Release 1–87 完整 open/save 矩阵及独立审查驱动全部通过。未来元数据还覆盖了 15,728,640 个空字符串的最坏密度：本地 Release 同步 task-begin 扫描约 0.08 秒。此路径仍有同步扫描，并不承诺每个增量 step 的固定毫秒预算；WASM 实测与完整 viewer 8 秒预算另行验证。

### Combined build size and performance

The default `all` Web/Node build uses `-Oz` with LTO; standalone `wld` and `plr` viewer builds retain `-O3` with LTO disabled. Explicit `-OptimizeFlag` and `-EnableLto:$false` arguments override these defaults. The Web size gate remains 327,680 bytes.

An isolated comparison of the same C source and existing 8400×2400 fixture passed all 148 combined contracts under each tested profile, including future strict reads, immutable version-reset guards, and original stream exports. `-O3` produced 375,564 Web WASM bytes with a 574.2 ms MAP render and an 884.3 ms full-preview process. `-Oz` + LTO produced 317,080 bytes, 813.1 ms MAP render, and a 953.8 ms full-preview process. The compact profile's Web preview used 104,529,920 linear-memory bytes with a 99,164,329-byte tracked heap peak. These single-run core measurements describe the size/speed tradeoff; they do not certify browser/UI end-to-end latency or total process memory for every file up to 200,000,000 bytes. The independently delivered viewer modules do not use the compact combined profile.
