#!/usr/bin/env python3
###############################################################################
#
# IKOS API spec -- analysis-side audit gate for linked stubs
#
# Contact: ikos@lists.nasa.gov
#
# Notices:
#
# Copyright (c) 2018-2023 United States Government as represented by the
# Administrator of the National Aeronautics and Space Administration.
# All Rights Reserved.
#
# Disclaimers:
#
# No Warranty: THE SUBJECT SOFTWARE IS PROVIDED "AS IS" WITHOUT WARRANTY OF
# ANY KIND, EITHER EXPRESSED, IMPLIED, OR STATUTORY, INCLUDING, BUT NOT
# LIMITED TO, ANY WARRANTY THAT THE SUBJECT SOFTWARE WILL CONFORM TO
# SPECIFICATIONS, ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
# PARTICULAR PURPOSE, OR NONINFRINGEMENT.  IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.
#
###############################################################################
"""Analysis-side audit gate for stub-linked whole-program bitcode.

The compile, signature and differential gates all run on the stub alone.
Two classes of stub defects only become visible AFTER ``ikos-stub link``
splices the stubs into the program and IKOS analyzes the linked artifact
(E2 experiment findings):

* definite errors inside the stub sources themselves.  E2 example: a
  stat64 stub doing ``__ikos_forget_mem(p1, 256)`` writes past the
  144-byte ``struct stat`` -- across 30 projects, 378 stub-side errors,
  97.6% of them buffer-overflow;
* reachability loss from under-approximation.  E2 example: a getopt_long
  stub whose return domain was tightened to [-1, 98] prunes ASCII
  'o' (111), turning the whole ``case 'o':`` branch into dead code --
  memcached "lost" 74 baseline errors this way, a coverage loss, not a
  precision gain.

This gate compares the analysis database of the plain program against
the one of the stub-linked program and fails (exit 1) when either:

* the stubbed database carries more definite (status=error) checks whose
  source file lives under a stubs directory than ``--max-stub-errors``
  (default 0), or
* the project-side unreachable statement count grows by more than
  ``--max-unreach-delta`` (default 0.05) of the baseline project-side
  statement total -- the under-approximation warning.

Counting convention (the R16/E2 yardstick): checks are first grouped by
(statement_id, call_context_id); a group whose statuses are all
unreachable (3) counts as one unreachable statement and nothing else;
within every other group, error/warning/safe checks are deduplicated by
(kind, operands, info).  "Stub side" is any check whose statement file
path matches ``(?:^|[\\/])stubs[^\\/]*[\\/]`` (also covers variant
directory names such as stubs-fix1); everything else is project side.

Tool failures (ikos/ikos-stub missing, nonzero exit, timeout) exit 2.

Usage:
  auditgate.py [--baseline X.db] [--stub-dir DIR] [--max-stub-errors N]
               [--max-unreach-delta PCT] [--keep-temps] [--json PATH]
               [--ikos-bin PATH] [--ikos-stub-bin PATH] [--timeout SEC]
               X.bc
"""
import argparse
import json
import os
import os.path
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time

EXIT_PASS = 0
EXIT_GATE = 1
EXIT_TOOL = 2

DEFAULT_TIMEOUT = 600
DEFAULT_IKOS_BIN_DIR = os.environ.get('IKOS_PREFIX', '/home/user/ikos-api-spec-install') + '/bin'

STUB_PATH_RE = re.compile(r'(?:^|[\/])stubs[^\/]*[\/]')

STATUS_SAFE = 0
STATUS_WARNING = 1
STATUS_ERROR = 2
STATUS_UNREACHABLE = 3

STATUS_NAMES = {STATUS_SAFE: 'safe',
                STATUS_WARNING: 'warning',
                STATUS_ERROR: 'error',
                STATUS_UNREACHABLE: 'unreachable'}

