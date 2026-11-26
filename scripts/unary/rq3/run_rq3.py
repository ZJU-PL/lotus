#!/usr/bin/env python3
"""Run RQ3 configurations with durable logs and relation capture."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
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

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from rq3 import config_rq3 as config
import provenance


CSV_COLUMNS = [
    "dataset",
    "analysis",
    "dot_name",
    "dot_path",
    "input_sha256",
    "binary_sha256",
    "extra_environment",
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
    "median_time_ms",
    "average_time_ms",
    "peak_rss_bytes",
    "median_peak_rss_bytes",
    "average_peak_rss_bytes",
    "wall_time_seconds",
    "median_wall_time_seconds",
    "average_wall_time_seconds",
    "timeout_seconds",
    "memory_limit_bytes",
    "stats_per_run",
    "relation_files",
    "relation_sha256",
    "log_files",
    "command",
    "capture_command",
    "capture_status",
    "capture_log_file",
    "capture_stats",
    "log_sha256",
    "capture_log_sha256",
    "performance_source",
    "source_row_sha256",
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
  started: str = ""


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
    engine_analyses = experiment.get("engine_analyses", {})
    if not isinstance(engine_analyses, dict) or any(
        not isinstance(key, str) or not isinstance(value, str)
        for key, value in engine_analyses.items()
    ):
      raise ValueError(f"invalid engine analysis mapping: {experiment}")
    supported = set(experiment.get("analyses", ["taint", "value-flow"]))
    unknown = set(engine_analyses) - supported
    if unknown:
      raise ValueError(
          f"engine analysis mapping has unsupported datasets: {sorted(unknown)}"
      )
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


def engine_analysis(experiment: dict[str, Any], analysis: str) -> str:
  """Translate a dataset analysis name to the selected tool's client name."""
  mapping = experiment.get("engine_analyses", {})
  return str(mapping.get(analysis, analysis))


def substitute_args(arguments: Sequence[Any], analysis: str) -> list[str]:
  return [str(value).replace("{analysis}", analysis) for value in arguments]


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
  if kind == "upper":
    return [base / "upper.pairs"]
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
      *substitute_args(
          experiment["args"], engine_analysis(experiment, benchmark.analysis)
      ),
  ]
  if capture and config.CAPTURE_RELATIONS:
    base = artifact_base(benchmark, experiment)
    if experiment["tool"] == "spds":
      command.append("--pairs")
    elif experiment["artifact_kind"] == "upper":
      command.extend(("--print-result", "-o", str(base / "upper.txt")))
    elif experiment["artifact_kind"] in {"staged", "union", "mcfl"}:
      command.extend(("--dump-relations", str(base)))
    else:
      command.extend(("--dump-components", str(base / "components.map")))
  command.append(str(benchmark.dot))
  return command


