# Composable C++ Libraries with CMake FetchContent
## Standalone + Diamond-Proof Dependency Management

A convention for managing a stack of independently consumable C++ libraries
where each one builds standalone but composes cleanly when a master project
consumes several together.

This document explains the convention from first principles, uses our actual
library tree as a worked example, and addresses the obvious community
critiques head-on. It is opinionated — pick the tradeoffs deliberately or
pick a different convention.

---

## 1. The problem

Any organization with more than a couple of internally-owned C++ libraries
that depend on each other needs five things to hold simultaneously:

1. **Standalone consumable** — clone one library, build it, no other org
   repos required.
2. **Composes without duplication** — a parent that depends on several of
   them ends up with one physical copy of each transitively-reached
   library, regardless of how many ancestors declare the same dep.
3. **Master controls versions** — the parent's pin wins over any child
   library's pin.
4. **One source checkout per dep across multiple build configurations** —
   release, debug, sanitized, cross-compiled.
5. **Local-dev friction-free** — editing a transitive dep doesn't require
   commit-push-bump cycles before the parent picks it up.

The usual answers each give up at least one of these:

- **Submodules** (Boost, Qt) fail (2) when libraries carry their own
  submodules and the parent reaches the same dep multiple ways.
- **Monorepo** (LLVM) trades (1) — single library extraction is awkward.
- **Bazel / Buck** solve it well, but require leaving CMake.
- **vcpkg / Conan** want a publish step and a release artifact, which
  fights (5) for libraries under active co-development.

The convention here is CMake-native and stays inside CMake's existing
machinery. It gives up some things — covered in section 9 — but holds
all five constraints for the shape of stack we have.

## 2. The example tree

```
psio                       (serialization — pSSZ, fracpack, schema)
psitri                     (mmap MVCC COW radix-trie KV store)
psizam                     (standalone WebAssembly engine)
   └─ depends on psio
psitri-multiindex          (chainbase-style multi-index over psitri trees)
   ├─ depends on psio
   └─ depends on psitri
psiserve                   (web app server / runtime)
   ├─ depends on psio, psitri, psizam, psitri-multiindex
```

Five repos, each shippable on its own. `psiserve` reaches `psio` three
different ways (direct, via `psizam`, via `psitri-multiindex`), and
`psitri` two ways (direct, via `psitri-multiindex`). Constraint (2)
demands these collapse to one copy each.

## 3. Why submodules don't fit

If each library carries its deps as nested submodules, `psiserve` ends up
like this:

```
psiserve/
├── external/psio/                          (submodule, pin A)
├── external/psitri/                        (submodule, pin B)
├── external/psizam/external/psio/          (submodule, pin C — possibly ≠ A)
├── external/psitri-multiindex/external/psio/    (submodule, pin D — possibly ≠ A, C)
└── external/psitri-multiindex/external/psitri/  (submodule, pin E — possibly ≠ B)
```

`psio` exists three times at potentially three different pins. Building
all three produces ODR violations. Skipping the duplicates requires
`if(NOT TARGET psio) add_subdirectory(...)` guards everywhere plus
careful `--init` usage to avoid populating the nested copies.

It works with discipline. The discipline is noisy and easy to forget.
The convention below removes the discipline — the machinery enforces
the invariant.

## 4. Why package managers don't fit (for *this* job)

vcpkg, Conan, CPM, and similar are excellent for **stable third-party
dependencies** — Boost, OpenSSL, simdjson, fmt, the libraries you don't
edit. We use `find_package(Boost REQUIRED)` against system packages
exactly here.

They are *less* well-suited for **internal libraries under active
co-development**:

- A package manager wants a versioned release artifact. Internal libraries
  evolve commit-by-commit.
- A developer editing `psio` wants their `psiserve` build to pick up the
  edit immediately. Package managers want a publish step.
- The package manager's own machinery (registry, build-from-source vs
  binary, lockfile) is overhead for libraries you control directly.

For stable third-party deps: package manager. For your own libraries that
you edit weekly: not the right tool.

## 5. Why monorepos don't fit (here)

The monorepo answer is clean: one repo, all sub-projects, no diamond
because there's only one of everything.

It also has well-known costs: large clone size, complicated CI, harder for
external consumers to use a single library, and harder to keep the
boundaries that make libraries truly reusable. It's the right answer for
some organizations (Google, Meta, LLVM) and the wrong answer for many.

