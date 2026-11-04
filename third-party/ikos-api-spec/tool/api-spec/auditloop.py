#!/usr/bin/env python3
###############################################################################
#
# IKOS API spec -- outer repair loop driven by analysis-side audit findings
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
"""Outer repair loop: feed analysis-side audit findings back to the LLM.

auditgate.py detects two stub defect classes that only exist after
``ikos-stub link`` + whole-program analysis (E2 experiment findings: 378
stub-side definite errors across 30 projects, 97.6% buffer-overflow, the
stat64 ``__ikos_forget_mem(p1, 256)`` over-write of a 144-byte struct;
and memcached losing 74 baseline errors to a getopt_long stub whose
tightened return domain pruned ``case 'o':`` -- a coverage loss, not a
precision gain).  This driver closes the loop for the first class:

1. audit every input bitcode exactly as auditgate.py does (baseline db
   reused from the sibling X.db unless --baseline-suffix says otherwise,
   ikos-stub link + ikos for the stubbed side); the stub-side classifier
   is extended beyond the stubs/ path convention to anything under the
   real --stub-dir, because ikos-stub resolves the directory to its real
   path before compiling and a directory named without a stubs prefix
   would otherwise misfile its errors as project-side;
2. aggregate the stub-side definite errors by stub file -- the file stem
   is the function name ``ikos-api-spec.py generate --only`` accepts;
3. regenerate each offending stub with the findings as --repair-hint
   (the hint is part of the response cache key, so a new finding never
   hits the previous round's cached body);
4. re-audit; converge when the stub side is clean, otherwise feed the
   NEW findings back, up to --max-rounds repair rounds;
5. --prune-failed removes the stub files of functions still violating
   after the last round: an unmodeled function (ikos-stub link warns and
   leaves it external) is strictly better than a stub with a proven
   definite error.

Stub-database eviction: the battery that admits a body into the database
is strictly weaker than this audit (E2: the stat64
``__ikos_forget_mem(p1, 4096)`` body sits in the database marked
verified), and ``generate`` consults the database BEFORE the LLM,
ignoring --repair-hint.  A convicted function's database entries are
therefore evicted before regeneration -- the database is a cache, never
an authority, and a whole-program proof of a definite error outranks the
battery that vouched for the entry.  Every eviction is recorded in the
report.

The unreachable delta rate is REPORTED ONLY, never turned into a repair
hint: an under-approximation finding (the memcached case) cannot yet be
attributed to a single stub function with enough evidence -- the delta is
a whole-program signal.

Exit codes: 0 = converged clean; 1 = violations remain (or stubs were
pruned); 2 = tool failure (ikos/ikos-stub/ikos-api-spec unusable, audit
infrastructure error).

Usage:
  auditloop.py [--stub-dir DIR] [--db-dir DIR] [--provider P] [--model M]
               [--real-libs PATH] [--max-rounds N] [--jobs N]
               [--prune-failed] [--keep-temps] [--json PATH]
               [--generate-opt OPT]... [--baseline-suffix SUF]
               X.bc [X.bc ...]
"""
import argparse
import difflib
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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import auditgate  # noqa: E402
import stubdb  # noqa: E402

from concurrent.futures import ThreadPoolExecutor  # noqa: E402

EXIT_PASS = 0
EXIT_GATE = 1
EXIT_TOOL = 2

HERE = os.path.dirname(os.path.abspath(__file__))
API_SPEC_PY = os.path.join(HERE, 'ikos-api-spec.py')
INSTALL_BIN_DIR = os.environ.get('IKOS_PREFIX', '/home/user/ikos-api-spec-install') + '/bin'

DEFAULT_MAX_ROUNDS = 2
DEFAULT_JOBS = 4
DIFF_EXCERPT_LINES = 60

