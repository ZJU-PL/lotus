# RQ3 experiment driver

This directory runs the typed lower/upper-bound experiment from paper Section
6.4.  All dataset, solver, repetition, timeout, memory, and output policy lives
in `config.py`.

The default experiment runs the C/C++ value-flow corpus with standalone
Union-Dyck, the final StagedBounds on-demand upper bound, and ACF.  MCFL remains
configured but disabled because it does not yet implement the client-specific
value-flow endpoint model.

Run with the requested Python environment:

```sh
cmake --build build --target lotus-cfl-interleaved-dyck -j2
conda run -n py311 python scripts/unary/rq3/run_rq3.py --dry-run
conda run -n py311 python scripts/unary/rq3/run_rq3.py --restart
conda run -n py311 python scripts/unary/rq3/aggregate_rq3.py
```

Use `--dataset`, `--benchmark`, `--experiment`, and `--restart` for pilot runs.  The runner
sets `RLIMIT_AS` from `MEMORY_LIMIT_GIB`, kills the complete process group on a
timeout, appends and fsyncs each completed CSV row, and resumes existing rows
by default.

Only the first successful measured repetition writes relations.  StagedBounds
writes sorted Union-Dyck and on-demand relations, and ACF writes a linear-size
vertex/component map.  Later repetitions measure the solver without relation
serialization.

The aggregator requires every enabled method to complete all configured
repetitions, verifies captured relation hashes, and computes

```text
L  = Union-Dyck
U0 = on-demand
U1 = on-demand intersect ACF
```

and rejects rows that violate `L subseteq U1 subseteq U0`.  It produces
`rq3-methods.csv` for method cost/size data and `rq3-bounds.csv` for the lower
bound, both upper bounds, unresolved gaps, certification ratios, newly excluded
pairs, and ACF upper/gap reduction.  The raw ACF relation size in the method
table covers the complete relaxed graph; the client-aware ACF result is
`acf_upper_size` in the bounds table.
