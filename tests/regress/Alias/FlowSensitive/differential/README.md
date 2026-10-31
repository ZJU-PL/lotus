# Reduced differential findings

These inputs are findings/calibration data, not passing parity tests. The files
return zero when their runtime behavior matches the expectation encoded in the
file. Run each with LLVM 14 `lli`, with and without an extra argument where the
input has two outcomes (for example `selected-struct-store.ll`).

Results below were produced with the local SVF build
(`~/Downloads/SVF-master/Release-build/bin/wpa`, LLVM 14) through
`scripts/compare_svf_fspta.py` and its SVF observer
(`scripts/svf-differential/`). SVF needs `-model-consts=true` to model separate
constant objects; without it several string constants collapse into one object
and spurious "only in SVF" alias pairs appear.

| Input | Expected observation | Observed after the fixes |
| --- | --- | --- |
| `struct-field-call-boundary.ll` | a non-zero-offset pointer field crosses call boundaries in both directions | Lotus fspta/vfspta and SVF agree; this input failed before the region-granular argument summary fix (see below) |
| `selected-struct-store.ll` | `r` may point to `a` or `b` | all four modes retain both |
| `indirect-unchanged-global.ll` | `r` points to `old` after calling `noop` | Lotus fspta/vfspta: `old`. SVF fspta/vfspta: empty. SVF's SVFG does not connect a resolved indirect callee that does not write memory; `-svfg-with-ind-call=true` instead reports `new` |
| `memcpy-pointer-slot.ll` | `r` points to `new` | Lotus: `new`. SVF: `old`. SVF does not model `memcpy` into a pointer slot here |
| `pointer-array-initializer.ll` | `r` points to `a` | Lotus: `{a,b}` (arrays collapsed). SVF: empty (global pointer-array initializer not seeded) |
| `main-argv.ll` | `argv` and `argv[0]` are non-null for the tested host invocations | all four modes return empty: agreement is not a soundness proof |

`struct-field-call-boundary.ll` is the regression input for a Lotus SVFG
builder bug: interprocedural memory summaries recorded only the *argument*
whose pointee is accessed, and `FormalIn`/`FormalOut` later mapped that to the
argument's base region.  A field access at a non-zero offset therefore had no
formal memory node, the callee load observed an empty memory state, and a
callee store was invisible to the caller (`fspta` and `vfspta` alike).  The
unit test `FlowSensitivePTATest.PropagatesStructPointerFieldsAcrossCalls`
covers the same case.

Neither implementation is an unquestioned oracle. When re-running a
comparison, record the queried value names, exact arguments, and binary/model
hashes alongside the results:

```sh
scripts/svf-differential/build.sh      # once per SVF build
scripts/compare_svf_fspta.py tests/regress/Alias/FlowSensitive/differential \
    --pattern '*.ll' --mode both --alias-pairs \
    --svf-extra=-alias-check=false --svf-extra=-model-consts=true
```
