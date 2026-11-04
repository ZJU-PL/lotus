#!/usr/bin/env python3
"""Run RQ3 configurations with durable logs and relation capture."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import resource
import shlex
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from statistics import fmean, median
from typing import Any, Iterable, Sequence

import config


CSV_COLUMNS = [
    "dataset",
    "analysis",
    "dot_name",
    "dot_path",
    "experiment",
    "tool",
    "configuration",
    "warmup_repetitions",
    "configured_repetitions",
    "completed_repetitions",
    "successful_repetitions",
    "statuses",
    "exit_codes",
    "time_ms",
    "average_time_ms",
    "median_time_ms",
    "peak_rss_bytes",
    "average_peak_rss_bytes",
    "median_peak_rss_bytes",
    "wall_time_seconds",
    "average_wall_time_seconds",
    "median_wall_time_seconds",
    "timeout_seconds",
    "memory_limit_bytes",
    "stats_per_run",
    "relation_files",
    "relation_sha256",
    "log_files",
    "command",
    "capture_command",
]


@dataclass(frozen=True)
class Benchmark:
  dataset: str
  analysis: str
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


def parse_arguments() -> argparse.Namespace:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--dry-run", action="store_true")
  parser.add_argument("--restart", action="store_true")
  parser.add_argument("--dataset", action="append", help="run only this suite")
  parser.add_argument(
      "--benchmark",
      action="append",
      help="run only this DOT filename or stem (repeatable)",
  )
  parser.add_argument(
      "--experiment", action="append", help="run only this experiment"
  )
  return parser.parse_args()


def json_cell(value: Any) -> str:
  return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def utc_now() -> str:
  return datetime.now(timezone.utc).isoformat()


def safe_name(value: str) -> str:
  return re.sub(r"[^A-Za-z0-9_.-]+", "_", value) or "unnamed"


def present(values: Iterable[int | float | None]) -> list[int | float]:
  return [value for value in values if value is not None]


def average(values: Iterable[int | float | None]) -> float | None:
  items = present(values)
  return fmean(items) if items else None


def median_value(values: Iterable[int | float | None]) -> float | None:
  items = present(values)
  return median(items) if items else None


def parse_integer(value: str | None) -> int | None:
  try:
    return int(value) if value is not None else None
  except ValueError:
    return None


def memory_limit_bytes() -> int | None:
  if config.MEMORY_LIMIT_GIB is None:
    return None
  return int(config.MEMORY_LIMIT_GIB * 1024**3)


def validate_configuration() -> list[dict[str, Any]]:
  if config.WARMUP_REPETITIONS < 0 or config.MEASURED_REPETITIONS <= 0:
    raise ValueError("repetition counts must be nonnegative/positive")
  if config.TIMEOUT_SECONDS is not None and config.TIMEOUT_SECONDS <= 0:
    raise ValueError("TIMEOUT_SECONDS must be positive or None")
  if config.MEMORY_LIMIT_GIB is not None and (
      isinstance(config.MEMORY_LIMIT_GIB, bool)
      or not isinstance(config.MEMORY_LIMIT_GIB, (int, float))
      or config.MEMORY_LIMIT_GIB <= 0
  ):
    raise ValueError("MEMORY_LIMIT_GIB must be positive or None")
  if not config.CAPTURE_RELATIONS:
    raise ValueError("CAPTURE_RELATIONS must be enabled for RQ3 aggregation")

  experiments = [item for item in config.EXPERIMENTS if item.get("enabled", True)]
  names: set[str] = set()
  for experiment in experiments:
    missing = {"name", "tool", "args", "artifact_kind"} - experiment.keys()
    if missing:
      raise ValueError(f"experiment missing {sorted(missing)}: {experiment}")
    if experiment["name"] in names:
      raise ValueError(f"duplicate experiment: {experiment['name']}")
    names.add(experiment["name"])
    if experiment["tool"] not in config.BINARIES:
      raise ValueError(f"unknown tool: {experiment['tool']}")
    if experiment["artifact_kind"] == "mcfl":
      if experiment.get("family") not in {"plus", "circ"}:
        raise ValueError(f"invalid MCFL family: {experiment}")
      if not isinstance(experiment.get("dimension"), int):
        raise ValueError(f"missing MCFL dimension: {experiment}")
  if not experiments:
    raise ValueError("no enabled experiments")
  return experiments


def discover_benchmarks(selected: set[str]) -> list[Benchmark]:
  benchmarks: list[Benchmark] = []
  suite_names: set[str] = set()
  for suite in config.BENCHMARK_SUITES:
    name = str(suite["name"])
    if name in suite_names:
      raise ValueError(f"duplicate benchmark suite: {name}")
    suite_names.add(name)
    if not suite.get("enabled", True) or (selected and name not in selected):
      continue
    root = Path(suite["directory"]).resolve()
    if not root.is_dir():
      raise FileNotFoundError(f"benchmark directory does not exist: {root}")
    iterator = (
        root.rglob(config.DOT_GLOB)
        if config.SEARCH_RECURSIVELY
        else root.glob(config.DOT_GLOB)
    )
    for dot in sorted(path.resolve() for path in iterator if path.is_file()):
      benchmarks.append(Benchmark(name, str(suite["analysis"]), root, dot))
  if selected - suite_names:
    raise ValueError(f"unknown datasets: {sorted(selected - suite_names)}")
  if not benchmarks:
    raise RuntimeError("no benchmark DOT files selected")
  return benchmarks


def substitute_args(arguments: Sequence[Any], benchmark: Benchmark) -> list[str]:
  return [str(value).replace("{analysis}", benchmark.analysis) for value in arguments]


def binary_for(experiment: dict[str, Any]) -> Path:
  return Path(config.BINARIES[experiment["tool"]]).resolve()


def artifact_base(benchmark: Benchmark, experiment: dict[str, Any]) -> Path:
  relative = benchmark.dot.relative_to(benchmark.root).with_suffix("")
  return (
      Path(config.ARTIFACT_DIRECTORY).resolve()
      / benchmark.dataset
      / relative
      / experiment["name"]
  )


def expected_artifacts(
    benchmark: Benchmark, experiment: dict[str, Any]
) -> list[Path]:
  base = artifact_base(benchmark, experiment)
  kind = experiment["artifact_kind"]
  if kind == "staged":
    return [base / "union.pairs", base / "on-demand.pairs"]
  if kind == "union":
    return [base / "union.pairs"]
  if kind == "components":
    return [base / "components.map"]
  family = experiment["family"]
  dimension = int(experiment["dimension"])
  return [base / f"g-{family}-{value}.pairs" for value in range(1, dimension + 1)]


def make_command(
    benchmark: Benchmark, experiment: dict[str, Any], *, capture: bool
) -> list[str]:
  command = [
      str(binary_for(experiment)),
      *substitute_args(experiment["args"], benchmark),
  ]
  if capture and config.CAPTURE_RELATIONS:
    base = artifact_base(benchmark, experiment)
    if experiment["artifact_kind"] in {"staged", "union", "mcfl"}:
      command.extend(("--dump-relations", str(base)))
    else:
      command.extend(("--dump-components", str(base / "components.map")))
  command.append(str(benchmark.dot))
  return command


def log_path_for(
    benchmark: Benchmark,
    experiment: dict[str, Any],
    repetition: int,
    *,
    warmup: bool,
) -> Path:
  relative = benchmark.dot.relative_to(benchmark.root).with_suffix("")
  suffix = f"warmup-{repetition}" if warmup else str(repetition)
  return (
      Path(config.LOG_DIRECTORY).resolve()
      / benchmark.dataset
      / relative
      / f"{safe_name(experiment['name'])}-{suffix}.log"
  )


def parse_stats(stdout: str) -> dict[str, str]:
  stats: dict[str, str] = {}
  for line in stdout.splitlines():
    if ":" in line:
      key, value = line.split(":", 1)
      if key.strip():
        stats[key.strip()] = value.strip()
  return stats


def is_memory_failure(stderr: str, exit_code: int | None) -> bool:
  lowered = stderr.lower()
  markers = ("std::bad_alloc", "cannot allocate memory", "memoryerror")
  return any(marker in lowered for marker in markers) or (
      memory_limit_bytes() is not None and exit_code == -signal.SIGKILL
  )


def write_log(
    path: Path,
    command: Sequence[str],
    started: str,
    result: RunResult,
) -> None:
  path.parent.mkdir(parents=True, exist_ok=True)
  with path.open("w", encoding="utf-8") as stream:
    stream.write(f"Command: {shlex.join(command)}\n")
    stream.write(f"Started (UTC): {started}\n")
    stream.write(f"Finished (UTC): {utc_now()}\n")
    stream.write(f"Status: {result.status}\n")
    stream.write(f"Exit code: {result.exit_code}\n")
    stream.write(f"Memory limit (bytes): {memory_limit_bytes()}\n")
    stream.write(f"Wall time (seconds): {result.wall_time_seconds:.6f}\n")
    stream.write("\n===== STDOUT =====\n")
    stream.write(result.stdout)
    stream.write("\n===== STDERR =====\n")
    stream.write(result.stderr)
    stream.flush()
    os.fsync(stream.fileno())


def run_once(command: Sequence[str], log_path: Path) -> RunResult:
  limit = memory_limit_bytes()

  def apply_limits() -> None:
    if limit is not None:
      resource.setrlimit(resource.RLIMIT_AS, (limit, limit))

  environment = os.environ.copy()
  environment.update({str(k): str(v) for k, v in config.EXTRA_ENVIRONMENT.items()})
  started = utc_now()
  start = time.perf_counter()
  stdout = ""
  stderr = ""
  exit_code: int | None = None
  status = "spawn-error"
  try:
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=environment,
        preexec_fn=apply_limits if limit is not None else None,
        start_new_session=True,
    )
    try:
      stdout, stderr = process.communicate(timeout=config.TIMEOUT_SECONDS)
      exit_code = process.returncode
      if exit_code == 0:
        status = "success"
      elif is_memory_failure(stderr, exit_code):
        status = "memory-limit"
      elif exit_code is not None and exit_code < 0:
        status = f"signal-{abs(exit_code)}"
      else:
        status = f"exit-{exit_code}"
    except subprocess.TimeoutExpired:
      os.killpg(process.pid, signal.SIGKILL)
      stdout, stderr = process.communicate()
      exit_code = process.returncode
      status = "timeout"
  except (OSError, subprocess.SubprocessError) as error:
    stderr = f"failed to start process: {error}\n"

  result = RunResult(
      status=status,
      exit_code=exit_code,
      wall_time_seconds=time.perf_counter() - start,
      stdout=stdout,
      stderr=stderr,
      stats=parse_stats(stdout),
      log_path=log_path,
  )
  write_log(log_path, command, started, result)
  return result


def sha256(path: Path) -> str:
  digest = hashlib.sha256()
  with path.open("rb") as stream:
    for block in iter(lambda: stream.read(1024 * 1024), b""):
      digest.update(block)
  return digest.hexdigest()


def stored_path(value: str) -> Path:
  path = Path(value)
  return path if path.is_absolute() else Path(config.REPOSITORY_ROOT) / path


def completed_csv_row(row: dict[str, str]) -> bool:
  """Return whether a persisted row is complete and safe to resume past."""
  try:
    configured = int(row["configured_repetitions"])
    completed = int(row["completed_repetitions"])
    successful = int(row["successful_repetitions"])
    statuses = json.loads(row["statuses"])
    relation_files = json.loads(row["relation_files"])
    relation_hashes = json.loads(row["relation_sha256"])
    stored_configuration = json.loads(row["configuration"])
  except (KeyError, TypeError, ValueError, json.JSONDecodeError):
    return False
  experiment = next(
      (
          item
          for item in config.EXPERIMENTS
          if item.get("enabled", True) and item["name"] == row.get("experiment")
      ),
      None,
  )
  expected_configuration = (
      {key: value for key, value in experiment.items() if key != "enabled"}
      if experiment is not None
      else None
  )
  if (
      configured <= 0
      or row.get("warmup_repetitions") != str(config.WARMUP_REPETITIONS)
      or configured != config.MEASURED_REPETITIONS
      or completed != configured
      or successful != configured
      or statuses != ["success"] * configured
      or not relation_files
      or set(relation_hashes) != set(relation_files)
      or stored_configuration != expected_configuration
      or row.get("timeout_seconds")
      != ("" if config.TIMEOUT_SECONDS is None else str(config.TIMEOUT_SECONDS))
      or row.get("memory_limit_bytes")
      != ("" if memory_limit_bytes() is None else str(memory_limit_bytes()))
  ):
    return False
  for value in relation_files:
    path = stored_path(value)
    if not path.is_file() or sha256(path) != relation_hashes[value]:
      return False
  return True


def terminal_optional_exclusion(row: dict[str, str]) -> bool:
  """Return whether an optional method was attempted and hit a resource limit."""
  experiments = {
      str(item["name"]): item
      for item in config.EXPERIMENTS
      if item.get("enabled", True)
  }
  experiment = experiments.get(row.get("experiment", ""))
  if experiment is None or experiment.get("required", True):
    return False
  try:
    statuses = json.loads(row["statuses"])
    successes = int(row["successful_repetitions"] or 0)
    configured = int(row["configured_repetitions"] or 0)
    stored_configuration = json.loads(row["configuration"])
  except (KeyError, TypeError, ValueError, json.JSONDecodeError):
    return False
  expected_configuration = {
      key: value for key, value in experiment.items() if key != "enabled"
  }
  return (
      configured == config.MEASURED_REPETITIONS
      and row.get("warmup_repetitions") == str(config.WARMUP_REPETITIONS)
      and successes == 0
      and bool(statuses)
      and statuses[0] in {"timeout", "memory-limit"}
      and stored_configuration == expected_configuration
      and row.get("timeout_seconds")
      == ("" if config.TIMEOUT_SECONDS is None else str(config.TIMEOUT_SECONDS))
      and row.get("memory_limit_bytes")
      == ("" if memory_limit_bytes() is None else str(memory_limit_bytes()))
  )


def remove_stale_artifacts(paths: Sequence[Path]) -> None:
  for path in paths:
    if path.is_file():
      path.unlink()


def relative_paths(paths: Sequence[Path]) -> list[str]:
  result: list[str] = []
  repository = Path(config.REPOSITORY_ROOT).resolve()
  for path in paths:
    resolved = path.resolve()
    try:
      result.append(str(resolved.relative_to(repository)))
    except ValueError:
      result.append(str(resolved))
  return result


def prepare_csv(path: Path, resume: bool) -> set[tuple[str, str, str]]:
  path.parent.mkdir(parents=True, exist_ok=True)
  completed: set[tuple[str, str, str]] = set()
  if resume and path.exists() and path.stat().st_size:
    with path.open("r", encoding="utf-8", newline="") as stream:
      reader = csv.DictReader(stream)
      if reader.fieldnames != CSV_COLUMNS:
        raise RuntimeError(f"CSV header mismatch: {path}; use --restart")
      rows = list(reader)
    retained: list[dict[str, str]] = []
    seen: set[tuple[str, str, str]] = set()
    for row in rows:
      key = (row["dataset"], row["dot_path"], row["experiment"])
      if key in seen:
        raise RuntimeError(f"duplicate CSV row for {key}; use --restart")
      seen.add(key)
      if completed_csv_row(row) or terminal_optional_exclusion(row):
        retained.append(row)
        completed.add(key)
    if len(retained) != len(rows):
      temporary_name: str | None = None
      try:
        with tempfile.NamedTemporaryFile(
            "w",
            encoding="utf-8",
            newline="",
            dir=path.parent,
            prefix=f".{path.name}.",
            delete=False,
        ) as stream:
          temporary_name = stream.name
          writer = csv.DictWriter(stream, fieldnames=CSV_COLUMNS)
          writer.writeheader()
          writer.writerows(retained)
          stream.flush()
          os.fsync(stream.fileno())
        os.replace(temporary_name, path)
        temporary_name = None
        print(
            f"Retrying {len(rows) - len(retained)} incomplete or stale rows",
            flush=True,
        )
      finally:
        if temporary_name is not None:
          try:
            Path(temporary_name).unlink()
          except FileNotFoundError:
            pass
    return completed
  with path.open("w", encoding="utf-8", newline="") as stream:
    csv.DictWriter(stream, fieldnames=CSV_COLUMNS).writeheader()
    stream.flush()
    os.fsync(stream.fileno())
  return completed


def append_row(path: Path, row: dict[str, Any]) -> None:
  with path.open("a", encoding="utf-8", newline="") as stream:
    csv.DictWriter(stream, fieldnames=CSV_COLUMNS).writerow(row)
    stream.flush()
    os.fsync(stream.fileno())


def run_experiment(
    benchmark: Benchmark, experiment: dict[str, Any]
) -> tuple[list[RunResult], list[Path], list[str]]:
  for repetition in range(1, config.WARMUP_REPETITIONS + 1):
    command = make_command(benchmark, experiment, capture=False)
    print(f"  [warm-up {repetition}] {shlex.join(command)}", flush=True)
    run_once(
        command,
        log_path_for(benchmark, experiment, repetition, warmup=True),
    )

  artifacts = expected_artifacts(benchmark, experiment)
  remove_stale_artifacts(artifacts)
  captured: list[Path] = []
  capture_commands: list[str] = []
  results: list[RunResult] = []
  for repetition in range(1, config.MEASURED_REPETITIONS + 1):
    capture = config.CAPTURE_RELATIONS and not captured
    if capture:
      artifact_base(benchmark, experiment).mkdir(parents=True, exist_ok=True)
    command = make_command(benchmark, experiment, capture=capture)
    if capture:
      capture_commands.append(shlex.join(command))
    print(f"  [run {repetition}] {shlex.join(command)}", flush=True)
    result = run_once(
        command,
        log_path_for(benchmark, experiment, repetition, warmup=False),
    )
    if capture and result.status == "success":
      missing = [path for path in artifacts if not path.is_file()]
      if missing:
        result.status = "artifact-error"
        result.stderr += "missing artifacts: " + ", ".join(map(str, missing))
      else:
        captured = artifacts
    results.append(result)
    if result.status != "success" and config.STOP_REPETITIONS_AFTER_FAILURE:
      break
  return results, captured, capture_commands


def create_row(
    benchmark: Benchmark,
    experiment: dict[str, Any],
    results: Sequence[RunResult],
    artifacts: Sequence[Path],
    capture_commands: Sequence[str],
) -> dict[str, Any]:
  successful_results = [result for result in results if result.status == "success"]
  times = [
      parse_integer(result.stats.get("Time (ms)"))
      for result in successful_results
  ]
  rss = [
      parse_integer(result.stats.get("process peak RSS (bytes)"))
      for result in successful_results
  ]
  walls = [round(result.wall_time_seconds, 6) for result in successful_results]
  relative_dot = str(benchmark.dot.relative_to(config.REPOSITORY_ROOT))
  logs = relative_paths([result.log_path for result in results])
  hashes = {
      stored: sha256(path)
      for stored, path in zip(relative_paths(artifacts), artifacts)
  }
  base_command = make_command(benchmark, experiment, capture=False)
  configuration = {
      key: value
      for key, value in experiment.items()
      if key not in {"enabled"}
  }
  return {
      "dataset": benchmark.dataset,
      "analysis": benchmark.analysis,
      "dot_name": benchmark.dot.name,
      "dot_path": relative_dot,
      "experiment": experiment["name"],
      "tool": experiment["tool"],
      "configuration": json_cell(configuration),
      "warmup_repetitions": config.WARMUP_REPETITIONS,
      "configured_repetitions": config.MEASURED_REPETITIONS,
      "completed_repetitions": len(results),
      "successful_repetitions": sum(result.status == "success" for result in results),
      "statuses": json_cell([result.status for result in results]),
      "exit_codes": json_cell([result.exit_code for result in results]),
      "time_ms": json_cell(times),
      "average_time_ms": average(times),
      "median_time_ms": median_value(times),
      "peak_rss_bytes": json_cell(rss),
      "average_peak_rss_bytes": average(rss),
      "median_peak_rss_bytes": median_value(rss),
      "wall_time_seconds": json_cell(walls),
      "average_wall_time_seconds": average(walls),
      "median_wall_time_seconds": median_value(walls),
      "timeout_seconds": config.TIMEOUT_SECONDS,
      "memory_limit_bytes": memory_limit_bytes(),
      "stats_per_run": json_cell([result.stats for result in results]),
      "relation_files": json_cell(relative_paths(artifacts)),
      "relation_sha256": json_cell(hashes),
      "log_files": json_cell(logs),
      "command": shlex.join(base_command),
      "capture_command": json_cell(list(capture_commands)),
  }


def main() -> int:
  arguments = parse_arguments()
  experiments = validate_configuration()
  selected_experiments = set(arguments.experiment or [])
  known_experiments = {item["name"] for item in experiments}
  if selected_experiments - known_experiments:
    raise ValueError(
        f"unknown or disabled experiments: {sorted(selected_experiments - known_experiments)}"
    )
  if selected_experiments:
    experiments = [item for item in experiments if item["name"] in selected_experiments]
  benchmarks = discover_benchmarks(set(arguments.dataset or []))
  selected_benchmarks = set(arguments.benchmark or [])
  if selected_benchmarks:
    matched = {
        value
        for value in selected_benchmarks
        if any(value in {item.dot.name, item.dot.stem} for item in benchmarks)
    }
    if matched != selected_benchmarks:
      raise ValueError(
          f"unknown benchmarks: {sorted(selected_benchmarks - matched)}"
      )
    benchmarks = [
        item
        for item in benchmarks
        if item.dot.name in selected_benchmarks
        or item.dot.stem in selected_benchmarks
    ]

  for experiment in experiments:
    binary = binary_for(experiment)
    if not arguments.dry_run and (not binary.is_file() or not os.access(binary, os.X_OK)):
      raise FileNotFoundError(f"binary is not executable: {binary}")

  if arguments.dry_run:
    print(f"Benchmarks: {len(benchmarks)}")
    print(f"Experiments per benchmark: {len(experiments)}")
    print(f"Warm-ups/measured: {config.WARMUP_REPETITIONS}/{config.MEASURED_REPETITIONS}")
    print(f"Timeout (s): {config.TIMEOUT_SECONDS}")
    print(f"Memory limit (GiB): {config.MEMORY_LIMIT_GIB}")
    for benchmark in benchmarks:
      for experiment in experiments:
        print(shlex.join(make_command(benchmark, experiment, capture=True)))
    return 0

  csv_path = Path(config.OUTPUT_CSV).resolve()
  completed = prepare_csv(
      csv_path, config.RESUME_EXISTING_CSV and not arguments.restart
  )
  total = len(benchmarks) * len(experiments)
  current = 0
  for benchmark in benchmarks:
    relative_dot = str(benchmark.dot.relative_to(config.REPOSITORY_ROOT))
    for experiment in experiments:
      current += 1
      key = (benchmark.dataset, relative_dot, experiment["name"])
      if key in completed:
        print(f"[{current}/{total}] skip {key}", flush=True)
        continue
      print(
          f"[{current}/{total}] {benchmark.dataset}/{benchmark.dot.name} "
          f"{experiment['name']}",
          flush=True,
      )
      results, artifacts, capture_commands = run_experiment(benchmark, experiment)
      append_row(
          csv_path,
          create_row(benchmark, experiment, results, artifacts, capture_commands),
      )
      print(f"  appended and synced: {csv_path}", flush=True)
  print(f"Finished. Results: {csv_path}")
  return 0


if __name__ == "__main__":
  try:
    sys.exit(main())
  except (OSError, RuntimeError, TypeError, ValueError) as error:
    print(f"run_rq3.py: {error}", file=sys.stderr)
    sys.exit(1)
