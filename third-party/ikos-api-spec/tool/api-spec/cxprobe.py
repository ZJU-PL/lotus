#!/usr/bin/env python3
###############################################################################
#
# IKOS API spec -- call-context type probe
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
# PARTICULAR PURPOSE, OR FREEDOM FROM INFRINGEMENT, ANY WARRANTY THAT, IF
# PROVIDED, THE DOCUMENTATION WILL CONFORM TO THE SUBJECT SOFTWARE.  NO
# GOVERNMENT AGENCY, NOR ANY EMPLOYEE THEREOF, ASSUMES ANY LEGAL LIABILITY OR
# RESPONSIBILITY FOR THE ACCURACY, COMPLETENESS, OR USEFULNESS OF ANY
# INFORMATION, APPARATUS, PRODUCT, OR PROCESS DISCLOSED, OR REPRESENTS THAT
# ITS USE WOULD NOT INFRINGE PRIVATELY OWNED RIGHTS.
#
###############################################################################
"""Call-context type probe: real parameter/return types and sizes, from the
bitcode itself.

For every external function a stub is generated for, this module resolves
-- from the program's own .ll disassembly -- the IR type and byte size of
every input parameter and of the return value:

* the declare line carries the IR signature; named struct/union types are
  followed to their ``%struct.X = type { ... }`` definitions (recursively,
  for nested references);
* sizes come from a SIZEOF ORACLE, not a Python layout re-implementation:
  the needed typedef lines are spliced into a probe module together with
  the original ``target datalayout``/``target triple``, each type T is
  wrapped as ``ptrtoint (getelementptr T, T* null, i32 1)`` inside a
  function, and ``opt -passes=instcombine`` constant-folds it to the
  integer size (field offsets via ``i32 0, i32 K`` the same way).
  Alignment, arrays, packing and nested aggregates are whatever LLVM's
  data layout says they are;
* when a parameter or the return is type-erased (``i8*``) or points into
  an opaque struct, the CALL SITES vote: argument-side the real type is
  recovered from bitcast/getelementptr definitions feeding each call
  (global initializers included), return-side from what the result is
  bitcast to / stored into.  The majority type wins and is tagged
  source='call-site'.

The module is standard library plus the LLVM command line tools only.
Tool lookup: an explicit ``Tools`` triple first, then the LLVM bin dir
recorded in the installed ikos python settings (located through
``--ikos-prefix`` or ``ikos-stub`` on PATH, same provenance as the
pipeline's clang flags), then PATH (versioned binaries included).  No
tools -> ProbeError, so callers fall back instead of failing.

CLI: ``python3 cxprobe.py [--ikos-prefix P] X.bc fn1 fn2 ...`` prints the
context table for each function.
"""
import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections import Counter, namedtuple


class ProbeError(Exception):
    """The probe cannot run (no LLVM tools, undecodable bitcode, ...)."""


# the LLVM command line tools; any field may be None except llvm_dis/opt
# for the paths that need them
Tools = namedtuple('Tools', 'clang opt llvm_dis', defaults=(None,) * 3)

DEFAULT_CACHE_DIR = '~/.cache/ikos-api-spec/cxprobe'

# process-local: (realpath, mtime_ns, size, ir_name) -> context dict
_CONTEXT_CACHE = {}


#############################
# Tool discovery #
#############################

def _settings_llvm_bin(ikos_prefix=None):
    """LLVM bin dir recorded in the installed ikos settings file (read,
    not imported: the module must not depend on the ikos package)."""
    prefixes = []
    if ikos_prefix:
        prefixes.append(ikos_prefix)
    exe = shutil.which('ikos-stub') or shutil.which('ikos')
    if exe:
        prefixes.append(os.path.dirname(
            os.path.dirname(os.path.realpath(exe))))
    for prefix in prefixes:
        for layout in ('libexec/lib', 'lib'):
            pattern = os.path.join(prefix, layout, 'python3.*',
                                   'site-packages')
            for site in sorted(glob.glob(pattern), reverse=True):
                settings = os.path.join(site, 'ikos', 'settings',
                                        '__init__.py')
                try:
                    with open(settings) as stream:
                        text = stream.read()
                except OSError:
                    continue
                match = re.search(r"^LLVM_BIN_DIR = '([^']+)'", text,
                                  re.M)
                if match:
                    return match.group(1)
    return None


