#!/usr/bin/env python3
"""Run fixed-counter/adaptive unary experiments and append durable CSV rows."""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import resource
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from statistics import fmean
from typing import Any, Iterable, Sequence

import config


CSV_COLUMNS = [
    "dataset",
    "dot_name",
    "dot_path",
    "experiment",
    "algorithm",
    "extra_arguments",
    "configured_repetitions",
    "completed_repetitions",
    "successful_repetitions",
    "statuses",
    "exit_codes",
    "time_ms",
    "average_time_ms",
    "peak_rss_bytes",
    "average_peak_rss_bytes",
    "wall_time_seconds",
    "average_wall_time_seconds",
    "components",
    "stats_per_run",
    "log_files",
    "command",
]


@dataclass(frozen=True)
class Benchmark:
  dataset: str
  root: Path
  dot: Path


@dataclass
class RunResult:
  status: str
  exit_code: int | None
  wall_time_seconds: float
  stdout: str
  stderr: str
  stats: dict[str, str]
  log_path: Path
  memory_limit_bytes: int | None


def parse_arguments() -> argparse.Namespace:
  parser = argparse.ArgumentParser(
      description="Run unary Interleaved-Dyck experiments from config.py."
  )
  parser.add_argument(
      "--dry-run",
      action="store_true",
      help="validate the configuration and print commands without running them",
  )
  parser.add_argument(
      "--restart",
      action="store_true",
      help="replace the configured CSV instead of resuming it",
  )
  return parser.parse_args()


def utc_now() -> str:
  return datetime.now(timezone.utc).isoformat()


def json_cell(value: Any) -> str:
  return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def average(values: Iterable[int | float | None]) -> float | None:
  present = [value for value in values if value is not None]
  return fmean(present) if present else None


def parse_integer(value: str | None) -> int | None:
  if value is None:
    return None
  try:
    return int(value)
  except ValueError:
    return None


def configured_memory_limit_bytes() -> int | None:
  limit_gib = config.MEMORY_LIMIT_GIB
  if limit_gib is None:
    return None
  return int(limit_gib * 1024**3)


def is_memory_limit_failure(stderr: str) -> bool:
  lowered = stderr.lower()
  return any(
      marker in lowered
      for marker in ("std::bad_alloc", "cannot allocate memory", "memoryerror")
  )


def parse_program_stats(stdout: str) -> dict[str, str]:
  """Parse all human-readable `key: value` lines without discarding details."""
  result: dict[str, str] = {}
  for line in stdout.splitlines():
    if ":" not in line:
      continue
    key, value = line.split(":", 1)
    key = key.strip()
    if key:
      result[key] = value.strip()
  return result


def safe_name(value: str) -> str:
  cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "_", value)
  return cleaned or "unnamed"


def discover_benchmarks() -> list[Benchmark]:
  roots = [Path(path).expanduser().resolve() for path in config.TESTSET_DIRECTORIES]
  labels = [root.name for root in roots]
  if len(labels) != len(set(labels)):
    raise ValueError(
        "TESTSET_DIRECTORIES must have unique final directory names because "
        "those names identify datasets in the CSV and log tree"
    )

  benchmarks: list[Benchmark] = []
  for root in roots:
    if not root.is_dir():
      raise FileNotFoundError(f"test-set directory does not exist: {root}")
    iterator = (
        root.rglob(config.DOT_GLOB)
        if config.SEARCH_RECURSIVELY
        else root.glob(config.DOT_GLOB)
    )
    for dot in sorted(path.resolve() for path in iterator if path.is_file()):
      benchmarks.append(Benchmark(root.name, root, dot))
  if not benchmarks:
    raise RuntimeError("no DOT files were found in TESTSET_DIRECTORIES")
  return benchmarks


