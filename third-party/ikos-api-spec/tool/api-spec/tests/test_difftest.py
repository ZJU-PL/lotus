import importlib.util
import os
import re
import subprocess
import sys
import tempfile
import unittest
from collections import namedtuple
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[3]
TOOL_DIR = REPO_ROOT / 'tool/api-spec'
INSTALLED_IKOS = REPO_ROOT / 'analyzer/python'
sys.path.insert(0, str(TOOL_DIR))
sys.path.insert(0, str(INSTALLED_IKOS))

from ikos import settings  # noqa: E402


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


difftest = load_module('difftest_under_test', TOOL_DIR / 'difftest.py')
api_spec = load_module('api_spec_under_test',
                       TOOL_DIR / 'ikos-api-spec.py')
coevolve = load_module('coevolve_under_test', TOOL_DIR / 'coevolve.py')
ikosjudge = load_module('ikosjudge_under_test', TOOL_DIR / 'ikosjudge.py')
Plan = namedtuple('Plan', 'name source_name rule')


STUB_HELPERS = {
    '__ikos_assert',
    '__ikos_assume',
    '__ikos_nondet_int',
    '__ikos_nondet_uint',
    '__ikos_malloc',
    '__ikos_check_mem_access',
    '__ikos_check_string_access',
    '__ikos_assume_mem_size',
    '__ikos_forget_mem',
    '__ikos_abstract_mem',
    '__ikos_assume_mem_value',
    '__ikos_assume_mem_rel',
    '__ikos_watch_mem',
}


class FakeLog:
    def debug(self, *_args, **_kwargs):
        pass

    def info(self, *_args, **_kwargs):
        pass

    def warning(self, *_args, **_kwargs):
        pass


class FakeStub:
    PLACEHOLDER_MARK = 'IKOS-STUB-SCAFFOLD-PLACEHOLDER'
    STUB_SUFFIX = '.c'
    ScaffoldError = ValueError
    SigError = TypeError

    def render_stub(self, *_args, **_kwargs):
        return '''#include <stddef.h>
#include <ikos/analyzer/intrinsic.h>
int f(int p0) {
    /* IKOS-STUB-SCAFFOLD-PLACEHOLDER */
    return 0;
}
'''

    def stub_key(self, name):
        return name

    def c_function_name(self, name):
        return name, None


class HelperContractTests(unittest.TestCase):
    def test_stub_helper_contract_stays_synchronized(self):
        self.assertEqual(api_spec.STUB_HELPER_CALLS, STUB_HELPERS)
        self.assertEqual(api_spec.ALLOWED_CALLS, STUB_HELPERS | {'sizeof'})
        for helper in STUB_HELPERS:
            self.assertIn(helper, api_spec.SYSTEM_PROMPT)
            self.assertIn(helper, coevolve.B_SYSTEM_PROMPT)

        canonical = (REPO_ROOT /
                     'analyzer/include/ikos/analyzer/intrinsic.h').read_text()
        shadow = (TOOL_DIR /
                  'difftest/include/ikos/analyzer/intrinsic.h').read_text()
        recorder = (TOOL_DIR / 'difftest/recorder.c').read_text()
        memory_domain = (REPO_ROOT /
                         'core/include/ikos/core/domain/memory/'
                         'abstract_domain.hpp').read_text()
        declaration_re = re.compile(
            r'\b(?:extern )?(?:void|int|unsigned|void\s*\*)\s+'
            r'(__ikos_\w+)\s*\(')
        canonical_declaration_re = re.compile(
            r'\b(?:extern (?:void|int|unsigned) )?(__ikos_\w+)\s*\(')
        canonical_helpers = set(canonical_declaration_re.findall(canonical))
        canonical_helpers |= set(
            re.findall(r'^#define (__ikos_\w+)\s*\(', canonical, re.M))
        shadow_helpers = set(declaration_re.findall(shadow))
        recorder_helpers = set(declaration_re.findall(recorder))

        self.assertLessEqual(STUB_HELPERS, canonical_helpers)
        self.assertEqual(shadow_helpers, STUB_HELPERS)
        self.assertEqual(recorder_helpers, STUB_HELPERS)
        self.assertIn('__ikos_assume_mem_rel', api_spec.SYSTEM_PROMPT)
        self.assertIn('pure refinement', api_spec.SYSTEM_PROMPT.lower())
        self.assertIn('pure refinement', coevolve.B_SYSTEM_PROMPT.lower())
        self.assertIn('pure refinement', canonical.lower())
        self.assertIn('pure refinement', memory_domain.lower())

    def test_score_snapshot_uses_single_settlement_without_player_moves(self):
        opt = SimpleNamespace(rounds=0, co_score_snapshot=True,
                              cache_dir='/tmp')
        kit = SimpleNamespace(log=FakeLog(), stub=FakeStub())
        plan = Plan('score-snapshot', 'score-snapshot', '')
        args = (kit, opt, plan, object(), object(),
                lambda *_: ('', {'total_tokens': 0}), {}, 'stub.c',
                'body', lambda value: value)
        with mock.patch.object(coevolve, '_play',
                               return_value=(None, {'status': 'pass'},
                                             0, None, None)) as play:
            reason, summary, tokens, final_body, floor = coevolve.gate(*args)

        self.assertIsNone(reason)
        self.assertEqual(summary['status'], 'pass')
        self.assertEqual(tokens, 0)
        self.assertIsNone(final_body)
        self.assertIsNone(floor)
        play.assert_called_once()

    def test_prompt_revision_invalidates_response_cache(self):
        opt = SimpleNamespace(model='test-model', domain='all')
        plan = Plan('test-name', 'test-name', '')
        info = SimpleNamespace(line='declare void @test-name()')

        original = api_spec.PROMPT_VERSION
        try:
            original_key = api_spec.cache_key(opt, plan, info)
            api_spec.PROMPT_VERSION = original + '-changed'
            revised_key = api_spec.cache_key(opt, plan, info)
        finally:
            api_spec.PROMPT_VERSION = original

        self.assertNotEqual(original_key, revised_key)

    def test_generated_stubs_prefer_checkout_intrinsics(self):
        canonical = (REPO_ROOT / 'analyzer/include/ikos/analyzer/'
                     'intrinsic.h').read_text()
        self.assertIn('__ikos_malloc', canonical)
        kit = SimpleNamespace(
            log=FakeLog(),
            settings=SimpleNamespace(
                clang=lambda: '/usr/bin/clang',
                INCLUDE_DIR='/installed/include'),
            analyzer=SimpleNamespace(
                clang_emit_llvm_flags=lambda: ['-c', '-emit-llvm'],
                clang_ikos_flags=lambda: ['-D__IKOS__']))

        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'stub.c'
            bitcode = Path(directory) / 'stub.bc'
            source.touch()
            with mock.patch.object(
                    api_spec.subprocess, 'run',
                    return_value=SimpleNamespace(returncode=0,
                                                 stderr='')) as run:
                self.assertTrue(api_spec.compile_candidate(
                    kit, str(source), str(bitcode)))

        command = run.call_args.args[0]
        checkout_index = command.index(
            str(REPO_ROOT / 'analyzer' / 'include'))
        installed_index = command.index('/installed/include')
        self.assertLess(checkout_index, installed_index)


