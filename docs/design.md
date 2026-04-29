# psitri-multiindex Design Spec

This library is designed to replace memory mapped boost multi_index containers using psitri and
the pssz zero-copy read format. The rationalle is that the memory model of boost's multi index
containers does not scale beyond RAM and is cache unfriendly; therefore, we want to use a truly
transactional database that still gives the transparent experience that closely models multindex.

## User's Starting Point — Defining their Table

A table is a plain C++ struct that can contain arbitrary data. It is reflected via psio, which
exposes the field names and types so the indexer can address fields, encode keys, and maintain
secondary-index invariants automatically.

```cpp
#include <psio/reflect.hpp>

struct account {
   uint64_t     id;          // primary key
   std::string  name;        // unique secondary
   uint64_t     balance;     // non-unique secondary
   std::string  notes;       // not indexed
};
PSIO_REFLECT(account, id, name, balance, notes)
```

A table is then declared by binding the struct to a primary-key extractor and zero or more
secondary-key extractors. Extractors can be:

- a pointer-to-data-member, e.g. `&account::id`
- a pointer-to-member-function (returning the key value), e.g. `&account::sort_key`
- a `composite_key<...>` of the above, for tuple keys
- a `nested_key<...>` for drilling into nested structs

```cpp
using accounts_table = psitri_multiindex::table<
   account,
   /* primary   = */  &account::id,
   /* secondary = */  &account::name,
   /* secondary = */  composite_key<&account::balance, &account::id>>;
```

The primary key may be any reflected member; there is no implicit `id_type`. Secondary indexes that
must allow duplicates encode the primary key as a tail component (the `composite_key<balance, id>`
above) so the underlying tree key remains unique even when the user-visible key (balance) is not.

## Motivation

Spring's (Antelope/EOS) chain state is held in a chainbase `multi_index_container`
(boost::multi_index over `boost::interprocess::managed_mapped_file`). Migrating to psitri requires:

1. A multi-index abstraction over psitri trees.
2. Per-record serialization that supports schema evolution and zero-copy reads.
3. Undo-session / transactional semantics that match chainbase's RAII undo behaviour.

`psitri-multiindex` supplies (1) and (3); psio's pSSZ supplies (2).

## Construction — tables bind to a transaction, not a database

A table is constructed from a live `psitri::transaction` reference and a per-table key prefix. The
table is depth-agnostic: it does not know whether the transaction is the outer transaction or a
nested sub-transaction, and the user can freely mix tables on the same transaction.

```cpp
auto db  = psitri::database::open(path);
auto ws  = db->start_write_session();
auto tx  = ws->start_transaction(/*root_index=*/0);

accounts_table       accounts(tx, /*prefix=*/"accounts/");
transactions_table   transactions(tx, "txs/");

accounts.put({1, "alice", 100, ""});
transactions.put({...});
tx.commit();
```

Tables hold a non-owning pointer to the transaction, plus the prefix and per-index byte offsets.
All mutations and reads delegate directly to the transaction's cursors. Multiple tables that share
a transaction implicitly commit and abort together — they share the transaction's frame stack.

## Sub-transactions — a stack inside the parent

`psitri::transaction` keeps an internal `std::vector<frame>` that acts as a LIFO stack. Calling
`tx.sub_transaction()` pushes a frame; the returned `transaction_frame_ref` is a move-only RAII
guard whose `commit()` / `abort()` / destructor pops the top of the stack.

Each pushed frame snapshots:

- The current root smart_ptr of every change_set (one per opened tree, i.e. one per table index).
- The size of the change_set vector itself (so any subtree change_sets opened *inside* the frame
  are dropped on abort).
- The buffer state, in `expect_failure` mode (not used by spring's expected `expect_success`
  workflow).

`commit_frame()` simply pops the snapshot and discards it. `abort_frame()` restores every saved
cursor root, dropping any nodes the frame allocated. Because every change_set is snapshotted
together, abort is **atomic across every table that shares the transaction**.