IR_NAME_RE = re.compile(r'^\s*\*?\s*ir-name:\s*(\S+)', re.MULTILINE)
SOURCE_NAME_RE = re.compile(r'^\s*\*?\s*source-name:\s*(\S+)',
                            re.MULTILINE)

UNREACH_NOTE = ('unreachable delta rates are reported for information'
                ' only, never used as a repair hint: an'
                ' under-approximation finding (E2: memcached losing 74'
                ' baseline errors to a getopt_long stub) cannot yet be'
                ' attributed to a single stub function with enough'
                ' evidence -- it is a whole-program signal')


#############################
# Findings #
#############################

def header_names(stub_file):
    """The (ir-name, source-name) pair of a stub header comment."""
    try:
        with open(stub_file) as stream:
            head = stream.read(2048)
    except OSError:
        return None, None

    def _name(regex):
        match = regex.search(head)
        return match and match.group(1) or None

    return _name(IR_NAME_RE), _name(SOURCE_NAME_RE)


def _info_summary(info, width=220):
    """The access_size/offset/points_to key fields of a check info blob.

    The info column is JSON for buffer-overflow style checks; anything
    unparseable falls back to a whitespace-collapsed truncation."""
    try:
        data = json.loads(info)
    except (TypeError, ValueError):
        return auditgate._short_info(info, width)
    if not isinstance(data, dict):
        return auditgate._short_info(info, width)
    parts = []
    for key in ('access_size', 'offset'):
        if key in data:
            parts.append('%s=%s' % (key, json.dumps(data[key],
                                                    sort_keys=True)))
    points_to = data.get('points_to')
    if isinstance(points_to, list):
        sizes = [json.dumps(item['size'], sort_keys=True)
                 for item in points_to[:3]
                 if isinstance(item, dict) and 'size' in item]
        parts.append('points_to=%d object(s)%s'
                     % (len(points_to),
                        sizes and (' sizes=' + ','.join(sizes)) or ''))
    return '; '.join(parts) or auditgate._short_info(info, width)


def build_hint(errors):
    """One repair hint text merging every error of one stub function."""
    findings = '; '.join('%s at %s:%s -- %s'
                         % (item['kind_name'], item['file'], item['line'],
                            _info_summary(item['info']))
                         for item in errors)
    return ('Whole-program IKOS analysis proved definite error(s) in the'
            ' previous stub body: %s. Do not read/write past the'
            ' caller-provided object; use the real object size instead of'
            ' guessed constants; never return input pointers, stack'
            ' addresses, or scalar arguments as pointer results.'
            % findings)


def map_findings(audits):
    """Stub-side errors of every bitcode, aggregated by stub function.

    The function name is the stub file stem (``generate --only`` accepts
    it); the bitcode that FIRST reported the function (input order)
    carries the repair, since stubs are shared across bitcodes."""
    findings = {}
    for audit in audits:
        for item in audit['report']['stub_errors']:
            # the error path goes through the audit's 'stubs' symlink;
            # realpath maps it back to the actual stub file for repair
            item['file'] = os.path.realpath(item['file'])
            name = os.path.splitext(os.path.basename(item['file']))[0]
            finding = findings.get(name)
            if finding is None:
                ir_name, source_name = header_names(item['file'])
                finding = {'function': name, 'file': item['file'],
                           'ir_name': ir_name, 'source_name': source_name,
                           'bc': audit['bitcode'], 'errors': []}
                findings[name] = finding
            finding['errors'].append(item)
    for finding in findings.values():
        finding['hint'] = build_hint(finding['errors'])
    return findings


#############################
# Audit (reuses auditgate.py) #
#############################