def log_path_for(
    benchmark: Benchmark,
    experiment: dict[str, Any],
    repetition: int | str,
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


def capture_upper_pairs(lines: Iterable[str], path: Path) -> dict[str, str]:
  """Extract --print-result pairs, checking the tool's reported count."""
  lines = iter(lines)
  header = next(lines, "").strip()
  label, separator, count = header.partition(":")
  if not separator or label not in {
      "regularization", "mutual-refinement upper bound"
  }:
    raise ValueError(f"unexpected upper-bound header: {header}")
  stats = {label: count.strip()}
  pair_count = 0
  with path.open("w", encoding="utf-8") as stream:
    for line in lines:
      line = line.strip()
      if line.startswith(("Time (ms):", "process peak RSS (bytes):")):
        stats.update(parse_stats(line))
        continue
      if not line:
        continue
      if re.fullmatch(r"-?\d+\s+-?\d+", line) is None:
        raise ValueError(f"invalid pair output: {line}")
      stream.write(line + "\n")
      pair_count += 1
  if pair_count != int(count):
    raise ValueError(f"expected {count.strip()} pairs, captured {pair_count}")
  return stats


def valid_stats(stats: dict[str, str]) -> bool:
  elapsed = parse_integer(stats.get("Time (ms)"))
  rss = parse_integer(stats.get("process peak RSS (bytes)"))
  return elapsed is not None and elapsed >= 0 and rss is not None and rss > 0


def valid_spds_stats(stats: dict[str, str], analysis: str) -> bool:
  count = parse_integer(stats.get("candidate-pairs"))
  return (
      analysis in {"nonreflexive-taint", "value-flow"}
      and stats.get("engine") == "spds"
      and stats.get("analysis") == analysis
      and stats.get("call-stack") == stats.get("field-stack") == "empty"
      and count is not None and count >= 0
  )


def capture_spds_pairs(lines: Iterable[str], path: Path, analysis: str) -> dict[str, str]:
  """Stream SPDS's nonreflexive sorted pairs and verify the reported size."""
  stats = {}
  count = 0
  previous = None
  with path.open("w", encoding="utf-8") as output:
    for line in lines:
      if line.startswith("pair:"):
        fields = line.removeprefix("pair:").split()
        if len(fields) != 2:
          raise ValueError(f"invalid SPDS pair: {line.strip()}")
        pair = tuple(map(int, fields))
        if pair[0] == pair[1] or (previous is not None and pair <= previous):
          raise ValueError("SPDS pairs must be nonreflexive, sorted and unique")
        output.write(f"{pair[0]} {pair[1]}\n")
        previous = pair
        count += 1
      else:
        stats.update(parse_stats(line))
  if (not valid_spds_stats(stats, analysis)
      or parse_integer(stats.get("candidate-pairs")) != count):
    raise ValueError("SPDS client or candidate-pairs count mismatch")
  return stats


def load_rq1_adaptive_results(csv_path: Path, expected: dict) -> dict:
  """Require matching, complete RQ1 samples before any RQ3 execution."""
  if not Path(csv_path).is_file():
    raise FileNotFoundError(f"RQ1 source CSV not found: {csv_path}; run RQ1 first")
  with Path(csv_path).open(encoding="utf-8", newline="") as stream:
    rows = list(csv.DictReader(stream))
  indexed = {}
  for row in rows:
    if row.get("experiment") != "adaptive":
      continue
    key = (row["dataset"], row["dot_path"])
    if key in indexed:
      raise RuntimeError(f"duplicate RQ1 adaptive row: {key}")
    indexed[key] = row
  reused = {}
  for key, identity in expected.items():
    if key not in indexed:
      raise RuntimeError(f"missing RQ1 adaptive data for {key}; run RQ1 first")
    row = indexed[key]
    provenance.validate(row, identity, require_success=True)
    if row.get("algorithm") != "adaptive" or json.loads(row["extra_arguments"]) != []:
      raise RuntimeError(f"RQ1 source is not the standard adaptive configuration: {key}")
    runs = [RunResult(status, code, float(wall), "", "", stats, stored_path(log))
            for status, code, wall, stats, log in zip(
                json.loads(row["statuses"]), json.loads(row["exit_codes"]),
                json.loads(row["wall_time_seconds"]), json.loads(row["stats_per_run"]),
                json.loads(row["log_files"]))]
    reused[key] = (runs, row["row_sha256"])
  return reused


def validate_logs(row: dict) -> None:
  provenance.validate_logs(row)
  if row.get("capture_log_file"):
    path = stored_path(row["capture_log_file"])
    if sha256(path) != row.get("capture_log_sha256"):
      raise ValueError(f"capture log SHA256 mismatch: {path}")


def validate_performance_source(row: dict) -> None:
  """Check that a reused row still refers to the same RQ1 samples."""
  if not row.get("performance_source"):
    return
  key = (row["dataset"], row["dot_path"])
  fields = ("input_sha256", "binary_sha256", "command", "extra_environment",
            "configured_repetitions", "warmup_repetitions", "timeout_seconds",
            "memory_limit_bytes")
  source = stored_path(row["performance_source"])
  runs, digest = load_rq1_adaptive_results(source, {key: {f: "" if row[f] is None else str(row[f]) for f in fields}})[key]
  if digest != row["source_row_sha256"]:
    raise ValueError(f"RQ1 source row changed: {key}")
  expected = {
      "time_ms": [int(r.stats["Time (ms)"]) for r in runs],
      "peak_rss_bytes": [int(r.stats["process peak RSS (bytes)"]) for r in runs],
      "wall_time_seconds": [r.wall_time_seconds for r in runs],
      "stats_per_run": [r.stats for r in runs],
      "log_files": relative_paths([r.log_path for r in runs]),
  }
  if any(json.loads(row[field]) != values for field, values in expected.items()):
    raise ValueError(f"RQ3 measurements differ from their RQ1 source: {key}")


def valid_measurements(row: dict[str, str]) -> bool:
  """Require complete samples and summaries that agree with those samples."""
  try:
    count = int(row["configured_repetitions"])
    if (count <= 0 or int(row["completed_repetitions"]) != count
        or int(row["successful_repetitions"]) != count
        or json.loads(row["statuses"]) != ["success"] * count
        or json.loads(row["exit_codes"]) != [0] * count):
      return False
    stats = json.loads(row["stats_per_run"])
    if len(stats) != count or not all(valid_stats(item) for item in stats):
      return False
    if row.get("tool") == "spds":
      configuration = json.loads(row["configuration"])
      analysis = engine_analysis(configuration, row["analysis"])
      if not all(valid_spds_stats(item, analysis) for item in stats):
        return False
    for field, minimum in (("time_ms", 0), ("peak_rss_bytes", 1),
                           ("wall_time_seconds", 0)):
      values = json.loads(row[field])
      if len(values) != count or any(
          not isinstance(value, (int, float)) or not math.isfinite(value)
          or value < minimum for value in values
      ):
        return False
      for prefix, summarize in (("median", median), ("average", fmean)):
        if not math.isclose(float(row[f"{prefix}_{field}"]), summarize(values),
                            rel_tol=1e-12, abs_tol=1e-12):
          return False
      stat_key = {"time_ms": "Time (ms)", "peak_rss_bytes": "process peak RSS (bytes)"}.get(field)
      if stat_key and values != [int(item[stat_key]) for item in stats]:
        return False
    return True
  except (KeyError, TypeError, ValueError):
    return False


def is_memory_failure(stderr: str, exit_code: int | None) -> bool:
  lowered = stderr.lower()
  markers = ("std::bad_alloc", "cannot allocate memory", "memoryerror")
  return any(marker in lowered for marker in markers)


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


def run_once(
    command: Sequence[str], log_path: Path, *, require_stats: bool = True,
    stdout_stream=None,
) -> RunResult:
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
        stdout=stdout_stream if stdout_stream is not None else subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=environment,
        preexec_fn=apply_limits if limit is not None else None,
        start_new_session=True,
    )
    try:
      remaining = (
          None if config.TIMEOUT_SECONDS is None
          else max(0.0, config.TIMEOUT_SECONDS - (time.perf_counter() - start))
      )
      stdout, stderr = process.communicate(timeout=remaining)
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
      try:
        os.killpg(process.pid, signal.SIGKILL)
      except ProcessLookupError:
        pass  # The process can exit between timeout detection and the kill.
      stdout, stderr = process.communicate()
      exit_code = process.returncode
      status = "timeout"
  except (OSError, subprocess.SubprocessError) as error:
    stderr = f"failed to start process: {error}\n"

  stdout = stdout or ""
  result = RunResult(
      status=status,
      exit_code=exit_code,
      wall_time_seconds=time.perf_counter() - start,
      stdout=stdout,
      stderr=stderr,
      stats=parse_stats(stdout),
      log_path=log_path,
      started=started,
  )
  if result.status == "success" and require_stats and not valid_stats(result.stats):
    result.status = "stats-error"
    result.stderr += "\nmissing or invalid time/RSS statistics\n"
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
      or not valid_measurements(row)
      or row.get("capture_status") != "success"
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