class DifferentialWrapperTests(unittest.TestCase):
    def test_wrapper_carries_generated_abi_typedefs(self):
        with tempfile.TemporaryDirectory() as directory:
            path = difftest._write_dt_wrapper(
                'real_impl', None,
                'struct ikos_cxx_0123456789abcdef* f(struct ikos_cxx_0123456789abcdef* p0)',
                'f', directory, 'real',
                ('struct ikos_cxx_0123456789abcdef;',))
            source = Path(path).read_text()

            self.assertLess(source.index('struct ikos_cxx_0123456789abcdef;'),
                            source.index('real_impl('))
            self.assertIn('struct ikos_cxx_0123456789abcdef* dt_target(', source)

            compiled = subprocess.run([
                settings.clang(), '-c', path, '-o',
                str(Path(directory) / 'wrapper.o')],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                universal_newlines=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)

    @mock.patch('shutil.which', side_effect=lambda name: '/usr/bin/' + name)
    def test_ikos_witness_runs_in_filesystem_sandbox(self, _which):
        kit = SimpleNamespace(settings=SimpleNamespace(
            clang=lambda: '/usr/bin/clang', INCLUDE_DIR='/usr/include'))
        opt = SimpleNamespace(dt_no_sanitize=True)

        with tempfile.TemporaryDirectory() as directory:
            harness = Path(directory) / 'harness.c'
            harness.touch()
            with mock.patch.object(ikosjudge.subprocess, 'run') as run:
                run.side_effect = [
                    SimpleNamespace(returncode=0, stderr=''),
                    SimpleNamespace(returncode=0, stdout=''),
                ]
                result = ikosjudge._witness_run(
                    kit, opt, str(harness), directory, [], 1,
                    'mkdir', [], lambda *_args: None)

            self.assertIsNotNone(result)
            self.assertEqual(len(run.call_args_list), 2)
            build_argv = run.call_args_list[0].args[0]
            self.assertLess(
                build_argv.index(str(ikosjudge.REPO_INCLUDE_DIR)),
                build_argv.index('/usr/include'))
            witness_argv = run.call_args_list[1].args[0]
            self.assertIn('bwrap',
                          [Path(arg).name for arg in witness_argv])
            self.assertIn('--ro-bind', witness_argv)
            self.assertIn(os.path.abspath(directory), witness_argv)
            self.assertEqual(run.call_args_list[1].kwargs['cwd'], directory)

    @mock.patch('shutil.which', side_effect=lambda name: '/usr/bin/' + name)
    def test_sandbox_can_use_explicit_apparmor_profile(self, _which):
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.dict(difftest.os.environ,
                                 {'DT_AA_PROFILE': 'vscode'}):
                prefix = difftest._sandbox_prefix(directory)

        self.assertIsNotNone(prefix)
        self.assertEqual(Path(prefix[0]).name, 'aa-exec')
        self.assertEqual(prefix[1:4], ['--profile', 'vscode', '--'])
        self.assertEqual(Path(prefix[4]).name, 'bwrap')

    def test_native_differential_execution_requires_bubblewrap(self):
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(difftest.shutil, 'which',
                                   return_value=None):
                run = difftest.run_once('/bin/true', [], {}, 1, directory)

        self.assertIsNone(run['rc'])
        self.assertIn('Bubblewrap is required', run['stderr'])
        self.assertEqual(difftest.run_status(run), 'crash-before-call')

    def test_native_differential_execution_preserves_contract_pipe(self):
        if not difftest.shutil.which('bwrap'):
            self.skipTest('Bubblewrap is required for native execution')
        with tempfile.TemporaryDirectory() as directory:
            run = difftest.run_once(
                '/bin/sh', ['-c', 'printf "DT1 call\\n" >&4'], {}, 3,
                directory)

        self.assertEqual(run['rc'], 0, run['stderr'])
        self.assertIn('DT1 call', run['contract'])

    def test_cxx_driver_uses_real_std_objects_and_compiles(self):
        source = r'''
#include "dt.h"
#include <string>

extern "C" bool dt_target(const std::string*);

int main(int argc, char** argv) {
    std::string value = argc > 1 ? argv[1] : "";
    __dt_begin(0);
    __dt_buf("obj", &value, sizeof value);
    __dt_call();
    bool result = dt_target(&value);
    __dt_ret(&result, sizeof result);
    __dt_end();
    return 0;
}
'''
        self.assertIsNone(difftest.driver_guard(source, 'dt_target'))
        error = difftest._driver_compile_check(
            source, settings.clang(), cxx=True)
        self.assertIsNone(error)

    def test_mangled_names_select_cxx_driver(self):
        self.assertTrue(difftest.is_cxx_function(
            Plan('_ZNSt...', 'std::string::empty()', '')))
        self.assertFalse(difftest.is_cxx_function(
            Plan('__cxa_atexit', '__cxa_atexit', '')))

    def test_aggregate_return_uses_sret_wrapper_abi(self):
        self.assertEqual(
            difftest.classify_return('struct ikos_cxx_result'),
            'aggregate')
        with tempfile.TemporaryDirectory() as directory:
            path = difftest._write_dt_wrapper(
                'real_impl', None,
                'struct ikos_cxx_result f(struct ikos_cxx_object* p0,'
                ' long long p1)',
                'f', directory, 'real',
                ('struct ikos_cxx_result;', 'struct ikos_cxx_object;'),
                sret_abi=True)
            source = Path(path).read_text()
            self.assertIn(
                'void real_impl(struct ikos_cxx_result* ikos_dt_sret,'
                ' struct ikos_cxx_object* p0, long long p1)', source)
            self.assertIn(
                'void dt_target(struct ikos_cxx_result* ikos_dt_sret,'
                ' struct ikos_cxx_object* p0, long long p1)', source)
            self.assertIn('real_impl(ikos_dt_sret, p0, p1);', source)

            compiled = subprocess.run([
                settings.clang(), '-c', path, '-o',
                str(Path(directory) / 'wrapper.o')],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                universal_newlines=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)

    def test_sret_driver_guard_requires_registered_result(self):
        source = r'''
#include "dt.h"
#include <string>

extern "C" void dt_target(std::string* result, const std::string* object,
                          unsigned long pos, unsigned long count);

int main(int argc, char** argv) {
    std::string object = argc > 1 ? argv[1] : "";
    std::string result;
    __dt_begin(0);
    __dt_buf("obj", &object, sizeof object);
    __dt_buf("result", &result, sizeof result);
    __dt_call();
    dt_target(&result, &object, 0, 1);
    __dt_ret(&result, sizeof result);
    __dt_end();
    return 0;
}
'''
        self.assertIsNone(difftest._driver_sret_guard(source))
        self.assertIn('void dt_target', difftest._driver_sret_guard(
            source.replace('void dt_target', 'std::string dt_target')))

    def test_cxx_reference_prototype_is_not_counted_as_a_call(self):
        source = r'''
#include "dt.h"
#include <cstddef>
#include <cstdlib>
#include <string>

extern "C" void dt_target(std::string* result,
                          const std::string& object,
                          std::size_t pos, std::size_t count);

int main(int argc, char** argv) {
    std::string object = argc > 1 ? argv[1] : "";
    std::size_t pos = argc > 2
        ? static_cast<std::size_t>(strtoul(argv[2], nullptr, 10)) : 0;
    std::size_t count = argc > 3
        ? static_cast<std::size_t>(strtoul(argv[3], nullptr, 10)) : 1;
    std::string result;
    __dt_begin(0);
    __dt_buf("obj", &object, sizeof object);
    __dt_buf("result", &result, sizeof result);
    __dt_call();
    dt_target(&result, object, pos, count);
    __dt_ret(&result, sizeof result);
    __dt_end();
    return 0;
}
'''
        self.assertIsNone(difftest.driver_guard(source, 'dt_target'))
        self.assertIsNone(difftest._driver_sret_guard(source))
        self.assertIsNone(difftest._driver_compile_check(
            source, settings.clang(), cxx=True))

    def test_hidden_sret_full_object_havoc_covers_result_writes(self):
        real = {'wrote': {'result': set(range(32))},
                'bufregs': [('result', 0x1000, 32)], 'data': {},
                'ret': b'\0' * 32, 'ret_raw': b'\0' * 16}
        alive = [(None, [('HAVOC_FORGET', 'UNREG', 0, 32)])]
        notes = []
        self.assertIsNone(difftest._phase_b(
            real, alive, notes, aggregate_result=True))
        self.assertTrue(notes)

        notes = []
        reason = difftest._phase_b(real, [alive[0]], notes)
        self.assertIn('missed write side effect', reason)

    def test_hidden_sret_requires_havoc_even_without_observed_writes(self):
        real = {'wrote': {}, 'bufregs': [('result', 0x1000, 32)],
                'data': {}, 'ret': b'\0' * 32, 'ret_raw': b'\0' * 16}
        reason = difftest._phase_b(real, [(None, [])], [],
                                   aggregate_result=True)
        self.assertIn('must havoc the full 32-byte result object', reason)

    def test_harness_failure_does_not_install_stub(self):
        plan = Plan('f', 'f', '')
        info = SimpleNamespace(line='declare i32 @f(i32)')
        kit = api_spec.Kit(None, None, None, None, FakeLog(), None,
                           FakeStub())
        scan = SimpleNamespace(typedefs=[])
        opt = SimpleNamespace(max_retries=1, force=True, no_cache=True,
                              no_difftest=False, cache_dir='/tmp',
                              save_failed=False, model='test-model')
        complete = lambda _messages: ('return p0;', {'total_tokens': 1})

        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(api_spec, 'compile_candidate',
                                   return_value=(True, '')), \
                 mock.patch.object(api_spec, 'check_signature',
                                   return_value=(True, None)), \
                 mock.patch.object(
                     api_spec, '_difftest_gate',
                     return_value=(None, {'status': 'harness-error',
                                          'detail': 'driver rejected'},
                                   0, None, None)), \
                 mock.patch.object(api_spec, 'install_stub') as install:
                result = api_spec.generate_one(
                    kit, opt, plan, info, scan, complete, 'llvm-dis', {},
                    directory)

        self.assertEqual(result.status, 'failed')
        self.assertIn('differential gate did not run/pass', result.detail)
        install.assert_not_called()

    def test_equilibrium_battery_runs_and_fails_closed(self):
        battery = [{'argv': ['edge']}]
        kit = opt = plan = parts = driver_path = stub_src = rundir = None
        info = SimpleNamespace(line='declare void @_Zf()')

        with mock.patch.object(coevolve.difftest, '_edge_inputs',
                               return_value=battery), \
             mock.patch.object(coevolve.difftest, 'run_battery',
                               return_value=(
                                   [{'verdict': 'unsound'}], None)) as run:
            broken = coevolve.verify_equilibrium(
                kit, opt, plan, parts, driver_path, [], stub_src,
                rundir, info)

        self.assertEqual(len(broken), 1)
        run.assert_called_once()

        with mock.patch.object(coevolve.difftest, '_edge_inputs',
                               return_value=battery), \
             mock.patch.object(coevolve.difftest, 'run_battery',
                               return_value=(None, 'build failed')):
            with self.assertRaisesRegex(RuntimeError, 'build failed'):
                coevolve.verify_equilibrium(
                    kit, opt, plan, parts, driver_path, [], stub_src,
                    rundir, info)


def _ptr(addr):
    """8-byte little-endian pointer encoding of the contract's ret line."""
    return addr.to_bytes(8, 'little')


def _real_ptr_obs(ret_addr, bufinout, bufregs=None):
    """Real-side observation for the pointer judge (contract parse shape:
    bufregs stay 3-tuples, the in/out flag travels in obs['bufinout'])."""
    return {'ret': _ptr(ret_addr), 'ret_raw': _ptr(ret_addr).hex(),
            'bufregs': (bufregs if bufregs is not None
                        else [('b0', 0x1000, 64), ('b1', 0x2000, 32)]),
            'bufinout': bufinout, 'wrote': {}, 'data': {}}


