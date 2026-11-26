#!/usr/bin/env python3
"""Regression tests for the RQ3 runner and aggregator."""

from __future__ import annotations

import csv
import json
import os
import random
import re
import signal
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rq3 import aggregate_rq3
from rq3 import config_rq3 as config
from rq3 import run_rq3


def json_cell(value: object) -> str:
  return json.dumps(value, separators=(",", ":"))


class RQ3Test(unittest.TestCase):
  def test_spds_scope_and_capture_command(self) -> None:
    experiment = next(e for e in config.EXPERIMENTS if e["name"] == "spds")
    self.assertTrue(config.supports_analysis(experiment, "value-flow"))
    self.assertTrue(config.supports_analysis(experiment, "taint"))
    for analysis, engine_analysis in (
        ("taint", "nonreflexive-taint"),
        ("value-flow", "value-flow"),
    ):
      with self.subTest(analysis=analysis):
        benchmark = run_rq3.Benchmark(
            analysis, analysis, config.REPOSITORY_ROOT,
            config.REPOSITORY_ROOT / "input.dot",
        )
        measured = run_rq3.make_command(benchmark, experiment, capture=False)
        captured = run_rq3.make_command(benchmark, experiment, capture=True)
        self.assertEqual(
            measured[measured.index("--analysis") + 1], engine_analysis
        )
        self.assertNotIn("--pairs", measured)
        self.assertIn("--all-pairs", measured)
        self.assertIn("--pairs", captured)
        self.assertNotIn("--print-result", captured)

  def test_spds_stream_checks(self) -> None:
    summary = ("engine: spds\nanalysis: value-flow\ncall-stack: empty\nfield-stack: empty\n"
               "candidate-pairs: 2\nTime (ms): 3\nprocess peak RSS (bytes): 100\n")
    with tempfile.TemporaryDirectory() as directory:
      path = Path(directory) / "upper.pairs"
      stats = run_rq3.capture_spds_pairs((summary + "pair: -1 2\npair: 2 3\n").splitlines(), path, "value-flow")
      self.assertTrue(run_rq3.valid_stats(stats))
      self.assertEqual(path.read_text(), "-1 2\n2 3\n")
      for pairs in ("pair: 1 2\n", "pair: 1 2\npair: 1 2\n",
                    "pair: 2 3\npair: 1 2\n", "pair: 1 1\npair: 2 3\n"):
        with self.subTest(pairs=pairs), self.assertRaises(ValueError):
          run_rq3.capture_spds_pairs((summary + pairs).splitlines(), path, "value-flow")
      with self.assertRaisesRegex(ValueError, "client"):
        run_rq3.capture_spds_pairs(
            (summary + "pair: -1 2\npair: 2 3\n").splitlines(),
            path, "nonreflexive-taint",
        )
      stats = run_rq3.capture_spds_pairs(
          (summary.replace("value-flow", "nonreflexive-taint")
           + "pair: -1 2\npair: 2 3\n").splitlines(),
          path, "nonreflexive-taint")
      self.assertEqual(stats["analysis"], "nonreflexive-taint")

  def test_spds_rejects_wrong_measured_client(self) -> None:
    experiment = next(e for e in config.EXPERIMENTS if e["name"] == "spds")
    benchmark = run_rq3.Benchmark("taint", "taint", config.REPOSITORY_ROOT,
                                 config.REPOSITORY_ROOT / "input.dot")
    stats = {"engine": "spds", "analysis": "value-flow", "call-stack": "empty",
             "field-stack": "empty", "candidate-pairs": "0",
             "Time (ms)": "1", "process peak RSS (bytes)": "100"}
    with tempfile.TemporaryDirectory() as directory:
      root = Path(directory)
      result = run_rq3.RunResult("success", 0, 0.1, "", "", stats, root / "run.log")
      with (mock.patch.object(config, "ARTIFACT_DIRECTORY", root / "artifacts"),
            mock.patch.object(run_rq3, "run_once", return_value=result)):
        runs, files, capture = run_rq3.run_experiment(benchmark, experiment)
      self.assertEqual(runs[0].status, "stats-error")
      self.assertEqual(files, [])
      self.assertIsNone(capture)
      self.assertIn("Status: stats-error", result.log_path.read_text())
      for key, value in (("engine", "wrong"), ("call-stack", "any"),
                         ("field-stack", "any"), ("candidate-pairs", "-1")):
        invalid = {**stats, "analysis": "nonreflexive-taint", key: value}
        self.assertFalse(
            run_rq3.valid_spds_stats(invalid, "nonreflexive-taint")
        )
      legacy = {**stats, "analysis": "taint"}
      self.assertFalse(run_rq3.valid_spds_stats(legacy, "taint"))

  def test_restart_preserves_unselected_rows_and_checks_versions(self) -> None:
    selected = self.make_row({"name": "spds", "tool": "spds"}, [])
    other = {**selected, "dot_path": "other.dot", "dot_name": "other.dot"}
    key = ("test", "input.dot", "spds")
    other_key = ("test", "other.dot", "spds")
    identity = {"binary_sha256": "2" * 64, "extra_environment": "{}"}
    with tempfile.TemporaryDirectory() as directory:
      path = Path(directory) / "runs.csv"
      aggregate_rq3.write_rows(path, run_rq3.CSV_COLUMNS, [selected, other])
      self.assertEqual(run_rq3.prepare_csv(path, False, {key: identity}), set())
      with path.open(newline="") as stream:
        self.assertEqual(list(csv.DictReader(stream)), [other])
      new_identity = {**identity, "binary_sha256": "3" * 64}
      self.assertEqual(run_rq3.prepare_csv(path, False, {key: new_identity}), set())
      with path.open(newline="") as stream:
        self.assertEqual(list(csv.DictReader(stream)), [other])
      with mock.patch.object(config, "REQUIRE_BINARY_IDENTITY", True):
        before = path.read_bytes()
        with self.assertRaisesRegex(RuntimeError, "different binary"):
          run_rq3.prepare_csv(path, False, {key: new_identity})
        self.assertEqual(path.read_bytes(), before)
      self.assertEqual(run_rq3.prepare_csv(path, False,
                       {key: new_identity, other_key: new_identity}), set())
      with path.open(newline="") as stream:
        self.assertEqual(list(csv.DictReader(stream)), [])

  def test_new_method_cannot_append_to_old_binary_results(self) -> None:
    row = self.make_row({"name": "acf", "tool": "unary"}, [])
    with tempfile.TemporaryDirectory() as directory:
      path = Path(directory) / "runs.csv"
      aggregate_rq3.write_rows(path, run_rq3.CSV_COLUMNS, [row])
      expected = {("test", "input.dot", "spds"): {
          "binary_sha256": "3" * 64, "extra_environment": "{}"}}
      self.assertEqual(run_rq3.prepare_csv(path, True, expected), set())
      with mock.patch.object(config, "REQUIRE_BINARY_IDENTITY", True):
        before = path.read_bytes()
        with self.assertRaisesRegex(RuntimeError, "different binary"):
          run_rq3.prepare_csv(path, True, expected)
        self.assertEqual(path.read_bytes(), before)

  @unittest.skipUnless(os.environ.get("RQ3_INTEGRATION_TESTS") == "1",
                       "set RQ3_INTEGRATION_TESTS=1 after building the tool")
  def test_live_measure_capture_and_aggregate(self) -> None:
    """Exercise the real binary, logs, artifacts and both suite schemas."""
    fixture = config.REPOSITORY_ROOT / "tests/regress/CFL/InterleavedDyck/SPDS/value-flow.dot"
    binary_hash = run_rq3.sha256(config.INTERLEAVED_DYCK_BINARY)
    with tempfile.TemporaryDirectory() as directory:
      root = Path(directory)
      with (
          mock.patch.object(config, "MEASURED_REPETITIONS", 2),
          mock.patch.object(config, "ARTIFACT_DIRECTORY", root / "artifacts"),
          mock.patch.object(config, "LOG_DIRECTORY", root / "log"),
      ):
        rows = []
        for dataset, analysis in (("taint", "taint"), ("valueflow", "value-flow")):
          benchmark = run_rq3.Benchmark(dataset, analysis, fixture.parent, fixture)
          for experiment in run_rq3.validate_configuration():
            if not config.supports_analysis(experiment, analysis):
              continue
            reused = None
            if experiment["name"] == "acf":
              from rq1 import run_rq1
              reused = []
              for index in range(config.MEASURED_REPETITIONS):
                result = run_rq1.run_once(run_rq3.make_command(benchmark, experiment, capture=False),
                                          root / f"rq1-{dataset}-{index}.log", settings=config)
                reused.append(run_rq3.RunResult(result.status, result.exit_code, result.wall_time_seconds,
                                               result.stdout, result.stderr, result.stats, result.log_path))
            runs, artifacts, capture = run_rq3.run_experiment(benchmark, experiment, reused_results=reused)
            self.assertEqual([r.status for r in runs], ["success", "success"])
            self.assertEqual(capture.status, "success", capture.stderr)
            row = run_rq3.create_row(benchmark, experiment, runs, artifacts, capture, {
                "input_sha256": run_rq3.sha256(fixture), "binary_sha256": binary_hash,
                "extra_environment": "{}"})
            rows.append(row)
            if experiment["name"] == "spds":
              expected = ("-9 8\n8 -9\n" if analysis == "value-flow" else
                          "-9 8\n-9 10\n-9 11\n1 3\n3 1\n8 -9\n8 10\n"
                          "8 11\n10 -9\n10 8\n10 11\n11 -9\n11 8\n11 10\n")
              count = "2" if analysis == "value-flow" else "14"
              self.assertEqual(artifacts[0].read_text(), expected)
              self.assertEqual(capture.stats["candidate-pairs"], count)
              engine_analysis = (
                  "value-flow" if analysis == "value-flow"
                  else "nonreflexive-taint"
              )
              self.assertEqual(capture.stats["analysis"], engine_analysis)
              for run in runs:
                self.assertEqual(run.stats["candidate-pairs"], count)
                self.assertNotIn("pair:", run.stdout)
                self.assertIn("process peak RSS (bytes):", run.log_path.read_text())
        csv_path = root / "runs.csv"
        aggregate_rq3.write_rows(csv_path, run_rq3.CSV_COLUMNS, rows)
        args = mock.Mock(runs=csv_path, methods=root / "methods.csv", bounds=root / "bounds.csv")
        with mock.patch.object(aggregate_rq3, "parse_arguments", return_value=args):
          self.assertEqual(aggregate_rq3.main(), 0)
        with args.methods.open() as stream:
          methods = list(csv.DictReader(stream))
        with args.bounds.open() as stream:
          bounds = list(csv.DictReader(stream))
        self.assertEqual(len(methods), len(rows))
        self.assertEqual(len(bounds), sum(row["experiment"] != "acf" for row in rows))
        for spds in (row for row in bounds if row["experiment"] == "spds"):
          count = "2" if spds["dataset"] == "valueflow" else "14"
          self.assertEqual(spds["typed_upper_size"], count)
          self.assertEqual(spds["intersection_size"], count)
          self.assertEqual(spds["newly_excluded_size"], "0")
        # SPDS is now required for both suites.
        aggregate_rq3.write_rows(csv_path, run_rq3.CSV_COLUMNS,
                                 [row for row in rows if row["experiment"] != "spds"])
        with mock.patch.object(aggregate_rq3, "parse_arguments", return_value=args):
          self.assertEqual(aggregate_rq3.main(), 1)

  @unittest.skipUnless(os.environ.get("RQ3_INTEGRATION_TESTS") == "1",
                       "set RQ3_INTEGRATION_TESTS=1 after building the tool")
  def test_nonreflexive_taint_matches_cfl_intersection(self) -> None:
    binary = str(config.INTERLEAVED_DYCK_BINARY)

    def pairs(path, args, prefix):
      completed = subprocess.run([binary, *args, str(path)], check=True,
                                 capture_output=True, text=True, timeout=30)
      return {tuple(map(int, pair)) for pair in re.findall(
          rf"^{prefix}(-?\d+) (-?\d+)$", completed.stdout, re.MULTILINE)}

    rng = random.Random(20261105)
    labels = ["normal"] + [f"{kind}--{i}" for kind in ("op", "cp", "ob", "cb") for i in (0, 1)]
    with tempfile.TemporaryDirectory() as directory:
      graphs = []
      for number in range(80):
        path = Path(directory) / f"{number}.dot"
        lines = [f'{rng.randrange(-3, 4)} -> {rng.randrange(-3, 4)} [label="{rng.choice(labels)}"];'
                 for _ in range(15)]
        path.write_text("digraph G {\n" + "\n".join(lines) + "\n}\n")
        graphs.append(path)
      graphs += [config.REPOSITORY_ROOT / "benchmarks/real-world/CFL/InterleavedDyck/taint" / f"{name}.dot"
                 for name in ("loozfon", "faketaobao")]
      for graph in graphs:
        with self.subTest(graph=graph.name):
          expected = pairs(
              graph,
              ["spds", "--analysis", "nonreflexive-taint",
               "--all-pairs", "--pairs"],
              "pair: ",
          )
          self.assertFalse(any(source == target for source, target in expected))
          cfl = pairs(graph, ["staged-bounds", "--analysis", "taint", "--method", "intersection", "--print-result"], "")
          self.assertEqual(expected, cfl)
          for direction in ("post", "pre"):
            actual = pairs(
                graph,
                ["spds", "--analysis", "nonreflexive-taint",
                 "--all-pairs", "--direction", direction, "--pairs"],
                "pair: ",
            )
            self.assertEqual(actual, expected)

  def make_row(
      self,
      experiment: dict[str, object],
      files: list[Path],
      *,
      statuses: list[str] | None = None,
      configured: int = 1,
  ) -> dict[str, str]:
    statuses = statuses or ["success"]
    successes = sum(status == "success" for status in statuses)
    stored = [str(path) for path in files]
    hashes = {str(path): run_rq3.sha256(path) for path in files}
    row = {column: "" for column in run_rq3.CSV_COLUMNS}
    row.update(
        {
            "dataset": "test",
            "analysis": "value-flow",
            "dot_name": "input.dot",
            "dot_path": "input.dot",
            "input_sha256": "1" * 64,
            "binary_sha256": "2" * 64,
            "extra_environment": "{}",
            "experiment": str(experiment["name"]),
            "tool": str(experiment["tool"]),
            "configuration": json_cell(
                {key: value for key, value in experiment.items() if key != "enabled"}
            ),
            "warmup_repetitions": str(config.WARMUP_REPETITIONS),
            "configured_repetitions": str(configured),
            "completed_repetitions": str(len(statuses)),
            "successful_repetitions": str(successes),
            "statuses": json_cell(statuses),
            "exit_codes": json_cell([0 if value == "success" else -9 for value in statuses]),
            "time_ms": json_cell([1 if value == "success" else None for value in statuses]),
            "average_time_ms": "1",
            "median_time_ms": "1",
            "peak_rss_bytes": json_cell([100 if value == "success" else None for value in statuses]),
            "average_peak_rss_bytes": "100",
            "median_peak_rss_bytes": "100",
            "wall_time_seconds": json_cell([0.1 if value == "success" else None for value in statuses]),
            "average_wall_time_seconds": "0.1",
            "median_wall_time_seconds": "0.1",
            "stats_per_run": json_cell([
                {"Time (ms)": "1", "process peak RSS (bytes)": "100"}
                for _ in statuses
            ]),
            "timeout_seconds": str(config.TIMEOUT_SECONDS),
            "memory_limit_bytes": str(int(config.MEMORY_LIMIT_GIB * 1024**3)),
            "relation_files": json_cell(stored),
            "relation_sha256": json_cell(hashes),
            "log_files": "[]",
            "capture_command": "[]",
            "capture_status": "success" if files else "not-run",
        }
    )
    directory = tempfile.TemporaryDirectory()
    self.addCleanup(directory.cleanup)
    logs = []
    for index in range(len(statuses)):
      path = Path(directory.name) / f"{index}.log"
      path.write_text("fixture log")
      logs.append(str(path))
    row["log_files"] = json_cell(logs)
    row["log_sha256"] = json_cell({name: run_rq3.sha256(Path(name)) for name in logs})
    return row

  def test_success_requires_every_configured_repetition(self) -> None:
    row = self.make_row(
        {"name": "union-dyck", "tool": "staged", "artifact_kind": "union"},
        [],
        statuses=["success", "timeout"],
        configured=2,
    )
    with mock.patch.object(config, "MEASURED_REPETITIONS", 2):
      self.assertFalse(aggregate_rq3.successful(row))
      self.assertEqual(aggregate_rq3.method_status(row), "partial")

  def test_resume_retries_required_failure_but_retains_optional_timeout(self) -> None:
    required = {
        "name": "union-dyck",
        "tool": "staged",
        "artifact_kind": "union",
        "enabled": True,
    }
    optional = {
        "name": "mcfl-plus-d3",
        "tool": "mcfl",
        "artifact_kind": "mcfl",
        "family": "plus",
        "dimension": 3,
        "required": False,
        "enabled": True,
    }
    with tempfile.TemporaryDirectory() as directory:
      csv_path = Path(directory) / "runs.csv"
      rows = [
          self.make_row(required, [], statuses=["timeout"]),
          self.make_row(optional, [], statuses=["timeout"]),
      ]
      with csv_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=run_rq3.CSV_COLUMNS)
        writer.writeheader()
        writer.writerows(rows)
      with mock.patch.object(config, "EXPERIMENTS", [required, optional]):
        with mock.patch.object(config, "MEASURED_REPETITIONS", 1):
          completed = run_rq3.prepare_csv(csv_path, resume=True)
      self.assertEqual(completed, {("test", "input.dot", "mcfl-plus-d3")})
      with csv_path.open("r", encoding="utf-8", newline="") as stream:
        retained = list(csv.DictReader(stream))
      self.assertEqual([row["experiment"] for row in retained], ["mcfl-plus-d3"])

  def test_missing_acf_makes_aggregation_incomplete(self) -> None:
    experiments: list[dict[str, object]] = [
        {"name": "regularization", "tool": "staged", "artifact_kind": "upper"},
        {"name": "mutual-refinement", "tool": "staged", "artifact_kind": "upper"},
        {"name": "acf", "tool": "unary", "artifact_kind": "components"},
    ]
    with tempfile.TemporaryDirectory() as directory:
      root = Path(directory)
      files: dict[str, list[Path]] = {}
      for name, names in {
          "regularization": ["upper.pairs"],
          "mutual-refinement": ["upper.pairs"],
      }.items():
        base = root / name
        base.mkdir()
        files[name] = []
        for filename in names:
          path = base / filename
          path.write_text("", encoding="utf-8")
          files[name].append(path)
      rows = [
          self.make_row(item, files[str(item["name"])])
          for item in experiments[:2]
      ]
      runs = root / "runs.csv"
      with runs.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=run_rq3.CSV_COLUMNS)
        writer.writeheader()
        writer.writerows(rows)
      arguments = [
          "aggregate_rq3.py",
          "--runs",
          str(runs),
          "--bounds",
          str(root / "bounds.csv"),
          "--methods",
          str(root / "methods.csv"),
      ]
      with (
          mock.patch.object(config, "EXPERIMENTS", experiments),
          mock.patch.object(config, "MEASURED_REPETITIONS", 1),
          mock.patch.object(sys, "argv", arguments),
      ):
        self.assertEqual(aggregate_rq3.main(), 1)
      with (root / "bounds.csv").open("r", encoding="utf-8", newline="") as stream:
        bounds = list(csv.DictReader(stream))
      self.assertEqual(bounds[0]["status"], "incomplete")
      self.assertEqual(len(bounds), 2)
      self.assertEqual(bounds[0]["intersection_size"], "")

  def test_both_upper_bounds_measure_acf_tightening(self) -> None:
    experiments: list[dict[str, object]] = [
        {"name": "regularization", "tool": "staged", "artifact_kind": "upper"},
        {"name": "mutual-refinement", "tool": "staged", "artifact_kind": "upper"},
        {"name": "acf", "tool": "unary", "artifact_kind": "components"},
    ]
    with tempfile.TemporaryDirectory() as directory:
      root = Path(directory)
      contents = {
          "regularization": {"upper.pairs": "1 2\n2 3\n3 4\n"},
          "mutual-refinement": {"upper.pairs": "1 2\n2 3\n"},
          "acf": {"components.map": "1 7\n2 7\n3 7\n4 8\n"},
      }
      files: dict[str, list[Path]] = {}
      for experiment_name, artifacts in contents.items():
        base = root / experiment_name
        base.mkdir()
        files[experiment_name] = []
        for filename, text in artifacts.items():
          path = base / filename
          path.write_text(text, encoding="utf-8")
          files[experiment_name].append(path)
      rows = [
          self.make_row(experiment, files[str(experiment["name"])])
          for experiment in experiments
      ]
      runs = root / "runs.csv"
      with runs.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=run_rq3.CSV_COLUMNS)
        writer.writeheader()
        writer.writerows(rows)
      arguments = [
          "aggregate_rq3.py",
          "--runs",
          str(runs),
          "--bounds",
          str(root / "bounds.csv"),
          "--methods",
          str(root / "methods.csv"),
      ]
      with (
          mock.patch.object(config, "EXPERIMENTS", experiments),
          mock.patch.object(config, "MEASURED_REPETITIONS", 1),
          mock.patch.object(sys, "argv", arguments),
      ):
        self.assertEqual(aggregate_rq3.main(), 0)
      with (root / "bounds.csv").open("r", encoding="utf-8", newline="") as stream:
        bounds = list(csv.DictReader(stream))
      self.assertEqual(len(bounds), 2)
      row = bounds[0]
      self.assertEqual(row["experiment"], "regularization")
      self.assertEqual(row["status"], "success")
      self.assertEqual(row["typed_upper_size"], "3")
      self.assertEqual(row["intersection_size"], "2")
      self.assertEqual(row["newly_excluded_size"], "1")
      self.assertAlmostEqual(float(row["upper_reduction"]), 1.0 / 3.0)
      self.assertEqual(bounds[1]["experiment"], "mutual-refinement")
      self.assertEqual(bounds[1]["typed_upper_size"], "2")
      self.assertEqual(bounds[1]["intersection_size"], "2")
      self.assertEqual(bounds[1]["newly_excluded_size"], "0")

  def test_capture_printed_upper_bound_and_reject_truncation(self) -> None:
    with tempfile.TemporaryDirectory() as directory:
      path = Path(directory) / "upper.pairs"
      output = (
          "mutual-refinement upper bound: 2\nTime (ms): 1\n"
          "1 2\n3 4\nprocess peak RSS (bytes): 1024\n"
      )
      run_rq3.capture_upper_pairs(output.splitlines(), path)
      self.assertEqual(aggregate_rq3.load_pairs(path), {(1, 2), (3, 4)})
      with self.assertRaisesRegex(ValueError, "captured 1"):
        run_rq3.capture_upper_pairs(output.replace("3 4\n", "").splitlines(), path)
      run_rq3.capture_upper_pairs(["regularization: 0", "Time (ms): 0"], path)
      self.assertEqual(aggregate_rq3.load_pairs(path), set())

  def test_measurement_summary_rejects_missing_and_changed_samples(self) -> None:
    row = self.make_row({"name": "regularization", "tool": "staged"}, [])
    self.assertTrue(run_rq3.valid_measurements(row))
    for field, value in (("time_ms", "[null]"), ("median_time_ms", "2"),
                         ("peak_rss_bytes", "[]"), ("stats_per_run", "[]")):
      with self.subTest(field=field):
        self.assertFalse(run_rq3.valid_measurements({**row, field: value}))

  def test_exit_zero_without_required_statistics_is_not_success(self) -> None:
    with tempfile.TemporaryDirectory() as directory:
      result = run_rq3.run_once(
          [sys.executable, "-c", "print('regularization: 0')"],
          Path(directory) / "run.log",
      )
      self.assertEqual(result.status, "stats-error")
      self.assertIn("Status: stats-error", result.log_path.read_text())

  def test_sigkill_alone_does_not_prove_memory_exhaustion(self) -> None:
    self.assertFalse(run_rq3.is_memory_failure("", -signal.SIGKILL))
    self.assertTrue(run_rq3.is_memory_failure("std::bad_alloc", 1))

  def test_failed_attempt_keeps_wall_time_without_partial_medians(self) -> None:
    experiment = next(item for item in config.EXPERIMENTS if item["name"] == "acf")
    benchmark = run_rq3.Benchmark(
        "test", "taint", config.REPOSITORY_ROOT, config.REPOSITORY_ROOT / "input.dot"
    )
    results = [
        run_rq3.RunResult("success", 0, 0.1, "", "",
                         {"Time (ms)": "1", "process peak RSS (bytes)": "100"},
                         Path("/tmp/success.log")),
        run_rq3.RunResult("timeout", -9, 30.0, "", "", {}, Path("/tmp/timeout.log")),
    ]
    with tempfile.TemporaryDirectory() as directory, mock.patch.object(config, "MEASURED_REPETITIONS", 2):
      for index, result in enumerate(results):
        result.log_path = Path(directory) / f"{index}.log"
        result.log_path.write_text("fixture log")
      row = run_rq3.create_row(benchmark, experiment, results, [], None, {})
    self.assertEqual(json.loads(row["wall_time_seconds"]), [0.1, 30.0])
    self.assertEqual(json.loads(row["time_ms"]), [1, None])
    self.assertEqual(json.loads(row["peak_rss_bytes"]), [100, None])
    self.assertIsNone(row["median_wall_time_seconds"])
    self.assertIsNone(row["median_time_ms"])
    self.assertEqual(row["capture_status"], "not-run")

  def test_capture_is_separate_and_artifact_errors_are_logged(self) -> None:
    experiment = next(
        item for item in config.EXPERIMENTS
        if item["name"] == "mutual-refinement"
    )
    benchmark = run_rq3.Benchmark(
        "test", "taint", config.REPOSITORY_ROOT, config.REPOSITORY_ROOT / "input.dot"
    )
    for truncated in (False, True):
      with self.subTest(truncated=truncated), tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        calls = []

        def fake_run(command, log_path, **kwargs):
          calls.append(command)
          stdout = (
              "mutual-refinement upper bound: 1\n"
              "Time (ms): 1\nprocess peak RSS (bytes): 100\n"
          )
          if "--print-result" in command:
            output = stdout.replace("Time (ms)", "1 2\nTime (ms)")
            if truncated:
              output = output.replace(
                  "mutual-refinement upper bound: 1",
                  "mutual-refinement upper bound: 2",
              )
            Path(command[command.index("-o") + 1]).write_text(output)
            stdout = ""
          log_path.parent.mkdir(parents=True, exist_ok=True)
          log_path.write_text(stdout)
          return run_rq3.RunResult(
              "success", 0, 0.1, stdout, "", run_rq3.parse_stats(stdout), log_path
          )

        with (
            mock.patch.object(config, "MEASURED_REPETITIONS", 2),
            mock.patch.object(config, "ARTIFACT_DIRECTORY", root / "artifacts"),
            mock.patch.object(config, "LOG_DIRECTORY", root / "log"),
            mock.patch.object(run_rq3, "run_once", side_effect=fake_run),
        ):
          results, files, capture = run_rq3.run_experiment(benchmark, experiment)
          row = run_rq3.create_row(benchmark, experiment, results, files, capture, {})
        self.assertEqual(len(results), 2)
        self.assertEqual(len(calls), 3)
        self.assertTrue(all("--print-result" not in command for command in calls[:2]))
        self.assertIn("--print-result", calls[2])
        expected = "artifact-error" if truncated else "success"
        self.assertEqual(capture.status, expected)
        self.assertEqual(row["capture_status"], expected)
        self.assertIn(f"Status: {expected}", capture.log_path.read_text())
        self.assertEqual(len(files), 0 if truncated else 1)

  def test_capture_failure_does_not_invalidate_performance(self) -> None:
    row = self.make_row({"name": "regularization", "tool": "staged"}, [])
    row["capture_status"] = "timeout"
    with mock.patch.object(config, "MEASURED_REPETITIONS", 1):
      self.assertTrue(aggregate_rq3.measurement_successful(row))
      self.assertEqual(aggregate_rq3.method_status(row), "success")
      self.assertFalse(aggregate_rq3.successful(row))
      with mock.patch.object(config, "TIMEOUT_SECONDS", config.TIMEOUT_SECONDS + 1):
        self.assertFalse(aggregate_rq3.measurement_successful(row))
        self.assertEqual(aggregate_rq3.method_status(row), "protocol-mismatch")

  def test_selected_resume_preserves_unselected_failed_records(self) -> None:
    experiment = {"name": "regularization", "tool": "staged", "artifact_kind": "upper"}
    selected = self.make_row(experiment, [], statuses=["timeout"])
    other = {**selected, "dot_path": "other.dot", "dot_name": "other.dot"}
    expected = {("test", "input.dot", "regularization"): {"input_sha256": selected["input_sha256"]}}
    with tempfile.TemporaryDirectory() as directory:
      path = Path(directory) / "runs.csv"
      aggregate_rq3.write_rows(path, run_rq3.CSV_COLUMNS, [selected, other])
      with mock.patch.object(config, "EXPERIMENTS", [experiment]):
        self.assertEqual(run_rq3.prepare_csv(path, True, expected), set())
      with path.open(newline="") as stream:
        retained = list(csv.DictReader(stream))
      self.assertEqual(retained, [other])

  def test_resume_rejects_changed_execution_identity(self) -> None:
    row = self.make_row({"name": "regularization", "tool": "staged"}, [])
    key = ("test", "input.dot", "regularization")
    with tempfile.TemporaryDirectory() as directory:
      path = Path(directory) / "runs.csv"
      aggregate_rq3.write_rows(path, run_rq3.CSV_COLUMNS, [row])
      for field in ("input_sha256", "analysis", "command", "extra_environment"):
        with self.subTest(field=field), self.assertRaisesRegex(
            RuntimeError, "changed|environment"
        ):
          run_rq3.prepare_csv(path, True, {key: {field: "changed"}})
      self.assertEqual(run_rq3.prepare_csv(
          path, True, {key: {"binary_sha256": "3" * 64}}), set())

  def test_aggregation_rejects_mixed_inputs_binaries_and_clients(self) -> None:
    row = self.make_row({"name": "regularization", "tool": "staged"}, [])
    other = {**row, "experiment": "acf"}
    aggregate_rq3.validate_run_identities([row, other])
    aggregate_rq3.validate_run_identities(
        [row, {**other, "binary_sha256": "3" * 64}]
    )
    for field, value in (("input_sha256", "3" * 64),
                         ("analysis", "taint"), ("extra_environment", '{"X":"1"}')):
      with self.subTest(field=field), self.assertRaisesRegex(ValueError, "mix"):
        aggregate_rq3.validate_run_identities([row, {**other, field: value}])
    with mock.patch.object(config, "REQUIRE_BINARY_IDENTITY", True), self.assertRaisesRegex(
        ValueError, "mix"
    ):
      aggregate_rq3.validate_run_identities(
          [row, {**other, "binary_sha256": "3" * 64}]
      )

  def test_timeout_stops_repetitions_and_memory_limit_always_stops(self) -> None:
    experiment = next(e for e in config.EXPERIMENTS if e["name"] == "mutual-refinement")
    benchmark = run_rq3.Benchmark("test", "taint", config.REPOSITORY_ROOT,
                                 config.REPOSITORY_ROOT / "input.dot")
    for statuses, expected_count in (
        (["timeout"] * 5, 1),
        (["timeout"] + ["success"] * 4, 1),
        (["memory-limit"] + ["success"] * 4, 1),
        (["timeout", "memory-limit"] + ["success"] * 3, 1),
    ):
      with self.subTest(statuses=statuses), tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        samples = [run_rq3.RunResult(status, 0 if status == "success" else -9,
                                     0.1, "", "", {}, root / f"{i}.log")
                   for i, status in enumerate(statuses)]
        with (mock.patch.object(config, "MEASURED_REPETITIONS", 5),
              mock.patch.object(config, "WARMUP_REPETITIONS", 0),
              mock.patch.object(config, "SKIP_BENCHMARK_AFTER_TIMEOUT", True),
              mock.patch.object(config, "ARTIFACT_DIRECTORY", root / "artifacts"),
              mock.patch.object(run_rq3, "run_once", side_effect=samples) as run):
          results, artifacts, capture = run_rq3.run_experiment(benchmark, experiment)
        self.assertEqual(run.call_count, expected_count)
        self.assertEqual([r.status for r in results], statuses[:expected_count])
        self.assertEqual(artifacts, [])
        self.assertIsNone(capture)

    samples = [run_rq3.RunResult("timeout", -9, 0.1, "", "", {}, Path("/tmp/timeout.log"))
               for _ in range(5)]
    with (mock.patch.object(config, "MEASURED_REPETITIONS", 5),
          mock.patch.object(config, "WARMUP_REPETITIONS", 0),
          mock.patch.object(config, "SKIP_BENCHMARK_AFTER_TIMEOUT", False),
          mock.patch.object(run_rq3, "run_once", side_effect=samples) as run):
      results, _, _ = run_rq3.run_experiment(benchmark, experiment)
    self.assertEqual(run.call_count, 5)
    self.assertEqual([result.status for result in results], ["timeout"] * 5)

  def test_timeout_skips_remaining_methods_for_same_benchmark(self) -> None:
    experiments = [
        next(item for item in config.EXPERIMENTS if item["name"] == name)
        for name in ("spds", "mutual-refinement")
    ]
    timeout = run_rq3.RunResult("timeout", -9, 0.1, "", "", {}, Path("/tmp/timeout.log"))
    success = run_rq3.RunResult("success", 0, 0.1, "", "", {}, Path("/tmp/success.log"))
    calls = []

    def fake_run_experiment(benchmark, experiment, *, reused_results=None):
      calls.append((benchmark.dot.name, experiment["name"]))
      result = timeout if benchmark.dot.name == "a.dot" else success
      return [result], [], None

    args = mock.Mock(dry_run=False, restart=False, dataset=None,
                     benchmark=None, experiment=None)
    with tempfile.TemporaryDirectory() as directory:
      root = Path(directory)
      binary = root / "solver"
      binary.write_text("fixture")
      binary.chmod(0o755)
      benchmarks = []
      for name in ("a.dot", "b.dot"):
        dot = root / name
        dot.write_text("fixture")
        benchmarks.append(run_rq3.Benchmark("test", "taint", root, dot))
      with (
          mock.patch.object(config, "REPOSITORY_ROOT", root),
          mock.patch.object(config, "OUTPUT_CSV", root / "runs.csv"),
          mock.patch.object(config, "SKIP_BENCHMARK_AFTER_TIMEOUT", True),
          mock.patch.object(run_rq3, "parse_arguments", return_value=args),
          mock.patch.object(run_rq3, "validate_configuration", return_value=experiments),
          mock.patch.object(run_rq3, "discover_benchmarks", return_value=benchmarks),
          mock.patch.object(run_rq3, "binary_for", return_value=binary),
          mock.patch.object(run_rq3, "sha256", return_value="0" * 64),
          mock.patch.object(run_rq3, "prepare_csv", return_value=set()),
          mock.patch.object(run_rq3, "create_row", return_value={}),
          mock.patch.object(run_rq3, "append_row"),
          mock.patch.object(run_rq3.provenance, "check_files"),
          mock.patch.object(run_rq3, "run_experiment", side_effect=fake_run_experiment),
      ):
        self.assertEqual(run_rq3.main(), 1)
    self.assertEqual(calls, [
        ("a.dot", "spds"),
        ("b.dot", "spds"),
        ("b.dot", "mutual-refinement"),
    ])

  def test_timeout_budget_includes_process_launch(self) -> None:
    process = mock.Mock()
    process.communicate.return_value = ("Time (ms): 1\nprocess peak RSS (bytes): 100\n", "")
    process.returncode = 0
    with (
        tempfile.TemporaryDirectory() as directory,
        mock.patch.object(config, "TIMEOUT_SECONDS", 1),
        mock.patch.object(run_rq3.subprocess, "Popen", return_value=process),
        mock.patch.object(run_rq3.time, "perf_counter", side_effect=[10.0, 10.4, 10.8]),
    ):
      result = run_rq3.run_once(["solver"], Path(directory) / "run.log")
    self.assertAlmostEqual(process.communicate.call_args.kwargs["timeout"], 0.6)
    self.assertAlmostEqual(result.wall_time_seconds, 0.8)
    self.assertEqual(result.status, "success")

  def test_timeout_remains_timeout_if_process_exits_before_kill(self) -> None:
    process = mock.Mock()
    process.communicate.side_effect = [subprocess.TimeoutExpired("solver", 1), ("", "")]
    process.returncode = -9
    with (
        tempfile.TemporaryDirectory() as directory,
        mock.patch.object(run_rq3.subprocess, "Popen", return_value=process),
        mock.patch.object(run_rq3.os, "killpg", side_effect=ProcessLookupError),
    ):
      result = run_rq3.run_once(["solver"], Path(directory) / "run.log")
    self.assertEqual(result.status, "timeout")

  def test_relation_hash_is_verified(self) -> None:
    experiment = {
        "name": "union-dyck",
        "tool": "staged",
        "artifact_kind": "union",
    }
    with tempfile.TemporaryDirectory() as directory:
      path = Path(directory) / "union.pairs"
      path.write_text("1 2\n", encoding="utf-8")
      row = self.make_row(experiment, [path])
      path.write_text("2 3\n", encoding="utf-8")
      with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
        aggregate_rq3.relation_path(row, "union.pairs")


if __name__ == "__main__":
  unittest.main()