def prepare_csv(
    path: Path, resume: bool,
    expected_runs: dict[tuple[str, str, str], dict[str, str]] | None = None,
) -> set[tuple[str, str, str]]:
  path.parent.mkdir(parents=True, exist_ok=True)
  completed: set[tuple[str, str, str]] = set()
  if (resume or expected_runs is not None) and path.exists() and path.stat().st_size:
    with path.open("r", encoding="utf-8", newline="") as stream:
      reader = csv.DictReader(stream)
      if reader.fieldnames != CSV_COLUMNS:
        raise RuntimeError(f"CSV header mismatch: {path}; use a fresh DATA_DIRECTORY")
      rows = list(reader)
    if expected_runs:
      environments = {
          item["extra_environment"]
          for item in expected_runs.values()
          if "extra_environment" in item
      }
      versions = {(item["binary_sha256"], item["extra_environment"])
                  for item in expected_runs.values()
                  if "binary_sha256" in item and "extra_environment" in item}
      preserved = rows if resume else [
          row for row in rows
          if (row["dataset"], row["dot_path"], row["experiment"]) not in expected_runs
      ]
      environment_changed = environments and any(
          row.get("extra_environment") not in environments for row in preserved
      )
      binary_changed = config.REQUIRE_BINARY_IDENTITY and versions and any(
          (row.get("binary_sha256"), row.get("extra_environment")) not in versions
          for row in preserved
      )
      if environment_changed or binary_changed:
        raise RuntimeError("existing CSV uses a different binary/environment; "
                           "use a fresh DATA_DIRECTORY or rerun all methods with --restart")
    retained: list[dict[str, str]] = []
    seen: set[tuple[str, str, str]] = set()
    for row in rows:
      key = (row["dataset"], row["dot_path"], row["experiment"])
      if key in seen:
        raise RuntimeError(f"duplicate CSV row for {key}; use --restart")
      seen.add(key)
      if resume or (expected_runs is not None and key not in expected_runs):
        validate_logs(row)
      if expected_runs is not None:
        if key not in expected_runs:
          retained.append(row)
          continue
        if not resume:
          continue
        changed = any(
            row.get(field) != value
            and not (field == "binary_sha256"
                     and not config.REQUIRE_BINARY_IDENTITY)
            for field, value in expected_runs[key].items()
        )
        if changed:
          raise RuntimeError(
              f"input, binary, analysis, command or environment changed for {key}; "
              "use a fresh output CSV or --restart"
          )
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
            f"Scheduling {len(rows) - len(retained)} selected rows for rerun",
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
    benchmark: Benchmark,
    experiment: dict[str, Any],
    *, reused_results: Sequence[RunResult] | None = None,
) -> tuple[list[RunResult], list[Path], RunResult | None]:
  if experiment["name"] == "acf" and not reused_results:
    raise ValueError("ACF requires validated RQ1 measurements; run RQ1 first")
  spds_analysis = engine_analysis(experiment, benchmark.analysis)
  artifacts = expected_artifacts(benchmark, experiment)
  remove_stale_artifacts(artifacts)
  captured: list[Path] = []
  results: list[RunResult] = []

  if reused_results is not None:
    print(
        f"  [reused {len(reused_results)} runs from RQ1] "
        f"{reused_results[0].log_path.parent}",
        flush=True,
    )
    results = list(reused_results)
  else:
    for repetition in range(1, config.WARMUP_REPETITIONS + 1):
      command = make_command(benchmark, experiment, capture=False)
      print(f"  [warm-up {repetition}] {shlex.join(command)}", flush=True)
      run_once(
          command,
          log_path_for(benchmark, experiment, repetition, warmup=True),
      )

    for repetition in range(1, config.MEASURED_REPETITIONS + 1):
      command = make_command(benchmark, experiment, capture=False)
      print(f"  [run {repetition}] {shlex.join(command)}", flush=True)
      result = run_once(
          command,
          log_path_for(benchmark, experiment, repetition, warmup=False),
      )
      if (result.status == "success" and experiment["tool"] == "spds"
          and not valid_spds_stats(result.stats, spds_analysis)):
        result.status = "stats-error"
        result.stderr += "\ninvalid SPDS engine/client/stack mode or candidate count\n"
        write_log(result.log_path, command, result.started, result)
      results.append(result)
      if result.status == "memory-limit" or (
          result.status == "timeout" and config.SKIP_BENCHMARK_AFTER_TIMEOUT
      ) or (
          result.status not in {"success", "timeout"}
          and config.STOP_REPETITIONS_AFTER_FAILURE
      ):
        break
  capture_result = None
  if results and all(result.status == "success" for result in results):
    base = artifact_base(benchmark, experiment)
    base.mkdir(parents=True, exist_ok=True)
    command = make_command(benchmark, experiment, capture=True)
    print(f"  [capture] {shlex.join(command)}", flush=True)
    capture_log = log_path_for(benchmark, experiment, "capture", warmup=False)
    if experiment["tool"] == "spds":
      with (base / "upper.txt").open("w", encoding="utf-8") as stream:
        capture_result = run_once(command, capture_log, require_stats=False,
                                  stdout_stream=stream)
    else:
      capture_result = run_once(
          command, capture_log,
          require_stats=experiment["artifact_kind"] != "upper",
      )
    if capture_result.status == "success":
      try:
        if experiment["artifact_kind"] == "upper":
          with (base / "upper.txt").open(encoding="utf-8") as stream:
            if experiment["tool"] == "spds":
              capture_result.stats = capture_spds_pairs(
                  stream, artifacts[0], spds_analysis
              )
            else:
              capture_result.stats = capture_upper_pairs(stream, artifacts[0])
          capture_result.stdout = "".join(
              f"{key}: {value}\n" for key, value in capture_result.stats.items()
          )
        if not valid_stats(capture_result.stats):
          raise ValueError("missing or invalid capture time/RSS statistics")
        missing = [path for path in artifacts if not path.is_file()]
        if missing:
          raise ValueError("missing artifacts: " + ", ".join(map(str, missing)))
        # The capture must agree with the relation sizes from measured runs.
        for key in ("regularization", "mutual-refinement upper bound", "Components", "candidate-pairs"):
          if key in capture_result.stats and any(
              result.stats.get(key) != capture_result.stats[key] for result in results
          ):
            raise ValueError(f"capture and measured runs disagree on {key}")
        if experiment["artifact_kind"] == "upper":
          (base / "upper.txt").unlink()
        captured = artifacts
      except (OSError, ValueError) as error:
        capture_result.status = "artifact-error"
        capture_result.stderr += f"\n{error}\n"
      write_log(capture_result.log_path, command, capture_result.started, capture_result)
  return results, captured, capture_result


