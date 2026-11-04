###############################################################################
#
# IKOS stub tool
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
# PARTICULAR PURPOSE, OR NON-INFRINGEMENT, ANY WARRANTY THAT THE SOFTWARE
# WILL BE ERROR FREE, ANY WARRANTY THAT, IF PROVIDED, THE DOCUMENTATION
# WILL CONFORM TO THE SOFTWARE.  NO GOVERNMENT AGENCY NOR ANY EMPLOYEE
# THEREOF, ASSUMES ANY LEGAL LIABILITY OR RESPONSIBILITY FOR THE ACCURACY,
# COMPLETENESS, OR USEFULNESS OF ANY INFORMATION, APPARATUS, PRODUCT, OR
# PROCESS DISCLOSED, OR THAT ITS USE WOULD NOT INFRINGE PRIVATELY OWNED
# RIGHTS.
#
# Neither the United States Government, nor any employee thereof, makes any
# warranty, express or implied, or assumes any legal liability or
# responsibility for the accuracy, completeness, or usefulness of any
# information, apparatus, product, or process disclosed, or represents that
# its use would not infringe privately owned rights.
#
# IN NO EVENT SHALL THE UNITED STATES GOVERNMENT OR ITS CONTRACTORS OR
# SUBCONTRACTORS BE LIABLE FOR ANY CLAIMS, DAMAGES OR OTHER LIABILITY,
# WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
# OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#
# Waiver and Indemnity:  RECIPIENT AGREES TO WAIVE AND AND RELEASE THE
# UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS, AND ANY
# EMPLOYEE THEREOF, FROM ANY AND ALL CLAIMS, DEMANDS OR DAMAGES, INCLUDING
# EXTRA COSTS THAT MAY ARISE FROM THE USE OF THIS SOFTWARE.  RECIPIENT'S
# SOLE REMEDY FOR ANY SUCH MATTER SHALL BE THE IMMEDIATE, UNILATERAL
# TERMINATION OF THIS AGREEMENT.
#
###############################################################################
"""Assemble program bitcode with library function stubs.

This module implements the ``ikos-stub`` command line tool.  It automates
the manual four-step stub workflow of ``analyzer/README.md`` ("Model library
functions to reduce warnings"): write a stub, compile it with the ikos clang
flags, llvm-link it into the program bitcode, analyze.  The stub *source
library* is a pluggable contract: a directory with one ``.c`` file per
function, named after the symbol it defines, so stubs can be written by hand
or produced by any generator.

Sub commands:

* ``check``    reports, for every external function of the input bitcode,
               whether it is handled by the builtin table (skipped), covered
               by a stub in the library, or missing;
* ``scaffold`` generates signed stub skeletons for the missing ones, with
               the C signature derived from the IR ``declare`` line;
* ``link``     compiles the selected stubs, llvm-links everything into a
               single bitcode, and verifies every stub signature against the
               program declaration.

The signature verification is the point of the tool: ``llvm-link`` does NOT
reject a declare/define signature mismatch, it silently rewrites call sites
with a bitcast, so the stub body would run with wrongly typed arguments.
ikos-stub compares the two textual signatures itself and fails hard.

See doc/STUB_TOOL.md for the full documentation.
"""
import argparse
import os
import os.path
import re
import shutil
import subprocess
import sys
import tempfile
import zlib
import hashlib
from collections import namedtuple
from multiprocessing import Pool

from ikos import analyzer
from ikos import api_census
from ikos import args
from ikos import colors
from ikos import log
from ikos import settings


###########
# Layout #
###########

STUB_SUFFIX = '.c'
MANIFEST_NAME = 'manifest.tsv'
PLACEHOLDER_MARK = 'IKOS-STUB-SCAFFOLD-PLACEHOLDER'

# reverse-mapping rules whose variant has the same signature as the source
# function, so a stub can be retargeted to the mounted name with a -D
# command-line macro.  fortify variants (__f_chk) add an argument at a
# position specific to each function, and interceptor names (\x01...) are
# not valid C identifiers; both need an exact-name stub file instead.
RENAME_RULES = frozenset(('lfs64', 'isoc99', 'isoc23'))

STUB_DECISIONS = frozenset(('stub-exact', 'stub-rename', 'stub-manifest'))

# one external per declare line: name, source file, raw line, parsed signature
ExternInfo = namedtuple('ExternInfo', 'name file line signature')

# the outcome for one external name
StubPlan = namedtuple('StubPlan',
                      'name source_name rule decision detail '
                      'stub_file rename_macro')

# a disassembled set of bitcode files
ScanResult = namedtuple('ScanResult', 'externals defines typedefs opaque')

# a stub library directory
StubIndex = namedtuple('StubIndex', 'files manifest placeholders')


#############################
# Textual IR type parsing #
#############################

class SigError(Exception):
    """Raised when a textual IR signature cannot be parsed."""


NAMED_TYPE_RE = re.compile(r'%([A-Za-z$._][-A-Za-z$._0-9]*)')
SCALAR_RE = re.compile(r'(void|half|bfloat|float|double|x86_fp80|fp128|'
                       r'ppc_fp128|label|metadata|token|opaque)\b')
INT_RE = re.compile(r'i(\d+)\b')
OPAQUE_PTR_RE = re.compile(r'ptr(?![\w$.])')
COUNT_RE = re.compile(r'(\d+)\s*x\s')
WORD_RE = re.compile(r'[\w.]+')

# tokens that may sit between `define`/`declare` and the return type
LINKAGE_TOKENS = frozenset((
    'dso_local', 'dso_preemptable', 'hidden', 'protected', 'default',
    'external', 'internal', 'private', 'linkonce', 'linkonce_odr', 'weak',
    'weak_odr', 'appending', 'extern_weak', 'available_externally',
    'dllexport', 'dllimport', 'unnamed_addr', 'local_unnamed_addr',
    'ccc', 'fastcc', 'coldcc', 'x86_stdcallcc', 'x86_fastcallcc',
))

# return-value attributes printed before the return type (`declare zeroext
# i32 @f()`); signext/zeroext participate in signature keys
RET_ATTRS = frozenset(('signext', 'zeroext', 'inreg', 'noundef', 'noalias',
                       'nonnull'))
RET_ATTR_RE = re.compile(r'(signext|zeroext|inreg|noundef|noalias|'
                         r'nonnull)\b')
RET_ATTR_PAREN_RE = re.compile(r'(dereferenceable(?:_or_null)?)'
                               r'\s*\(([^()]*)\)')
RET_ATTR_ALIGN_RE = re.compile(r'align\s+\d+\b')

# parameter attributes: the first group participates in signature keys, the
# rest is annotation only
KEY_ATTRS = frozenset(('byval', 'sret', 'inalloca', 'signext', 'zeroext'))
PARAM_ATTR_RE = re.compile(r'(byval|sret|inalloca|inreg|returned|signext|'
                           r'zeroext|swiftself|swifterror|nest|noalias|'
                           r'nocapture|nofree|nonnull|noundef|writeonly|'
                           r'readonly|immarg|dead_on_unwind)\b')
PARAM_ATTR_PAREN_RE = re.compile(r'(byval|sret|inalloca|preallocated|byref|'
                                 r'elementtype|dereferenceable|'
                                 r'dereferenceable_or_null|align)'
                                 r'\s*\(([^()]*)\)')
ALIGN_RE = re.compile(r'align\s+\d+')


def _skip_spaces(text, pos):
    while pos < len(text) and text[pos] in ' \t':
        pos += 1
    return pos


def _parse_quoted_local_name(text, pos):
    """Parse an LLVM `%"quoted name"` identifier; -> ``(name, end)``."""
    if text[pos:pos + 2] != '%"':
        return None
    pos += 2
    start = pos
    while pos < len(text):
        ch = text[pos]
        if ch == '"':
            return text[start:pos], pos + 1
        if ch == '\\':
            pos += 1
        pos += 1
    return None


def _parse_local_name(text, pos):
    """Parse an unquoted or quoted LLVM local identifier."""
    pos = _skip_spaces(text, pos)
    quoted = _parse_quoted_local_name(text, pos)
    if quoted is not None:
        return quoted
    match = NAMED_TYPE_RE.match(text, pos)
    if match:
        return match.group(1), match.end()
    return None, pos


