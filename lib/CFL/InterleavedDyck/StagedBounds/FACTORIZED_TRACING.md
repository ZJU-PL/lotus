# Factorized tracing for staged bounds

Factorized tracing computes the union of terminal edges contributing to selected
CFL-reachability facts without retaining a record for every derivation. Mutual
refinement uses this union to remove graph edges unsupported by a projection,
then repeats the projected analyses until the retained graph stabilizes.

The optimization preserves the supplied grammar's reachability relation and
contributing-edge closure. It does not change the staged solver's lower- and
upper-bound interpretation or make general interleaved-Dyck reachability exact.
See [the module overview](README.md) for those guarantees.

## Relations and derivations

Represent a fact as `(u, A, v)`: there is a path from vertex `u` to vertex `v`
whose label sequence belongs to the language of symbol `A`. Let `C` be the least
fixed point of the input terminal facts under the grammar productions:

- `A -> epsilon` adds `(u, A, u)` for each vertex.
- `A -> B` derives `(u, A, v)` from `(u, B, v)`.
- `A -> B D` derives `(u, A, v)` from `(u, B, w)` and `(w, D, v)`.

For each symbol, store the two views

```text
Out_A(u) = { v | (u, A, v) is in C }
In_A(v)  = { u | (u, A, v) is in C }
```

Eager tracing records the unary children and binary triples `(B, w, D)` for each
parent fact. Factorized tracing retains the relations and grammar instead. A
binary parent's possible derivations can then be reconstructed by intersecting
`Out_B(u)` with `In_D(v)`.

The required result is the union of contributing terminal edges, not a single
witness or the full derivation graph. Alternative productions and pivots must
therefore be preserved whenever they can contribute a new edge.

## Shared relation representation

[CnfGraph.cpp](CnfGraph.cpp) partitions both relation directions by symbol and
endpoint. Only populated rows are allocated. Rows adapt to their occupancy:

| Representation | Contents | Operations |
|---|---|---|
| Small sorted row | Individual endpoints | Binary-search membership |
| Compressed bitmap | Occupied word indices and 64-bit words | Word lookup and bit operations |
| Dense bitmap | A word array covering the vertex universe | Direct word lookup |

The same rows support saturation, membership tests, and backward tracing.
Reconstruction does not build another pair of adjacency indices. Compressed
rows skip absent words; densely populated rows avoid storing word indices.

## Saturation without provenance records

Each newly inserted fact is processed once as a worklist event. For an event
`(u, B, w)` and a production `A -> B D`, only the targets in

```text
Out_D(w) minus Out_A(u)
```

can produce new facts. The symmetric join processes an event `(w, D, v)` using
`In_B(w) minus In_A(v)`. Bitmap rows implement these differences word by word.
Unary productions propagate their corresponding facts, and nullable productions
seed reflexive facts.

A join chooses between two ways to find matching productions: iterate the
productions indexed by the event's RHS position, or iterate the symbols present
at the join vertex and look up matching RHS pairs. Choosing the smaller list
avoids scanning large sets of irrelevant grammar alternatives or incident
symbols.

Insertions are deferred until the current event's row traversals finish. This
prevents iterator invalidation when a sparse row grows or becomes a bitmap.
Only successful insertions create new worklist events.

Skipping a derivation whose parent is already known is valid for reachability:
it cannot enlarge the fixed point. In eager mode, however, that derivation may
still contribute provenance, so the engine records it even when the parent fact
already exists. Factorized mode recovers such alternatives from the completed
relations during tracing.

## Backward reconstruction

Tracing starts only after saturation finishes. Let `Q` be the selected root
facts, normally non-reflexive start-symbol facts, or one start fact for a targeted
query. Maintain a visited bitmap, a worklist of marked nonterminals, and a set of
contributing terminal edges.

```text
mark(edge):
    if edge has already been marked: return
    mark edge and update coverage counters
    if edge is terminal: add it to the answer
    otherwise: enqueue it for expansion

mark every root in Q that belongs to C
while the worklist is not empty:
    remove (u, A, v)
    for each A -> B:
        if (u, B, v) belongs to C: mark (u, B, v)
    for each A -> B D:
        for each w in Out_B(u) intersect In_D(v):
            mark (u, B, w)
            mark (w, D, v)
```

Empty productions have no terminal children. The visited bitmap terminates
recursive and nullable cycles without discarding alternative derivations.
Sparse rows assign bitmap slots to stored endpoints; bitmap rows assign slots
within their stored words. Slots and counters are initialized afresh for each
trace, including repeated queries over the same saturated graph.

### Coverage-based pruning

Marking a fact decrements counts of unmarked facts in its symbol relation, its
outgoing row, and its incoming row. This supports three shortcuts:

1. For `A -> B D`, skip further joins if both complete child relations are marked.
2. For a particular parent `(u, A, v)`, skip the join if both `Out_B(u)` and
   `In_D(v)` are marked completely. Unmarked facts elsewhere in those symbols
   must not prevent this local shortcut.
3. Stop tracing when every terminal fact has entered the answer.

These checks concern discovery, not completion of worklist processing. A marked
nonterminal is already scheduled for expansion, so rediscovering it through
another pivot cannot add work or change the eventual terminal-edge union.
Stopping after all terminal facts are found is safe because the result cannot
contain anything outside that set.

### Correctness

