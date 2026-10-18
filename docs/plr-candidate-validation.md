# PLR external-file candidate validation

External-file saving still writes an exclusive same-directory candidate and
uses the platform's existing replacement operation. It never reintroduces an
in-place `fopen(path, "wb")` overwrite of the only original file.

Before creating the candidate, encoded bytes now pass the full encrypted-player
decoder/parser. This is the same parser used by normal opening, including its
bounded compatibility path for regional files with trailing zero capacity. The
validation tree is temporary: it uses no public document slot, does not install
another document/cache, and is freed on success or failure.

After the complete write, flush and close, the candidate is read back in at most
4 KiB chunks. Every byte must match the already parsed encoded payload, and EOF
must occur at its exact length. Read, close, corruption, trailing-data and
truncation failures prevent replacement. The original remains untouched by these
pre-publication failures. Normal buffer encoding does not acquire filesystem
publication responsibilities or run this extra file validation.

The prior implementation returned success when a test changed a candidate byte
between writing and replacement. The regression now rejects changed, appended
and truncated candidates. Twelve allocation-fault positions with a warm encoding
cache cover nine validation rejections and three complete publications; original
bytes and live parser allocations are checked on every path. A save with all 16
public document slots occupied proves that validation does not claim a slot.

This strengthens the candidate-validation boundary only. `fflush` plus atomic
replacement is not claimed to be power-loss durability, directory `fsync`, a
managed version/receipt CAS, or a long-lived publication receipt. Remote or
unsupported platform replacement semantics remain outside the verified native
local-filesystem evidence. G03's broader platform/unknown-publication acceptance
therefore remains open.