```cpp
auto sub = tx.sub_transaction();
accounts.put(...);
balances.put(...);
sub.abort();           // both tables roll back
```

Nesting is required to be LIFO; the destructor of an inner `transaction_frame_ref` aborts that
frame, and the type system enforces that move-only frame_refs cannot outlive their parent. Misuse
trips an `assert(!_frames.empty())` in psitri.

## CRUD operations

```cpp
// Insert / replace.
accounts.put(account{42, "alice", 100, "primary user"});

// Look up by primary key — returns std::optional<view<account>>.
if (auto v = accounts.get(42)) {
   uint64_t bal = v->balance();      // accessor through the view; zero copy
}

// Modify in place where possible, with a lambda over a write-view.
accounts.modify(42, [](auto& w) {
   w.balance = w.balance + 50;       // fixed-size field → in-place memcpy when the
                                     // leaf is uniquely owned and writable.
   w.notes   = "promoted";           // variable-size → marks dirty, full re-serialize
                                     // on lambda exit.
});

// Erase by primary key.
accounts.erase(42);

// Erase by reference / view.
accounts.remove(*accounts.get(43));
```

`put` always replaces; the chainbase-style `db.modify(obj, λ)` is provided by `modify(pk, λ)` over
a psio write-view. The wrapper inspects which fields the lambda actually touched and chooses the
cheap or expensive path:

- **Fixed-size fields** (integers, fixed-width records, optional<T> when present-bit unchanged):
  written through psio's wview directly. SAL's `modify_guard` decides COW-vs-in-place per
  refcount and segment ownership — no work for the table layer.
- **Variable-size fields** (strings, vectors, optional<T> growing or shrinking): the wview marks
  the record dirty; on lambda exit the table re-serializes and calls `tx.upsert(pk, new_bytes)`.

This collapses chainbase's two-step pattern (`find` + `modify`) into one call, and lets reads of
unchanged fields cost nothing.

## Iteration — zero-copy views via per-iterator read locks

Each index exposes the standard set:

```cpp
auto idx = accounts.get_index<&account::name>();   // by name (secondary)
auto it  = idx.lower_bound("a");
auto end = idx.upper_bound("b");
for (; it != end; ++it) {
   psio::view<const account> v = *it;
   std::cout << v.id() << " " << v.name() << "\n";
}
```

The iterator is constructed under a `sal::read_lock` that it holds for its entire lifetime. While
the lock is held:

- The segment containing the leaf data is pinned: SAL will not recycle it, demote it to read-only
  via `mprotect`, or relocate it.
- Iterator dereference returns a `psio::view<T>` whose `const char*` points directly into mmap
  memory. No copy, no decode, no allocation.
- The iterator's snapshot smart_ptr increments the refcount on the cursor's root, which causes
  any concurrent write through the same transaction to walk the dispatch's `r.ref() > 1` check
  and downgrade to the shared (full COW) path — the iterator's view stays valid even if the user
  writes to the table while iterating.

In other words, holding an iterator **freezes the read view** and forces writes during iteration
to take the COW path. The fast in-place modify path resumes the moment the iterator goes out of
scope. There is no chainbase-style "iterator invalidation on modify"; views are stable for as
long as the iterator exists.

### Range and prefix queries

For composite keys, the iterator supports prefix-search via partial tuples:

```cpp
auto idx = accounts.get_index<composite_key<&account::balance, &account::id>>();

// All accounts whose balance >= 100
auto it = idx.lower_bound(std::tuple{100ull});
// All accounts with balance == 100, ordered by id ascending
auto sub = idx.subindex(std::tuple{100ull});
for (auto&& v : sub) {
   ...
}
```

`subindex(prefix)` returns a new `index` view restricted to the lexicographic range of that
prefix, so further `lower_bound` / `upper_bound` calls on the subindex apply to the suffix of the
key.

## Key encoding