def find_tools(ikos_prefix=None, llvm_bin=None):
    """(clang, opt, llvm-dis) Tools, or None when opt/llvm-dis are missing."""
    dirs = []
    if llvm_bin:
        dirs.append(llvm_bin)
    recorded = _settings_llvm_bin(ikos_prefix)
    if recorded:
        dirs.append(recorded)
    paths = {}
    for name in ('clang', 'opt', 'llvm-dis'):
        for base in dirs:
            candidate = os.path.join(base, name)
            if os.path.isfile(candidate):
                paths[name] = candidate
                break
        else:
            candidate = shutil.which(name)
            if candidate is None:
                # versioned fallback: llvm-dis-14, opt-16, ...
                for entry in os.environ.get('PATH', '').split(os.pathsep):
                    found = sorted(glob.glob(
                        os.path.join(entry or '.', name + '-*')))
                    if found:
                        candidate = found[-1]
                        break
            if candidate:
                paths[name] = candidate
    if 'opt' not in paths or 'llvm-dis' not in paths:
        return None
    return Tools(paths.get('clang'), paths['opt'], paths['llvm-dis'])


#############################
# Disassembly cache #
#############################

def ir_text(bc, cache_dir=None, tools=None, ikos_prefix=None):
    """The .ll text of bc; disk-cached by (abspath, mtime, size).

    A large bitcode is disassembled once and shared by every function
    probed for it.  Raises ProbeError when llvm-dis is unavailable or
    rejects the file.
    """
    if tools is None:
        tools = find_tools(ikos_prefix)
    if tools is None or not tools.llvm_dis:
        raise ProbeError('llvm-dis not found')
    path = os.path.realpath(bc)
    try:
        info = os.stat(path)
    except OSError as error:
        raise ProbeError('cannot stat %s: %s' % (bc, error))
    cache_root = os.path.expanduser(cache_dir or DEFAULT_CACHE_DIR)
    # the tool matters: a newer llvm-dis re-prints a typed bitcode in
    # opaque-pointer form and would poison the cache for the pipeline's
    # own (matching) llvm-dis
    try:
        version = subprocess.run([tools.llvm_dis, '--version'],
                                 stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT,
                                 universal_newlines=True,
                                 errors='replace').stdout
    except OSError:
        version = tools.llvm_dis
    digest = hashlib.sha256()
    digest.update(path.encode('utf-8'))
    digest.update(b'\0')
    digest.update(version.encode('utf-8', 'replace'))
    key = digest.hexdigest()[:16]
    tag = '%s-%d-%d.ll' % (key, info.st_size, info.st_mtime_ns)
    cached = os.path.join(cache_root, tag)
    try:
        with open(cached) as stream:
            return stream.read()
    except OSError:
        pass
    proc = subprocess.run([tools.llvm_dis, '-o', '-', path],
                          stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE,
                          universal_newlines=True, errors='replace')
    if proc.returncode != 0:
        raise ProbeError('llvm-dis failed on %s: %s'
                         % (bc, proc.stderr.strip()[-200:]))
    try:
        os.makedirs(cache_root, exist_ok=True)
        tmp = cached + '.tmp'
        with open(tmp, 'w') as stream:
            stream.write(proc.stdout)
        os.replace(tmp, cached)
        for stale in glob.glob(os.path.join(cache_root, key + '-*.ll')):
            if stale != cached:
                try:
                    os.unlink(stale)
                except OSError:
                    pass
    except OSError:
        pass  # caching is best effort; the text is already in hand
    return proc.stdout


#############################
# IR type parsing #
#############################

# quoted names ("..."), identifiers (including %/@-prefixed and the '...'
# varargs token), one punctuation character otherwise
_TOKEN_RE = re.compile(r'"(?:\\.|[^"\\])*"|[\w%@.$:\-]+|[^\s]')

_INT_RE = re.compile(r'^i(\d+)$')

# parameter/return attributes that may precede a type; LLVM prints
# parameter attributes AFTER the type, return attributes BEFORE it
_LEADING_ATTR_RE = re.compile(
    r'^(?:noundef|nonnull|noalias|nocapture|signext|zeroext|inreg'
    r'|readonly|writeonly|readnone|returned|immarg|nest|swiftself'
    r'|swifterror|dso_local|dso_preemptable|local_unnamed_addr'
    r'|unnamed_addr|align\s+\d+'
    r'|(?:byval|sret|inalloca|byref|preallocated|elementtype)'
    r'\s*\([^)]*\)'
    r'|dereferenceable(?:_or_null)?\s*\(\s*\d+\s*\))\s+')


class _TypeParser:
    def __init__(self, text):
        self.tokens = _TOKEN_RE.findall(text)
        self.pos = 0

    def peek(self):
        return self.tokens[self.pos] if self.pos < len(self.tokens) \
            else None

    def take(self):
        token = self.peek()
        self.pos += 1
        return token


