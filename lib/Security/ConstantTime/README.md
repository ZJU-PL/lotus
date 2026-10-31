# ConstantTime

`lib/Security/ConstantTime` hosts Lotus's CT-LLVM-based constant-time analysis
for side-channel detection in sensitive code. The pass tracks tainted secret
values, propagates them through def-use and alias relationships, and reports
cache, branch, and variable-timing leaks.

## Public entrypoints

- Header: `Security/ConstantTime/ConstantTimePass.h`
- Pass type: `CTPass`
- Pipeline name: `ctllvm`
- Plugin registration: `getPassPluginInfo()` and `llvmGetPassPluginInfo()`

## Internal layout

- `ConstantTimePass.cpp`: public pass facade and plugin registration
- `ConstantTimeAnalysis.cpp`: core taint propagation and leak detection
- `ConstantTimeInlining.cpp`: cloning and recursive inlining helpers
- `ConstantTimeTargets.cpp`: target/declassification selection and debug-info-based
  source matching
- `ConstantTimeDebugInfo.cpp`: internal debug metadata lookup helpers
- `ConstantTimeReporting.cpp`: JSON summaries, source printing, and statistics
- `ConstantTimeAnalysisSupport.h`: private analysis support declarations shared
  by the `.cpp` files

## Default options

`CTOptions` holds the analysis options; the defaults are:

- `type_system = true`
- `test_all_parameters = true`
- `enable_may_leak = true`
- `try_hard_on_name = true`
- `user_specify = false`
- `soundness_mode = true`
- `alias_threshold = 2000`
- `report_leakages = true`
- `time_analysis = false`
- `auto_continue = true`
- `inline_threshold = 10`
- `debug = false`
- `print_function = false`

`file_path` is used for source-file lookup during reporting and defaults to the
empty string.

## Testing

Unit coverage for this subsystem lives in `tests/unit/Security/ConstantTime/CryptoAnalysisTest.cpp`.
The tests cover:

- default-option compatibility
- `ctllvm` pipeline registration
- branch-leak reporting
- alias-threshold accounting
- inline failure statistics for indirect calls

## References

- CT-LLVM: Automatic Large-Scale Constant-Time Analysis
  [paper](https://eprint.iacr.org/2025/338.pdf),
  [artifact](https://github.com/Neo-Outis/CT-LLVM-Artifact)
- Related work: ECOOP 2024 CtChecker
