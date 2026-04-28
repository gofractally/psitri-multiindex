# psitri-multiindex — design notes

## Motivation

Spring's chain state is held in a chainbase `multi_index_container`
(boost::multi_index over `boost::interprocess::managed_mapped_file`).
Migrating to psitri requires:

- A multi-index abstraction over psitri trees.
- Per-record serialization that supports schema evolution and zero-copy
  reads.
- Undo-session / transactional semantics that match chainbase's RAII
  undo behaviour.

`psitri-multiindex` supplies (1) and (3); psio's pSSZ supplies (2).

## Storage layout

One psitri tree per multi-index "table". Within a table:

- **Primary tree**: key = encoded primary key, value = pSSZ-encoded row.
- **Secondary trees**: key = encoded (secondary key, primary key), value =
  empty. Maintained alongside the primary on every mutation.

This mirrors `chain-psitri`'s existing layout for contract rows but
generalizes it to arbitrary record types via psio schemas.

## Open questions

- Where do schemas live — in code, on disk, or both? psio's schema IR
  supports round-tripping with WIT/fbs/capnp; do we lean on that for
  cross-language consumers, or generate C++ accessors and stop there?
- Undo-session semantics: psitri's `sub_transaction` + the recently-
  fixed `abort_frame` already give us nested rollback. Do we expose
  that directly to consumers or wrap it in a chainbase-style `session`
  RAII type?
- Multi-table atomicity: a write that touches two multi-indexes should
  commit/abort both together. Easy if we keep one outer psitri
  `transaction` per "session" and open multiple roots inside it.