def parse_type(text, pos):
    """Parse one LLVM type at ``text[pos:]``; return ``(node, end)``.

    A node is a nested tuple:

    * ``('void',)``, ``('double',)``, ... for scalars (no payload),
    * ``('int', bits)``,
    * ``('ptr', pointee)`` with ``pointee is None`` for opaque pointers,
    * ``('named', 'struct._IO_FILE')``,
    * ``('array', n, elem)``, ``('vector', n, elem)``,
    * ``('agg', packed, [elems])``,
    * ``('func', ret, [params], vararg)``.
    """
    pos = _skip_spaces(text, pos)
    if pos >= len(text):
        raise SigError('unexpected end of type')
    ch = text[pos]
    if ch == '{':
        elems, end = _parse_agg_body(text, pos + 1, '}')
        return ('agg', False, elems), end
    if ch == '<':
        if text[pos + 1:pos + 2] == '{':
            elems, end = _parse_agg_body(text, pos + 2, '}')
            end = _skip_spaces(text, end)
            if text[end:end + 1] != '>':
                raise SigError('expected > after packed struct')
            return ('agg', True, elems), end + 1
        match = COUNT_RE.match(text, pos + 1)
        if not match:
            raise SigError('cannot parse vector type')
        elem, end = parse_type(text, match.end())
        end = _skip_spaces(text, end)
        if text[end:end + 1] != '>':
            raise SigError('expected > after vector type')
        return ('vector', int(match.group(1)), elem), end + 1
    if ch == '[':
        match = COUNT_RE.match(text, pos + 1)
        if not match:
            raise SigError('cannot parse array type')
        elem, end = parse_type(text, match.end())
        end = _skip_spaces(text, end)
        if text[end:end + 1] != ']':
            raise SigError('expected ] after array type')
        return ('array', int(match.group(1)), elem), end + 1
    if ch == '%':
        name, end = _parse_local_name(text, pos)
        if name is None:
            raise SigError('unterminated quoted type at %r'
                           % text[pos:pos + 16])
        node = ('named', name)
    else:
        match = OPAQUE_PTR_RE.match(text, pos)
        if match:
            node, end = ('ptr', None), match.end()
        else:
            match = SCALAR_RE.match(text, pos)
            if match:
                node, end = (match.group(1), ), match.end()
            else:
                match = INT_RE.match(text, pos)
                if not match:
                    raise SigError('unknown type at %r' % text[pos:pos + 16])
                node, end = ('int', int(match.group(1))), match.end()
    # postfix pointers, then an optional function-type suffix (always used
    # as a pointer in a signature: `i32 (i8*, ...)*`)
    while True:
        pos = _skip_spaces(text, end)
        if text[pos:pos + 1] == '*':
            node = ('ptr', node)
            end = pos + 1
            continue
        if text[pos:pos + 1] == '(':
            params, vararg, end = _parse_type_list(text, pos + 1)
            node = ('func', node, params, vararg)
            pos = _skip_spaces(text, end)
            if text[pos:pos + 1] != '*':
                raise SigError('function type not used as a pointer')
            node = ('ptr', node)
            end = pos + 1
            continue
        break
    return node, end


def _parse_agg_body(text, pos, closer):
    """Parse ``T, T, ...`` up to ``closer``; return ``([elems], end)``."""
    elems = []
    pos = _skip_spaces(text, pos)
    if text[pos:pos + 1] == closer:
        return elems, pos + 1
    while True:
        node, pos = parse_type(text, pos)
        elems.append(node)
        pos = _skip_spaces(text, pos)
        ch = text[pos:pos + 1]
        if ch == ',':
            pos += 1
            continue
        if ch == closer:
            return elems, pos + 1
        raise SigError('expected %r in aggregate type' % closer)


def _parse_type_list(text, pos):
    """Parse the parameter types of a function type; no attributes here."""
    params = []
    vararg = False
    pos = _skip_spaces(text, pos)
    if text[pos:pos + 1] == ')':
        return params, vararg, pos + 1
    while True:
        if text[pos:pos + 3] == '...':
            vararg = True
            pos += 3
        else:
            node, pos = parse_type(text, pos)
            params.append(node)
        pos = _skip_spaces(text, pos)
        ch = text[pos:pos + 1]
        if ch == ',':
            pos = _skip_spaces(text, pos + 1)
            continue
        if ch == ')':
            return params, vararg, pos + 1
        raise SigError('expected ) after parameter types')


def _attr_type_key(operand):
    """Key of a byval(%struct.X)/sret(%struct.X) operand."""
    operand = operand.strip()
    if operand.startswith('%'):
        name = operand[1:]
        if len(name) >= 2 and name[0] == '"' and name[-1] == '"':
            name = name[1:-1]
        return _named_type_key(name)
    if operand == 'ptr':
        return 'ptr'
    return operand


def _parse_param_attrs(text, pos):
    """Collect parameter attributes after a type; return ``(attrs, end)``."""
    attrs = set()
    while True:
        pos = _skip_spaces(text, pos)
        match = PARAM_ATTR_PAREN_RE.match(text, pos)
        if match:
            name = match.group(1)
            if name in ('byval', 'sret'):
                attrs.add('%s:%s' % (name, _attr_type_key(match.group(2))))
            else:
                attrs.add(name)
            pos = match.end()
            continue
        match = PARAM_ATTR_RE.match(text, pos)
        if match:
            attrs.add(match.group(1))
            pos = match.end()
            continue
        match = ALIGN_RE.match(text, pos)
        if match:
            pos = match.end()
            continue
        break
    return attrs, pos


def _named_key(name):
    """Key of a named type: `struct._IO_FILE` -> `_IO_FILE`.

    The kind prefix is dropped so a C scaffold (which can only emit `struct`
    and `union` tags) matches a C++-origin `%class.Foo` pointee.
    """
    if len(name) >= 2 and name[0] == '"' and name[-1] == '"':
        name = name[1:-1]
    return name.split('.', 1)[1] if '.' in name else name


def _cpp_named_key(name):
    """Stable short key for C++-origin named IR types."""
    return hashlib.sha1(name.encode('utf-8')).hexdigest()[:16]


def _named_type_key(name):
    named = _named_key(name)
    if ':' in named:
        return 'cpp:' + _cpp_named_key(named)
    if (named.startswith('ikos_cxx_')
            and len(named) == len('ikos_cxx_') + 16):
        return 'cpp:' + named[len('ikos_cxx_'):]
    return named


def function_name(line):
    """Normalized function name of a define/declare/alias line, or None."""
    match = api_census.DECLARE_NAME_RE.search(line)
    if not match:
        return None
    name = match.group(1)
    if name is None:
        name = match.group(2)
    if name.startswith('\\01'):
        # the textual form `"\01foo"` spells the interceptor prefix byte
        # the builtin name table writes `"\x01foo"`
        name = '\\x01' + name[3:]
    return name


def parse_signature(line):
    """Parse a textual IR declare/define line into a ``Signature``.

    Returns None for lines that are not declare/define, or whose name cannot
    be extracted.  Raises SigError on a malformed signature.
    """
    if line.startswith('declare'):
        keyword = 'declare'
    elif line.startswith('define'):
        keyword = 'define'
    else:
        return None
    name = function_name(line)
    if name is None:
        return None
    match = api_census.DECLARE_NAME_RE.search(line)
    # skip the keyword, then linkage/visibility tokens and return-value
    # attributes, before the return type (the type text never contains @,
    # so the first @ starts the name)
    pos = len(keyword)
    ret_attrs = set()
    while pos < match.start():
        pos = _skip_spaces(line, pos)
        match_attr = RET_ATTR_PAREN_RE.match(line, pos)
        if match_attr:
            pos = match_attr.end()
            continue
        match_attr = RET_ATTR_ALIGN_RE.match(line, pos)
        if match_attr:
            pos = match_attr.end()
            continue
        match_attr = RET_ATTR_RE.match(line, pos)
        if match_attr:
            ret_attrs.add(match_attr.group(1))
            pos = match_attr.end()
            continue
        word = WORD_RE.match(line, pos)
        if word and word.group(0) in LINKAGE_TOKENS:
            pos = word.end()
        else:
            break
    ret_text = line[pos:match.start()].strip()
    if not ret_text:
        raise SigError('no return type in %r' % line.strip())
    ret_node, _ = parse_type(ret_text, 0)
    # parameter list: first ( after the name, balanced (sret(%X) and
    # function-pointer types contain nested parentheses)
    open_pos = line.find('(', match.end())
    if open_pos < 0:
        raise SigError('no parameter list in %r' % line.strip())
    depth = 0
    close_pos = open_pos
    while close_pos < len(line):
        ch = line[close_pos]
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
            if depth == 0:
                break
        close_pos += 1
    params = []
    vararg = False
    for part in _split_params(line[open_pos + 1:close_pos]):
        if part == '...':
            vararg = True
            continue
        node, end = parse_type(part, 0)
        attrs, _ = _parse_param_attrs(part, end)
        params.append((node, attrs))
    return Signature(name, keyword, ret_node, params, vararg, ret_attrs,
                     line.rstrip('\n'))


