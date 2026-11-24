# PhASAR adaptations

Upstream: https://github.com/secure-software-engineering/phasar
Source checkout revision: `88366fc69`.
License: [MIT](LICENSE.txt).

## Library dataflow summaries

`config/taint.spec` contains selected dependencies adapted from upstream
`lib/Utils/LibCSummary.cpp`. They use Lotus's existing `PIPE` format
(`destination access source access`); no PhASAR runtime is required.
Copyright (c) PhASAR contributors; see the retained license above.

The additional audited batch covers 24 previously unmodeled function names:

- Memory/string searches: `memmem`, `memrchr`, `rawmemchr`, `strchrnul`,
  `index`, `rindex`, `wcschr`, `wcsrchr`, `wcspbrk`, `wcsstr`, `wmemchr`.
- Buffer operations: `memccpy`, `wmemset`, `strxfrm`, `wcsxfrm`.
- Address conversions: `inet_ntop`, `inet_pton`.
- Math output parameters: `frexpf`, `frexpl`, `modff`, `modfl`, `sincos`,
  `sincosf`, `sincosl`. The existing `modf` output index is corrected to 1.

Dependencies are reviewed against the GNU libc manual's
[search functions](https://sourceware.org/glibc/manual/latest/html_node/Search-Functions.html),
[collation functions](https://sourceware.org/glibc/manual/latest/html_node/Collation-Functions.html),
[host address functions](https://sourceware.org/glibc/manual/latest/html_node/Host-Address-Functions.html),
[normalization functions](https://sourceware.org/glibc/manual/latest/html_node/Normalization-Functions.html),
[rounding functions](https://sourceware.org/glibc/manual/latest/html_node/Rounding-Functions.html),
and [trigonometric functions](https://sourceware.org/glibc/manual/latest/html_node/Trig-Functions.html).
The migration distinguishes pointer values (`V`) from pointed-to data (`D`).
Searches preserve haystack data on the returned pointer; search operands can
influence the selected address without copying their contents into the result.
Copies and fills propagate into both destination memory and returned memory
where a destination pointer is returned. Collation transforms propagate input
data into the output buffer and returned length.

These are additive may-flow models, not complete libc specifications. They do
not encode byte ranges, zero-length/no-match/error conditions, locale state,
strong updates, or general pointer alias relationships. Conditional outputs
can therefore be conservatively tainted. Unreviewed varargs, callbacks,
multi-level pointer updates, ABI-dependent aggregate arguments, and upstream
misspellings remain excluded. Existing source, sink, and sanitizer policies
are unchanged.

`tests/unit/Dataflow/IfdsIde/LibCSummaryTest.cpp` checks input/output direction,
pointer versus data dependencies, void versus scalar results, and a complete
source-to-fill-to-search-to-load flow in dense and sparse IFDS execution, with
a separate clean buffer as a negative control.

## DFI backend

`include/Dataflow/IFDS/Solver/DFI/` and
`lib/Dataflow/IFDS/Solver/DFI/DFIExplodedSupergraph.cpp` adapt the design and
implementation of upstream `DataFlow/IfdsIde/Solver/DFI*` to C++17, LLVM 14 and
Lotus's existing `IFDSProblem` interface. Copyright (c) 2026 Fabian Schiebel
and other PhASAR contributors. The original permission notice is retained in
LICENSE.txt.

The backend keeps ID-based graph materialization, callee-first SCC scheduling,
interval-indexed local entry/exit summaries, and optional forward/backward
reachability. It uses Lotus's FunctionIndex, CallGraphSCCs and interval index;
there is no dependency on PhASAR's C++20 concepts, typed vectors, CSR graph,
solver mixins or pointer-analysis interface. Lotus-specific adaptations include
caller-fact-dependent return functions, additive summary flows, partial/unknown
callee sets, zero-fact policy, explicit alias injection and certified sparse
transfers. Results are owning snapshots instead of non-owning upstream views.

The present implementation rebuilds local interval indexes when dirty and uses
standard containers. It is an experimental alternative backend; upstream or
paper performance figures are not measurements of this adaptation. No IDE
lattice-value computation, tabulation path-edge export or CLI engine selection
is included. The existing IFDS and IDE solvers remain independent.

## Precision and IDE migration layers

The focused XTaint migration is available through
`IDEAliasAwareExtendedTaintAnalysis` (also exposed as
`IDEExtendedTaintMemoryAnalysis`). Its `AbstractMemoryLocation` facts are
interned, field-sensitive, k-bounded paths with optional alias-aware loads,
stores, sanitizer strong updates, interprocedural formal-to-actual transfer,
and the existing Lotus source/PIPE configuration model.

`IterativeIDESolver` is a Lotus-compatible facade over `IDESolver`: it keeps
the existing tabulation semantics while optionally collecting dense integer
IDs for instructions, facts, and path edges. It can release the pointer-rich
tabulation caches after value propagation. `IDEFeatureTaintAnalysis` accepts
callback-driven generators and named feature sets while retaining its legacy
64-bit source-mask API.
