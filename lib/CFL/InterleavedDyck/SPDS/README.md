# Synchronized pushdown systems (SPDS)

This module implements the reachability construction of:

Johannes Spath, Karim Ali, and Eric Bodden. **Context-, Flow-, and
Field-Sensitive Data-Flow Analysis using Synchronized Pushdown Systems**.
POPL 2019, PACMPL 3, Article 48. DOI: 10.1145/3290361.

Primary specification: the paper above, particularly Sections 2-4
and 5.1-5.3. The paper describes the call/field encodings and synchronization,
but delegates `post*` and `pre*` to established PDS algorithms. This module
supplies an independently written saturation implementation; it does not import
the authors' Java implementation or reproduce their entire experimental artifact.

Public namespace: `lotus::cfl::interleaved_dyck::spds`.
Library target: `CanaryInterleavedDyckSPDS`.
Executable: `lotus-cfl-interleaved-dyck spds`.

## Guarantees and scope

Each individual PDS closure is exact relative to the supplied rules and seed
regular language. Synchronization is the conjunction in Definition 4, not a
same-path, two-stack reachability test. Two different graph paths may witness
the two projections (Section 4.1, Figure 7). Therefore the graph result is named
`upper_bound` and positive queries are named `mayReach` / `mayAccept`.

For the default graph query, both the initial and final stacks are empty:

```
R_call  = {(u,v) | some u->v path has a balanced parenthesis projection}
R_field = {(u,v) | some u->v path has a balanced bracket projection}
upper_bound = R_call intersection R_field
```

A true interleaved-Dyck path witnesses both projections and is never discarded.
A missing pair certifies absence of such a path. A present pair need not have a
single balanced witness. The implementation deliberately does not intersect
path witnesses or iteratively prune one PDS with the other; those changes
would not be Definition 4's construction.

The paper also queries nonempty field stacks (access paths) and pending calling
contexts. Its Figure 7 contains an outstanding outer call. Use a precise stack
query or existential pending-call mode for that example: the default
empty/empty graph query is intentionally a different question.

This is an engine and a front-end-neutral data-flow rule builder. It is **not**
a complete Soot/Boomerang/IDEal or LLVM pointer-analysis/typestate front end.
Clients remain responsible for extracting CFG/call edges, kills and identity
flows, allocation sites, alias-induced rules, and sound strong-update decisions.
Section 5.1's alias-injection mechanism is supported by rule insertion and
illustrated by the Figure 8 test, not by an automatic general points-to client.
Section 5.3's primitives are provided (`pre*`, forward queries); automatic
backward-demand/forward-answer scheduling remains a client responsibility.

## Components and correspondence with the paper

| Header | Implemented functionality | Paper basis |
|---|---|---|
| `Pushdown.h` | Normal/push/pop PDS rules; regular seed languages; forward `post*`; backward `pre*`; weighted saturation; incremental insertion | Definitions 1-2, Section 2; Sections 5.2-5.3 |
| `Synchronized.h` | Call-PDS over variable controls and statement stacks; field-PDS over variable-at-statement controls and field stacks; synchronized configuration queries | Definition 3, Sections 3.1-3.3, Definition 4 |
| `Semiring.h` | Boolean weights and finite binary-relation typestate weights; customizable finite-height idempotent semiring interface | Section 5.2 |
| `Solver.h` | Lotus directed/typed Graph adapter, all-pairs and per-source/per-target queries | Graph specialization of the Section 4 construction |

For `SynchronizedSystem`, the call-PDS controls are globally unique variable
IDs, its stack symbols are statement IDs, and the top symbol is the current
statement. The remaining call stack lists pending return sites. Field-PDS
controls are `(variable, statement)` pairs. Its stack is an access path.

Each builder operation contributes **one data-flow transfer**, as follows:

| Operation | Call-PDS | Field-PDS |
|---|---|---|
| `addNormal(x@s,y@t)` | replace `s` with `t`, change control `x` to `y` | preserve field top and change location |
| `addStore(x@s,y@t,f)` | same normal call-PDS rule | prepend `f` to field stack |
| `addLoad(x@s,y@t,f)` | same normal call-PDS rule | pop precisely `f` |
| `addCall(x@s,y@entry,return_site)` | replace `s` by `entry return_site` | preserve field stack |
| `addReturn(x@exit,y@caller)` | pop `exit`, map control to `y` | normal flow to `y@caller` |