def _split_params(inner):
    """Split a parameter list on commas at bracket depth zero."""
    parts = []
    depth = 0
    start = 0
    for idx, ch in enumerate(inner):
        if ch in '([{<':
            depth += 1
        elif ch in ')]}>':
            depth -= 1
        elif ch == ',' and depth == 0:
            parts.append(inner[start:idx].strip())
            start = idx + 1
    tail = inner[start:].strip()
    if tail:
        parts.append(tail)
    return parts


Signature = namedtuple('Signature',
                       'name kind ret params vararg ret_attrs raw')


def parse_typedef_line(line):
    """Parse ``%struct.X = type ...``; return ``(name, body_text) or None``.

    The returned name carries no ``%`` prefix, like NAMED_TYPE_RE groups.
    """
    if not line.startswith('%'):
        return None
    name, end = _parse_local_name(line, 0)
    if name is None:
        return None
    rest = line[end:].strip()
    if not rest.startswith('= type'):
        return None
    body = rest[len('= type'):].strip()
    return name, body


def _iter_types(sig):
    """Yield every type node of a signature (for opaque detection)."""
    yield sig.ret
    for node, _ in sig.params:
        yield node


def _walk_node(node):
    yield node
    kind = node[0]
    if kind == 'ptr' and node[1] is not None:
        for sub in _walk_node(node[1]):
            yield sub
    elif kind in ('array', 'vector'):
        for sub in _walk_node(node[2]):
            yield sub
    elif kind == 'agg':
        for elem in node[2]:
            for sub in _walk_node(elem):
                yield sub
    elif kind == 'func':
        for sub in _walk_node(node[1]):
            yield sub
        for param in node[2]:
            for sub in _walk_node(param):
                yield sub


def type_key(node):
    """Comparison key of a type node (strict, literal).

    Scalars are exact; typed pointers key on their pointee, named pointees
    by tag name.  No relaxation: two pointers with differently spelled
    pointees never match.
    """
    kind = node[0]
    if kind == 'ptr':
        if node[1] is None:
            return 'ptr'
        return 'ptr:' + type_key(node[1])
    if kind == 'named':
        return 'named:' + _named_type_key(node[1])
    if kind == 'int':
        return 'i%d' % node[1]
    if kind in ('array', 'vector'):
        return '%s:%d[%s]' % (kind, node[1], type_key(node[2]))
    if kind == 'agg':
        return 'agg%s(%s)' % ('!' if node[1] else '',
                              ','.join(type_key(e) for e in node[2]))
    if kind == 'func':
        if node[3] and not node[2]:
            return 'fn-open(%s)' % type_key(node[1])
        return 'fn(%s->%s%s)' % (
            ','.join(type_key(p) for p in node[2]),
            type_key(node[1]), '...' if node[3] else '')
    return kind


def signature_key(sig):
    """Comparison key of a signature (return, params, vararg)."""
    params = []
    for node, attrs in sig.params:
        kept = tuple(sorted(a for a in attrs
                            if a.split(':', 1)[0] in KEY_ATTRS))
        if node[0] == 'int' and node[1] in (1, 8):
            # C compilers emit sign/zero extension for `_Bool` and `char`
            # parameters; the extension attribute does not change the ABI.
            kept = tuple(a for a in kept
                         if a not in ('signext', 'zeroext'))
        params.append((type_key(node), kept))
    ret_sign = (tuple() if sig.ret[0] == 'int' and sig.ret[1] in (1, 8)
                else tuple(sorted(a for a in sig.ret_attrs
                                  if a in ('signext', 'zeroext'))))
    return (type_key(sig.ret), ret_sign, tuple(params), sig.vararg)


#####################
# Bitcode scanning #
#####################

def _is_instrumentation(name):
    return name.startswith(api_census.INSTRUMENTATION_PREFIXES)


def _line_info(line, path):
    """ExternInfo of a declare/define line, with signature if parseable."""
    name = function_name(line)
    if name is None or _is_instrumentation(name):
        return None
    try:
        sig = parse_signature(line)
    except SigError as e:
        log.debug('%s: %s' % (path, e))
        sig = None
    return ExternInfo(name, path, line.rstrip('\n'), sig)


def scan_bitcode(path):
    """One bitcode file -> ``(declares, defines, typedefs)``.

    Names are dict keys; alias/ifunc definitions count as defines (clang
    emits e.g. a C1 constructor as an alias of C2), without a signature.
    """
    declares = {}
    defines = {}
    typedefs = {}
    for line in api_census.ir_lines(path):
        if line.startswith('declare'):
            info = _line_info(line, path)
            if info:
                declares.setdefault(info.name, info)
        elif line.startswith('define'):
            info = _line_info(line, path)
            if info:
                defines.setdefault(info.name, info)
        elif line.startswith('@') and (' alias ' in line or
                                       ' ifunc ' in line):
            name = function_name(line)
            if name and not _is_instrumentation(name):
                defines.setdefault(name,
                                   ExternInfo(name, path,
                                              line.rstrip('\n'), None))
        else:
            typedef = parse_typedef_line(line)
            if typedef:
                typedefs.setdefault(typedef[0], typedef[1])
    return declares, defines, typedefs


def _scan_chunk(paths):
    """Scan a chunk of bitcode files (multiprocessing worker)."""
    declares = {}
    defines = {}
    typedefs = {}
    for path in paths:
        path_declares, path_defines, path_typedefs = scan_bitcode(path)
        for name, info in path_declares.items():
            declares.setdefault(name, info)
        for name, info in path_defines.items():
            defines.setdefault(name, info)
        for name, body in path_typedefs.items():
            typedefs.setdefault(name, body)
    return declares, defines, typedefs


def _has_opaque(infos):
    for info in infos.values():
        sig = info.signature
        if sig is None:
            continue
        for node in _iter_types(sig):
            for sub in _walk_node(node):
                if sub == ('ptr', None):
                    return True
    return False


def scan_externals(paths, llvm_dis, jobs):
    """Scan bitcode files; externals = declares - defines (union order)."""
    chunks = api_census.chunked(paths, 32)
    if jobs > 1 and len(chunks) > 1:
        with Pool(processes=jobs,
                  initializer=api_census._init_worker,
                  initargs=(llvm_dis, )) as pool:
            results = pool.map(_scan_chunk, chunks)
    else:
        api_census._init_worker(llvm_dis)
        results = [_scan_chunk(chunk) for chunk in chunks]
    declares = {}
    defines = {}
    typedefs = {}
    for chunk_declares, chunk_defines, chunk_typedefs in results:
        for name, info in chunk_declares.items():
            declares.setdefault(name, info)
        for name, info in chunk_defines.items():
            defines.setdefault(name, info)
        for name, body in chunk_typedefs.items():
            typedefs.setdefault(name, body)
    # a declare that another module defines is internal to the program
    externals = {name: declares[name]
                 for name in sorted(declares) if name not in defines}
    opaque = _has_opaque(externals) or _has_opaque(defines)
    return ScanResult(externals, defines, typedefs, opaque)


def collect_bitcode(paths):
    """Input paths -> a list of .bc files (directories scanned)."""
    files = []
    for path in paths:
        if os.path.isdir(path):
            found = api_census.find_bitcode(path)
            if not found:
                log.warning('%s: no bitcode found; skipped' % path)
            files += found
        elif os.path.isfile(path) and os.path.splitext(path)[1] == '.bc':
            files.append(path)
        else:
            log.error('%s: not a .bc file or a directory' % path)
            sys.exit(1)
    if not files:
        log.error('no bitcode input (build with ikos-scan or'
                  ' clang -emit-llvm first)')
        sys.exit(1)
    return files


##################
# Stub library #
##################

_SAFE_NAME_RE = re.compile(r'[^0-9A-Za-z._-]')


