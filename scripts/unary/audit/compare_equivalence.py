#!/usr/bin/env python3
"""Compare complete unary-reachability partitions across configurations.

The benchmark CSV is used only to select projects on which both configurations
completed successfully.  Each selected configuration is rerun once with
``--dump-components``.  Component identifiers are arbitrary, so the script
canonicalizes every component to its smallest vertex before comparing maps.

Examples:
  python3 scripts/unary/audit/compare_equivalence.py --dry-run
  python3 scripts/unary/audit/compare_equivalence.py \
      --comparison acf-direct-acf
  python3 scripts/unary/audit/compare_equivalence.py \
      --comparison kp22-acf --output-dir /tmp/kp22-acf-equivalence
"""

from __future__ import annotations

import argparse
import csv
import resource
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


UNARY_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(UNARY_DIRECTORY))
from rq1 import config_rq1 as config  # noqa: E402


@dataclass(frozen=True)
class Configuration:
  csv_name: str
  display_name: str
  algorithm: str
  arguments: tuple[str, ...] = ()


@dataclass(frozen=True)
class Comparison:
  name: str
  left: Configuration
  right: Configuration


COMPARISONS = {
    "kp22-acf": Comparison(
        "kp22-acf",
        Configuration("fixed-counter", "KP22", "fixed-counter"),
        Configuration("adaptive", "ACF", "adaptive"),
    ),
    "kp22-acf-direct": Comparison(
        "kp22-acf-direct",
        Configuration("fixed-counter", "KP22", "fixed-counter"),
        Configuration(
            "adaptive-direct", "ACF-Direct", "adaptive", ("--direct",)
        ),
    ),
    "acf-direct-acf": Comparison(
        "acf-direct-acf",
        Configuration(
            "adaptive-direct", "ACF-Direct", "adaptive", ("--direct",)
        ),
        Configuration("adaptive", "ACF", "adaptive"),
    ),
}


SUMMARY_COLUMNS = [
    "comparison",
    "dataset",
    "project",
    "status",
    "vertices",
    "left_components",
    "right_components",
    "differing_vertices",
    "elapsed_seconds",
    "left_map",
    "right_map",
]


def parse_arguments() -> argparse.Namespace:
  parser = argparse.ArgumentParser(
      description=(
          "Rerun jointly completed unary configurations and compare their "
          "complete vertex partitions."
      )
  )
  parser.add_argument(
      "--comparison",
      choices=("all", *COMPARISONS),
      default="all",
      help="configuration pair to check (default: all)",
  )
  parser.add_argument(
      "--results",
      type=Path,
      default=Path(config.OUTPUT_CSV),
      help="benchmark CSV used to select jointly completed projects",
  )
  parser.add_argument(
      "--binary",
      type=Path,
      default=Path(config.BINARY_PATH),
      help="lotus-cfl-interleaved-dyck binary",
  )
  parser.add_argument(
      "--output-dir",
      type=Path,
      help="retain component maps and equivalence-summary.csv in this directory",
  )
  parser.add_argument(
      "--timeout",
      type=float,
      default=float(config.TIMEOUT_SECONDS or 0),
      help="per-process timeout in seconds; 0 disables it",
  )
  parser.add_argument(
      "--memory-limit-gib",
      type=float,
      default=float(config.MEMORY_LIMIT_GIB or 0),
      help="per-process RLIMIT_AS in GiB; 0 disables it",
  )
  parser.add_argument(
      "--dry-run",
      action="store_true",
      help="print selected projects and commands without running them",
  )
  arguments = parser.parse_args()
  if arguments.timeout < 0:
    parser.error("--timeout cannot be negative")
  if arguments.memory_limit_gib < 0:
    parser.error("--memory-limit-gib cannot be negative")
  return arguments


def completed(row: dict[str, str]) -> bool:
  try:
    configured = int(row["configured_repetitions"])
    successful = int(row["successful_repetitions"])
  except (KeyError, ValueError) as error:
    raise ValueError(f"invalid repetition counts in CSV row: {row}") from error
  return configured > 0 and successful == configured