def _parse_base(parser):
    token = parser.take()
    if token is None:
        raise ProbeError('type expected')
    if token == 'void':
        return ('void', )
    if token == 'ptr':
        # LLVM >= 15 opaque pointer: a pointer with the pointee erased
        return ('optr', )
    if token in ('half', 'bfloat', 'float', 'double', 'fp128', 'x86_fp80',
                 'ppc_fp128', 'label', 'metadata', 'token'):
        return ('prim', token)
    match = _INT_RE.match(token)
    if match:
        return ('int', int(match.group(1)))
    if token == '{':
        elems = _parse_aggregate_body(parser, '}')
        return ('struct', False, elems)
    if token == '<':
        if parser.peek() == '{':
            parser.take()
            elems = _parse_aggregate_body(parser, '}')
            if parser.peek() == '>':
                parser.take()
            return ('struct', True, elems)
        vscale = False
        if parser.peek() == 'vscale':
            parser.take()
            vscale = True
            if parser.peek() == 'x':
                parser.take()
        count = int(parser.take())
        if parser.peek() == 'x':
            parser.take()
        elem = _parse_type(parser)
        if parser.peek() == '>':
            parser.take()
        return ('vector', count, elem, vscale)
    if token == '[':
        count = int(parser.take())
        if parser.peek() == 'x':
            parser.take()
        elem = _parse_type(parser)
        if parser.peek() == ']':
            parser.take()
        return ('array', count, elem)
    if token.startswith('%') or token.startswith('"'):
        name = token if token.startswith('%') else '%' + token
        return ('named', name)
    raise ProbeError('cannot parse type near %r' % token)


def _parse_aggregate_body(parser, closer):
    elems = []
    while parser.peek() is not None and parser.peek() != closer:
        elems.append(_parse_type(parser))
        if parser.peek() == ',':
            parser.take()
    parser.take()  # the closer
    return elems


def _parse_type(parser):
    node = _parse_base(parser)
    while True:
        token = parser.peek()
        if token == '*':
            parser.take()
            node = ('ptr', node)
        elif token == 'addrspace':
            parser.take()
            if parser.peek() == '(':
                parser.take()
                while parser.peek() not in (')', None):
                    parser.take()
                parser.take()
            # the address space qualifies the following '*'; the probe
            # only needs the pointer shape, so it is dropped
        elif token == '(':
            parser.take()
            params = []
            vararg = False
            while parser.peek() is not None and parser.peek() != ')':
                if parser.peek() == '...':
                    parser.take()
                    vararg = True
                else:
                    params.append(_parse_type(parser))
                if parser.peek() == ',':
                    parser.take()
            parser.take()
            node = ('func', node, params, vararg)
        else:
            return node


def parse_type(text):
    """One IR type -> node; ProbeError on text outside the subset."""
    return _parse_type(_TypeParser(text.strip()))


def render_type(node, opaque=False):
    """Node -> canonical IR text; opaque=True emits LLVM >= 15 'ptr'."""
    kind = node[0]
    if kind == 'void':
        return 'void'
    if kind == 'optr':
        return 'ptr'
    if kind == 'prim':
        return node[1]
    if kind == 'int':
        return 'i%d' % node[1]
    if kind == 'named':
        return node[1]
    if kind == 'struct':
        body = ', '.join(render_type(elem, opaque) for elem in node[2])
        return '<{ %s }>' % body if node[1] else '{ %s }' % body
    if kind == 'array':
        return '[%d x %s]' % (node[1], render_type(node[2], opaque))
    if kind == 'vector':
        if node[3]:
            return '<vscale x %d x %s>' % (node[1],
                                           render_type(node[2], opaque))
        return '<%d x %s>' % (node[1], render_type(node[2], opaque))
    if kind == 'ptr':
        if opaque:
            return 'ptr'
        return '%s*' % render_type(node[1], opaque)
    if kind == 'func':
        params = [render_type(param, opaque) for param in node[2]]
        if node[3]:
            params.append('...')
        return '%s (%s)' % (render_type(node[1], opaque),
                            ', '.join(params))
    raise ProbeError('cannot render type node %r' % (node, ))


def _split_top(text):
    """Split on top-level commas (nested brackets and quoted spans safe)."""
    parts = []
    depth = 0
    current = []
    index = 0
    while index < len(text):
        char = text[index]
        if char == '"':
            end = index + 1
            while end < len(text) and text[end] != '"':
                end += 2 if text[end] == '\\' else 1
            current.append(text[index:end + 1])
            index = end + 1
            continue
        if char in '([{<':
            depth += 1
        elif char in ')]}>':
            depth -= 1
        if char == ',' and depth == 0:
            parts.append(''.join(current))
            current = []
        else:
            current.append(char)
        index += 1
    parts.append(''.join(current))
    return parts