# CheckKind enum, analyzer/include/ikos/analyzer/checker/kind.hpp; the
# database stores the raw enum value (checks.kind), markers included.
CHECK_KIND_NAMES = [
    'Unreachable',
    'UnexpectedOperand',
    'UninitializedVariable',
    'Assert',
    'DivisionByZero',
    'ShiftCount',
    '_BeginIntOverflow',
    'SignedIntUnderflow',
    'SignedIntOverflow',
    'UnsignedIntUnderflow',
    'UnsignedIntOverflow',
    '_EndIntOverflow',
    'NullPointerDereference',
    'NullPointerComparison',
    'InvalidPointerComparison',
    'PointerComparison',
    'PointerOverflow',
    'InvalidPointerDereference',
    'UnknownMemoryAccess',
    'UnalignedPointer',
    '_BeginBufferOverflow',
    'BufferOverflowGets',
    'BufferOverflow',
    '_EndBufferOverflow',
    '_BeginSoundness',
    'IgnoredStore',
    'IgnoredMemoryCopy',
    'IgnoredMemoryMove',
    'IgnoredMemorySet',
    'IgnoredFree',
    'IgnoredCallSideEffectOnPointerParameter',
    'IgnoredCallSideEffect',
    'RecursiveFunctionCall',
    '_EndSoundness',
    '_BeginFunctionCall',
    'FunctionCallInlineAssembly',
    'UnknownFunctionCallPointer',
    'FunctionCall',
    '_EndFunctionCall',
    'Free',
]


class ToolError(Exception):
    """A missing/failing/timed-out external tool: exit 2."""


def kind_name(kind):
    if 0 <= kind < len(CHECK_KIND_NAMES):
        return CHECK_KIND_NAMES[kind]
    return 'kind%d' % kind


def _warn(message):
    sys.stderr.write('warning: %s\n' % message)


#############################
# External tools #
#############################

def resolve_tool(name, explicit):
    """Path of an external tool: --*-bin flag, then PATH, then the
    well-known install prefix."""
    if explicit:
        if os.path.isfile(explicit) and os.access(explicit, os.X_OK):
            return explicit
        raise ToolError('%s is not an executable file' % explicit)
    found = shutil.which(name)
    if found:
        return found
    candidate = os.path.join(DEFAULT_IKOS_BIN_DIR, name)
    if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
        return candidate
    raise ToolError('%s not found (not on PATH, not under %s); '
                    'pass --%s-bin' % (name, DEFAULT_IKOS_BIN_DIR, name))


def run_command(cmd, timeout, what, log_path=None):
    """Run a tool, capturing its output; ToolError on timeout/failure.

    Returns the wall time in seconds.  The combined output is written to
    log_path when given (kept by --keep-temps)."""
    started = time.time()
    try:
        proc = subprocess.run(cmd,
                              stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT,
                              universal_newlines=True,
                              errors='replace',
                              timeout=timeout)
    except subprocess.TimeoutExpired:
        raise ToolError('%s timed out after %ds (raise with --timeout)'
                        % (what, timeout))
    except OSError as error:
        raise ToolError('%s could not run: %s' % (what, error))
    wall = round(time.time() - started, 3)
    if log_path:
        try:
            with open(log_path, 'w') as stream:
                stream.write(proc.stdout or '')
        except OSError:
            pass
    if proc.returncode != 0:
        tail = (proc.stdout or '').strip()[-400:]
        raise ToolError('%s failed (exit %d)%s'
                        % (what, proc.returncode,
                           tail and (': ' + tail) or ''))
    return wall


def run_ikos(ikos_bin, bitcode, db_path, timeout, log_path):
    """ikos X.bc -o X.db with the R16 default parameters (no -a/-d)."""
    wall = run_command([ikos_bin, bitcode, '-o', db_path, '--color=no'],
                       timeout, 'ikos on %s' % bitcode, log_path)
    if not os.path.isfile(db_path):
        raise ToolError('ikos produced no database %s' % db_path)
    return wall


#############################
# Database reading #
#############################

def _connect_ro(db_path):
    uri = 'file:%s?mode=ro' % db_path.replace('?', '%3f').replace('#',
                                                                  '%23')
    return sqlite3.connect(uri, uri=True, timeout=10)