def validate_experiments() -> list[dict[str, Any]]:
  if config.MEASURED_REPETITIONS <= 0:
    raise ValueError("MEASURED_REPETITIONS must be positive")
  if config.WARMUP_REPETITIONS < 0:
    raise ValueError("WARMUP_REPETITIONS cannot be negative")
  if config.TIMEOUT_SECONDS is not None and config.TIMEOUT_SECONDS <= 0:
    raise ValueError("TIMEOUT_SECONDS must be positive or None")
  if config.MEMORY_LIMIT_GIB is not None and (
      isinstance(config.MEMORY_LIMIT_GIB, bool)
      or not isinstance(config.MEMORY_LIMIT_GIB, (int, float))
      or config.MEMORY_LIMIT_GIB <= 0
  ):
    raise ValueError("MEMORY_LIMIT_GIB must be a positive number or None")

  experiments = list(config.EXPERIMENTS)
  names: set[str] = set()
  for experiment in experiments:
    missing = {"name", "algorithm", "args"} - experiment.keys()
    if missing:
      raise ValueError(f"experiment is missing fields {sorted(missing)}: {experiment}")
    if experiment["name"] in names:
      raise ValueError(f"duplicate experiment name: {experiment['name']}")
    names.add(experiment["name"])
    if experiment["algorithm"] not in {"adaptive", "fixed-counter"}:
      raise ValueError(f"unsupported algorithm: {experiment['algorithm']}")
    if not isinstance(experiment["args"], (list, tuple)):
      raise TypeError(f"experiment args must be a list or tuple: {experiment}")
  if not experiments:
    raise ValueError("EXPERIMENTS cannot be empty")
  return experiments


def make_command(
    binary: Path, benchmark: Benchmark, experiment: dict[str, Any]
) -> list[str]:
  return [
      str(binary),
      str(config.ENGINE),
      "--algorithm",
      str(experiment["algorithm"]),
      *map(str, config.COMMON_ARGUMENTS),
      *map(str, experiment["args"]),
      str(benchmark.dot),
  ]


def log_path_for(
    benchmark: Benchmark,
    experiment_name: str,
    repetition: int,
    *,
    warmup: bool,
) -> Path:
  relative = benchmark.dot.relative_to(benchmark.root)
  log_dir = Path(config.LOG_DIRECTORY).expanduser().resolve() / benchmark.dataset
  log_dir /= relative.parent
  suffix = f"warmup-{repetition}" if warmup else str(repetition)
  filename = (
      f"{safe_name(relative.stem)}-{safe_name(experiment_name)}-{suffix}.log"
  )
  return log_dir / filename


def write_log(
    path: Path,
    command_text: str,
    started_at: str,
    finished_at: str,
    result: RunResult,
) -> None:
  path.parent.mkdir(parents=True, exist_ok=True)
  with path.open("w", encoding="utf-8") as stream:
    stream.write(f"Command: {command_text}\n")
    stream.write(f"Started (UTC): {started_at}\n")
    stream.write(f"Finished (UTC): {finished_at}\n")
    stream.write(f"Status: {result.status}\n")
    stream.write(f"Exit code: {result.exit_code}\n")
    stream.write(f"Memory limit (bytes): {result.memory_limit_bytes}\n")
    stream.write(f"Wall time (seconds): {result.wall_time_seconds:.6f}\n")
    stream.write("\n===== STDOUT =====\n")
    stream.write(result.stdout)
    if result.stdout and not result.stdout.endswith("\n"):
      stream.write("\n")
    stream.write("\n===== STDERR =====\n")
    stream.write(result.stderr)
    if result.stderr and not result.stderr.endswith("\n"):
      stream.write("\n")
    stream.flush()
    os.fsync(stream.fileno())