def _strip_leading_attrs(text):
    while True:
        match = _LEADING_ATTR_RE.match(text)
        if not match:
            return text
        text = text[match.end():]


_DECLARED_NAME_RE = re.compile(
    r'@("(?:\\.|[^"\\])*"|[\w$.\-]+)\s*\(')


def _declared_name(line):
    match = _DECLARED_NAME_RE.search(line)
    if not match:
        return None
    name = match.group(1)
    return name[1:-1] if name.startswith('"') else name


def parse_declare(line):
    """(ret node, [param nodes], vararg) out of an IR declare line."""
    match = _DECLARED_NAME_RE.search(line)
    if not match:
        raise ProbeError('no function name in declare line')
    ret_text = line[:match.start()].strip()
    if ret_text.startswith('declare'):
        ret_text = ret_text[len('declare'):].strip()
    ret_text = _strip_leading_attrs(ret_text)
    ret_node = parse_type(ret_text)
    # the parameter list spans from the name's '(' to its matching ')'
    open_idx = line.index('(', match.end() - 1)
    depth = 0
    close_idx = len(line)
    index = open_idx
    while index < len(line):
        char = line[index]
        if char == '"':
            index += 1
            while index < len(line) and line[index] != '"':
                index += 2 if line[index] == '\\' else 1
        elif char == '(':
            depth += 1
        elif char == ')':
            depth -= 1
            if depth == 0:
                close_idx = index
                break
        index += 1
    params_text = line[open_idx + 1:close_idx].strip()
    params = []
    vararg = False
    if params_text and params_text != 'void':
        for raw in _split_top(params_text):
            raw = raw.strip()
            if raw == '...':
                vararg = True
                continue
            parser = _TypeParser(raw)
            try:
                node = _parse_type(parser)
            except (ProbeError, ValueError):
                # a parameter attribute preceding the type (old style)
                node = _parse_type(
                    _TypeParser(_strip_leading_attrs(raw)))
            params.append(node)
    return ret_node, params, vararg


_NAMED_REF_RE = re.compile(r'%"(?:\\.|[^"\\])*"|%[\w$.\-]+')


def named_refs(text):
    """Every %named type referenced by an IR type/body text."""
    return _NAMED_REF_RE.findall(text)


#############################
# Typedef extraction #
#############################

_TYPEDEF_RE = re.compile(
    r'^(%"(?:\\.|[^"\\])*"|%[\w$.\-]+) = type (.+)$')

_TARGET_RE = re.compile(r'^target (datalayout|triple) = "(.*)"', re.M)


def extract_typedefs(ir):
    """{name: body} for every '%X = type body' line; body may be 'opaque'."""
    defs = {}
    for line in ir.splitlines():
        match = _TYPEDEF_RE.match(line)
        if match:
            defs[match.group(1)] = match.group(2).strip()
    return defs


def _collect_typedefs(texts, defs):
    """{name: body-or-None} closure of the named types texts reference."""
    out = {}
    work = [ref for text in texts for ref in named_refs(text)]
    while work:
        name = work.pop()
        if name in out:
            continue
        body = defs.get(name)
        if body is None or body == 'opaque':
            out[name] = None  # opaque / not defined in this bitcode
            continue
        out[name] = body
        work.extend(named_refs(body))
    return out


#############################
# Size / offset oracle #
#############################

def _probeable(node, typedefs):
    """False for types the GEP probe cannot size (void, functions,
    scalable vectors, references to opaque/undefined named types)."""
    kind = node[0]
    if kind in ('void', 'func'):
        return False
    if kind == 'prim':
        return node[1] in ('half', 'bfloat', 'float', 'double', 'fp128',
                           'x86_fp80', 'ppc_fp128')
    if kind == 'vector' and node[3]:
        return False
    for ref in named_refs(render_type(node)):
        if typedefs.get(ref) is None:
            return False
    return True


_OPAQUE_PTR_RE = re.compile(r'(?<![\w$.\-%@])ptr(?![\w$.\-])')


def _ir_uses_opaque(ir):
    """Does the disassembly spell pointers as LLVM >= 15 'ptr'?"""
    for line in ir.splitlines():
        if (line.startswith(('declare', 'define')) or ' = type ' in line) \
                and _OPAQUE_PTR_RE.search(line):
            return True
    return False