Keys go down to psitri as memcmp-sortable byte strings. The encoding is psio's
`to_key`, designed so that `memcmp(to_key(a), to_key(b))` reproduces the natural ordering of `a`
and `b`:

| Type | Encoding |
|---|---|
| Unsigned ints | big-endian |
| Signed ints | sign bit flipped, big-endian |
| Float / double | IEEE-754 sign transform, big-endian |
| `bool` | `0x00` / `0x01` |
| `string`, byte vector | content with `\0 → \0\1` escape, terminated `\0\0` |
| Vector of T | `\1` ‖ each element ‖ `\0` |
| `optional<T>` | `\0` (absent) or `\1 ‖ to_key(value)` |
| `variant<...>` | 1-byte tag ‖ encoded payload |
| Reflected struct | fields concatenated in declaration order |
| Tuple / composite_key | components concatenated |

Each tree key follows the same layout chainbase-via-prefix and psibase use:

```
| index    | tree key bytes                             | tree value bytes              |
| primary  | prefix ‖ 0  ‖ to_key(primary)              | pSSZ-encoded row              |
| sec(i)   | prefix ‖ i  ‖ to_key(sec_i)                | tree key of primary           |
```

The secondary value is the bytes of the primary tree key (including its `prefix ‖ 0` lead) so a
secondary lookup is two `tx.get` calls — secondary → primary key bytes → row — with both keys
already encoded.

For a non-unique secondary, the index is declared with `composite_key<extractor, &T::primary>` so
the encoded bytes are `prefix ‖ i ‖ to_key(sec) ‖ to_key(pk)` and uniqueness in the tree is
preserved by the trailing primary component.

## Storage layout

One psitri tree per multi-index "table" prefix bucket. Within a table, the first sub-prefix byte
selects between metadata, the primary tree, and the secondary trees. Metadata is split between a
small **hot header** read on every open and most writes, and a **cold schema record** read only
when structural schema comparison is needed.

```
prefix ‖ 0x00 ‖ 0x00          → hot header        (pSSZ-encoded struct, hot path)
prefix ‖ 0x00 ‖ 0x01          → canonical schema  (kilobytes, cold path, written rarely)
prefix ‖ 0x01 ‖ pk_bytes      → primary tree      (value: pSSZ-encoded row)
prefix ‖ 0x02 ‖ sk1_bytes     → secondary index 1 (value: prefix ‖ 0x01 ‖ pk_bytes)
prefix ‖ 0x03 ‖ sk2_bytes     → secondary index 2
…
prefix ‖ 0xFF ‖ sk254_bytes   → secondary index 254
```

The `0x00` prefix is a meta-namespace; sub-byte `0x00` is the hot header, `0x01` the canonical
schema, `0x02..0xFF` reserved for future metadata (per-index statistics, partial-index schemas,
etc.). User indexes start at `0x01` (primary). 254 secondaries is ample.

Implementation: a single psitri root with these prefixed keys. Multiple roots via
`transaction::open_root` is an alternative but adds complexity for no benefit at this scale —
keep it simple, revisit only if cursor contention shows up.

## Table metadata

Metadata is split into two records keyed under `prefix ‖ 0x00`:

### Hot header (`prefix ‖ 0x00 ‖ 0x00`)

The header is a **pSSZ-encoded struct that is intentionally extensible** via trailing optional
fields — the same append-optional rule that governs row schema evolution applies to this
metadata struct too. To keep the encoding tight, we bound the type's growth ceilings using two
psio annotations (specified for the upcoming psio release):

```cpp
struct table_header {
   uint64_t                next_id;         // auto-increment cursor (frequently bumped)
   uint64_t                schema_hash;     // xxh64 of canonical schema bytes
   uint16_t                schema_version;  // sum of fixed-field counts (≤ 65535 is plenty)
   uint16_t                flags;           // bit 0: row_count_enabled, etc.
   std::optional<uint64_t> row_count;       // present only when opted-in
   // Future metadata appended here as optional<...> fields.
};
PSIO_REFLECT(table_header,
             maxFields(16),         // header will never have more than 16 fields total
             maxDynamicData(192),   // heap region (sum of all optional payloads) ≤ 192 bytes
             next_id, schema_hash, schema_version, flags, row_count)
```

