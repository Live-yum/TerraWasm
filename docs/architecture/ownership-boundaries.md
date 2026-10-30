# Core ownership boundaries

This document defines the target split for the native WLD engine. It is a migration contract, not a claim that the current `TxWorld` aggregate has already been decomposed.

## Target responsibilities

- **WldCodec** owns bounded byte input/output, WLD version interpretation, parsing, and serialization. It does not own public handles or host task lifetimes.
- **WorldDocument** owns the effective document view: original sections, overrides/patches, revision, and document-level invariants. Queries and serialization must observe the same revision.
- **RuntimeSession** owns handle generations, source leases, cooperative tasks, transient caches, and retirement. It is not the WLD format model.
- **API/composition boundary** validates public numeric handles and connects the document and runtime session to codec operations. Core routines should receive explicit document/session references rather than rediscovering owners through a global registry.

## Current debt retained intentionally

`WorldDocument` and `TxRuntimeSession` are now explicit types. `TxWorld` remains a temporary compatibility facade with anonymous unions that preserve direct field access, so the physical split is not complete. The world slot table, generation allocator, and handle lookup now live in `terra_registry.c`; `terra_api.c` no longer owns or iterates the table directly and instead uses registry-owned claim/visit operations. Several legacy core modules still call `tx_get_world()` directly, so handle resolution has not yet been fully pushed to the API/composition boundary.

## First ratchet

`scripts/check-architecture-boundaries.py` compares the PR against its target branch and rejects increases in direct lexical dependencies per changed C source/header file. Comments and string/character literals are excluded, and calls/declarations may span lines. It rejects increased:
- direct `tx_get_world()` call sites outside `src/terra_api.c`;
- duplicate declarations of the registry lookup outside its owner;
- duplicate definitions of `TxWorld` outside `include/terra_types.h`.

The lookup implementation and declaration are allowed in `src/terra_registry.c` and `src/terra_registry.h`. Slot-table access remains confined to those owner files, including from the API layer. The check is not a full C preprocessor or ownership proof: macro-expanded/indirect calls and relocation of an equal number of legacy calls within a file are not certified. Runtime task-slot accessors still return mutable borrowed storage and remain encapsulation debt.

Existing call sites are deliberately grandfathered so the first guardrail does not make the current branch unbuildable. A passing result means only that the PR did not increase these dependencies.

## Migration order

1. Introduce a world-registry port owned by the API/composition layer; keep generation validation and stale-handle rejection at that boundary.
2. Move task orchestration to an explicit session/owner interface so tasks do not reach into the API registry.
3. Extract `WorldDocument` state from `TxWorld`, preserving allocation marks, stream offsets, override semantics, and revision behavior.
4. Move WLD parsing/serialization behind `WldCodec`; keep the native and JS codecs under differential round-trip contracts.
5. Split runtime caches and task/lease retirement into `RuntimeSession`, with failure/retry and final-ledger-zero tests.

## Required behavior contracts

- Current-handle reads at revision N equal reads after save and reopen at revision N.
- A failed close retains the owner and lease; retry reaches the real host close.
- Cancellation and retirement do not allocate after the allocator enters terminal cleanup.
- WLD byte round trips, header rollback, and cross-codec differential tests remain green.