def load_rows(path: Path) -> dict[tuple[str, str, str], dict[str, str]]:
  with path.open("r", encoding="utf-8", newline="") as stream:
    rows = list(csv.DictReader(stream))
  index: dict[tuple[str, str, str], dict[str, str]] = {}
  for row in rows:
    key = (row["dataset"], row["dot_name"], row["experiment"])
    if key in index:
      raise ValueError(f"duplicate CSV row: {key}")
    index[key] = row
  return index


def selected_cases(
    rows: dict[tuple[str, str, str], dict[str, str]], comparison: Comparison
) -> list[tuple[str, str, Path]]:
  cases: list[tuple[str, str, Path]] = []
  projects = sorted({(dataset, name) for dataset, name, _ in rows})
  for dataset, name in projects:
    left = rows.get((dataset, name, comparison.left.csv_name))
    right = rows.get((dataset, name, comparison.right.csv_name))
    if left is None or right is None or not completed(left) or not completed(right):
      continue
    if left["dot_path"] != right["dot_path"]:
      raise ValueError(
          f"{dataset}/{name}: configurations reference different DOT files"
      )
    dot = Path(left["dot_path"])
    if not dot.is_absolute():
      dot = Path(config.REPOSITORY_ROOT) / dot
    cases.append((dataset, name, dot.resolve()))
  return cases


def component_command(
    binary: Path, configuration: Configuration, dot: Path, output: Path
) -> list[str]:
  return [
      str(binary),
      str(config.ENGINE),
      "--algorithm",
      configuration.algorithm,
      "--bidirect",
      *configuration.arguments,
      "--dump-components",
      str(output),
      str(dot),
  ]


def memory_limiter(limit_gib: float):
  if limit_gib == 0:
    return None
  limit_bytes = int(limit_gib * 1024**3)

  def apply_limit() -> None:
    resource.setrlimit(resource.RLIMIT_AS, (limit_bytes, limit_bytes))

  return apply_limit


def run_command(
    command: Sequence[str], timeout: float, limit_gib: float
) -> subprocess.CompletedProcess[str]:
  return subprocess.run(
      command,
      check=False,
      stdout=subprocess.PIPE,
      stderr=subprocess.STDOUT,
      text=True,
      timeout=timeout or None,
      preexec_fn=memory_limiter(limit_gib),
  )


def load_canonical_partition(path: Path) -> dict[int, int]:
  vertex_components: dict[int, int] = {}
  for line_number, line in enumerate(path.read_text().splitlines(), 1):
    fields = line.split()
    if len(fields) != 2:
      raise ValueError(f"{path}:{line_number}: expected VERTEX COMPONENT")
    vertex, component = map(int, fields)
    if vertex in vertex_components:
      raise ValueError(f"{path}:{line_number}: duplicate vertex {vertex}")
    vertex_components[vertex] = component

  groups: dict[int, list[int]] = {}
  for vertex, component in vertex_components.items():
    groups.setdefault(component, []).append(vertex)
  representatives = {
      component: min(vertices) for component, vertices in groups.items()
  }
  return {
      vertex: representatives[component]
      for vertex, component in vertex_components.items()
  }


def relative_or_absolute(path: Path, root: Path) -> str:
  try:
    return str(path.relative_to(root))
  except ValueError:
    return str(path)


def write_summary(path: Path, rows: Iterable[dict[str, object]]) -> None:
  with path.open("w", encoding="utf-8", newline="") as stream:
    writer = csv.DictWriter(stream, fieldnames=SUMMARY_COLUMNS)
    writer.writeheader()
    writer.writerows(rows)


