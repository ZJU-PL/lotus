# Loop Analysis

This subtree provides Lotus's loop-centric analysis stack. It wraps LLVM loop
structure information into Lotus-specific representations and then incrementally
materializes derived analyses used by dependence reasoning, induction-variable
discovery, loop-carried dependence classification, and transformation-oriented
loop summaries.

## What It Builds

At the top level, `FunctionLoopAnalyses` constructs and owns per-function loop
state:

- `LoopStructure` and `LoopForest` describe individual loops and their nesting.
- `LoopContent` is the per-loop container for lazily materialized analyses.
- Both the new pass manager (`FunctionLoopAnalysesPass`) and the legacy pass
  manager (`FunctionLoopAnalysesWrapperPass`) are supported.

The usual materialization pipeline inside a `LoopContent` is:

1. Build a `LoopDependenceGraph` from the PDG.
2. Collapse the loop graph into a `LoopSCCDAG`.
3. Materialize scalar facts such as invariants and induction variables.
4. Build the `LoopEnvironment` for live-ins/live-outs.
5. Mark loop-carried dependences.
6. Run iteration-space and memory-cloning analyses.
7. Classify SCCs with `SCCDAGAttrs`.

## Major Components

- `LoopForest.*`: builds a forest of nested `LoopTree` nodes from discovered
  `LoopStructure`s and dominance summaries.
- `LoopContent.*`: owns the derived analyses for one loop and coordinates
  materialization and invalidation.
- `LoopDependenceGraph.*` and `LoopLDGBuilder.*`: import PDG edges into a
  loop-scoped dependence graph over internal and external values.
- `LoopSCCDAG.*`: computes SCCs over the loop dependence graph and exposes a
  DAG ordering between SCCs.
- `Invariants.*`, `InductionVariable.*`, `InductionVariables.*`,
  `LoopGoverningInductionVariable.*`, and `ScalarEvolutionReferencer.*`:
  recover loop invariants, induction variables, and scalar-evolution-backed
  structure.
- `LoopEnvironment.*`: computes the loop environment, including live-in and
  live-out producers.
- `LoopCarriedDependencies.*`: identifies which dependence edges cross loop
  iterations.
- `LoopIterationSpaceAnalysis.*`: uses ScalarEvolution and IV information to
  prove when memory accesses are disjoint across iterations.
- `MemoryCloningAnalysis.*`: identifies stack allocations that can be privately
  cloned per iteration or per parallel context.
- `SCCDAGAttrs.*`: classifies SCCs into categories such as loop-iteration,
  reduction, induction-variable, periodic, recomputable, memory-clonable, and
  unknown loop-carried SCCs.
- `Variable.*` and `LoopNestingGraph.*`: helper representations used by the
  higher-level loop analyses.

## Key Concepts

- `LoopDependenceGraph` distinguishes `Variable`, `Control`, and `Memory`
  dependences, and records whether an edge is loop-carried.
- `LoopEnvironment` models the values that enter or leave a loop body and is
  used by later classification and transformation logic.
- `LoopSCCDAG` groups mutually dependent loop values so clients reason about
  cyclic structures rather than isolated instructions.
- `SCCDAGAttrs` turns raw SCCs into semantic categories that are more directly
  useful to optimizations and parallelization analyses.

## Build

This directory builds the static library `CanaryLoopAnalysis` and links against:

- `CanaryCFG`
- `CanaryDebugInfo`
- `CanaryPDGConstruction`
- `CanaryAliasAnalysisWrapper`
- `LLVMAnalysis`

## Headers

Public interfaces live under `include/Analysis/Loop/`. Most clients should
start from `FunctionLoopAnalyses.h` and `LoopContent.h`, then materialize only
the analyses they need.