As in the paper's Table 2, a call-PDS pop rule does not separately test the
specified `caller` statement: the actual next statement is revealed by the
saved call stack. The field-PDS follows the explicitly supplied possible
caller edge. Synchronization requires agreement on the queried statement.

Transfers are explicit: clients supply identity flows, kills, caller edges, and
variable/statement metadata. The raw `Graph` adapter instead uses vertices as
controls; each label changes only its corresponding stack.

## Saturation and implementation decisions

A PDS rule replaces one top symbol by zero, one, or two symbols. Stacks are
top-first; epsilon is an automaton label, never a stack symbol. Forward and
backward saturation index rules and two-symbol dependencies, intern generated
push states, and maintain epsilon prefix/suffix closure. Weighted push
continuations retain prior history and the rule weight in execution order.

Graph-derived preserve/push rules are parameterized by the current top instead
of expanded across the alphabet. Transitions use a flat record arena, hash
key-to-ID indexes, integer-ID adjacency, and separate epsilon lists. Every
strict weight promotion is re-enqueued.

### Regular seed normalization

A client-supplied `RegularSet` can contain incoming edges to PDS controls,
accepting controls, and epsilon cycles. Before saturation, seed states that can
reach a final are cloned and each corresponding original control is connected
to its clone by epsilon; dead seed states are discarded. This ensures no edge
enters an original PDS control in the initial automaton. Without this standard
separation, post* rules can incorrectly rewrite a seed stack's continuation. A
regression explicitly exercises that failure mode.

The primitive regular-set API permits unioned seeds within a single PDS.
**Do not independently union different allocation-site seeds in the two PDSs
and then synchronize:** that would mix objects. `SynchronizedSystem::postStar`
and `preStar` deliberately accept one synchronized configuration at a time.
Use one result/session per allocation site, as Section 5.1 describes.

### Weighted execution order

The domain contract is:

```
using Weight = ...;
Weight zero() const;
Weight one() const;
Weight combine(Weight, Weight) const;
bool combineWith(Weight &left, const Weight &right) const;
Weight extend(Weight, Weight) const;
// Optional fused update used when the destination transition already exists:
bool extendAndCombine(Weight &target, const Weight &left,
                      const Weight &right) const;
// Weight also supports equality/inequality.
```

`extend(a,b)` always means executing `a` and then `b`. Combine must be
associative, commutative and idempotent; extend associative and distributive;
`combineWith` must update its left operand to the same value as `combine` and
return whether that value changed. It lets saturation avoid copying a weight
only to test equality. If supplied, `extendAndCombine` must update `target` to
`combine(target, extend(left, right))`; domains without it use the two-step path.
Domains may additionally define a `PreparedWeight` and matching prepared-extend
methods; the PDS stores that immutable rule metadata once and reuses it.
zero must annihilate extend; the ascending order induced by combine must have
finite height. Domain objects can hold state (e.g. the number of typestates).
Algebraic laws are a client contract, not automatically checked properties.

Weighted post* automaton paths accumulate weights **right to left**, whereas
pre* paths accumulate them **left to right**. Forward rules append their rule
weight to the consumed edge weight; backward rules prepend it to the RHS path
weight. This is essential for noncommutative typestate relations. A pre* query
still returns weights in the original program's forward execution order, not
an inverse typestate transition.

`RelationSemiring(n)` represents arbitrary finite binary relations on `n`
typestates. Combine is union and extend is relational composition. Bit rows
span as many machine words as needed; there is no 64-state limit. For this
domain every edge has at most `n*n` strict weight promotions. This does not
implement IDEal's independent alias-based strong-update decisions.

### Protected bottoms