class NewGateTests(unittest.TestCase):
    """The semantic gate rules added against the E2 defect stubs:

    rule A  -- input-buffer aliasing (fopen64 returning the path argument)
    rule B  -- stack return (g_malloc's "return &p0")
    rule C  -- pointer/int confusion, note only (_TIFFmalloc's "(void*)p0")
    rule 2  -- havoc claims running past the registered buffer
    item 4  -- assume_mem_size over-estimates a pure output buffer
    item 3  -- narrow-range lint on the integer return axis, note only
    """

    FRAME = 0x7fffffffe000

    def stub_run(self, ret_addr, bufregs=(), events=()):
        return ({'ret': _ptr(ret_addr), 'bufregs': list(bufregs)},
                list(events))

    # ---- rule A: return aliases an input buffer ----

    def test_return_aliasing_input_buffer_is_unsound(self):
        # real returns a fresh library pointer (OTHER), the stub hands the
        # caller's own input buffer b0 back
        real = _real_ptr_obs(0x506000000010, {'b0': 'in', 'b1': 'out'})
        alive = [self.stub_run(0x1000, [('b0', 0x1000, 64)],
                               [('FRAME', self.FRAME)])]
        notes = []
        verdict, reason = difftest._phase_c(real, alive, 'pointer', notes)
        self.assertEqual(verdict, 'unsound')
        self.assertTrue(reason.startswith(
            'unsound: return aliases input buffer b0'), reason)

    def test_return_inside_same_input_buffer_is_legal(self):
        # strchr shape: real and stub both return inside argument b0
        real = _real_ptr_obs(0x1005, {'b0': 'in'},
                             [('b0', 0x1000, 64)])
        alive = [self.stub_run(0x1003, [('b0', 0x1000, 64)]),
                 self.stub_run(0x1007, [('b0', 0x1000, 64)])]
        verdict, _reason = difftest._phase_c(real, alive, 'pointer', [])
        self.assertEqual(verdict, 'sound')

    def test_return_of_output_buffer_is_exempt(self):
        # strcpy shape against a fresh real pointer: the output buffer is
        # the stub's storage, aliasing it is the sanctioned approximation
        real = _real_ptr_obs(0x506000000010, {'b1': 'out'})
        alive = [self.stub_run(0x2000, [('b1', 0x2000, 32)],
                               [('FRAME', self.FRAME)])]
        verdict, _reason = difftest._phase_c(real, alive, 'pointer', [])
        self.assertEqual(verdict, 'sound*')

    def test_return_aliasing_input_buffer_rejected_even_when_real_null(self):
        # real returned NULL, the stub returns the input buffer on some
        # schedule and NULL on another: still an aliasing violation
        real = _real_ptr_obs(0, {'b0': 'in'}, [('b0', 0x1000, 64)])
        alive = [self.stub_run(0, [('b0', 0x1000, 64)]),
                 self.stub_run(0x1000, [('b0', 0x1000, 64)])]
        verdict, reason = difftest._phase_c(real, alive, 'pointer', [])
        self.assertEqual(verdict, 'unsound')
        self.assertTrue(reason.startswith('unsound: return aliases'),
                        reason)

    # ---- rule B: return points into the target's stack frame ----

    def test_stack_return_is_unsound(self):
        real = _real_ptr_obs(0x506000000010, {'b0': 'in'})
        alive = [self.stub_run(self.FRAME + 0x40, [],
                               [('FRAME', self.FRAME)])]
        notes = []
        verdict, reason = difftest._phase_c(real, alive, 'pointer', notes)
        self.assertEqual(verdict, 'unsound')
        self.assertTrue(reason.startswith(
            'unsound: return points into the target stack frame'), reason)

    def test_stack_return_exempt_when_real_points_nearby(self):
        # both sides point into the same caller-side stack region (argv
        # strings and the like): the fresh-block gap, not a dead local
        real = _real_ptr_obs(self.FRAME + 0x800, {'b0': 'in'})
        alive = [self.stub_run(self.FRAME + 0x840, [],
                               [('FRAME', self.FRAME)])]
        verdict, _reason = difftest._phase_c(real, alive, 'pointer', [])
        self.assertEqual(verdict, 'sound*')

    def test_stack_return_not_judged_without_frame_landmark(self):
        real = _real_ptr_obs(0x506000000010, {'b0': 'in'})
        alive = [self.stub_run(self.FRAME + 0x40, [], [])]
        verdict, _reason = difftest._phase_c(real, alive, 'pointer', [])
        self.assertEqual(verdict, 'sound*')

    # ---- rule C: pointer/int confusion is a note, never a veto ----

    def test_pointer_int_confusion_only_notes(self):
        real = _real_ptr_obs(0x506000000010, {})
        alive = [self.stub_run(0x10, [], [('FRAME', self.FRAME)]),
                 self.stub_run(0x10, [], [('FRAME', self.FRAME)])]
        notes = []
        verdict, _reason = difftest._phase_c(real, alive, 'pointer', notes)
        self.assertEqual(verdict, 'sound*')
        self.assertTrue(any('pointer/int confusion' in note
                            for note in notes), notes)

    def test_pointer_int_confusion_silent_with_large_other(self):
        # the battery itself injects 1/-1, so a lone small value proves
        # nothing while a large OTHER return exists
        real = _real_ptr_obs(0x506000000010, {})
        alive = [self.stub_run(0x10, [], []),
                 self.stub_run(0xffffffffffffffff, [], [])]
        notes = []
        difftest._phase_c(real, alive, 'pointer', notes)
        self.assertFalse(any('pointer/int confusion' in note
                             for note in notes), notes)

    # ---- allocators get a fresh-block contract, not the OTHER waiver ----

    def test_malloc_like_detection(self):
        parts = SimpleNamespace(signature_line='void* f(long long p0)')
        self.assertTrue(difftest.is_malloc_like(
            Plan('_TIFFmalloc', '_TIFFmalloc', ''), parts))
        self.assertTrue(difftest.is_malloc_like(
            Plan('g_malloc', 'g_malloc', ''), parts))
        self.assertFalse(difftest.is_malloc_like(
            Plan('calloc', 'calloc', ''), parts))
        self.assertFalse(difftest.is_malloc_like(
            Plan('malloc_size', 'malloc_size', ''),
            SimpleNamespace(signature_line='void* f(size_t p0)')))

    def test_allocator_size_as_pointer_is_unsound(self):
        real = _real_ptr_obs(0x506000000010, {})
        alive = [self.stub_run(0x10)]
        verdict, reason = difftest._phase_c(real, alive, 'pointer', [],
                                            allocator=True)
        self.assertEqual(verdict, 'unsound')
        self.assertIn('allocator return is not fresh storage', reason)

    def test_allocator_fresh_helper_is_sound(self):
        real = _real_ptr_obs(0x506000000010, {})
        alive = [self.stub_run(
            0x506000000100,
            events=[('MALLOC', 0x506000000100, 16)])]
        verdict, reason = difftest._phase_c(real, alive, 'pointer', [],
                                            allocator=True)
        self.assertEqual((verdict, reason),
                         ('sound', 'fresh dynamic-allocation capability'))

    def test_recorder_parses_fresh_allocation_event(self):
        self.assertEqual(difftest.parse_dtlog('MALLOC 506000000100 16\n'),
                         [('MALLOC', 0x506000000100, 16)])

    # ---- rule 2: havoc past the registered buffer ----

    def havoc_real(self, bufinout):
        return {'wrote': {'b1': set(range(32))},
                'bufregs': [('b1', 0x2000, 32)], 'bufinout': bufinout,
                'data': {}, 'ret': b'\0' * 4, 'ret_raw': '0' * 8}

    def test_havoc_beyond_registered_buffer_is_unsound(self):
        alive = [(None, [('HAVOC_FORGET', 'b1', 0, 256)])]
        reason = difftest._phase_b(self.havoc_real({'b1': 'out'}),
                                   alive, [])
        self.assertIsNotNone(reason)
        self.assertTrue(reason.startswith(
            'unsound: havoc exceeds output buffer b1 (256 > 32)'), reason)

    def test_havoc_inside_registered_buffer_passes(self):
        alive = [(None, [('HAVOC_FORGET', 'b1', 0, 32)])]
        self.assertIsNone(difftest._phase_b(self.havoc_real({'b1': 'out'}),
                                            alive, []))

    def test_unregistered_havoc_only_notes(self):
        alive = [(None, [('HAVOC_FORGET', 'b1', 0, 32),
                         ('HAVOC_FORGET', 'UNREG', 0, 64)])]
        notes = []
        reason = difftest._phase_b(self.havoc_real({'b1': 'out'}),
                                   alive, notes)
        self.assertIsNone(reason)
        self.assertTrue(any('unregistered pointer' in note
                            for note in notes), notes)

    # ---- item 4: assume_mem_size over-estimate on an output buffer ----

    def test_assume_mem_size_over_estimate_on_output_is_unsound(self):
        alive = [(None, [('HAVOC_FORGET', 'b1', 0, 32),
                         ('ASSUME_SIZE', 'b1', 0, 64)])]
        reason = difftest._phase_b(self.havoc_real({'b1': 'out'}),
                                   alive, [])
        self.assertIsNotNone(reason)
        self.assertTrue(reason.startswith(
            'unsound: assume_mem_size claims 64 bytes on output buffer b1'),
            reason)

    def test_assume_mem_size_over_estimate_on_input_is_not_this_rule(self):
        alive = [(None, [('HAVOC_FORGET', 'b1', 0, 32),
                         ('ASSUME_SIZE', 'b1', 0, 64)])]
        self.assertIsNone(difftest._phase_b(self.havoc_real({'b1': 'in'}),
                                            alive, []))

    # ---- item 3: narrow-range lint, note only ----

    def test_narrow_range_lint_notes_without_veto(self):
        real = {'ret': (10).to_bytes(4, 'little', signed=True),
                'ret_raw': '0a000000', 'bufregs': [], 'bufinout': {},
                'wrote': {}, 'data': {}}
        alive = [({'ret': (0).to_bytes(4, 'little', signed=True)}, []),
                 ({'ret': (10).to_bytes(4, 'little', signed=True)}, [])]
        notes = []
        verdict, _reason = difftest._phase_c(real, alive, ('int', 4), notes)
        self.assertEqual(verdict, 'sound')
        self.assertTrue(any(note.startswith('narrow-range: achievable'
                                            ' [0, 10] much narrower')
                            for note in notes), notes)

    def test_narrow_range_lint_silent_far_from_edge(self):
        real = {'ret': (50000).to_bytes(4, 'little', signed=True),
                'ret_raw': '50c30000', 'bufregs': [], 'bufinout': {},
                'wrote': {}, 'data': {}}
        alive = [({'ret': (0).to_bytes(4, 'little', signed=True)}, []),
                 ({'ret': (50000).to_bytes(4, 'little', signed=True)}, []),
                 ({'ret': (100000).to_bytes(4, 'little', signed=True)}, [])]
        notes = []
        verdict, _reason = difftest._phase_c(real, alive, ('int', 4), notes)
        self.assertEqual(verdict, 'sound')
        self.assertFalse(any('narrow-range' in note for note in notes),
                         notes)


