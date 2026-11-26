# Experiment scripts

```text
config.py                 # Shared binary, benchmarks, repetitions and limits
provenance.py             # Shared content identity and integrity checks
build.sh                  # Builds build/bin/lotus-cfl-interleaved-dyck
run.sh                    # Runs RQ1, RQ2, RQ3 and the RQ3 aggregator
rq1/
  config_rq1.py
  run_rq1.py
  rq1-data/               # rq1-results.csv and log/
rq2/
  config_rq2.py
  run_rq2.py
  rq2-data/               # rq2-breakdown.csv and log/
rq3/
  config_rq3.py
  run_rq3.py
  aggregate_rq3.py
  summarize_rq3_precision.py
  rq3-data/               # Run/summary CSVs, log/ and artifacts/
```

The runners use the binary built beside these scripts, not the repository-root
build. Paths are derived from file locations, so the experiment directory can
be renamed while remaining directly under `scripts/`. Generated directories are
created when needed.

Common settings are in `config.py`; each `config_rqN.py` imports them and defines
its methods, output paths and failure policy. Keep per-RQ overrides there.

The existing experiment grouping is preserved: `run_rq1.py` runs fixed-counter,
adaptive and adaptive-direct. Its CSV supplies both RQ1 and RQ2's sparsification
ablation. `run_rq2.py` runs the separate phase-breakdown experiment, and its
`ABLATION_CSV` setting points to the RQ1 CSV. RQ3 reuses RQ1's adaptive
performance samples and runs only ACF relation capture. Its other methods retain their measured runs and separate capture.
Run RQ1 before RQ3; `RQ1_OUTPUT_CSV` in `rq3/config_rq3.py` identifies the source.
Reuse requires all repetitions to succeed and matching input/binary hashes,
commands, warm-ups, repetitions, resource limits and `EXTRA_ENVIRONMENT`.
Missing or incompatible source data stops RQ3 before it writes outputs;
there is no automatic duplicate ACF measurement.

From this directory:

```sh
bash build.sh
bash run.sh --dry-run
bash run.sh
```

Each runner also works independently and from any working directory, for example
`python rq2/run_rq2.py --dry-run` or
`python rq3/run_rq3.py --dataset taint --experiment acf --dry-run`.
The wrapper accepts the common `--dry-run` and `--restart` options; use each
runner directly for RQ3-specific filters. It still aggregates completed RQ3
rows after a resource-limit failure and returns a nonzero status on failure.

Old data and backup directories are not moved, rewritten, or loaded into the
new output directories automatically. `--restart` applies to the configured
new CSV paths. The auxiliary audit and plotting scripts are outside this
refactor; their existing report/figure output conventions are unchanged.

Each `average_*` column in the run CSVs is immediately preceded by its
`median_*` column, calculated from the same per-run values. RQ1/RQ2 ignore
missing values for both summaries (all missing produces an empty cell); RQ3
continues to leave summaries empty unless all measured repetitions succeed.
RQ2 also reports medians for every phase, the phase sum and unaccounted time.
Existing CSV files are not rewritten to add or reorder these columns.

New RQ1/RQ2 rows record input, binary and measured-log SHA256 hashes, execution
settings, and a checksum of the CSV row. Resume checks these before skipping
recorded attempts, including failures recorded under the configured stopping
policy. Truncated, corrupted or incompatible records require a fresh output
directory or an explicit `--restart`; old rows are never retroactively stamped
with the current binary's identity. Hashing is outside the timed intervals,
and inputs/binaries are checked before and after each experiment.

RQ3 also checks measured/capture log hashes and retains relation-file hashes.
Reused rows store the source CSV and source-row checksum. Resume and aggregation
check the reused source; rerunning RQ1 can invalidate dependent RQ3 rows, which
then need to be regenerated with `--restart`. Preserve RQ1 CSVs and logs together
with RQ3 data. The checks identify file contents and explicit configuration;
they do not certify an unchanged machine or the entire inherited environment.

After building the binary, run the integrity/reuse tests and RQ3 tests
(including small real-binary checks in temporary directories):

```sh
python -m unittest discover -s . -p 'test_provenance.py'
RQ3_INTEGRATION_TESTS=1 python -m unittest discover -s rq3 -p 'test_rq3.py'
```