def read_stats(db_path, stub_real):
    """auditgate.read_stats with the stub-side test extended to --stub-dir.

    Same grouping/dedup convention; only the stub-side classifier differs:
    a check is stub-side when its source path carries a stubs/ segment
    (STUB_PATH_RE) OR lives under the real stub directory -- ikos-stub
    resolves the directory to its real path before compiling, so a stub
    directory named without a stubs prefix (r18-smoke-stubs, mystubs)
    would otherwise misfile its errors as project-side.  Used for BOTH
    databases of a comparison so the project-side yardsticks stay
    consistent (the baseline simply never matches)."""
    side_cache = {}

    def stub_side(path):
        if not path:
            return False
        if path not in side_cache:
            if auditgate.STUB_PATH_RE.search(path):
                side_cache[path] = True
            else:
                try:
                    side_cache[path] = os.path.commonpath(
                        (os.path.realpath(path), stub_real)) == stub_real
                except ValueError:
                    side_cache[path] = False
        return side_cache[path]

    conn = None
    try:
        conn = auditgate._connect_ro(db_path)
        rows = conn.execute(
            'SELECT c.statement_id, c.call_context_id, c.kind, c.status,'
            ' c.operands, c.info, f.path, s.line'
            ' FROM checks c'
            ' JOIN statements s ON c.statement_id = s.id'
            ' LEFT JOIN files f ON s.file_id = f.id').fetchall()
    except sqlite3.Error as error:
        raise auditgate.ToolError('cannot read checks from %s: %s'
                                  % (db_path, error))
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
        is_stub = stub_side(path)
        if statuses == {auditgate.STATUS_UNREACHABLE}:
            if not is_stub:
                project['unreachable'] += 1
                loc = (path or '?', line or 0)
                unreach_by_loc[loc] = unreach_by_loc.get(loc, 0) + 1
            continue
        seen = set()
        for kind, status, operands, info, _, _ in checks:
            if status not in (auditgate.STATUS_SAFE,
                              auditgate.STATUS_WARNING,
                              auditgate.STATUS_ERROR):
                continue
            dedup = (kind, operands, info, status)
            if dedup in seen:
                continue
            seen.add(dedup)
            if is_stub:
                if status == auditgate.STATUS_ERROR:
                    item_key = (path, line, kind, operands, info)
                    stub_errors.setdefault(
                        item_key,
                        {'file': path, 'line': line, 'kind': kind,
                         'kind_name': auditgate.kind_name(kind),
                         'operands': operands, 'info': info})
            else:
                project[auditgate.STATUS_NAMES[status]] += 1

    errors = sorted(stub_errors.values(),
                    key=lambda item: (item['file'], item['line'],
                                      item['kind']))
    return {'project': project,
            'unreach_by_loc': unreach_by_loc,
            'stub_errors': errors}


def audit_one(bitcode, index, state, args, ikos_bin, ikos_stub_bin,
              round_no):
    """One bitcode through the auditgate pipeline; audit record dict.

    state carries the baseline database path across rounds: the program
    never changes, so the baseline is computed at most once."""
    stem = os.path.splitext(os.path.basename(bitcode))[0]
    work = os.path.join(args.tmp_dir, 'round%d' % round_no,
                        '%d-%s' % (index, stem))
    os.makedirs(work, exist_ok=True)
    timings = {}

    baseline_db = state.get('baseline_db')
    baseline_provided = bool(baseline_db)
    if not baseline_db:
        sibling = os.path.splitext(bitcode)[0] + args.baseline_suffix
        if os.path.isfile(sibling):
            baseline_db = sibling
            baseline_provided = True
        else:
            baseline_db = os.path.join(work, 'base.db')
            timings['baseline_ikos'] = auditgate.run_ikos(
                ikos_bin, bitcode, baseline_db, args.timeout,
                os.path.join(work, 'base.ikos.log'))
        state['baseline_db'] = baseline_db

    stubbed_bc = os.path.join(work, stem + '.stubbed.bc')
    stub_db = os.path.join(work, 'stub.db')
    timings['ikos_stub_link'] = auditgate.run_command(
        [ikos_stub_bin, 'link', '--stub-dir', args.stub_dir,
         '-o', stubbed_bc, bitcode],
        args.timeout, 'ikos-stub link',
        os.path.join(work, 'link.log'))
    if not os.path.isfile(stubbed_bc):
        raise auditgate.ToolError('ikos-stub link produced no %s'
                                  % stubbed_bc)
    timings['stub_ikos'] = auditgate.run_ikos(
        ikos_bin, stubbed_bc, stub_db, args.timeout,
        os.path.join(work, 'stub.ikos.log'))

    base_stats = read_stats(baseline_db, args.stub_real)
    stub_stats = read_stats(stub_db, args.stub_real)
    report = auditgate.build_report(bitcode, baseline_db,
                                    baseline_provided, stubbed_bc, stub_db,
                                    stub_stats, base_stats,
                                    0, float('inf'), timings)
    return {'bitcode': bitcode,
            'baseline_db': baseline_db,
            'stub_db': stub_db,
            'stub_error_count': len(report['stub_errors']),
            'unreach_delta_rate': report['gates']['unreach_delta']['rate'],
            'project': report['project'],
            'timings': timings,
            'report': report}