def create_row(
    benchmark: Benchmark,
    experiment: dict[str, Any],
    results: Sequence[RunResult],
    artifacts: Sequence[Path],
    capture_result: RunResult | None,
    identity: dict[str, str],
) -> dict[str, Any]:
  times = [
      parse_integer(result.stats.get("Time (ms)")) if result.status == "success" else None
      for result in results
  ]
  rss = [
      parse_integer(result.stats.get("process peak RSS (bytes)")) if result.status == "success" else None
      for result in results
  ]
  walls = [round(result.wall_time_seconds, 6) for result in results]
  complete = (
      len(results) == config.MEASURED_REPETITIONS
      and all(result.status == "success" for result in results)
  )
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
      "median_time_ms": median_value(times) if complete else None,
      "average_time_ms": average(times) if complete else None,
      "peak_rss_bytes": json_cell(rss),
      "median_peak_rss_bytes": median_value(rss) if complete else None,
      "average_peak_rss_bytes": average(rss) if complete else None,
      "wall_time_seconds": json_cell(walls),
      "median_wall_time_seconds": median_value(walls) if complete else None,
      "average_wall_time_seconds": average(walls) if complete else None,
      "timeout_seconds": config.TIMEOUT_SECONDS,
      "memory_limit_bytes": memory_limit_bytes(),
      "stats_per_run": json_cell([result.stats for result in results]),
      "relation_files": json_cell(relative_paths(artifacts)),
      "relation_sha256": json_cell(hashes),
      "log_files": json_cell(logs),
      "log_sha256": json_cell({name: sha256(result.log_path) for name, result in zip(logs, results)}),
      "capture_log_sha256": sha256(capture_result.log_path) if capture_result else "",
      "performance_source": "",
      "source_row_sha256": "",
      "command": shlex.join(base_command),
      "capture_command": json_cell(
          [shlex.join(make_command(benchmark, experiment, capture=True))]
          if capture_result is not None else []
      ),
      "capture_status": capture_result.status if capture_result else "not-run",
      "capture_log_file": (
          relative_paths([capture_result.log_path])[0] if capture_result else ""
      ),
      "capture_stats": json_cell(capture_result.stats if capture_result else {}),
      **identity,
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

  experiments = [item for item in experiments
                 if any(config.supports_analysis(item, b.analysis) for b in benchmarks)]
  if not experiments:
    raise ValueError("no applicable experiments for the selected datasets")
  for experiment in experiments:
    binary = binary_for(experiment)
    if not arguments.dry_run and (not binary.is_file() or not os.access(binary, os.X_OK)):
      raise FileNotFoundError(f"binary is not executable: {binary}")

  if arguments.dry_run:
    print(f"Benchmarks: {len(benchmarks)}")
    print(f"Applicable runs: {sum(config.supports_analysis(e, b.analysis) for b in benchmarks for e in experiments)}")
    print(f"Warm-ups/measured: {config.WARMUP_REPETITIONS}/{config.MEASURED_REPETITIONS}")
    print(f"Timeout (s): {config.TIMEOUT_SECONDS}")
    print(f"Memory limit (GiB): {config.MEMORY_LIMIT_GIB}")
    print(f"Skip benchmark after timeout: {config.SKIP_BENCHMARK_AFTER_TIMEOUT}")
    for benchmark in benchmarks:
      for experiment in experiments:
        if not config.supports_analysis(experiment, benchmark.analysis):
          continue
        if experiment["name"] == "acf":
          print(f"reuse RQ1 adaptive: {config.RQ1_OUTPUT_CSV} ({benchmark.dataset}/{benchmark.dot.name})")
        else:
          print("measure: " + shlex.join(make_command(benchmark, experiment, capture=False)))
        print("capture: " + shlex.join(make_command(benchmark, experiment, capture=True)))
    return 0

  csv_path = Path(config.OUTPUT_CSV).resolve()
  # Hash inputs and executables once, outside every timed interval. These
  # identify the actual content used by this invocation, not just its paths.
  paths = {benchmark.dot for benchmark in benchmarks}
  paths.update(binary_for(experiment) for experiment in experiments)
  fingerprints = {path: sha256(path) for path in paths}
  identities = {
      (benchmark.dataset, str(benchmark.dot.relative_to(config.REPOSITORY_ROOT)),
       experiment["name"]): {
          "input_sha256": fingerprints[benchmark.dot],
          "binary_sha256": fingerprints[binary_for(experiment)],
          "analysis": benchmark.analysis,
          "command": shlex.join(make_command(benchmark, experiment, capture=False)),
          "extra_environment": json_cell({
              str(key): str(value) for key, value in sorted(config.EXTRA_ENVIRONMENT.items())
          }),
      }
      for benchmark in benchmarks for experiment in experiments
      if config.supports_analysis(experiment, benchmark.analysis)
  }
  source_expected = {
      key[:2]: {**{field: value for field, value in identity.items() if field != "analysis"},
                "configured_repetitions": str(config.MEASURED_REPETITIONS),
                "warmup_repetitions": str(config.WARMUP_REPETITIONS),
                "timeout_seconds": "" if config.TIMEOUT_SECONDS is None else str(config.TIMEOUT_SECONDS),
                "memory_limit_bytes": "" if memory_limit_bytes() is None else str(memory_limit_bytes())}
      for key, identity in identities.items() if key[2] == "acf"
  }
  rq1_reused = load_rq1_adaptive_results(config.RQ1_OUTPUT_CSV, source_expected) if source_expected else {}
  for key, identity in identities.items():
    if key[2] == "acf":
      identity.update(performance_source=relative_paths([Path(config.RQ1_OUTPUT_CSV)])[0],
                      source_row_sha256=rq1_reused[key[:2]][1])
  completed = prepare_csv(
      csv_path, config.RESUME_EXISTING_CSV and not arguments.restart, identities
  )
  total = len(identities)
  current = 0
  failed = False
  for benchmark in benchmarks:
    relative_dot = str(benchmark.dot.relative_to(config.REPOSITORY_ROOT))
    for experiment in experiments:
      if not config.supports_analysis(experiment, benchmark.analysis):
        continue
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
      reused = (
          rq1_reused[(benchmark.dataset, relative_dot)][0]
          if experiment["name"] == "acf"
          else None
      )
      provenance.check_files(benchmark.dot, binary_for(experiment), identities[key])
      results, artifacts, capture_result = run_experiment(
          benchmark, experiment, reused_results=reused
      )
      provenance.check_files(benchmark.dot, binary_for(experiment), identities[key])
      append_row(
          csv_path,
          create_row(benchmark, experiment, results, artifacts, capture_result, identities[key]),
      )
      print(f"  appended and synced: {csv_path}", flush=True)
      if (any(result.status != "success" for result in results)
          or capture_result is None or capture_result.status != "success"):
        failed = True
      timed_out = (
          any(result.status == "timeout" for result in results)
          or (capture_result is not None and capture_result.status == "timeout")
      )
      if timed_out and config.SKIP_BENCHMARK_AFTER_TIMEOUT:
        print(
            f"  timeout; skipping remaining experiments for {benchmark.dot.name}",
            flush=True,
        )
        break
  print(f"Finished. Results: {csv_path}")
  return 1 if failed else 0


if __name__ == "__main__":
  try:
    sys.exit(main())
  except (OSError, RuntimeError, TypeError, ValueError) as error:
    print(f"run_rq3.py: {error}", file=sys.stderr)
    sys.exit(1)
