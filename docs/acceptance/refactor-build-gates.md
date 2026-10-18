# Refactor artifact acceptance

Engineering quotas are in `config/package-budgets.json`: all/wld 512 KiB blocking and 480 KiB warning; PLR remains 320 KiB. They do not establish WeChat AppID entitlement. `build.ps1 -Test` collects behavior and size failures independently after producing the artifact.

The quality workflow consumes the **all Web** artifact from the source SHA. `tests/production_all_web_contract.cjs` checks the bytes against the producer manifest, compiled source identity and initial memory, then invokes world/circuit, player JSON and pixel workspace lifecycles. Existing circuit acceptance still runs against that artifact. This is core smoke coverage, not complete platform acceptance or full player/pixel semantics.

Private world input is opt-in:

```sh
cmake -S . -B build-native -DTERRAWASM_NATIVE_TESTS=ON -DTERRAWASM_REAL_WORLD_FIXTURE=/private/world.wld
cmake --build build-native
ctest --test-dir build-native --output-on-failure
```

An explicit nonexistent path fails configuration. Without a path, `terra_world_checkpoint_real_file` is not registered: this is **not** a real-file pass. No user world bytes are checked in. G47/G48 still require the actual private corpus, full tile oracle, and device runs. No current Emscripten rebuild or target device result is implied by tests against an existing viewer bundle.
