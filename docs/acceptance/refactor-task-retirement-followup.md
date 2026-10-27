# 增量打开任务的清理与移交修复

检查基线：`681c93f7deeca82c82ce564b719effa008e7ec10`。本次修复保持现有导出、ABI 版本、功能集合和内存上限。

## 可复现的问题

### 关闭被拒绝，却丢失任务所有权

增量打开完成后，`terra_world_task_get_world_handle` 可以取得尚未由
`terra_world_open_finish` 移交的世界句柄。若调用方使用该句柄建立电路会话，
世界会被电路租约保护，`terra_world_close` 会返回 `STATE_ERROR`。

原来的 `terra_world_open_cancel` 和 `terra_world_task_close` 忽略该结果，
仍清空 `world_handle`，甚至销毁任务并解除其堆重置保护。世界实际仍然存活，
任务却已经报告清理完成；后续只重试任务清理也无法回收这个世界。

现在仅在世界已经退休或成功关闭后清空所有权。关闭被租约拒绝时，原样保留
任务、世界句柄、完成状态、进度和堆重置保护，并返回原关闭错误。
调用方关闭电路会话后，可以对同一个任务重试取消或关闭。

### 移交已经失效的世界句柄

调用方也可以先通过公开的 `terra_world_close` 关闭上述借用句柄。
原来的 `terra_world_open_finish` 仅检查任务保存了非零数值，随后成功返回
这个已经失效的句柄。

现在移交前会核对该代次的世界是否仍然存在。已经退休的世界返回
`STATE_ERROR`，输出句柄保持为零；任务自身仍可正常取消和关闭。
正常 `open_finish` 完成后，所有权仍按既有合同转交调用方，
终态任务的迟到取消和关闭不会影响已经转交或替换的世界。

## 回归与验证

- `tests/world_open_owner_contract.c` 新增真实电路租约阻止任务清理、
  连续重试、解除租约后清理、提前关闭后拒绝移交等原生回归。
- `tests/test_open_task.js` 新增两项通过实际 Wasm 公共 ABI 执行的对应回归，
  同时检查取消与关闭两条路径。
- 两个原生回归分别在修改生产代码之前对基线执行，均在对应行为断言处失败。
- 修复后完整 Release CTest（含 fuzz smoke）**58/58 通过**。
- 相同源码的 ASan、UBSan、LeakSanitizer CTest **58/58 通过**，
  `detect_leaks=1`、`halt_on_error=1`，未关闭泄漏检查。
- 初次 sanitizer 执行所在的受限环境无法读取 `/proc`，LeakSanitizer 因环境错误退出；
  后续在允许检查自身进程的环境中重跑同一编译产物后通过。

原生复现入口：

```sh
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release \
  -DTERRAWASM_NATIVE_TESTS=ON -DTERRAWASM_FUZZ_SMOKE=ON
cmake --build build-native --parallel
ctest --test-dir build-native --output-on-failure

cmake -S . -B build-native-sanitizer -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DTERRAWASM_NATIVE_TESTS=ON -DTERRAWASM_SANITIZERS=ON \
  -DTERRAWASM_FUZZ_SMOKE=ON
cmake --build build-native-sanitizer --parallel
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build-native-sanitizer --output-on-failure
```

生产源码变化需要使用固定 Emscripten 5.0.7 从新的干净提交重新构建 MP/H5，
再核对实际产物的来源、摘要、导出、ABI 和内存合同。原生通过不能替代
重新构建后的 Wasm 回归、viewer 集成构建、微信开发者工具或真机验收。
