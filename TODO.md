# TODO

Living list of open work in psitri-multiindex. Newer entries go on top of
each section. When something lands, drop the bullet and reference the
commit in the relevant doc / changelog instead of leaving a "[done]" stub.

---

## Known bugs / sharp edges

- **Mid-`put` exception leaves partial secondary state.** When the second
  (or later) secondary's `secondary_collision` check throws, the primary
  row and any earlier secondaries are already written. The enclosing
  transaction unwinds correctly only if the caller aborts the (sub-)tx;
  if they catch the exception and commit, the table is left inconsistent.
  Fix options: (a) pre-validate every secondary before any write, (b)
  document "always wrap multi-index writes in a sub-tx," (c) rewrite
  `put` to itself open an internal sub-tx. (a) is cheapest for the common
  no-collision case.
- **`pin_values()` invalidation is silent.** A `value_pin` holds a
  segment read-lock; mutating ops on the same `psitri::transaction` may
  cause the pinned bytes to be rewritten under the reader. Today the
  test suite never crosses pin-then-write, so the failure mode is
  unproven. Add a test that documents the expected behavior (probably:
  "pinned views are valid until the next mutating call on this tx") and
  guard with an assertion in debug builds.
- **`insert(row)` extractor constraint is unfriendly.** `insert` requires
  `Primary::extractor` to be a member-object pointer (so it can write
  the new id back into the row). Using `composite_key<...>` or a
  member-fn pointer triggers a SFINAE failure with no message. Add a
  `static_assert` at the top of `insert()` with a clear diagnostic.

## Correctness gaps

- **`schema_hash` is a placeholder.** Field is reserved + reflected but
  always zero. Wire it once schema-validation modes ship (next section).
- **No `clear()` / `erase_range(lo, hi)` / `drop()`.** Single-pk `erase`
  only. Wiping a whole table on test teardown forces an iterator + N
  deletes today. `clear()` should be a single `psitri::transaction`
  prefix-erase.
- **`find<Tag>` for non-unique returns first only.** Documented, but
  awkward — unique and non-unique tags share a name with different
  return semantics. Decision needed: rename non-unique to `find_first`,
  or drop it for non-unique and force callers to `equal_range`.
- **Composite-key delimiter uncertainty.** `std::tuple<A, B, ...>` is
  encoded by concatenating each element's `psio::key` bytes. For
  fixed-width elements this is collision-free; for variable-width
  elements (e.g. `string`) it depends on the v3 `psio::key` encoding
  including a self-delimiting marker. Verify against `psio::key`'s
  `string` encoding and add a regression test for `tuple<string, ...>`
  collisions.

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

- `begin<Tag>()` / `end<Tag>()` — iterate in secondary-key order.
- `lower_bound<Tag>(k)` / `upper_bound<Tag>(k)` — symmetry with the
  primary versions.
- `equal_range<Tag>(k)` for **unique** secondaries — currently throws a
  `static_assert`. Returning a 0-or-1 element range is a cleaner
  uniform API.
- `count<Tag>(k)` — non-unique block size. Linear, but useful.
- `modify(pk, λ)` — read-mutate-write helper. Avoids the manual
  `auto v = get(pk); v->x = ...; put(*v);` round-trip and gives us
  a single point to add the in-place fast path (see next section).
- `iterator::seek(k)` — re-position an existing iterator without
  constructing a fresh cursor.
- `contains<Tag>(k)` — symmetry with the primary `contains(pk)`.

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
- **Header rewrites on every `insert()`** (next_id changes) and on
  every mutation when `track_row_count` is on. Lazy / batched header
  flush at tx commit would save N-1 header writes per tx.

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

- **README is empty-ish.** Add a quickstart: declare row + named tags,
  construct table, insert/get/iter, sub-tx pattern, the `value_pin`
  zero-copy idiom.
- **API reference.** A single `class table<...>` synopsis with
  preconditions, complexity, and exception spec for each method.
- **Schema-validation guide.** Lands with the schema_hash work.
- **Ordering rules.** `psio::key` encoding semantics for the types we
  actually use (uints, strings, optional, tuple). Big-endian-int
  policy. The "what makes a key memcmp-sortable" rules.

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
