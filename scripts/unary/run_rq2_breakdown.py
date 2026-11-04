#!/usr/bin/env python3
"""Run RQ2.2 and write explicit per-phase timing arrays to a separate CSV."""

from __future__ import annotations

import argparse
import csv
import json
import os
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Any, Sequence

import rq2_breakdown_config as config
import run_experiments as base_runner


BASE_COLUMNS = [
    "dataset",
    "dot_name",
    "dot_path",
    "configured_repetitions",
    "completed_repetitions",
    "successful_repetitions",
    "phase_complete_repetitions",
    "statuses",
    "exit_codes",
    "missing_phase_stats",
    "time_ms",
    "average_time_ms",
    "total_time_us",
    "average_total_time_us",
    "peak_rss_bytes",
    "average_peak_rss_bytes",
    "wall_time_seconds",
    "average_wall_time_seconds",
    "components",
    "memory_limit_bytes",
]


def csv_columns() -> list[str]:
  columns = list(BASE_COLUMNS)
  for name, _ in config.PHASE_STAT_KEYS:
    columns.extend((name, f"average_{name}"))
  columns.extend(
      (
          "phase_sum_us",
          "average_phase_sum_us",
          "unaccounted_us",
          "average_unaccounted_us",
          "log_files",
          "command",
      )
  )
  return columns


CSV_COLUMNS = csv_columns()


def parse_arguments() -> argparse.Namespace:
  parser = argparse.ArgumentParser(
      description="Run the instrumented Adaptive phase-breakdown experiment."
  )
  parser.add_argument(
      "--dry-run",
      action="store_true",
      help="print commands without running the instrumented binary",
  )
  parser.add_argument(
      "--restart",
      action="store_true",
      help="replace only rq2-breakdown.csv and rerun every DOT file",
  )
  return parser.parse_args()


def validate_configuration() -> None:
  base_runner.validate_experiments()
  if config.MEASURED_REPETITIONS <= 0:
    raise ValueError("MEASURED_REPETITIONS must be positive")
  names = [name for name, _ in config.PHASE_STAT_KEYS]
  keys = [key for _, key in config.PHASE_STAT_KEYS]
  if len(names) != len(set(names)):
    raise ValueError("PHASE_STAT_KEYS contains duplicate CSV names")
  if len(keys) != len(set(keys)):
    raise ValueError("PHASE_STAT_KEYS contains duplicate output keys")


def discover_benchmarks() -> list[base_runner.Benchmark]:
  roots = [Path(path).expanduser().resolve() for path in config.TESTSET_DIRECTORIES]
  labels = [root.name for root in roots]
  if len(labels) != len(set(labels)):
    raise ValueError("test-set directory names must be unique")

  benchmarks: list[base_runner.Benchmark] = []
  for root in roots:
    if not root.is_dir():
      raise FileNotFoundError(f"test-set directory does not exist: {root}")
    iterator = (
        root.rglob(config.DOT_GLOB)
        if config.SEARCH_RECURSIVELY
        else root.glob(config.DOT_GLOB)
    )
    for dot in sorted(path.resolve() for path in iterator if path.is_file()):
      benchmarks.append(base_runner.Benchmark(root.name, root, dot))
  if not benchmarks:
    raise RuntimeError("no DOT files were found")
  return benchmarks


def make_command(binary: Path, benchmark: base_runner.Benchmark) -> list[str]:
  return [str(binary), *map(str, config.COMMAND_ARGUMENTS), str(benchmark.dot)]


def check_phase_timing_support(binary: Path) -> None:
  completed = subprocess.run(
      [str(binary), str(config.ENGINE), "--help"],
      stdout=subprocess.PIPE,
      stderr=subprocess.PIPE,
      text=True,
      encoding="utf-8",
      errors="replace",
      check=False,
  )
  output = completed.stdout + completed.stderr
  if completed.returncode != 0 or "--phase-timing" not in output:
    raise RuntimeError(
        "the benchmark binary does not support --phase-timing; "
        "implement the C++ timing switch and rebuild the "
        "lotus-cfl-interleaved-dyck target first"
    )


def log_path_for(
    benchmark: base_runner.Benchmark, repetition: int, *, warmup: bool
) -> Path:
  relative = benchmark.dot.relative_to(benchmark.root)
  directory = Path(config.LOG_DIRECTORY).resolve() / benchmark.dataset
  directory /= relative.parent
  suffix = f"warmup-{repetition}" if warmup else str(repetition)
  return directory / f"{base_runner.safe_name(relative.stem)}-{suffix}.log"