def _probe_batch(entries, typedefs, tools, datalayout=None, triple=None,
                 opaque_hint=False):
    """{key: int or None} for [(key, kind, node, field)] oracle entries.

    kind 'size': ptrtoint (getelementptr (T, T* null, i32 1)).
    kind 'offset': ... i32 0, i32 field -- the byte offset of a struct
    member, ptrtoint'd from the member's own pointer type.  One opt run
    folds every probe; opaque_hint (or a typed-pointer failure) retries
    in opaque-pointer ('ptr') spelling for LLVM >= 15 toolchains.
    """
    results = {key: None for key, _kind, _node, _field in entries}
    live = [(idx, entry) for idx, entry in enumerate(entries)
            if _probeable(entry[2], typedefs)]
    if not live:
        return results

    def build(opaque):
        lines = []
        if datalayout:
            lines.append('target datalayout = "%s"' % datalayout)
        if triple:
            lines.append('target triple = "%s"' % triple)
        for name in sorted(typedefs):
            body = typedefs[name]
            if body is not None:
                lines.append('%s = type %s' % (name, body))
        lines.append('')
        for idx, (_key, kind, node, field) in live:
            text = render_type(node, opaque)
            ptr = 'ptr' if opaque else '%s*' % text
            if kind == 'offset':
                field_text = render_type(field[1], opaque)
                fptr = 'ptr' if opaque else '%s*' % field_text
                gep = ('getelementptr %s, %s null, i32 0, i32 %d'
                       % (text, ptr, field[0]))
                cast = 'ptrtoint %s %%p to i64' % fptr
            else:
                gep = 'getelementptr %s, %s null, i32 1' % (text, ptr)
                cast = 'ptrtoint %s %%p to i64' % ptr
            lines.append('define i64 @__cxprobe_%d() {' % idx)
            lines.append('  %%p = %s' % gep)
            lines.append('  %%v = %s' % cast)
            lines.append('  ret i64 %v')
            lines.append('}')
        return '\n'.join(lines) + '\n'

    folded = None
    for opaque in ((opaque_hint, not opaque_hint)
                   if opaque_hint else (False, True)):
        source = build(opaque)
        with tempfile.NamedTemporaryFile('w', suffix='.ll',
                                         delete=False) as stream:
            stream.write(source)
            probe_path = stream.name
        try:
            for flags in (['-passes=instcombine'], ['-instcombine']):
                proc = subprocess.run(
                    [tools.opt, '-S'] + flags + [probe_path, '-o', '-'],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    universal_newlines=True, errors='replace')
                if proc.returncode == 0:
                    folded = proc.stdout
                    break
        except OSError:
            folded = None
        finally:
            try:
                os.unlink(probe_path)
            except OSError:
                pass
        if folded is not None:
            break
    if folded is None:
        return results
    for match in re.finditer(
            r'define i64 @__cxprobe_(\d+)\(\)\s*\{\s*ret i64 (\d+)',
            folded):
        idx = int(match.group(1))
        key = entries[idx][0]
        results[key] = int(match.group(2))
    return results


#############################
# Call-site type voting #
#############################

_VALUE_RE = r'(?:%[\w$.\-]+|@"(?:\\.|[^"\\])*"|@[\w$.\-]+)'

_DEF_BITCAST_RE = re.compile(
    r'^\s*(%[\w$.\-]+) = bitcast\s+(.+?)\s+(' + _VALUE_RE + r')\s+to\s+'
    r'(.+?)\s*(?:,.*)?$')
_DEF_GEP_RE = re.compile(
    r'^\s*(%[\w$.\-]+) = getelementptr\s+(?:inbounds\s+)?(.+?),\s*.+?'
    r'\s+(' + _VALUE_RE + r')\s*,')
_DEF_GLOBAL_RE = re.compile(
    r'^\s*(@"(?:\\.|[^"\\])*"|@[\w$.\-]+)\s*=\s+'
    r'(?:[\w.]+(?:\(\d+\))?\s+)*?(?:global|constant)\s+(.+)$')
_USE_BITCAST_RE = re.compile(
    r'^\s*(%[\w$.\-]+) = bitcast\s+(.+?)\s+(%[\w$.\-]+)\s+to\s+(.+?)'
    r'\s*(?:,.*)?$')
_USE_GEP_RE = re.compile(
    r'^\s*(%[\w$.\-]+) = getelementptr\s+(?:inbounds\s+)?(.+?),\s*.+?'
    r'\s+(%[\w$.\-]+)\s*,')
_USE_STORE_RE = re.compile(
    r'^\s*store\s+.+?\s+(' + _VALUE_RE + r')\s*,\s*(.+?)\s+('
    + _VALUE_RE + r')\s*(?:,.*)?$')

_INLINE_BITCAST_RE = re.compile(r'\bbitcast\s*\((.+?)\s+' + _VALUE_RE
                                + r'\s+to\s+.+?\)$')
