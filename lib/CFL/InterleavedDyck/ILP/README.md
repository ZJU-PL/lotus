# Single-source/single-target Interleaved-Dyck ILP engine (POPL 2023)

This module implements Yuanbo Li, Qirun Zhang, and Thomas Reps, *Single-Source-
Single-Target Interleaved-Dyck Reachability via Integer Linear Programming*,
POPL 2023. It consumes `Core::Graph` and exposes
`lotus::cfl::interleaved_dyck::ilp::Solver`.

```cpp
#include "CFL/InterleavedDyck/ILP/Solver.h"
namespace dyck = lotus::cfl::interleaved_dyck;

dyck::Graph graph;
graph.addEdge(0, 1, dyck::Label::openParenthesis(0));
graph.addEdge(1, 2, dyck::Label::closeParenthesis(0));
auto result = dyck::ilp::Solver{}.analyze(graph, 0, 2);
```

## Paper-to-code mapping

| POPL 2023 component | Implementation |
| --- | --- |
| Algorithm 1, path expression generation | Existing `lotus::pathexpressions::PathExpressionComputer` (Tarjan 1981) |
| HCAG terminal/concat/or/star nodes | `RegexBuilder`, with structural hash-consing |
| ILP standard form, Theorem 4.2 | `StandardForm::expand`; distributes concatenation over alternatives and rewrites `(a|b)*` as `(a*b*)*` (generalized to n alternatives) |
| Algorithm 2, bottom-up count table | `Encoder::compute` over a postorder DAG traversal |
| Cycle traversal integer variables | `Encoder::fresh(true)` for each star; variables are shared across all parenthesis dimensions contributed by that cycle |
| Nonlinear-product abstraction | Fresh unrestricted integer variables for cells whose child count already contains a variable |
| Distinct-expression variable independence | `copyWithFreshVariables` alpha-renames variables when combining distinct concatenation children |
| Algorithm 1, ILP feasibility | Z3 integer linear arithmetic through the stable Z3 C API |
| Section 4.4, LCL pruning | A derived epsilon-NFA graph per HCAG node, checked by the existing LCL engine's summary relation |

The LCL engine now exposes `Result::summary_pairs`, which records endpoint pairs
with any trellis summary, not just accepting summaries. This distinction is
required: a single unmatched opener is a valid fragment of a larger InterDyck
word, even though it is not itself an accepting path. Epsilon-only pairs are
also included. The existing `upper_bound` contract is unchanged.

## Result semantics and limitations

`may_reach == false` is a rejection by the paper's over-approximation. A true
result is only a candidate, not a concrete balanced-path witness. The ILP
encoding preserves the paper's intentional linearization of products involving
existing variables; it does not add constraints that would make this
approximation exact. As discussed in the paper, parenthesis ordering is only
partially enforced by LCL pruning and InterDyck reachability remains undecidable.

The path-expression standard form can have exponentially many disjuncts. The
optional `allow_unmatched_opens` selects the paper's optional nonnegative final-count
constraints instead of balanced equalities. `max_disjuncts` and
`max_expression_nodes` limits throw
`std::length_error`; no partial result is returned. Z3 `unknown` is reported as
an exception, never silently treated as infeasible. The implementation requires
the Z3 library already discovered by Lotus's root CMake configuration.

## Command line

Executable: `lotus-cfl-interleaved-dyck ilp`. Unlike the LCL engine, this
engine is demand-driven, so `--query SOURCE TARGET` is required:

```bash
build/bin/lotus-cfl-interleaved-dyck ilp --query 0 4 graph.dot
build/bin/lotus-cfl-interleaved-dyck ilp --query 0 4 \
    --allow-unmatched-opens --max-disjuncts 10000 graph.dot
```

Use `--no-lcl-refinement` to compare against the pure counting encoding. The
command prints the query verdict plus path-expression, disjunct, and integer
variable/constraint counts.
