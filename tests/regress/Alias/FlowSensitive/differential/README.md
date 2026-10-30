# Reduced differential findings

These inputs are findings/calibration data, not passing parity tests. The files
return zero when their runtime behavior matches the expectation encoded in the
file. Run each with LLVM 14 `lli`, with and without an extra argument; the two
invocations exercise both outcomes in `selected-struct-store.ll`.

| Input | Expected observation | Observations before the differential fixes |
| --- | --- | --- |
| `selected-struct-store.ll` | `r` may point to `a` or `b` | Lotus fspta misses `a`; Lotus vfspta and both SVF modes retain both |
| `indirect-unchanged-global.ll` | `r` points to `old` after calling `noop` | Lotus vfspta and both SVF modes return empty; Lotus fspta preserves `old` |
| `memcpy-pointer-slot.ll` | `r` points to `new` | Lotus modes return `new`; the local SVF configuration returns `old` |
| `pointer-array-initializer.ll` | `r` points to `a` | Lotus modes return `{a,b}` under array collapsing; local SVF modes return empty |
| `main-argv.ll` | `argv` and `argv[0]` are non-null for the tested host invocations | All four modes return empty: agreement is not a soundness proof |

Current Lotus builds preserve `{a,b}` in the selected-store case and `{old}` in
the indirect-noop case in both solver modes. The unit suite also covers writing
callees (the old value must be killed), alternative callees that modify
different objects, and memcpy into a previously unstored local destination.
The other rows remain reference/modeling differences to investigate.

The memcpy and array findings concern this local SVF build/configuration,
including its LLVM 14 frontend adapter. They must not be interpreted as general
claims about every SVF version. Neither implementation is an unquestioned
oracle. Compare using `scripts/compare_svf_fspta.py`; its JSON retains queried
value names, exact arguments and binary/model hashes.
