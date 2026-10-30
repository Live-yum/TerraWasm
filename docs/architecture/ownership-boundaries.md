# Core ownership boundaries

This document defines the target split for the native WLD engine. It is a migration contract, not a claim that the current `TxWorld` aggregate has already been decomposed.

## Target responsibilities

- **WldCodec** owns bounded byte input/output, WLD version interpretation, parsing, and serialization. It does not own public handles or host task lifetimes.
- **WorldDocument** owns the effective document view: original sections, overrides/patches, revision, and document-level invariants. Queries and serialization must observe the same revision.
- **RuntimeSession** owns handle generations, source leases, cooperative tasks, transient caches, and retirement. It is not the WLD format model.
- **API/composition boundary** validates public numeric handles and connects the document and runtime session to codec operations. Core routines should receive explicit document/session references rather than rediscovering owners through a global registry.

## Current debt retained intentionally

`TxWorld` still combines file bytes, stream offsets, parsed format fields, section overrides, rendering caches, marker state, and task-related state. `terra_api.c` still owns the world slot table and `tx_get_world`; several legacy core modules call that lookup directly. Moving the declaration alone would not solve the coupling, so this migration must move ownership and call flow, not merely headers.

## First ratchet

`scripts/check-architecture-boundaries.py` compares the PR against its target branch and rejects newly added:
- direct `tx_get_world()` call sites outside `src/terra_api.c`;
- duplicate declarations of the registry lookup outside its owner;
- duplicate definitions of `TxWorld` outside `include/terra_types.h`.

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