#############################
# Repair #
#############################

def _diff_excerpt(old, new, path):
    """Unified diff of the two stub bodies, capped for the report."""
    diff = list(difflib.unified_diff(old.splitlines(), new.splitlines(),
                                     fromfile=path + ' (previous)',
                                     tofile=path + ' (regenerated)',
                                     lineterm=''))
    if len(diff) > DIFF_EXCERPT_LINES:
        diff = diff[:DIFF_EXCERPT_LINES] + [
            '... (%d more diff lines)' % (len(diff) - DIFF_EXCERPT_LINES)]
    return '\n'.join(diff)


def evict_db_entries(finding, db_dir):
    """Drop the stub-database slots vouching for a convicted body.

    generate consults the database before the response cache and the LLM
    and never considers --repair-hint there; the battery that admitted an
    entry is weaker than this audit, so a convicted function's slots (its
    source-name shard, entries whose ir_name matches) are removed first.
    Returns the evicted slot keys."""
    source_name = finding.get('source_name')
    ir_name = finding.get('ir_name')
    if not db_dir or not source_name or not ir_name:
        return []
    shard = os.path.join(os.path.expanduser(db_dir), stubdb.ENTRIES_DIR,
                         stubdb.shard_name(source_name))
    try:
        with open(shard) as stream:
            table = json.load(stream)
    except (OSError, ValueError):
        return []
    if not isinstance(table, dict):
        return []
    doomed = [key for key, entry in table.items()
              if isinstance(entry, dict)
              and entry.get('ir_name') == ir_name]
    if not doomed:
        return []
    for key in doomed:
        del table[key]
    try:
        tmp = shard + '.tmp'
        with open(tmp, 'w') as stream:
            json.dump(table, stream, indent=1, sort_keys=True)
        os.replace(tmp, shard)
    except OSError:
        return []
    return sorted(doomed)