def split_stat(stats: dict[str, str], key: str, index: int) -> int | None:
  value = stats.get(key)
  if value is None:
    return None
  parts = value.split("/")
  if index >= len(parts):
    return None
  return base_runner.parse_integer(parts[index])


def phase_value(
    stats: dict[str, str], name: str, output_key: str
) -> int | None:
  direct = base_runner.parse_integer(stats.get(output_key))
  if direct is not None:
    return direct

  fallbacks = {
      "projection_us": ("projection/preprocessing/decomposition (us)", 0),
      "quotient_sparsification_us": (
          "projection/preprocessing/decomposition (us)",
          1,
      ),
      "decomposition_us": ("projection/preprocessing/decomposition (us)", 2),
      "vertical_solving_us": ("vertical/horizontal/merge (us)", 0),
      "horizontal_solving_us": ("vertical/horizontal/merge (us)", 1),
      "output_lifting_us": ("solving/lifting/total (us)", 1),
  }
  fallback = fallbacks.get(name)
  if fallback is None:
    return None
  return split_stat(stats, *fallback)


def total_time_us(stats: dict[str, str]) -> int | None:
  return split_stat(stats, "solving/lifting/total (us)", 2)


def prepare_csv(path: Path, resume: bool) -> set[tuple[str, str]]:
  path.parent.mkdir(parents=True, exist_ok=True)
  completed: set[tuple[str, str]] = set()
  if resume and path.exists() and path.stat().st_size:
    with path.open("r", encoding="utf-8", newline="") as stream:
      reader = csv.DictReader(stream)
      if reader.fieldnames != CSV_COLUMNS:
        raise RuntimeError(
            f"existing RQ2.2 CSV header does not match: {path}; "
            "use --restart or change OUTPUT_CSV"
        )
      for row in reader:
        completed.add((row["dataset"], row["dot_path"]))
    return completed

  with path.open("w", encoding="utf-8", newline="") as stream:
    writer = csv.DictWriter(stream, fieldnames=CSV_COLUMNS)
    writer.writeheader()
    stream.flush()
    os.fsync(stream.fileno())
  return completed


def append_csv_row(path: Path, row: dict[str, Any]) -> None:
  with path.open("a", encoding="utf-8", newline="") as stream:
    csv.DictWriter(stream, fieldnames=CSV_COLUMNS).writerow(row)
    stream.flush()
    os.fsync(stream.fileno())


def create_csv_row(
    benchmark: base_runner.Benchmark,
    command: Sequence[str],
    results: Sequence[base_runner.RunResult],
) -> dict[str, Any]:
  phase_runs: list[dict[str, int | None]] = []
  missing_runs: list[list[str]] = []
  for result in results:
    values = {
        name: phase_value(result.stats, name, key)
        for name, key in config.PHASE_STAT_KEYS
    }
    phase_runs.append(values)
    missing_runs.append([name for name, value in values.items() if value is None])

  times = [base_runner.parse_integer(r.stats.get("Time (ms)")) for r in results]
  totals = [total_time_us(result.stats) for result in results]
  rss_values = [
      base_runner.parse_integer(result.stats.get("process peak RSS (bytes)"))
      for result in results
  ]
  components = [
      base_runner.parse_integer(result.stats.get("Components"))
      for result in results
  ]
  wall_times = [round(result.wall_time_seconds, 6) for result in results]
  phase_sums = [
      sum(value for value in values.values() if value is not None)
      if all(value is not None for value in values.values())
      else None
      for values in phase_runs
  ]
  unaccounted = [
      total - phase_sum
      if total is not None and phase_sum is not None
      else None
      for total, phase_sum in zip(totals, phase_sums)
  ]
  relative_dot = benchmark.dot.relative_to(config.REPOSITORY_ROOT)
  log_files = [
      str(result.log_path.relative_to(config.REPOSITORY_ROOT))
      for result in results
  ]

  row: dict[str, Any] = {
      "dataset": benchmark.dataset,
      "dot_name": benchmark.dot.name,
      "dot_path": str(relative_dot),
      "configured_repetitions": config.MEASURED_REPETITIONS,
      "completed_repetitions": len(results),
      "successful_repetitions": sum(r.status == "success" for r in results),
      "phase_complete_repetitions": sum(not missing for missing in missing_runs),
      "statuses": base_runner.json_cell([r.status for r in results]),
      "exit_codes": base_runner.json_cell([r.exit_code for r in results]),
      "missing_phase_stats": base_runner.json_cell(missing_runs),
      "time_ms": base_runner.json_cell(times),
      "average_time_ms": base_runner.average(times),
      "total_time_us": base_runner.json_cell(totals),
      "average_total_time_us": base_runner.average(totals),
      "peak_rss_bytes": base_runner.json_cell(rss_values),
      "average_peak_rss_bytes": base_runner.average(rss_values),
      "wall_time_seconds": base_runner.json_cell(wall_times),
      "average_wall_time_seconds": base_runner.average(wall_times),
      "components": base_runner.json_cell(components),
      "memory_limit_bytes": base_runner.configured_memory_limit_bytes(),
      "phase_sum_us": base_runner.json_cell(phase_sums),
      "average_phase_sum_us": base_runner.average(phase_sums),
      "unaccounted_us": base_runner.json_cell(unaccounted),
      "average_unaccounted_us": base_runner.average(unaccounted),
      "log_files": base_runner.json_cell(log_files),
      "command": shlex.join(command),
  }
  for name, _ in config.PHASE_STAT_KEYS:
    values = [phase[name] for phase in phase_runs]
    row[name] = base_runner.json_cell(values)
    row[f"average_{name}"] = base_runner.average(values)
  return row