class PipelineGateTests(unittest.TestCase):
    """The three pipeline gates added after the E2 audit (ikos-api-spec
    side): noreturn enforcement, the special-control-flow unembeddable
    short-circuit, and the ikos stub self-check."""

    # ---- noreturn_guard ----

    def test_noreturn_body_without_assume0_is_rejected(self):
        reason = api_spec.noreturn_guard('return;', 'exit', 'exit',
                                         'declare void @exit(i32)')
        self.assertTrue(reason.startswith('noreturn-missing-assume0: exit'),
                        reason)

    def test_noreturn_declare_attribute_channel_is_rejected(self):
        reason = api_spec.noreturn_guard('int x = 0;', 'my_fn', 'my_fn',
                                         'declare noreturn void @my_fn()')
        self.assertTrue(reason.startswith('noreturn-missing-assume0'),
                        reason)

    def test_noreturn_return_after_assume0_is_rejected(self):
        reason = api_spec.noreturn_guard('__ikos_assume(0);\nreturn;',
                                         'abort', 'abort',
                                         'declare void @abort()')
        self.assertTrue(reason.startswith('noreturn-late-return'), reason)

    def test_noreturn_side_effects_before_assume0_pass(self):
        body = '__ikos_forget_mem(p0, 8);\n__ikos_assume(0);'
        self.assertIsNone(api_spec.noreturn_guard(
            body, 'exit', 'exit', 'declare void @exit(i32)'))

    def test_noreturn_guard_ignores_regular_functions(self):
        self.assertIsNone(api_spec.noreturn_guard('return 0;', 'f', 'f',
                                                  'declare i32 @f()'))

    def test_noreturn_dead_return_after_assume0_is_rejected_lexically(self):
        # the guard is lexical and conservative: even an unreachable
        # `if (0) return;` after the last __ikos_assume(0) is rejected
        body = '__ikos_assume(0);\nif (0) { return; }'
        reason = api_spec.noreturn_guard(body, 'exit', 'exit',
                                         'declare void @exit(i32)')
        self.assertTrue(reason.startswith('noreturn-late-return'), reason)

    # ---- unembeddable short-circuit ----

    def test_is_unembeddable_names_and_escape(self):
        opt = SimpleNamespace(allow_unembeddable=[])
        self.assertTrue(api_spec.is_unembeddable(opt, '_setjmp', '_setjmp'))
        self.assertTrue(api_spec.is_unembeddable(opt, '__cxa_throw',
                                                 'std::terminate'))
        self.assertFalse(api_spec.is_unembeddable(opt, 'memcpy', 'memcpy'))
        escape = SimpleNamespace(allow_unembeddable=['_setjmp'])
        self.assertFalse(api_spec.is_unembeddable(escape, '_setjmp',
                                                  '_setjmp'))

    def test_generate_one_short_circuits_unembeddable_without_llm(self):
        plan = Plan('_setjmp', '_setjmp', '')
        info = SimpleNamespace(line='declare i32 @_setjmp(i8*)')
        kit = api_spec.Kit(None, None, None, None, FakeLog(), None,
                           FakeStub())
        scan = SimpleNamespace(typedefs=[])
        opt = SimpleNamespace(allow_unembeddable=[])
        calls = []

        def complete(_messages):
            calls.append(1)
            return ('', {})

        with tempfile.TemporaryDirectory() as directory:
            result = api_spec.generate_one(
                kit, opt, plan, info, scan, complete, 'llvm-dis', {},
                directory)

        self.assertEqual(result.status, 'unembeddable')
        self.assertEqual(result.detail,
                         'special-control-flow: manual modeling required')
        self.assertEqual(calls, [])

    # ---- ikosjudge.selfcheck, end to end with the real analyzer ----

    STAT64_STUB = '''#include <stddef.h>
#include <ikos/analyzer/intrinsic.h>
struct stat64;
int stat64(const char* p0, struct stat64* p1) {
%s
}
'''
    STAT64_PARTS = SimpleNamespace(
        typedef_lines=['struct stat64;'],
        signature_line='int stat64(const char* p0, struct stat64* p1)')

    def selfcheck_kit(self):
        import os
        from ikos import analyzer
        try:
            binaries = (settings.ikos(), settings.clang(),
                        settings.llvm_link())
        except Exception as error:
            self.skipTest('ikos toolchain unavailable: %s' % error)
        missing = [path for path in binaries
                   if not path or not os.path.isfile(path)]
        if missing:
            self.skipTest('ikos toolchain unavailable: %s'
                          % ', '.join(missing))
        return SimpleNamespace(settings=settings, analyzer=analyzer,
                               log=FakeLog())

    def run_selfcheck(self, body, parts=None, source_name='stat64'):
        kit = self.selfcheck_kit()
        opt = SimpleNamespace()
        with tempfile.TemporaryDirectory() as directory:
            stub_path = Path(directory) / 'stub.c'
            stub_path.write_text(self.STAT64_STUB % body)
            return ikosjudge.selfcheck(
                kit, opt, str(stub_path), parts or self.STAT64_PARTS,
                str(Path(directory) / 'work'), source_name)

    def test_selfcheck_catches_havoc_past_real_struct(self):
        # the E2 stat64 defect class: forgetting 256 bytes over a
        # 144-byte struct stat64 is a definite buffer overflow the
        # differential gate could not see
        verdict, detail = self.run_selfcheck(
            '    __ikos_forget_mem(p1, 256);\n    return 0;')
        if verdict is None:
            self.skipTest('self-check harness could not run: %s' % detail)
        self.assertIs(verdict, False)
        self.assertIn('buffer-overflow', detail)

    def test_selfcheck_passes_exact_size_havoc(self):
        # same stub forgetting exactly sizeof(struct stat64): the
        # sys/stat.h driver mapping sizes the pointee for real, so the
        # legitimate whole-struct write is not misjudged
        verdict, detail = self.run_selfcheck(
            '    __ikos_forget_mem(p1, 144);\n    return 0;')
        if verdict is None:
            self.skipTest('self-check harness could not run: %s' % detail)
        self.assertIs(verdict, True)

    def test_selfcheck_function_pointer_signature_gives_no_evidence(self):
        parts = SimpleNamespace(
            typedef_lines=[],
            signature_line='void qsort(void* p0, unsigned long p1,'
                           ' unsigned long p2,'
                           ' int (*p3)(const void*, const void*))')
        verdict, detail = self.run_selfcheck(
            '    __ikos_assume(0);\n    return;', parts, 'qsort')
        self.assertIsNone(verdict)
        self.assertEqual(detail, 'unsupported signature')


class AnalyzerFailureTests(unittest.TestCase):
    def test_failed_or_missing_analyzer_results_are_not_success(self):
        from ikos import analyzer
        kit = SimpleNamespace(
            analyzer=analyzer, log=FakeLog(),
            settings=SimpleNamespace(
                clang=lambda: '/fake/clang', llvm_link=lambda: '/fake/llvm-link',
                opt=lambda: '/fake/opt', ikos=lambda: '/fake/ikos',
                INCLUDE_DIR='/fake/include'))
        parts = SimpleNamespace(typedef_lines=[], signature_line='int f(void)')
        for status, database in ((1, None), (0, None), (0, 'invalid sqlite')):
            with self.subTest(status=status, database=database):
                with tempfile.TemporaryDirectory() as directory:
                    def run(command, **kwargs):
                        if command[0] == '/fake/ikos':
                            if database is not None:
                                Path(command[command.index('-o') + 1]).write_text(database)
                            return SimpleNamespace(returncode=status, stdout='', stderr='')
                        return SimpleNamespace(returncode=0, stdout='', stderr='')
                    with mock.patch.object(ikosjudge.subprocess, 'run', side_effect=run):
                        verdict, detail = ikosjudge.selfcheck(
                            kit, SimpleNamespace(), '/fake/stub.c', parts, directory)
                self.assertIsNone(verdict)
                self.assertTrue(detail)