def compare_one(
    comparison: Comparison,
    dataset: str,
    project: str,
    dot: Path,
    root: Path,
    binary: Path,
    timeout: float,
    limit_gib: float,
) -> dict[str, object]:
  output = root / comparison.name / dataset / Path(project).stem
  output.mkdir(parents=True, exist_ok=True)
  left_map = output / "left.components"
  right_map = output / "right.components"
  left_command = component_command(binary, comparison.left, dot, left_map)
  right_command = component_command(binary, comparison.right, dot, right_map)
  start = time.monotonic()

  try:
    left_run = run_command(left_command, timeout, limit_gib)
  except subprocess.TimeoutExpired:
    return result_row(comparison, dataset, project, "left-timeout", start)
  if left_run.returncode != 0:
    print(left_run.stdout[-1000:], file=sys.stderr)
    return result_row(comparison, dataset, project, "left-error", start)

  try:
    right_run = run_command(right_command, timeout, limit_gib)
  except subprocess.TimeoutExpired:
    return result_row(comparison, dataset, project, "right-timeout", start)
  if right_run.returncode != 0:
    print(right_run.stdout[-1000:], file=sys.stderr)
    return result_row(comparison, dataset, project, "right-error", start)

  left = load_canonical_partition(left_map)
  right = load_canonical_partition(right_map)
  all_vertices = set(left) | set(right)
  differing = [
      vertex for vertex in all_vertices if left.get(vertex) != right.get(vertex)
  ]
  status = "match" if not differing else "mismatch"
  return {
      "comparison": comparison.name,
      "dataset": dataset,
      "project": project,
      "status": status,
      "vertices": len(all_vertices),
      "left_components": len(set(left.values())),
      "right_components": len(set(right.values())),
      "differing_vertices": len(differing),
      "elapsed_seconds": f"{time.monotonic() - start:.3f}",
      "left_map": relative_or_absolute(left_map, root),
      "right_map": relative_or_absolute(right_map, root),
  }


def result_row(
    comparison: Comparison,
    dataset: str,
    project: str,
    status: str,
    start: float,
) -> dict[str, object]:
  return {
      "comparison": comparison.name,
      "dataset": dataset,
      "project": project,
      "status": status,
      "vertices": "",
      "left_components": "",
      "right_components": "",
      "differing_vertices": "",
      "elapsed_seconds": f"{time.monotonic() - start:.3f}",
      "left_map": "",
      "right_map": "",
  }


def main() -> int:
  arguments = parse_arguments()
  results_path = arguments.results.expanduser().resolve()
  binary = arguments.binary.expanduser().resolve()
  if not results_path.is_file():
    raise FileNotFoundError(results_path)
  if not binary.is_file():
    raise FileNotFoundError(binary)
  rows = load_rows(results_path)
  comparisons = (
      list(COMPARISONS.values())
      if arguments.comparison == "all"
      else [COMPARISONS[arguments.comparison]]
  )

  selected = [(item, selected_cases(rows, item)) for item in comparisons]
  if arguments.dry_run:
    for comparison, cases in selected:
      print(f"{comparison.name}: {len(cases)} jointly completed projects")
      for dataset, project, dot in cases:
        placeholder = Path("COMPONENTS.map")
        print(" ".join(component_command(binary, comparison.left, dot, placeholder)))
        print(" ".join(component_command(binary, comparison.right, dot, placeholder)))
    return 0

  temporary: tempfile.TemporaryDirectory[str] | None = None
  if arguments.output_dir:
    output_root = arguments.output_dir.expanduser().resolve()
    output_root.mkdir(parents=True, exist_ok=True)
  else:
    temporary = tempfile.TemporaryDirectory(prefix="lotus-unary-equivalence-")
    output_root = Path(temporary.name)

  summary: list[dict[str, object]] = []
  try:
    for comparison, cases in selected:
      print(f"{comparison.name}: comparing {len(cases)} projects", flush=True)
      for index, (dataset, project, dot) in enumerate(cases, 1):
        row = compare_one(
            comparison,
            dataset,
            project,
            dot,
            output_root,
            binary,
            arguments.timeout,
            arguments.memory_limit_gib,
        )
        summary.append(row)
        print(
            f"[{index}/{len(cases)}] {dataset}/{project}: {row['status']} "
            f"vertices={row['vertices']} components="
            f"{row['left_components']}/{row['right_components']} "
            f"elapsed={row['elapsed_seconds']}s",
            flush=True,
        )
    if arguments.output_dir:
      write_summary(output_root / "equivalence-summary.csv", summary)
  finally:
    if temporary is not None:
      temporary.cleanup()

  failures = [row for row in summary if row["status"] != "match"]
  print(
      f"summary: {len(summary) - len(failures)}/{len(summary)} match; "
      f"{len(failures)} failures",
      flush=True,
  )
  return 1 if failures else 0


if __name__ == "__main__":
  raise SystemExit(main())
