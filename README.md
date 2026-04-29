# psitri-multiindex

Chainbase-style multi-index container backed by
[psitri](https://github.com/gofractally/arbtrie) trees with
[psio](https://github.com/gofractally/psio) pSSZ-encoded records.

A `table<Row, Primary, Secondary...>` exposes:
- a primary index (auto-id or user-supplied key)
- any number of named secondary indexes (`ordered_unique` / `ordered_non_unique`)
- composite-key extractors via `composite_key<&Row::a, &Row::b, ...>`
- iteration in primary or any secondary tag's order, with bounds + `equal_range`
- zero-copy reads via psio `view<T, pssz>` pinned to the segment
- atomic `put` (collision pre-validation across every unique secondary)
- sub-transaction–scoped writes (commit/abort) inherited from psitri

## Status

Pre-1.0. Core API is in place and tested; the schema-hash / schema-version
validation story is the main outstanding piece. See [`TODO.md`](TODO.md)
for the open work.

## Quickstart

```cpp
#include <psitri_multiindex/table.hpp>
#include <psitri/database.hpp>
#include <psio/reflect.hpp>

struct Account {
   std::uint64_t id;
   std::string   name;
   std::uint64_t balance;
};
PSIO_REFLECT(Account, id, name, balance)

namespace {
   struct by_id;
   struct by_name;
   struct by_balance;
}

using accounts_table = psitri_multiindex::table<
   Account,
   psitri_multiindex::ordered_unique    <by_id,      &Account::id>,
   psitri_multiindex::ordered_unique    <by_name,    &Account::name>,
   psitri_multiindex::ordered_non_unique<by_balance, &Account::balance>>;

auto db = psitri::database::open("/tmp/accounts");
auto ws = db->start_write_session();
auto tx = ws->start_transaction(0);

accounts_table accounts(tx, "acc/");

// Auto-id insert. Sets row.id from the table header's next_id counter.
auto pk = accounts.insert(Account{0, "alice", 100});  // pk == 0
accounts.insert(Account{0, "bob",   250});            // pk == 1

// Tag-based reads.
if (auto a = accounts.find<by_name>(std::string("alice")))
   std::cout << a->balance << '\n';

// Range over a non-unique secondary.
for (auto&& a : accounts.equal_range<by_balance>(std::uint64_t{100}))
   std::cout << a.name << '\n';

// Read-mutate-write helper.
accounts.modify(pk, [](Account& a) { a.balance += 50; });

// Sub-transaction: commit makes writes visible; abort rolls them back
// across every index and the table header.
{
   auto sub = tx.sub_transaction();
   accounts.erase(std::uint64_t{1});
   sub.abort();   // bob is back
}

tx.commit();
```

See [`docs/design.md`](docs/design.md) for the full API surface and
design notes.

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requires a C++23 compiler. On macOS use Homebrew clang:
`-DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++`.

First configure clones psitri and psio into `external/` (next to the
source tree, not the build tree). Subsequent build dirs reuse the
checkout.

### Local-dev override

To point at a sibling checkout of psio or psitri without committing /
pushing the upstream:

```bash
cmake -B build \
      -DFETCHCONTENT_SOURCE_DIR_PSIO=$HOME/projects/psio \
      -DFETCHCONTENT_SOURCE_DIR_PSITRI=$HOME/projects/psitri
```

### As a subproject

When `add_subdirectory(...)`'d by a parent that already declared psio
and/or psitri via `FetchContent_Declare`, this project's declarations
become no-ops and the parent's pins win — single source of truth, no
duplicate checkouts. See [`docs/dependency-pattern.md`](docs/dependency-pattern.md)
for the full convention.

## Layout

```
src/        — implementation
include/    — public headers (psitri_multiindex/*.hpp)
tests/      — unit + integration tests
docs/       — design notes, dependency convention
external/   — populated by FetchContent at configure time (gitignored)
TODO.md     — open work, known sharp edges, parking lot
```
