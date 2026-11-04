"""The ikos battery: judge a stub with the real analyzer, not a simulation.

The differential gate historically ran the stub natively with a recorder
intercepting __ikos_* intrinsics (concrete, schedule-driven, <=48
schedules) while a Python judge simulated ikos's domain semantics.  This
module replaces the simulation with the analyzer itself: for every input
it generates a tiny deterministic harness that pins the input as
compile-time literals, calls the stub once, then

  * prints the abstract state at the observation point
    (__ikos_print_values under -a dbg: the return's interval or
    points-to/offset/nullity, and every probed pointee cell loaded into
    a NAMED local -- named because the debugger prints source variable
    names), and
  * poses exclusion assertions, one per source line (__ikos_assert under
    -a prover): ikos PROVING `r != y` means the stub excludes the real
    observation; verdicts are read from the sqlite checks table.

Pinning faithfulness is underdetermined (the DT1 contract never recorded
scalar argument values nor the param<->buffer mapping), so the SAME
harness source is built a second time as a WITNESS: without -D__IKOS__
every intrinsic expands to a no-op macro, the call target switches to
the REAL symbol, and an #ifdef'd printf block reports the concrete
ret/cells/bases.  A proved exclusion is only trusted when the witness
reproduces the real observation; a bad pin degrades to dropped evidence
(per-input fallback), never to a false unsound verdict.

The module is mechanism only -- generation, building, running, parsing,
caching.  Every verdict lives in difftest._ikos_phase.
"""

import hashlib
import os
import re
import sqlite3
import subprocess
import sys
import time
from contextlib import closing


def _memory_limit_flags():
    # Darwin rejects RLIMIT_AS in the Python analyzer driver's preexec hook.
    # Keep the analyzer timer and subprocess wall timeout on every platform.
    return [] if sys.platform == 'darwin' else ['--mem', '512']

try:
    import cxprobe
except ImportError:  # pragma: no cover -- shipped next to this module
    cxprobe = None

# bump to invalidate the harness cache when the template changes
IKOSJUDGE_TEMPLATE_VERSION = '1'

# Generated stubs and harnesses must see the checkout's intrinsics before
# any older headers in the installed analyzer prefix.
REPO_INCLUDE_DIR = os.path.abspath(
    os.path.join(os.path.dirname(__file__), '..', '..', 'analyzer',
                 'include'))


def _ikos_include_flags(settings):
    """Include checkout intrinsics before the installed analyzer headers."""
    return ['-isystem', REPO_INCLUDE_DIR, '-isystem', settings.INCLUDE_DIR]

# per-input probe budget (exclusion assertions in one harness)
MAX_PROBES = 64

# buffer pinning bound (bytes per registered buffer)
MAX_BUFFER = 4096

# how many prefix 8-byte windows of every buffer are probed even when
# unwritten (catches over-eager claims on read-only regions)
PREFIX_WINDOWS = 4

# the assume_mem_value/rel primitives refine 8-byte cells (lo/hi carry
# the width); byte loads cannot project the refinement out, so probes
# read aligned 8-byte windows -- the granularity the analyzer can prove
WINDOW = 8

_INT_LINE_RE = re.compile(
    r'^\t(\S+) is in \[(-?[0-9]+|-inf|\+?inf), ?(-?[0-9]+|-inf|\+?inf)\]$')
_POINT_LINE_RE = re.compile(r'^\t(\S+) is (-?[0-9]+|bottom)$')
# single-element points-to sets print WITHOUT braces: "r points to b_s"
_POINTS_TO_RE = re.compile(r'^\t(\S+) points to \{?(.*?)\}?$')
_OFFSET_RE = re.compile(r'^\toffset of (\S+) is in \[([^\]]+)\]$')
# singleton offsets print without the interval: "offset of r is 1"
_OFFSET_POINT_RE = re.compile(r'^\toffset of (\S+) is (-?[0-9]+)$')
_NULLITY_RE = re.compile(r'^\t(\S+) is (non-null|may be null)$')

_WITNESS_PREFIX = 'IJW '

# process-local: (stub text + harness text + domain + template) -> result
_CACHE = {}


def _sha(text):
    return hashlib.sha256(text.encode('utf-8', 'replace')).hexdigest()


def _tag_stem(tag):
    return re.sub(r'\W', '_', tag).strip('_') or 'buf'


def buffer_var(tag):
    """Harness C variable name for a registered buffer tag."""
    return 'b_' + _tag_stem(tag)


#############################
# Harness source generation #
#############################

