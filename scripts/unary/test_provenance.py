"""Integrity and cross-RQ reuse regression tests; all outputs are temporary."""
import argparse
import csv
import io
import json
import tempfile
import unittest
from contextlib import ExitStack, redirect_stdout
from pathlib import Path
from unittest import mock

import provenance
from rq1 import run_rq1 as rq1
from rq2 import run_rq2 as rq2
from rq3 import run_rq3 as rq3


class ProvenanceTest(unittest.TestCase):
  def setUp(self):
    self.stack = ExitStack()
    self.addCleanup(self.stack.close)
    self.root = Path(self.stack.enter_context(tempfile.TemporaryDirectory(dir=provenance.REPOSITORY_ROOT)))
    self.stack.enter_context(redirect_stdout(io.StringIO()))
    self.dot = self.root / 'input.dot'
    fixture = provenance.REPOSITORY_ROOT / 'tests/regress/CFL/InterleavedDyck/SPDS/value-flow.dot'
    self.dot.write_bytes(fixture.read_bytes())
    self.benchmark = rq1.Benchmark('fixture', self.root, self.dot)
    self.benchmark3 = rq3.Benchmark('fixture', 'value-flow', self.root, self.dot)
    for runner in (rq1, rq2, rq3):
      name = runner.__name__.split('.')[0]
      for key, value in dict(MEASURED_REPETITIONS=2, WARMUP_REPETITIONS=0,
          OUTPUT_CSV=self.root / name / 'runs.csv', LOG_DIRECTORY=self.root / name / 'log').items():
        self.stack.enter_context(mock.patch.object(runner.config, key, value))
      args = argparse.Namespace(dry_run=False, restart=False, dataset=None, benchmark=None, experiment=['acf'])
      self.stack.enter_context(mock.patch.object(runner, 'parse_arguments', return_value=args))
      benchmarks = [self.benchmark3] if runner is rq3 else [self.benchmark]
      self.stack.enter_context(mock.patch.object(runner, 'discover_benchmarks', return_value=benchmarks))
    self.stack.enter_context(mock.patch.object(rq3.config, 'RQ1_OUTPUT_CSV', rq1.config.OUTPUT_CSV))
    self.stack.enter_context(mock.patch.object(rq3.config, 'ARTIFACT_DIRECTORY', self.root / 'artifacts'))
    self.experiment = next(e for e in rq1.config.EXPERIMENTS if e['name'] == 'adaptive')
    self.stack.enter_context(mock.patch.object(rq1.config, 'EXPERIMENTS', [self.experiment]))

  def source(self):
    self.assertEqual(rq1.main(), 0)
    with rq1.config.OUTPUT_CSV.open() as stream:
      return next(csv.DictReader(stream))

  def expected(self):
    identity = provenance.identity(self.dot, rq1.config.BINARY_PATH,
        rq1.make_command(rq1.config.BINARY_PATH, self.benchmark, self.experiment), rq1.config)
    return {('fixture', str(self.dot.relative_to(provenance.REPOSITORY_ROOT))): identity}

  def test_full_workflow_reuses_exact_samples_and_resumes_without_runs(self):
    source = self.source()
    self.assertEqual(rq2.main(), 0)
    with mock.patch.object(rq3, 'run_once', wraps=rq3.run_once) as run:
      self.assertEqual(rq3.main(), 0)
      self.assertEqual(run.call_count, 1)
      self.assertIn('--dump-components', run.call_args.args[0])
    with rq3.config.OUTPUT_CSV.open() as stream:
      row = next(csv.DictReader(stream))
    for field in ('time_ms', 'peak_rss_bytes', 'wall_time_seconds', 'stats_per_run', 'log_files'):
      self.assertEqual(json.loads(row[field]), json.loads(source[field]))
    self.assertEqual(row['source_row_sha256'], source['row_sha256'])
    rq3.validate_performance_source(row)
    with self.assertRaisesRegex(ValueError, 'differ from'):
      rq3.validate_performance_source({**row, 'time_ms': '[999,999]'})
    for runner in (rq1, rq2, rq3):
      before = runner.config.OUTPUT_CSV.read_bytes()
      with mock.patch.object(runner if runner is not rq2 else rq1, 'run_once') as run:
        self.assertEqual(runner.main(), 0)
        run.assert_not_called()
      self.assertEqual(runner.config.OUTPUT_CSV.read_bytes(), before)

  def test_missing_source_does_not_create_rq3_outputs(self):
    with mock.patch.object(rq3, 'run_once') as run, self.assertRaises(FileNotFoundError):
      rq3.main()
    run.assert_not_called()
    self.assertFalse(rq3.config.OUTPUT_CSV.exists())

  def test_input_change_and_log_damage_reject_resume_and_reuse(self):
    row = self.source()
    before = rq1.config.OUTPUT_CSV.read_bytes()
    self.dot.write_text(self.dot.read_text() + '\n')
    for runner in (rq1, rq3):
      with self.assertRaisesRegex(RuntimeError, 'input_sha256'):
        runner.main()
    self.dot.write_bytes((provenance.REPOSITORY_ROOT / 'tests/regress/CFL/InterleavedDyck/SPDS/value-flow.dot').read_bytes())
    log = provenance.stored_path(json.loads(row['log_files'])[0])
    log.write_text(log.read_text() + '\nchanged')
    for runner in (rq1, rq3):
      with self.assertRaisesRegex(RuntimeError, 'log SHA256'):
        runner.main()
    self.assertEqual(rq1.config.OUTPUT_CSV.read_bytes(), before)
    self.assertFalse(rq3.config.OUTPUT_CSV.exists())

  def test_reuse_rejects_changed_conditions_and_csv_cells(self):
    row = self.source()
    with mock.patch.object(provenance, 'REQUIRE_BINARY_IDENTITY', True):
      for field, value in [('binary_sha256', '0'*64), ('extra_environment', '{"X":"1"}'),
                           ('configured_repetitions', '3'), ('warmup_repetitions', '1'),
                           ('timeout_seconds', '1'), ('memory_limit_bytes', '1'), ('command', 'wrong')]:
        expected = self.expected()
        next(iter(expected.values()))[field] = value
        with self.subTest(field=field), self.assertRaisesRegex(RuntimeError, field):
          rq3.load_rq1_adaptive_results(rq1.config.OUTPUT_CSV, expected)
    expected = self.expected()
    next(iter(expected.values()))['binary_sha256'] = '0' * 64
    rq3.load_rq1_adaptive_results(rq1.config.OUTPUT_CSV, expected)
    row['time_ms'] = '[1,2]'
    with self.assertRaisesRegex(RuntimeError, 'row SHA256'):
      provenance.validate(row)

  def test_missing_duplicate_and_failed_source_rows_are_rejected(self):
    row = self.source()
    path = rq1.config.OUTPUT_CSV
    def write(rows):
      with path.open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=rq1.CSV_COLUMNS)
        writer.writeheader()
        writer.writerows(rows)
    for rows, message in [([], 'missing RQ1'), ([row, row], 'duplicate RQ1')]:
      write(rows)
      with self.assertRaisesRegex(RuntimeError, message):
        rq3.main()
      self.assertFalse(rq3.config.OUTPUT_CSV.exists())
    row['statuses'] = '["success","timeout"]'
    row['successful_repetitions'] = '1'
    provenance.seal(row)
    write([row])
    with self.assertRaisesRegex(RuntimeError, 'all repetitions'):
      rq3.main()

  def test_rq2_log_and_configuration_changes_stop_before_writing(self):
    self.assertEqual(rq2.main(), 0)
    before = rq2.config.OUTPUT_CSV.read_bytes()
    with mock.patch.object(rq2.config, 'WARMUP_REPETITIONS', 1), self.assertRaisesRegex(RuntimeError, 'warmup'):
      rq2.main()
    with rq2.config.OUTPUT_CSV.open() as stream:
      row = next(csv.DictReader(stream))
    provenance.stored_path(json.loads(row['log_files'])[0]).unlink()
    with self.assertRaises(RuntimeError):
      rq2.main()
    self.assertEqual(rq2.config.OUTPUT_CSV.read_bytes(), before)

  def test_source_rerun_invalidates_rq3_resume_and_capture_log_is_checked(self):
    self.source()
    self.assertEqual(rq3.main(), 0)
    before = rq3.config.OUTPUT_CSV.read_bytes()
    with rq3.config.OUTPUT_CSV.open() as stream:
      row = next(csv.DictReader(stream))
    capture_log = provenance.stored_path(row['capture_log_file'])
    original_log = capture_log.read_bytes()
    capture_log.write_text('damaged')
    with self.assertRaisesRegex(ValueError, 'capture log'):
      rq3.validate_logs(row)
    capture_log.write_bytes(original_log)
    self.assertEqual(rq1.main(), 0)
    args = argparse.Namespace(dry_run=False, restart=True)
    with mock.patch.object(rq1, 'parse_arguments', return_value=args):
      self.assertEqual(rq1.main(), 0)
    with self.assertRaises((RuntimeError, ValueError)):
      rq3.main()
    self.assertEqual(rq3.config.OUTPUT_CSV.read_bytes(), before)

  def test_old_schema_is_not_rewritten(self):
    for runner in (rq1, rq2):
      path = runner.config.OUTPUT_CSV
      path.parent.mkdir(parents=True, exist_ok=True)
      path.write_text('dataset,dot_path\nold,old.dot\n')
      before = path.read_bytes()
      with self.assertRaisesRegex(RuntimeError, 'header mismatch'):
        runner.main()
      self.assertEqual(path.read_bytes(), before)


if __name__ == '__main__':
  unittest.main()
