# Optimization tools

This directory contains command-line frontends for Lotus optimization passes.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

The binaries are written to `build/bin/`.

## Tool

| Tool | Purpose | Notes |
| --- | --- | --- |
| `lotus-opt-ipo` | Driver for Lotus interprocedural IPO passes | Implemented by `tools/optimization/lotus-opt-ipo.cpp`; runs the MemorySSA/ShadowMem-backed passes in `lib/Optimization/IPO`. |
| `lotus-lif` | Driver for the migrated LIF isochronous transform | Implemented by `tools/optimization/lotus-opt-lif.cpp` under the unified `Security/LIF` subsystem; currently commented out/disabled in `CMakeLists.txt`. |
| `lotus-opt-purity` | Purity inference and summary workflow driver | Implemented by `tools/optimization/lotus-opt-purity.cpp`; automatically prepares SeaDsa/ShadowMem-backed MemorySSA unless `--disable-memoryssa-prep` is passed; defaults that prep to `--sea-dsa=ci` for robustness, but accepts an explicit `--sea-dsa=...` override. |
| `lotus-opt-prefetch` | Driver for Lotus software prefetch passes | Implemented by `tools/optimization/lotus-opt-prefetch.cpp`; runs the software prefetching pass in `lib/Optimization/Prefetch`. |

## Input and output

The optimization tools consume LLVM bitcode or textual IR and write either
bitcode or LLVM assembly.

```bash
clang -emit-llvm -c test.c -o test.bc
build/bin/lotus-opt-ipo test.bc -o out.bc
build/bin/lotus-opt-purity test.bc --purity-summary-file summaries.json
build/bin/lotus-opt-ipo test.bc -S -o out.ll
build/bin/lotus-opt-prefetch test.bc -S -o prefetch.ll
```

## LIF migration target

`lotus-lif` is the Lotus-integrated frontend for the imported LIF
isochronous transform (currently commented out in `tools/optimization/CMakeLists.txt` pending runtime validation).

Current limitations:

- The imported code still carries its original GPLv3 provenance and headers.
- The LLVM 14 build migration is in place, but runtime validation is not yet
  complete for all inputs.

## IPO passes

- `--ip-all` enables every IPO pass.
- `--ipdse` runs interprocedural dead store elimination.
- `--ip-rle` runs interprocedural redundant load elimination.
- `--ip-sink` runs interprocedural store sinking.
- `--ip-forward` runs interprocedural store-to-load forwarding.

## Prefetch pass

- `lotus-opt-prefetch` always runs `SWPrefetchingLLVMPass`.
- Prefetch-distance selection is controlled by the pass-level flags already
  declared in `lib/Optimization/Prefetch`, such as `--prefetch-distance-provider`,
  `--profile`, `--dist`, and `--llm-dist`.
- When `--prefetch-distance-provider=profile` is used, pass a sample-profile
  file with `--profile=<file>`.

## Examples

```bash
# Run the full interprocedural optimization bundle
build/bin/lotus-opt-ipo test.bc --ip-all -o opt.bc

# Run a selected subset of IPO passes
build/bin/lotus-opt-ipo test.bc \
  --ipdse \
  --ip-forward \
  -o opt-subset.bc

# Run software prefetching with explicit LBR distances
build/bin/lotus-opt-prefetch test.bc \
  --prefetch-distance-provider=lbr \
  --dist=32 \
  -o prefetch.bc

# Run profile-guided software prefetching
build/bin/lotus-opt-prefetch test.bc \
  --prefetch-distance-provider=profile \
  --profile=perf.prof \
  -o prefetch-profile.bc
```

## Related documentation

- `lib/Optimization/README.md` documents the optimization libraries.
- `lib/Optimization/IPO/README.md` documents the passes exposed by `lotus-opt-ipo`.
- `lib/Optimization/Prefetch/` contains the software prefetching implementation
  used by `lotus-opt-prefetch`.