def read_stats(db_path):
    """The E2 counting convention over one analysis database.

    Returns {'project': {'error','warning','safe','unreachable'},
             'unreach_by_loc': {(file, line): count},
             'stub_errors': [item]} where item = {'file', 'line',
             'kind', 'kind_name', 'operands', 'info'} deduplicated by
             (statement_id, call_context_id, kind, operands, info)."""
    conn = None
    try:
        conn = _connect_ro(db_path)
        rows = conn.execute(
            'SELECT c.statement_id, c.call_context_id, c.kind, c.status,'
            ' c.operands, c.info, f.path, s.line'
            ' FROM checks c'
            ' JOIN statements s ON c.statement_id = s.id'
            ' LEFT JOIN files f ON s.file_id = f.id').fetchall()
    except sqlite3.Error as error:
        raise ToolError('cannot read checks from %s: %s' % (db_path,
                                                            error))
    finally:
        if conn is not None:
            conn.close()

    groups = {}
    for statement_id, context_id, kind, status, operands, info, path, \
            line in rows:
        key = (statement_id, context_id)
        groups.setdefault(key, []).append(
            (kind, status, operands, info, path, line))

    project = {'error': 0, 'warning': 0, 'safe': 0, 'unreachable': 0}
    unreach_by_loc = {}
    stub_errors = {}
    for checks in groups.values():
        statuses = {status for _, status, _, _, _, _ in checks}
        path = checks[0][4]
        line = checks[0][5]
        stub_side = bool(path) and bool(STUB_PATH_RE.search(path))
        if statuses == {STATUS_UNREACHABLE}:
            if not stub_side:
                project['unreachable'] += 1
                loc = (path or '?', line or 0)
                unreach_by_loc[loc] = unreach_by_loc.get(loc, 0) + 1
            continue
        seen = set()
        for kind, status, operands, info, _, _ in checks:
            if status not in (STATUS_SAFE, STATUS_WARNING, STATUS_ERROR):
                continue
            dedup = (kind, operands, info, status)
            if dedup in seen:
                continue
            seen.add(dedup)
            if stub_side:
                if status == STATUS_ERROR:
                    item_key = (path, line, kind, operands, info)
                    stub_errors.setdefault(item_key,
                                           {'file': path,
                                            'line': line,
                                            'kind': kind,
                                            'kind_name': kind_name(kind),
                                            'operands': operands,
                                            'info': info})
            else:
                project[STATUS_NAMES[status]] += 1

    errors = sorted(stub_errors.values(),
                    key=lambda item: (item['file'], item['line'],
                                      item['kind']))
    return {'project': project,
            'unreach_by_loc': unreach_by_loc,
            'stub_errors': errors}


#############################
# Gate #
#############################

def build_report(bitcode, baseline_db, baseline_provided, stubbed_bc,
                 stub_db, stub_stats, base_stats, max_stub_errors,
                 max_unreach_delta, timings):
    """The full gate report (dict, JSON-serializable)."""
    base = base_stats['project']
    stub = stub_stats['project']
    delta = {name: stub[name] - base[name]
             for name in ('error', 'warning', 'safe', 'unreachable')}
    base_total = max(sum(base.values()), 1)
    unreach_delta = (stub['unreachable']
                     - base['unreachable']) / float(base_total)

    increases = []
    for loc, stub_count in stub_stats['unreach_by_loc'].items():
        grew = stub_count - base_stats['unreach_by_loc'].get(loc, 0)
        if grew > 0:
            increases.append({'file': loc[0], 'line': loc[1],
                              'baseline': base_stats['unreach_by_loc']
                              .get(loc, 0),
                              'stubbed': stub_count, 'delta': grew})
    increases.sort(key=lambda item: (-item['delta'], item['file'],
                                     item['line']))

    stub_error_count = len(stub_stats['stub_errors'])
    gates = {
        'stub_errors': {
            'count': stub_error_count,
            'threshold': max_stub_errors,
            'pass': stub_error_count <= max_stub_errors,
        },
        'unreach_delta': {
            'rate': round(unreach_delta, 6),
            'threshold': max_unreach_delta,
            'base_total': base_total,
            'pass': unreach_delta <= max_unreach_delta,
        },
    }
    passed = all(gate['pass'] for gate in gates.values())
    return {
        'bitcode': bitcode,
        'baseline_db': baseline_db,
        'baseline_provided': baseline_provided,
        'stubbed_bc': stubbed_bc,
        'stub_db': stub_db,
        'gates': gates,
        'pass': passed,
        'project': {'baseline': base, 'stubbed': stub, 'delta': delta},
        'stub_errors': stub_stats['stub_errors'],
        'top_unreach_increase': increases[:10],
        'timings': timings,
    }


