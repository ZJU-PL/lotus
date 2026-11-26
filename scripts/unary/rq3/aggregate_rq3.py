#!/usr/bin/env python3
"""Compare typed upper bounds with ACF and summarize RQ3 performance."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from rq3 import config_rq3 as config
from rq3.run_rq3 import valid_measurements, validate_logs, validate_performance_source


Pair = tuple[int, int]

BOUNDS_COLUMNS = [
    "dataset",
    "dot_name",
    "dot_path",
    "experiment",
    "status",
    "typed_upper_size",
    "intersection_size",
    "upper_reduction",
    "newly_excluded_size",
]

METHOD_COLUMNS = [
    "dataset",
    "dot_name",
    "dot_path",
    "experiment",
    "tool",
    "status",
    "capture_status",
    "relation_size",
    "median_time_ms",
    "median_peak_rss_bytes",
    "median_wall_time_seconds",
    "successful_repetitions",
]


def parse_arguments() -> argparse.Namespace:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--runs", type=Path, default=config.OUTPUT_CSV)
  parser.add_argument("--bounds", type=Path, default=config.BOUNDS_CSV)
  parser.add_argument("--methods", type=Path, default=config.METHODS_CSV)
  return parser.parse_args()


def load_pairs(path: Path) -> set[Pair]:
  result: set[Pair] = set()
  lines = 0
  with path.open("r", encoding="utf-8") as stream:
    for number, line in enumerate(stream, start=1):
      fields = line.split()
      if not fields:
        continue
      if len(fields) != 2:
        raise ValueError(f"{path}:{number}: expected two integers")
      pair = (int(fields[0]), int(fields[1]))
      if pair[0] == pair[1]:
        raise ValueError(f"{path}:{number}: reflexive pair {pair}")
      result.add(pair)
      lines += 1
  if lines != len(result):
    raise ValueError(f"{path}: duplicate pairs detected")
  return result


def load_components(path: Path) -> dict[int, int]:
  result: dict[int, int] = {}
  with path.open("r", encoding="utf-8") as stream:
    for number, line in enumerate(stream, start=1):
      fields = line.split()
      if not fields:
        continue
      if len(fields) != 2:
        raise ValueError(f"{path}:{number}: expected vertex and component")
      vertex, component = map(int, fields)
      if vertex in result:
        raise ValueError(f"{path}:{number}: duplicate vertex {vertex}")
      result[vertex] = component
  return result


def write_rows(path: Path, columns: list[str], rows: list[dict[str, Any]]) -> None:
  path.parent.mkdir(parents=True, exist_ok=True)
  with path.open("w", encoding="utf-8", newline="") as stream:
    writer = csv.DictWriter(stream, fieldnames=columns)
    writer.writeheader()
    writer.writerows(rows)
    stream.flush()
    os.fsync(stream.fileno())


def measurement_successful(row: dict[str, str]) -> bool:
  configured = int(row["configured_repetitions"] or 0)
  completed = int(row["completed_repetitions"] or 0)
  successes = int(row["successful_repetitions"] or 0)
  statuses = json.loads(row["statuses"])
  return (
      configured > 0
      and valid_measurements(row)
      and configured == config.MEASURED_REPETITIONS
      and completed == configured
      and successes == configured
      and statuses == ["success"] * configured
      and row.get("warmup_repetitions") == str(config.WARMUP_REPETITIONS)
      and row.get("timeout_seconds")
      == ("" if config.TIMEOUT_SECONDS is None else str(config.TIMEOUT_SECONDS))
      and row.get("memory_limit_bytes")
      == (
          ""
          if config.MEMORY_LIMIT_GIB is None
          else str(int(config.MEMORY_LIMIT_GIB * 1024**3))
      )
  )


def successful(row: dict[str, str]) -> bool:
  """Precision comparisons additionally require a successful relation capture."""
  return (
      measurement_successful(row)
      and row.get("capture_status") == "success"
      and bool(json.loads(row["relation_files"]))
  )


def sha256(path: Path) -> str:
  digest = hashlib.sha256()
  with path.open("rb") as stream:
    for block in iter(lambda: stream.read(1024 * 1024), b""):
      digest.update(block)
  return digest.hexdigest()


def relation_path(row: dict[str, str], filename: str) -> Path:
  stored = json.loads(row["relation_files"])
  matches = [value for value in stored if Path(value).name == filename]
  if len(matches) != 1:
    raise ValueError(
        f"{row['dot_path']} {row['experiment']}: expected one {filename}"
    )
  value = matches[0]
  candidate = Path(value)
  path = candidate if candidate.is_absolute() else config.REPOSITORY_ROOT / candidate
  if not path.is_file():
    raise FileNotFoundError(path)
  hashes = json.loads(row["relation_sha256"])
  expected = hashes.get(value)
  if expected is None:
    raise ValueError(f"{row['dot_path']} {row['experiment']}: missing SHA-256")
  actual = sha256(path)
  if actual != expected:
    raise ValueError(
        f"{row['dot_path']} {row['experiment']}: SHA-256 mismatch for {path}"
    )
  return path


def primary_relation_size(row: dict[str, str]) -> int | None:
  if not successful(row):
    return None
  configuration = json.loads(row["configuration"])
  kind = configuration["artifact_kind"]
  if kind == "components":
    components = load_components(relation_path(row, "components.map"))
    sizes: dict[int, int] = {}
    for component in components.values():
      sizes[component] = sizes.get(component, 0) + 1
    return sum(size * (size - 1) for size in sizes.values())
  return len(load_pairs(relation_path(row, "upper.pairs")))


def method_status(row: dict[str, str]) -> str:
  statuses = json.loads(row["statuses"])
  if measurement_successful(row):
    return "success"
  if statuses and all(status == "success" for status in statuses):
    return "protocol-mismatch"
  if any(status == "success" for status in statuses):
    return "partial"
  return statuses[0] if statuses else "not-run"


def configuration_matches(
    row: dict[str, str] | None, experiment: dict[str, Any]
) -> bool:
  if row is None:
    return False
  expected = {key: value for key, value in experiment.items() if key != "enabled"}
  return json.loads(row["configuration"]) == expected


def validate_run_identities(rows: list[dict[str, str]]) -> None:
  """Check fixed inputs/environment and, when enabled, one binary."""
  identities: dict[tuple[str, str], tuple[str, str]] = {}
  for row in rows:
    for field in ("input_sha256", "binary_sha256"):
      value = row.get(field, "")
      if len(value) != 64 or any(char not in "0123456789abcdef" for char in value):
        raise ValueError(f"missing or invalid {field}: {row['dot_path']}")
    key = (row["dataset"], row["dot_path"])
    identity = (row["analysis"], row["input_sha256"])
    if key in identities and identities[key] != identity:
      raise ValueError(f"mixed input contents or client analyses: {key}")
    identities[key] = identity
  if len({row["extra_environment"] for row in rows}) > 1:
    raise ValueError("RQ3 rows mix different EXTRA_ENVIRONMENT settings")
  if config.REQUIRE_BINARY_IDENTITY and len({row["binary_sha256"] for row in rows}) > 1:
    raise ValueError("RQ3 rows mix different binaries")


def main() -> int:
  arguments = parse_arguments()
  with arguments.runs.resolve().open("r", encoding="utf-8", newline="") as stream:
    run_rows = list(csv.DictReader(stream))
  if not run_rows:
    raise RuntimeError(f"no run rows in {arguments.runs}")
  validate_run_identities(run_rows)
  for row in run_rows:
    if "log_sha256" in row:
      validate_logs(row)
      validate_performance_source(row)

  enabled = [item for item in config.EXPERIMENTS if item.get("enabled", True)]
  experiment_by_name = {str(item["name"]): item for item in enabled}
  unexpected_methods = 0
  index: dict[tuple[str, str], dict[str, dict[str, str]]] = {}
  method_rows: list[dict[str, Any]] = []
  for row in run_rows:
    key = (row["dataset"], row["dot_path"])
    experiment_rows = index.setdefault(key, {})
    if row["experiment"] in experiment_rows:
      raise ValueError(
          f"duplicate run row: {key[0]} {key[1]} {row['experiment']}"
      )
    experiment_rows[row["experiment"]] = row
    experiment = experiment_by_name.get(row["experiment"])
    status = method_status(row)
    if experiment is None or not config.supports_analysis(experiment, row["analysis"]):
      status = "unexpected"
      unexpected_methods += 1
    elif not configuration_matches(row, experiment):
      status = "configuration-mismatch"
    costs_ready = status == "success"
    method_rows.append(
        {
            "dataset": row["dataset"],
            "dot_name": row["dot_name"],
            "dot_path": row["dot_path"],
            "experiment": row["experiment"],
            "tool": row["tool"],
            "status": status,
            "capture_status": row["capture_status"],
            "relation_size": (
                primary_relation_size(row) if status == "success" else None
            ),
            "median_time_ms": row["median_time_ms"] if costs_ready else None,
            "median_peak_rss_bytes": row["median_peak_rss_bytes"] if costs_ready else None,
            "median_wall_time_seconds": row["median_wall_time_seconds"] if costs_ready else None,
            "successful_repetitions": row["successful_repetitions"],
        }
    )

  if "acf" not in experiment_by_name or any(
      item["artifact_kind"] not in {"upper", "components"} for item in enabled
  ):
    raise ValueError("RQ3 requires ACF and typed upper-bound methods")
  missing_methods = 0
  failed_methods = 0
  bounds_rows: list[dict[str, Any]] = []
  for (dataset, dot_path), rows in sorted(index.items()):
    representative = next(iter(rows.values()))
    applicable = [item for item in enabled
                  if config.supports_analysis(item, representative["analysis"])]
    expected_names = {item["name"] for item in applicable}
    required_names = {item["name"] for item in applicable if item.get("required", True)}
    missing = sorted(expected_names - rows.keys())
    if missing:
      missing_methods += len(missing)
      print(
          f"Incomplete {dataset}/{dot_path}: missing {', '.join(missing)}",
          file=sys.stderr,
      )
    failed_methods += sum(
        name in rows
        and (
            not measurement_successful(rows[name])
            or not configuration_matches(rows[name], experiment_by_name[name])
        )
        for name in required_names
    )
    acf = rows.get("acf")
    acf_ready = (
        acf is not None and successful(acf)
        and configuration_matches(acf, experiment_by_name["acf"])
    )
    components = {}
    if acf_ready:
      components = load_components(relation_path(acf, "components.map"))
    for name in (item["name"] for item in applicable if item["artifact_kind"] == "upper"):
      typed = rows.get(name)
      typed_ready = (
          typed is not None and successful(typed)
          and configuration_matches(typed, experiment_by_name[name])
      )
      approx = load_pairs(relation_path(typed, "upper.pairs")) if typed_ready else set()
      common_ready = typed_ready and acf_ready
      intersection_size = None
      if common_ready:
        missing_vertices = {
            vertex for pair in approx for vertex in pair if vertex not in components
        }
        if missing_vertices:
          raise ValueError(
              f"{dot_path}: ACF map misses vertices {sorted(missing_vertices)[:5]}"
          )
        intersection_size = sum(
            components[source] == components[target] for source, target in approx
        )
      bounds_rows.append(
          {
              "dataset": representative["dataset"],
              "dot_name": representative["dot_name"],
              "dot_path": representative["dot_path"],
              "experiment": name,
              "status": "success" if common_ready else "incomplete",
              "typed_upper_size": len(approx) if typed_ready else None,
              "intersection_size": intersection_size,
              "upper_reduction": (
                  1.0 - intersection_size / len(approx)
                  if common_ready and approx else None
              ),
              "newly_excluded_size": (
                  len(approx) - intersection_size if common_ready else None
              ),
          }
      )

  write_rows(arguments.methods.resolve(), METHOD_COLUMNS, method_rows)
  write_rows(arguments.bounds.resolve(), BOUNDS_COLUMNS, bounds_rows)
  incomplete = sum(row["status"] == "incomplete" for row in bounds_rows)
  print(f"Methods: {arguments.methods.resolve()}")
  print(f"Bounds: {arguments.bounds.resolve()}")
  print(
      f"Bound rows: {len(bounds_rows)}; "
      f"incomplete: {incomplete}; "
      f"missing methods: {missing_methods}; "
      f"failed methods: {failed_methods}; unexpected methods: "
      f"{unexpected_methods}"
  )
  return 1 if (
      incomplete
      or missing_methods
      or failed_methods
      or unexpected_methods
  ) else 0


if __name__ == "__main__":
  try:
    sys.exit(main())
  except (OSError, RuntimeError, TypeError, ValueError) as error:
    print(f"aggregate_rq3.py: {error}", file=sys.stderr)
    sys.exit(1)