#### Bounds annotations (proposed psio additions)

`maxFields(N)` and `maxDynamicData(N)` are new psio annotations alongside the existing
`definitionWillNotChange()`. They are orthogonal to "schema is locked":

- `definitionWillNotChange()` — schema cannot be extended. Eliminates the fixed_size header
  and trailing-optional handling entirely. Yields the smallest possible encoding but kills
  forward compatibility.
- `maxFields(N)` — the type may grow up to N fields total over its lifetime. Caps the fixed
  region size at compile time; if `N` is small enough, the fixed_size header itself can shrink
  from `u16` to `u8`.
- `maxDynamicData(N)` — the type's heap region (variable-size payloads) is bounded to N
  bytes. If `N ≤ 255`, all heap-pointing offsets in the fixed region can shrink from `u32` to
  `u8`. If `N ≤ 65535`, they shrink to `u16`. Otherwise `u32`.

For the `table_header` we set `maxFields(16)` (current 5 fields plus 11 spare future-optional
slots) and `maxDynamicData(192)` (16 × 8-byte optional<u64> = 128 bytes with comfortable
margin). Both bounds keep offsets and the fixed_size header at `u8`.

#### Encoded sizes

With u8 offsets and u8 fixed_size header (per the bounds above):

- `row_count = nullopt` (default, trailing optional elided): u8 fixed_size + 20 bytes fixed
  region = **21 bytes**.
- `row_count = some(n)`: u8 fixed_size + 21 bytes fixed (incl. one u8 offset) + 8 bytes heap
  = **30 bytes**.

Compare to the current pSSZ encoding without bounds annotations (u16 fixed_size, u32
offsets):

| Encoded form          | Without bounds (today) | With `maxFields(16)+maxDynamicData(192)` |
|-----------------------|-----------------------:|-----------------------------------------:|
| `row_count = nullopt` |                22 B    |                                  21 B   |
| `row_count = some(n)` |                34 B    |                                  30 B   |
| Header at full extension (16 optionals all `some`) | 142 B  | 111 B |

Modest savings on the 5-field common case (1–4 bytes), but the savings scale linearly with
appended optional fields. More importantly, applying the same bounds annotation pattern to
**rows** (the actual user records, which often contain bounded strings/vectors) is where the
encoding wins compound — every offset shrinks from 4 bytes to 1, on every row, on every
secondary-index key payload, everywhere.

#### Why these field types

- `next_id`, `schema_hash` stay at 64 bits — the auto-id space has to grow without a
  wraparound ceiling on long-lived databases, and xxh64 is the canonical schema-hash width.
- `schema_version` is the sum of fixed-field counts across all unique reflected types. Even
  a large app rarely accumulates more than a few thousand fields total over its lifetime;
  16 bits (max 65535) is far more headroom than realistic schema growth needs.
- `flags` is for opt-in feature bits; 16 bits gives 16 reserved bits, which is plenty (we use
  one for `row_count_enabled`, leaving 15 for future).
- `row_count` is `optional<uint64_t>` so a table that never opts in elides it entirely (the
  trailing-optional elision rule), saving even the offset.

#### Encoding properties

- Fixed fields (`next_id`, `schema_hash`, `schema_version`, `flags`) have **stable byte
  offsets** in the encoded buffer relative to the start of the fixed region, so a per-insert
  bump of `next_id` is an 8-byte `memcpy` at a known offset — no decode, no re-serialize.
- Toggling `row_count_enabled` requires re-serializing the header (the encoded size changes
  because the optional appears or disappears). Updating an already-present `row_count` is an
  in-place 8-byte `memcpy` to the heap slot via the offset.