def stub_key(name):
    """Filesystem-safe stub file stem for an IR symbol name."""
    key = _SAFE_NAME_RE.sub('_', name)
    if key != name or not key:
        # keep the mapping collision-free and self-explanatory
        key = '%s-x%08x' % (key or 'stub',
                            zlib.crc32(name.encode('utf-8')) & 0xffffffff)
    return key


def load_manifest(path):
    """manifest.tsv -> {ir_name: stub_key}.

    Each row is ``<stub key> TAB <ir name> [TAB <ir name>...]``: the stub
    file named after the first column is compiled once per extra name with a
    ``-D<key>=<name>`` macro, so it defines that mounted symbol instead.
    """
    manifest = {}
    with open(path, 'r') as stream:
        for lineno, line in enumerate(stream, 1):
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            columns = line.split('\t')
            if len(columns) < 2 or not all(columns):
                log.warning('%s:%d: expected <stub key> TAB <ir name>'
                            ' [TAB <ir name>...]; ignored'
                            % (path, lineno))
                continue
            key = columns[0].strip()
            for name in columns[1:]:
                if name != key:
                    manifest[name.strip()] = key
    return manifest


def load_stub_dir(directory):
    """Load a stub library directory; dies if it does not exist."""
    if not os.path.isdir(directory):
        log.error('%s: not a stub library directory' % directory)
        sys.exit(1)
    files = {}
    for entry in sorted(os.listdir(directory)):
        path = os.path.join(directory, entry)
        if entry.endswith(STUB_SUFFIX) and os.path.isfile(path):
            files[entry[:-len(STUB_SUFFIX)]] = path
    manifest_path = os.path.join(directory, MANIFEST_NAME)
    manifest = {}
    if os.path.isfile(manifest_path):
        manifest = load_manifest(manifest_path)
        for name in sorted(manifest):
            if manifest[name] not in files:
                log.warning('manifest: %s has no stub file %s.c'
                            % (name, manifest[name]))
    placeholders = set()
    for key, path in files.items():
        with open(path, 'r', errors='replace') as stream:
            for line in stream:
                if PLACEHOLDER_MARK in line:
                    placeholders.add(key)
                    break
    return StubIndex(files, manifest, placeholders)


EMPTY_INDEX = StubIndex({}, {}, frozenset())


###############
# Decisions #
###############

def plan_stubs(externals, categories, reverse, index, forced=frozenset()):
    """Decide the fate of every external function.

    Order: builtin table (unless forced) -> exact stub file -> manifest ->
    interceptor check -> same-signature rename -> inalloca check -> missing.
    Explicit intent (an exact file, a manifest row) beats the heuristic.
    """
    plans = []
    for name in sorted(externals):
        info = externals[name]
        source_name, rule = reverse.get(name, (name, ''))
        forced_hit = name in forced or source_name in forced
        stub_file = None
        rename_macro = None
        category = categories.get(name)
        if category and not forced_hit:
            decision, detail = 'builtin', category
        elif stub_key(name) in index.files:
            decision = 'stub-exact'
            stub_file = index.files[stub_key(name)]
            detail = os.path.basename(stub_file)
        elif name in index.manifest:
            key = index.manifest[name]
            decision = 'stub-manifest'
            stub_file = index.files[key]
            rename_macro = '%s=%s' % (key, name)
            detail = '%s.c -D%s' % (key, rename_macro)
        elif name.startswith('\\x01'):
            # not a valid C identifier; needs a hand-written stub with an
            # __asm__ label, registered in manifest.tsv
            decision = 'skip-interceptor'
            detail = 'interceptor name; hand-write with __asm__ label'
        elif (rule in RENAME_RULES and source_name in index.files):
            decision = 'stub-rename'
            stub_file = index.files[source_name]
            rename_macro = '%s=%s' % (source_name, name)
            detail = '%s.c -D%s' % (source_name, rename_macro)
        elif _has_inalloca(info):
            decision = 'skip-inalloca'
            detail = ('inalloca parameter; C cannot express it,'
                      ' hand-write a C++ stub')
        else:
            decision, detail = 'missing', 'no stub file'
        plans.append(StubPlan(name, source_name, rule, decision, detail,
                              stub_file, rename_macro))
    return plans


def _has_inalloca(info):
    sig = info.signature
    if sig is None:
        return False
    return any('inalloca' in attrs for _, attrs in sig.params)


###############
# Scaffold #
###############

class ScaffoldError(Exception):
    """Raised when a stub skeleton cannot be generated."""


C_INT_WIDTHS = {8: 'char', 16: 'short', 32: 'int', 64: 'long long'}
UNSIGNED_C = {8: 'unsigned char', 16: 'unsigned short', 32: 'unsigned',
              64: 'unsigned long long'}
# scalar nodes are single-element tuples: parse_type builds ('double',),
# not ('float', 'double')
C_FLOAT_TYPES = {'float': 'float', 'double': 'double',
                 'x86_fp80': 'long double'}
UNSUPPORTED_FLOATS = frozenset(('half', 'bfloat', 'fp128', 'ppc_fp128'))

C_IDENT_RE = re.compile(r'^[A-Za-z_]\w*$')
C_KEYWORDS = frozenset((
    'auto', 'break', 'case', 'char', 'const', 'continue', 'default', 'do',
    'double', 'else', 'enum', 'extern', 'float', 'for', 'goto', 'if',
    'inline', 'int', 'long', 'register', 'restrict', 'return', 'short',
    'signed', 'sizeof', 'static', 'struct', 'switch', 'typedef', 'union',
    'unsigned', 'void', 'volatile', 'while', '_Bool', '_Complex', 'asm',
))


def _c_type_tag(name):
    """A valid C struct tag for an IR named type.

    C++-origin IR names can contain namespaces and template punctuation
    (`class.std::basic_string`).  Those bytes never reach the program's C
    namespace: the tag only has to be valid, stable, and ABI-compatible
    when used consistently by the generated C translation unit.
    """
    named = _named_key(name)
    tag = re.sub(r'\W', '_', named)
    if ':' in named:
        tag = 'ikos_cxx_' + _cpp_named_key(named)
    if not tag or not re.match(r'^[A-Za-z_]\w*$', tag) or tag in C_KEYWORDS:
        tag = 'ikos_abi_' + tag
    return tag


def _new_context(typedefs):
    """State for rendering one stub file (typedef table, prelude lines)."""
    return {'typedefs': typedefs, 'prelude': [], 'emitted': set(),
            'forwarded': set(), 'anon': 0}


def _c_scalar(node):
    kind = node[0]
    if kind == 'int':
        width = node[1]
        if width == 1:
            return '_Bool'
        if width == 128:
            return '__int128'
        if width not in C_INT_WIDTHS:
            raise ScaffoldError('cannot map i%d to C' % width)
        return C_INT_WIDTHS[width]
    if kind in C_FLOAT_TYPES:
        return C_FLOAT_TYPES[kind]
    if kind in UNSUPPORTED_FLOATS:
        raise ScaffoldError('cannot map %s to C' % kind)
    raise ScaffoldError('cannot map %r to a C scalar' % (node, ))


def _c_fields(node, ctx):
    """Field declarations of a struct/union body (without semicolons)."""
    kind = node[0]
    if kind == 'agg':
        return ['%s f%d' % (_c_value_type(elem, ctx), index)
                for index, elem in enumerate(node[2])]
    if kind == 'named':
        return ['%s f0' % _c_value_type(node, ctx)]
    if kind == 'array':
        return ['%s f0[%d]' % (_c_value_type(node[2], ctx), node[1])]
    if kind == 'ptr':
        return ['%s f0' % _c_pointer(node[1], ctx)]
    if kind == 'vector':
        raise ScaffoldError('cannot map a vector type to C')
    return ['%s f0' % _c_scalar(node)]


def _c_named_struct(name, ctx):
    """Ensure a C struct/union definition exists; return ``(kind, tag)``.

    The C tag reproduces the IR name: `%struct._IO_FILE` comes from
    ``struct _IO_FILE``, so emitting that tag gives the same pointee name.
    """
    c_kind, tag = _c_forward_struct(name, ctx)
    if name not in ctx['emitted']:
        ctx['emitted'].add(name)
        body = ctx['typedefs'].get(name)
        if body is None or body == 'opaque':
            return c_kind, tag
        else:
            node, _ = parse_type(body, 0)
            # render fields first: nested typedefs must precede this struct
            fields = _c_fields(node, ctx)
            ctx['prelude'].append('%s %s {' % (c_kind, tag))
            for field in fields:
                ctx['prelude'].append('    %s;' % field)
            ctx['prelude'].append('};')
    return c_kind, tag