The graph adapter and field-PDS builder encode an empty stack by a protected
bottom symbol `0`. It is distinct from automaton epsilon and is never popped.
External label/field IDs are mapped to positive symbols, including external
ID `0`. Graph labels use a 64-bit symbol for `unsigned(id)+1`, so `UINT_MAX`
does not overflow. Normal and store edges use parameterized rules rather than
being expanded once per stack symbol.

The call-PDS builder follows the paper literally: its top is a statement, and
it does not add a bottom statement. A synchronized configuration always has a
current statement; a primitive call-PDS configuration whose statement stack
was entirely popped is not by itself such a synchronized configuration.

## APIs

### Lotus graph queries

```cpp
#include "CFL/InterleavedDyck/SPDS/Solver.h"
namespace dyck = lotus::cfl::interleaved_dyck;
namespace spds = dyck::spds;

dyck::Graph graph;
graph.addEdge(0, 1, dyck::Label::openParenthesis(1));
graph.addEdge(1, 2, dyck::Label::openBracket(2));
graph.addEdge(2, 3, dyck::Label::closeParenthesis(1));
graph.addEdge(3, 4, dyck::Label::closeBracket(2));

spds::Solver solver;
auto analysis = solver.prepare(graph);
auto all = analysis.analyzeAll();
bool candidate = all.mayReach(0, 4);
auto successors = analysis.analyzeFrom(0); // Bulk empty-stack readout.
auto forward = analysis.queryFrom(0);
auto backward = analysis.queryTo(4);
// Both true for this graph:
(void)forward.mayReach(4);
(void)backward.mayReach(0);
// Precise endpoint/predecessor stack pair at node 2:
(void)forward.mayAccept(2, {1}, {2});
(void)backward.mayAccept(2, {1}, {2});
```

`Options::parentheses` and `Options::brackets` select `StackAcceptance::Empty`
(default) or `Any` for the queried vertex. In forward mode, Any permits pending
opens but never mismatched pops or underflow. In backward mode, it quantifies
over predecessor stacks that can reach the empty-stack anchor, which is a
different question from accepting a forward prefix. The CLI only exposes
prefix flags for forward queries to avoid that ambiguity.

`prepare` compiles the two projections once. `analyzeAll` performs two
saturations per source; `analyzeFrom` and `analyzeTo` use one anchor and bulk
read out every opposite endpoint. `queryFrom` and `queryTo` retain the two
automata for detailed stack queries. `analyzeDemands` accepts a vector of
source/target pairs and groups it by
the smaller number of distinct sources or targets unless a post/pre direction is
forced. Source/target anchors must exist; queries about an unknown candidate
vertex return false. `Result` also exposes both independent projection relations
and total statistics. No graph edges are symmetrized; duplicate arcs are handled
by the shared Graph.

### Synchronized data-flow and typestate

```cpp
#include "CFL/InterleavedDyck/SPDS/Synchronized.h"
namespace spds = lotus::cfl::interleaved_dyck::spds;

spds::RelationSemiring weights(3);
spds::SynchronizedSystem<spds::RelationSemiring> system(weights);
// Nodes are {globally_unique_variable_id, statement_id}.
system.addNormal({0, 1}, {1, 2}, weights.transition(0, 1));
system.addStore({1, 2}, {2, 3}, 7);
system.addCall({2, 3}, {3, 10}, 4, weights.transition(1, 2));
system.addReturn({3, 10}, {4, 4});
system.addLoad({4, 4}, {5, 5}, 7);

auto result = system.postStar({{0, 1}, {}, {}});
// Explicit empty calling context and empty access path:
auto relation = result.weight({{5, 5}, {}, {}});
bool reaches_state_2 = relation.contains(0, 2);
// Any calling context at the callee, with access path field 7:
auto in_callee = result.weightAt({3, 10}, {7});
// Exact backward reachability for each projection:
auto predecessors = system.preStar({{5, 5}, {}, {}});
```

For unweighted use, `SynchronizedSystem<>` uses `BooleanSemiring`.
`mayAlias(node)` asks whether the base variable (empty field sequence) reaches
the tracked seed at that node under some context. `mayReachNode(node)` allows
both an arbitrary context and an arbitrary access path. The names intentionally
separate those questions. `weightAt` filters the weighted call answer through
the unweighted field automaton, as described in Section 5.2.