def repair_one(finding, args):
    """Regenerate one offending stub with the findings as repair hint.

    ikos-api-spec refuses --only for an already-stubbed function ("delete
    its stub file to regenerate"), so the old file is removed first and
    restored when the regeneration itself fails: a failed repair must not
    silently leave the function unmodeled."""
    name = finding['function']
    stub_file = finding['file']
    record = {'function': name, 'file': stub_file,
              'ir_name': finding['ir_name'], 'bc': finding['bc'],
              'errors': finding['errors'], 'hint': finding['hint']}
    try:
        with open(stub_file) as stream:
            old_body = stream.read()
    except OSError as error:
        record.update({'action': 'repair-failed',
                       'detail': 'cannot read stub file: %s' % error})
        return record
    record['db_evicted'] = evict_db_entries(finding, args.db_dir)

    cmd = [sys.executable, API_SPEC_PY, 'generate',
           '-o', args.stub_dir, '--only', name,
           '--repair-hint', finding['hint']]
    if args.db_dir:
        cmd += ['--db-dir', args.db_dir]
    if args.provider:
        cmd += ['--provider', args.provider]
    if args.model:
        cmd += ['--model', args.model]
    for path in args.real_libs or ():
        cmd += ['--real-libs', path]
    cmd += list(args.generate_opt or ())
    cmd.append(finding['bc'])

    os.remove(stub_file)
    started = time.time()
    try:
        proc = subprocess.run(cmd,
                              stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT,
                              universal_newlines=True, errors='replace')
    except OSError as error:
        record.update({'action': 'repair-failed',
                       'detail': 'ikos-api-spec.py could not run: %s'
                                 % error})
        _restore(stub_file, old_body)
        return record
    record['wall'] = round(time.time() - started, 3)
    record['returncode'] = proc.returncode
    record['output_tail'] = (proc.stdout or '').strip()[-600:]

    new_body = None
    try:
        with open(stub_file) as stream:
            new_body = stream.read()
    except OSError:
        pass
    if proc.returncode == 0 and new_body is not None:
        record['action'] = 'regenerated'
        record['changed'] = new_body != old_body
        record['diff'] = _diff_excerpt(old_body, new_body, stub_file)
    else:
        record['action'] = 'repair-failed'
        record['detail'] = ('ikos-api-spec.py exit %d without installing'
                            ' a new stub; previous body restored'
                            % proc.returncode)
        _restore(stub_file, old_body)
    return record


def _restore(stub_file, body):
    try:
        with open(stub_file, 'w') as stream:
            stream.write(body)
    except OSError:
        pass


def prune_one(finding, args):
    """Remove a still-violating stub: unmodeled beats proven-wrong."""
    record = {'function': finding['function'], 'file': finding['file'],
              'errors': finding['errors'], 'action': 'pruned'}
    try:
        os.remove(finding['file'])
    except OSError as error:
        record['action'] = 'prune-failed'
        record['detail'] = str(error)
    return record


#############################
# Loop #
#############################

def run_audits(bitcodes, states, args, ikos_bin, ikos_stub_bin, round_no):
    """All bitcodes audited in a thread pool, findings in input order."""
    audits = [None] * len(bitcodes)
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(audit_one, bc, index, states[index], args,
                               ikos_bin, ikos_stub_bin, round_no): index
                   for index, bc in enumerate(bitcodes)}
        for future, index in futures.items():
            audits[index] = future.result()
    return audits


def run_repairs(findings, args):
    """One repair task per violating function, parallel over --jobs."""
    names = sorted(findings)
    records = {}
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(repair_one, findings[name], args): name
                   for name in names}
        for future, name in futures.items():
            records[name] = future.result()
    return [records[name] for name in names]


def print_round(round_no, audits, findings):
    print('round %d: audited %d bitcode(s)'
          % (round_no, len(audits)))
    for audit in audits:
        print('  %s: %d stub-side error(s); unreachable delta rate %.4f'
              % (audit['bitcode'], audit['stub_error_count'],
                 audit['unreach_delta_rate']))
    if findings:
        print('  violating function(s): %s'
              % ', '.join('%s (%d error(s), %s)'
                          % (name, len(item['errors']), item['file'])
                          for name, item in sorted(findings.items())))


def print_repairs(repairs):
    for record in repairs:
        line = '  repair %s: %s' % (record['function'], record['action'])
        if record['action'] == 'regenerated':
            line += record['changed'] and ' (body changed)' or \
                ' (body identical)'
        elif record.get('detail'):
            line += ' -- %s' % record['detail']
        if record.get('db_evicted'):
            line += ' [db: evicted %d slot(s)]' % len(record['db_evicted'])
        print(line)


#############################
# Command line #
#############################

