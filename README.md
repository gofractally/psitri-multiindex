# psitri-multiindex

Multi-index container backed by [psitri](https://github.com/gofractally/arbtrie)
trees with [psio](https://github.com/gofractally/psio) pSSZ-encoded records.

The goal is to provide a chainbase-style multi-index abstraction (primary
+ secondary indexes, undo sessions, transactional MVCC reads) on top of
psitri's COW radix trie, with zero-copy record access via psio's
`fracpack`/pSSZ encoding.

## Status

Pre-alpha. The build skeleton fetches psitri and psio via CMake
`FetchContent` and links them together. The actual multi-index API is
not yet implemented.

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

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

### As a subproject (consumed by spring or other parents)

When `add_subdirectory(...)`'d by a parent that already declared psio
and/or psitri via `FetchContent_Declare`, this project's declarations
become no-ops and the parent's pins win — single source of truth, no
duplicate checkouts. See `CMakeLists.txt` for the pattern.

## Layout

```
src/        — implementation
include/    — public headers (psitri_multiindex/*.hpp)
tests/      — unit + integration tests
docs/       — design notes, ADRs
external/   — populated by FetchContent at configure time (gitignored)
```
