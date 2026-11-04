"""Exercise the actual Lotus launchers and LLVM tools without network access."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class IKOSApiSpecIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.source = self.work / 'input.c'
        self.source.write_text('extern int lotus_external(int);\n'
                               'int main(void) { return lotus_external(4); }\n')
        self.bc = self.work / 'input.bc'
        self.run_command(LLVM_BIN / 'clang', '-c', '-emit-llvm', '-g',
                         self.source, '-o', self.bc)
        self.stubs = self.work / 'stubs'
        self.stubs.mkdir()
        self.output = self.work / 'linked.bc'

    def run_command(self, *arguments, success=True):
        process = subprocess.run([str(arg) for arg in arguments], cwd=self.work,
                                 text=True, capture_output=True, timeout=30)
        if success:
            self.assertEqual(process.returncode, 0,
                             process.stdout + process.stderr)
        else:
            self.assertNotEqual(process.returncode, 0,
                                process.stdout + process.stderr)
        return process.stdout + process.stderr

    def stub(self, command, *arguments, **kwargs):
        return self.run_command(BIN_DIR / 'lotus-ikos-stub', command, '-j', '1',
                                *arguments, self.bc, **kwargs)

    def test_census_scaffold_and_verified_link(self):
        census = self.run_command(BIN_DIR / 'lotus-ikos-api-census', 'ir',
                                  '-j', '2', self.work)
        self.assertIn('1 external APIs', census)
        missing = (self.work / 'ir-api-census-results' /
                   (self.work.name + '.unmodeled.txt')).read_text()
        self.assertIn('lotus_external', missing)
        self.assertIn('missing', self.stub('check'))
        self.stub('scaffold', '-o', self.stubs)
        source = self.stubs / 'lotus_external.c'
        self.assertTrue(source.is_file())
        self.stub('link', '--stub-dir', self.stubs, '-o', self.output,
                  success=False)
        self.assertFalse(self.output.exists())
        source.write_text('int lotus_external(int x) { return x + 1; }\n')
        self.stub('link', '--stub-dir', self.stubs, '--fail-missing',
                  '-o', self.output)
        ir = self.run_command(LLVM_BIN / 'llvm-dis', self.output, '-o', '-')
        self.assertRegex(ir, r'define .*@lotus_external\(i32')
        self.assertNotRegex(ir, r'declare .*@lotus_external\(')
        self.run_command(LLVM_BIN / 'opt', '-verify', '-disable-output', self.output)

    def test_rejects_signature_mismatch(self):
        (self.stubs / 'lotus_external.c').write_text(
            'double lotus_external(double x) { return x; }\n')
        result = self.stub('link', '--stub-dir', self.stubs, '-o', self.output,
                           success=False)
        self.assertIn('mismatch', result.lower())
        self.assertFalse(self.output.exists())

    def test_prefetch_without_installed_ikos_or_database(self):
        output = self.run_command(BIN_DIR / 'lotus-ikos-api-spec', 'prefetch',
                                  '-j', '1', '--db-dir', self.work / 'empty-db',
                                  '-o', self.stubs, self.bc)
        self.assertIn('missing', output)
        self.assertFalse((self.stubs / 'lotus_external.c').exists())

    def test_all_command_help(self):
        for command in ('stub', 'api-census', 'api-spec'):
            with self.subTest(command=command):
                self.assertIn('usage:', self.run_command(
                    BIN_DIR / ('lotus-ikos-' + command), '--help'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, required=True)
    parser.add_argument('--llvm-bin', type=Path, required=True)
    options, rest = parser.parse_known_args()
    BIN_DIR = options.bin_dir.resolve()
    LLVM_BIN = options.llvm_bin.resolve()
    os.environ['LOTUS_IKOS_LLVM_BIN'] = str(LLVM_BIN)
    unittest.main(argv=[__file__] + rest)