def parse_args(argv):
    parser = argparse.ArgumentParser(
        prog='auditloop.py',
        description='Outer repair loop: audit stub-linked bitcode with'
                    ' auditgate, regenerate offending stubs with the'
                    ' findings as --repair-hint, repeat until clean or'
                    ' out of rounds.')
    parser.add_argument('bitcodes',
                        nargs='+',
                        metavar='X.bc',
                        help='Whole-program LLVM bitcode files to audit'
                             ' and repair stubs for')
    parser.add_argument('--stub-dir',
                        dest='stub_dir',
                        metavar='DIR',
                        default='stubs',
                        help='Stub library directory (default: %(default)s)')
    parser.add_argument('--db-dir',
                        dest='db_dir',
                        metavar='DIR',
                        help='Stub database directory passed to generate'
                             ' (default: generate\'s own default)')
    parser.add_argument('--provider',
                        dest='provider',
                        metavar='P',
                        help='LLM provider passed to generate')
    parser.add_argument('--model',
                        dest='model',
                        metavar='M',
                        help='LLM model passed to generate')
    parser.add_argument('--real-libs',
                        dest='real_libs',
                        metavar='PATH',
                        action='append',
                        default=None,
                        help='Real-library map TSV passed to generate'
                             ' (repeatable)')
    parser.add_argument('--max-rounds',
                        dest='max_rounds',
                        metavar='N',
                        type=int,
                        default=DEFAULT_MAX_ROUNDS,
                        help='Maximum repair rounds after the first audit'
                             ' (default: %(default)s)')
    parser.add_argument('--jobs',
                        dest='jobs',
                        metavar='N',
                        type=int,
                        default=DEFAULT_JOBS,
                        help='Parallel audits / repairs (default:'
                             ' %(default)s)')
    parser.add_argument('--prune-failed',
                        dest='prune_failed',
                        action='store_true',
                        help='Delete the stub files of functions still'
                             ' violating after the last round')
    parser.add_argument('--keep-temps',
                        dest='keep_temps',
                        action='store_true',
                        help='Keep the temporary directory (linked bitcode,'
                             ' per-round databases, ikos logs)')
    parser.add_argument('--json',
                        dest='json_path',
                        metavar='PATH',
                        help='Write the full loop report as JSON to PATH')
    parser.add_argument('--generate-opt',
                        dest='generate_opt',
                        metavar='OPT',
                        action='append',
                        default=None,
                        help='Extra argument appended verbatim to every'
                             ' ikos-api-spec.py generate command line'
                             ' (repeatable; split value-taking options'
                             ' into two --generate-opt)')
    parser.add_argument('--baseline-suffix',
                        dest='baseline_suffix',
                        metavar='SUF',
                        default='.db',
                        help='Suffix of the baseline database beside each'
                             ' X.bc (default: %(default)s; missing'
                             ' baselines are computed with ikos)')
    parser.add_argument('--timeout',
                        dest='timeout',
                        metavar='SEC',
                        type=int,
                        default=auditgate.DEFAULT_TIMEOUT,
                        help=argparse.SUPPRESS)  # auditgate plumbing
    args = parser.parse_args(argv)
    if args.max_rounds < 1:
        parser.error('--max-rounds must be >= 1')
    if args.jobs < 1:
        parser.error('--jobs must be >= 1')
    return args


