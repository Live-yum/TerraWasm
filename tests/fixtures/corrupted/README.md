This directory contains committed malformed WLD inputs used by reader regression and fuzz smoke tests.

- `pattern-4096.wld` is a deterministic 4096-byte invalid payload checked into the repo so reader tests stay self-contained.
- Additional malformed cases may be derived in test code, but required CI coverage must continue to pass with the files committed here.
