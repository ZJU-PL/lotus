# Analysis Library

Core analyses built on LLVM IR.

| Subdir | Purpose |
|--------|---------|
| **CallGraph** | Interprocedural call graph construction (CHA, RTA, VTA, FPA, OTF) and indirect-call resolution. |
| **CFG** | Reachability, dominators, post-dominators, topological order, back edges, code metrics. |
| **ControlDependence** | Standard CD, NTSCD variants, DOD variants, strong control closure, and Lotus ICFG integration. |
| **DebugInfo** | MetadataManager, LoopStructure, debug-info-driven annotations. |
| **Loop** | Loop forest/structure, dependence graphs, SCC DAGs, invariants, induction variables, loop-carried dependences, iteration-space and memory-cloning analyses. See `Loop/README.md`. |
| **Memory** | Heap pointer analysis and memory utility analyses (`HeapPointerAnalysis`). |
| **NullPointer** | Null-check, null-flow, null-equivalence; context-sensitive variants. |
| **Profile** | Profile-guided hotness analysis utilities. |
| **SCCP** | Sparse conditional constant propagation analysis and related support. |
| **Summary** | Summary and side-effect analyses: `Purity` (function purity & attribute inference), `ParameterSummary` (interprocedural parameter effects & resource table), and `Multiplicity` (allocation multiplicity classification). See `Summary/README.md`. |
| **TypeHierarchy** | C++ class hierarchy, vtable reconstruction, virtual-call resolution. |

Security-oriented side-channel analyses and transformations live under
`lib/Security`.
