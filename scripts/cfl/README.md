# Scripts for CFL Reachability Evaluation

`evaluate_staged_bounds.py` compares two staged-bounds executables on medium and
hard interleaved-Dyck inputs. It runs fresh processes sequentially, records
whole-process peak RSS and solver time, and hashes sorted output pairs to check
that completed runs agree. `--include-eager` also evaluates the candidate's eager
mode, separating the benefits of the shared saturation engine from tracing.

```sh
python3 scripts/cfl/evaluate_staged_bounds.py \
  --baseline /path/to/old/lotus-cfl-interleaved-dyck \
  --candidate build/bin/lotus-cfl-interleaved-dyck \
  --benchmarks imagick x264 cactus omnetpp scipiex \
  --timeout 120 --repeats 3 --include-eager --output /tmp/staged-results.json
```

Write generated measurements outside the repository, for example under `/tmp`
as shown above.

Requires macOS `/usr/bin/time -l` or Linux GNU `/usr/bin/time -v`, and `sort`.
Stage RSS values are cumulative high-water marks, not resettable stage peaks.
Timeouts are reported separately and are not evidence of pair-set equivalence.

Use `--method stronger-grammar` or `--method on-demand` to check the two stronger
refinement variants with the same driver. Each run still includes its prerequisite
stages; use the candidate's stage reports to distinguish total pipeline cost from
the cost of the last stage.
