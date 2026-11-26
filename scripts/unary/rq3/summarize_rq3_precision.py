#!/usr/bin/env python3
"""Print the paper's RQ3 precision statistics from rq3-bounds.csv."""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from rq3 import config_rq3 as config


METHODS = ("mutual-refinement", "spds")
SUITES = ("taint", "valueflow")


def print_table(headers: list[str], rows: list[list[str]]) -> None:
  widths = [max(len(row[i]) for row in [headers, *rows])
            for i in range(len(headers))]
  for row in [headers, *rows]:
    print("  ".join(value.ljust(width) for value, width in zip(row, widths)).rstrip())


def percent(value: float | None) -> str:
  return "N/A" if value is None else f"{100 * value:.3f}%"


def summarize(path: Path) -> None:
  with path.open(newline="", encoding="utf-8") as stream:
    rows = [row for row in csv.DictReader(stream)
            if row["experiment"] in METHODS]
  if not rows:
    raise ValueError("no Mutual refinement or SPDS comparisons found")

  seen = set()
  for row in rows:
    key = (row["dataset"], row["dot_name"], row["experiment"])
    if row["dataset"] not in SUITES or key in seen:
      raise ValueError(f"unexpected suite or duplicate comparison: {key}")
    seen.add(key)
    if row["status"] == "success":
      upper = int(row["typed_upper_size"])
      kept = int(row["intersection_size"])
      removed = int(row["newly_excluded_size"])
      if not 0 <= kept <= upper or removed != upper - kept:
        raise ValueError(f"inconsistent pair counts: {key}")

  summary = []
  details = []
  for suite in SUITES:
    for method in METHODS:
      selected = [r for r in rows
                  if r["dataset"] == suite and r["experiment"] == method]
      completed = [r for r in selected if r["status"] == "success"]
      upper_total = removed_total = 0
      reductions = []
      improved = []
      for row in completed:
        upper = int(row["typed_upper_size"])
        removed = int(row["newly_excluded_size"])
        upper_total += upper
        removed_total += removed
        if upper:
          reductions.append(removed / upper)
        if removed:
          improved.append(row)
      summary.append([
          suite, method, f"{len(completed)}/{len(selected)}",
          f"{len(improved)}/{len(completed)}", str(removed_total),
          percent(sum(reductions) / len(reductions) if reductions else None),
          percent(removed_total / upper_total if upper_total else None),
          percent(max(reductions) if reductions else None),
      ])
      for row in sorted(improved, key=lambda r: (
          -int(r["newly_excluded_size"]) / int(r["typed_upper_size"]), r["dot_name"]
      )):
        details.append([
            suite, method, Path(row["dot_name"]).stem,
            row["typed_upper_size"], row["intersection_size"],
            row["newly_excluded_size"],
            percent(int(row["newly_excluded_size"]) / int(row["typed_upper_size"])),
        ])

  print(f"Source: {path.resolve()}")
  print("Completed = successful / CSV comparisons; improved = reduced / successful.")
  print("Failed/incomplete rows are excluded. Macro excludes empty upper bounds.")
  print("Macro = mean per-input reduction; micro = total removed / total candidates.")
  print("All reductions are percentages; N/A means no defined ratio.\n")
  print_table(["Suite", "Method", "Completed", "Improved", "Removed",
               "Macro", "Micro", "Max"], summary)
  print("\nInputs with additional pruning:")
  print_table(["Suite", "Method", "Input", "Before", "After", "Removed", "Reduction"],
              details)


def main() -> None:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--bounds", type=Path, default=config.BOUNDS_CSV,
                      help="input precision CSV (default: configured rq3-bounds.csv)")
  args = parser.parse_args()
  try:
    summarize(args.bounds)
  except (OSError, KeyError, ValueError) as error:
    parser.exit(1, f"{error}\n")


if __name__ == "__main__":
  main()