class RepairHintTests(unittest.TestCase):
    """--repair-hint: outer audit loop findings steer the first-shot body."""

    def test_repair_hint_reaches_first_shot_prompt(self):
        plan = Plan('f', 'f', '')
        info = SimpleNamespace(line='declare i32 @f(i32)')
        parts = api_spec.Skeleton((), 'int f(int p0)')

        bare = api_spec.user_prompt(plan, info, parts)
        self.assertNotIn('Findings from whole-program', bare)
        hinted = api_spec.user_prompt(plan, info, parts,
                                      ('stub line 5: buffer-overflow',))
        self.assertIn('## Findings from whole-program static analysis of'
                      ' your previous stub', hinted)
        self.assertIn('stub line 5: buffer-overflow', hinted)
        self.assertIn('Address every finding above in the new body.',
                      hinted)
        self.assertLess(hinted.index('Reply with only the body'
                                     ' statements.'),
                        hinted.index('## Findings'))

        # the generate_one entry point forwards opt.repair_hint into the
        # user message (stop the pipeline on the first LLM call)
        kit = api_spec.Kit(None, None, None, None, FakeLog(), None,
                           FakeStub())
        scan = SimpleNamespace(typedefs=[])
        opt = SimpleNamespace(allow_unembeddable=[], domain='all',
                              model='test-model', no_cache=True,
                              max_retries=1, force=True,
                              repair_hint=['stub line 5:'
                                           ' buffer-overflow'])
        captured = []

        def complete(messages):
            captured.extend(messages)
            raise api_spec.llm_client.LLMError('stop here')

        with tempfile.TemporaryDirectory() as directory:
            result = api_spec.generate_one(
                kit, opt, plan, info, scan, complete, 'llvm-dis', {},
                directory)

        self.assertEqual(result.status, 'llm-error')
        self.assertEqual(len(captured), 2)
        self.assertEqual(captured[1]['role'], 'user')
        self.assertIn('stub line 5: buffer-overflow',
                      captured[1]['content'])

    def test_repair_hint_changes_cache_key(self):
        plan = Plan('f', 'f', '')
        info = SimpleNamespace(line='declare i32 @f(i32)')
        base = SimpleNamespace(model='test-model', domain='all')
        hinted = SimpleNamespace(model='test-model', domain='all',
                                 repair_hint=['finding A'])
        other = SimpleNamespace(model='test-model', domain='all',
                                repair_hint=['finding B'])

        # an attribute-less opt (legacy callers/tests) and an explicit
        # empty list share the old key shape, so hint-less caches stay
        # valid; any hint -- and any hint change -- rekeys
        self.assertEqual(api_spec.cache_key(base, plan, info),
                         api_spec.cache_key(
                             SimpleNamespace(model='test-model',
                                             domain='all',
                                             repair_hint=[]),
                             plan, info))
        self.assertNotEqual(api_spec.cache_key(base, plan, info),
                            api_spec.cache_key(hinted, plan, info))
        self.assertNotEqual(api_spec.cache_key(hinted, plan, info),
                            api_spec.cache_key(other, plan, info))


import cxprobe  # noqa: E402  (TOOL_DIR is on sys.path, see the header)