def run_once(command: Sequence[str], log_path: Path) -> RunResult:
  command_text = shlex.join(command)
  environment = os.environ.copy()
  environment.update({str(k): str(v) for k, v in config.EXTRA_ENVIRONMENT.items()})
  started_at = utc_now()
  start = time.perf_counter()
  stdout = ""
  stderr = ""
  exit_code: int | None = None
  status = "spawn-error"
  memory_limit_bytes = configured_memory_limit_bytes()

  def apply_memory_limit() -> None:
    if memory_limit_bytes is not None:
      resource.setrlimit(
          resource.RLIMIT_AS, (memory_limit_bytes, memory_limit_bytes)
      )

  try:
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=environment,
        preexec_fn=apply_memory_limit if memory_limit_bytes is not None else None,
    )
    try:
      stdout, stderr = process.communicate(timeout=config.TIMEOUT_SECONDS)
      exit_code = process.returncode
      if exit_code == 0:
        status = "success"
      elif memory_limit_bytes is not None and is_memory_limit_failure(stderr):
        status = "memory-limit"
      elif exit_code is not None and exit_code < 0:
        status = f"signal-{abs(exit_code)}"
      else:
        status = f"exit-{exit_code}"
    except subprocess.TimeoutExpired:
      process.kill()
      stdout, stderr = process.communicate()
      exit_code = process.returncode
      status = "timeout"
  except (OSError, subprocess.SubprocessError) as error:
    stderr = f"failed to start process: {error}\n"

  elapsed = time.perf_counter() - start
  stats = parse_program_stats(stdout)
  if memory_limit_bytes is not None:
    stats["runner memory limit (bytes)"] = str(memory_limit_bytes)
  result = RunResult(
      status=status,
      exit_code=exit_code,
      wall_time_seconds=elapsed,
      stdout=stdout,
      stderr=stderr,
      stats=stats,
      log_path=log_path,
      memory_limit_bytes=memory_limit_bytes,
  )
  write_log(log_path, command_text, started_at, utc_now(), result)
  return result


def prepare_csv(path: Path, resume: bool) -> set[tuple[str, str, str]]:
  path.parent.mkdir(parents=True, exist_ok=True)
  completed: set[tuple[str, str, str]] = set()

  if resume and path.exists() and path.stat().st_size:
    with path.open("r", encoding="utf-8", newline="") as stream:
      reader = csv.DictReader(stream)
      if reader.fieldnames != CSV_COLUMNS:
        raise RuntimeError(
            f"existing CSV header does not match this runner: {path}; "
            "use --restart or change OUTPUT_CSV"
        )
      for row in reader:
        completed.add((row["dataset"], row["dot_path"], row["experiment"]))
    return completed

  with path.open("w", encoding="utf-8", newline="") as stream:
    writer = csv.DictWriter(stream, fieldnames=CSV_COLUMNS)
    writer.writeheader()
    stream.flush()
    os.fsync(stream.fileno())
  return completed


def append_csv_row(path: Path, row: dict[str, Any]) -> None:
  """Append, flush and fsync one completed project/configuration immediately."""
  with path.open("a", encoding="utf-8", newline="") as stream:
    writer = csv.DictWriter(stream, fieldnames=CSV_COLUMNS)
    writer.writerow(row)
    stream.flush()
    os.fsync(stream.fileno())


def create_csv_row(
    benchmark: Benchmark,
    experiment: dict[str, Any],
    command: Sequence[str],
    results: Sequence[RunResult],
) -> dict[str, Any]:
  times = [parse_integer(result.stats.get("Time (ms)")) for result in results]
  rss_values = [
      parse_integer(result.stats.get("process peak RSS (bytes)"))
      for result in results
  ]
  components = [
      parse_integer(result.stats.get("Components")) for result in results
  ]
  wall_times = [round(result.wall_time_seconds, 6) for result in results]
  relative_dot = benchmark.dot.relative_to(config.REPOSITORY_ROOT)
  log_files = [
      str(result.log_path.relative_to(config.REPOSITORY_ROOT)) for result in results
  ]

  return {
      "dataset": benchmark.dataset,
      "dot_name": benchmark.dot.name,
      "dot_path": str(relative_dot),
      "experiment": experiment["name"],
      "algorithm": experiment["algorithm"],
      "extra_arguments": json_cell(list(experiment["args"])),
      "configured_repetitions": config.MEASURED_REPETITIONS,
      "completed_repetitions": len(results),
      "successful_repetitions": sum(r.status == "success" for r in results),
      "statuses": json_cell([result.status for result in results]),
      "exit_codes": json_cell([result.exit_code for result in results]),
      "time_ms": json_cell(times),
      "average_time_ms": average(times),
      "peak_rss_bytes": json_cell(rss_values),
      "average_peak_rss_bytes": average(rss_values),
      "wall_time_seconds": json_cell(wall_times),
      "average_wall_time_seconds": average(wall_times),
      "components": json_cell(components),
      "stats_per_run": json_cell([result.stats for result in results]),
      "log_files": json_cell(log_files),
      "command": shlex.join(command),
  }