- New optional fields can be appended in future versions up to the `maxFields(16)` ceiling;
  old binaries reading newer records see their known fields plus a longer-than-expected
  fixed_size, ignoring the trailing bytes. New binaries reading older records observe a
  shorter fixed_size and treat the missing optionals as `nullopt`.
- The `maxFields` / `maxDynamicData` ceilings are part of the type's identity in pSSZ — they
  participate in the schema hash, so a reader compiled with different bounds will see a
  schema mismatch and trigger structural validation.

**MVCC reality.** "Fits inline, in-place memcpy" is the best case: a uniquely-owned tree with
no open snapshots or iterators. Realistic case in spring's workload is concurrent readers or
overlapping transactions, in which case the leaf carrying the header has refcount > 1 and
updates walk the COW path. The header may then be promoted to a `value_node` so MVCC snapshots
can carry multiple versions on the chain. In that mode every insert appends a chain entry
under its txn version. The per-txn coalesce path (the one we just hardened in psitri) keeps
multiple writes within the same transaction folded onto a single chain entry, so the worst
case is "one chain entry per committing transaction" rather than "one per insert", and chain
entries are GC'd as their snapshots close.

So:
- Single-writer, no readers → inline value, in-place memcpy on update.
- Concurrent readers or active snapshots → COW + value_node chain, one entry per outer
  transaction, coalesced within transactions, reaped on snapshot close.

Either way the header is small enough that the cost difference is one memcpy vs one
allocation + chain append.

### Canonical schema record (`prefix ‖ 0x00 ‖ 0x01`)

The full pSSZ-encoded canonical schema bytes for the row type. Typically a few kilobytes —
goes through the `value_node` path. Read **only** when:

- An open with `schema_check::structural` hits a hash mismatch and needs to perform full
  structural compare against the compiled schema.
- An external reader (different process, different language binding) opens the database
  standalone and needs to discover the schema.
- Schema upgrade tooling reads the previous schema before writing the new one.

Steady-state opens (matching hash) and all reads/writes never touch this key. Storing it as a
separate trie value keeps the hot leaf small and avoids re-COWing kilobytes of schema bytes
every time the auto-id counter bumps.

The schema record is written once when the table is first populated and on schema upgrades.
Optional: a future shared schema-registry tree keyed by `schema_hash` can deduplicate when many
tables share the same row type — the table just stores the hash in the hot header and looks up
the bytes by hash. Both hot header and canonical schema live in the trie so they COW and roll
back atomically with row writes.

### Auto-increment

```cpp
account a{.name="alice", .balance=100};
auto pk = accounts.insert(a);   // allocates pk from header.next_id, returns it
accounts.put(account{99, ...}); // explicit pk, never touches next_id
```

The first cut allocates one id per `insert` call (one extra header read + write per insert). That
keeps abort semantics trivial: aborting a sub-transaction restores `next_id` along with the rest
of the trie.

For high-throughput inserts a future optimization is to reserve id ranges in memory (`(lo, hi]`
cached on the table object, header bumped by 1000 at a time). That requires the table to register
an abort callback on the transaction so it can restore the in-memory range when a sub aborts.

### Optional row count

`row_count` is **opt-in**, encoded as the trailing `optional<uint64_t>` field of the header
struct. The reason: every `insert` and every `remove` would update it, and on tables that don't
need the count this is pure overhead — especially `remove`, which otherwise touches no
metadata at all.

```cpp
accounts_table accounts(tx, "accounts/", schema_check::hash_only,
                        table_options{.track_row_count = true});
```

When **disabled** (`row_count = nullopt`):
- `insert`/`put` updates only `next_id` in the header — an 8-byte memcpy at a stable offset
  when the leaf is uniquely owned.
- `remove` does not touch the header at all.
- `accounts.size()` is unavailable (or, optionally, performs a full scan with a clear cost
  warning).

When **enabled** (`row_count = some(n)`):
- `insert`/`put` (creating a new row) updates `next_id` and `row_count` — both fixed-offset
  memcpy operations on the same encoded header buffer.