def main(argv=None):
    args = parse_args(argv)

    for bitcode in args.bitcodes:
        if not os.path.isfile(bitcode):
            sys.stderr.write('error: no such bitcode file: %s\n' % bitcode)
            return EXIT_TOOL
    if not os.path.isdir(args.stub_dir):
        sys.stderr.write('error: no such stub directory: %s\n'
                         % args.stub_dir)
        return EXIT_TOOL
    if not os.path.isfile(API_SPEC_PY):
        sys.stderr.write('error: %s not found beside %s\n'
                         % (API_SPEC_PY, os.path.abspath(__file__)))
        return EXIT_TOOL

    # ikos-api-spec locates the ikos python package via ikos-stub on
    # PATH; prepend the well-known install bin dir when it exists
    if os.path.isdir(INSTALL_BIN_DIR):
        path = os.environ.get('PATH', '')
        if INSTALL_BIN_DIR not in path.split(os.pathsep):
            os.environ['PATH'] = INSTALL_BIN_DIR + os.pathsep + path
    try:
        ikos_bin = auditgate.resolve_tool('ikos', None)
        ikos_stub_bin = auditgate.resolve_tool('ikos-stub', None)
    except auditgate.ToolError as error:
        sys.stderr.write('error: %s\n' % error)
        return EXIT_TOOL

    args.tmp_dir = tempfile.mkdtemp(prefix='auditloop-')
    args.stub_real = os.path.realpath(args.stub_dir)
    states = [{} for _ in args.bitcodes]
    loop = {'stub_dir': os.path.abspath(args.stub_dir),
            'max_rounds': args.max_rounds,
            'prune_failed': args.prune_failed,
            'unreach_note': UNREACH_NOTE,
            'rounds': [],
            'pruned': [],
            'temps_kept': None}
    try:
        findings = None
        for round_no in range(1, args.max_rounds + 2):
            audits = run_audits(args.bitcodes, states, args,
                                ikos_bin, ikos_stub_bin, round_no)
            findings = map_findings(audits)
            print_round(round_no, audits, findings)
            round_record = {'round': round_no,
                            'audits': [{key: audit[key]
                                        for key in ('bitcode',
                                                    'baseline_db',
                                                    'stub_db',
                                                    'stub_error_count',
                                                    'unreach_delta_rate',
                                                    'project', 'timings')}
                                       for audit in audits],
                            'findings': {name: {'file': item['file'],
                                                'ir_name': item['ir_name'],
                                                'source_name':
                                                    item['source_name'],
                                                'bc': item['bc'],
                                                'errors': item['errors'],
                                                'hint': item['hint']}
                                         for name, item
                                         in sorted(findings.items())},
                            'repairs': []}
            loop['rounds'].append(round_record)
            if not findings:
                loop['final'] = 'pass'
                break
            if round_no > args.max_rounds:
                loop['final'] = 'violations'
                break
            repairs = run_repairs(findings, args)
            round_record['repairs'] = repairs
            print_repairs(repairs)

        if loop['final'] == 'violations' and args.prune_failed:
            pruned = [prune_one(findings[name], args)
                      for name in sorted(findings)]
            loop['pruned'] = pruned
            for record in pruned:
                print('  prune %s: %s' % (record['function'],
                                          record['action']))
            loop['final'] = 'pruned'

        if loop['final'] == 'pass':
            print('auditloop: PASS (stub side clean, round %d)'
                  % loop['rounds'][-1]['round'])
            exit_code = EXIT_PASS
        else:
            remaining = {name: len(item['errors'])
                         for name, item in (findings or {}).items()}
            loop['remaining_violations'] = remaining
            print('auditloop: FAIL (%s: %s)'
                  % (loop['final'],
                     ', '.join('%s=%d error(s)' % item
                               for item in sorted(remaining.items()))))
            exit_code = EXIT_GATE
    except auditgate.ToolError as error:
        sys.stderr.write('error: %s\n' % error)
        if not args.keep_temps:
            shutil.rmtree(args.tmp_dir, ignore_errors=True)
        return EXIT_TOOL

    if args.keep_temps:
        loop['temps_kept'] = args.tmp_dir
        print('temporary files kept in %s' % args.tmp_dir)
    else:
        shutil.rmtree(args.tmp_dir, ignore_errors=True)
    if args.json_path:
        try:
            with open(args.json_path, 'w') as stream:
                json.dump(loop, stream, indent=1, sort_keys=True)
                stream.write('\n')
        except OSError as error:
            sys.stderr.write('warning: cannot write JSON report %s: %s\n'
                             % (args.json_path, error))
    return exit_code


if __name__ == '__main__':
    sys.exit(main())