_INLINE_GEP_RE = re.compile(r'\bgetelementptr\s+(?:inbounds\s+)?\(\s*'
                            r'(?:inbounds\s+)?(.+?)\s*,')


def _definition_maps(ir):
    """Variable -> real type evidence, from bitcast/GEP/global defs."""
    real_type = {}    # %var -> pointed-at type text (a pointer type)
    global_type = {}  # @var -> object type text
    for line in ir.splitlines():
        match = _DEF_BITCAST_RE.match(line)
        if match:
            real_type[match.group(1)] = match.group(2)
            continue
        match = _DEF_GEP_RE.match(line)
        if match:
            real_type[match.group(1)] = match.group(2) + '*'
            continue
        match = _DEF_GLOBAL_RE.match(line)
        if match:
            rest = match.group(2).strip()
            # the type runs to the initializer: a c"..." string literal,
            # or the last value token for scalar/compound initializers
            literal = re.match(r'(.+?)\s+c"(?:\\.|[^"\\])*"', rest)
            if literal:
                global_type[match.group(1)] = literal.group(1)
                continue
            stripped = _ARG_VALUE_RE.match(rest)
            global_type[match.group(1)] = (stripped.group(1).strip()
                                           if stripped else rest)
    return real_type, global_type


def _resolve_value_type(value, real_type, global_type, seen=()):
    """Chase a value through bitcasts/GEPs to its real pointer type."""
    if value in seen:
        return None
    seen = seen + (value, )
    if value.startswith('@'):
        found = global_type.get(value)
        return found + '*' if found else None
    found = real_type.get(value)
    if found is None:
        return None
    tokens = found.rsplit(None, 1)
    if len(tokens) == 2 and tokens[1].startswith(('%', '@')):
        resolved = _resolve_value_type(tokens[1], real_type, global_type,
                                       seen)
        if resolved:
            return resolved
    return found


def _iter_call_lines(ir, ir_name):
    name = re.escape(ir_name)
    pattern = re.compile(r'@(?:"%s"|%s)\s*\(' % (name, name))
    for line in ir.splitlines():
        if 'call' not in line and 'invoke' not in line:
            continue
        if re.search(r'^\s*(?:declare|define)\b', line):
            continue
        match = pattern.search(line)
        if match and re.search(r'\b(?:call|invoke)\b',
                               line[:match.start()]):
            yield line, match.end() - 1


def _call_args(line, open_idx):
    """Parameter-list text of a call instruction -> top-level args."""
    depth = 0
    for index in range(open_idx, len(line)):
        char = line[index]
        if char == '(':
            depth += 1
        elif char == ')':
            depth -= 1
            if depth == 0:
                return _split_top(line[open_idx + 1:index])
    return []


_ARG_VALUE_RE = re.compile(
    r'^(.*?)\s+(' + _VALUE_RE + r'|null|undef|poison|zeroinitializer'
    r'|true|false|-?\d+)$', re.S)


def _arg_type_evidence(arg, real_type, global_type):
    """The most honest type for one call argument.

    `TY %v` follows %v through its defining bitcast/GEP/global; inline
    `TY bitcast (REALTY %v to TY)` / `getelementptr (REAL, ...)` constant
    expressions surrender their inner type directly.
    """
    arg = arg.strip()
    match = _INLINE_BITCAST_RE.search(arg)
    if match:
        return match.group(1).strip()
    match = _INLINE_GEP_RE.search(arg)
    if match:
        return match.group(1).strip() + '*'
    match = _ARG_VALUE_RE.match(arg)
    if not match:
        return None
    type_text, value = match.group(1).strip(), match.group(2)
    if value.startswith(('%', '@')):
        resolved = _resolve_value_type(value, real_type, global_type)
        if resolved:
            return resolved
    return type_text


def _call_site_votes(ir, ir_name, n_params, watch_return):
    """([Counter per param index], return Counter) of call-site types."""
    real_type, global_type = _definition_maps(ir)
    param_votes = [Counter() for _ in range(n_params)]
    ret_votes = Counter()
    results = []
    for line, open_idx in _iter_call_lines(ir, ir_name):
        for index, arg in enumerate(_call_args(line, open_idx)):
            if index >= n_params:
                break
            evidence = _arg_type_evidence(arg, real_type, global_type)
            if evidence:
                param_votes[index][evidence] += 1
        if watch_return:
            match = re.match(
                r'\s*(%[\w$.\-]+) =\s*(?:tail\s+|musttail\s+|notail\s+)?'
                r'call\b', line)
            if match:
                results.append(match.group(1))
    if watch_return and results:
        wanted = set(results)
        for line in ir.splitlines():
            match = _USE_BITCAST_RE.match(line)
            if match and match.group(3) in wanted:
                ret_votes[match.group(4)] += 1
                continue
            match = _USE_GEP_RE.match(line)
            if match and match.group(3) in wanted:
                ret_votes[match.group(2) + '*'] += 1
                continue
            match = _USE_STORE_RE.match(line)
            if (match and match.group(1) in wanted
                    and match.group(2).endswith('*')):
                ret_votes[match.group(2)[:-1].strip()] += 1
    return param_votes, ret_votes


