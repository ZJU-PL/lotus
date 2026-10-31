# Classical CFL-Reachability Graphs

This directory vendors the graph and grammar inputs used by the artifact for
“Context-Free Language Reachability via Skewed Tabulation” (PLDI 2024), DOI
`10.1145/3656451`; the archived artifact is available as Zenodo record
`10.5281/zenodo.10892936`. The inputs are general classical CFL-reachability
instances and are not tied to one Lotus solver.

## Layout

```text
Classical/
├── grammars/
│   ├── aa.cfg / aa.ecfg
│   ├── vf.cfg / vf.ecfg
│   └── taint.cfg / taint.ecfg
└── spec2017/
│   ├── pegs/*.g
│   └── svfgs/*.g
```

The three client configurations are:

| Client | Graph | Original grammar | Transformed grammar | Result |
|---|---|---|---|---|
| Alias | `spec2017/pegs/NAME.g` | `aa.cfg` | `aa.ecfg` | `V` |
| Value flow | `spec2017/svfgs/NAME.g` | `vf.cfg` | `vf.ecfg` | `A` |
| Taint | `spec2017/svfgs/NAME.g` | `taint.cfg` | `taint.ecfg` | `S` |

Graph rows are `source target label [attribute]`. Both `call 7` and
`call_i 7` normalize to `call_7`; the same convention applies to field labels.
The grammar files use `Production`, `Insert`, `Follow`, and `Count` sections.

## Direct use

Run one original-grammar instance:

```bash
build/bin/lotus-cfl-solve \
  --grammar benchmarks/real-world/CFL/Classical/grammars/aa.cfg \
  --graph benchmarks/real-world/CFL/Classical/spec2017/pegs/lbm.g \
  --graph-mode plain --solver pocr --result-scope count --json-stats
```

Run its transformed grammar with a target-projecting backend:

```bash
build/bin/lotus-cfl-solve \
  --grammar benchmarks/real-world/CFL/Classical/grammars/aa.ecfg \
  --graph benchmarks/real-world/CFL/Classical/spec2017/pegs/lbm.g \
  --graph-mode plain --solver skewed --result-scope count --json-stats
```

The input graphs already contain the paper's preprocessing. Do not add
`--direction bidirectional` or graph simplification when reproducing the
published instances.

## Batch use

From the repository root, the default configurations compare
`pocr:cfg:count` with `skewed:ecfg:count` for all three analyses:

```bash
python3 scripts/cfl/run_cfl_classical.py --output results.jsonl
```

Configurations are independent triples, so the same inputs can exercise other
engines:

```bash
python3 scripts/cfl/run_cfl_classical.py \
  --configuration sparse-bitvector:cfg:count \
  --configuration sqid:cfg:count \
  --configuration endpoint-quotient:cfg:count
```

For the common case of running several solvers on the same grammar, use
``--solver`` repeatedly or as a comma-separated list. Analyses and benchmarks
support the same two forms:

```bash
# One analysis, two solvers, two programs.
python3 scripts/cfl/run_cfl_classical.py \
  --analysis alias --case lbm,mcf --solver pocr,cert

# Two analyses may also be given as repeated options.
python3 scripts/cfl/run_cfl_classical.py \
  --analysis alias --analysis taint \
  --solver sparse-bitvector --solver endpoint-quotient

# All three analyses, four concurrent processes, one-hour timeout per run.
python3 scripts/cfl/run_cfl_classical.py \
  --analysis alias,value-flow,taint --solver pocr,cert \
  --workers 4 --timeout 3600 --memory-limit-mb 8192 \
  --output results.jsonl
```

``--workers`` defaults to one to avoid accidental memory oversubscription.
``--timeout`` is per solver process; zero disables it. Timed-out and failed
runs are emitted as JSONL records, and the driver returns a non-zero status.
``--memory-limit-mb`` sets a per-run resident-memory limit; zero disables it.
The parent samples Linux ``/proc`` or macOS ``libproc`` and terminates a solver
process group after its RSS crosses the limit. Sampling is periodic, so a run
can briefly overshoot. This is not a global budget: aggregate RSS can still
approach ``workers × memory-limit-mb``.
``--fail-fast``, ``--dry-run``, and ``--validate-only`` are available for
automation and preflight checks. With multiple workers, JSONL records are
written in completion order. Each solver runs in its own process group;
pressing ``Ctrl-C`` terminates every active group, cancels queued work, and
returns exit status 130 instead of leaving background solvers running.

``--resume`` appends to an existing ``--output`` file and skips task keys that
already have a JSONL record. This is useful after interruption or host-level
resource pressure without rerunning completed or timed-out tasks.

Run every generic backend, including ``cert``:

```bash
python3 scripts/cfl/run_cfl_classical.py --all-solvers --case lbm
```

STG is not part of ``--all-solvers`` because it additionally requires a
problem-specific ``--stg-spec`` decomposition.

`omnetpp` has more than 100,000 call-site attributes. The batch script raises
the grammar-expansion limit to one million for the complete suite.

## LLVM frontend interoperability

Lotus can export new instances in the same normalized interchange layer:

```bash
build/bin/lotus-cfl-alias --encoding cfl-peg \
  --dump-cfl-graph out.peg --dump-cfl-grammar out.grammar input.bc

build/bin/lotus-cfl-vf --encoding classical-cfl \
  --dump-cfl-graph out.vfg --dump-cfl-grammar out.grammar input.bc
```

These exports match the graph-label and grammar semantics, but graph identity
is frontend-dependent. In particular, the Lotus SVFG uses AserPTA results, so
its nodes and memory-flow edges need not be identical to graphs generated by a
specific SVF release.
