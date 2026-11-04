#!/usr/bin/env python3
"""Validate captured RQ3 relations and produce method/bound CSV files."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import sys
from pathlib import Path
from typing import Any

import config


Pair = tuple[int, int]

BOUNDS_COLUMNS = [
    "dataset",
    "dot_name",
    "dot_path",
    "status",
    "lower_size",
    "staged_upper_size",
    "acf_upper_size",
    "staged_gap_size",
    "acf_gap_size",
    "staged_certification_ratio",
    "acf_certification_ratio",
    "upper_reduction",
    "gap_reduction",
    "newly_excluded_size",
    "violations",
]

METHOD_COLUMNS = [
    "dataset",
    "dot_name",
    "dot_path",
    "experiment",
    "tool",
    "status",
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


def successful(row: dict[str, str]) -> bool:
  configured = int(row["configured_repetitions"] or 0)
  completed = int(row["completed_repetitions"] or 0)
  successes = int(row["successful_repetitions"] or 0)
  statuses = json.loads(row["statuses"])
  relation_files = json.loads(row["relation_files"])
  return (
      configured > 0
      and configured == config.MEASURED_REPETITIONS
      and completed == configured
      and successes == configured
      and statuses == ["success"] * configured
      and bool(relation_files)
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


def samples(pairs: set[Pair], limit: int = 5) -> list[list[int]]:
  return [list(pair) for pair in sorted(pairs)[:limit]]


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
  if kind == "union":
    filename = "union.pairs"
  elif kind == "staged":
    filename = "on-demand.pairs"
  else:
    filename = f"g-{configuration['family']}-{configuration['dimension']}.pairs"
  return len(load_pairs(relation_path(row, filename)))


def method_status(row: dict[str, str]) -> str:
  statuses = json.loads(row["statuses"])
  if successful(row):
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


def main() -> int:
  arguments = parse_arguments()
  with arguments.runs.resolve().open("r", encoding="utf-8", newline="") as stream:
    run_rows = list(csv.DictReader(stream))
  if not run_rows:
    raise RuntimeError(f"no run rows in {arguments.runs}")

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
    if experiment is None:
      status = "unexpected"
      unexpected_methods += 1
    elif not configuration_matches(row, experiment):
      status = "configuration-mismatch"
    method_rows.append(
        {
            "dataset": row["dataset"],
            "dot_name": row["dot_name"],
            "dot_path": row["dot_path"],
            "experiment": row["experiment"],
            "tool": row["tool"],
            "status": status,
            "relation_size": (
                primary_relation_size(row) if status == "success" else None
            ),
            "median_time_ms": row["median_time_ms"],
            "median_peak_rss_bytes": row["median_peak_rss_bytes"],
            "median_wall_time_seconds": row["median_wall_time_seconds"],
            "successful_repetitions": row["successful_repetitions"],
        }
    )

  expected_names = {str(item["name"]) for item in enabled}
  required_names = {
      str(item["name"]) for item in enabled if item.get("required", True)
  }
  core_names = {"union-dyck", "staged-on-demand", "acf"}
  if expected_names != core_names:
    raise ValueError(
        "reduced RQ3 requires exactly union-dyck, staged-on-demand, and acf"
    )
  missing_methods = 0
  failed_methods = 0
  bounds_rows: list[dict[str, Any]] = []
  for (dataset, dot_path), rows in sorted(index.items()):
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
            not successful(rows[name])
            or not configuration_matches(rows[name], experiment_by_name[name])
        )
        for name in required_names
    )
    staged = rows.get("staged-on-demand")
    acf = rows.get("acf")
    union_row = rows.get("union-dyck")
    common_ready = (
        staged is not None
        and acf is not None
        and union_row is not None
        and successful(staged)
        and successful(acf)
        and successful(union_row)
        and configuration_matches(staged, experiment_by_name["staged-on-demand"])
        and configuration_matches(acf, experiment_by_name["acf"])
        and configuration_matches(union_row, experiment_by_name["union-dyck"])
    )
    union: set[Pair] = set()
    approx: set[Pair] = set()
    components: dict[int, int] = {}
    upper: set[Pair] = set()
    violations: dict[str, Any] = {}
    status = "success" if common_ready else "incomplete"
    if common_ready:
      union = load_pairs(relation_path(staged, "union.pairs"))
      approx = load_pairs(relation_path(staged, "on-demand.pairs"))
      components = load_components(relation_path(acf, "components.map"))
      missing_vertices = {
          vertex for pair in approx for vertex in pair if vertex not in components
      }
      if missing_vertices:
        raise ValueError(
            f"{staged['dot_path']}: ACF map misses vertices "
            f"{sorted(missing_vertices)[:5]}"
        )
      upper = {
          pair for pair in approx if components[pair[0]] == components[pair[1]]
      }
      standalone_union = load_pairs(relation_path(union_row, "union.pairs"))
      if standalone_union != union:
        raise ValueError(
            f"{staged['dot_path']}: standalone and pipeline Union-Dyck differ"
        )

      checks = {
          "lower_not_in_staged_upper": union - approx,
          "lower_not_in_acf": {
              pair
              for pair in union
              if components.get(pair[0]) != components.get(pair[1])
          },
      }
      for name, bad in checks.items():
        if bad:
          violations[name] = {"count": len(bad), "sample": samples(bad)}
      if violations:
        status = "invalid"

    staged_gap = approx - union if common_ready else set()
    acf_gap = upper - union if common_ready else set()
    representative = next(iter(rows.values()))
    bounds_rows.append(
        {
            "dataset": representative["dataset"],
            "dot_name": representative["dot_name"],
            "dot_path": representative["dot_path"],
            "status": status,
            "lower_size": len(union) if common_ready else None,
            "staged_upper_size": len(approx) if common_ready else None,
            "acf_upper_size": len(upper) if common_ready else None,
            "staged_gap_size": len(staged_gap) if common_ready else None,
            "acf_gap_size": len(acf_gap) if common_ready else None,
            "staged_certification_ratio": (
                1.0 if not approx else len(union) / len(approx)
            )
            if common_ready
            else None,
            "acf_certification_ratio": (
                1.0 if not upper else len(union) / len(upper)
            )
            if common_ready
            else None,
            "upper_reduction": (
                None if not approx else 1.0 - len(upper) / len(approx)
            )
            if common_ready
            else None,
            "gap_reduction": (
                None
                if not staged_gap
                else 1.0 - len(acf_gap) / len(staged_gap)
            )
            if common_ready
            else None,
            "newly_excluded_size": (
                len(approx - upper) if common_ready else None
            ),
            "violations": json.dumps(
                violations, ensure_ascii=False, separators=(",", ":")
            ),
        }
    )

  write_rows(arguments.methods.resolve(), METHOD_COLUMNS, method_rows)
  write_rows(arguments.bounds.resolve(), BOUNDS_COLUMNS, bounds_rows)
  invalid = sum(row["status"] == "invalid" for row in bounds_rows)
  incomplete = sum(row["status"] == "incomplete" for row in bounds_rows)
  print(f"Methods: {arguments.methods.resolve()}")
  print(f"Bounds: {arguments.bounds.resolve()}")
  print(
      f"Bound rows: {len(bounds_rows)}; invalid: {invalid}; "
      f"incomplete: {incomplete}; "
      f"missing methods: {missing_methods}; "
      f"failed methods: {failed_methods}; unexpected methods: "
      f"{unexpected_methods}"
  )
  return 1 if (
      invalid
      or incomplete
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
