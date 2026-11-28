# Loop Analysis

This subtree provides Lotus's loop-centric analysis stack. It wraps LLVM loop
structure information into self-contained Lotus representations and
incrementally materializes derived analyses for dependence reasoning,
induction-variable discovery, iteration-space disjointness, stack privatization,
and SCC classification.

## Layout

Public headers under `include/Analysis/Loop/` mirror implementations under
`lib/Analysis/Loop/`:

- Root (`LoopStructure.*`, `LoopForest.*`, `LoopEnvironment.*`, `LoopContent.*`,
  `FunctionLoopAnalyses.*`, `LoopNestingGraph.*`): core loop descriptors,
  per-function orchestration (`FunctionLoopAnalysesPass` /
  `FunctionLoopAnalysesWrapperPass`), per-loop state (`LoopContent`), loop
  boundary live-ins/live-outs, and the interprocedural loop nesting graph.
- `Dependence/` (`LoopDependenceGraph.*`, `LoopDependenceBuilder.*`,
  `LoopSCCDAG.*`, `LoopCarriedDependencies.*`): loop dependence graph import and
  refinement passes, Tarjan SCC condensation, and loop-carried dependence
  tagging.
- `Scalar/` (`LoopInvariants.*`, `InductionVariable.*`, `InductionVariables.*`,
  `LoopGoverningInductionVariable.*`, `ScalarEvolutionReferencer.*`):
  loop-invariant detection, induction variable recognition, exit-condition
  governing IV attribution, and SCEV step-expansion helpers.
- `Memory/` (`LoopIterationSpaceAnalysis.*`, `MemoryCloningAnalysis.*`):
  affine subscript disjointness proofs and stack-allocation privatization
  analysis.
- `Classification/` (`LoopCarriedVariable.*`, `LoopSCCClassification.*`,
  `SCCDAGAttrs.*`): reducible loop-carried variable algebra, `GenericSCC`
  attribute models, and `SCCDAGAttrs` classification.

## Pipeline

Inside `LoopContent`, derived analyses materialize incrementally:

1. Build the base `LoopDependenceGraph` and `LoopSCCDAG` from the PDG.
2. Refine memory edges via `LoopDependenceBuilder::refineLoopDependenceGraph`
   (PRVG, affine iteration space, imported read-only PDG edges, memory cloning,
   and thread-safe allocator/deallocator specifications), retaining
   `MemoryCloningAnalysis` and the control+variable `LoopSCCDAG` when valid.
3. Build `LoopEnvironment` (live-ins / live-outs), `InvariantManager`, and
   `InductionVariableManager`.
4. Materialize `LoopIterationSpaceAnalysis` over the refined IV state.
5. Classify SCCs via `SCCDAGAttrs`, reusing the cached `MemoryCloningAnalysis`.

## Build

This directory builds two static libraries:

- `CanaryLoopStructure`: standalone `LoopStructure` descriptor linking only
  `LLVMAnalysis` (consumed directly by `CanaryProfile` as well as
  `CanaryLoopAnalysis`).
- `CanaryLoopAnalysis`: full loop analysis stack linking `CanaryLoopStructure`,
  `CanaryCFG`, `CanaryPDGCore`, `CanaryPDGConstruction`, `AliasSpecManager`, and
  `LLVMAnalysis`.
