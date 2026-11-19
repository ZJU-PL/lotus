# Comparing the local Lotus and SVF solvers

The observer links only to SVF and runs its existing `FlowSensitive` or
`VersionedFlowSensitive` class. It exports the same named-value alias schema as
`lotus-alias-fspta --comparison-json`. It does not change either solver.

From the Lotus repository root, using the local LLVM 14 and SVF builds:

```sh
cmake --build build --target lotus-alias-fspta -j 6
cmake -S scripts/svf-differential -B build/svf-differential \
  -DSVF_DIR="$HOME/Downloads/SVF-master/Release-build/lib/cmake/SVF" \
  -DLLVM_DIR="$HOME/Work/tools/llvm-project-llvmorg-14.0.0/build/lib/cmake/llvm" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/svf-differential -j 4

python3 scripts/compare_svf_fspta.py \
  --llvm-bin "$HOME/Work/tools/llvm-project-llvmorg-14.0.0/build/bin" \
  --svf-arg="-extapi=$HOME/Downloads/SVF-master/Release-build/lib/extapi.bc" \
  --output build/my-fspta-comparison \
  tests/regress/Alias/PTA \
  tests/regress/Alias/FlowSensitive/differential
```

Use a fresh output directory on each run. Python 3.11+ is required. To generate
100 additional pointer programs, add `--generate 100 --seed 0`. The generator
uses five families: straight-line updates, diamonds, bounded loops, direct
calls, and indirect calls. Half use stack pointer slots and half use struct
fields. The `.ll` sources, assembled `.bc` inputs, and per-run named bitcode
are retained under the output directory. These programs do not provide a
soundness oracle and do not exercise all LLVM instructions.

For larger inputs, use `--limit 128 --sample-seed 19 --timeout 30` to bound
query volume and each process's wall time. Sampling is applied separately to
each exporter's available values; the report records the intersection and
values present on only one side. Small cases in the accompanying audit use all
common exported values without sampling.

`results.json` contains four comparisons per input: Lotus vs SVF for each
mode, and fspta vs vfspta within each framework. It records the commands,
working directories, original/analyzed input hashes, binary/model hashes,
timings, return codes, queried values, unmatched queries, differing alias
pairs, and diagnostic points-to cardinalities. Full stdout/stderr is saved
beside each normalized input. `agree` means agreement on the compared alias
pairs; it does not mean equal points-to sets. Object IDs and raw set sizes are
not interchangeable across the frameworks. Fewer than two common values is
`insufficient`; a nonzero return code, timeout, or absent export is `error`.
Differences are findings and do not make the harness exit unsuccessfully;
errors and insufficient coverage do.

Two controls are necessary for meaningful comparisons:

- LLVM 14's **legacy** `-instnamer` names original instructions before either
  framework sees the input. The new-pass-manager form skips `optnone`
  functions; comparing numeric SSA slots after SVF introduces instructions
  can pair unrelated values. Both processes receive exactly the same named
  bitcode file.
- Analyses run from the repository root by default. Lotus finds
  `config/ptr.spec` and `config/modref.spec` relative to its working directory
  (or `LOTUS_CONFIG_DIR`). Running from per-case log directories silently
  changes its library models. `--working-directory` allows an explicit
  override. Keep `LOTUS_CONFIG_DIR` unset to use the recorded repository
  models, or set it deliberately.

SVF uses `-model-consts=true` so distinct constant objects do not all become
one artificial source of alias differences. Its external model is still
different from Lotus's model.