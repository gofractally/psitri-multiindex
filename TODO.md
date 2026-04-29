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

## Correctness gaps

- **`schema_hash` is a placeholder.** Field is reserved + reflected but
  always zero. Wire it once schema-validation modes ship (next section).

## Schema validation (unblocked — xxhash now vendored)

- Compute `xxh64(canonical-schema-bytes)` at table construction. Stamp
  on first write; verify on subsequent opens.
- `schema_options` enum: `strict` (throw on mismatch), `lenient` (log,
  proceed), `migrate` (call user-supplied migration fn), `overwrite`
  (re-stamp). Default = `strict`.
- `schema_version` ladder: refuse to open when stored version >
  runtime, unless `schema_options.allow_forward = true`.
- Throw a typed `schema_mismatch` exception (parallel to
  `secondary_collision`) carrying both stored + expected hashes.
- Decision still open: hash the psio::schema IR (canonical, format-
  independent) or the WIT/pSSZ wire bytes. The IR is more stable; the
  wire bytes catch annotation drift that doesn't affect the IR.

## API surface

- **`psio::key` long-double / float128 support.** `psio::key` handles
  `float` and `double` (sign-flipped BE, matches IEEE 754). 128-bit
  float (Spring's `index_long_double_index`) is unsupported — would
  need an upstream psio addition or, more practically, a user-defined
  `sortable_binary_category` adapter that converts long-double to a
  fixed-width sort-preserving byte sequence.

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

- **Vendored `detail/xxhash.h` not yet consumed.** When schema_hash
  wiring lands, the single inclusion site needs `#define XXH_INLINE_ALL`
  *before* the `#include` so xxhash compiles as header-only and we
  don't need a `.c` translation unit.
- **CI workflow.** No GitHub Actions yet. Matrix should be {linux/clang,
  linux/gcc, macos/homebrew-clang} × {C++23}. Run `psitri_multiindex_tests`
  + smoke. Target Catch2 from apt/brew rather than vendoring.
- **Install / export rules.** `install(TARGETS psitri_multiindex EXPORT
  ...)` is not wired; system-install / vcpkg consumers can't pick this
  up yet. Defer until the API stabilizes (post-v0.1).
- **Version tagging.** CMakeLists declares `VERSION 0.1.0` but no git
  tag has been cut. Tag once schema validation lands.

## Docs

- **Schema-validation guide.** Lands with the schema_hash work.
- **Ordering rules deep-dive.** `psio::key` encoding semantics for the
  types we actually use (uints, strings, optional, tuple, float/double,
  uint128/256). Big-endian-int policy. "What makes a key memcmp-
  sortable" rules. Today scattered across `docs/api.md` §1 and
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