def _c_forward_struct(name, ctx):
    """Return ``(kind, tag)`` and emit only an incomplete declaration."""
    tag = _c_type_tag(name)
    named = _named_key(name)
    c_kind = 'union' if named.split('.', 1)[0] == 'union' else 'struct'
    if name not in ctx['forwarded']:
        ctx['forwarded'].add(name)
        ctx['prelude'].append('%s %s;' % (c_kind, tag))
    return c_kind, tag


def _c_anon_agg(elems, packed, ctx):
    """typedef for an anonymous by-value aggregate; returns its name.

    Used by value (parameter or return), the typedef compiles back to the
    literal IR aggregate `{ ... }`, matching structurally.
    """
    count = ctx['anon']
    ctx['anon'] += 1
    name = 'ikos_stub_agg_%d' % count
    fields = ['%s f%d;' % (_c_value_type(elem, ctx), index)
              for index, elem in enumerate(elems)]
    head = ('typedef struct __attribute__((packed)) {' if packed
            else 'typedef struct {')
    ctx['prelude'].append(head)
    for field in fields:
        ctx['prelude'].append('    %s' % field)
    ctx['prelude'].append('} %s;' % name)
    return name


def _c_anon_array(count, elem, ctx):
    """typedef for an array; returns its name."""
    number = ctx['anon']
    ctx['anon'] += 1
    name = 'ikos_stub_arr_%d' % number
    ctx['prelude'].append('typedef %s %s[%d];'
                          % (_c_value_type(elem, ctx), name, count))
    return name


def _c_fnptr(fn_node, ctx):
    """typedef for a function pointer; returns its name."""
    count = ctx['anon']
    ctx['anon'] += 1
    name = 'ikos_stub_fn_%d' % count
    ret_c = ('void' if fn_node[1] == ('void', )
             else _c_value_type(fn_node[1], ctx))
    arg_types = [_c_value_type(param, ctx) for param in fn_node[2]]
    if fn_node[3]:
        if not fn_node[2]:
            # C requires a named parameter before `...`; an opaque object
            # pointer preserves the pointer ABI for this unused callback type.
            arg_types = ['void*']
        arg_types.append('...')
    if not arg_types:
        arg_types = ['void']
    ctx['prelude'].append('typedef %s (*%s)(%s);'
                          % (ret_c, name, ', '.join(arg_types)))
    return name


def _c_pointer(pointee, ctx):
    """C type string (ending in ``*``) for a typed-pointer pointee."""
    if pointee is None or pointee == ('int', 8):
        return 'void*'
    kind = pointee[0]
    if kind == 'ptr':
        return _c_pointer(pointee[1], ctx) + '*'
    if kind == 'named':
        c_kind, tag = _c_forward_struct(pointee[1], ctx)
        return '%s %s*' % (c_kind, tag)
    if kind == 'func':
        return _c_fnptr(pointee, ctx)
    if kind == 'array':
        return _c_anon_array(pointee[1], pointee[2], ctx) + '*'
    if kind == 'vector':
        raise ScaffoldError('cannot map a vector pointer to C')
    return _c_scalar(pointee) + '*'


def _c_value_type(node, ctx):
    """C type for a node used by value."""
    kind = node[0]
    if kind == 'ptr':
        return _c_pointer(node[1], ctx)
    if kind == 'named':
        body = ctx['typedefs'].get(node[1])
        if body is None or body == 'opaque':
            raise ScaffoldError('incomplete named type %r used by value'
                                % node[1])
        c_kind, tag = _c_named_struct(node[1], ctx)
        return '%s %s' % (c_kind, tag)
    if kind == 'agg':
        return _c_anon_agg(node[2], node[1], ctx)
    if kind == 'array':
        return _c_anon_array(node[1], node[2], ctx)
    if kind == 'func':
        raise ScaffoldError('bare function type in a signature')
    return _c_scalar(node)


def _c_type_of(node, ctx, unsigned=False):
    if unsigned and node[0] == 'int' and node[1] in UNSIGNED_C:
        return UNSIGNED_C[node[1]]
    return _c_value_type(node, ctx)


def _ret_kind(node, unsigned):
    kind = node[0]
    if kind == 'int':
        return 'uint' if unsigned else 'int'
    if kind in C_FLOAT_TYPES:
        return 'float'
    if kind == 'ptr':
        return 'ptr'
    return 'agg'


def _type_size_align(node, ctx):
    """Native size/alignment of an IR value type in the C scaffold."""
    kind = node[0]
    if kind == 'int':
        size = (node[1] + 7) // 8
        return size, max(1, size)
    if kind in C_FLOAT_TYPES:
        size = 16 if kind == 'x86_fp80' else (4 if kind == 'float' else 8)
        return size, size
    if kind == 'ptr':
        return 8, 8
    if kind == 'array':
        elem_size, align = _type_size_align(node[2], ctx)
        return elem_size * node[1], align
    if kind == 'named':
        body = ctx['typedefs'].get(node[1])
        if body is None or body == 'opaque':
            raise ScaffoldError('incomplete named type %r used by value'
                                % node[1])
        return _type_size_align(parse_type(body, 0)[0], ctx)
    if kind == 'agg':
        size = 0
        align = 1 if node[1] else 0
        for elem in node[2]:
            elem_size, elem_align = _type_size_align(elem, ctx)
            if not node[1]:
                align = max(align, elem_align)
                size = (size + elem_align - 1) // elem_align * elem_align
            size += elem_size
        if align:
            size = (size + align - 1) // align * align
        return size, align
    raise ScaffoldError('cannot size IR type %r' % (node, ))


def _c_signature(sig, ctx):
    """Map a parsed IR signature to ``(ret_c, ret_kind, [c_params])``."""
    params = list(sig.params)
    ret_node = sig.ret
    # a large struct return shows up as a leading sret pointer parameter;
    # the C function simply returns the struct by value
    if params and any(attr.startswith('sret:') for attr in params[0][1]):
        ret_node = params[0][0][1]
        params = params[1:]
        size, _align = _type_size_align(ret_node, ctx)
        if size <= 16:
            raise ScaffoldError(
                'small non-trivial C++ sret return cannot be expressed in C')
    if ret_node == ('void', ):
        ret_c, ret_kind = 'void', 'void'
    else:
        ret_kind = _ret_kind(ret_node, 'zeroext' in sig.ret_attrs)
        ret_c = _c_value_type(ret_node, ctx)
    c_params = []
    for index, (node, attrs) in enumerate(params):
        if 'inalloca' in attrs:
            raise ScaffoldError('inalloca parameter; write a C++ stub')
        if any(attr.startswith('byval:') for attr in attrs):
            node = node[1]  # pass the pointee struct by value
        c_type = _c_type_of(node, ctx, 'zeroext' in attrs)
        c_params.append('%s p%d' % (c_type, index))
    if sig.vararg:
        if not c_params:
            raise ScaffoldError('vararg function without named parameters')
        c_params.append('...')
    return ret_c, ret_kind, c_params


def c_function_name(name):
    """(C identifier, asm label or None) for an IR function name."""
    if C_IDENT_RE.match(name) and name not in C_KEYWORDS:
        return name, None
    # e.g. `foo.bar` or `foo$bar`: define under a valid identifier and
    # export the exact symbol with an asm label
    ident = 'ikos_stub_' + re.sub(r'\W', '_', name).strip('_')
    if not re.match(r'^[A-Za-z_]', ident):
        ident = 'ikos_stub_fn_' + ident
    label = name.replace('\\x01', '\\001')
    return ident, label


def _placeholder_lines(ret_kind, ret_c):
    if ret_kind == 'void':
        return []
    if ret_kind == 'uint':
        return ['    return __ikos_nondet_uint();']
    if ret_kind == 'int':
        return ['    return __ikos_nondet_int();']
    if ret_kind in ('float', 'ptr'):
        return ['    return 0;']
    return ['    %s ikos_stub_result = {0};' % ret_c,
            '    return ikos_stub_result;']