### Incremental rules and regular languages

```cpp
spds::PushdownSystem<> pds;
auto x = pds.addControl();
auto y = pds.addControl();
auto seed = spds::RegularSet::singleton(pds.controls(), {x, {42}});
spds::SaturationSession<> session(pds, seed);
session.run();
session.addRule(x, 42, y, {43});
const auto &updated = session.run();
// updated.accepts(y, {43}) == true
```

Controls are fixed for a session; new rules may refer to any predeclared
control. An existing transition's new weight is propagated as well. Call
`run()` after insertions. `result()` throws while work is incomplete. Returned
references belong to the session and must not outlive it. A session references
the base PDS and its precompiled post/pre rule indexes instead of copying them,
so the PDS must outlive the session. The
higher-level `SynchronizedSystem` can also accept new transfers between
queries, but its next `postStar`/`preStar` rebuilds the pair; it does not
automatically maintain a coordinated pair of incremental sessions.

`RegularSet` has explicit state, transition and final-state construction for
regular rather than singleton seeds. `postStar(pds, seed)` and
`preStar(pds, target)` return immutable queryable `Automaton` values. Use
`accepts(p,{})` for an empty PDS stack, or `acceptsPrefix(p,{})` for existence of
any accepted stack. Weighted equivalents are `weight` and `weightWithPrefix`.

## Complexity and resource limits

There is no stack-height, path-length, or iteration cutoff. Cycles in the
finite automata represent unbounded stacks. Exact single-PDS saturation
terminates over the stated finite-height semiring contract.

The implementation materializes epsilon-composed edges. Transitions live in a
flat arena with an append-only open-addressing ID index; pre* waiting records
also retain integer IDs, and fixed push rules use precompiled generated-state
slots. Complexity depends on the number of materialized transitions, strict
weight promotions, and matching rules. Graph all-pairs mode repeats saturation
per source and stores a quadratic pair relation.

`Limits` can bound automaton states, stored transitions, or successful weight
updates. Zero means unlimited. Counts include seed normalization and initial
transitions. Limits are per PDS saturation, not whole-process memory limits;
they do not bound input parsing/rule compilation or query evaluation costs.
Exceeding a limit throws `ResourceLimit`; no incomplete relation is returned
as an upper bound. A failed incremental session is poisoned and cannot return
or resume a partial result. Discard it and restart with an appropriate budget.

## Build and CLI

In an already configured Lotus build, reconfigure with its usual dependencies
and options, then build:

```sh
cmake -S . -B build -DLOTUS_BUILD_TESTS=ON
cmake --build build --target CanaryInterleavedDyckSPDS \
  lotus-cfl-interleaved-dyck cfl_tests
ctest --test-dir build -R 'cfl_tests.InterleavedDyckSPDS' --output-on-failure

build/bin/lotus-cfl-interleaved-dyck spds --query 0 4 \
  tests/regress/CFL/InterleavedDyck/SPDS/crossing.dot
```

The CLI requires one explicit `--all-pairs`, `--source`, `--target`, `--query`,
or `--queries FILE` scope. Batch files
contain one `SOURCE TARGET` pair per line and may be read from stdin with
`--queries -`. `--direction auto|post|pre` controls pair and batch evaluation.
Run `--help` for prefix, output, and resource options. Exit 0 means a completed computation
(including an unreachable answer), 2 means invalid input, and 3 means a resource
limit. Failure cases print no result relation.

The serial corpus runner referenced by earlier revisions was removed; the
checked-in corpus manifest no longer exists, so never infer all-pairs from graph
size. Drive the CLI directly with an explicit `--queries` batch file:

```sh
build/bin/lotus-cfl-interleaved-dyck spds --queries pairs.txt --timings graph.dot
build/bin/lotus-cfl-interleaved-dyck affine-spds --all-pairs --timings graph.dot
```

Use a Release or RelWithDebInfo build for performance comparisons; Debug leaves
the template-heavy saturation and affine operations unoptimized. `--timings`
reports the instrumented projection, session-setup, saturation, and readout
times; the CLI also reports facts, promotions, and rules.
