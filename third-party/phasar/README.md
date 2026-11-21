# PhASAR DFI adaptation

Upstream: https://github.com/secure-software-engineering/phasar
Source checkout revision: `88366fc69`.
License: [MIT](LICENSE.txt).

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
