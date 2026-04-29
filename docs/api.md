# psitri-multiindex — API reference

The single user-facing template is
`psitri_multiindex::table<Row, Primary, Secondary...>`. Everything below
is a method on it (or a free helper in `psitri_multiindex::`).

This document is normative for the surface tested in `tests/`. For the
big-picture rationale see `docs/design.md`; for open work see
`TODO.md`.

## 1. Declaring a table type

```cpp
struct Row { /* PSIO_REFLECT'd */ };
struct by_id; struct by_name; struct by_group;

using my_table = psitri_multiindex::table<
   Row,
   psitri_multiindex::ordered_unique    <by_id,    &Row::id>,
   psitri_multiindex::ordered_unique    <by_name,  &Row::name>,
   psitri_multiindex::ordered_non_unique<by_group, &Row::group_id>>;
```

Constraints:
- `Primary` MUST be `ordered_unique<...>`. Compile error otherwise.
- Tags are user-declared empty types (`struct by_id;`). Names are
  irrelevant — only the type identity matters.
- An `Extractor` is a member-object pointer (`&Row::field`), member-
  function pointer (`&Row::method`), or a `composite_key<&Row::a,
  &Row::b, ...>` value-NTTP. Any callable taking `const Row&` and
  returning a memcmp-sortable encoded type works.

### Composite keys

`composite_key<Ex...>` produces a `std::tuple<...>` of extracted values.
psio's `key` format encodes tuples by concatenating each element's
`psio::encode(key{}, ...)` bytes. Strings are NUL-terminated (with
`\0\1` escape for embedded NULs) so adjacent string components stay
self-delimiting — `tuple("a","bc")` and `tuple("ab","c")` encode to
distinct byte sequences.

For non-unique secondaries, the tree key is automatically composed as
`(extractor_value, primary_key_value)` so the underlying trie stays
unique-by-construction.

## 2. Construction

```cpp
my_table t(tx, "T/", psitri_multiindex::table_options{...});
```

- `tx`: a live `psitri::transaction&`. The table holds a borrowed
  pointer; lifetime is the caller's.
- `"T/"`: per-table prefix in the trie. All keys live under this
  prefix. Two tables with different prefixes are isolated; same prefix
  with different schemas is a programmer error.
- `table_options::track_row_count` (default `false`): opt-in row
  counter in the table header. Adds one header read+write per
  insert/erase that changes the count.

The constructor reads the header once. If `track_row_count` is on and
the persisted header doesn't have it, the flag is stamped immediately.

## 3. Mutations

| Method | Effect | Throws |
|---|---|---|
| `void put(const Row&)` | Atomic upsert. Validates every unique secondary BEFORE any tree write. | `secondary_collision` if a unique secondary's encoded key already maps to a different primary. |
| `primary_key_type insert(Row)` | Allocates the next id from `header.next_id`, writes it back into `row` via the primary extractor (must be a data-member pointer), calls `put`. Returns the assigned id. Combines `next_id`, `row_count`, and the row write into one header update. | `secondary_collision`; `static_assert` if the primary extractor isn't a data-member pointer. |
| `bool erase(const K& pk)` | Removes the row at `pk` plus every secondary entry. Returns `true` if a row was removed. | (no business throws) |
| `bool remove(const Row&)` | Convenience: extracts the primary key from `row` and calls `erase`. | (no business throws) |
| `bool modify(const K& pk, F&& fn)` | Read-mutate-write: reads the row at `pk`, invokes `fn(Row&)`, writes back via `put`. Returns `false` if the row doesn't exist (callback not invoked). | `std::logic_error` if the callback changes the primary-key field; `secondary_collision` from the trailing `put`. |
| `void clear()` | Erases every key under the table prefix (primary tree + every secondary tree + header). Re-stamps `track_row_count` if it was enabled. | `std::invalid_argument` if the prefix has no lex successor (all-`0xff` bytes — pathological). |
| `std::size_t erase_range(const K& lo, const K& hi)` | Half-open primary erase `[lo, hi)`. Walks the range, calls `erase` per row when there are secondaries; otherwise issues a single `psitri::transaction::remove_range`. | (no business throws) |

All mutations are `psitri::transaction`-scoped: a sub-tx abort rolls
back primary, every secondary, and any header counter changes
atomically.

## 4. Single-row reads

| Method | Returns | Notes |
|---|---|---|
| `std::optional<Row> get(const K& pk)` | row at `pk` or `nullopt` | full pSSZ decode |
| `bool contains(const K& pk)` | exists in the primary tree | no decode |
| `std::optional<Row> find<Tag>(const K& key)` | for primary or unique tag, the matching row; for non-unique, the first row in tree order | one extra trie hop on unique secondaries (sk → pk → row) |
| `bool contains<Tag>(const K& key)` | exists at this tag | no row decode |
| `std::size_t count<Tag>(const K& key)` | 0/1 for primary or unique, exact count for non-unique | non-unique walks the (sk, *) block |
| `table_header header()` | persisted header (`next_id`, `flags`, `row_count`, …) | one trie read |
| `std::optional<uint64_t> size()` | `row_count` if tracked, else `nullopt` | one trie read |

## 5. Iteration

### Primary

