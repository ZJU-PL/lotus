# Mosaic

Mosaic is a theory-modular solver for constrained Horn clauses (CHCs) over
fixed-size bit-vectors. It partitions clauses into bit-vector and integer
arithmetic fragments, solves each fragment with Z3 Spacer, and exchanges facts
through theory translations.

This is the Lotus integration of the Mosaic 1.0 artifact accompanying:

> Omer Rappoport, Orna Grumberg, and Yakir Vizel. *Bit-Precise CHC
> Satisfiability Using Theory-Modular Reasoning.* ATVA 2026.

The upstream implementation used the names `MultiTheoryHorn`, `mth`, and
`MT_fixedpoint`. The integrated public API lives in `lotus::mosaic`; its main
entry point is `MosaicFixedpoint` in
`third-party/Mosaic/include/Mosaic/MosaicFixedpoint.h`.

## Build and run

Mosaic is vendored under `third-party/Mosaic` and built by default with Lotus.
It uses the Z3 installation already selected by the top-level CMake
configuration and is wired up from `third-party/CMakeLists.txt`.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DLOTUS_BUILD_TESTS=ON
cmake --build build --target lotus-verify-mosaic verification_tests
build/bin/lotus-verify-mosaic path/to/problem.smt2
build/bin/lotus-verify-mosaic --backend z3 path/to/problem.smt2
```

The tool accepts standard Horn SMT-LIB2 directly and obtains bit-vector widths
from the declarations in the file. Mosaic is the default backend; `--backend
z3` runs the same parsed problem with Z3's bit-vector fixedpoint engine for
comparison.

Each input must contain exactly one `(query name)` for a zero-argument relation
and exactly one rule defining that relation. The parser converts that rule to
Mosaic's existential-query representation. All eleven artifact workloads are
retained as concrete 4-bit micro inputs under
`benchmarks/micro/Verification/Mosaic`; their filenames make the concrete
width explicit. They are not installed or registered by the executable.

## License

Mosaic is distributed under the MIT license. See [LICENSE](LICENSE).