def render_stub(info, source_name, rule, typedefs):
    """Full C source of a stub skeleton for one external function."""
    sig = info.signature
    if sig is None:
        raise ScaffoldError('declaration line cannot be parsed')
    ctx = _new_context(typedefs)
    ret_c, ret_kind, c_params = _c_signature(sig, ctx)
    ident, asm_label = c_function_name(info.name)
    params_text = ', '.join(c_params) if c_params else 'void'
    lines = []
    lines.append('/* ikos-stub scaffold -- implement the body, then remove')
    lines.append(' * the %s line below.' % PLACEHOLDER_MARK)
    lines.append(' *')
    lines.append(' * ir-name: %s' % info.name)
    if source_name != info.name:
        lines.append(' * source-name: %s (rule: %s)' % (source_name, rule))
    lines.append(' * declare: %s' % sig.raw)
    lines.append(' * from: %s' % os.path.basename(info.file))
    lines.append(' *')
    lines.append(' * Semantics helpers (ikos/analyzer/intrinsic.h):')
    lines.append(' *   __ikos_assert(cond)  precondition to prove at call'
                 ' sites')
    lines.append(' *   __ikos_assume(cond)  postcondition to retain')
    lines.append(' *   __ikos_nondet_int() / __ikos_nondet_uint()'
                 ' nondeterministic value')
    lines.append(' *   __ikos_malloc(n)  fresh dynamic allocation (malloc model)')
    lines.append(' *   __ikos_check_mem_access(p, n)')
    lines.append(' *   __ikos_check_string_access(s)')
    lines.append(' *   __ikos_assume_mem_size(p, n)')
    lines.append(' *   __ikos_forget_mem(p, n)  arbitrary bytes, may hold'
                 ' pointers')
    lines.append(' *   __ikos_abstract_mem(p, n)  arbitrary bytes, no valid'
                 ' pointers')
    lines.append(' *   __ikos_assume_mem_value(p, n, lo, hi)')
    lines.append(' *   __ikos_assume_mem_rel(p1, n1, p2, n2, kind)')
    lines.append(' *   __ikos_watch_mem(p, n)  debugger watch only, no'
                 ' semantic havoc')
    lines.append(' */')
    lines.append('#include <stddef.h>')
    lines.append('#include <ikos/analyzer/intrinsic.h>')
    lines.append('')
    lines.extend(ctx['prelude'])
    if ctx['prelude']:
        lines.append('')
    if asm_label:
        lines.append('%s %s(%s) __asm__("%s");'
                     % (ret_c, ident, params_text, asm_label))
    lines.append('%s %s(%s) {' % (ret_c, ident, params_text))
    lines.append('    /* %s -- remove after implementing */'
                 % PLACEHOLDER_MARK)
    lines.extend(_placeholder_lines(ret_kind, ret_c))
    lines.append('}')
    return '\n'.join(lines) + '\n'


############################
# Compile, link, verify #
############################

def find_llvm_dis_or_die():
    llvm_dis = api_census.find_llvm_dis()
    if not llvm_dis:
        log.error('could not find llvm-dis (looked in: %s and PATH)'
                  % settings.LLVM_BIN_DIR)
        log.error('llvm-dis ships with the llvm installation ikos was'
                  ' configured against')
        sys.exit(1)
    return llvm_dis


def find_opt():
    """Return the llvm opt binary path, or None."""
    # same resolution order as api_census.find_llvm_dis
    path = os.path.join(settings.LLVM_BIN_DIR, 'opt')
    if os.path.isfile(path) and os.access(path, os.X_OK):
        return path
    return shutil.which('opt')


def compile_stub(src_path, bc_path, rename=None):
    """Compile one stub file to bitcode with the ikos clang flags."""
    define_flags = [rename] if rename else None
    try:
        analyzer.clang(bc_path,
                       src_path,
                       define_flags=define_flags,
                       disable_warnings=True,
                       colors=False)
    except subprocess.CalledProcessError:
        log.error('compilation failed: %s' % src_path)
        if rename:
            log.error('  (compiled with -D%s)' % rename)
        sys.exit(1)
    # promote allocas to SSA registers (2026-09-29): the ikos clang flags
    # compile -O0, so every local lives in a stack slot; __ikos_assume's
    # edge-predicate refinements then land on loaded registers only, and
    # on a non-relational domain (interval) a later reload of the slot
    # fetches the stale cell value -- a `nondet + assume + return` stub
    # body delivered TOP to callers. mem2reg keeps the branch structure
    # (the assume refinements) while removing the slot round-trip, so the
    # constraints reach the caller on every domain. the ikos clang flags
    # already pass -Xclang -disable-O0-optnone, so opt accepts the module.
    opt_bin = find_opt()
    if opt_bin is None:
        log.warning('could not find llvm opt; stub stays -O0 (assume'
                    ' constraints may be invisible to interval-domain'
                    ' analysis)')
        return
    tmp_path = bc_path + '.m2r.bc'
    try:
        subprocess.check_call([opt_bin, '-mem2reg', bc_path,
                               '-o', tmp_path])
        os.replace(tmp_path, bc_path)
    except subprocess.CalledProcessError:
        log.warning('opt -mem2reg failed on %s; stub stays -O0' % bc_path)
        if os.path.exists(tmp_path):
            os.remove(tmp_path)


# vector (<N x T>, <vscale x N x T>) and MMX (x86_mmx) llvm types: the
# analyzer's importer rejects them with `unsupported llvm type`, so a
# stub carrying them must never be linked into a program module
_VECTOR_TYPE_RE = re.compile(r'<\d+ x |<vscale x |x86_mmx')


def stub_has_vector_types(bc_path, llvm_dis):
    """True if the compiled stub module uses vector or MMX llvm types.

    Metadata lines are skipped: debug info could name such types without
    the module actually using them.
    """
    proc = subprocess.Popen([llvm_dis, bc_path, '-o', '-'],
                            stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL,
                            universal_newlines=True,
                            errors='replace')
    found = False
    for line in proc.stdout:
        if not line.startswith('!') and _VECTOR_TYPE_RE.search(line):
            found = True
            break
    proc.stdout.close()
    proc.wait()
    return found


def link_bitcodes(inputs, output):
    """llvm-link every module into one bitcode; False on failure."""
    cmd = [settings.llvm_link()] + inputs + ['-o', output]
    log.debug('running %s' % ' '.join(cmd))
    proc = subprocess.run(cmd,
                          stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE,
                          universal_newlines=True,
                          errors='replace')
    if proc.returncode != 0:
        log.error('llvm-link failed:')
        for line in proc.stderr.splitlines():
            log.error('  %s' % line)
        return False
    return True


def stub_defines(bc_paths, llvm_dis):
    """{name: [ExternInfo]} for every symbol the stub modules define."""
    api_census._init_worker(llvm_dis)
    defines = {}
    for path in bc_paths:
        _, path_defines, _ = scan_bitcode(path)
        for name, info in path_defines.items():
            defines.setdefault(name, []).append(info)
    return defines


def verify_signatures(plans, stub_defines, externals, bc_sources=None):
    """The signature gate; returns the list of offending names.

    llvm-link does NOT reject a declare/define signature mismatch: it
    silently rewrites call sites with a bitcast, so the stub body would
    receive wrongly typed arguments.  This is the only check.
    """
    # report stub .c sources, not their temp compilation outputs
    bc_sources = bc_sources or {}
    errors = []
    for name, infos in sorted(stub_defines.items()):
        if len(infos) > 1:
            log.error('%s: defined by %d stub modules:'
                      % (name, len(infos)))
            for info in infos:
                log.error('  %s' % bc_sources.get(info.file, info.file))
            errors.append(name)
    for plan in plans:
        infos = stub_defines.get(plan.name)
        if not infos:
            log.error('%s: the stub does not define this symbol' % plan.name)
            log.error('  %s' % plan.detail)
            errors.append(plan.name)
            continue
        extern = externals[plan.name]
        stub_sig = infos[0].signature
        if stub_sig is None or extern.signature is None:
            log.error('%s: signature cannot be parsed for comparison'
                      % plan.name)
            errors.append(plan.name)
            continue
        if (signature_key(stub_sig)
                != signature_key(extern.signature)):
            log.error('%s: stub signature does not match the program'
                      ' declaration' % plan.name)
            log.error('  program: %s' % extern.line)
            log.error('  stub:    %s' % stub_sig.raw)
            log.error('  stub file: %s' % plan.stub_file)
            log.error('  fix the stub signature, or rerun with'
                      ' --skip-mismatched to drop it from this link')
            errors.append(plan.name)
    return errors