```cpp
for (auto it = t.begin(); it != t.end(); ++it)
   visit(*it);

auto lo = t.lower_bound(uint64{10});   // ≥ 10
auto hi = t.upper_bound(uint64{50});   // > 50
it.seek(uint64{30});                   // re-position in place
```

`primary_iterator::operator*()` returns an owned `Row` (one decode per
deref). For zero-copy access, see §7.

### Secondary tag

```cpp
for (auto it = t.begin<by_name>(); it != t.end<by_name>(); ++it)
   visit(*it);

auto lo = t.lower_bound<by_name>("alice");
auto hi = t.upper_bound<by_name>("z");
it.seek("bob");
```

`upper_bound<Tag>` for non-unique tags lex-increments the `(idx||sk)`
prefix to skip past the entire `(sk, *)` block — i.e. it returns the
first row whose `sk > key_value`, not the first whose `(sk, *) > key`.

### Range query

```cpp
for (auto&& r : t.equal_range<by_group>(uint64{100}))
   visit(r);
```

Works for both unique tags (range is 0 or 1) and non-unique tags (range
walks the whole `(sk, *)` block).

## 6. Header / metadata

```cpp
auto h = t.header();   // table_header value
// h.next_id, h.schema_hash, h.schema_version, h.flags, h.row_count
```

The header is pSSZ-encoded with `maxFields(8)` + `maxDynamicData(32)`
bounds — append-optional fields can be added without breaking older
readers.

`schema_hash` is currently a placeholder zero. Schema validation modes
(strict / lenient / migrate / overwrite) are tracked in `TODO.md`.

## 7. Zero-copy reads via `value_pin`

```cpp
auto pin = t.pin_values();             // RAII; non-movable
for (auto it = t.begin(); it != t.end(); ++it) {
   auto v = it.view(pin);              // psio::view<Row, pssz, const_borrow>
   process(v.template get<0>());       // arithmetic by value
}
```

Contract:
- `value_pin` keeps the segment(s) holding the trie's bytes mapped.
- A view obtained via `it.view(pin)` references trie memory directly.
  It stays valid as long as `pin` lives **and the underlying tree is
  not mutated**. Cursors are invalidated by mutations; views derived
  from a cursor should not be reused after a mutation.
- The pin itself is non-movable (held as a local; views captured into
  containers stay valid alongside it for the iteration scope).

For the documented invalidation rules straight from psitri, see
`psitri/value_pin.hpp` — psitri-multiindex inherits them as-is.

## 8. Exceptions thrown

| Type | When | Recovery |
|---|---|---|
| `psitri_multiindex::secondary_collision` | unique secondary check fails on `put`/`insert`/`modify` | nothing has been written; safe to catch and continue, or rethrow and abort the (sub-)tx |
| `std::logic_error` | `modify` callback mutated the primary-key field | abort the tx — the row was rewritten before the post-check |
| `std::invalid_argument` | `clear()` with an all-`0xff` prefix | use a non-saturated prefix |

`secondary_collision` aborts BEFORE any tree write (atomicity rule for
`put`). The other two reflect programmer errors and should be treated
as such.

## 9. Antelope / Spring shape compatibility

Tables in Spring's `libraries/chain/include/eosio/chain/*.hpp` that
work as-is when ported:

| Spring table | Pattern | Notes |
|---|---|---|
| `account_index`, `account_metadata_index`, `account_ram_correction_index` | auto-id + `by_name` (uint64-wrapped) | reflect `name` as a record with one `uint64_t` field |
| `permission_index` | auto-id + composites `(parent, id)`, `(owner, name)`, `(name, id)` | use `composite_key<...>` on data-member pointers |
| `code_index` | auto-id + composite `(code_hash, vm_type, vm_version)` | `code_hash` needs digest-as-key support (see gaps) |
| `transaction_multi_index`, `generated_transaction_multi_index` | auto-id + `(time_point, id)` etc. | `time_point` reflects to int64; works |
| `permission_link_index` | auto-id + `(account, code, message_type)`, `(account, required_permission, id)` | works |
| `table_id_multi_index` | auto-id + `(code, scope, table)` | works |
| `block_summary_multi_index`, `global_property_multi_index`, `dynamic_global_property_multi_index`, `protocol_state_multi_index`, `database_header_multi_index` | auto-id only | works |
| `key_value_index`, `index64/128/256_index` | auto-id + composite `(t_id, primary_key)` etc. with explicit `composite_key_compare<std::less<...>, ...>` | the explicit comparator is the default; works |

Gaps blocked on psio upstream:
- **Digest-as-key** (`fc::sha256`, `transaction_id_type`, `digest_type`,
  `block_id_type`). `psio::key` doesn't currently dispatch on
  `std::array<uint8_t, N>`. Workarounds: reflect the digest as a
  record of two `psio::uint128` halves, or as four `uint64`. Pending
  proper support upstream.
- **`long double` / `float128_t`** sort. `psio::key` handles `float`
  and `double` via sign-flipped big-endian (matches IEEE 754 less-than
  on finite values, including the NaN / signed-zero subtleties Spring's
  `soft_double_less` is built around). 128-bit float is not yet
  supported.
- **Member-function extractors** — supported, but uncommon in Spring's
  persistent tables (they appear in in-memory `bmi::multi_index`-only
  containers like `snapshot_scheduler`).

See `tests/antelope_shapes_tests.cpp` for working examples of every
supported shape.