#############################
# Context assembly #
#############################

def _is_erased_ptr(node):
    """i8*-style type-erased pointer, or an LLVM >= 15 opaque 'ptr'."""
    return (node[0] == 'optr'
            or (node[0] == 'ptr' and node[1][0] == 'int'
                and node[1][1] == 8))


def _majority(counter):
    if not counter:
        return None
    (winner, _count), = counter.most_common(1) or ((None, 0), )
    return winner


def _entry(index, node, size, source, inferred=None):
    shown = inferred if inferred is not None else node
    return {'index': index,
            'ir_type': render_type(shown),
            'pointee_ir_type': (render_type(shown[1])
                                if shown[0] == 'ptr' else None),
            'size': size,
            'source': source}


def context_from_declare(declare_line, ir_name=None, ir=None, tools=None):
    """Structured type/size context for one IR declare line.

    `ir` (the disassembled module text) unlocks typedefs, the data
    layout and call-site voting; without it sizes degrade to scalar
    defaults and source='opaque' where the bc alone cannot tell.
    """
    if tools is None:
        tools = find_tools()
    if tools is None or not tools.opt:
        raise ProbeError('opt not found')
    ret_node, param_nodes, _vararg = parse_declare(declare_line)
    if ir_name is None:
        ir_name = _declared_name(declare_line)

    typedefs = {}
    datalayout = triple = None
    if ir is not None:
        defs = extract_typedefs(ir)
        texts = [render_type(ret_node)]
        texts += [render_type(node) for node in param_nodes]
        typedefs = _collect_typedefs(texts, defs)
        for kind, value in _TARGET_RE.findall(ir):
            if kind == 'datalayout' and datalayout is None:
                datalayout = value
            elif kind == 'triple' and triple is None:
                triple = value

    def opaque_pointee(node):
        return (node[0] == 'ptr' and node[1][0] == 'named'
                and typedefs.get(node[1][1]) is None)

    # call-site voting only where the declare line itself is silent
    need = [_is_erased_ptr(node) or opaque_pointee(node)
            for node in param_nodes]
    watch_return = _is_erased_ptr(ret_node) or opaque_pointee(ret_node)
    param_votes = ret_votes = None
    if ir is not None and (any(need) or watch_return):
        param_votes, ret_votes = _call_site_votes(ir, ir_name or '',
                                                  len(param_nodes),
                                                  watch_return)

    # adopt the majority call-site type where the declare erased its own
    inferred_params = []
    for index, node in enumerate(param_nodes):
        winner = None
        if need[index] and param_votes is not None:
            winner = _majority(param_votes[index])
        adopted = None
        if winner:
            try:
                candidate = parse_type(winner)
                if candidate[0] == 'ptr' and candidate != node:
                    adopted = candidate
            except ProbeError:
                adopted = None
        inferred_params.append(adopted)
        if adopted is not None:
            texts = [render_type(adopted[1])]
            typedefs.update(_collect_typedefs(
                texts, extract_typedefs(ir) if ir is not None else {}))
    inferred_ret = None
    if watch_return and ret_votes is not None:
        winner = _majority(ret_votes)
        if winner:
            try:
                candidate = parse_type(winner)
                if candidate[0] == 'ptr':
                    inferred_ret = candidate
                    typedefs.update(_collect_typedefs(
                        [render_type(candidate[1])],
                        extract_typedefs(ir) if ir is not None else {}))
            except ProbeError:
                inferred_ret = None

    # one oracle batch sizes every parameter and the return
    def probe_node_for(node):
        if node[0] == 'optr':
            # an opaque pointer's own size (element 'ptr' in the GEP)
            return ('ptr', ('int', 8))
        if node[0] != 'ptr':
            return node
        pointee = node[1]
        # multi-level pointer (char**): the pointee's size is a pointer's
        return ('ptr', ('int', 8)) if pointee[0] == 'ptr' else pointee

    entries = []
    slots = []
    for index, node in enumerate(param_nodes):
        shown = inferred_params[index] or node
        entries.append((len(entries), 'size', probe_node_for(shown),
                        None))
        slots.append(len(entries) - 1)
    shown_ret = inferred_ret or ret_node
    entries.append((len(entries), 'size', probe_node_for(shown_ret),
                    None))
    slots.append(len(entries) - 1)
    sizes = _probe_batch(entries, typedefs, tools, datalayout, triple,
                         _ir_uses_opaque(ir) if ir is not None else False)

    def finish(index, node, inferred):
        shown = inferred or node
        size = sizes.get(slots[index])
        if node[0] == 'optr':
            # LLVM >= 15 spelling: nothing left to recover from
            source = 'call-site' if inferred is not None else 'opaque'
        elif node[0] != 'ptr':
            source = 'scalar'
        elif inferred is not None:
            source = 'call-site'
        elif opaque_pointee(node):
            source = 'opaque'
            size = None
        else:
            source = 'bc-typedef'
        if size is None and source != 'opaque' and shown[0] == 'ptr' \
                and shown[1][0] == 'named':
            source = 'opaque'
        return shown, size, source

    params = []
    for index, node in enumerate(param_nodes):
        shown, size, source = finish(index, node,
                                     inferred_params[index])
        params.append(_entry(index, shown, size, source))
    shown, size, source = finish(len(param_nodes), ret_node,
                                 inferred_ret)
    ret = _entry(-1, shown, size, source)
    ret.pop('index')
    return {'ir_name': ir_name, 'params': params, 'return': ret}