- `remove`/`erase` updates `row_count`.
- `accounts.size()` reads the header's `row_count` field directly.

Toggling tracking on after rows already exist requires a one-time scan to populate the count;
toggling off just sets the field back to `nullopt` (re-serializes the header without it).

Keeping `row_count` inside the header struct rather than in a separate trie key means each
mutation touches one trie value, not two — and pSSZ's optional-field encoding keeps the cost of
"disabled" down to a single presence byte plus the unchanged auto-id update path.
Defer until a benchmark says it matters.

## Schema validation

The schema is the canonical pSSZ encoding of the reflected struct hierarchy. xxh64 of the
canonical bytes is the schema hash. Both are written into the header on first write and checked
on every open.

```cpp
enum class schema_check {
   skip,         // trust the caller (tools)
   hash_only,    // fast: xxh64 of compiled vs stored
   structural,   // hash mismatch → decode stored schema, full structural compare
};

accounts_table accounts(tx, "accounts/", schema_check::hash_only);
```

Outcomes of a `structural` open, given compiled vs stored schema:

| compiled.hash | compiled.version | resolution |
|---|---|---|
| == stored.hash    | == stored.version | identical — open R/W |
| ≠                 | < stored.version  | stored is newer — open R/O; R/W requires `force_overwrite=true` |
| ≠                 | > stored.version  | compiled is newer — open R/W; header schema upgraded on first write |
| ≠                 | == stored.version | structural compare must succeed (rare "fork" case, see below) |

### Schema versioning

```
schema_version = sum( fixed_field_count(t) for t in unique reflected types )
```

Every reflected type contributes its number of declared fields. Sum across all unique types in
the schema's transitive closure (no double-counting). Adding any field to any type bumps the sum
by exactly 1, so the version is a free total ordering as long as schema evolution is linear.

The version is **not** unique — two divergent code branches that each add one field to different
sub-types both produce `version + 1`. That is the "fork" case the table can detect (hash differs
but version matches) and which the structural comparator must resolve.

This library assumes linear schema evolution: at any time at most one branch is "ahead". Out-of-
band branch reconciliation (merging two diverged schemas) is out of scope; if you fork the
schema, you have to merge before redeploying.

### Cross-version write safety

When the compiled schema is older than the stored schema (`compiled.version < stored.version`),
naive writes from older code would zero out the trailing fields stored on disk that the older
code doesn't know about. The library defends against this by default.

`modify(pk, λ)` over a write-view:

1. Read the existing record bytes.
2. Decode the fixed-region prefix that compiled knows about; lambda mutates a wview over those
   fields.
3. On commit, re-emit the new record as `compiled fixed-region ‖ trailing fixed-region bytes from
   original ‖ heap region from original`, with any heap pointers that referenced trailing fields
   preserved verbatim.
4. Heap data referenced by fields the compiled code touched is re-serialized normally.

The result: older code can patch the fields it knows about, and the newer fields it doesn't
survive untouched. Reads see the modified fields plus the preserved tail.

`force_overwrite=true` (set on the table, or per-call on `modify`) skips the preservation and
writes only what compiled knows about — useful for deliberate schema rollback. Without the flag,
attempting `put(value)` (full replace) on a row whose stored schema is newer is rejected.

### Stored-schema location

The canonical schema bytes live in a separate trie key (`prefix ‖ 0x00 ‖ 0x01`), not in the hot
header. See *Table metadata → Canonical schema record* above. This split keeps the hot path
(open, insert) reading only the 40-byte fixed header, while the kilobyte-sized schema bytes are
fetched only on the structural-validation slow path or by external readers.

A future optimization is a shared schema-registry tree keyed by `schema_hash → schema bytes`
when many tables share the same row type. The hot header already carries the hash, so swapping
to indirection is a localized change.

## Iterator + cursor lifecycle

Each iterator owns:

- A `psitri::cursor` (read cursor with a path stack).
- A `sal::read_lock` held for the iterator's lifetime.
- A snapshot `smart_ptr` to the cursor's root captured at construction.

These three together give the zero-copy read property:

- The read lock pins the segment so its memory cannot be `mprotect`'d or recycled.
- The smart_ptr pins the trie nodes so a concurrent COW cannot free them.
- Dereference returns `psio::view<T>` whose `const char*` points into the pinned segment.

### Cost of iteration

`lower_bound` / `upper_bound` / `begin` is one descent from root, O(log n). `++` and `--` are
O(1) at the leaf level (just bump `branch_number`) and O(log n) only when crossing a leaf
boundary. The path-stack avoids re-walking from root.

Each iterator allocates a cursor (a few hundred bytes including the path stack). Stand them up in
the loop scope and let RAII tear them down.

### Iterators across sub-transactions

An iterator's snapshot smart_ptr is independent of the cursor used by the transaction's writes:

- `tx.sub_transaction()` pushes a frame whose `cs_roots` add another ref to the cursor's root.
- Writes inside the sub take the COW path (refcount ≥ 2 because of both the frame snapshot and
  the iterator's snapshot).
- `sub.abort()` restores the cursor root from the frame snapshot. The iterator's snapshot is
  unaffected — it points at *its own* root, not the cursor's.
- `sub.commit()` pops the frame snapshot. Same: iterator unaffected.

Net behaviour: an iterator sees neither committed nor aborted writes from any sub-transaction
opened during its lifetime. It is a frozen view of the table at the moment it was constructed.

### Iterators across writes on the same transaction

The same picture applies to writes directly on the parent `tx` while an iterator is open.
Refcount-based COW does the right thing for free.

The deliberate consequence: while *any* iterator on the transaction is alive, every write takes
the full-COW path. The in-place fast path returns the moment the last iterator is destroyed.

> **Rule:** an iterator holds a stable snapshot of the table at the position it was constructed.
> Writes through the same transaction during the iterator's lifetime do not disturb its view,
> but pay the COW cost. Hold iterators in tight scopes when you intend to write.

### Iterate-and-modify pattern

```cpp
for (auto it = idx.lower_bound(key); it != idx.end(); ++it) {
   accounts.modify((*it).id, [](auto& w) { w.balance += 1; });
}
```

Inside the loop:
- `*it` returns the **pre-modify** value, even on subsequent iterations after a `modify`. The
  iterator's snapshot is frozen.
- The `modify` walks the trie via the cursor (not the iterator) and COWs the spine.
- `++it` advances on the snapshot.

If the user wants to see the post-modify values, they reopen the iterator after the loop. This
matches chainbase semantics: "iteration is over a snapshot."

### Surprising edge case: iterator survives sub.abort() of writes it observed

```cpp
auto sub = tx.sub_transaction();
accounts.put({1, "alice", 100, ""});
auto it = accounts.get_index<&account::id>().lower_bound(1);
// *it sees alice@100
sub.abort();
// *it STILL sees alice@100 — the iterator's smart_ptr keeps the just-aborted
// nodes alive. The cursor was rolled back; the iterator was not.
++it;
```

Internally consistent (iterator = frozen snapshot, abort is a cursor operation) but worth
documenting so users don't expect chainbase's "abort invalidates iterators" semantic. Once `it`
goes out of scope the orphaned nodes are released to dead_versions for GC.

### Iterator caching across host-call boundaries (spring shadow)

For pure C++ users (the psitri-multiindex public API), iterators are normal C++ objects with
RAII. Drop them when done.

For spring's WASM host-call shim — where one host call asks for an iterator and a later host
call dereferences or advances it — the iterator must be cached behind a stable integer handle.
The existing `iterator_cache` in `chain-psitri` already implements this for the contract_db
shadow path; it lifts cleanly into psitri-multiindex without changes to the underlying iterator
semantics.

## Comparison summary

|                              | chainbase             | psibase Table          | psitri-multiindex      |
|------------------------------|-----------------------|------------------------|------------------------|
| Storage backend              | mmap multi_index      | host KV via abi        | psitri trees (mmap)    |
| Record encoding              | `fc::raw`             | psio fracpack          | psio pSSZ              |
| Primary key                  | implicit `id_type`    | extractor              | extractor              |
| Non-unique secondary         | yes (`ordered_non_unique`) | tail-of-pk trick   | tail-of-pk trick       |
| Update API                   | `modify(obj, λ)`      | `put(value)`           | `modify(pk, λ)` w/ wview |
| Sub-transactions             | RAII undo session     | host-managed only      | psitri sub_transaction |
| Atomic across tables         | yes (one db)          | n/a                    | yes (one transaction)  |
| Zero-copy reads              | by-ref to mapped image| `psio::view<T>`        | `psio::view<T>` + read-lock |
| In-place modify              | yes for fixed fields  | not exposed            | yes for fixed fields, when uniquely owned |

## Lifecycle reference

```cpp
auto db = psitri::database::open(path);
auto ws = db->start_write_session();

// One block / outer transaction.
auto tx = ws->start_transaction(0);

accounts_table accounts(tx, "accounts/");
balances_table balances(tx, "balances/");

// One nested transaction (e.g. one EOS transaction inside a block).
{
   auto trx = tx.sub_transaction();

   accounts.put({1, "alice", 100, ""});
   balances.put({1, 100});

   try {
      run_action(...);
      trx.commit();           // merges into outer
   } catch (...) {
      trx.abort();            // rolls back accounts + balances atomically
      throw;
   }
}

// Iterate to verify state — read lock held only for the iterator's scope.
{
   auto idx = accounts.get_index<&account::name>();
   for (auto it = idx.lower_bound("a"); it != idx.end(); ++it) {
      psio::view<const account> v = *it;
      std::cout << v.id() << " " << v.balance() << "\n";
   }
}

tx.commit();                  // publishes the new root atomically
```

## Open implementation questions

- **psio bounds annotations**: `maxFields(N)` and `maxDynamicData(N)` are required for the
  table_header (and beneficial for row types with bounded variable-length fields) — they let
  the encoder shrink offsets from u32 to u8/u16 and the fixed_size header to u8. These
  annotations are a planned addition to psio alongside the existing
  `definitionWillNotChange()`. Until they land, the encoder falls back to today's u16
  fixed_size + u32 offsets — the design works either way; the bounds form is just tighter.
  Track as a psio enhancement.
- **Reserved auto-id ranges**: in-memory id-range caching (`(lo, hi]` with batched header bumps)
  needs the transaction to expose an abort callback so the table can restore its cached range on
  sub.abort(). Defer until benchmarks demand it; eager allocation is correct out of the gate.
- **Schema registry tree**: when many tables share a schema, embedding canonical schema bytes in
  every header duplicates them. A shared schema-registry root keyed by `schema_hash → schema
  bytes` deduplicates. Header layout already accommodates this (empty `schema` field + lookup by
  hash).
- **Schema evolution rules**: linear-only is the documented assumption. The "fork" case (two
  branches each adding to different sub-types, same `schema_version` sum) is detectable but not
  resolvable in-library — needs out-of-band reconciliation. We should write up the disallowed
  schema mutations explicitly (no field reorders, no type changes, append-optional only,
  matching psibase's existing rules).
- **Multi-process readers**: psitri's MVCC supports cross-process read-only snapshots through
  the segment-version mechanism. The table API just needs a `snapshot()` constructor that takes
  a published root version rather than a live transaction. No fundamental obstacle.
- **WASM host-call iterator cache**: existing `iterator_cache` in `chain-psitri` is the right
  shape; needs to be lifted out of the spring tree into psitri-multiindex (or at least the
  `iterator_cache.hpp` header) so non-spring consumers don't pay for spring-specific shim code.
