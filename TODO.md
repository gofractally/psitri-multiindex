# TODO

Living list of open work in psitri-multiindex. Newer entries go on top of
each section. When something lands, drop the bullet and reference the
commit in the relevant doc / changelog instead of leaving a "[done]" stub.

---

## Known bugs / sharp edges

- **`pin_values()` debug-assert.** The positive contract is now
  documented (`tests/table_iteration_tests.cpp` "pin keeps captured
  views readable…"): pin-derived views stay valid as long as no
  mutation happens on the underlying tx. The negative case (mutate-then-
  read-pinned) is intentionally untested per psitri's stated contract
  ("not a snapshot, does not make cursors valid across mutation"). Add
  a debug-build assertion that fires when a cursor advance is followed
  by a use-after-mutate of a pinned view from the same iteration.

## Schema validation — follow-ups

The core landed: `compute_schema_hash<T>()` walks reflection metadata
recursively to fingerprint a row type, the `table` constructor stamps
on fresh tables and validates on re-open, and `schema_options` exposes
`strict` / `lenient` / `overwrite` modes plus a forward-version
allowlist. Open follow-ups:

- **`migrate` mode.** A user-supplied callback that gets the persisted
  header + the runtime's expected hash + a tx and is responsible for
  re-encoding existing rows. Sketched out earlier; deferred because
  the migration API needs the table itself for re-encoding, which
  means a chicken-and-egg with the constructor-time validation.
  Likely shape: `schema_options::migrate = std::function<void(
  psitri::transaction&, std::string_view prefix,
  const table_header& persisted, table_header& target)>`.
- **Hash deeper.** Today the fingerprint walks reflected types
  recursively but falls back to `get_type_name` (or `anon{size:align}`)
  for non-reflected nested types. A future pass could hash type-level
  annotations (`maxFields`, `maxDynamicData`, `definitionWillNotChange`,
  `as_spec<Tag>`) so wire-format drift inside otherwise-identical
  shapes is also caught. This is the "hash the psio::schema IR"
  alternative we deferred — meaningful when we want round-trip
  guarantees across tagged-binary / WIT / pSSZ formats.

## API surface

(empty — Spring/Antelope shape closure landed.)

## Performance / fast paths

- **`mutable_view<T, pssz>` (waiting on psio upstream).** Today a
  modify is decode → mutate → encode → upsert, allocating twice. With
  a write-view we can update fixed-size fields in place inside the
  value_node bytes. Discussed earlier; tracked here so we wire it into
  `modify(pk, λ)` once psio lands.
- **Iterator default decode is full pSSZ.** `pin_values()` enables
  zero-copy via `view<T, pssz>` but is opt-in. Consider making it the
  default for record types whose `effective_max_dynamic_v<T>` is
  known-bounded — those decode straight from the segment without a
  copy regardless.
- **Header rewrites on every mutation when `track_row_count` is on.**
  `insert()` already collapses the next_id + row_count bump into one
  header write; `put()` and `erase()` still write per call. Batched /
  end-of-tx flush would save N-1 header writes per tx of the same
  kind.

## Build / packaging

- **CI workflow.** No GitHub Actions yet. Matrix should be {linux/clang,
  linux/gcc, macos/homebrew-clang} × {C++23}. Run `psitri_multiindex_tests`
  + smoke. Target Catch2 from apt/brew rather than vendoring.
- **Install / export rules.** `install(TARGETS psitri_multiindex EXPORT
  ...)` is not wired; system-install / vcpkg consumers can't pick this
  up yet. Defer until the API stabilizes (post-v0.1).
- **Version tagging.** CMakeLists declares `VERSION 0.1.0` but no git
  tag has been cut. Tag once schema validation lands.

## Docs

- **Ordering rules deep-dive.** `psio::key` encoding semantics for the
  types we actually use (uints, strings, optional, tuple, float/double,
  uint128/256, float128). Big-endian-int policy. "What makes a key
  memcmp-sortable" rules. Today scattered across `docs/api.md` §1 and
  `tests/key_codec_tests.cpp`; gather into a single doc.

## Parking lot — open design questions

- **Should `track_row_count` default to `true`?** Today it's false to
  avoid the per-mutation header write. Tables under ~1k rows probably
  benefit; large hot tables don't. Need a perf number before flipping.
- **Per-table metadata trees.** Some users will want non-row state
  (config, audit log, last-compacted-at) attached to a table. Today
  they'd carve out their own prefix. Worth a `meta<Tag, T>` accessor
  layered over a reserved index byte? Or keep it user-territory?
- **Multi-tx-per-table reads vs. writes.** Today `table` holds a
  `psitri::transaction*`. A read-only handle that takes a const-tx
  would let read scans share the same tx machinery without the
  templated mutation surface.