def find_declare(ir, ir_name):
    """The declare line for ir_name in a disassembled module; or None."""
    name = re.escape(ir_name)
    pattern = re.compile(r'^declare\b[^\n]*@(?:"%s"|%s)\s*\('
                         % (name, name), re.M)
    match = pattern.search(ir)
    if not match:
        return None
    start = ir.rfind('\n', 0, match.start()) + 1
    end = ir.find('\n', match.start())
    return ir[start:end if end >= 0 else len(ir)]


def context_signature(declare, bc=None, ir=None, cache_dir=None,
                      tools=None, ikos_prefix=None):
    """Context for a pipeline extern-info object (attrs .line/.name) or a
    raw declare line; bc/ir supply typedefs and call-site evidence."""
    line = getattr(declare, 'line', declare)
    name = getattr(declare, 'name', None) or _declared_name(line)
    if ir is None and bc is not None:
        ir = ir_text(bc, cache_dir, tools, ikos_prefix)
    if tools is None:
        tools = find_tools(ikos_prefix)
    return context_from_declare(line, name, ir=ir, tools=tools)


def context_for_name(bc, ir_name, cache_dir=None, tools=None,
                     ikos_prefix=None):
    """Standalone entry: context of one function, straight from a .bc."""
    if tools is None:
        tools = find_tools(ikos_prefix)
    ir = ir_text(bc, cache_dir, tools, ikos_prefix)
    declare = find_declare(ir, ir_name)
    if declare is None:
        return None
    cache_key = (os.path.realpath(bc), os.stat(bc).st_mtime_ns,
                 os.stat(bc).st_size, ir_name)
    context = context_from_declare(declare, ir_name, ir=ir, tools=tools)
    if len(_CONTEXT_CACHE) >= 256:
        _CONTEXT_CACHE.pop(next(iter(_CONTEXT_CACHE)))
    _CONTEXT_CACHE[cache_key] = context
    return context


#############################
# CLI #
#############################

def main(argv):
    parser = argparse.ArgumentParser(
        description='Probe parameter/return types and sizes of external'
                    ' functions straight from LLVM bitcode.')
    parser.add_argument('bc', metavar='<bitcode>')
    parser.add_argument('names', nargs='+', metavar='<ir-name>')
    parser.add_argument('--ikos-prefix', default=None, metavar='<prefix>')
    parser.add_argument('--cache-dir', default=None, metavar='<dir>')
    opt = parser.parse_args(argv[1:])
    try:
        tools = find_tools(opt.ikos_prefix)
        if tools is None:
            sys.stderr.write('error: LLVM tools (opt, llvm-dis) not'
                             ' found\n')
            return 1
        for name in opt.names:
            context = context_for_name(opt.bc, name, opt.cache_dir,
                                       tools)
            if context is None:
                print('%s: no declare found' % name)
                continue
            print('%s:' % name)
            for param in context['params']:
                print('  param %d: %s pointee=%s size=%s source=%s'
                      % (param['index'], param['ir_type'],
                         param['pointee_ir_type'], param['size'],
                         param['source']))
            ret = context['return']
            print('  return: %s pointee=%s size=%s source=%s'
                  % (ret['ir_type'], ret['pointee_ir_type'],
                     ret['size'], ret['source']))
    except ProbeError as error:
        sys.stderr.write('error: %s\n' % error)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