Our shape — four libraries, each consumable individually — does not need a
monorepo. It needs *coordination*, not *unification*.

## 6. The convention

CMake's `FetchContent` module has a property that, deployed deliberately,
solves the diamond cleanly:

> The first `FetchContent_Declare(name ...)` for a given name wins; later
> declarations of the same name are silently ignored.

This is the foundation. Every library in the stack declares its deps via
`FetchContent_Declare`. When a parent project consumes several of them,
the parent declares them *first*, in its own top-level CMakeLists. The
parent's pins become the authoritative pins. The child libraries' own
declarations turn into no-ops.

> **The single most important rule: leaf-first ordering.** A parent's
> `FetchContent_Declare` for *every* direct and transitive dep must
> appear **before** any `FetchContent_MakeAvailable` (its own or one
> implicitly triggered by a child library's CMakeLists). Once
> `MakeAvailable` runs for a child, that child's own
> `FetchContent_Declare` calls fire, and at that point a later parent
> `FetchContent_Declare` for the same name is too late — the child's
> pin is what got recorded. Get the order wrong and the diamond
> guarantees evaporate silently. The CMake community has converged on
> this rule in 2025–2026 discussions; it's the well-known footgun in
> this pattern.

Together with three layout choices, this produces the convention.

### 6.1 Source goes into `./external/<name>/`, not `build/_deps/<name>-src/`

CMake's default `FetchContent_BASE_DIR` is `${CMAKE_BINARY_DIR}/_deps/`.
Sources cloned into the build tree means each build dir (release, debug,
sanitized, cross) maintains its own independent clone of the same source
— wasted disk, wasted time, more network traffic.

We override `SOURCE_DIR` explicitly:

```cmake
FetchContent_Declare(psio
   GIT_REPOSITORY https://github.com/gofractally/psio.git
   GIT_TAG        main
   SOURCE_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/external/psio)
```

`SOURCE_DIR` is in the source tree. First configure clones into
`external/psio/`. Subsequent configures of any build dir find `external/psio/`
populated and reuse it. Constraint 4 satisfied.

### 6.2 `external/` is gitignored

The cloned source isn't tracked. The pin lives in the `GIT_TAG` line of
CMakeLists.txt, which IS tracked. Bumping a dep is a one-line edit in
source control; the diff is unambiguous.

This is also why submodules' source-tree integration is unnecessary here —
the version IS controlled, just at a different layer.

### 6.3 Each library declares its OWN deps as fallbacks

Every library's CMakeLists.txt declares everything it needs:

```cmake
# In gofractally/psizam/CMakeLists.txt:
include(FetchContent)
FetchContent_Declare(psio
   GIT_REPOSITORY https://github.com/gofractally/psio.git
   GIT_TAG        main
   SOURCE_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/external/psio)
FetchContent_MakeAvailable(psio)
```

This is what makes psizam standalone-consumable (constraint 1). When
someone clones psizam and builds it, this declaration fires, psio is
fetched, and the build proceeds.

When psizam is consumed by psiserve — which has *already* declared psio
with its own pin — the `FetchContent_Declare(psio ...)` inside psizam is
silently ignored. Psiserve's pin wins. Constraint 3 satisfied.

### 6.4 Local-dev override uses CMake's built-in escape hatch

```bash
cmake -B build -DFETCHCONTENT_SOURCE_DIR_PSIO=$HOME/dev/psio
```

`FETCHCONTENT_SOURCE_DIR_<NAME>` is a CMake built-in. When set, FetchContent
uses the local path as if it had been cloned there. No fetch, no clone, no
submodule pointer to manage. Constraint 5 satisfied.

This is the recommended local-development workflow when editing a transitive
dep. Edit the override path; the parent project picks up the change at
the next reconfigure. Commit when ready; remove the override; the parent
project moves to the new pin.

### 6.5 The complete picture

For a parent project (here, psiserve) that wants the whole stack:

```cmake
# psiserve/CMakeLists.txt — top-level
cmake_minimum_required(VERSION 3.20)
project(psiserve)

include(FetchContent)

# Declare every direct + transitive dep first. The order matters: each
# Declare must come before any FetchContent_MakeAvailable that would
# trigger a sub-library's own Declare for the same name. Sort
# leaf-deps-first so a library that's about to declare its own deps
# always finds those deps already declared.
FetchContent_Declare(psio
   GIT_REPOSITORY https://github.com/gofractally/psio.git
   GIT_TAG        abc123
   SOURCE_DIR     ${CMAKE_SOURCE_DIR}/external/psio)
FetchContent_Declare(psitri
   GIT_REPOSITORY https://github.com/gofractally/arbtrie.git
   GIT_TAG        def456
   SOURCE_DIR     ${CMAKE_SOURCE_DIR}/external/psitri)
FetchContent_Declare(psizam
   GIT_REPOSITORY https://github.com/gofractally/psizam.git
   GIT_TAG        ghi789
   SOURCE_DIR     ${CMAKE_SOURCE_DIR}/external/psizam)
FetchContent_Declare(psitri-multiindex
   GIT_REPOSITORY https://github.com/gofractally/psitri-multiindex.git
   GIT_TAG        jkl012
   SOURCE_DIR     ${CMAKE_SOURCE_DIR}/external/psitri-multiindex)

# psizam's own CMakeLists declares psio.
# psitri-multiindex's own CMakeLists declares psio AND psitri.
# Both are already declared by us above, so those inner declarations
# become no-ops. psiserve's pins win for every dep.
FetchContent_MakeAvailable(psio psitri psizam psitri-multiindex)

# psiserve's own targets
add_subdirectory(src)
```

After configuring this:

```
psiserve/
├── external/                            ← gitignored
│   ├── psio/                            (cloned at abc123)
│   ├── psitri/                          (cloned at def456)
│   ├── psizam/                          (cloned at ghi789)
│   │   └── external/                    (NOT populated — its Declares were no-ops)
│   └── psitri-multiindex/               (cloned at jkl012)
│       └── external/                    (NOT populated — its Declares were no-ops)
├── build-release/                       (no source under here)
├── build-debug/                         (no source under here, shares external/)
└── CMakeLists.txt
```

One copy of each dep. Three reach-paths to `psio` collapse to one
checkout. Two reach-paths to `psitri` collapse to one. Multiple build
dirs share them all. Master controls every pin. Each library remains
independently buildable when cloned standalone.

## 7. Workflows

| Task | Command |
|---|---|
| First clone of psiserve | `git clone gofractally/psiserve && cmake -B build && cmake --build build`. First configure populates `external/`. |
| Add a second build dir (e.g. debug) | `cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug`. Reuses `external/`. |
| Edit psio while building psiserve | `cmake -B build-debug -DFETCHCONTENT_SOURCE_DIR_PSIO=$HOME/dev/psio`. Local edits visible immediately. |
| Bump psio in psiserve | Edit `GIT_TAG abc123 → GIT_TAG xyz999` in psiserve's CMakeLists; reconfigure; FetchContent re-fetches. |
| Test psitri standalone | `git clone gofractally/psitri && cmake -B build && ctest --test-dir build`. |
| Build psitri and edit psio at the same time | `cmake -B build -DFETCHCONTENT_SOURCE_DIR_PSIO=$HOME/dev/psio`. |

Notice that the difference between standalone, parent-controlled, and
local-dev workflows is just the configure invocation. The library code,
the CMakeLists, the source tree — all unchanged.

## 8. The pin-drift safeguard

Each library's CMakeLists.txt declares its own known-good pins. When the
library is consumed by a parent that pins the same dep differently, the
parent's pin wins. This means a library can be tested against pin X but
end up built against pin Y when the parent declares Y.

For most cases this is fine. The library's own pin says "we last verified
against this version"; the parent's pin says "we want this version
specifically." Drift between the two happens naturally during development.

When drift becomes a problem (parent's pin breaks the library), the fix
is in one of two places:

1. The library bumps its own pin to match the parent and verifies.
2. The parent backs off to the library's pin until the library catches up.

A simple CI check in the library can fail fast if its declared pin and a
known-good consumer's pin diverge by more than N commits. Worth adding
once the cadence stabilizes; not necessary at the outset.

## 9. What we trade away

**Configure-time network access.** First configure on a fresh tree clones
from GitHub. Subsequent configures find `external/` populated and don't
re-fetch. CI without network: pre-populate `external/` from a snapshot,
or set `FETCHCONTENT_FULLY_DISCONNECTED=ON` after the first fetch.

**Reproducibility.** "Works on my machine" with `GIT_TAG main` is a real
concern: `main` moves, two developers configuring the same parent at
different times can end up with different `external/` contents. The
discipline that addresses it is straightforward — for any branch you
expect to be reproducible (release tags, locked CI configs, anything
billed as "this is what we shipped"), pin `GIT_TAG` to a SHA, not a
branch. The CMakeLists then *is* the lockfile: identical SHAs in →
identical sources out. Active-development branches can use `main` for
ergonomics while accepting that two configures may diverge until the
next pin update. CMake won't tell you which mode you're in; the
convention is to pin SHAs in any branch tagged for release.

**No `git status` summary across nested repos.** Submodules show
"submodule X has uncommitted changes". FetchContent doesn't. For
libraries you co-edit, point `FETCHCONTENT_SOURCE_DIR_<X>` at your dev
tree and `git status` works there normally.

**CMake-specific.** Consumers on Bazel or Buck can't use this as-is.
Acceptable for a CMake-native ecosystem.

**Order-dependent (the central footgun).** Parent declarations must come
before `FetchContent_MakeAvailable` triggers a child's own declarations.
The machinery doesn't enforce this. **Never mix `FetchContent_Declare`
calls with `add_subdirectory` of a child library that itself calls
`FetchContent_MakeAvailable` without verifying every transitive dep has
already been declared above it.** A header comment in the parent CMake
listing the discipline is the standard mitigation; CI can also assert
that `FetchContent_GetProperties(psio SOURCE_DIR …)` resolves to the
expected `external/psio` path before any consumer runs.

## 10. Reception

This isn't a novel build system or a new tool — it's three deliberate
choices on top of CMake mechanisms that already exist:
"first-declaration-wins" applied at scale, `SOURCE_DIR` pointed into the
source tree, and `FETCHCONTENT_SOURCE_DIR_<NAME>` as the dev override.
Anyone who's hit the diamond problem will recognize the shape.

The substantive critique to expect is the package-manager one: "you've
re-invented vcpkg/Conan poorly." It's a fair point as far as it goes —
package managers solve a superset of this problem for stable third-party
deps. They're a worse fit for internal libraries you edit weekly, where
the publish-version-consume cycle is friction without payoff. We use
both: package managers for `Boost`/`simdjson`/etc., this convention for
our own stack.

This pattern is also compatible with (and complementary to)
[CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) — many teams layer
CPM on top of `FetchContent` for ergonomics (version handling,
single-line `CPMAddPackage(...)` calls). Same first-declare-wins
discipline applies; CPM just makes the calls shorter. If your team
already uses CPM, swap the `FetchContent_Declare`/`MakeAvailable` pair
for the equivalent CPM invocation and the rest of the convention stands.

This is a convention, not a universal best practice. It fits a specific
shape — small stack of internally-owned, CMake-native, co-developed,
individually-consumable libraries. Outside that shape, other tools fit
better.

## 11. When NOT to use this convention

- **Heavy binary deps you don't control** (ICU, OpenSSL, Boost when used
  for compiled libraries). Use system / vcpkg / Conan.
- **Cycle-prone dep graphs.** FetchContent doesn't help; the cycle has
  to be broken architecturally.
- **Build systems other than CMake.** This is CMake-specific. A
  Bazel-using consumer will need different machinery.
- **Strict offline / air-gapped CI without a fetch cache.** Can be made
  to work, but the friction outweighs the benefit.
- **A single-library project with no internal deps.** Don't use a
  pattern designed for graph composition when your graph has one node.

## 12. Summary

The convention is a small set of CMake decisions that, taken together,
solve the diamond-dependency problem for an internal library stack
without leaving CMake or adding a package manager:

1. Each library declares its deps via `FetchContent_Declare`.
2. `SOURCE_DIR` is explicit and points into the source tree (not the
   build tree).
3. `external/` is gitignored.
4. Parent projects declare every direct and transitive dep first; CMake's
   "first declaration wins" rule then makes the parent's pins
   authoritative.
5. `FETCHCONTENT_SOURCE_DIR_<NAME>` is the local-dev override, no custom
   variables required.

The result is each library standalone-consumable, the parent project in
full control of versions, one copy per dep on disk regardless of how
many ancestors reach it, multiple build dirs sharing a single source
checkout, and local-dev overrides via a built-in CMake mechanism.

Whether this is "innovative" or "hacky" depends largely on which
constituency reads it. We've found it to be the lowest-friction option
for our specific stack shape; a different stack might pick differently
and be right.
