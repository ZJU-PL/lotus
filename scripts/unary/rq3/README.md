# RQ3 experiment driver

RQ3 compares Mutual Refinement, SPDS, and ACF on the enabled benchmark suites.
SPDS uses `--analysis nonreflexive-taint --all-pairs` for the taint suite and
`--analysis value-flow --all-pairs` for the value-flow suite. The current
configuration enables 15 taint inputs and three methods (45 result rows); the
value-flow suite is disabled. ACF reuses RQ1's five adaptive measurements; the
other two methods run five measurements each. Every method
runs one separate capture if all measurements succeed. Its taint client intersects the
two balanced projection relations after the configured reverse-edge completion; its
value-flow client additionally constrains each witness to start/end with
opening/closing brackets. Both return nonreflexive upper-bound pairs and
reject prefix acceptance. The taint relation matches the classic CFL
intersection baseline, not necessarily Regularization or Mutual Refinement.
The RQ3 Mutual Refinement command explicitly passes `--plain-mr` so its
measurements continue to use plain MR semantics; standalone
`staged-bounds` commands use the optimized semantics unless that flag is
provided.
Shared binary, dataset, repetition, timeout, and memory settings live in
`../config.py`. Method and output settings live in `config_rq3.py`.

Select suites with `enabled` in `BENCHMARK_SUITES`. Standalone Union-Dyck,
the full on-demand pipeline, and MCFL are
disabled. `--method mutual-refinement` still executes its prerequisite stages,
including the internal lower-bound computation; its total cost includes them.

Run with the requested Python environment:

```sh
bash scripts/unary/build.sh
conda run -n py311 python scripts/unary/rq1/run_rq1.py
conda run -n py311 python scripts/unary/rq3/run_rq3.py --dry-run
conda run -n py311 python scripts/unary/rq3/run_rq3.py
conda run -n py311 python scripts/unary/rq3/aggregate_rq3.py
conda run -n py311 python scripts/unary/plots/plot_rq3_performance_heatmaps.py
```

From this script's directory, `python run_rq3.py` runs all applicable methods.
Use `python run_rq3.py --dataset valueflow --experiment spds --dry-run` to inspect
the SPDS commands. Rebuilding the executable changes its identity: old results
must not be mixed with SPDS results from the new executable. Set a fresh
`DATA_DIRECTORY` and rerun all methods, or explicitly use `--restart` for a full
rerun. A selected SPDS-only run does not produce a complete comparison by itself.

All generated results default to `rq3-data/` beside this script, controlled by
`DATA_DIRECTORY` in `config_rq3.py`. Existing `data/`, `data.bak/`, and
`data.under/` directories are left untouched and are not resumed automatically.

```text
rq3-data/
├── rq3-runs.csv                 # Raw measurements and capture metadata
├── rq3-methods.csv              # Performance summary
├── rq3-bounds.csv               # Precision summary
├── log/                        # Measured and capture run logs
├── artifacts/                  # Upper-bound pairs and component maps
└── rq3-performance-heatmaps.pdf # Performance plot
```

Output directories are created automatically and `rq3-data/` is ignored by Git.
Aggregation and plotting read the configured run CSV by default.

Use `--dataset`, `--benchmark`, `--experiment`, and `--restart` for pilot runs.  The runner
sets `RLIMIT_AS` from `MEMORY_LIMIT_GIB`, kills the complete process group on a
timeout, appends and fsyncs each completed CSV row, and resumes existing rows
by default. With `SKIP_BENCHMARK_AFTER_TIMEOUT` enabled, the first timeout stops
the remaining repetitions and skips the remaining methods for that benchmark;
set it to `False` to run all repetitions after a timeout. A memory-limit failure
always stops the remaining repetitions for that input/method. Other failures
follow `STOP_REPETITIONS_AFTER_FAILURE`. Relation capture still requires every
measured repetition to succeed.

Selected reruns, including `--restart`, preserve CSV rows for unselected
inputs/methods. Without filters, `--restart` reruns all enabled inputs/methods.
If preserved rows use a different executable/environment, the runner stops
before changing the CSV; use a fresh data directory or a complete rerun.
Each row records
SHA-256 hashes of the input and executable, the benchmark analysis, the actual
command, and `EXTRA_ENVIRONMENT`. `engine_analyses` records mappings such as
`taint -> nonreflexive-taint`. Resume rejects a changed identity; aggregation and
plotting reject mixed executable/environment versions or different input/client
identities for the same benchmark. Hashing happens outside measured intervals.
Keep inputs, the executable, and the execution environment fixed during a run.