def run_experiment(
    binary: Path,
    benchmark: Benchmark,
    experiment: dict[str, Any],
) -> tuple[list[str], list[RunResult]]:
  command = make_command(binary, benchmark, experiment)
  command_text = shlex.join(command)

  for repetition in range(1, config.WARMUP_REPETITIONS + 1):
    log_path = log_path_for(
        benchmark, experiment["name"], repetition, warmup=True
    )
    print(
        f"[warm-up {repetition}/{config.WARMUP_REPETITIONS}] {command_text}",
        flush=True,
    )
    run_once(command, log_path)

  results: list[RunResult] = []
  for repetition in range(1, config.MEASURED_REPETITIONS + 1):
    log_path = log_path_for(
        benchmark, experiment["name"], repetition, warmup=False
    )
    print(
        f"[run {repetition}/{config.MEASURED_REPETITIONS}] {command_text}",
        flush=True,
    )
    result = run_once(command, log_path)
    results.append(result)
    if result.status == "memory-limit" or (
        result.status != "success" and config.STOP_REPETITIONS_AFTER_FAILURE
    ):
      break
  return command, results


def main() -> int:
  arguments = parse_arguments()
  experiments = validate_experiments()
  benchmarks = discover_benchmarks()
  binary = Path(config.BINARY_PATH).expanduser().resolve()

  if not arguments.dry_run and not binary.is_file():
    raise FileNotFoundError(f"benchmark binary does not exist: {binary}")
  if not arguments.dry_run and not os.access(binary, os.X_OK):
    raise PermissionError(f"benchmark binary is not executable: {binary}")

  if arguments.dry_run:
    print(f"Discovered {len(benchmarks)} DOT files.")
    print(f"Configured {len(experiments)} experiments per DOT file.")
    print(f"Measured repetitions: {config.MEASURED_REPETITIONS}")
    print(f"Memory limit (GiB): {config.MEMORY_LIMIT_GIB}")
    print(f"CSV: {Path(config.OUTPUT_CSV).resolve()}")
    print(f"Logs: {Path(config.LOG_DIRECTORY).resolve()}")
    for benchmark in benchmarks:
      for experiment in experiments:
        print(shlex.join(make_command(binary, benchmark, experiment)))
    return 0

  csv_path = Path(config.OUTPUT_CSV).expanduser().resolve()
  resume = config.RESUME_EXISTING_CSV and not arguments.restart
  completed = prepare_csv(csv_path, resume)
  total = len(benchmarks) * len(experiments)
  finished = 0

  for benchmark in benchmarks:
    for experiment in experiments:
      key = (
          benchmark.dataset,
          str(benchmark.dot.relative_to(config.REPOSITORY_ROOT)),
          experiment["name"],
      )
      finished += 1
      if key in completed:
        print(f"[{finished}/{total}] skip completed {key}", flush=True)
        continue

      print(
          f"[{finished}/{total}] {benchmark.dataset}/{benchmark.dot.name} "
          f"({experiment['name']})",
          flush=True,
      )
      command, results = run_experiment(binary, benchmark, experiment)
      append_csv_row(
          csv_path,
          create_csv_row(benchmark, experiment, command, results),
      )
      print(f"  appended and synced: {csv_path}", flush=True)

  print(f"Finished. Results: {csv_path}")
  return 0


if __name__ == "__main__":
  try:
    sys.exit(main())
  except (OSError, RuntimeError, TypeError, ValueError) as error:
    print(f"run_experiments.py: {error}", file=sys.stderr)
    sys.exit(1)