def _short_info(info, width=140):
    text = ' '.join(str(info or '').split())
    if len(text) > width:
        text = text[:width - 3] + '...'
    return text


def print_summary(report):
    """The human-readable stdout summary."""
    print('auditgate: %s' % report['bitcode'])
    print('baseline: %s%s'
          % (report['baseline_db'],
             report['baseline_provided'] and ' (provided)' or ''))
    print('stubbed:  %s' % report['stub_db'])

    stub_gate = report['gates']['stub_errors']
    verdict = stub_gate['pass'] and 'PASS' or 'FAIL'
    print('stub-side definite errors: %d (threshold %d) [%s]'
          % (stub_gate['count'], stub_gate['threshold'], verdict))
    errors = report['stub_errors']
    shown = errors if not stub_gate['pass'] else errors[:20]
    for item in shown:
        print('  %s:%s: %s: %s'
              % (item['file'], item['line'], item['kind_name'],
                 _short_info(item['info'])))
    if len(errors) > len(shown):
        print('  ... and %d more (see --json report)'
              % (len(errors) - len(shown)))

    base = report['project']['baseline']
    stub = report['project']['stubbed']
    delta = report['project']['delta']
    print('project-side counts (baseline -> stubbed, delta):')
    for name in ('error', 'warning', 'safe', 'unreachable'):
        print('  %-11s %6d -> %-6d (%+d)'
              % (name, base[name], stub[name], delta[name]))

    unreach_gate = report['gates']['unreach_delta']
    verdict = unreach_gate['pass'] and 'PASS' or 'FAIL'
    print('unreachable delta rate: %.4f (threshold %.4f) [%s]'
          % (unreach_gate['rate'], unreach_gate['threshold'], verdict))
    if not unreach_gate['pass']:
        print('top unreachable increases (project side):')
        for item in report['top_unreach_increase']:
            print('  %s:%s: %d -> %d (%+d)'
                  % (item['file'], item['line'], item['baseline'],
                     item['stubbed'], item['delta']))

    print('gate: %s' % (report['pass'] and 'PASS' or 'FAIL'))


#############################
# Command line #
#############################