def post_link_check(output, names, llvm_dis):
    """Every stubbed symbol must be defined in the linked output."""
    api_census._init_worker(llvm_dis)
    _, defines, _ = scan_bitcode(output)
    for name in names:
        if name not in defines:
            log.error('%s: not defined in the linked output' % name)
            return False
    return True


#############
# Output #
#############

DECISION_COLORS = {
    'missing': 'red',
    'stub-rename': 'yellow',
    'stub-manifest': 'yellow',
    'skip-interceptor': 'yellow',
    'skip-inalloca': 'yellow',
    'stub-exact': 'green',
}


def print_check_table(plans, opt):
    print('| ir name | source name | rule | decision | detail |')
    print('| --- | --- | --- | --- | --- |')
    counts = {}
    for plan in plans:
        counts[plan.decision] = counts.get(plan.decision, 0) + 1
        if plan.decision == 'builtin' and not opt.all:
            continue
        decision = plan.decision
        color = DECISION_COLORS.get(decision)
        if color:
            decision = colors.colorize(decision, color)
        source = plan.source_name if plan.source_name != plan.name else ''
        print('| %s | %s | %s | %s | %s |'
              % (plan.name, source, plan.rule or '', decision, plan.detail))
    stubbed = sum(counts.get(decision, 0) for decision in STUB_DECISIONS)
    skipped = sum(counts.get(decision, 0)
                  for decision in ('skip-interceptor', 'skip-inalloca'))
    print()
    print('| external | builtin | stubbed | missing | skipped |')
    print('| --- | --- | --- | --- | --- |')
    print('| %d | %d | %d | %d | %d |'
          % (len(plans), counts.get('builtin', 0), stubbed,
             counts.get('missing', 0), skipped))


def write_check_tsv(plans, path):
    with open(path, 'w') as stream:
        stream.write('ir_name\tsource_name\trule\tdecision\tdetail\n')
        for plan in plans:
            stream.write('%s\t%s\t%s\t%s\t%s\n'
                         % (plan.name,
                            plan.source_name,
                            plan.rule,
                            plan.decision,
                            plan.detail))


#######################
# Command handlers #
#######################

def _parse_forced(value):
    if not value:
        return frozenset()
    return frozenset(name.strip()
                     for name in value.split(',') if name.strip())


def _scan_and_plan(opt, known):
    """Shared front-end of the three sub commands."""
    llvm_dis = find_llvm_dis_or_die()
    log.debug('using llvm-dis: %s' % llvm_dis)
    files = collect_bitcode(opt.inputs)
    scan = scan_externals(files, llvm_dis, opt.jobs)
    if scan.opaque:
        log.warning('input bitcode uses opaque pointers (clang >= 15);'
                    ' stubs are compiled with typed pointers and llvm-link'
                    ' will refuse to merge them: recompile the program with'
                    ' the ikos toolchain')
    categories = api_census.category_of(known)
    reverse = api_census.reverse_source_names(scan.externals)
    index = load_stub_dir(opt.stub_dir) if opt.stub_dir else EMPTY_INDEX
    forced = _parse_forced(opt.force_stub)
    plans = plan_stubs(scan.externals, categories, reverse, index, forced)
    return scan, index, plans


def check_cmd(opt, known):
    scan, index, plans = _scan_and_plan(opt, known)
    print_check_table(plans, opt)
    if opt.tsv:
        write_check_tsv(plans, opt.tsv)
        log.info('written: %s' % opt.tsv)


def scaffold_cmd(opt, known):
    scan, index, plans = _scan_and_plan(opt, known)
    todo = [plan for plan in plans if plan.decision == 'missing']
    if not todo:
        log.info('nothing to scaffold')
        return
    os.makedirs(opt.output_dir, exist_ok=True)
    generated = []
    failed = []
    for plan in todo:
        info = scan.externals[plan.name]
        try:
            text = render_stub(info, plan.source_name, plan.rule,
                               scan.typedefs)
        except (ScaffoldError, SigError) as error:
            failed.append((plan.name, str(error)))
            continue
        path = os.path.join(opt.output_dir,
                            stub_key(plan.name) + STUB_SUFFIX)
        if os.path.exists(path) and not opt.force:
            log.warning('%s already exists; skipped (use --force)'
                        % os.path.basename(path))
            continue
        with open(path, 'w') as stream:
            stream.write(text)
        generated.append(os.path.basename(path))
    for name in generated:
        print(name)
    log.info('scaffolded %d stub(s) into %s' % (len(generated),
                                                opt.output_dir))
    if failed:
        log.warning('%d function(s) cannot be scaffolded; write them by'
                    ' hand:' % len(failed))
        for name, reason in failed:
            log.warning('  %s: %s' % (name, reason))
    log.info('next: implement the bodies, remove the %s lines,'
             ' then run:' % PLACEHOLDER_MARK)
    log.info('  ikos-stub link --stub-dir %s -o <out.bc> <bitcode>'
             % opt.output_dir)


def link_cmd(opt, known):
    if not opt.stub_dir:
        log.error('link: --stub-dir is required')
        sys.exit(1)
    llvm_dis = find_llvm_dis_or_die()
    scan, index, plans = _scan_and_plan(opt, known)
    selected = [plan for plan in plans if plan.decision in STUB_DECISIONS]

    # an unfilled scaffold skeleton must not pass as real semantics
    placeholders = [plan for plan in selected
                    if os.path.basename(plan.stub_file)[:-len(STUB_SUFFIX)]
                    in index.placeholders]
    if placeholders and not opt.allow_placeholder:
        for plan in placeholders:
            log.error('%s: still contains the %s marker'
                      % (plan.stub_file, PLACEHOLDER_MARK))
        log.error('implement the stub body first, or pass'
                  ' --allow-placeholder to link it anyway (UNSOUND)')
        sys.exit(1)
    if placeholders:
        log.warning('linking %d placeholder stub(s): UNSOUND semantics'
                    % len(placeholders))

    missing = [plan for plan in plans if plan.decision == 'missing']
    if missing:
        if opt.fail_missing:
            for plan in missing:
                log.error('%s: no stub' % plan.name)
            log.error('generate skeletons with: ikos-stub scaffold'
                      ' -o %s %s' % (opt.stub_dir, ' '.join(opt.inputs)))
            sys.exit(1)
        log.warning('%d external function(s) left unmodeled: %s'
                    % (len(missing), ', '.join(p.name for p in missing)))
    for plan in plans:
        if plan.decision.startswith('skip-'):
            log.warning('%s: %s (%s)'
                        % (plan.name, plan.decision, plan.detail))

    # one compilation unit per (file, macro): the same stub file may be
    # compiled several times, once per renamed target
    temp_dir = opt.temp_dir or tempfile.mkdtemp(prefix='ikos-stub-')
    os.makedirs(temp_dir, exist_ok=True)
    try:
        units = {}
        for plan in selected:
            units.setdefault((plan.stub_file, plan.rename_macro),
                             len(units))
        stub_bcs = []
        bc_sources = {}
        vector_units = set()
        for (src_path, rename), number in units.items():
            bc_path = os.path.join(temp_dir, 'stub-%d.bc' % number)
            compile_stub(src_path, bc_path, rename)
            stub_bcs.append(bc_path)
            bc_sources[bc_path] = src_path
            if (not opt.allow_vectors
                    and stub_has_vector_types(bc_path, llvm_dis)):
                vector_units.add((src_path, rename))
        # Stubs whose bitcode uses vector/MMX types: correctly generated
        # and modelable, but the analyzer cannot import those llvm types
        # (`unsupported llvm type`) -- one such stub dragged into a module
        # that never had them kills the whole bc's analysis.  They are
        # classified APART from `left unmodeled` (the stubs exist and are
        # valid; this is an analyzer syntax limitation, not a missing
        # model) and simply refused at link time.  --allow-vectors
        # restores linking them (for an analyzer with vector support).
        vector_dropped = []
        if vector_units:
            vector_dropped = [plan for plan in selected
                              if (plan.stub_file, plan.rename_macro)
                              in vector_units]
            selected = [plan for plan in selected
                        if (plan.stub_file, plan.rename_macro)
                        not in vector_units]
            keep = {(plan.stub_file, plan.rename_macro)
                    for plan in selected}
            stub_bcs = [os.path.join(temp_dir, 'stub-%d.bc' % number)
                        for unit, number in units.items() if unit in keep]
            log.warning('%d stub(s) not linked: vector/MMX types are'
                        ' not supported by the analyzer (the stubs are'
                        ' generated and valid): %s'
                        % (len(vector_dropped),
                           ', '.join(sorted(p.name for p in vector_dropped))))
        if not opt.no_verify:
            defines = stub_defines(stub_bcs, llvm_dis)
            errors = verify_signatures(selected, defines, scan.externals,
                                       bc_sources)
            if errors:
                if not opt.skip_mismatched:
                    sys.exit(1)
                # drop the offending stubs from THIS link only (the .c
                # files stay in the stub dir; bcs whose declaration
                # matches keep using them).  The functions stay external:
                # llvm-link would silently bitcast mismatched call sites,
                # so they must NOT be linked
                bad = set(errors)
                dropped = [plan for plan in selected if plan.name in bad]
                selected = [plan for plan in selected if plan.name not in bad]
                keep = {(plan.stub_file, plan.rename_macro)
                        for plan in selected}
                stub_bcs = [os.path.join(temp_dir, 'stub-%d.bc' % number)
                            for unit, number in units.items() if unit in keep]
                missing = missing + dropped
                log.warning('%d external function(s) left unmodeled: stub(s)'
                            ' dropped by the signature gate: %s'
                            % (len(dropped),
                               ', '.join(sorted(p.name for p in dropped))))
        if not link_bitcodes(collect_bitcode(opt.inputs) + stub_bcs,
                             opt.output):
            sys.exit(1)
        if (not opt.no_verify
                and not post_link_check(opt.output,
                                        [plan.name for plan in selected],
                                        llvm_dis)):
            sys.exit(1)
    finally:
        if not (opt.save_temps or opt.temp_dir):
            shutil.rmtree(temp_dir, ignore_errors=True)
        else:
            log.info('stub compilation directory: %s' % temp_dir)
    log.info('written: %s (%d stub(s), %d vector-type stub(s) skipped,'
             ' %d still unmodeled)'
             % (opt.output, len(selected), len(vector_dropped),
                len(missing)))