def _probe_offsets(real_obs):
    """{tag: [window offsets]}: real-written cells plus a fixed prefix,
    as aligned 8-byte windows (the primitive's refinement granularity)."""
    table = {}
    for tag, _base, _length in real_obs.get('bufregs') or ():
        wrote = real_obs.get('wrote', {}).get(tag, ())
        data = real_obs.get('data', {}).get(tag, b'')
        windows = set()
        for off in wrote:
            if off < len(data) and off < MAX_BUFFER:
                windows.add(off - off % WINDOW)
        windows.update(off * WINDOW
                       for off in range(min(PREFIX_WINDOWS,
                                            (min(len(data), MAX_BUFFER)
                                             + WINDOW - 1) // WINDOW)))
        table[tag] = sorted(windows)[:MAX_PROBES]
    return table


def harness_source(parts, real_symbol, real_label, stub_ident, stub_label,
                   argv, real_obs, ret_probe_value, pointer_cell_offsets):
    """(source, probes, buffers) for one input; None = out of scope.

    `ret_probe_value`: the excluded return value for the ret assertion
    (None = no ret probe: void/pointer/float returns).  `pointer_cell_
    offsets`: {(tag, off)} cells the real run wrote pointers into.
    """
    open_paren = parts.signature_line.find('(')
    params_text = parts.signature_line[open_paren + 1:
                                        parts.signature_line.rfind(')')]
    if params_text.strip() in ('', 'void'):
        params = []
    else:
        params = [p.strip() for p in params_text.split(',') if p.strip()]
    for param in params:
        # a by-value aggregate cannot be pinned faithfully; skip honestly
        if ('struct' in param or 'union' in param) and '*' not in param:
            return None
    # the signature line is `<ret> <name>(<params>)`: the last token of
    # the head is the function name, everything before it the return type
    head_tokens = parts.signature_line[:open_paren].split()
    ret_text = ' '.join(head_tokens[:-1]) if len(head_tokens) > 1 \
        else head_tokens[0]
    is_ptr_ret = '*' in ret_text

    # one buffer per registered region, content pinned from the observed
    # post-call data (the best available reconstruction; the witness
    # arbitrates whether the pin was behaviorally faithful)
    buffers = []  # (tag, c_var, length)
    for tag, _base, length in real_obs.get('bufregs') or ():
        length = max(64, min(int(length), MAX_BUFFER))
        buffers.append((tag, buffer_var(tag), length))
    lines = []
    lines.append('/* generated by ikosjudge %s -- do not edit */'
                 % IKOSJUDGE_TEMPLATE_VERSION)
    lines.append('#include <stddef.h>')
    lines.append('#include <stdint.h>')
    lines.append('#ifdef IKOSJUDGE_WITNESS')
    lines.append('#include <stdio.h>')
    lines.append('#endif')
    lines.append('#include <ikos/analyzer/intrinsic.h>')
    lines.extend(parts.typedef_lines)
    stub_suffix = ' __asm__("%s")' % stub_label if stub_label else ''
    real_suffix = ' __asm__("%s")' % real_label if real_label else ''
    lines.append('#ifdef IKOSJUDGE_WITNESS')
    lines.append('extern %s %s(%s)%s;'
                 % (ret_text, real_symbol, params_text, real_suffix))
    lines.append('#define IJ_TARGET %s' % real_symbol)
    lines.append('#else')
    lines.append('extern %s %s(%s)%s;'
                 % (ret_text, stub_ident, params_text, stub_suffix))
    lines.append('#define IJ_TARGET %s' % stub_ident)
    lines.append('#endif')
    lines.append('')
    lines.append('int main(void) {')

    data_by_tag = real_obs.get('data', {})
    for tag, c_var, length in buffers:
        payload = ', '.join(
            str(data_by_tag.get(tag, b'')[o] & 0xFF)
            if o < len(data_by_tag.get(tag, b'')) else '0'
            for o in range(length))
        lines.append('    unsigned char %s[%d] = {%s};' % (c_var, length,
                                                           payload))
        lines.append('    (void) %s;' % c_var)

    # argument pinning: pointer params take buffers in order (FILE*/opaque
    # and leftovers get NULL), scalar params take argv tokens as
    # literals -- the mapping is a heuristic the witness arbitrates
    tokens = list(argv[1:]) if argv else []

    def scalar(pos):
        try:
            return int(str(tokens[pos]), 10)
        except (ValueError, IndexError):
            return 0

    args = []
    buf_pos = 0
    for pos, param in enumerate(params):
        if '*' in param:
            if 'FILE' in param or buf_pos >= len(buffers):
                args.append('NULL')
            else:
                args.append(buffers[buf_pos][1])
                buf_pos += 1
        else:
            args.append(str(scalar(pos)))
    if ret_text == 'void':
        lines.append('    IJ_TARGET(%s);' % ', '.join(args))
        ret_name = None
    else:
        lines.append('    %s r = IJ_TARGET(%s);'
                     % (ret_text, ', '.join(args)))
        ret_name = 'r'

    # observation: every probed window read into a NAMED local at the
    # primitive's own granularity (fixed-size memcpy is exact in ikos);
    # pointer cells get pointer reads
    cells = []  # (name, tag, window offset, observed int64)
    ptr_vars = []
    for tag, c_var, _length in buffers:
        for off in _probe_offsets(real_obs).get(tag, ()):
            name = 'cell_%s_%d' % (_tag_stem(tag), off)
            data = data_by_tag.get(tag, b'')
            chunk = bytes(data[off:off + WINDOW]).ljust(WINDOW, b'\0')
            value = int.from_bytes(chunk, 'little', signed=True)
            lines.append('    long long %s;' % name)
            lines.append('    __builtin_memcpy(&%s, %s + %d, %d);'
                         % (name, c_var, off, WINDOW))
            cells.append((name, tag, off, value))
        for off in sorted(o for (t, o) in pointer_cell_offsets
                          if t == tag):
            if off + 8 > MAX_BUFFER:
                continue
            name = 'cellptr_%s_%d' % (_tag_stem(tag), off)
            lines.append('    void* %s = *(void**)&%s[%d];'
                         % (name, c_var, off))
            ptr_vars.append(name)

    printed = ([ret_name] if ret_name else []) \
        + [name for name, _t, _o, _v in cells] + list(ptr_vars)
    lines.append('    __ikos_print_values("post"%s);'
                 % ((', ' + ', '.join(printed)) if printed else ''))

    # exclusion assertions, ONE PER LINE: the source line number is the
    # probe identity in the checks table
    probes = []  # {'line', 'kind', 'tag', 'off', 'value'}
    if ret_name and ret_probe_value is not None:
        lines.append('    __ikos_assert(r != %d);' % ret_probe_value)
        probes.append({'kind': 'ret', 'tag': '-', 'off': -1,
                       'value': ret_probe_value})
    for name, tag, off, value in cells:
        lines.append('    __ikos_assert(%s != %d);' % (name, value))
        probes.append({'kind': 'cell', 'tag': tag, 'off': off,
                       'value': value})

    # witness reporting: the concrete observation on the pinned input
    lines.append('#ifdef IKOSJUDGE_WITNESS')
    if ret_name is not None:
        lines.append('    printf("%sRET %%lld\\n", (long long) r);'
                     % _WITNESS_PREFIX)
        if is_ptr_ret:
            lines.append('    printf("%sRETP %%p\\n", (void*) r);'
                         % _WITNESS_PREFIX)
    for name, tag, off, _value in cells:
        lines.append('    printf("%sCELL %s %d %%lld\\n", %s);'
                     % (_WITNESS_PREFIX, tag, off, name))
    for name in ptr_vars:
        lines.append('    printf("%sPTR %s %%p\\n", (void*) %s);'
                     % (_WITNESS_PREFIX, name, name))
    for tag, c_var, _length in buffers:
        lines.append('    printf("%sBASE %s %%p\\n", (void*) %s);'
                     % (_WITNESS_PREFIX, tag, c_var))
    lines.append('#endif')
    lines.append('    return 0;')
    lines.append('}')
    source = '\n'.join(lines) + '\n'

    # annotate probes with their final source lines
    final = source.split('\n')
    for probe in probes:
        marker = '__ikos_assert(r !=' if probe['kind'] == 'ret' \
            else '__ikos_assert(cell_%s_%d !=' % (probe['tag'],
                                                  probe['off'])
        for lineno, text in enumerate(final, start=1):
            if marker in text:
                probe['line'] = lineno
                break
        else:  # pragma: no cover -- generation bug guard
            probe['line'] = -1
    return source, probes, buffers


#############################
# Output parsing #
#############################

def _parse_bound(token):
    token = token.strip()
    if token in ('+inf', 'inf'):
        return None
    if token == '-inf':
        return '-inf'
    return int(token)


def parse_print_values(stdout):
    """{name: value} out of the debugger output.

    Integer locals map to [lo, hi] (None = +inf), a singleton int, or
    'bottom'; pointer locals map to a dict with points_to/offset/nullity.
    """
    values = {}

    def slot(name):
        if name not in values or not isinstance(values[name], dict):
            if isinstance(values.get(name), dict):
                return values[name]
            values[name] = {}
        return values[name]

    for line in stdout.splitlines():
        match = _POINT_LINE_RE.match(line)
        if match:
            values[match.group(1)] = int(match.group(2)) \
                if match.group(2) != 'bottom' else 'bottom'
            continue
        match = _INT_LINE_RE.match(line)
        if match:
            values[match.group(1)] = [_parse_bound(match.group(2)),
                                      _parse_bound(match.group(3))]
            continue
        match = _POINTS_TO_RE.match(line)
        if match:
            slot(match.group(1))['points_to'] = [
                entry.strip() for entry in match.group(2).split(',')
                if entry.strip()]
            continue
        match = _OFFSET_RE.match(line)
        if match:
            lo, hi = match.group(2).split(',')
            slot(match.group(1))['offset'] = [_parse_bound(lo),
                                              _parse_bound(hi)]
            continue
        match = _OFFSET_POINT_RE.match(line)
        if match:
            value = int(match.group(2))
            slot(match.group(1))['offset'] = [value, value]
            continue
        match = _NULLITY_RE.match(line)
        if match:
            slot(match.group(1))['nullity'] = match.group(2)
            continue
    return values


def parse_witness(stdout):
    """Witness report: concrete ret/cells/pointer-cells/bases."""
    report = {'ret': None, 'ret_ptr': None, 'cells': {}, 'ptrs': {},
              'bases': {}}
    for line in stdout.splitlines():
        if not line.startswith(_WITNESS_PREFIX):
            continue
        fields = line[len(_WITNESS_PREFIX):].split()
        if not fields:
            continue
        kind = fields[0]
        try:
            if kind == 'RET' and len(fields) >= 2:
                report['ret'] = int(fields[1])
            elif kind == 'RETP' and len(fields) >= 2:
                report['ret_ptr'] = _hex(fields[1])
            elif kind == 'CELL' and len(fields) >= 4:
                report['cells'][(fields[1], int(fields[2]))] = \
                    int(fields[3])
            elif kind == 'PTR' and len(fields) >= 3:
                report['ptrs'][fields[1]] = _hex(fields[2])
            elif kind == 'BASE' and len(fields) >= 3:
                report['bases'][fields[1]] = _hex(fields[2])
        except ValueError:
            continue
    return report


def _hex(token):
    return 0 if token in ('(nil)', '0x0') else int(token, 16)


def classify_blocks(witness, lengths):
    """Concrete pointer values -> ('NULL',)|('BUF', tag, off)|('OTHER', v)."""
    def classify(value):
        if not value:
            return ('NULL', )
        for tag, base in witness.get('bases', {}).items():
            if base and base <= value < base + max(lengths.get(tag, 1),
                                                   1):
                return ('BUF', tag, value - base)
        return ('OTHER', value)

    blocks = {}
    if witness.get('ret_ptr') is not None:
        blocks['r'] = classify(witness['ret_ptr'])
    for name, value in witness.get('ptrs', {}).items():
        blocks[name] = classify(value)
    return blocks


def read_checks(db_path, harness_path, stub_path):
    """({harness assert line: status}, [stub rows], all_unreachable).

    status: 0 = PROVED, 1 = unknown, 2 = refuted.  kind 0 rows are
    Unreachable statements: a harness whose every assertion is
    unreachable means the analyzer pruned all paths after the call.
    """
    if not os.path.isfile(db_path):
        return {}, [], False
    uri = 'file:%s?mode=ro' % db_path.replace('?', '%3f').replace('#',
                                                                   '%23')
    try:
        conn = sqlite3.connect(uri, uri=True, timeout=10)
        rows = conn.execute(
            'SELECT f.path, s.line, c.kind, c.status FROM checks c'
            ' JOIN statements s ON c.statement_id = s.id'
            ' JOIN files f ON s.file_id = f.id'
            ' WHERE c.checker = 3 AND c.kind IN (0, 3)').fetchall()
        conn.close()
    except sqlite3.Error:
        return {}, [], False
    harness_real = os.path.realpath(harness_path)
    stub_real = os.path.realpath(stub_path)
    probes = {}
    stub_rows = []
    unreachable = 0
    for path, line, kind, status in rows:
        real = os.path.realpath(path)
        if real == harness_real and kind == 3:
            probes[line] = status
        elif real == harness_real and kind == 0:
            unreachable += 1
        elif real == stub_real:
            stub_rows.append({'line': line, 'kind': kind,
                              'status': status})
    all_unreachable = unreachable > 0 and not probes
    return probes, stub_rows, all_unreachable


#############################
# Build / run / cache #
#############################

def _mem2reg(opt_path, bc_path, warn):
    """opt -mem2reg in place; mirrors ikos-stub's 2026-09-29 fix."""
    if not opt_path:
        warn('ikosjudge: no opt binary; keeping -O0 form (ikos-pp basic'
             ' still promotes memory to registers)')
        return
    tmp = bc_path + '.m2r.bc'
    proc = subprocess.run([opt_path, '-mem2reg', bc_path, '-o', tmp],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, errors='replace')
    if proc.returncode == 0 and os.path.isfile(tmp):
        os.replace(tmp, bc_path)
    else:
        warn('ikosjudge: mem2reg failed; keeping -O0 form')


def run(kit, opt, stub_src, parts, real_symbol, real_label, real_libs,
        stub_ident, stub_label, argv, real_obs, ret_probe_value,
        pointer_cell_offsets, batdir, index, domain, cpu, wall,
        timeout_run):
    """One input's ikos + witness evaluation.  -> dict | None.

    None = no usable evidence (build failure, analyzer crash, unparsable
    output, out-of-scope signature); the caller falls back to the
    simulation path for this input.
    """
    warn = kit.log.warning
    if getattr(opt, 'dt_no_ikos', False):
        return None
    built = harness_source(parts, real_symbol, real_label, stub_ident,
                           stub_label, argv, real_obs, ret_probe_value,
                           pointer_cell_offsets)
    if built is None:
        return None
    source, probes, buffers = built
    with open(stub_src) as stream:
        stub_text = stream.read()
    key = _sha(stub_text + source + domain + IKOSJUDGE_TEMPLATE_VERSION)
    if key in _CACHE:
        return _CACHE[key]

    rundir = os.path.join(batdir, 'ikos-%d' % index)
    os.makedirs(rundir, exist_ok=True)
    harness_path = os.path.join(rundir, 'harness.c')
    with open(harness_path, 'w') as stream:
        stream.write(source)
    result = _run_pair(kit, opt, rundir, harness_path, stub_src, probes,
                       buffers, domain, cpu, wall, timeout_run,
                       real_symbol, real_libs, warn)
    if result is not None:
        if len(_CACHE) >= 512:
            _CACHE.pop(next(iter(_CACHE)))
        _CACHE[key] = result
    return result


def _run_pair(kit, opt, rundir, harness_path, stub_src, probes, buffers,
              domain, cpu, wall, timeout_run, real_symbol, real_libs,
              warn):
    settings = kit.settings
    common = [settings.clang(), '-c', '-emit-llvm'] + \
        kit.analyzer.clang_ikos_flags() + \
        _ikos_include_flags(settings)
    harness_bc = os.path.join(rundir, 'harness.bc')
    stub_bc = os.path.join(rundir, 'stub.bc')
    linked = os.path.join(rundir, 'linked.bc')
    db_path = os.path.join(rundir, 'judge.db')
    try:
        for src, out in ((harness_path, harness_bc), (stub_src, stub_bc)):
            proc = subprocess.run(common + [src, '-o', out],
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE,
                                  universal_newlines=True,
                                  errors='replace')
            if proc.returncode != 0:
                warn('ikosjudge: clang failed: %s'
                     % proc.stderr.strip()[-200:])
                return None
        opt_path = shutil_which_opt(settings)
        _mem2reg(opt_path, harness_bc, warn)
        _mem2reg(opt_path, stub_bc, warn)
        proc = subprocess.run([settings.llvm_link(), harness_bc, stub_bc,
                               '-o', linked],
                              stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE,
                              universal_newlines=True, errors='replace')
        if proc.returncode != 0:
            warn('ikosjudge: llvm-link failed: %s'
                 % proc.stderr.strip()[-200:])
            return None

        started = time.time()
        proc = subprocess.run(
            [settings.ikos(), linked, '-o', db_path, '--color=no', '-q',
             '-a=dbg,prover', '-d', domain, '--cpu', str(cpu)]
            + _memory_limit_flags(),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            universal_newlines=True, errors='replace', timeout=wall)
        wall_used = round(time.time() - started, 3)
        try:  # keep the raw debugger output for --dt-keep-temps forensics
            with open(os.path.join(rundir, 'ikos.stdout'), 'w') as stream:
                stream.write(proc.stdout)
        except OSError:
            pass
        if proc.returncode != 0 or not os.path.isfile(db_path):
            warn('ikosjudge: analyzer failed or produced no database')
            return None
        values = parse_print_values(proc.stdout)
        if not values:
            warn('ikosjudge: no print_values output for %s' % rundir)
            return None
        probe_status, stub_asserts, all_unreachable = read_checks(
            db_path, harness_path, stub_src)
        return {'domain': domain, 'wall': wall_used, 'values': values,
                'probe_status': probe_status,
                'stub_asserts': stub_asserts,
                'all_unreachable': all_unreachable,
                'probes': probes,
                'witness': _witness_run(kit, opt, harness_path, rundir,
                                        buffers, timeout_run,
                                        real_symbol, real_libs, warn)}
    except subprocess.TimeoutExpired:
        warn('ikosjudge: analyzer exceeded %ss wall budget' % wall)
        return None
    except OSError as error:
        warn('ikosjudge: %s' % error)
        return None


def shutil_which_opt(settings):
    import shutil
    try:
        return settings.opt()
    except Exception:
        return shutil.which('opt')


def _witness_run(kit, opt, harness_path, rundir, buffers, timeout_run,
                 real_symbol, real_libs, warn):
    """Native build of the same harness against the REAL function."""
    clang = kit.settings.clang()
    binary = os.path.join(rundir, 'witness.bin')
    sanitize = [] if getattr(opt, 'dt_no_sanitize', False) else \
        ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    cmd = [clang, '-O0', '-g', '-U_FORTIFY_SOURCE',
           '-D_FORTIFY_SOURCE=0', '-DIKOSJUDGE_WITNESS',
           ] + _ikos_include_flags(kit.settings) + sanitize + \
        [harness_path, '-o', binary, '-lm'] + list(real_libs or [])
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE,
                              universal_newlines=True, errors='replace')
        if proc.returncode != 0:
            warn('ikosjudge: witness build failed: %s'
                 % proc.stderr.strip()[-160:])
            return None
        import resource

        def limits():
            # no RLIMIT_AS: the witness is an ASan build like the real
            # side of the battery, whose shadow reservation needs the
            # address space (the stub-side cap exists for recorder runs)
            resource.setrlimit(resource.RLIMIT_NOFILE, (32, 32))
            resource.setrlimit(resource.RLIMIT_FSIZE, (1 << 20,) * 2)

        started = time.time()
        # The witness links and calls the REAL function.  It must use the
        # same filesystem isolation as the differential runner: otherwise
        # a target such as chmod/mkdir/unlink can mutate the service's
        # working directory.  Missing Bubblewrap is a harness failure, not
        # a reason to run the witness unsandboxed.
        from difftest import EXEC_PREFIX, _sandbox_prefix
        sandbox = _sandbox_prefix(rundir)
        if not sandbox:
            warn('ikosjudge: witness requires Bubblewrap; refusing to'
                 ' run unsandboxed')
            return None
        argv = sandbox + list(EXEC_PREFIX) + [binary]
        run = subprocess.run(argv, stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL,
                              universal_newlines=True, errors='replace',
                              timeout=timeout_run + 5, preexec_fn=limits,
                              cwd=rundir)
        report = parse_witness(run.stdout)
        report['wall'] = round(time.time() - started, 3)
        if run.returncode != 0:
            report['ok'] = False
            report['reason'] = 'witness exited %d' % run.returncode
            return report
        report['ok'] = True
        report['blocks'] = classify_blocks(
            report, {tag: length for tag, _v, length in buffers})
        return report
    except subprocess.TimeoutExpired:
        return {'ok': False, 'reason': 'witness timed out'}
    except OSError as error:
        warn('ikosjudge: witness %s' % error)
        return None


def summary(result):
    """Compact JSON-safe projection for reports and game persistence."""
    if not result:
        return None
    witness = result.get('witness') or {}
    return {'domain': result['domain'], 'wall': result['wall'],
            'values': result['values'],
            'probe_status': {str(k): v
                             for k, v in
                             result['probe_status'].items()},
            'probes': result['probes'],
            'all_unreachable': result['all_unreachable'],
            'witness': {'ok': witness.get('ok'),
                        'ret': witness.get('ret'),
                        'ret_ptr': witness.get('ret_ptr'),
                        'blocks': {k: list(v)
                                   for k, v in
                                   (witness.get('blocks') or
                                    {}).items()}}}


#############################
# Stub self-check #
#############################

# fallback buffer size (bytes) for pointer parameters whose pointee type
# the driver cannot size for real (opaque struct, typedef'd FILE, ...)
SELFCHECK_FALLBACK_BYTES = 128

# functions whose pointer parameters name a system struct the driver can
# size for real once it includes the right header; without it an opaque
# pointee falls back to a 128-byte buffer and a stub that legitimately
# writes a whole 144-byte struct stat would be misjudged as overflowing
# (the E2 audit's dominant stub-side defect class, mirrored)
SELFCHECK_HEADERS = {
    'stat': 'sys/stat.h',
    'stat64': 'sys/stat.h',
    'fstat64': 'sys/stat.h',
    'lstat64': 'sys/stat.h',
    'sigaction': 'signal.h',
}

# checker/check-kind ids in the checks table (checker/name.hpp and
# checker/kind.hpp of the analyzer, INCLUDING the _Begin/_End sentinel
# enumerators, which shift every following id), for human-readable
# findings
_CHECKER_NAMES = {0: 'boa', 1: 'dbz', 2: 'nullity', 3: 'prover',
                  4: 'upa', 5: 'uva', 6: 'sio', 7: 'uio', 8: 'shc',
                  9: 'poa', 10: 'pcmp', 11: 'sound', 12: 'fca',
                  13: 'dca', 14: 'dfa'}
_CHECK_KIND_NAMES = {2: 'uninitialized-variable', 3: 'assert',
                     4: 'division-by-zero', 5: 'invalid-shift-count',
                     7: 'signed-int-underflow', 8: 'signed-int-overflow',
                     9: 'unsigned-int-underflow',
                     10: 'unsigned-int-overflow',
                     12: 'null-dereference',
                     17: 'invalid-pointer-dereference',
                     22: 'buffer-overflow', 39: 'invalid-free'}

_STRUCT_REF_RE = re.compile(r'\b(struct|union)\s+(\w+)')

# IR pointee shapes worth sizing exactly (a struct/array buffer the
# callee may legitimately write whole); scalar pointees keep the
# SELFCHECK_FALLBACK_BYTES floor because real callees read strings and
# arrays past the first element
_AGGREGATE_IR_RE = re.compile(
    r'^(%"?(?:struct|union|class)[."]|%?\[|[<{])')


def _selfcheck_cxcontext(kit, opt, parts, warn):
    """cxprobe type/size context of the target function; None on any
    failure (no tools, undecodable bitcode, name not found) -- the driver
    then falls back to the header map and the 128-byte buffers."""
    if cxprobe is None:
        return None
    inputs = getattr(opt, 'inputs', None) or ()
    bc = inputs[0] if inputs else None
    if not bc or not bc.endswith('.bc') or not os.path.isfile(bc):
        return None
    open_paren = parts.signature_line.find('(')
    ir_name = parts.signature_line[:open_paren].split()[-1]
    settings = kit.settings
    llvm_bin = getattr(settings, 'LLVM_BIN_DIR', None)
    tools = cxprobe.Tools(
        settings.clang(),
        os.path.join(llvm_bin, 'opt') if llvm_bin else None,
        os.path.join(llvm_bin, 'llvm-dis') if llvm_bin else None)
    cache_dir = os.path.join(
        os.path.expanduser(getattr(opt, 'cache_dir', None)
                           or '~/.cache/ikos-api-spec'),
        'cxprobe')
    try:
        return cxprobe.context_for_name(bc, ir_name,
                                        cache_dir=cache_dir, tools=tools)
    except Exception as error:
        warn('ikosjudge selfcheck: cxprobe unavailable for %s (%s)'
             % (ir_name, error))
        return None


def _selfcheck_driver(parts, source_name, cxcontext=None):
    """C source of a driver calling the stub once; None = out of scope.

    Every input is fully nondeterministic: scalar parameters take
    __ikos_nondet_int, pointer parameters point at a buffer sized by
    cxprobe's bitcode evidence first (bc-typedef/call-site sizes;
    aggregates exactly, scalar pointees with a SELFCHECK_FALLBACK_BYTES
    floor), then at a struct sized through SELFCHECK_HEADERS, then at a
    zeroed fallback buffer; every byte is forgotten with
    __ikos_forget_mem.  A pointer return whose probed pointee is an
    aggregate additionally gets a post-call __ikos_check_mem_access:
    the analyzer then sees how large the returned object must be (the
    getservbyname defect class).
    """
    open_paren = parts.signature_line.find('(')
    params_text = parts.signature_line[open_paren + 1:
                                       parts.signature_line.rfind(')')]
    if params_text.strip() in ('', 'void'):
        params = []
    else:
        params = [p.strip() for p in params_text.split(',') if p.strip()]
    head_tokens = parts.signature_line[:open_paren].split()
    name = head_tokens[-1]
    ret_text = ' '.join(head_tokens[:-1]) if len(head_tokens) > 1 \
        else head_tokens[0]

    # align cxprobe's IR-parameter evidence with the C parameters; a
    # count mismatch (sret/byval ABI lowering) discards the probe rather
    # than mis-sizing by position
    cx_sizes = {}
    cx_ret = None
    if cxcontext:
        named = [i for i, param in enumerate(params) if param != '...']
        cx_params = cxcontext.get('params') or []
        if len(cx_params) == len(named):
            for pos, cx in zip(named, cx_params):
                pointee = cx.get('pointee_ir_type') or ''
                size = cx.get('size')
                if size and cx.get('source') in ('bc-typedef',
                                                 'call-site'):
                    cx_sizes[pos] = (size
                                     if _AGGREGATE_IR_RE.match(pointee)
                                     else max(size,
                                              SELFCHECK_FALLBACK_BYTES))
        cxret = cxcontext.get('return') or {}
        if ('*' in ret_text and cxret.get('size')
                and cxret.get('source') in ('bc-typedef', 'call-site')
                and _AGGREGATE_IR_RE.match(cxret.get('pointee_ir_type')
                                           or '')):
            cx_ret = cxret['size']

    typedef_text = '\n'.join(parts.typedef_lines)
    headers = []
    decls = []
    args = []
    for pos, param in enumerate(params):
        if param == '...':
            continue  # variadic: drive the named parameters only
        if '(' in param:
            # a function-pointer parameter cannot be driven honestly
            # (NULL would make a legitimate callback call a definite
            # error): no evidence, not a rejection
            return None
        var = 'a%d' % pos
        if '*' not in param:
            if 'struct' in param or 'union' in param:
                return None  # by-value aggregate: out of scope
            decls.append('    int %s = __ikos_nondet_int();' % var)
            args.append(var)
            continue
        match = _STRUCT_REF_RE.search(param)
        kind, tag = (match.group(1), match.group(2)) if match \
            else (None, None)
        defined = bool(tag) and re.search(
            r'\b(?:struct|union)\s+%s\s*\{' % re.escape(tag),
            typedef_text)
        header = None
        if tag and not defined:
            header = SELFCHECK_HEADERS.get(source_name or '')
        if pos in cx_sizes:
            decls.append('    void* %s = calloc(1, %d);'
                         % (var, cx_sizes[pos]))
            decls.append('    __ikos_forget_mem(%s, %d);'
                         % (var, cx_sizes[pos]))
            args.append(var)
        elif tag and (defined or header):
            if header and header not in headers:
                headers.append(header)
            decls.append('    %s %s %s;' % (kind, tag, var))
            decls.append('    __ikos_forget_mem(&%s, sizeof(%s %s));'
                         % (var, kind, tag))
            args.append('&%s' % var)
        else:
            decls.append('    void* %s = calloc(1, %d);'
                         % (var, SELFCHECK_FALLBACK_BYTES))
            decls.append('    __ikos_forget_mem(%s, %d);'
                         % (var, SELFCHECK_FALLBACK_BYTES))
            args.append(var)

    lines = []
    lines.append('/* generated by ikosjudge selfcheck -- do not edit */')
    if headers:
        lines.append('#define _GNU_SOURCE 1')
    lines.append('#include <stddef.h>')
    lines.append('#include <stdint.h>')
    lines.append('#include <stdlib.h>')
    for header in headers:
        lines.append('#include <%s>' % header)
    lines.append('#include <ikos/analyzer/intrinsic.h>')
    lines.extend(parts.typedef_lines)
    lines.append('%s;' % parts.signature_line)
    lines.append('')
    lines.append('int main(void) {')
    lines.extend(decls)
    if ret_text == 'void':
        lines.append('    %s(%s);' % (name, ', '.join(args)))
    else:
        lines.append('    %s r = %s(%s);'
                     % (ret_text, name, ', '.join(args)))
        if cx_ret:
            lines.append('    if (r) {')
            lines.append('        __ikos_check_mem_access((void*) r, %d);'
                         % cx_ret)
            lines.append('    }')
        lines.append('    (void) r;')
    lines.append('    return 0;')
    lines.append('}')
    return '\n'.join(lines) + '\n'


def read_stub_findings(db_path, stub_path):
    """([error rows], [warning rows]) located inside the stub file itself.

    status: 0 = proved/safe, 1 = warning, 2 = definite error.  Unlike
    read_checks (assert-prover rows only), every checker contributes:
    the self-check runs the safety analyses, and any definite error
    whose statement lives in the stub source is evidence against it.
    """
    if not os.path.isfile(db_path):
        return [], []
    uri = 'file:%s?mode=ro' % db_path.replace('?', '%3f').replace('#',
                                                                   '%23')
    try:
        conn = sqlite3.connect(uri, uri=True, timeout=10)
        rows = conn.execute(
            'SELECT f.path, s.line, c.checker, c.kind, c.status'
            ' FROM checks c'
            ' JOIN statements s ON c.statement_id = s.id'
            ' JOIN files f ON s.file_id = f.id'
            ' WHERE c.status IN (1, 2)').fetchall()
        conn.close()
    except sqlite3.Error:
        return [], []
    stub_real = os.path.realpath(stub_path)
    errors = []
    warnings = []
    for path, line, checker, kind, status in rows:
        if os.path.realpath(path) != stub_real:
            continue
        row = {'line': line, 'checker': checker, 'kind': kind}
        (errors if status == 2 else warnings).append(row)
    return errors, warnings


def _selfcheck_reason(errors):
    """The repair-round rejection text for definite stub-side errors."""
    findings = []
    for row in errors[:5]:
        checker = _CHECKER_NAMES.get(row['checker'],
                                     'checker-%d' % row['checker'])
        kind = _CHECK_KIND_NAMES.get(row['kind'],
                                     'check-%d' % row['kind'])
        findings.append('%s (%s) at stub line %d'
                        % (kind, checker, row['line']))
    text = '; '.join(findings)
    if len(errors) > 5:
        text += '; +%d more' % (len(errors) - 5)
    return ('self-check: ikos proved %d definite error(s) inside the stub'
            ' body itself: %s.  The stub must stay memory-safe on fully'
            ' nondeterministic inputs: never read or write outside the'
            ' buffers its parameters point to (size them with sizeof on'
            ' the pointee type or the given typedefs, not a guessed'
            ' constant), do not dereference a pointer that can be invalid,'
            ' and keep every __ikos_assert in the body provable'
            % (len(errors), text))


def selfcheck(kit, opt, stub_src, parts, workdir, source_name=None,
              timeout=60):
    """Analyze the stub alone with the real analyzer; -> (verdict, detail).

    verdict: True = no definite error inside the stub file, False =
    definite error(s) found (detail is the repair-round reason), None =
    no evidence (unsupported signature, build failure, analyzer timeout)
    -- the caller must not reject on None.  The run reuses the ikos
    judge's compile/mem2reg/llvm-link chain but poses no harness
    assertions and no debugger: the default domain over
    boa/nullity/uva/prover/dfa, one call of the stub per run.
    """
    settings = kit.settings
    warn = kit.log.warning
    cxcontext = _selfcheck_cxcontext(kit, opt, parts, warn)
    source = _selfcheck_driver(parts, source_name, cxcontext)
    if source is None:
        return None, 'unsupported signature'
    os.makedirs(workdir, exist_ok=True)
    driver_path = os.path.join(workdir, 'selfcheck.c')
    with open(driver_path, 'w') as stream:
        stream.write(source)
    common = [settings.clang(), '-c', '-emit-llvm'] + \
        kit.analyzer.clang_ikos_flags() + \
        _ikos_include_flags(settings)
    driver_bc = os.path.join(workdir, 'selfcheck.bc')
    stub_bc = os.path.join(workdir, 'stub.bc')
    linked = os.path.join(workdir, 'linked.bc')
    db_path = os.path.join(workdir, 'selfcheck.db')
    try:
        for src, out in ((driver_path, driver_bc), (stub_src, stub_bc)):
            proc = subprocess.run(common + [src, '-o', out],
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE,
                                  universal_newlines=True,
                                  errors='replace')
            if proc.returncode != 0:
                warn('ikosjudge selfcheck: clang failed: %s'
                     % proc.stderr.strip()[-200:])
                return None, 'build'
        opt_path = shutil_which_opt(settings)
        _mem2reg(opt_path, driver_bc, warn)
        _mem2reg(opt_path, stub_bc, warn)
        proc = subprocess.run([settings.llvm_link(), driver_bc, stub_bc,
                               '-o', linked],
                              stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE,
                              universal_newlines=True, errors='replace')
        if proc.returncode != 0:
            warn('ikosjudge selfcheck: llvm-link failed: %s'
                 % proc.stderr.strip()[-200:])
            return None, 'link'
        proc = subprocess.run(
            [settings.ikos(), linked, '-o', db_path, '--color=no', '-q',
             '-a=boa,nullity,uva,prover,dfa', '--cpu', '10']
            + _memory_limit_flags(),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            universal_newlines=True, errors='replace', timeout=timeout)
        try:  # keep the raw output for --dt-keep-temps-style forensics
            with open(os.path.join(workdir, 'ikos.stdout'), 'w') as stream:
                stream.write(proc.stdout)
        except OSError:
            pass
    except subprocess.TimeoutExpired:
        warn('ikosjudge selfcheck: analyzer exceeded %ss wall budget'
             % timeout)
        return None, 'timeout'
    except OSError as error:
        warn('ikosjudge selfcheck: %s' % error)
        return None, 'harness'
    if proc.returncode != 0 or not os.path.isfile(db_path):
        return None, 'analyzer failed or produced no database'
    try:
        # A truncated or corrupt result is not evidence of a clean stub.
        with closing(sqlite3.connect(db_path)) as connection:
            connection.execute('SELECT status FROM checks LIMIT 1').fetchall()
    except sqlite3.Error:
        return None, 'invalid analyzer database'
    errors, warnings = read_stub_findings(db_path, stub_src)
    if errors:
        return False, _selfcheck_reason(errors)
    return True, '%d warning(s)' % len(warnings) if warnings else ''