Saturation computes the least fixed point, so every stored fact has a finite
derivation from input terminal facts and empty productions. A reconstructed
unary child is stored, and a reconstructed binary pivot has both premises in the
completed relations. Substituting their finite derivations therefore gives a
valid derivation of the parent.

Conversely, every production application in any finite derivation of a selected
root has its premises in `C`. The backward traversal enumerates that production
and pivot unless the children have already been marked. Thus it finds every
contributing terminal edge. Coverage pruning omits only rediscoveries, and
visited marking limits expansions to the finite set of stored facts.

## Output materialization

[Grammar.cpp](Grammar.cpp) exports start facts as a unique vector when a caller
needs to enumerate them without another membership table. It reuses that vector
as the all-roots tracing input; a targeted trace needs only a singleton root.
The saturated engine and eager records are released before constructing the
public pair and graph containers.

Regularization uses a streaming consumer instead. Each start fact is filtered
by its product-automaton endpoints and mapped to original vertices immediately.
Pairs rejected by this projection are never stored in an intermediate pair set.
These lifetime and export choices apply to both tracing modes.

## Trimming the taint regularization product

[Regularization.cpp](Regularization.cpp) specializes product construction to the
taint automaton. For `k` bracket types, it has `k + 2` states: the initial,
accepting state `q0`, one intermediate state `qi` per type, and an accepting sink.

| Input edge | Automaton transition |
|---|---|
| Non-bracket edge | Preserve the automaton state and original label |
| `ob_i` at `q0` | Enter `qi`; emit a neutral label |
| `ob_i` at any intermediate state | Enter the sink; emit a neutral label |
| `cb_i` at `qi` | Return to `q0`; emit a neutral label |
| Any bracket edge at the sink | Stay in the sink; emit a neutral label |

There are no other bracket transitions. In particular, an intermediate state
can be entered only through its own opening type and can leave through either
its matching close or another opening. The sink retains the original automaton's
accepting behavior, including unmatched closing brackets.

Let `G_plain` contain only non-bracket edges of the input graph. For each type
`i`, compute:

```text
Entries_i = targets of ob_i edges
Exits_i   = sources of cb_i edges or of any opening-bracket edge
F_i       = vertices reachable from Entries_i in G_plain
L_i       = vertices in F_i that can reach Exits_i in G_plain
```

The implementation obtains `L_i` by reverse traversal restricted to `F_i`.
It materializes only these intermediate-state vertices and the original
transitions connecting them. All non-bracket edges at `q0` remain available,
since every original vertex is a possible query source and accepting endpoint.
The sink is constructed by forward reachability from the retained transitions
entering it, using all input edge kinds.

This trim deliberately ignores parenthesis balance. Every accepted product
path's intermediate segment has an entry, a non-bracket interior, and an exit;
all its vertices therefore lie in `L_i`. Every sink segment is reachable from a
retained sink entry. No accepted path is removed, and every retained edge belongs
to the full product. Intersecting these paths with the parenthesis-Dyck language
and projecting their endpoints consequently produces the same result.

Traversal marks and queues are reused across types, so scratch storage does not
require a vertex-by-state matrix. The value-flow client's fixed automaton is
constructed separately and does not use this taint-specific trim.

## Integration with the refinement methods

All three methods in [Solver.cpp](Solver.cpp) pass `options.factorized_tracing`
through [Refinement.cpp](Refinement.cpp) to the same saturation and tracing engine:

| Method | Grammar and root selection |
|---|---|
| `mutual-refinement` | Classic projections; all candidate roots |
| `stronger-grammar` | Parity/endpoint projections; all candidate roots |
| `on-demand` | Per-pair classic refinement, followed by per-pair parity refinement |

Neither saturation nor tracing depends on a particular Dyck production pattern.
The on-demand passes forward the target pair as well as the tracing mode.
Each solve reconstructs its own contributing-edge union; the optimization does
not provide incremental closure reuse between different target pairs.

`--factorized-tracing` selects reconstruction; eager tracing remains the default.
The CLI's `--method` selects the last pipeline stage, so prerequisite stages still
run. Product trimming and compact output handling benefit both modes and every
method using those prerequisites.

## Complexity and limits

Let `n` be the vertex count of the graph supplied to a saturation, `c = |C|` its
number of stored facts, `p` the grammar size, and `d` the number of unary and
binary derivation records. Persistent relation storage, queued facts, and
factorized tracing use `O(n + c + p)` space, in addition to the input and public
outputs. Saturation also holds a deferred proposal batch: if its maximum size
is `b`, peak working storage is `O(n + c + p + b)`. Different productions can
propose the same new fact before insertion, so this batch is not necessarily
deduplicated. Eager provenance requires an additional `O(d)` space. Dense rows
are selected only when
enough words are occupied, so they do not create an unconditional dense table
for every symbol and vertex.

Word-level differences, intersections, and coverage checks reduce redundant
work. They do not guarantee that every backward join is skipped, or eliminate
the worst-case cost of CFL saturation and pivot reconstruction. The saturated
relation and returned all-pairs sets may still be quadratic in the vertex count.

For an input graph with `V` vertices, `E` edges, and `k` bracket types, taint
product trimming uses `O(V + E + k)` scratch space in addition to the retained
product. Its graph traversals take `O(k(V + E))` time in the worst case. If all
state copies are useful, the product still has the full asymptotic size.

Reducing provenance alone cannot reduce memory retained by other stages. This
is why compact relation storage, output lifetimes, and product construction are
separate parts of the implementation rather than assumptions about tracing's
space bound.
