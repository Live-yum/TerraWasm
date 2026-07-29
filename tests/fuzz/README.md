# Fuzz harnesses

`fuzz_wld_open.c` and `fuzz_json.c` exercise the bounded reader arithmetic with arbitrary bytes. They expose small dependency-free harness functions for the native CTest smoke target and can be compiled with a libFuzzer driver by defining `LIBFUZZER`.
