"""Verify the bundled analyzer, report driver, and API-spec intrinsic semantics."""
import argparse
from contextlib import closing
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import unittest


class IKOSAnalyzerIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)

    def run_command(self, *args, env=None):
        process = subprocess.run([str(arg) for arg in args], cwd=self.work,
                                 capture_output=True, text=True, timeout=60,
                                 env=env)
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        return process.stdout + process.stderr

    def analyze(self, source, checker):
        database = self.work / 'result.db'
        self.run_command(BIN_DIR / 'lotus-ikos', source, '-o', database,
                         '-a', checker, '--color=no', '--display-times=no')
        with closing(sqlite3.connect(database)) as connection:
            results = connection.execute('SELECT status FROM checks').fetchall()
        self.assertTrue(results, 'Analysis must actually record checks')
        return results, database

    def test_proves_intrinsic_assumption_and_reports(self):
        source = self.work / 'safe.c'
        source.write_text('#include <ikos/analyzer/intrinsic.h>\n'
                          'int main(void) {\n'
                          '  int n = __ikos_nondet_int();\n'
                          '  __ikos_assume(n >= 0 && n <= 3);\n'
                          '  __ikos_assert(n + 1 > 0);\n'
                          '  return 0;\n}\n')
        results, database = self.analyze(source, 'prover')
        self.assertEqual(set(results), {(0,)})
        report = self.run_command(BIN_DIR / 'ikos-report', database, '--color=no',
                                  '--status-filter=safe')
        self.assertIn('safe', report.lower())

    def test_detects_definite_division_by_zero(self):
        source = self.work / 'unsafe.c'
        source.write_text('#include <ikos/analyzer/intrinsic.h>\n'
                          'int main(void) {\n'
                          '  int n = __ikos_nondet_int();\n'
                          '  __ikos_assume(n == 0);\n'
                          '  return 10 / n;\n}\n')
        results, _ = self.analyze(source, 'dbz')
        self.assertIn((2,), results)

    def test_linked_stub_assumptions_reach_caller(self):
        source = self.work / 'caller.c'
        source.write_text('#include <ikos/analyzer/intrinsic.h>\n'
                          'extern int lotus_external(int);\n'
                          'int main(void) {\n'
                          '  int n = lotus_external(4);\n'
                          '  __ikos_assert(n >= 4 && n <= 6);\n'
                          '  return 0;\n}\n')
        include = self.run_command(BIN_DIR / 'ikos-config', '--includedir').strip()
        bc = self.work / 'caller.bc'
        self.run_command(LLVM_BIN / 'clang', '-emit-llvm', '-c', '-g', '-O0',
                         '-D__IKOS__', '-Xclang', '-disable-O0-optnone',
                         '-isystem', include, source, '-o', bc)
        stubs = self.work / 'stubs'
        stubs.mkdir()
        (stubs / 'lotus_external.c').write_text(
            '#include <ikos/analyzer/intrinsic.h>\n'
            'int lotus_external(int p0) {\n'
            '  int r = __ikos_nondet_int();\n'
            '  __ikos_assume(r >= p0 && r <= p0 + 2);\n'
            '  return r;\n}\n')
        linked = self.work / 'linked.bc'
        self.run_command(BIN_DIR / 'lotus-ikos-stub', 'link', '--stub-dir', stubs,
                         '-j', '2', '-o', linked, bc)
        results, _ = self.analyze(linked, 'prover')
        self.assertEqual(set(results), {(0,)})

    def test_compiler_wrappers_and_configuration(self):
        for command in ('ikos', 'ikos-report', 'ikos-view', 'ikos-config',
                        'ikos-scan', 'ikos-scan-extract'):
            with self.subTest(command=command):
                self.assertIn('usage:', self.run_command(BIN_DIR / command, '--help'))
        for command in ('ikos-scan-cc', 'ikos-scan-c++'):
            self.assertIn('clang version 14',
                          self.run_command(BIN_DIR / command, '--version',
                            env=dict(os.environ, IKOS_SCAN_NOTIFIER_FILES=str(
                                self.work / 'notifications'))))
        self.assertIn('3.5', self.run_command(BIN_DIR / 'ikos', '--version'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, required=True)
    parser.add_argument('--llvm-bin', type=Path, required=True)
    options, rest = parser.parse_known_args()
    BIN_DIR = options.bin_dir.resolve()
    LLVM_BIN = options.llvm_bin.resolve()
    unittest.main(argv=[__file__] + rest)
