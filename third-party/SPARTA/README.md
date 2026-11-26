# SPARTA in Lotus

This directory vendors the complete C++ header library from the local
`SPARTA-main` snapshot by Meta Platforms, Inc. and affiliates. Its MIT license
is retained in [LICENSE](LICENSE). The separate Rust implementation is not part
of Lotus's C++ integration.

SPARTA is disabled by default. Enable it with:

```sh
cmake -S . -B build -DLOTUS_ENABLE_SPARTA=ON
```

Lotus components link the header-only `SPARTA::SPARTA` target and retain the
upstream `#include <sparta/...>` paths. The upstream target names `sparta` and
`sparta::sparta` are also available. The library requires C++17 and the platform
threading facilities exposed by `Threads::Threads`; it has no Boost dependency.

The upstream tests and migration regressions live in
`tests/unit/Dataflow/SPARTA`. To select them, enable `LOTUS_BUILD_TESTS` and set
`LOTUS_TEST_SUBSYSTEMS=dataflow/sparta`. These tests require Google Test and
Google Mock in addition to Lotus's normal build dependencies.

## Standalone and installed use

The vendored directory also supports standalone configuration and installation:

```sh
cmake -S third-party/SPARTA -B build-sparta -DCMAKE_INSTALL_PREFIX=/path/to/prefix
cmake --install build-sparta
```

Consumers can use `add_subdirectory`, or `find_package(sparta CONFIG REQUIRED)`
with `CMAKE_PREFIX_PATH` pointing to the installation. The build directory can
also be supplied as `sparta_DIR`. The installed package is relocatable and
exports both `sparta::sparta` and `SPARTA::SPARTA`.

## Changes from upstream

- Optional results use `std::optional`; disjoint unions use standard variant
  access and throw `std::bad_variant_access` for a wrong-type `get()`. Visitors
  retain support for a declared `result_type`. Throwing alternatives use
  transactional storage to preserve the upstream nonempty-variant guarantee.
- Typed exception attributes and source locations use SPARTA's own
  `error_info`, `get_error_info`, `throw_file`, `throw_line`, and
  `throw_function`. Diagnostic text formatting differs from Boost. Access from
  a known SPARTA exception type also works with RTTI disabled; recovering
  metadata through an erased standard exception type requires RTTI.
- Local intrusive pointers retain the upstream reference counts and Patricia
  leaf tags. Collision buckets retain single-element inline storage. Aligned
  pointer constants retain pointer-sized tagging, and small finite-lattice
  encodings retain compact storage. Fixed-width bit encodings implement the
  operations used by SPARTA rather than the general Boost.Multiprecision API.
- Flat containers retain comparator state and comparator-based lookup. Their
  set operations handle self-aliasing and non-default orderings correctly.
- Thread-pool exception state is cleared after propagation. POSIX tests retain
  the upstream 512 KiB traversal stack limit without Boost.Thread.
- Hash values can differ from upstream; equality and hashing remain consistent.

This is source integration rather than binary compatibility with Boost-backed
SPARTA types. The upstream abstract-domain, persistent-container, graph-ordering,
and fixpoint APIs are retained, with the standard-type substitutions above.