def run_benchmark(
    binary: Path, benchmark: base_runner.Benchmark
) -> tuple[list[str], list[base_runner.RunResult]]:
  command = make_command(binary, benchmark)
  command_text = shlex.join(command)

  for repetition in range(1, config.WARMUP_REPETITIONS + 1):
    path = log_path_for(benchmark, repetition, warmup=True)
    print(
        f"[warm-up {repetition}/{config.WARMUP_REPETITIONS}] {command_text}",
        flush=True,
    )
    base_runner.run_once(command, path)

  results: list[base_runner.RunResult] = []
  for repetition in range(1, config.MEASURED_REPETITIONS + 1):
    path = log_path_for(benchmark, repetition, warmup=False)
    print(
        f"[run {repetition}/{config.MEASURED_REPETITIONS}] {command_text}",
        flush=True,
    )
    result = base_runner.run_once(command, path)
    results.append(result)
    if result.status == "memory-limit" or (
        result.status != "success" and config.STOP_REPETITIONS_AFTER_FAILURE
    ):
      break
  return command, results


def main() -> int:
  arguments = parse_arguments()
  validate_configuration()
  benchmarks = discover_benchmarks()
  binary = Path(config.BINARY_PATH).resolve()
  if not binary.is_file() or not os.access(binary, os.X_OK):
    raise FileNotFoundError(f"benchmark binary is not executable: {binary}")

  if arguments.dry_run:
    print(f"Discovered {len(benchmarks)} DOT files.")
    print(f"Measured repetitions: {config.MEASURED_REPETITIONS}")
    print(f"Memory limit (GiB): {config.MEMORY_LIMIT_GIB}")
    print(f"CSV: {Path(config.OUTPUT_CSV).resolve()}")
    print(f"Logs: {Path(config.LOG_DIRECTORY).resolve()}")
    for benchmark in benchmarks:
      print(shlex.join(make_command(binary, benchmark)))
    return 0

  check_phase_timing_support(binary)
  csv_path = Path(config.OUTPUT_CSV).resolve()
  resume = config.RESUME_EXISTING_CSV and not arguments.restart
  completed = prepare_csv(csv_path, resume)

  for index, benchmark in enumerate(benchmarks, start=1):
    relative_dot = str(benchmark.dot.relative_to(config.REPOSITORY_ROOT))
    key = (benchmark.dataset, relative_dot)
    if key in completed:
      print(f"[{index}/{len(benchmarks)}] skip completed {key}", flush=True)
      continue
    print(
        f"[{index}/{len(benchmarks)}] "
        f"{benchmark.dataset}/{benchmark.dot.name}",
        flush=True,
    )
    command, results = run_benchmark(binary, benchmark)
    append_csv_row(
        csv_path,
        create_csv_row(benchmark, command, results),
    )
    print(f"  appended and synced: {csv_path}", flush=True)

  print(f"Finished. Results: {csv_path}")
  return 0


if __name__ == "__main__":
  try:
    sys.exit(main())
  except (OSError, RuntimeError, TypeError, ValueError) as error:
    print(f"run_rq2_breakdown.py: {error}", file=sys.stderr)
    sys.exit(1)
