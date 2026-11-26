#!/usr/bin/env bash
set -u
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
status=0

# Still aggregate completed rows when a method hits a resource limit.
python "$script_dir/rq1/run_rq1.py" "$@" || status=1
python "$script_dir/rq2/run_rq2.py" "$@" || status=1
python "$script_dir/rq3/run_rq3.py" "$@" || status=1
if [[ " $* " != *" --dry-run "* ]]; then
  python "$script_dir/rq3/aggregate_rq3.py" || status=1
fi
exit "$status"