`RQ1_OUTPUT_CSV` points to RQ1's CSV. Reuse requires complete successful
adaptive samples with matching input, executable, command, environment,
repetitions, warm-ups and resource limits. Source rows and measured logs must
pass checksum validation. Missing or incompatible sources stop the runner
before any measurements or CSV changes; ACF does not fall back to remeasurement.
Reused rows retain RQ1 log paths, the source CSV path and its row checksum.
Keep those files with the RQ3 results. Restart dependent RQ3 rows after rerunning
RQ1; this still only reruns ACF's capture. Resume and aggregation verify log
hashes, and aggregation also checks reused samples against their RQ1 source.

All five measured repetitions omit relation export. Once they succeed, a
separate capture run writes `upper.pairs` for typed methods or `components.map`
for ACF. Typed output goes to a temporary file and is processed line by line,
so the runner does not keep the entire printed relation in memory. SPDS's
`--pairs` stdout is likewise streamed to a temporary file; its sorted unique
pairs and `candidate-pairs` count are checked before creating `upper.pairs`.
Every measured SPDS run must also identify the correct engine/client and
empty-stack acceptance. Invalid metadata is recorded as `stats-error`.
Raw typed output is retained as `upper.txt` when capture validation fails.
Capture
costs are excluded from the performance samples; its command, status, log path,
and solver statistics are recorded separately in the CSV.

`median_wall_time_seconds` covers process launch, input parsing, solving, and
normal summary output. `median_time_ms` is the tool's internal solving time,
which starts after DOT parsing. `median_peak_rss_bytes` uses the process RSS
high-water mark reported by the tool at the end of solving, including parsing;
it is not ACF's construction-payload estimate. The heatmap uses wall time and
RSS. Each median is computed independently across the five measured runs.
For SPDS, the internal total includes PDS preparation, saturation and final
readout. `nonreflexive-taint` retains self reachability during saturation and
includes client pruning, product construction and original-ID mapping.
Its phase-level `*-us` counters are not summed to estimate this total.
The timeout uses the same wall-clock start, including process launch. Killing
and reaping a timed-out process can make its recorded duration exceed the limit
slightly. The 128 GiB limit applies to virtual address space (`RLIMIT_AS`),
not RSS; reported peak RSS can therefore be below the limit on allocation failure.

Successful runs must report valid time and RSS statistics. Failed attempts
retain their wall times and status, with null solver time/RSS samples; incomplete
measurements have blank performance summaries. Capture errors are logged and
prevent a successful precision comparison. The runner exits nonzero if a
selected experiment fails. An unexplained SIGKILL is recorded as a signal,
not automatically labeled memory exhaustion.

The aggregator verifies repetitions, sample/summary consistency, configurations,
and artifact hashes. It
writes `rq3-methods.csv` with performance and relation sizes, and
`rq3-bounds.csv` with one row per typed method and benchmark:

```text
typed_upper_size    = |U|                 (Mutual Refinement or SPDS)
intersection_size  = |U intersect ACF|
newly_excluded_size = |U| - |U intersect ACF|
upper_reduction    = newly_excluded_size / |U|
```

An empty typed upper bound has a blank reduction ratio. Missing or failed
methods produce incomplete comparisons and a nonzero aggregator exit status;
the other typed method can still be compared with ACF independently. No lower
bound or certification ratio is reported, and ACF's unary exactness is not an
exactness claim about the typed relation. ACF's standalone relation size covers
the whole relaxed graph and should not be compared directly with
client-filtered typed relation sizes.

For the paper's precision table, run this from the RQ3 directory:

```sh
python summarize_rq3_precision.py
```

This reads `rq3-bounds.csv` (override with `--bounds PATH`) and prints only
Mutual Refinement and SPDS comparisons. It reports completed and improved input
counts, total removed pairs, macro/micro/maximum reduction percentages, and
every input with additional pruning. Failed comparisons are excluded; empty
upper bounds are excluded from the macro average because their ratio is
undefined. The micro average divides total removed pairs by total candidates.
No files are written and no experiments are rerun.

The demonstration heatmap reads `rq3-runs.csv` directly (override with `--runs PATH`)
and shows the configured methods: Mutual Refinement, SPDS, and ACF. Successful cells
and solved counts require all configured performance repetitions; resource
failures are shown as TO/MO. Relation-capture failure does not erase valid
performance measurements: `rq3-methods.csv` records performance `status` and
`capture_status` separately, with no relation size when capture failed. Precision
comparisons additionally require successful capture. Plotting and aggregation
use the same performance checks.

Run regression tests, including a real-binary measurement/capture/aggregation
smoke test in temporary directories (the experiment data is not touched):

```sh
RQ3_INTEGRATION_TESTS=1 conda run -n py311 python -m unittest scripts/unary/rq3/test_rq3.py
```