class CxProbeTests(unittest.TestCase):
    """cxprobe: bitcode-derived parameter/return types and sizes, the
    call-site reverse lookup, and its use by the self-check driver."""

    PROBE_C = r'''
#include <stddef.h>
struct inner { char c; long l; };
union uni { char b[3]; long v; };
struct outer { char a; struct inner in; long arr[3]; union uni u; };
struct opaque_t;
struct servent_like { char* name; char** aliases; int port; char* proto; };
int fn_nested(struct outer* p);
long fn_array(long (*p)[3]);
union uni* fn_union(union uni* u);
void fn_opaque(struct opaque_t* o);
char** fn_ptrptr(char** p);
long fn_scalar(long x, char c);
int erased_stat(char* path, void* buf);
void* getserv_like(int idx);
_Static_assert(__builtin_offsetof(struct outer, in) == 8, "off in");
_Static_assert(__builtin_offsetof(struct outer, arr) == 24, "off arr");
_Static_assert(__builtin_offsetof(struct outer, u) == 48, "off u");
_Static_assert(sizeof(struct inner) == 16, "inner");
void driver(void) {
    static struct outer store;
    static long grid[3];
    static union uni uu;
    fn_nested(&store);
    fn_array(&grid);
    fn_union(&uu);
    fn_opaque(0);
    fn_ptrptr(0);
    fn_scalar(1, 2);
    erased_stat("x", &store);
    struct servent_like* s = (struct servent_like*) getserv_like(1);
    (void) s;
}
'''

    PRINTF_C = r'''
#include <stdio.h>
#include <stddef.h>
struct inner { char c; long l; };
union uni { char b[3]; long v; };
struct outer { char a; struct inner in; long arr[3]; union uni u; };
struct servent_like { char* name; char** aliases; int port; char* proto; };
int main(void) {
    printf("outer=%zu inner=%zu uni=%zu arr3=%zu servent_like=%zu"
           " off_in=%zu off_arr=%zu off_u=%zu\n",
           sizeof(struct outer), sizeof(struct inner), sizeof(union uni),
           sizeof(long[3]), sizeof(struct servent_like),
           __builtin_offsetof(struct outer, in),
           __builtin_offsetof(struct outer, arr),
           __builtin_offsetof(struct outer, u));
    return 0;
}
'''

    @staticmethod
    def llvm_tools():
        import os
        llvm_bin = getattr(settings, 'LLVM_BIN_DIR', None)
        if not llvm_bin:
            return None
        return cxprobe.Tools(
            settings.clang(),
            os.path.join(llvm_bin, 'opt'),
            os.path.join(llvm_bin, 'llvm-dis'))

    def setUp(self):
        import os
        self.tools = self.llvm_tools()
        clang = getattr(self.tools, 'clang', None)
        if (not clang or not os.path.isfile(clang)
                or not os.path.isfile(self.tools.opt)
                or not os.path.isfile(self.tools.llvm_dis)):
            self.skipTest('LLVM toolchain unavailable')
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.workdir = Path(directory.name)
        src = self.workdir / 'probe.c'
        src.write_text(self.PROBE_C)
        self.bc = str(self.workdir / 'probe.bc')
        proc = subprocess.run([clang, '-O0', '-c', '-emit-llvm', str(src),
                               '-o', self.bc],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              universal_newlines=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def context(self, name):
        return cxprobe.context_for_name(self.bc, name, tools=self.tools)

    def native_sizes(self):
        src = self.workdir / 'sizeof.c'
        src.write_text(self.PRINTF_C)
        binary = str(self.workdir / 'sizeof')
        proc = subprocess.run([self.tools.clang, '-O0', str(src), '-o',
                               binary],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              universal_newlines=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        out = subprocess.run([binary], stdout=subprocess.PIPE,
                             universal_newlines=True).stdout
        values = {}
        for token in out.split():
            key, value = token.split('=')
            values[key] = int(value)
        return values

    # (a) bc-typedef sizes vs clang's own sizeof/offsetof

    def test_struct_array_union_sizes_match_clang_sizeof(self):
        expected = self.native_sizes()
        ctx = self.context('fn_nested')
        self.assertEqual(ctx['params'][0]['pointee_ir_type'],
                         '%struct.outer')
        self.assertEqual(ctx['params'][0]['size'], expected['outer'])
        self.assertEqual(ctx['params'][0]['source'], 'bc-typedef')
        self.assertEqual(ctx['return']['size'], 4)

        ctx = self.context('fn_array')
        self.assertEqual(ctx['params'][0]['size'], expected['arr3'])
        self.assertEqual(ctx['params'][0]['source'], 'bc-typedef')

        ctx = self.context('fn_union')
        self.assertEqual(ctx['params'][0]['size'], expected['uni'])
        self.assertEqual(ctx['return']['pointee_ir_type'], '%union.uni')
        self.assertEqual(ctx['return']['size'], expected['uni'])

        ctx = self.context('fn_ptrptr')
        self.assertEqual(ctx['params'][0]['pointee_ir_type'], 'i8*')
        self.assertEqual(ctx['params'][0]['size'], 8)  # a pointer's size

        ctx = self.context('fn_opaque')
        self.assertEqual(ctx['params'][0]['source'], 'opaque')
        self.assertIsNone(ctx['params'][0]['size'])

    def test_field_offsets_match_clang_offsetof(self):
        expected = self.native_sizes()
        ir = cxprobe.ir_text(self.bc, tools=self.tools)
        defs = cxprobe.extract_typedefs(ir)
        typedefs = cxprobe._collect_typedefs(['%struct.outer'], defs)
        target = dict(cxprobe._TARGET_RE.findall(ir))
        outer = cxprobe.parse_type('%struct.outer')
        body = cxprobe.parse_type(typedefs['%struct.outer'])
        entries = []
        for field_index, field_node in enumerate(body[2]):
            entries.append((field_index, 'offset', outer,
                            (field_index, field_node)))
        offsets = cxprobe._probe_batch(
            entries, typedefs, self.tools,
            target.get('datalayout'), target.get('triple'))
        self.assertEqual(offsets[1], expected['off_in'])
        self.assertEqual(offsets[2], expected['off_arr'])
        self.assertEqual(offsets[3], expected['off_u'])

    # (b) call-site reverse lookup on erased i8* parameters/returns

    def test_call_site_recovers_erased_parameter_type(self):
        ctx = self.context('erased_stat')
        self.assertEqual(ctx['params'][1]['ir_type'], '%struct.outer*')
        self.assertEqual(ctx['params'][1]['source'], 'call-site')
        self.assertEqual(ctx['params'][1]['size'],
                         self.native_sizes()['outer'])

    def test_call_site_recovers_erased_return_type(self):
        ctx = self.context('getserv_like')
        self.assertEqual(ctx['return']['ir_type'], '%struct.servent_like*')
        self.assertEqual(ctx['return']['source'], 'call-site')
        self.assertEqual(ctx['return']['size'],
                         self.native_sizes()['servent_like'])

    # (c) graceful fallback when the LLVM tools are missing

    def test_missing_tools_fail_soft(self):
        import os
        empty = str(self.workdir)
        with mock.patch.dict(os.environ, {'PATH': empty}):
            self.assertIsNone(
                cxprobe.find_tools(ikos_prefix='/nonexistent-cxprobe'))
            with self.assertRaises(cxprobe.ProbeError):
                cxprobe.ir_text(self.bc, tools=None,
                                ikos_prefix='/nonexistent-cxprobe')

    # ---- self-check driver use of the probe ----

    def test_driver_allocates_probed_sizes(self):
        parts = SimpleNamespace(
            typedef_lines=['struct stat64;'],
            signature_line='int stat64(const char* p0, struct stat64* p1)')
        cxcontext = {
            'ir_name': 'stat64',
            'params': [
                {'index': 0, 'ir_type': 'i8*', 'pointee_ir_type': 'i8',
                 'size': 1, 'source': 'bc-typedef'},
                {'index': 1, 'ir_type': '%struct.stat*',
                 'pointee_ir_type': '%struct.stat', 'size': 144,
                 'source': 'bc-typedef'}],
            'return': {'ir_type': 'i32', 'pointee_ir_type': None,
                       'size': 4, 'source': 'scalar'}}
        driver = ikosjudge._selfcheck_driver(parts, 'stat64', cxcontext)
        self.assertIn('calloc(1, 144)', driver)   # aggregate: exact
        self.assertIn('calloc(1, 128)', driver)   # scalar pointee: floor
        # a count-mismatched probe (sret/byval lowering) is discarded
        skewed = dict(cxcontext, params=cxcontext['params'][:1])
        driver = ikosjudge._selfcheck_driver(parts, 'stat64', skewed)
        self.assertNotIn('calloc(1, 144)', driver)

    def test_driver_checks_pointer_return_object_size(self):
        parts = SimpleNamespace(
            typedef_lines=['struct servent;'],
            signature_line='struct servent* getservbyname(const char* p0,'
                           ' const char* p1)')
        cxcontext = {
            'ir_name': 'getservbyname',
            'params': [
                {'index': 0, 'ir_type': 'i8*', 'pointee_ir_type': 'i8',
                 'size': 1, 'source': 'bc-typedef'},
                {'index': 1, 'ir_type': 'i8*', 'pointee_ir_type': 'i8',
                 'size': 1, 'source': 'bc-typedef'}],
            'return': {'ir_type': '%struct.servent*',
                       'pointee_ir_type': '%struct.servent', 'size': 32,
                       'source': 'bc-typedef'}}
        driver = ikosjudge._selfcheck_driver(parts, 'getservbyname',
                                             cxcontext)
        self.assertIn('__ikos_check_mem_access((void*) r, 32);', driver)
        # no probe at all: the pre-cxprobe driver shape is unchanged
        driver = ikosjudge._selfcheck_driver(parts, 'getservbyname', None)
        self.assertNotIn('__ikos_check_mem_access', driver)

    # ---- end to end: probed buffer sizes convict an overflowing stub ----

    def test_selfcheck_convicts_overflow_with_probed_size(self):
        try:
            settings.ikos()
        except RuntimeError as error:
            self.skipTest(str(error))
        class Log:
            def warning(self, *args):
                pass

        from ikos import analyzer
        kit = SimpleNamespace(settings=settings, analyzer=analyzer,
                              log=Log())
        # the stub forgets 256 bytes into a parameter whose probed
        # call-site size is sizeof(struct outer): a definite overflow
        stub_src = self.workdir / 'stub.c'
        stub_src.write_text(
            '#include <stddef.h>\n'
            '#include <ikos/analyzer/intrinsic.h>\n'
            'struct outer;\n'
            'int erased_stat(char* p0, struct outer* p1) {\n'
            '    __ikos_forget_mem(p1, 256);\n'
            '    return 0;\n'
            '}\n')
        parts = SimpleNamespace(
            typedef_lines=['struct outer;'],
            signature_line='int erased_stat(char* p0, struct outer* p1)')
        opt = SimpleNamespace(inputs=[self.bc], cache_dir=str(self.workdir))
        verdict, detail = ikosjudge.selfcheck(
            kit, opt, str(stub_src), parts,
            str(self.workdir / 'sc'), 'erased_stat')
        if verdict is None:
            self.skipTest('self-check harness could not run: %s' % detail)
        self.assertIs(verdict, False)
        self.assertIn('buffer-overflow', detail)

    # ---- real-bitcode smoke (skip when the projects are absent) ----

    def test_real_bitcode_smoke(self):
        import os
        bzip2 = '/home/user/OSS-FUZZ-Project/bzip2/bzip2.bc'
        keysign = '/home/user/OSS-FUZZ-Project/openssh-portable' \
                  '/ssh-keysign.bc'
        if not (os.path.isfile(bzip2) and os.path.isfile(keysign)):
            self.skipTest('OSS-FUZZ project bitcode not available')
        ctx = cxprobe.context_for_name(bzip2, 'stat64', tools=self.tools)
        stat_param = ctx['params'][1]
        self.assertIn(stat_param['pointee_ir_type'],
                      ('%struct.stat', '%struct.stat64'))
        self.assertEqual(stat_param['size'], 144)
        ctx = cxprobe.context_for_name(keysign, 'getservbyname',
                                       tools=self.tools)
        self.assertEqual(ctx['return']['pointee_ir_type'],
                         '%struct.servent')
        self.assertGreater(ctx['return']['size'], 4)


SCENARIO_DRIVER = r'''
#include "dt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>

int dt_target(regex_t* preg, const char* str, int cflags);

int main(int argc, char** argv) {
    static regex_t re;
    const char* input = argc > 1 ? argv[1] : "abc";
    regcomp(&re, input, 0);
    __dt_begin(0);
    __dt_buf("b0", &re, sizeof re);
    __dt_call();
    int r = dt_target(&re, input, 0);
    __dt_ret(&r, sizeof r);
    __dt_end();
    regfree(&re);
    return 0;
}
'''


BARE_DRIVER = r'''
#include "dt.h"
#include <stdlib.h>

int dt_target(int p0);

int main(int argc, char** argv) {
    int p0 = argc > 1 ? atoi(argv[1]) : 0;
    __dt_begin(0);
    __dt_call();
    int r = dt_target(p0);
    __dt_ret(&r, sizeof r);
    __dt_end();
    return 0;
}
'''

INPUTS_JSON = '[{"argv": ["0"], "stdin": ""}]'


class ScenarioDriverTests(unittest.TestCase):
    """Scenario-mode drivers: setup constructor calls before the single
    target call, gated by the real-side washout escalation."""

    def _artifact_fixtures(self, directory):
        opt = SimpleNamespace(model='m', force=True, cache_dir=directory,
                              real_lib_map=None, dt_driver_retries=1,
                              dt_inputs=1)
        plan = Plan('f', 'f', '')
        info = SimpleNamespace(line='declare i32 @f(i32)')
        parts = SimpleNamespace(signature_line='int f(int p0)',
                                typedef_lines=[])
        return opt, plan, info, parts

    # ---- guard ----

    def test_guard_accepts_whitelisted_setup_in_scenario_mode(self):
        self.assertIsNone(difftest.driver_guard(
            SCENARIO_DRIVER, 'dt_target', scenario=True))
        # the same source is illegal for a bare driver: regex.h and the
        # regcomp/regfree calls are scenario-only
        self.assertIsNotNone(difftest.driver_guard(
            SCENARIO_DRIVER, 'dt_target'))

    def test_guard_rejects_non_whitelisted_setup(self):
        src = SCENARIO_DRIVER.replace(
            'regcomp(&re, input, 0);',
            'regcomp(&re, input, 0); system("ls");')
        reason = difftest.driver_guard(src, 'dt_target', scenario=True)
        self.assertIn('system', reason)

    def test_guard_rejects_two_target_calls_in_scenario_mode(self):
        src = SCENARIO_DRIVER.replace(
            'int r = dt_target(&re, input, 0);',
            'int r = dt_target(&re, input, 0);\n'
            '    int r2 = dt_target(&re, input, 0);')
        reason = difftest.driver_guard(src, 'dt_target', scenario=True)
        self.assertIn('exactly once', reason)

    def test_guard_caps_the_setup_call_budget(self):
        src = SCENARIO_DRIVER.replace(
            'regcomp(&re, input, 0);',
            ' '.join('regcomp(&re, input, 0);' for _ in range(7)))
        reason = difftest.driver_guard(src, 'dt_target', scenario=True)
        self.assertIn('constructor calls', reason)

    def test_guard_admits_tier2_library_setup_names(self):
        src = SCENARIO_DRIVER.replace('regcomp(&re, input, 0);',
                                      'xmlInitParser();')
        self.assertIsNone(difftest.driver_guard(
            src, 'dt_target', scenario=True,
            extra_setup=('xmlInitParser',)))
        self.assertIsNotNone(difftest.driver_guard(
            src, 'dt_target', scenario=True))

    def test_setup_constructors_stay_synchronized_with_prompt(self):
        addendum = difftest.scenario_addendum()
        for name, purpose in difftest.DT_SETUP_CONSTRUCTORS.items():
            self.assertIn('%s -- %s' % (name, purpose), addendum)
        self.assertIn('xmlInitParser',
                      difftest.scenario_addendum(('xmlInitParser',)))

    # ---- cache keys / escalation ladder ----

    def test_cache_keys_distinguish_scenario_modes(self):
        opt = SimpleNamespace(model='m')
        plan = Plan('f', 'f', '')
        info = SimpleNamespace(line='declare i32 @f(i32)')
        keys = {difftest.dt_cache_key(kind, opt, plan, info)
                for kind in ('driver', 'driver-scenario',
                             'driver-scenario-lib')}
        self.assertEqual(len(keys), 3)

    def test_scenario_escalates_after_real_side_washout(self):
        with tempfile.TemporaryDirectory() as directory:
            opt, plan, info, parts = self._artifact_fixtures(directory)
            replies = iter([(BARE_DRIVER, {}), (INPUTS_JSON, {}),
                            (SCENARIO_DRIVER, {}), (INPUTS_JSON, {})])
            complete = lambda _messages: next(replies)
            cache = {}
            with mock.patch.object(difftest, '_ensure_real_link',
                                   return_value=None), \
                 mock.patch.object(difftest, '_real_side_probe',
                                   side_effect=[(False, 'rc=1 digest',
                                                 None),
                                                (True, None, 0)]) as probe:
                driver_path, inputs, error = difftest._obtain_artifacts(
                    opt, plan, info, parts, complete, cache, directory,
                    settings.clang())
            self.assertIsNone(error)
            self.assertEqual(Path(driver_path).read_text(), SCENARIO_DRIVER.strip())
        self.assertEqual(probe.call_count, 2)
        self.assertTrue(inputs)
        # both modes live under their own cache keys
        bare_key = difftest.dt_cache_key('driver', opt, plan, info)
        scen_key = difftest.dt_cache_key('driver-scenario', opt, plan,
                                         info)
        self.assertIn(bare_key, cache)
        self.assertIn(scen_key, cache)
        # the washout evidence reached the scenario request
        self.assertEqual(probe.call_args_list[0].args[0], opt)

    def test_scenario_keeps_bare_driver_when_real_side_usable(self):
        with tempfile.TemporaryDirectory() as directory:
            opt, plan, info, parts = self._artifact_fixtures(directory)
            replies = iter([(BARE_DRIVER, {}), (INPUTS_JSON, {})])
            complete = lambda _messages: next(replies)
            with mock.patch.object(difftest, '_ensure_real_link',
                                   return_value=None), \
                 mock.patch.object(difftest, '_real_side_probe',
                                   return_value=(True, None, 0)) as probe:
                driver_path, _inputs, error = difftest._obtain_artifacts(
                    opt, plan, info, parts, complete, {}, directory,
                    settings.clang())
            self.assertIsNone(error)
            self.assertEqual(Path(driver_path).read_text(), BARE_DRIVER.strip())
        # the iterator holds no further replies: no second driver was bought
        self.assertEqual(probe.call_count, 1)

    def test_scenario_never_probes_when_disabled(self):
        with tempfile.TemporaryDirectory() as directory:
            opt, plan, info, parts = self._artifact_fixtures(directory)
            opt.dt_scenario = False
            replies = iter([(BARE_DRIVER, {}), (INPUTS_JSON, {})])
            complete = lambda _messages: next(replies)
            with mock.patch.object(difftest, '_ensure_real_link',
                                   return_value=None), \
                 mock.patch.object(difftest, '_real_side_probe') as probe:
                driver_path, _inputs, error = difftest._obtain_artifacts(
                    opt, plan, info, parts, complete, {}, directory,
                    settings.clang())
            self.assertIsNone(error)
            self.assertEqual(Path(driver_path).read_text(), BARE_DRIVER.strip())
        probe.assert_not_called()

    def test_probe_verified_input_is_rotated_to_front(self):
        with tempfile.TemporaryDirectory() as directory:
            opt, plan, info, parts = self._artifact_fixtures(directory)
            opt.dt_inputs = 3
            many_inputs = ('[{"argv": ["a"], "stdin": ""},'
                           ' {"argv": ["b"], "stdin": ""},'
                           ' {"argv": ["c"], "stdin": ""}]')
            replies = iter([(BARE_DRIVER, {}), (many_inputs, {})])
            complete = lambda _messages: next(replies)
            with mock.patch.object(difftest, '_ensure_real_link',
                                   return_value=None), \
                 mock.patch.object(difftest, '_real_side_probe',
                                   return_value=(True, None, 2)):
                _path, inputs, error = difftest._obtain_artifacts(
                    opt, plan, info, parts, complete, {}, directory,
                    settings.clang())
        self.assertIsNone(error)
        # the battery probes inputs[0] and dies when that entry crashes
        # before the target call: the probe-verified one must lead
        self.assertEqual(inputs[0]['argv'], ['c'])
        self.assertEqual(inputs[1]['argv'], ['a'])

    def test_scenario_washout_on_every_tier_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            opt, plan, info, parts = self._artifact_fixtures(directory)
            replies = iter([(BARE_DRIVER, {}), (INPUTS_JSON, {}),
                            (SCENARIO_DRIVER, {}), (INPUTS_JSON, {})])
            complete = lambda _messages: next(replies)
            with mock.patch.object(difftest, '_ensure_real_link',
                                   return_value=None), \
                 mock.patch.object(difftest, '_real_side_probe',
                                   side_effect=[(False, 'digest-bare',
                                                 None),
                                                (False, 'digest-scenario',
                                                 None)]):
                _path, _inputs, error = difftest._obtain_artifacts(
                    opt, plan, info, parts, complete, {}, directory,
                    settings.clang())
        self.assertIn('no contract-usable input even with a scenario'
                      ' driver', error)
        self.assertIn('digest-scenario', error)

    def test_scenario_lib_tier_marks_plan_for_stub_side_link(self):
        with tempfile.TemporaryDirectory() as directory:
            opt, plan, info, parts = self._artifact_fixtures(directory)
            opt.dt_scenario_lib = True
            replies = iter([(BARE_DRIVER, {}), (INPUTS_JSON, {}),
                            (SCENARIO_DRIVER, {}), (INPUTS_JSON, {}),
                            (SCENARIO_DRIVER, {}), (INPUTS_JSON, {})])
            complete = lambda _messages: next(replies)
            with mock.patch.object(difftest, '_ensure_real_link',
                                   return_value=None), \
                 mock.patch.object(difftest, '_scenario_lib_candidates',
                                   return_value=('setup_fn',)), \
                 mock.patch.object(difftest, '_real_side_probe',
                                   side_effect=[(False, 'd1', None),
                                                (False, 'd2', None),
                                                (True, None, 0)]):
                driver_path, _inputs, error = difftest._obtain_artifacts(
                    opt, plan, info, parts, complete, {}, directory,
                    settings.clang())
            self.assertIsNone(error)
            self.assertEqual(Path(driver_path).read_text(), SCENARIO_DRIVER.strip())
        self.assertIn('f', opt._dt_scenario_lib_plans)

    # ---- Tier-2: candidates and the stub-wins link ----

    def test_scenario_lib_candidates_use_callgraph_adjacency(self):
        ir = ('declare i32 @target(i32)\n'
              'declare i32 @setup_fn(i32)\n'
              'declare i32 @other_fn(i32)\n'
              '\n'
              'define i32 @caller(i32 %x) {\n'
              '  %1 = call i32 @target(i32 %x)\n'
              '  %2 = call i32 @setup_fn(i32 %1)\n'
              '  %3 = call i32 @local_helper(i32 %2)\n'
              '  ret i32 %3\n'
              '}\n'
              '\n'
              'define i32 @local_helper(i32 %x) {\n'
              '  ret i32 %x\n'
              '}\n')
        fake = SimpleNamespace(ir_text=lambda bc, cache_dir=None: ir)
        with tempfile.TemporaryDirectory() as directory:
            bc = Path(directory) / 'prog.bc'
            bc.touch()
            opt = SimpleNamespace(inputs=[str(bc)], cache_dir=directory,
                                  real_lib_map={'setup_fn': []})
            with mock.patch.dict(sys.modules, {'cxprobe': fake}):
                candidates = difftest._scenario_lib_candidates(
                    opt, Plan('target', 'target', ''))
        # other_fn is not resolvable through the real-lib map, local_helper
        # has no external declaration: only setup_fn qualifies
        self.assertEqual(candidates, ('setup_fn',))

    def test_scenario_lib_candidates_empty_without_bitcode(self):
        opt = SimpleNamespace(inputs=[], cache_dir='/tmp')
        self.assertEqual(difftest._scenario_lib_candidates(
            opt, Plan('target', 'target', '')), ())

    def test_scenario_lib_candidates_find_constructors_by_type(self):
        ir = ('declare %struct.Conn* @make_conn(i32)\n'
              'declare i32 @use_conn(%struct.Conn* noundef, i8* noundef)\n'
              'declare i32 @other_fn(i32)\n'
              '\n'
              'define i32 @caller(i32 %x) {\n'
              '  %1 = call i32 @use_conn(%struct.Conn* null, i8* null)\n'
              '  ret i32 %1\n'
              '}\n')
        fake = SimpleNamespace(ir_text=lambda bc, cache_dir=None: ir)
        with tempfile.TemporaryDirectory() as directory:
            bc = Path(directory) / 'prog.bc'
            bc.touch()
            opt = SimpleNamespace(inputs=[str(bc)], cache_dir=directory,
                                  real_lib_map={'make_conn': []})
            with mock.patch.dict(sys.modules, {'cxprobe': fake}):
                candidates = difftest._scenario_lib_candidates(
                    opt, Plan('use_conn', 'use_conn', ''))
        # the constructor outranks call-graph adjacency: it returns the
        # pointee of the target's pointer parameter
        self.assertEqual(candidates, ('make_conn',))

    @unittest.skipUnless(sys.platform.startswith('linux'),
                         'Scenario native linking requires a Linux linker')
    def test_scenario_lib_links_archive_with_stub_target_winning(self):
        archiver = difftest.shutil.which('ar')
        if archiver is None:
            self.skipTest('ar not available')
        clang = settings.clang()
        with tempfile.TemporaryDirectory() as directory:
            real = Path(directory) / 'real.c'
            real.write_text('int target(int x) { return x * 1000; }\n'
                            'int setup_state(void) { return 42; }\n')
            stub = Path(directory) / 'stub.c'
            stub.write_text('int target(int x) { return x + 1; }\n')
            driver = Path(directory) / 'driver.c'
            driver.write_text('int target(int x);\n'
                              'int setup_state(void);\n'
                              'int main(void) {\n'
                              '    return (target(1) == 2'
                              ' && setup_state() == 42) ? 0 : 1;\n'
                              '}\n')
            obj = Path(directory) / 'real.o'
            archive = Path(directory) / 'libreal.a'
            prog = Path(directory) / 'prog.bin'
            for cmd in ([clang, '-c', str(real), '-o', str(obj)],
                        [archiver, 'rcs', str(archive), str(obj)]):
                proc = subprocess.run(cmd, capture_output=True, text=True)
                self.assertEqual(proc.returncode, 0, proc.stderr)
            # the archive member is pulled for setup_state and then
            # collides with the stub's target: without the flag the link
            # MUST fail (static archive on-demand semantics)
            proc = subprocess.run(
                [clang, str(driver), str(stub), str(archive),
                 '-o', str(prog)], capture_output=True, text=True)
            self.assertNotEqual(proc.returncode, 0)
            # stub first + the flag: ld keeps the first (stub) target
            # definition, the archive still supplies setup_state
            proc = subprocess.run(
                [clang, str(driver), str(stub),
                 '-Wl,--allow-multiple-definition', str(archive),
                 '-o', str(prog)], capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            proc = subprocess.run([str(prog)], capture_output=True)
            self.assertEqual(proc.returncode, 0)

    def test_run_battery_honours_scenario_lib_plan_marker(self):
        washout = {'rc': 1, 'contract': '', 'stderr': '', 'timeout': False,
                   'log': ''}
        kit = SimpleNamespace(
            settings=SimpleNamespace(clang=lambda: '/usr/bin/clang'),
            stub=FakeStub())
        parts = SimpleNamespace(signature_line='int f(int p0)',
                                typedef_lines=[])
        plan = Plan('f', 'f', '')
        info = SimpleNamespace(line='declare i32 @f(i32)')
        with tempfile.TemporaryDirectory() as directory:
            opt = SimpleNamespace(cache_dir=directory,
                                  _dt_scenario_lib_plans={'f'})
            with mock.patch.object(difftest, '_build',
                                   return_value=('r.bin', 's.bin', None)
                                   ) as build, \
                 mock.patch.object(difftest, 'run_once',
                                   return_value=dict(washout)):
                difftest.run_battery(kit, opt, plan, parts, 'driver.c',
                                     [{'argv': ['0']}], 'stub.c', directory,
                                     info)
            self.assertIs(build.call_args.args[-1], True)

            opt2 = SimpleNamespace(cache_dir=directory)
            with mock.patch.object(difftest, '_build',
                                   return_value=('r.bin', 's.bin', None)
                                   ) as build, \
                 mock.patch.object(difftest, 'run_once',
                                   return_value=dict(washout)):
                difftest.run_battery(kit, opt2, plan, parts, 'driver.c',
                                     [{'argv': ['0']}], 'stub.c', directory,
                                     info)
            self.assertIs(build.call_args.args[-1], False)


class SandboxDbusTests(unittest.TestCase):
    """Private in-sandbox D-Bus session for dbus_/g_bus_/sd_bus_ targets."""

    def test_needs_dbus_truth_table(self):
        for name in ('dbus_bus_get', 'dbus_connection_send',
                     'g_bus_own_name', 'sd_bus_open'):
            self.assertTrue(difftest._needs_dbus(Plan(name, name, '')),
                            name)
        # the source spelling counts too (mangled IR name)
        self.assertTrue(difftest._needs_dbus(
            Plan('_Z4test', 'dbus_connection_send()', '')))
        for name in ('malloc', 'regexec', 'xdbus_helper', ''):
            self.assertFalse(difftest._needs_dbus(Plan(name, name, '')),
                             name)

    def test_session_wrap_constructs_command_and_runtime_dir(self):
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(difftest.shutil, 'which',
                                   return_value='/usr/bin/dbus-run-session'):
                prefix, env = difftest._dbus_session_wrap(directory)
            xdg = os.path.join(os.path.abspath(directory), 'xdg')
            conf = os.path.join(xdg, 'session.conf')
            self.assertEqual(prefix, ['/usr/bin/dbus-run-session',
                                      '--config-file', conf, '--'])
            self.assertEqual(env['XDG_RUNTIME_DIR'], xdg)
            self.assertEqual(env['TMPDIR'], xdg)
            self.assertTrue(os.path.isdir(xdg))
            self.assertEqual(os.stat(xdg).st_mode & 0o777, 0o700)
            # the daemon's socket must live under the writable workdir,
            # never on the sandbox's read-only /tmp
            text = Path(conf).read_text()
            self.assertIn('unix:path=%s' % os.path.join(xdg, 'bus'), text)
            self.assertNotIn('unix:tmpdir=/tmp', text)

    def test_session_wrap_missing_tool_falls_back_and_warns_once(self):
        import warnings
        with tempfile.TemporaryDirectory() as directory:
            difftest._DBUS_WARNED = False
            with mock.patch.object(difftest.shutil, 'which',
                                   return_value=None):
                with warnings.catch_warnings(record=True) as seen:
                    warnings.simplefilter('always')
                    first = difftest._dbus_session_wrap(directory)
                    second = difftest._dbus_session_wrap(directory)
        self.assertEqual(first, (None, {}))
        self.assertEqual(second, (None, {}))
        self.assertEqual(len([w for w in seen
                              if issubclass(w.category, RuntimeWarning)]),
                         1)

    @mock.patch('shutil.which', side_effect=lambda name: '/usr/bin/' + name)
    def test_run_once_dbus_command_construction(self, _which):
        captured = {}

        class FakeProc:
            returncode = 0

            def communicate(self, timeout=None):
                return b'', b''

            def kill(self):
                pass

        def fake_popen(cmd, **kwargs):
            captured['cmd'] = cmd
            captured['env'] = kwargs.get('env')
            return FakeProc()

        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(difftest.subprocess, 'Popen',
                                   fake_popen):
                difftest.run_once('/bin/driver', ['x'], {}, 5, directory,
                                  dbus=True)
            cmd = captured['cmd']
            names = [os.path.basename(arg) for arg in cmd]
            self.assertIn('dbus-run-session', names)
            self.assertLess(names.index('dbus-run-session'),
                            cmd.index('/bin/driver'))
            self.assertIn('--config-file', cmd)
            self.assertEqual(captured['env']['XDG_RUNTIME_DIR'],
                             os.path.join(os.path.abspath(directory),
                                          'xdg'))
            # and the plain path is untouched
            with mock.patch.object(difftest.subprocess, 'Popen',
                                   fake_popen):
                difftest.run_once('/bin/driver', ['x'], {}, 5, directory)
            self.assertNotIn('dbus-run-session',
                             [os.path.basename(a)
                              for a in captured['cmd']])
            self.assertNotIn('XDG_RUNTIME_DIR', captured['env'])

    def test_run_once_dbus_e2e_private_bus_ping(self):
        send = difftest.shutil.which('dbus-send')
        if not (send and difftest.shutil.which('dbus-run-session')
                and difftest.shutil.which('bwrap')):
            self.skipTest('dbus-run-session/dbus-send/bwrap not available')
        with tempfile.TemporaryDirectory() as directory:
            run = difftest.run_once(
                send, ['--session', '--print-reply',
                       '--dest=org.freedesktop.DBus', '/',
                       'org.freedesktop.DBus.Peer.Ping'],
                {}, 15, directory, dbus=True)
            self.assertEqual(run['rc'], 0, run['stderr'])
            # without the wrap there is no bus to ping
            plain = difftest.run_once(
                send, ['--session', '--dest=org.freedesktop.DBus', '/',
                       'org.freedesktop.DBus.Peer.Ping'],
                {}, 15, directory)
            self.assertNotEqual(plain['rc'], 0)


if __name__ == '__main__':
    unittest.main()