########################
# Command line parsing #
########################

def add_target_arguments(parser):
    parser.add_argument('inputs',
                        nargs='+',
                        metavar='<bitcode>',
                        help='Program LLVM bitcode files (.bc), or'
                             ' directories scanned for .bc files')
    parser.add_argument('--stub-dir',
                        dest='stub_dir',
                        metavar='<dir>',
                        help='Stub library directory: one .c file per'
                             ' function, named after the symbol it defines;'
                             ' an optional manifest.tsv maps a stub to'
                             ' extra mounted names')
    parser.add_argument('--names-from',
                        dest='names_from',
                        metavar='<file>',
                        help='Path to library_function.cpp'
                             ' (default: auto-detect)')
    parser.add_argument('--force-stub',
                        dest='force_stub',
                        metavar='<names>',
                        help='Comma-separated functions to stub even when'
                             ' modeled by the builtin table')
    parser.add_argument('-j',
                        '--jobs',
                        dest='jobs',
                        type=args.Integer(min=1),
                        default=min(16, os.cpu_count() or 4),
                        help='Number of parallel llvm-dis jobs')


def stub_parse_arguments(argv):
    usage = '%(prog)s [options] <command> ...'
    description = ('Assemble program bitcode with library function stubs:'
                   ' check what is covered, scaffold the missing ones,'
                   ' compile and link everything into one bitcode.')
    parser = argparse.ArgumentParser(usage=usage,
                                     description=description,
                                     formatter_class=argparse
                                     .RawTextHelpFormatter)
    parser.add_argument('--version',
                        action=args.VersionAction,
                        nargs=0,
                        help='Show ikos version')

    subparsers = parser.add_subparsers(dest='command', required=True)

    check_parser = subparsers.add_parser(
        'check',
        help='report the stub decision for every external function',
        description='For every external function of the input bitcode,'
                    ' report whether it is handled by the builtin table'
                    ' (skipped), covered by a stub in the library, or'
                    ' missing. A dry-run of link.')
    check_parser.set_defaults(cmd=check_cmd)
    add_target_arguments(check_parser)
    check_parser.add_argument('--tsv',
                              dest='tsv',
                              metavar='<file>',
                              help='Also write decisions as a TSV file')
    check_parser.add_argument('--all',
                              dest='all',
                              action='store_true',
                              help='Also list builtin-table functions')
    api_census.add_common_arguments(check_parser)

    scaffold_parser = subparsers.add_parser(
        'scaffold',
        help='generate stub skeletons for missing functions',
        description='Generate one signed stub skeleton per missing external'
                    ' function, with the C signature derived from the IR'
                    ' declare line. Implement the body with __ikos_*'
                    ' helpers and remove the placeholder line.')
    scaffold_parser.set_defaults(cmd=scaffold_cmd)
    add_target_arguments(scaffold_parser)
    scaffold_parser.add_argument('-o',
                                 '--output-dir',
                                 dest='output_dir',
                                 metavar='<dir>',
                                 default='.',
                                 help='Directory for generated stub files')
    scaffold_parser.add_argument('--force',
                                 dest='force',
                                 action='store_true',
                                 help='Overwrite existing stub files')
    api_census.add_common_arguments(scaffold_parser)

    link_parser = subparsers.add_parser(
        'link',
        help='compile stubs and link everything into one bitcode',
        description='Select stubs for the external functions, compile them'
                    ' with the ikos clang flags, llvm-link them with the'
                    ' program bitcode, and verify every stub signature'
                    ' against the program declaration (llvm-link itself'
                    ' does not check it: it silently bitcasts mismatched'
                    ' call sites).')
    link_parser.set_defaults(cmd=link_cmd)
    add_target_arguments(link_parser)
    link_parser.add_argument('-o',
                             '--output',
                             dest='output',
                             metavar='<file>',
                             default='stubbed.bc',
                             help='Output linked bitcode')
    link_parser.add_argument('--fail-missing',
                             dest='fail_missing',
                             action='store_true',
                             help='Fail if an external function has no stub'
                                  ' (default: warn and leave it external)')
    link_parser.add_argument('--allow-placeholder',
                             dest='allow_placeholder',
                             action='store_true',
                             help='Allow linking scaffold stubs whose'
                                  ' placeholder was not removed (UNSOUND)')
    link_parser.add_argument('--skip-mismatched',
                             dest='skip_mismatched',
                             action='store_true',
                             help='Drop stubs whose signature does not match'
                                  ' the program declaration instead of'
                                  ' failing the whole link: the function'
                                  ' stays external (counted unmodeled)')
    link_parser.add_argument('--allow-vectors',
                             dest='allow_vectors',
                             action='store_true',
                             help='Link stubs whose bitcode uses vector or'
                                  ' MMX types even though the analyzer'
                                  ' cannot import them (default: refuse'
                                  ' the stub -- it is generated and valid,'
                                  ' but the analyzer rejects these types)')
    link_parser.add_argument('--no-verify',
                             dest='no_verify',
                             action='store_true',
                             help='Skip signature verification (not'
                                  ' recommended)')
    link_parser.add_argument('--save-temps',
                             dest='save_temps',
                             action='store_true',
                             help='Keep the stub compilation directory')
    link_parser.add_argument('--temp-dir',
                             dest='temp_dir',
                             metavar='<dir>',
                             help='Use a fixed stub compilation directory')
    api_census.add_common_arguments(link_parser)

    opt = parser.parse_args(argv)

    # verbosity changes the log level, if --log is not specified
    if opt.log_level is None:
        if opt.verbosity <= 0:
            opt.log_level = 'error'
        elif opt.verbosity == 1:
            opt.log_level = 'info'
        elif opt.verbosity == 2:
            opt.log_level = 'debug'
        else:
            opt.log_level = 'all'

    return opt


def stub_main(argv):
    # parse arguments
    opt = stub_parse_arguments(argv[1:])

    # setup colors and logging
    colors.setup(opt.color, file=log.out)
    log.setup(opt.log_level)

    # the builtin name table decides builtin vs missing; fail early rather
    # than misreport every function
    known, path = api_census.resolve_known_apis(opt.names_from)
    log.debug('builtin name table: %s' % path)
    opt.cmd(opt, known)


if __name__ == '__main__':
    stub_main(sys.argv)