def parse_args(argv):
    parser = argparse.ArgumentParser(
        prog='auditgate.py',
        description='Analysis-side audit gate: compare the IKOS databases'
                    ' of a program before/after ikos-stub link, and fail'
                    ' on stub-side definite errors or project-side'
                    ' reachability loss.')
    parser.add_argument('bitcode',
                        metavar='X.bc',
                        help='Whole-program LLVM bitcode to audit')
    parser.add_argument('--baseline',
                        dest='baseline',
                        metavar='X.db',
                        help='Existing baseline analysis database of X.bc'
                             ' (default: computed by running ikos on X.bc)')
    parser.add_argument('--stub-dir',
                        dest='stub_dir',
                        metavar='DIR',
                        default='stubs',
                        help='Stub library directory handed to'
                             ' ikos-stub link (default: %(default)s)')
    parser.add_argument('--max-stub-errors',
                        dest='max_stub_errors',
                        metavar='N',
                        type=int,
                        default=0,
                        help='Maximum tolerated definite errors in stub'
                             ' sources (default: %(default)s)')
    parser.add_argument('--max-unreach-delta',
                        dest='max_unreach_delta',
                        metavar='PCT',
                        type=float,
                        default=0.05,
                        help='Maximum tolerated growth rate of project-side'
                             ' unreachable statements, as a fraction of the'
                             ' baseline project-side statement total'
                             ' (default: %(default)s)')
    parser.add_argument('--keep-temps',
                        dest='keep_temps',
                        action='store_true',
                        help='Keep the temporary directory (linked bitcode,'
                             ' databases, ikos logs)')
    parser.add_argument('--json',
                        dest='json_path',
                        metavar='PATH',
                        help='Write the full gate report as JSON to PATH')
    parser.add_argument('--ikos-bin',
                        dest='ikos_bin',
                        metavar='PATH',
                        help='Path of the ikos executable (default: PATH,'
                             ' then ' + DEFAULT_IKOS_BIN_DIR + ')')
    parser.add_argument('--ikos-stub-bin',
                        dest='ikos_stub_bin',
                        metavar='PATH',
                        help='Path of the ikos-stub executable (default:'
                             ' PATH, then ' + DEFAULT_IKOS_BIN_DIR + ')')
    parser.add_argument('--timeout',
                        dest='timeout',
                        metavar='SEC',
                        type=int,
                        default=DEFAULT_TIMEOUT,
                        help='Wall-clock budget per tool invocation'
                             ' (default: %(default)s)')
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)

    if not os.path.isfile(args.bitcode):
        sys.stderr.write('error: no such bitcode file: %s\n'
                         % args.bitcode)
        return EXIT_TOOL
    if args.baseline and not os.path.isfile(args.baseline):
        sys.stderr.write('error: no such baseline database: %s\n'
                         % args.baseline)
        return EXIT_TOOL
    if not os.path.isdir(args.stub_dir):
        sys.stderr.write('error: no such stub directory: %s\n'
                         % args.stub_dir)
        return EXIT_TOOL

    try:
        ikos_bin = resolve_tool('ikos', args.ikos_bin)
        ikos_stub_bin = resolve_tool('ikos-stub', args.ikos_stub_bin)
    except ToolError as error:
        sys.stderr.write('error: %s\n' % error)
        return EXIT_TOOL

    stem = os.path.splitext(os.path.basename(args.bitcode))[0]
    tmp_dir = tempfile.mkdtemp(prefix='auditgate-')
    timings = {}
    try:
        # 1. baseline database
        if args.baseline:
            baseline_db = args.baseline
            baseline_provided = True
        else:
            baseline_db = os.path.join(tmp_dir, 'base.db')
            baseline_provided = False
            timings['baseline_ikos'] = run_ikos(
                ikos_bin, args.bitcode, baseline_db, args.timeout,
                os.path.join(tmp_dir, 'base.ikos.log'))

        # 2. link stubs and analyze the linked bitcode
        stubbed_bc = os.path.join(tmp_dir, stem + '.stubbed.bc')
        stub_db = os.path.join(tmp_dir, 'stub.db')
        timings['ikos_stub_link'] = run_command(
            [ikos_stub_bin, 'link', '--stub-dir', args.stub_dir,
             '-o', stubbed_bc, args.bitcode],
            args.timeout, 'ikos-stub link',
            os.path.join(tmp_dir, 'link.log'))
        if not os.path.isfile(stubbed_bc):
            raise ToolError('ikos-stub link produced no %s' % stubbed_bc)
        timings['stub_ikos'] = run_ikos(
            ikos_bin, stubbed_bc, stub_db, args.timeout,
            os.path.join(tmp_dir, 'stub.ikos.log'))

        # 3. statistics and 4. gate decision
        base_stats = read_stats(baseline_db)
        stub_stats = read_stats(stub_db)
        report = build_report(args.bitcode, baseline_db,
                              baseline_provided, stubbed_bc, stub_db,
                              stub_stats, base_stats,
                              args.max_stub_errors,
                              args.max_unreach_delta, timings)
    except ToolError as error:
        sys.stderr.write('error: %s\n' % error)
        if args.keep_temps:
            sys.stderr.write('temporary files kept in %s\n' % tmp_dir)
        else:
            shutil.rmtree(tmp_dir, ignore_errors=True)
        return EXIT_TOOL

    report['temps_kept'] = args.keep_temps and tmp_dir or None
    print_summary(report)
    if args.json_path:
        try:
            with open(args.json_path, 'w') as stream:
                json.dump(report, stream, indent=1, sort_keys=True)
                stream.write('\n')
        except OSError as error:
            _warn('cannot write JSON report %s: %s' % (args.json_path,
                                                       error))
    if args.keep_temps:
        print('temporary files kept in %s' % tmp_dir)
    else:
        shutil.rmtree(tmp_dir, ignore_errors=True)
    return EXIT_PASS if report['pass'] else EXIT_GATE


if __name__ == '__main__':
    sys.exit(main())
