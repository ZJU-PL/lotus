#!/usr/bin/env python3
###############################################################################
#
# IKOS API spec -- differential validation of generated stubs
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
# ANY KIND, EITHER EXPRESSED, IMPLIED, OR STATORY, INCLUDING, BUT NOT
# LIMITED TO, ANY WARRANTY THAT THE SUBJECT SOFTWARE WILL CONFORM TO
# SPECIFICATIONS, ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
# PARTICULAR PURPOSE, OR NONINFRINGEMENT.  IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.
#
###############################################################################
"""Differential validation gate for the stubs ikos-api-spec generates.

The compile and signature gates check mechanical correctness only.  This
module checks *semantic over-approximation*, per input, by executing the
behavior twice:

* the real build links the driver against the real library;
* the stub build links the same driver against the stub plus a recorder
  runtime that turns every ``__ikos_*`` call into an observation.

The judge mirrors IKOS's actual abstract domains (the grounding is the
domain stack of ``analyzer/src/analysis/value/abstract_domain.cpp``):

* integers  -- MachineInt domain.  The backend is user-selectable (-d):
               the interval family joins to the convex hull, the
               congruence family (-d congruence, interval-congruence, ...)
               joins to the congruence class x = r0 (mod d).  Admission is
               both: the real value must fall inside the hull AND the
               congruence closure of the achievable stub returns (verified:
               join({0},{2}) proves x != 1 under congruence but not under
               interval).  ``--domain`` (the token of the analysis run's
               -d flag) narrows admission to the families that backend
               actually tracks, via admission_mode().  The relational
               backends (dbm, apron octagon / polyhedra / linear
               equalities -- see relation_class()) narrow two-variable
               __ikos_assume predicates, so a claim relating the return
               to the parameters is judged input by input and stays
               sound under every backend; a body whose assume prunes the
               whole battery gets an argv-seeded retry (the input's own
               tokens injected into the nondet slots) before the
               'unverified' fallback, so a wrong relation (r == p0
               against a real |p0|) becomes a measurable 'return too
               narrow' rejection instead of slipping through.  The int32
               nondet vocabulary cannot express a 64-bit parameter
               exactly; there the 'unverified' fallback stands;
* pointers  -- the four-component abstraction (uninitialized, nullity,
               points-to set, offset interval): admission is per component,
               with the fresh-block gap made explicit when the real function
               returns a library-allocated pointer the stub vocabulary
               cannot express.  The gap is not a blank cheque: a stub may
               never return its own stack storage (dead for the caller,
               caught via the recorder's FRAME landmark), may not alias a
               caller INPUT buffer the real return does not also alias
               (strchr/getcwd-style intra-argument returns and strcpy-style
               output-buffer returns stay legal), and a uniformly
               sub-64KB pointer set is flagged as a pointer/int confusion
               suspect (note only);
* memory    -- per-block cell abstraction: every byte the real function
               writes must be havoced by the stub (``forget_mem`` when the
               written bytes may hold a valid pointer, ``abstract_mem``
               otherwise), mirroring the ``_pointer_sets`` semantics;
* floats    -- IKOS abstracts floats by initialization status only, so a
               float return value can never make a stub unsound and the
               check is vacuous (with a note).

A stub judged unsound produces a counterexample text that the caller feeds
into the standard repair loop.
"""
import hashlib
import json
import math
import os
import os.path
import re
import resource
import shutil
import struct
import subprocess
import tempfile
import tempfile
import time
import warnings

# the real-library link map ships next to this script
import reallibs
# the analyzer-real judge battery ships next to this script
import ikosjudge

try:
    # the pipeline's own IR-name -> C-identifier mapping (ikos.stub); the
    # fallback mirrors it for standalone uses where ikos is not importable
    from ikos.stub import c_function_name as _c_function_name
except ImportError:
    def _c_function_name(name):
        if re.match(r'^[A-Za-z_][A-Za-z0-9_]*$', name):
            return name, None
        ident = 'ikos_stub_' + re.sub(r'\W', '_', name).strip('_')
        if not re.match(r'^[A-Za-z_]', ident):
            ident = 'ikos_stub_fn_' + ident
        return ident, name

# bump to invalidate cached driver/inputs responses when these prompts change
DT_PROMPT_VERSION = '7-cxx-sret-driver'

DT_RUNTIME_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              'difftest')

# execution containment for LLM-written drivers: they are compiled AND run
# locally.  run_once adds PID/network/mount isolation with bubblewrap, in
# addition to these resource limits.
DT_RLIMIT_AS = 512 * 1024 * 1024
DT_RLIMIT_FSIZE = 1024 * 1024
DT_RLIMIT_NOFILE = 32
DT_RLIMIT_NPROC = 2048

DT_BOUNDARY_VALUES = (0, 1, -1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 63, 64,
                      255, 256, 65535, 65536,
                      -2147483648, 2147483647)

# edge tokens appended to the model's inputs (substituted into its own argv
# shapes, so the arity always matches what the driver expects)
DT_EDGE_TOKENS = ('0', '-1', '', 'NULL')

DT_DEFAULTS = {'timeout': 5, 'schedules': 48, 'inputs': 6,
               'driver_retries': 3, 'link_retries': 2,
               'ikos_cpu': 10, 'ikos_wall': 60,
               # scenario drivers: bare first, escalate to a setup-call
               # driver only when the real side yields no usable input;
               # the library tier (stub build links the real library) is
               # experimental and off by default
               'scenario': True, 'scenario_lib': False,
               # private in-sandbox D-Bus session for dbus_/g_bus_/sd_bus_
               # targets (bluez cluster): dbus-run-session wraps every
               # run of a matching function, ~50ms per run
               'sandbox_dbus': True}

# the numeric backends ikos -d accepts (analyzer/args.py), plus 'all' =
# the generator's default portable target (sound under every backend)
DOMAINS = ('all',
           'interval', 'congruence', 'interval-congruence',
           'dbm', 'var-pack-dbm', 'var-pack-dbm-congruence',
           'gauge', 'gauge-interval-congruence',
           'apron-interval', 'apron-octagon', 'apron-polka-polyhedra',
           'apron-polka-linear-equalities', 'apron-ppl-polyhedra',
           'apron-ppl-linear-congruences',
           'apron-pkgrid-polyhedra-lin-cong',
           'var-pack-apron-octagon', 'var-pack-apron-polka-polyhedra',
           'var-pack-apron-polka-linear-equalities',
           'var-pack-apron-ppl-polyhedra',
           'var-pack-apron-ppl-linear-congruences',
           'var-pack-apron-pkgrid-polyhedra-lin-cong')


def admission_mode(domain):
    """Which join families the judge consults under a --domain selection.

    'all' (the default) demands both families, so an admitted stub is
    sound under every -d backend.  A concrete backend relaxes the families
    it does not track: the pure congruence domain joins constants to their
    congruence class only ('closure'), the congruence-bearing products
    (interval-congruence, ...-lin-cong, ...) track both ('dual'), and
    everything else joins by convex hull ('hull' -- relational backends
    refine the hull further, but a stub's claims are interval/enumeration
    shaped, so hull admission bounds what they assert).  Trading the
    untracked family away is exactly the precision-for-portability deal
    --domain offers: the {-1,1} enumeration is rejected under 'all' and
    admitted under 'interval'; a hull miss is rejected under 'all' and
    admitted under 'congruence'.
    """
    if domain == 'all':
        return 'dual'
    tracks_congruence = 'congruence' in domain or 'lin-cong' in domain
    tracks_interval = domain != 'congruence'
    if tracks_congruence and tracks_interval:
        return 'dual'
    return 'closure' if tracks_congruence else 'hull'


ADMISSION_MODES = ('hull', 'closure', 'dual')


def as_mode(mode):
    """Normalize an admission mode; anything unknown judges under the
    strictest one (both families).  A typo'd or foreign string must never
    quietly require NO family -- that would admit everything.
    """
    return mode if mode in ADMISSION_MODES else 'dual'


# the relational tier: backends built on core::machine_int::
# NumericDomainAdapter over a relational core::numeric domain (dbm.cpp,
# apron_*.cpp), where a two-variable __ikos_assume narrows the invariant
# instead of degrading to the interval projection.  The tier guides the
# writing hints and the game's per-input metric only; admission stays
# admission_mode() (hull, or dual for the congruence-bearing products).
RELATION_CLASSES = {
    'dbm': 'difference',
    'var-pack-dbm': 'difference',
    'var-pack-dbm-congruence': 'difference',
    'apron-octagon': 'difference',
    'var-pack-apron-octagon': 'difference',
    'apron-polka-polyhedra': 'polyhedra',
    'apron-ppl-polyhedra': 'polyhedra',
    'apron-pkgrid-polyhedra-lin-cong': 'polyhedra',
    'var-pack-apron-polka-polyhedra': 'polyhedra',
    'var-pack-apron-ppl-polyhedra': 'polyhedra',
    'var-pack-apron-pkgrid-polyhedra-lin-cong': 'polyhedra',
    'apron-polka-linear-equalities': 'equalities',
    'var-pack-apron-polka-linear-equalities': 'equalities',
    'apron-ppl-linear-congruences': 'equalities',
    'var-pack-apron-ppl-linear-congruences': 'equalities',
}


def relation_class(domain):
    """-> 'difference' | 'polyhedra' | 'equalities' | None.

    None = non-relational (interval boxes, pure congruence, gauge --
    gauge relates variables to loop counters only).  Membership mirrors
    the adapters in analyzer/src/analysis/value/machine_int_domain/: dbm
    and the apron octagon track difference constraints r - p <= c;
    polka / ppl polyhedra (and pkgrid's polyhedra half) track linear
    combinations incl. equalities; the linear-equalities family (and
    ppl linear congruences, weaker: an equality is kept as a congruence
    relation) refines equalities only.
    """
    return RELATION_CLASSES.get(domain)


def _dt(opt, name):
    return getattr(opt, 'dt_' + name, DT_DEFAULTS[name])


#############################
# Driver / inputs prompting #
#############################

DRIVER_SYSTEM_PROMPT = """You write small C driver programs that exercise exactly one library \
function for differential testing of static-analysis stubs.  The driver is \
compiled twice -- once against the real library, once against a stub -- and \
must make the function's observable behavior measurable.

Mandatory contract (write every contract line with the __dt_* helpers from \
"dt.h", never with printf, so a function that prints cannot corrupt it):

  #include "dt.h"

  int main(int argc, char** argv) {
      <set up arguments from argv[1], argv[2], ...>
      __dt_begin(0);
      __dt_buf("b0", buf, sizeof buf);       /* buffer whose contents you */
                                             /* initialized yourself      */
      __dt_buf_out("b1", out, sizeof out);   /* pure output buffer, left  */
                                             /* uninitialized             */
      __dt_call();
      <type> r = <function>(<arguments>);    /* exactly ONE call          */
      __dt_ret(&r, sizeof r);                /* __dt_ret_void(); if void  */
      __dt_end();
      return 0;
  }

Rules:
  - Read the test input from argv[1..]: each token is one value.  Define the \
mapping yourself (numbers with strtod/strtol, buffer contents from a string \
token, ...).  The token "NULL" means "pass a null pointer" for a pointer \
argument; skip registration then.
  - Register EVERY buffer you pass a pointer to, with "b0", "b1", ... in \
order.  Registration is the bounds oracle: without it nothing about that \
buffer can be judged.
  - Do not branch on the result in a way that could skip contract lines; the \
contract must stay straight-line.  Use the return value only via __dt_ret.
  - If the function needs a FILE* stream, set it up from a file in the \
current working directory (fopen/freopen); never read stdin implicitly.
  - Hard constraints: C99; #include only from {dt.h, stdio.h, stdlib.h, \
string.h, math.h, limits.h, stdint.h, stddef.h, ctype.h, stdbool.h, \
float.h}; call only these functions besides <function> and the __dt_* \
helpers: atoi atol strtol strtoul strtod malloc calloc free memset memcpy \
memmove strlen strcmp strncmp snprintf isdigit isalpha tolower toupper \
fopen fclose fread fwrite fseek ftell freopen fmemopen.  No errno, no \
__asm__, no #pragma, no __attribute__, no volatile, no printf, no rand, no \
time/clock, no threads.
  - Declare the prototype of <function> yourself, with EXACTLY the given \
signature types.

Output: the complete .c file, nothing else -- no markdown fences, no \
explanations."""


def driver_user_prompt(plan, parts):
    lines = ['Write the C driver for this function.', '']
    lines.append('source name: %s (call it dt_target in the driver -- the'
                 ' build renames it back; a local prototype named'
                 ' dt_target cannot clash with system headers)'
                 % plan.source_name)
    lines.append('C signature (types are fixed; declare it as dt_target):')
    lines.append('  %s' % parts.signature_line.replace(
        plan.source_name, 'dt_target', 1))
    if parts.typedef_lines:
        lines.append('')
        lines.append('typedefs (copy them verbatim if you need the types):')
        lines += ['  %s' % line for line in parts.typedef_lines]
    lines.append('')
    lines.append('The function is exercised with diverse argv token lists;'
                 ' define a sensible token mapping.')
    return '\n'.join(lines)


def is_cxx_function(plan):
    """Whether a mangled symbol needs a C++ real-side driver.

    Runtime helpers such as ``__cxa_atexit`` intentionally stay C: their
    source-level ABI is a C ABI even when they serve C++ programs.
    """
    return plan.name.startswith('_Z')


CXX_DRIVER_SYSTEM_PROMPT = """You write a small C++17 driver that exercises exactly one mangled C++ function for differential testing of static-analysis stubs. The driver is compiled twice: once against the real library and once against a C-compatible wrapper around the stub.

Mandatory contract:
  #include "dt.h"

  int main(int argc, char** argv) {
      <construct valid C++ values from argv[1], argv[2], ...>
      __dt_begin(0);
      __dt_buf("obj", &object, sizeof object);
      __dt_call();
      <type> r = dt_target(<arguments>);
      __dt_ret(&r, sizeof r);
      __dt_end();
      return 0;
  }

The wrapper visible to this driver is extern "C". Infer the source-level signature from the demangled name and declare an ABI-compatible prototype, for example:
  extern "C" bool dt_target(const std::string*);
  extern "C" std::string dt_target(const std::string&);

LLVM sret rule: when the generated stub-side scaffold returns a struct/union
by value, do NOT declare that C++ return type. The wrapper exposes the
hidden ABI result pointer explicitly. Declare void and put that pointer first:
  extern "C" void dt_target(std::string* result, const std::string& object,
                            std::size_t pos, std::size_t count);
Default-construct `result` before __dt_begin, register it with
__dt_buf("result", &result, sizeof result), keep it alive, call dt_target
without using its return value, and pass &result to __dt_ret afterwards.

Hard rules:
  - Map every scalar function parameter to its own argv token (argv[1], argv[2], ...); never hardcode all scalar arguments.
  - Convert unsigned integer tokens with strtoull and clamp or reject values outside the real function's documented preconditions.
  - Use real C++ types and construct real objects. Never memset, alias raw bytes as a C++ object, or call a method on a fabricated object.
  - Prefer default initialization and initializer syntax such as `std::string s = argv[1] ? argv[1] : "";`.
  - Register storage of every C++ object passed by pointer or reference with __dt_buf, not __dt_buf_out; canary-filling a live non-trivial object corrupts it.
  - Keep every object alive until after __dt_end() so destructors run only after the contract is complete.
  - Include only the standard C++ headers needed for those values, plus dt.h and the allowed C headers.
  - Besides dt_target and __dt_* helpers, call only the whitelisted C helpers and benign standard-container methods such as size, data, c_str, insert, push_back, begin, and end.
  - Call dt_target exactly once, in result position, between __dt_call() and __dt_ret or __dt_ret_void.
  - If the signature needs a project-defined object whose header is unavailable, do not fake its representation. Use NULL only when that is a genuine function precondition; otherwise let the tool reject the driver.
  - Do not use printf, exception control flow, threads, time, random, errno, __asm__, #pragma, __attribute__, or volatile.

Output one complete C++ .cc file and nothing else: no markdown fences and no explanations."""


def cxx_driver_user_prompt(plan, parts):
    lines = ['Write the C++17 differential driver for this mangled function.', '']
    lines.append('mangled IR name: %s' % plan.name)
    lines.append('demangled source signature: %s' % plan.source_name)
    lines.append('Declare and call the wrapper as dt_target.')
    lines.append('')
    lines.append('Generated stub-side C ABI scaffold:')
    lines.append('  %s' % parts.signature_line)
    ret_type, _name = split_signature(parts.signature_line)
    if ret_type and ('struct' in ret_type or 'union' in ret_type) \
            and '*' not in ret_type:
        lines.append('')
        lines.append('This scaffold uses an LLVM sret return. Declare'
                     ' dt_target as void with `result` as its first'
                     ' parameter, as described by the system contract.')
    if parts.typedef_lines:
        lines.append('C ABI typedefs, shown for ABI understanding; use real'
                     ' C++ source types in the driver:')
        lines += ['  %s' % line for line in parts.typedef_lines]
    lines.append('')
    lines.append('Construct only values for which the real implementation is'
                 ' defined. Diverse argv inputs will be supplied later.')
    return '\n'.join(lines)


INPUTS_SYSTEM_PROMPT = ('You propose test inputs for a C driver program.'
                        '  Reply with a JSON array of objects'
                        ' {"argv": ["token", ...], "stdin": ""}, each a'
                        ' full argument list for the driver (argv[1..]).'
                        '  Cover: normal values, boundary values, and'
                        ' inputs that make the function fail (bad paths,'
                        ' missing data).  Plain tokens, no quoting, no'
                        ' newlines inside tokens.  Reply with the JSON'
                        ' array only.')


def inputs_user_prompt(plan, count, driver_src):
    return ('The function under test is %s.\n\n'
            'The driver (how tokens map to arguments is visible in its'
            ' main):\n\n%s\n\n'
            'Propose %d diverse inputs.'
            % (plan.source_name, driver_src, count))


def dt_rejection_prompt(reason):
    return ('Your previous driver was rejected by the toolchain:\n\n'
            '%s\n\n'
            'Return a corrected complete driver file. Same rules as before.'
            % reason)


def dt_cache_key(kind, opt, plan, info):
    digest = hashlib.sha256()
    for part in (DT_PROMPT_VERSION, kind, opt.model or '', plan.name,
                 info.line):
        digest.update(part.encode('utf-8'))
        digest.update(b'\0')
    return digest.hexdigest()


#############################
# Driver safety guard #
#############################

COMMENT_RE = re.compile(r'/\*.*?\*/|//[^\n]*', re.S)
CALL_RE = re.compile(r'([A-Za-z_]\w*)\s*\(')
C_KEYWORDS = frozenset(('if', 'else', 'for', 'while', 'switch', 'case',
                        'default', 'do', 'goto', 'return', 'sizeof'))

DT_ALLOWED_INCLUDES = frozenset((
    'dt.h', 'stdio.h', 'stdlib.h', 'string.h', 'math.h', 'limits.h',
    'stdint.h', 'stddef.h', 'ctype.h', 'stdbool.h', 'float.h',
    'cctype', 'cfloat', 'climits', 'cmath', 'cstddef', 'cstdint',
    'cstdio', 'cstdlib', 'cstring',
    'algorithm', 'exception', 'functional', 'ios', 'iosfwd', 'istream',
    'iterator', 'limits', 'list', 'locale', 'map', 'memory', 'new',
    'ostream', 'queue', 'set', 'sstream', 'stack', 'stdexcept',
    'streambuf', 'string', 'string_view', 'tuple', 'typeinfo',
    'unordered_map', 'unordered_set', 'utility', 'vector',
))

DT_ALLOWED_CALLS = frozenset((
    '__dt_begin', '__dt_buf', '__dt_buf_out', '__dt_call', '__dt_ret',
    '__dt_ret_void', '__dt_end',
    'main',
    'atoi', 'atol', 'strtol', 'strtoul', 'strtoull', 'strtod',
    'malloc', 'calloc', 'free',
    'memset', 'memcpy', 'memmove',
    'strlen', 'strcmp', 'strncmp', 'snprintf',
    'isdigit', 'isalpha', 'tolower', 'toupper',
    'fopen', 'fclose', 'fread', 'fwrite', 'fseek', 'ftell',
    'freopen', 'fmemopen',
    # C++ drivers need just enough standard-container vocabulary to
    # construct valid objects. Execution remains sandboxed and rlimited.
    'at', 'back', 'begin', 'c_str', 'clear', 'crbegin', 'crend', 'data',
    'emplace', 'emplace_back', 'empty', 'end', 'erase', 'find', 'front',
    'get', 'insert', 'pop_back', 'push_back', 'rbegin', 'release',
    'rend', 'reserve', 'resize', 'size', 'substr', 'swap',
))

# Tier-1 scenario constructors: libc state builders a scenario driver may
# call BEFORE the single target call (a bare driver that crashes the real
# side for lack of state -- regexec without regcomp, fdopen without open --
# is the battery's top failure class).  The scenario prompt renders the
# purpose column; driver_guard shares the same table, so the two can never
# drift apart (the test suite pins the synchronization).  All of these
# live in glibc (pthread merged in 2.34), so the scenario tier needs no
# link-line change.
DT_SETUP_CONSTRUCTORS = {
    'open': 'obtain a valid fd on a file in the working directory',
    'close': 'release an fd produced by the setup',
    'dup': 'duplicate an fd',
    'fcntl': 'query or adjust fd status flags',
    'mkstemp': 'create a uniquely-named temporary file and its fd',
    'tmpfile': 'an anonymous temporary FILE*',
    'fdopen': 'wrap an existing fd in a FILE*',
    'opendir': 'open a DIR* on a directory in the working directory',
    'regcomp': 'compile a regex_t before the regex APIs',
    'regfree': 'release a compiled regex_t',
    'socket': 'create an unconnected socket fd',
    'pipe': 'create a connected fd pair',
    'pthread_mutex_init': 'initialize a pthread_mutex_t object',
    'malloc': 'heap storage for the target arguments',
    'calloc': 'zeroed heap storage',
    'free': 'release heap storage',
    'fopen': 'a FILE* on a file in the working directory',
}

# extra headers the scenario constructors need (scenario mode only)
DT_SCENARIO_INCLUDES = frozenset((
    'fcntl.h', 'unistd.h', 'dirent.h', 'regex.h', 'pthread.h',
    'sys/stat.h', 'sys/types.h', 'sys/socket.h',
))


def scenario_addendum(lib_functions=()):
    """The scenario-mode section appended to the driver system prompt.

    Renders DT_SETUP_CONSTRUCTORS itself (the guard's own table, so the
    prompt and the guard cannot drift apart) plus, for the library tier,
    the target library's own functions the setup may also call.
    """
    lines = ['',
             'Scenario mode: the target needs caller-built state a bare'
             ' call cannot construct (a compiled regex_t, a valid fd, an'
             ' initialized library object).  You may call up to 4'
             ' functions from this setup whitelist BEFORE __dt_call() to'
             ' build that state:']
    lines += ['  %s -- %s' % (name, purpose)
              for name, purpose in sorted(DT_SETUP_CONSTRUCTORS.items())]
    if lib_functions:
        lines += ['',
                  'You may ALSO call these functions of the target library'
                  ' itself during setup (declare each prototype yourself,'
                  ' with the source-level types):',
                  '  ' + ', '.join(lib_functions)]
    lines += ['',
              'Setup rules:',
              '  - Run the setup between the argv argument mapping and'
              ' __dt_begin(); the contract sequence itself stays'
              ' straight-line and unchanged.',
              '  - Call dt_target exactly once, AFTER all setup.  Never'
              ' call dt_target -- nor a sibling from its family (regexec'
              ' when the target is regcomp, fclose when it is fopen, ...)'
              ' -- during setup.',
              '  - Register every setup-produced object you pass to the'
              ' target and whose storage you own (regex_t, heap blocks,'
              ' structs) with __dt_buf/__dt_buf_out before __dt_call;'
              ' handles you do not own (fd, FILE*, DIR*) need no'
              ' registration.',
              '  - Scenario mode also allows these headers: %s.'
              % ' '.join(sorted(DT_SCENARIO_INCLUDES)),
              '  - Create any file the setup opens inside the current'
              ' working directory first (fopen/fwrite), then open() it.']
    return '\n'.join(lines)

DT_TOKEN_BAN_RE = re.compile(
    r'__asm__|\berrno\b|#pragma|__attribute__|\bvolatile\b|__IKOS')


def driver_guard(src, target, scenario=False, extra_setup=()):
    """Why this driver must be rejected before it is compiled and run.

    target is the dt_target alias: the driver declares and calls
    dt_target everywhere; the build -D-renames it to the real function
    (real side) or the stub (stub side), so a local prototype can never
    clash with a system header.  scenario=True admits the Tier-1 setup
    constructors (DT_SETUP_CONSTRUCTORS, with their headers) and any
    Tier-2 library setup names in extra_setup; the target itself must
    still be called exactly once, in result position.
    """
    if 'int main' not in src:
        return 'the driver must define int main'
    includes = DT_ALLOWED_INCLUDES
    calls = DT_ALLOWED_CALLS
    if scenario:
        includes = includes | DT_SCENARIO_INCLUDES
        calls = (calls | frozenset(DT_SETUP_CONSTRUCTORS)
                 | frozenset(extra_setup))
    for match in re.finditer(r'#\s*include\s*[<"]([^>"]+)[>"]', src):
        if match.group(1) not in includes:
            return ('#include <%s> is not on the allowlist (%s)'
                    % (match.group(1), ' '.join(sorted(includes))))
    code = COMMENT_RE.sub(' ', src)
    if DT_TOKEN_BAN_RE.search(code):
        return ('the driver must not use errno, __asm__, #pragma,'
                ' __attribute__, volatile or __IKOS identifiers')
    found = CALL_RE.findall(code)
    for name in found:
        if name not in calls and name not in C_KEYWORDS \
                and name != target:
            return ('the driver may only call the whitelisted helpers and'
                    ' %s (found a call to %s)' % (target, name))
    if scenario:
        # the setup builds state, it is not a second test program
        setup_calls = sum(1 for name in found
                          if name in DT_SETUP_CONSTRUCTORS
                          or name in extra_setup)
        if setup_calls > 6:
            return ('scenario setup is limited to a few constructor calls'
                    ' (found %d); keep the state building minimal'
                    % setup_calls)
    # the call must appear exactly once, in result position: "r = f(...)"
    # (a void return has no result position; a single bare call is fine;
    # the local prototype "void dt_target(...);" is stripped first)
    code_wo_proto = re.sub(
        r'^[ \t]*(?:extern\s+"C"\s+)?[A-Za-z_][^;{}=]*?\b%s'
        r'[ \t]*\([^;{}]*\);[ \t]*$' % re.escape(target),
        '', code, flags=re.MULTILINE)
    calls = re.findall(r'(?:=\s*|\breturn\s+)%s\s*\(' % re.escape(target),
                       code_wo_proto)
    bare = re.findall(r'(?<![\w.>=!-])%s\s*\(' % re.escape(target),
                      code_wo_proto)
    if len(calls) != 1 and len(bare) != 1:
        return ('the driver must call %s exactly once, in result position'
                ' (e.g. "<type> r = %s(args);"; a void return may be a'
                ' single bare call) between __dt_call() and __dt_ret;'
                ' found %d' % (target, target, len(bare)))
    return None


#############################
# Contract / log parsing #
#############################

def _strip_fences(text):
    lines = text.split('\n')
    if lines and lines[0].lstrip().startswith('```'):
        lines = lines[1:]
    if lines and lines[-1].strip() == '```':
        lines = lines[:-1]
    return '\n'.join(lines)


def parse_contract(text):
    """DT1 lines -> observation dict."""
    obs = {'begin': False, 'bufregs': [], 'call': False,
           'ret': None, 'ret_raw': None, 'wrote': {}, 'wrote_unknown': [],
           'data': {}, 'ok': False, 'bufinout': {}}
    for line in text.split('\n'):
        fields = line.split()
        if len(fields) < 2 or fields[0] != 'DT1':
            continue
        kind = fields[1]
        try:
            if kind == 'begin':
                obs['begin'] = True
            elif kind == 'bufreg' and len(fields) >= 5:
                obs['bufregs'].append((fields[2], int(fields[3], 16),
                                       int(fields[4])))
                # fourth column: __dt_buf = in, __dt_buf_out = out (older
                # runtimes emit three columns; those read as input buffers)
                obs['bufinout'][fields[2]] = (fields[5]
                                              if len(fields) >= 6
                                              and fields[5] in ('in', 'out')
                                              else 'in')
            elif kind == 'call':
                obs['call'] = True
            elif kind == 'ret' and len(fields) >= 3:
                obs['ret_raw'] = fields[2]
                if fields[2] == 'void':
                    obs['ret'] = 'void'
                elif fields[2] == 'null':
                    obs['ret'] = None
                else:
                    obs['ret'] = bytes.fromhex(fields[2][:32])
            elif kind == 'wrote' and len(fields) >= 4:
                obs['wrote'][fields[2]] = _parse_ranges(fields[3])
                if fields[3] == '?':
                    # three-state: snapshot failed (driver shape bug), which
                    # is neither "nothing changed" nor measurable writes
                    obs['wrote_unknown'].append(fields[2])
            elif kind == 'data' and len(fields) >= 4:
                obs['data'][fields[2]] = bytes.fromhex(fields[3])
            elif kind == 'ok':
                obs['ok'] = True
        except ValueError:
            continue
    return obs


def _parse_ranges(text):
    offsets = set()
    if text in ('-', '?'):
        return offsets
    for part in text.split(','):
        try:
            if '-' in part:
                lo, hi = part.split('-', 1)
                offsets.update(range(int(lo), int(hi) + 1))
            else:
                offsets.add(int(part))
        except ValueError:
            continue
    return offsets


def parse_dtlog(text):
    """recorder log -> list of ('EVENT', payload...) tuples."""
    events = []
    for line in text.split('\n'):
        fields = line.split()
        if not fields:
            continue
        kind, rest = fields[0], fields[1:]
        try:
            if kind == 'NONDET':
                events.append(('NONDET', int(rest[0])))
            elif kind == 'FRAME':
                events.append(('FRAME', int(rest[0], 16)))
            elif kind == 'MALLOC':
                events.append(('MALLOC', int(rest[0], 16), int(rest[1])))
            elif kind in ('ASSERT', 'ASSUME'):
                events.append((kind, rest[0]))
            elif kind in ('CHECK_MEM', 'ASSUME_SIZE', 'WATCH',
                          'HAVOC_FORGET', 'HAVOC_ABSTRACT'):
                if rest[0] == 'NULL':
                    events.append((kind, 'NULL', 0, 0))
                elif rest[0] == 'UNREG':
                    events.append((kind, 'UNREG', 0, int(rest[2])))
                else:
                    events.append((kind, rest[0], int(rest[1]),
                                   int(rest[2])))
            elif kind == 'CHECK_STR':
                if rest[0] == 'NULL':
                    events.append((kind, 'NULL', 0, 'NULL'))
                elif rest[0] == 'UNREG':
                    events.append((kind, 'UNREG', 0, 'UNREG'))
                else:
                    events.append((kind, rest[0], int(rest[1]), rest[2]))
            elif kind == 'ASSUME_VAL':
                if rest[0] == 'NULL':
                    events.append((kind, 'NULL', 0, 0, int(rest[1]),
                                   int(rest[2])))
                elif rest[0] == 'UNREG':
                    events.append((kind, 'UNREG', 0, int(rest[2]),
                                   int(rest[3]), int(rest[4])))
                else:
                    events.append((kind, rest[0], int(rest[1]), int(rest[2]),
                                   int(rest[3]), int(rest[4])))
        except (IndexError, ValueError):
            continue
    return events


#############################
# Signature typing #
#############################

SIG_RE = re.compile(r'^\s*(.+?)\s*\b([A-Za-z_]\w*)\s*\((.*)\)\s*$', re.S)


def split_signature(signature_line):
    match = SIG_RE.match(signature_line)
    if not match:
        return None, None
    return match.group(1).strip(), match.group(2)


def classify_return(ret_type):
    """Return kind used by the differential judge."""
    text = ret_type.replace(' ', '')
    if '*' in text:
        return 'pointer'
    if text == 'void':
        return 'void'
    if 'struct' in text or 'union' in text or '{' in text:
        return 'aggregate'
    if text in ('float', 'double', 'longdouble'):
        return 'float'
    width = 4
    if 'longlong' in text:
        width = 8
    elif 'long' in text or 'size_t' in text or 'ssize_t' in text \
            or 'intptr_t' in text or 'uintptr_t' in text or 'ptrdiff_t' in text:
        width = 8
    elif 'short' in text:
        width = 2
    elif 'char' in text:
        width = 1
    return ('int', width)


def trunc_int(raw, width):
    if not raw:
        return None
    return int.from_bytes(raw[:width], 'little', signed=True)


def classify_pointer(raw, bufregs):
    """-> ('NULL',) | ('BUF', tag, off) | ('OTHER', addr)."""
    if not raw or raw == 'void' or not any(raw):
        return ('NULL',)
    addr = int.from_bytes(raw[:8], 'little')
    if addr == 0:
        return ('NULL',)
    for tag, base, length in bufregs:
        if base <= addr < base + length:
            return ('BUF', tag, addr - base)
    return ('OTHER', addr)


def classify_claimed_pointer(raw, bufregs, events=()):
    """Classify a stub pointer, resolving fresh dynamic allocations.

    IKOS identifies malloc results by allocation site, not by their unstable
    concrete addresses.  The native recorder logs those addresses for one run;
    a returned address matching one of them is the concrete witness for that
    abstract DynAllocMemoryLocation.
    """
    cls = classify_pointer(raw, bufregs)
    if cls[0] != 'OTHER':
        return cls
    return next(
        (('FRESH', event[2]) for event in events
         if event[0] == 'MALLOC' and event[1] == cls[1]),
        cls,
    )


def is_malloc_like(plan, parts):
    """Whether a target is a single-size allocator returning fresh storage."""
    name = (getattr(plan, 'source_name', None)
            or getattr(plan, 'name', '') or '')
    name = name.split('(', 1)[0].strip().lower()
    ret = (getattr(parts, 'signature_line', '') or '').split('(', 1)[0]
    return name.endswith('malloc') and '*' in ret


def _class_text(cls):
    """short rendering of a classify_pointer result for verdict texts."""
    if cls[0] == 'BUF':
        return 'buffer %s' % cls[1]
    if cls[0] == 'OTHER':
        return 'OTHER 0x%x' % cls[1]
    return 'NULL'


#############################
# Running #
#############################

def _exec_prefix():
    """['setarch', <arch>, '-R'] when available, else [].

    Kernels with high-entropy ASLR (vm.mmap_rnd_bits >= 28) crash the
    clang-14 sanitizer runtime at startup, nondeterministically; disabling
    address randomization for the child fixes it and is harmless (even
    beneficial: stable addresses) for the unsanitized stub build.
    """
    exe = shutil.which('setarch')
    if exe:
        return [exe, os.uname().machine, '-R']
    return []


EXEC_PREFIX = _exec_prefix()


def _sandbox_prefix(workdir):
    """Prefix for an isolated native execution, or None when unavailable."""
    bwrap = shutil.which('bwrap')
    if not bwrap:
        return None
    prefix = []
    # Ubuntu 24.04 restricts unprivileged user namespaces by AppArmor label.
    # Processes started by the user manager can be labelled "unconfined" and
    # denied userns even when an interactive application profile is allowed.
    # DT_AA_PROFILE opts into an existing local profile for bwrap only.
    aa_profile = os.environ.get('DT_AA_PROFILE')
    if aa_profile:
        aa_exec = shutil.which('aa-exec')
        if not aa_exec:
            return None
        prefix.extend([aa_exec, '--profile', aa_profile, '--'])
    return prefix + [bwrap,
            '--unshare-all',
            '--unshare-user',
            '--die-with-parent',
            '--new-session',
            '--ro-bind', '/', '/',
            '--proc', '/proc',
            '--dev', '/dev',
            '--bind', os.path.abspath(workdir), os.path.abspath(workdir)]


def _nproc_prefix():
    """Apply RLIMIT_NPROC inside the PID namespace.

    Setting it in preexec can prevent Bubblewrap from creating namespaces at
    all: RLIMIT_NPROC counts the caller's existing desktop threads, which may
    already exceed the sandbox limit.  Once bwrap has entered its private PID
    namespace, prlimit can safely cap the target process's own forks.
    """
    prlimit = shutil.which('prlimit')
    if not prlimit:
        return []
    return [prlimit, '--nproc=%d' % DT_RLIMIT_NPROC, '--']


# targets that need a live D-Bus session bus inside the sandbox (the bluez
# dbus_* cluster: bwrap's IPC/network isolation makes dbus_bus_get fail,
# which is the cluster's whole "no usable real input" failure mode)
DBUS_TARGET_PREFIXES = ('dbus_', 'g_bus_', 'sd_bus_')


def _needs_dbus(plan):
    """Whether the target needs a private D-Bus session bus to run at all.

    V1 is a name-prefix test on the IR and source names.  A hit pays one
    dbus-run-session wrapper per run (~50ms); everything else is exactly
    as before.
    """
    names = (getattr(plan, 'name', '') or '',
             getattr(plan, 'source_name', '') or '')
    return any(name.startswith(DBUS_TARGET_PREFIXES) for name in names)


_DBUS_WARNED = False

# minimal private session-bus config: the socket lives under the
# (writable) workdir -- the sandbox /tmp is read-only and some daemon
# builds ignore TMPDIR when picking the run-session address.  The policy
# is allow-all: the bus is disposable and unreachable from outside the
# sandbox.
_DBUS_SESSION_CONF = '''<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:path=%s</listen>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
'''


def _dbus_session_wrap(workdir):
    """-> (cmd_prefix, env) for a private in-sandbox D-Bus session.

    dbus-run-session starts a private daemon, points
    DBUS_SESSION_BUS_ADDRESS at it and cleans up on exit; daemon and
    driver share the same bwrap instance, so the socket never leaves the
    sandbox.  XDG_RUNTIME_DIR is a fresh 0700 dir under the (writable)
    workdir for the daemon's runtime files.  (None, {}) when
    dbus-run-session is unavailable: warn once per process and run as
    before -- never fail-closed.
    """
    global _DBUS_WARNED
    exe = shutil.which('dbus-run-session')
    if not exe:
        if not _DBUS_WARNED:
            _DBUS_WARNED = True
            warnings.warn('dbus-run-session not found; dbus-target runs'
                          ' fall back to the plain sandbox (no session'
                          ' bus)', RuntimeWarning)
        return None, {}
    xdg = os.path.join(os.path.abspath(workdir), 'xdg')
    os.makedirs(xdg, mode=0o700, exist_ok=True)
    os.chmod(xdg, 0o700)
    conf = os.path.join(xdg, 'session.conf')
    try:
        with open(conf, 'w') as stream:
            stream.write(_DBUS_SESSION_CONF % os.path.join(xdg, 'bus'))
    except OSError:
        return None, {}
    # TMPDIR too: builds that do honor it then keep every runtime file
    # off the read-only /tmp
    return ([exe, '--config-file', conf, '--'],
            {'XDG_RUNTIME_DIR': xdg, 'TMPDIR': xdg})


def run_once(binpath, argv, dt_env, timeout, workdir, limit_as=None,
             dbus=False):
    contract_r, contract_w = os.pipe()

    def preexec():
        os.dup2(contract_w, 4)
        os.close(contract_r)
        if contract_w != 4:
            # when the pipe already landed on fd 4, dup2 above was a no-op
            # and closing it would cut the contract channel itself
            os.close(contract_w)
        resource.setrlimit(resource.RLIMIT_CPU, (timeout + 1, timeout + 1))
        resource.setrlimit(resource.RLIMIT_FSIZE,
                           (DT_RLIMIT_FSIZE, DT_RLIMIT_FSIZE))
        resource.setrlimit(resource.RLIMIT_NOFILE,
                           (DT_RLIMIT_NOFILE, DT_RLIMIT_NOFILE))
        if limit_as:
            # NOTE: never applied to ASan builds, the sanitizer shadow
            # mapping needs terabytes of address space
            resource.setrlimit(resource.RLIMIT_AS, (limit_as, limit_as))

    env = {'PATH': '/usr/bin:/bin',
           # detect_stack_use_after_return=0 keeps ASan stack locals on the
           # real stack: on the fake stack a "return &local" address sits
           # GBs away from the recorder's FRAME landmark and the judge's
           # stack-return check goes blind
           'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1'
                           ':detect_stack_use_after_return=0',
           'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=0'}
    env.update(dt_env)
    session = []
    if dbus:
        # private D-Bus session bus inside the sandbox: daemon and driver
        # share the bwrap IPC namespace, the socket never leaves it
        session, session_env = _dbus_session_wrap(workdir)
        env.update(session_env)
        if session is None:
            session = []
    # an LLM-proposed argv token can carry an embedded NUL: exec cannot
    # express it and Popen would raise ValueError all the way out of the
    # battery, killing the whole invocation -- strip it, the same way the
    # caller truncates excess length
    argv = [str(tok).replace('\x00', '') for tok in argv]
    try:
        sandbox = _sandbox_prefix(workdir)
        if sandbox is None:
            os.close(contract_r)
            os.close(contract_w)
            return {'rc': None, 'contract': '',
                    'stderr': 'Bubblewrap is required for native differential'
                              ' execution',
                    'timeout': False, 'log': ''}
        proc = subprocess.Popen(sandbox + _nproc_prefix() + session
                                + EXEC_PREFIX + [binpath] + list(argv),
                                preexec_fn=preexec,
                                stdout=subprocess.DEVNULL,
                                stderr=subprocess.PIPE,
                                stdin=subprocess.DEVNULL,
                                cwd=workdir,
                                env=env,
                                pass_fds=(contract_w,))
    except (ValueError, OSError) as error:
        os.close(contract_r)
        return {'rc': None, 'contract': '', 'stderr': str(error),
                'timeout': False, 'log': ''}
    os.close(contract_w)
    try:
        _out, err = proc.communicate(timeout=timeout)
        timed_out = False
    except subprocess.TimeoutExpired:
        proc.kill()
        _out, err = proc.communicate()
        timed_out = True
    chunks = []
    try:
        while True:
            chunk = os.read(contract_r, 65536)
            if not chunk:
                break
            chunks.append(chunk)
    finally:
        os.close(contract_r)
    return {'rc': proc.returncode,
            'contract': b''.join(chunks).decode('utf-8', 'replace'),
            # the tail covers the ASan/UBSan summary; the scenario-driver
            # escalation feeds a digest of it back into the repair loop
            'stderr': (err or b'').decode('utf-8', 'replace')[-2000:],
            'timeout': timed_out, 'log': ''}


def run_status(run):
    """'ok' | 'crash-in-target' | 'crash-before-call' | 'timeout'."""
    obs = parse_contract(run['contract'])
    if obs['ok']:
        return 'ok'
    if run['timeout'] and not obs['call']:
        return 'timeout-before-call'
    if obs['call']:
        return 'crash-in-target'
    return 'crash-before-call'


def _probe_error(run):
    """A concise, diagnostic error for a probe that never reached the target."""
    detail = 'rc=%s' % run['rc']
    # a setup crash lands here: the scenario ladder feeds this text back
    # to the model as the evidence it repairs against
    detail += _stderr_digest(run)
    if run['contract']:
        detail += ', contract=%r' % run['contract'][-120:]
    return 'driver failed before target call on probe input (%s)' % detail


#############################
# Schedules #
#############################

def y_values(ret_kind, y_raw):
    """forced-value injections derived from the real return."""
    if ret_kind == 'pointer':
        return [0, 1, -1]
    if not y_raw:
        # no observed return bytes on this input (real side crashed or timed
        # out before returning): no derivation possible, use the default
        # injections -- same convention as the unparseable-bytes branch below
        return [0, 1]
    if ret_kind == 'float':
        try:
            bits = int.from_bytes(y_raw[:8], 'little', signed=False)
            number = struct.unpack('<d', struct.pack('<Q', bits))[0]
            return [int(number // 1), int(-((-number) // 1))]
        except (ValueError, struct.error):
            return [0, 1]
    if isinstance(ret_kind, tuple):
        value = trunc_int(y_raw, ret_kind[1])
        if value is not None:
            return [value, value - 1, value + 1]
    return [0, 1]


def build_schedules(yvals, nondet_count, limit):
    count = max(nondet_count, 1)
    schedules = []
    seen = set()

    def add(values):
        key = tuple(values)
        if key not in seen:
            seen.add(key)
            schedules.append(list(values))

    for pos in range(1, min(4, count + 1) + 1):
        for value in yvals:
            sched = [0] * (count + 2)
            sched[pos - 1] = value
            add(sched)
    for value in DT_BOUNDARY_VALUES:
        sched = [0] * (count + 2)
        sched[0] = value
        add(sched)
    add([0] * (count + 2))
    add([-1] * (count + 2))
    state = 0x12345
    for _ in range(16):
        state = (state * 6364136223846793005
                 + 1442695040888963407) & ((1 << 63) - 1)
        sched = []
        value = state
        for _ in range(count + 2):
            value = (value * 6364136223846793005
                     + 1442695040888963407) & ((1 << 31) - 1)
            sched.append(value - (1 << 30))
        add(sched)
    return schedules[:limit]


#############################
# The judge #
#############################

def alive_runs(stub_runs):
    """The schedule runs the analyzer would not prune: every run whose
    recorder logged a violated __ikos_assume is dead by the analyzer's
    pruning semantics."""
    alive = []
    for run in stub_runs:
        if run_status(run) == 'timeout-before-call':
            continue
        events = parse_dtlog(run['log'])
        if any(kind == 'ASSUME' and outcome == 'VIOLATED'
               for kind, outcome, *_rest in events):
            continue  # the analyzer prunes this path
        alive.append((parse_contract(run['contract']), events))
    return alive


def judge_input(real_runs, stub_runs, ret_kind, mode='dual', ikos=None,
                allocator=False):
    """-> (verdict, reason, notes, detail) for one input.

    detail carries the per-input abstractions the co-evolution game needs:
    the real observation ('real', the canary-unioned contract parse, set
    where the verdict made it meaningless) and the alive stub claims
    ('alive', (obs, events) pairs of the schedules the analyzer would not
    prune), plus the analyzer-real judge summary ('ikos') when the ikos
    battery produced usable evidence for this input.  mode is the
    admission_mode() of the run's --domain selection ('dual' keeps the
    portable both-families admission; an unknown string is normalized by
    as_mode() to the strictest).
    """
    mode = as_mode(mode)
    notes = []
    detail = {'real': None, 'alive': []}
    rA, rB = real_runs
    statusA, statusB = run_status(rA), run_status(rB)
    if statusA == 'crash-in-target' or statusB == 'crash-in-target':
        return ('n/a', 'real function crashed or hung on this input; it is'
                ' outside the real precondition and carries no verdict'
                ' weight', notes, detail)
    if statusA != 'ok' or statusB != 'ok':
        return ('n/a', 'driver misbehaved before the tested call (crash,'
                ' hang or malformed contract)', notes, detail)

    obsA = parse_contract(rA['contract'])
    obsB = parse_contract(rB['contract'])
    if obsA['ret_raw'] != obsB['ret_raw']:
        return ('unverified', 'the real function returns different values'
                ' across identical runs (flaky)', notes, detail)
    # write sets are UNIONED across the two canary runs: a byte the function
    # wrote with a value equal to one canary is invisible to that run and
    # caught by the other -- differing write sets are the canary mechanism
    # working, not nondeterminism
    for tag, offsets in obsB['wrote'].items():
        obsA['wrote'][tag] = obsA['wrote'].get(tag, set()) | offsets

    alive = alive_runs(stub_runs)

    detail['real'] = obsA
    detail['alive'] = alive
    if not alive:
        return ('unverified', 'every schedule was pruned by __ikos_assume:'
                ' the stub may cut ALL caller paths (contradictory'
                ' postconditions); needs manual review', notes, detail)

    reason = _phase_a(alive, notes)
    if reason:
        return 'unsound', reason, notes, detail
    reason = _phase_b(obsA, alive, notes,
                      aggregate_result=(ret_kind == 'aggregate'))
    if reason:
        return 'unsound', reason, notes, detail
    verdict, why = None, None
    if ikos is not None:
        verdict, why = _ikos_phase(ikos, obsA, ret_kind, notes)
        if verdict is not None:
            return verdict, why, notes, detail
        if _ikos_return_definitive(ikos) \
                and isinstance(ret_kind, tuple) and ret_kind[0] == 'int':
            # the exclusion assertion was REFUTED: the stub definitely
            # can return the real value -- the simulated hull of _phase_c
            # has nothing left to add for the return axis
            return ('sound', 'ikos (-d %s) shows the stub can produce the'
                    ' real return value on this input' % ikos['domain'],
                    notes, detail)
    if verdict is None:
        verdict, why = _phase_c(obsA, alive, ret_kind, notes, mode,
                                allocator=allocator)
    return verdict, why, notes, detail


def _ikos_return_definitive(ikos):
    """True when the ret exclusion assertion was refuted (status 2).

    kind 3 status 2 on the ret probe means `r != y` is definitely false
    on the surviving paths: the analyzer proved r == y is reachable --
    stronger than any hull admission.
    """
    probes = {p['line']: p for p in ikos.get('probes', ())
              if p.get('kind') == 'ret'}
    for line, status in (ikos.get('probe_status') or {}).items():
        if int(line) in probes and status == 2:
            return True
    return False


def _ikos_phase(ik, real_obs, ret_kind, notes):
    """Analyzer-real admission for one input.

    -> (verdict, reason) with verdict 'unsound'/'unverified', or
    (None, None) when this input carries no definitive ikos evidence and
    the simulation phases stay in charge.  Every rejection quotes the
    domain that proved it, making the verdict reproducible by hand.
    """
    witness = ik.get('witness') or {}
    if not witness.get('ok'):
        notes.append('ikos evidence dropped: %s'
                     % (witness.get('reason')
                        or 'the witness run did not reproduce the input'))
        return None, None
    # witness faithfulness: the pinned input must reproduce the real
    # observation before any proved exclusion is trusted.  Integers
    # compare numerically; pointers compare by BLOCK identity (the two
    # processes lay the buffers out at different addresses, the buffer
    # tag+offset is the invariant)
    ret = real_obs.get('ret')
    if isinstance(ret, (bytes, bytearray)) and len(ret) >= 8:
        expected = int.from_bytes(ret[:8], 'little', signed=True)
        if ret_kind == 'pointer':
            real_block = classify_pointer(ret, real_obs.get('bufregs')
                                          or ())
            wit_block = (witness.get('blocks') or {}).get('r')
            if wit_block is not None and real_block[0] in ('NULL', 'BUF') \
                    and wit_block != real_block:
                notes.append('ikos evidence dropped: witness return block'
                             ' %s != real block %s (pinning mismatch)'
                             % (wit_block, real_block))
                return None, None
        elif witness.get('ret') != expected:
            notes.append('ikos evidence dropped: witness returned %s but'
                         ' the real observation is %s (input pinning'
                         ' mismatch)' % (witness.get('ret'), expected))
            return None, None
    domain = ik.get('domain')
    probes = {p['line']: p for p in ik.get('probes', ())}
    for line, status in (ik.get('probe_status') or {}).items():
        if status != 0:  # only PROVED exclusions reject
            continue
        probe = probes.get(int(line))
        if probe is None:
            continue
        if probe['kind'] == 'ret':
            return ('unsound', 'ikos (-d %s) proves the stub EXCLUDES the'
                    ' real return value %d on this input'
                    % (domain, probe['value']))
        return ('unsound', 'ikos (-d %s) proves the stub EXCLUDES the real'
                ' byte value %d at %s:%d (over-narrow pointee claim)'
                % (domain, probe['value'], probe['tag'], probe['off']))
    for row in ik.get('stub_asserts') or ():
        if row.get('kind') == 3 and row.get('status') == 2:
            return ('unsound', 'precondition too strong (analyzer'
                    ' semantics): ikos refutes the stub\'s own'
                    ' __ikos_assert at stub line %d on an input the real'
                    ' function accepts' % row['line'])
    if ik.get('all_unreachable'):
        return ('unverified', 'ikos (-d %s) prunes every path after the'
                ' call: the stub may cut ALL caller paths, or the pinned'
                ' input contradicts a stub postcondition' % domain)

    # pointer admission: the real concrete target must fall inside the
    # analyzer's points-to / offset / nullity declaration
    blocks = witness.get('blocks') or {}
    for name in ['r'] + sorted(blocks):
        if name not in blocks or blocks[name][0] == 'OTHER':
            continue  # fresh-block gap: the old classify_pointer path
            # judges OTHER with its own approximation note
        declared = ik.get('values', {}).get(name)
        if not isinstance(declared, dict):
            continue
        real_block = blocks[name]
        nullity = declared.get('nullity')
        if real_block[0] == 'NULL':
            if nullity == 'non-null':
                return ('unsound', 'ikos (-d %s) proves %s is non-null'
                        ' but the real run returns/observes NULL'
                        % (domain, name))
            continue
        # BUF target: the claimed set must name the buffer and the
        # offset interval must contain the real offset
        wanted = ikosjudge.buffer_var(real_block[1])
        claimed = declared.get('points_to') or []
        if claimed and wanted not in claimed \
                and not any(e.startswith('dyn_alloc') for e in claimed):
            return ('unsound', 'ikos (-d %s) proves %s can only point to'
                    ' {%s} but the real run observes it in buffer %s'
                    ' (wrong block, analyzer points-to)'
                    % (domain, name, ','.join(claimed), real_block[1]))
        offset = declared.get('offset')
        if offset and len(real_block) >= 3:
            lo, hi = offset
            lo = lo if isinstance(lo, int) else -(1 << 62)
            hi = hi if isinstance(hi, int) else (1 << 62)
            if not (lo <= real_block[2] <= hi):
                return ('unsound', 'ikos (-d %s) constrains the offset of'
                        ' %s to [%s, %s] but the real run observes offset'
                        ' %d' % (domain, name, lo, hi, real_block[2]))
    return None, None


def _phase_a(alive, notes):
    """preconditions must hold on real-consistent arguments."""
    for obs, events in alive:
        lengths = {tag: length for tag, _b, length in obs['bufregs']}
        for event in events:
            kind = event[0]
            if kind == 'ASSERT' and event[1] == 'FAIL':
                return ('precondition too strong: __ikos_assert failed on'
                        ' arguments the real function accepts')
            if kind in ('CHECK_MEM', 'CHECK_STR') and event[1] == 'NULL':
                return ('precondition too strong: %s rejects a null pointer'
                        ' the real function accepts' % kind)
            if kind == 'CHECK_MEM' and event[1] not in ('NULL', 'UNREG'):
                _kind, tag, off, size = event
                if off + size > lengths.get(tag, off + size):
                    return ('precondition too strong: check_mem_access'
                            ' claims [%s+%d, +%d) which exceeds the buffer'
                            ' the caller actually provides'
                            % (tag, off, size))
            if kind == 'CHECK_MEM' and event[1] == 'UNREG':
                notes.append('check_mem_access on an unregistered pointer:'
                             ' bounds unchecked')
            if kind == 'CHECK_STR' and event[3] == 'NOTERM':
                return ('precondition too strong: check_string_access'
                        ' requires a terminator the real string does not'
                        ' have inside the registered buffer')
    outcome_sets = set()
    for _obs, events in alive:
        outcome_sets.add(tuple(outcome
                               for kind, outcome, *_rest in events
                               if kind == 'ASSERT'))
    if len(outcome_sets) > 1:
        return ('precondition depends on a nondeterministic value:'
                ' __ikos_assert both passes and fails across schedules,'
                ' which turns into spurious analyzer warnings')
    return None


def _phase_b(real_obs, alive, notes, aggregate_result=False):
    """real writes must be covered by stub havoc claims."""
    havoc = {}
    abstract_only = {}
    aggregate_havoc = False
    result_lengths = [length for name, _base, length
                      in real_obs['bufregs'] if name == 'result']
    result_length = result_lengths[0] if result_lengths else None
    for _obs, events in alive:
        for event in events:
            if event[0] not in ('HAVOC_FORGET', 'HAVOC_ABSTRACT',
                                'ASSUME_VAL'):
                continue
            _kind, tag, off, size = event[:4]
            if aggregate_result and event[0] in ('HAVOC_FORGET',
                                                 'HAVOC_ABSTRACT'):
                if tag == 'result' and result_length is not None:
                    aggregate_havoc = (
                        aggregate_havoc
                        or off <= 0 and off + size >= result_length)
                elif (tag == 'UNREG' and result_length is not None
                      and size >= result_length):
                    aggregate_havoc = True
            if tag in ('NULL', 'UNREG'):
                continue
            havoc.setdefault(tag, set()).update(range(off, off + size))
            if event[0] == 'HAVOC_ABSTRACT':
                abstract_only.setdefault(tag, set()).update(
                    range(off, off + size))
    if aggregate_result and result_length is not None and not aggregate_havoc:
        return ('aggregate result is not over-approximated: the stub must'
                ' havoc the full %d-byte result object' % result_length)
    for tag, offsets in real_obs['wrote'].items():
        missing = offsets - havoc.get(tag, set())
        if missing and aggregate_result and tag == 'result':
            if aggregate_havoc:
                notes.append('aggregate result covered by the hidden-sret'
                             " stub's full-object unregistered havoc")
                missing = set()
        if missing:
            return ('missed write side effect: the real function writes'
                    ' %s at %s but the stub havocs nothing there'
                    % (tag, _format_offsets(sorted(missing))))
    # pointer-valued writes need forget_mem, not abstract_mem: a load from
    # abstracted memory yields no valid pointers in the analyzer
    for tag, offsets in _pointer_write_offsets(real_obs).items():
        bad = offsets & (abstract_only.get(tag, set())
                         - _havoc_offsets(alive, tag, 'HAVOC_FORGET'))
        if bad:
            return ('pointer write approximated with __ikos_abstract_mem:'
                    ' the real function stores a valid pointer at %s %s;'
                    ' legitimate dereferences of that pointer would raise'
                    ' null-dereference alarms'
                    % (tag, _format_offsets(sorted(bad))))
    if havoc:
        extra = sorted(set(havoc) - set(real_obs['wrote']))
        if extra:
            notes.append('stub havocs beyond the real write set: %s'
                         ' (precision cost, sound)' % ','.join(extra))
    # __ikos_assume_mem_value claims: the real written byte values must fall
    # in the claimed [lo,hi]; a byte outside it is missed (unsound).
    for _obs, events in alive:
        for event in events:
            if event[0] != 'ASSUME_VAL':
                continue
            _kind, tag, off, size, lo, hi = event
            if tag in ('NULL', 'UNREG'):
                continue
            data = real_obs['data'].get(tag, b'')
            for o in sorted(real_obs['wrote'].get(tag, ())):
                if off <= o < off + size and o < len(data):
                    v = data[o]
                    if not (lo <= v <= hi):
                        return ('assume_mem_value claims [%d,%d] but the real'
                                ' function writes byte value %d at %s:%d'
                                % (lo, hi, v, tag, o))
    # __ikos_assume_mem_size claims: an under-estimate of the real buffer
    # size is unsound (it would hide overflow beyond the claimed size);
    # an over-estimate on a pure OUTPUT buffer is the same lie in the
    # other direction (the stub claims storage past the caller's object).
    buf_len = {tag: length for tag, _b, length in real_obs['bufregs']}
    inout = real_obs.get('bufinout') or {}
    for _obs, events in alive:
        for event in events:
            if event[0] != 'ASSUME_SIZE':
                continue
            _kind, tag, off, size = event
            if tag in ('NULL', 'UNREG'):
                continue
            real_len = buf_len.get(tag)
            if real_len is not None and size < real_len:
                return ('assume_mem_size claims %d bytes but the real buffer'
                        ' is %d bytes (under-estimate is unsound)'
                        % (size, real_len))
            if real_len is not None and size > real_len \
                    and inout.get(tag) == 'out':
                return ('unsound: assume_mem_size claims %d bytes on'
                        ' output buffer %s but the caller provides %d'
                        ' (over-estimate hides overflow past the object)'
                        % (size, tag, real_len))
    # havoc claims must stay inside the registered buffer: a forget or
    # abstract range running past the caller's object writes past it in
    # the analyzer too (stat64's forget_mem(p1, 256) on a 144-byte
    # struct stat).  UNREG claims cannot be bounded by the registration
    # oracle; they are only noted.
    unreg_havoc = False
    for _obs, events in alive:
        for event in events:
            if event[0] not in ('HAVOC_FORGET', 'HAVOC_ABSTRACT'):
                continue
            _kind, tag, off, size = event[:4]
            if tag == 'NULL':
                continue
            if tag == 'UNREG':
                unreg_havoc = True
                continue
            real_len = buf_len.get(tag)
            if real_len is not None and off + size > real_len:
                return ('unsound: havoc exceeds output buffer %s (%d > %d)'
                        % (tag, off + size, real_len))
    if unreg_havoc and not aggregate_result:
        # (the hidden-sret aggregate shape covers its result object with a
        # sanctioned unregistered full-object havoc -- expected, not news)
        notes.append('havoc on an unregistered pointer: bounds unchecked')
    return None


def _havoc_offsets(alive, tag, kind):
    offsets = set()
    for _obs, events in alive:
        for event in events:
            if event[0] == kind and event[1] == tag:
                offsets.update(range(event[2], event[2] + event[3]))
    return offsets


def _pointer_write_offsets(real_obs):
    """offsets of 8-byte windows whose written value points into a
    registered buffer (or equals the returned pointer)."""
    result = {}
    bases = [(base, length) for _tag, base, length in real_obs['bufregs']]
    ret_ptr = None
    if real_obs['ret'] and real_obs['ret'] != 'void' \
            and any(real_obs['ret']):
        ret_ptr = int.from_bytes(real_obs['ret'][:8], 'little')
    for tag, data in real_obs['data'].items():
        offsets = real_obs['wrote'].get(tag, set())
        hits = set()
        for start in range(0, max(len(data) - 7, 0)):
            if not all(o in offsets for o in range(start, start + 8)):
                continue
            value = int.from_bytes(data[start:start + 8], 'little')
            if ret_ptr is not None and value == ret_ptr:
                hits.add(start)
            elif any(base <= value < base + length
                     for base, length in bases):
                hits.add(start)
        if hits:
            result[tag] = hits
    return result


def _phase_c(real_obs, alive, ret_kind, notes, mode='dual',
             allocator=False):
    """return admission; -> (verdict, reason).

    mode is the admission_mode() of the run's --domain selection: 'dual'
    (the default) requires the real value inside the hull AND the
    congruence closure, 'hull' only inside the hull, 'closure' only inside
    the congruence class (an unknown string is normalized by as_mode() to
    the strictest).
    """
    mode = as_mode(mode)
    if ret_kind == 'void':
        return 'sound', ''
    if ret_kind == 'float':
        notes.append('float return: ikos abstracts floats by initialization'
                     ' status only, so the value cannot make the stub'
                     ' unsound (check vacuous)')
        return 'sound', ''
    if ret_kind == 'aggregate':
        # Aggregate values are admitted through the registered result
        # object, not a scalar interval.  _phase_b has already required
        # every real result write to lie under a havoc claim.
        notes.append('aggregate return: admitted through result-object'
                     ' write coverage')
        return 'sound*', 'result-object write coverage'
    y_raw = real_obs['ret']
    if y_raw in (None, 'void') or len(y_raw) > 8:
        return 'unverified', 'return value not measurable as a scalar'

    if ret_kind == 'pointer':
        y_class = classify_pointer(y_raw, real_obs['bufregs'])
        if allocator and y_class[0] == 'OTHER':
            y_class = ('FRESH', None)
        classes = [classify_claimed_pointer(obs['ret'], obs['bufregs'],
                                            events)
                   for obs, events in alive]
        inout = real_obs.get('bufinout') or {}
        # stack return: a pointer into the target's own frame is dead
        # storage for the caller (g_malloc's "return &p0").  Judged per
        # run against that run's FRAME landmark.  Exempt an OTHER real
        # return at a nearby address: both sides then point into the same
        # caller-side stack region (argv strings and the like), which is
        # the fresh-block gap, not a dead local.
        for (_obs, events), cls in zip(alive, classes):
            if cls[0] != 'OTHER':
                continue
            frame = next((e[1] for e in events if e[0] == 'FRAME'), None)
            if frame is None or abs(cls[1] - frame) >= (1 << 20):
                continue
            if y_class[0] == 'OTHER' \
                    and abs(cls[1] - y_class[1]) < (1 << 20):
                continue
            return ('unsound', 'unsound: return points into the target'
                    ' stack frame (0x%x within 1MB of frame 0x%x): dead'
                    ' storage the caller must not receive'
                    % (cls[1], frame))
        # input-buffer aliasing: returning the caller's INPUT storage is
        # only a legal approximation when the real function does so too
        # (strchr/getcwd return inside their argument); against a fresh,
        # NULL or different-block real return it hands the caller's own
        # initialized buffer back as the result (fopen64 returning the
        # path argument, getservbyname returning the "ssh" string).
        # Output buffers are exempt: strcpy returns its dest.
        aliased = sorted({cls[1] for cls in classes
                          if cls[0] == 'BUF'
                          and inout.get(cls[1], 'in') == 'in'
                          and not (y_class[0] == 'BUF'
                                   and y_class[1] == cls[1])})
        if aliased:
            return ('unsound', 'unsound: return aliases input buffer %s'
                    ' (real returned %s)'
                    % (aliased[0], _class_text(y_class)))
        # pointer/int confusion smoke signal (_TIFFmalloc's "return
        # (void*)p0" hands the allocation size back as a pointer): every
        # non-NULL achievable return sits below 64KB.  The battery itself
        # injects 0/1/-1, so a lone small value proves nothing -- note
        # only when NO large non-NULL return exists.
        non_null = [cls for cls in classes if cls[0] != 'NULL']
        if non_null and all(cls[0] == 'OTHER' and cls[1] < 0x10000
                            for cls in non_null):
            notes.append('pointer/int confusion suspected: every non-NULL'
                         ' achievable return is below 0x10000 (a scalar'
                         ' argument cast to a pointer?); review')
        if allocator:
            bad = [cls for cls in non_null if cls[0] != 'FRESH']
            if bad:
                return ('unsound', 'allocator return is not fresh storage'
                        ' (achievable non-NULL classes: %s); use'
                        ' __ikos_malloc(size), never an integer cast as'
                        ' the pointer'
                        % ','.join(sorted(_class_text(cls)
                                          for cls in non_null)))
            if y_class[0] == 'NULL':
                if any(cls[0] == 'NULL' for cls in classes):
                    return 'sound', ''
                return ('unsound', 'return too narrow: the real allocator'
                        ' returned NULL on this input but the stub never can')
            if not non_null:
                return ('unsound', 'return too narrow: the real function'
                        ' returned a non-NULL pointer but the stub never can')
            if not any(cls[0] == 'FRESH' for cls in non_null):
                return ('unsound', 'allocator stub has no fresh-allocation'
                        ' capability; use __ikos_malloc(size)')
            return 'sound', 'fresh dynamic-allocation capability'
        if y_class[0] == 'NULL':
            if any(cls[0] == 'NULL' for cls in classes):
                return 'sound', ''
            return ('unsound', 'return too narrow: the real function'
                    ' returned NULL on this input but the stub never can')
        if not non_null:
            return ('unsound', 'return too narrow: the real function'
                    ' returned a non-NULL pointer but the stub never can')
        if y_class[0] == 'OTHER':
            notes.append('fresh-block gap: the real pointer belongs to no'
                         ' caller buffer; the stub vocabulary cannot express'
                         ' points-to TOP, so parameter aliasing is the'
                         ' sanctioned approximation')
            return 'sound*', 'admitted on nullity + non-NULL capability'
        tag, real_off = y_class[1], y_class[2]
        same = [c for c in non_null if c[0] == 'BUF' and c[1] == tag]
        if not same:
            achieved = ','.join(sorted({c[1] for c in non_null
                                        if c[0] == 'BUF'})) or 'nothing'
            return ('unsound', 'wrong block: the real pointer points into'
                    ' %s but the stub can only return %s'
                    % (tag, achieved))
        offsets = [c[2] for c in same] + [0]
        if min(offsets) <= real_off <= max(offsets):
            # pointer offsets reuse the integer domain; under a hull-only
            # target the congruence exclusion is invisible, so admit plainly
            if mode == 'hull' or _in_closure(offsets, real_off):
                return 'sound', ''
            return ('sound*', 'offset approximation: real offset %d is in'
                    ' the achievable hull [%d, %d] but outside the'
                    ' congruence closure of the achievable offsets, so the'
                    ' congruence domains exclude it (identity'
                    ' approximation, cf. the official fgets stub)'
                    % (real_off, min(offsets), max(offsets)))
        return ('sound*', 'identity approximation: real offset %d outside'
                ' the achievable offset hull [%d, %d]'
                % (real_off, min(offsets), max(offsets)))

    width = ret_kind[1]
    y = trunc_int(y_raw, width)
    values = [trunc_int(obs['ret'], width)
              for obs, _events in alive
              if obs['ret'] not in (None, 'void') and len(obs['ret']) <= 8]
    values = [v for v in values if v is not None]
    if not values or y is None:
        return 'unverified', 'no measurable stub return'
    lo, hi = min(values), max(values)
    # narrow-range lint (never a rejection): the achievable interval is a
    # sliver of the return type AND the real value grazes its edge -- the
    # getopt shape, where a hand-tightened range quietly pruned 'o'
    if hi - lo < (1 << (8 * width)) // 16 \
            and (abs(y - lo) <= 1 or abs(y - hi) <= 1):
        notes.append('narrow-range: achievable [%d, %d] much narrower than'
                     ' int%d; verify the stub does not prune real behaviors'
                     % (lo, hi, 8 * width))
    if y in values:
        return 'sound', ''
    wide = hi - lo > (1 << (8 * width)) // 2
    in_hull = lo <= y <= hi
    need_hull = mode in ('hull', 'dual')
    need_clo = mode in ('closure', 'dual')
    if not in_hull and wide:
        # a WIDE hull means the return is derived from nondeterminism by
        # arithmetic the slot-injection battery cannot drive to an exact
        # value (e.g. two __ikos_nondet_uint() calls combined); the
        # analyzer's join over such an expression is likely far wider than
        # the concrete battery, so this is not evidence of unsoundness
        return ('unverified', 'return battery under-drive: %d outside the'
                ' concretely driven hull [%d, %d], but the hull is wide'
                ' (composite nondeterministic return; slot injection may'
                ' not reach every value the analyzer would join in)'
                % (y, lo, hi))
    if in_hull and (not need_clo or _in_closure(values, y)):
        # admitted by every family the target domain consults, though the
        # battery never drove the exact value concretely
        if need_clo:
            notes.append('domain-admitted: %d is inside the hull [%d, %d]'
                         ' and the congruence class of the achievable'
                         ' returns, but was never concretely returned'
                         % (y, lo, hi))
            return 'sound*', 'interval+congruence admission'
        notes.append('domain-admitted: %d is inside the hull [%d, %d] of'
                     ' the achievable returns, but was never concretely'
                     ' returned (the target domain joins by convex hull'
                     ' only)' % (y, lo, hi))
        return 'sound*', 'interval admission'
    if in_hull:
        # the hull admits; the congruence closure does not
        if wide:
            # with a WIDE hull the return is derived from nondeterminism
            # by arithmetic the battery cannot drive to an exact value, so
            # the concrete value set -- and its closure -- is not evidence
            notes.append('interval-admitted only: %d in hull [%d, %d]; the'
                         ' hull is wide, so the congruence closure of the'
                         ' battery is not conclusive for the congruence'
                         ' domains' % (y, lo, hi))
            return 'sound*', 'interval admission'
        r0, d = _congruence_closure(values)
        return ('unsound', 'return too narrow for the congruence domains:'
                ' the achievable returns %s join to x = %d (mod %d) under'
                ' -d congruence / interval-congruence, excluding the real'
                ' value %d which only the interval hull [%d, %d] covers;'
                ' enumerate the complete value set (add %d) or return a'
                ' full-width nondeterministic value narrowed with'
                ' __ikos_assume'
                % (sorted(set(values)), r0, d, y, lo, hi, y))
    # y is outside the hull: unsound for every hull-tracking domain; the
    # pure congruence target never consults the hull, so its class decides
    if need_hull:
        return ('unsound', 'return too narrow: the real function returned'
                ' %d but the achievable stub returns span [%d, %d]'
                % (y, lo, hi))
    r0, d = _congruence_closure(values)
    if not _in_closure(values, y):
        return ('unsound', 'return too narrow for the congruence domains:'
                ' the achievable returns %s join to x = %d (mod %d), which'
                ' excludes the real value %d; enumerate the complete value'
                ' set (add %d) or return a full-width nondeterministic'
                ' value narrowed with __ikos_assume'
                % (sorted(set(values)), r0, d, y, y))
    notes.append('congruence-admitted: %d is outside the hull [%d, %d] but'
                 ' inside x = %d (mod %d), the class the achievable'
                 ' returns join to under the target domain' % (y, lo, hi,
                                                              r0, d))
    return ('sound*', 'congruence admission: the real value %d is outside'
            ' the hull [%d, %d] but the target domain joins by congruence'
            ' class x = %d (mod %d), which contains it' % (y, lo, hi, r0, d))


def _congruence_closure(values):
    """(r0, d) of the class a congruence-domain join of `values` produces.

    d is the gcd of the pairwise differences (0 for a singleton set, i.e.
    the class is the exact value); the join of {0, 2} in ikos's congruence
    domain is exactly "x = 0 (mod 2)" -- every odd value is EXCLUDED, which
    is more precise than the interval hull [0, 2] in both directions.
    """
    d = 0
    for value in values[1:]:
        d = math.gcd(d, abs(value - values[0]))
    return values[0], d


def _in_closure(values, y):
    r0, d = _congruence_closure(values)
    if d == 0:
        return y == r0
    return (y - r0) % d == 0


def _format_offsets(offsets):
    spans = []
    start = prev = None
    for value in offsets:
        if start is None:
            start = prev = value
        elif value == prev + 1:
            prev = value
        else:
            spans.append(str(start) if start == prev
                         else '%d-%d' % (start, prev))
            start = prev = value
    if start is not None:
        spans.append(str(start) if start == prev else '%d-%d' % (start, prev))
    return ','.join(spans) or '(none)'


#############################
# The gate #
#############################

def check_stub(kit, opt, plan, info, parts, complete, cache, stub_src):
    """Differential gate for one stub candidate.

    -> (reason, summary): reason is None unless the stub is provably NOT an
    over-approximation on some input (then it is a counterexample text for
    the repair loop).  summary['status'] is 'pass', 'unsound' or
    'harness-error' (gate skipped with a warning).
    """
    log = kit.log
    workdir = os.path.join(os.path.expanduser(opt.cache_dir), 'difftest',
                           kit.stub.stub_key(plan.name))
    os.makedirs(workdir, exist_ok=True)
    rundir = tempfile.mkdtemp(prefix='ikos-dt-')

    try:
        # ---- driver + inputs from the model (cached, repaired) ----
        driver_path, inputs, error = _obtain_artifacts(opt, plan, info,
                                                       parts, complete,
                                                       cache, rundir,
                                                       kit.settings.clang(),
                                                       kit=kit)
        if error:
            log.warning('%s: differential gate skipped: %s'
                        % (plan.name, error))
            return None, {'status': 'harness-error', 'fn': plan.name,
                          'detail': error}
        with open(driver_path) as stream:
            driver_src = stream.read()

        records, run_err = run_battery(kit, opt, plan, parts, driver_path,
                                       inputs, stub_src, rundir, info)
        if run_err:
            log.warning('%s: differential gate skipped: %s'
                        % (plan.name, run_err))
            return None, {'status': 'harness-error', 'fn': plan.name,
                          'detail': run_err}

        verdicts = []
        reason = None
        for record in records:
            verdicts.append({'argv': record['argv'],
                             'verdict': record['verdict'],
                             'reason': record['reason'],
                             'notes': record['notes']})
            if record['verdict'] == 'unsound' and reason is None:
                reason = _counterexample(plan, record['entry'],
                                         record['detail']['real'],
                                         record['reason'])
        summary = _summarize(plan, verdicts)
        _persist(workdir, driver_src, inputs, summary, opt)
        if reason is None:
            log.info('%s: differential validation: %s'
                     % (plan.name, summary['counts_line']))
        return reason, summary
    finally:
        if getattr(opt, 'dt_keep_temps', False):
            log.info('%s: difftest scratch kept in %s' % (plan.name, rundir))
        else:
            shutil.rmtree(rundir, ignore_errors=True)


def _ikos_domain(opt):
    """The -d token the analyzer-real judge runs with."""
    override = getattr(opt, 'dt_ikos_domain', None)
    if override:
        return override
    domain = getattr(opt, 'domain', 'all')
    return 'interval-congruence' if domain == 'all' else domain


def _run_ikos_judge(kit, opt, stub_src, parts, real_runs, ret_kind, argv,
                    batdir, index, real_symbol, extra_libs, ident,
                    stub_label, ikos_symbol, ikos_label, domain):
    """The analyzer-real judge for one input.  -> summary dict | None.

    None keeps the simulation phases in charge for this input (stage
    disabled, unusable real runs, out-of-scope signature, or any build/
    analyzer failure inside ikosjudge -- per-input hybrid fallback).
    """
    if getattr(opt, 'dt_no_ikos', False) \
            or ret_kind in ('other', 'aggregate'):
        return None
    usable = [run for run in real_runs if run_status(run) == 'ok']
    if not usable:
        return None
    obs = parse_contract(usable[0]['contract'])
    # canary-union the write sets exactly like judge_input
    for run in usable[1:]:
        other = parse_contract(run['contract'])
        for tag, offsets in other['wrote'].items():
            obs['wrote'][tag] = obs['wrote'].get(tag, set()) | offsets
    ret_probe = None
    if isinstance(ret_kind, tuple) and ret_kind[0] == 'int' \
            and isinstance(obs.get('ret'), (bytes, bytearray)):
        ret_probe = trunc_int(obs['ret'], ret_kind[1])
    pointer_cells = set()
    for tag, offsets in _pointer_write_offsets(obs).items():
        pointer_cells.update((tag, off) for off in offsets)
    return ikosjudge.run(kit, opt, stub_src, parts, ikos_symbol, ikos_label,
                         extra_libs, ident, stub_label, argv, obs,
                         ret_probe, pointer_cells, batdir, index, domain,
                         _dt(opt, 'ikos_cpu'), _dt(opt, 'ikos_wall'),
                         _dt(opt, 'timeout'))


def run_battery(kit, opt, plan, parts, driver_path, inputs, stub_src,
                rundir, info=None, scenario_lib=False):
    """Build both sides and judge every input; -> (records, error).

    One record per input: {'index', 'argv', 'entry', 'verdict', 'reason',
    'notes', 'detail'} with the same verdict semantics as check_stub, plus
    the game's raw material in 'detail' (the canary-unioned real observation
    and the alive stub claims).  error is set on a harness failure (build or
    driver contract), which is never a stub rejection.  scenario_lib is the
    Tier-2 scenario flag: the stub build links the real library too (for
    library-internal setup calls), the stub's target definition winning;
    plans marked by the scenario ladder (_obtain_scenario_artifacts) are
    honored the same way, since the co-evolution game owns this call and
    cannot know which tier produced the driver.
    """
    if plan.name in (getattr(opt, '_dt_scenario_lib_plans', None) or ()):
        scenario_lib = True
    ident, stub_label = kit.stub.c_function_name(plan.name)
    real_bin, stub_bin, build_err = _build(kit, opt, driver_path, stub_src,
                                           ident, plan, rundir, parts, info,
                                           scenario_lib)
    if build_err:
        return None, build_err
    real_symbol, extra_libs = _real_link_inputs(opt, plan)
    ikos_symbol, ikos_label = kit.stub.c_function_name(real_symbol)
    ikos_domain = _ikos_domain(opt)

    # every battery call gets its own subdirectory tree: the recorder
    # opens DT_LOG in append mode, so reusing in-<index> dirs across
    # calls (the game re-judges the same pool under every revised body)
    # would blend the __ikos_assume events of DIFFERENT bodies into one
    # stream, and the alive filter would prune runs on a stale body's
    # violations -- a sound body then prices as "return too narrow" on
    # events a rejected revision left behind
    batdir = tempfile.mkdtemp(prefix='bat-', dir=rundir)
    dbus = _dt(opt, 'sandbox_dbus') and _needs_dbus(plan)

    # ---- probe the driver contract once ----
    probe = run_once(real_bin, inputs[0].get('argv') or ['0'], {},
                     _dt(opt, 'timeout'), rundir, dbus=dbus)
    probe_status = run_status(probe)
    if probe['timeout']:
        return None, ('driver timed out on probe input%s'
                      % (' after entering the target'
                         if probe_status == 'crash-in-target' else ''))
    if probe_status in ('crash-before-call', 'timeout-before-call'):
        return None, _probe_error(probe)
    # A non-timeout crash inside the target can be one argv value outside
    # the real precondition.  The battery below still judges every input and
    # requires at least one contract-usable differential observation.

    ret_type, _name = split_signature(parts.signature_line)
    ret_kind = classify_return(ret_type or 'int')
    mode = admission_mode(getattr(opt, 'domain', 'all'))
    allocator = is_malloc_like(plan, parts)
    records = []
    for index, entry in enumerate(inputs):
        argv = [str(tok)[:4096]
                for tok in (entry.get('argv') or [])][:16]
        real_runs = [run_once(real_bin, argv, {'DT_CANARY': canary},
                              _dt(opt, 'timeout'), rundir, dbus=dbus)
                     for canary in ('aa', '55')]
        stub_runs = _stub_runs(opt, stub_bin, argv, real_runs,
                               ret_kind, batdir, index, dbus=dbus)
        retry_runs, fired = _empty_alive_retry(opt, stub_bin, argv,
                                               stub_runs, real_runs,
                                               batdir, index, dbus=dbus)
        if fired:
            stub_runs = stub_runs + retry_runs
        ikos_result = _run_ikos_judge(kit, opt, stub_src, parts,
                                      real_runs, ret_kind, argv, batdir,
                                      index, real_symbol, extra_libs,
                                      ident, stub_label, ikos_symbol,
                                      ikos_label, ikos_domain)
        verdict, why, notes, detail = judge_input(real_runs, stub_runs,
                                                  ret_kind, mode,
                                                  ikos=ikos_result,
                                                  allocator=allocator)
        if ikos_result is not None:
            detail['ikos'] = ikosjudge.summary(ikos_result)
        if fired:
            notes.append('empty-alive retry: %d argv-seeded schedule'
                         ' run(s) added' % len(retry_runs))
        records.append({'index': index, 'argv': argv, 'entry': entry,
                        'verdict': verdict, 'reason': why, 'notes': notes,
                        'detail': detail})
    if not any(record['verdict'].startswith('sound') for record in records):
        return None, ('the real function produced no contract-usable'
                      ' differential input (crashes, timeouts, flaky returns,'
                      ' or contradictory stub schedules)')
    return records, None


def _build_real_probe(kit, opt, plan, info, parts, driver_path, rundir,
                      clang):
    """-> (probe_bin, error): the real side of _build, standalone.

    The scenario escalation probes the REAL side before any stub enters
    the artifact flow (coevolve owns the battery loop and re-judges with
    the SAME driver every game round, so a real-side washout must be
    detected where the driver is chosen, not after the battery).
    """
    real_symbol, extra_libs = _real_link_inputs(opt, plan)
    cfn = _c_function_name
    if kit is not None and getattr(kit, 'stub', None) is not None:
        cfn = kit.stub.c_function_name
    ident, _label = cfn(plan.name)
    real_ident, real_label = cfn(real_symbol)
    cxx = is_cxx_function(plan)
    sret_abi = cxx and _uses_sret_return(info)
    wrap = _write_dt_wrapper(real_ident, real_label, parts.signature_line,
                             ident, rundir, 'probe', parts.typedef_lines,
                             sret_abi)
    if wrap is None:
        return None, ('cannot rename %s to dt_target in %s'
                      % (ident, parts.signature_line.strip()))
    sanitize = [] if getattr(opt, 'dt_no_sanitize', False) else \
        ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    probe_bin = os.path.join(rundir, 'probe.bin')
    cmd = ([clang, '-O0', '-g', '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=0',
            '-I', os.path.join(DT_RUNTIME_DIR, 'include')]
           + sanitize + [wrap]
           + (['-x', 'c++', driver_path, '-x', 'none'] if cxx
              else [driver_path])
           + [os.path.join(DT_RUNTIME_DIR, 'dt_real.c'),
              '-o', probe_bin, '-lm'] + extra_libs
           + (['-lstdc++'] if cxx else []))
    proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE,
                          universal_newlines=True, errors='replace')
    if proc.returncode != 0:
        return None, 'clang failed: %s' % proc.stderr[-500:]
    return probe_bin, None


def _real_side_probe(opt, plan, info, parts, driver_path, inputs, rundir,
                     clang, kit):
    """-> (usable, evidence, good_index) | None (inconclusive).

    Would the battery get one contract-usable differential input out of
    this driver's REAL side?  An input counts when both canary runs
    complete the contract and agree on the return bytes -- exactly what
    judge_input can weigh.  good_index is the first such input: the
    battery probes inputs[0] and dies when that one crashes before the
    target call, so the caller rotates it to the front.  None means the
    probe itself cannot answer (probe build failure): the battery's own
    diagnostics report it better, so the caller keeps the driver it has.
    """
    probe_bin, error = _build_real_probe(kit, opt, plan, info, parts,
                                         driver_path, rundir, clang)
    if error:
        return None
    evidence = 'no probe input produced a contract'
    dbus = _dt(opt, 'sandbox_dbus') and _needs_dbus(plan)
    for index, entry in enumerate(inputs[:4]):
        argv = [str(tok)[:4096]
                for tok in (entry.get('argv') or [])][:16]
        # two canary runs, TWICE: a garbage-state driver (pattern bytes as
        # a regex_t) crashes nondeterministically, and a single lucky pair
        # must not count as usable -- the battery re-samples later and
        # would wash out on the same flakiness the probe overlooked
        runs = [run_once(probe_bin, argv, {'DT_CANARY': canary},
                         _dt(opt, 'timeout'), rundir, dbus=dbus)
                for _rep in range(2) for canary in ('aa', '55')]
        statuses = [run_status(run) for run in runs]
        if all(status == 'ok' for status in statuses):
            rets = {parse_contract(run['contract'])['ret_raw']
                    for run in runs}
            if len(rets) == 1:
                return True, None, index
            evidence = ('the real function returns different values'
                        ' across identical runs (flaky)')
            continue
        bad = next(run for run, status in zip(runs, statuses)
                   if status != 'ok')
        status = run_status(bad)
        if bad['timeout']:
            evidence = ('driver timed out on probe input%s'
                        % (' after entering the target'
                           if status == 'crash-in-target' else ''))
        elif status == 'crash-in-target':
            evidence = ('the real function crashed inside the target on'
                        ' probe input (rc=%s)%s'
                        % (bad['rc'], _stderr_digest(bad)))
        else:
            evidence = _probe_error(bad)
    return False, evidence, None


def _inputs_first(inputs, index):
    """inputs rotated so the probe-verified usable entry comes first."""
    if not index:
        return inputs
    return [inputs[index]] + [entry for pos, entry in enumerate(inputs)
                              if pos != index]


def _stderr_digest(run):
    """The crash digest (ASan summary + first frames) of a failed run."""
    if not run['stderr']:
        return ''
    return ', stderr digest:\n%s' % '\n'.join(
        run['stderr'].splitlines()[:20])


def _obtain_scenario_artifacts(opt, plan, info, parts, complete, cache,
                               rundir, clang, kit, evidence):
    """The scenario ladder after a real-side washout; -> (driver_path,
    inputs, error).

    Tier 1 buys a scenario driver over the libc setup constructors
    (DT_SETUP_CONSTRUCTORS); the dt_scenario_lib tier (experimental)
    additionally offers the target library's own setup functions and
    marks the plan so run_battery links the real library into the stub
    side too (see _build).  Every tier is pre-probed on the real side;
    the first usable one wins.  When every tier washes out the error
    says so -- the battery would only rediscover it.
    """
    tiers = [((), 'libc setup whitelist')]
    if _dt(opt, 'scenario_lib'):
        candidates = _scenario_lib_candidates(opt, plan)
        if candidates:
            tiers.append((candidates,
                          'library setup: %s' % ','.join(candidates)))
    memo = getattr(opt, '_dt_scenario_lib_plans', None)
    if memo is None:
        memo = set()
        setattr(opt, '_dt_scenario_lib_plans', memo)
    memo.discard(plan.name)
    description = tiers[-1][1]
    for lib_functions, description in tiers:
        driver_path, inputs, error = _obtain_artifacts(
            opt, plan, info, parts, complete, cache, rundir, clang,
            scenario=True, lib_functions=lib_functions, evidence=evidence,
            kit=kit)
        if error:
            return None, None, error
        probe = _real_side_probe(opt, plan, info, parts, driver_path,
                                 inputs, rundir, clang, kit)
        if probe is None:
            # inconclusive probe: hand the scenario driver to the battery
            if lib_functions:
                memo.add(plan.name)
            return driver_path, inputs, None
        usable, evidence, good = probe
        if usable:
            if lib_functions:
                memo.add(plan.name)
            return driver_path, _inputs_first(inputs, good), None
    return (None, None,
            'the real function produced no contract-usable input even'
            ' with a scenario driver (%s): %s' % (description, evidence))


def _scenario_lib_candidates(opt, plan, limit=8):
    """Tier-2 library-internal setup candidates; -> tuple (maybe empty).

    Two V1 heuristics over the real bitcode, merged:

    * constructor match -- an external function returning the pointee of
      one of the target's pointer parameters builds exactly the state the
      target needs (dbus_bus_get -> DBusConnection* for
      dbus_bus_request_name); ranked first, since a caller that never
      calls the target itself is invisible to the second heuristic;
    * call-graph adjacency -- an external function called by a definition
      that also calls the target probably builds or consumes the same
      state (xmlInitParser next to xmlReadMemory).

    Both are restricted to names with an external declaration in the
    bitcode and, when a real-library map is available, to symbols the map
    can resolve; most frequent co-occurrence first within each group.
    """
    try:
        import cxprobe
    except ImportError:
        return ()
    bc = None
    for arg in getattr(opt, 'inputs', None) or ():
        if arg.endswith('.bc') and os.path.isfile(arg):
            bc = arg
            break
    if bc is None:
        return ()
    cache_dir = os.path.join(os.path.expanduser(
        getattr(opt, 'cache_dir', '~/.cache')), 'cxprobe')
    try:
        ir = cxprobe.ir_text(bc, cache_dir=cache_dir)
    except Exception:
        return ()  # candidate discovery is best effort, never a failure

    declare_lines = re.findall(r'^declare\b[^\n]*', ir, re.M)
    declared = set()
    for line in declare_lines:
        match = re.search(r'@("(?:\\.|[^"\\])*"|[\w$.-]+)', line)
        if match:
            declared.add(match.group(1).strip('"'))

    # ---- constructor match: return pointee == a parameter pointee ----
    target_decl = next((line for line in declare_lines
                        if re.search(r'@("?(?:%s)"?)\s*\('
                                     % re.escape(plan.name), line)), None)
    constructors = []
    if target_decl is not None:
        params = target_decl[target_decl.find('('):]
        pointees = set(re.findall(r'(%[\w.$-]+)\s*\*+', params))
        if pointees:
            for line in declare_lines:
                ret_region = line.split('@', 1)[0]
                if not any(pointee in ret_region for pointee in pointees):
                    continue
                match = re.search(r'@("(?:\\.|[^"\\])*"|[\w$.-]+)', line)
                name = match.group(1).strip('"') if match else None
                if name and name != plan.name:
                    constructors.append(name)

    # ---- call-graph adjacency ----
    counts = {}
    for chunk in re.split(r'\n(?=define\b)', ir):
        if not chunk.startswith('define'):
            continue
        names = {name.strip('"') for name in
                 re.findall(r'@("(?:\\.|[^"\\])*"|[\w$.-]+)\s*\(', chunk)}
        if plan.name not in names:
            continue
        for name in names - {plan.name}:
            counts[name] = counts.get(name, 0) + 1
    table = getattr(opt, 'real_lib_map', None) or {}

    def resolvable(name):
        return name in declared and (not table or name in table)

    chosen = [name for name in sorted(constructors) if resolvable(name)]
    ranked = sorted(counts, key=lambda name: (-counts[name], name))
    chosen += [name for name in ranked
               if name not in chosen and resolvable(name)]
    return tuple(chosen[:limit])


def _stub_runs(opt, stub_bin, argv, real_runs, ret_kind, rundir, index,
               dbus=False):
    """all schedule-driven stub runs for one input (probe learns K).

    Each input gets its own subdirectory inside the per-call battery dir
    (see run_battery): the recorder opens its log in append mode, so
    sharing names across inputs or battery calls would blend the events
    of different runs into one file and the judge would read another
    input's -- or another body's -- preconditions (a false unsound).
    """
    timeout = _dt(opt, 'timeout')
    subdir = os.path.join(rundir, 'in-%d' % index)
    os.makedirs(subdir, exist_ok=True)
    counter = [0]

    def one(schedule):
        logpath = os.path.join(subdir, 'dtlog-%d.txt' % counter[0])
        counter[0] += 1
        run = run_once(stub_bin, argv,
                       {'DT_NONDET': ','.join(map(str, schedule)),
                        'DT_LOG': logpath},
                       timeout, rundir, limit_as=DT_RLIMIT_AS, dbus=dbus)
        try:
            with open(logpath) as stream:
                run['log'] = stream.read()
        except OSError:
            run['log'] = ''
        return run

    probe = one([])
    nondet_count = sum(1 for event in parse_dtlog(probe['log'])
                       if event[0] == 'NONDET')
    obsA = parse_contract(real_runs[0]['contract'])
    schedules = build_schedules(y_values(ret_kind, obsA['ret']),
                                nondet_count, _dt(opt, 'schedules'))
    runs = [probe]
    for schedule in schedules:
        runs.append(one(schedule))
    return runs


def _argv_int_seeds(argv, limit=8):
    """Integer values to inject for the input's own tokens.

    The driver's token -> argument mapping is model-defined, so this is
    a heuristic: base-10 ints only, {v, v-1, v+1} per token.  NOT
    truncated to the return width -- the assume compares the nondet slot
    (an int32) against the parameter as C computes it, and pre-truncating
    would corrupt the comparison (a char return with p0 = 300 would seed
    44).  Boundary values are left out: the base battery already pruned
    them.  The recorder's (int) cast truncates naturally.
    """
    seeds = set()
    for token in argv[:limit]:
        try:
            value = int(token, 10)
        except ValueError:
            continue
        seeds.update((value, value - 1, value + 1))
    return sorted(seeds)


def _seed_schedules(seeds, nondet_count, limit):
    """Schedules injecting each seed at the first nondet slots: the
    constrained value is nondet #0 in the common shapes (r = nondet();
    assume(r ... p0)); slot #1 covers one preceding helper draw."""
    count = max(nondet_count, 1)
    schedules = []
    seen = set()

    def add(values):
        key = tuple(values)
        if key not in seen:
            seen.add(key)
            schedules.append(list(values))

    for pos in range(1, min(2, count) + 1):
        for value in seeds:
            sched = [0] * (count + 2)
            sched[pos - 1] = value
            add(sched)
    return schedules[:min(16, limit)]


def _empty_alive_retry(opt, stub_bin, argv, stub_runs, real_runs,
                       rundir, index, dbus=False):
    """Argv-seeded schedules for a body whose assume pruned the whole
    battery (-> (extra_runs, fired)).

    The base battery seeds the nondet slots from the REAL return only,
    so a relational claim (r == p0) can prune every schedule without
    leaving a measurable hull -- the wrong-relation case would fall to
    'unverified' and install with a caveat.  This retry injects the
    input's own tokens so the claim becomes judgeable.  The extra runs
    are APPENDED to the battery: a sound body keeps its y-seeded
    survivors, so the retry can never turn a pass into a rejection;
    when every y-seed was already pruned, that pruning is itself the
    evidence (the claim concretely excludes the real value) and the
    retry merely supplies the hull the judge needs to phrase 'return
    too narrow'.  Fires only when the real runs are usable and the body
    drew at least one nondet value (a constant contradiction cannot be
    rescued); uses its own subdirectory inside the per-call battery dir
    because the recorder opens its log in append mode.
    """
    if alive_runs(stub_runs) or any(run_status(r) != 'ok'
                                    for r in real_runs):
        return [], False
    probe_log = stub_runs[0]['log'] if stub_runs else ''
    nondet_count = sum(1 for event in parse_dtlog(probe_log)
                       if event[0] == 'NONDET')
    if nondet_count < 1:
        return [], False
    seeds = _argv_int_seeds(argv)
    if not seeds:
        return [], False
    subdir = os.path.join(rundir, 'in-%d-retry' % index)
    os.makedirs(subdir, exist_ok=True)
    timeout = _dt(opt, 'timeout')
    counter = [0]

    def one(schedule):
        logpath = os.path.join(subdir, 'dtlog-%d.txt' % counter[0])
        counter[0] += 1
        run = run_once(stub_bin, argv,
                       {'DT_NONDET': ','.join(map(str, schedule)),
                        'DT_LOG': logpath},
                       timeout, rundir, limit_as=DT_RLIMIT_AS, dbus=dbus)
        try:
            with open(logpath) as stream:
                run['log'] = stream.read()
        except OSError:
            run['log'] = ''
        return run

    runs = [one(schedule)
            for schedule in _seed_schedules(seeds, nondet_count,
                                            _dt(opt, 'schedules'))]
    return runs, True


def _counterexample(plan, entry, obs, why):
    writes = '; '.join('%s=%s' % (tag, _format_offsets(sorted(offsets)))
                       for tag, offsets in sorted(obs['wrote'].items()))
    return ('the differential validation found this stub is NOT an'
            ' over-approximation:\n'
            '  input argv: %s\n'
            '  real %s returned %s%s\n'
            '  your stub: %s\n'
            'Fix the body so it over-approximates the real behavior (havoc'
            ' more, widen the return, weaken the precondition).'
            % ([str(t) for t in entry.get('argv', [])],
               plan.source_name,
               obs['ret_raw'],
               ('; wrote ' + writes) if writes else '',
               why))


def _summarize(plan, verdicts):
    counts = {}
    for item in verdicts:
        counts[item['verdict']] = counts.get(item['verdict'], 0) + 1
    line = ', '.join('%d %s' % (count, kind)
                     for kind, count in sorted(counts.items())) \
        or 'no inputs'
    unsound = any(item['verdict'] == 'unsound' for item in verdicts)
    return {'status': 'unsound' if unsound else 'pass',
            'fn': plan.name, 'verdicts': verdicts, 'counts': counts,
            'counts_line': ('unsoundness FOUND on %d input(s): %s'
                            % (counts.get('unsound', 0), line)
                            if unsound else
                            'no unsoundness found on %d input(s): %s'
                            % (len(verdicts), line))}


def _persist(workdir, driver, inputs, summary, opt):
    try:
        with open(os.path.join(workdir, 'driver.c'), 'w') as stream:
            stream.write(driver)
        with open(os.path.join(workdir, 'inputs.json'), 'w') as stream:
            json.dump(inputs, stream, indent=1)
        with open(os.path.join(workdir, 'report.json'), 'w') as stream:
            json.dump(summary, stream, indent=1)
        report_path = getattr(opt, 'dt_report', None)
        if report_path:
            data = {}
            try:
                with open(report_path) as stream:
                    data = json.load(stream)
            except (OSError, ValueError):
                pass
            data[summary['fn']] = summary
            with open(report_path, 'w') as stream:
                json.dump(data, stream, indent=1)
    except OSError:
        pass  # artifacts are diagnostics, never worth failing the gate


def _write_dt_wrapper(call_ident, call_label, sig_line, target, rundir,
                      suffix, typedef_lines=(), sret_abi=False):
    """A one-line wrapper `dt_target -> call_ident`, using the IR types.

    The wrapper is its own translation unit: the only declaration of the
    callee it ever sees is the one written here, so the IR-translated
    types can never clash with a system header's prototype.  `target` is
    the name as it actually appears in sig_line (the stub's C
    identifier); the rename is word-boundary anchored so mangled C++
    names and glibc-renamed spellings (`__isoc99_*`, `*_lfs64`) survive
    -- a plain substring replace used to cut inside `__isoc99_sscanf`
    and leave `dt_target` undefined.  `call_label` is the __asm__ label
    when call_ident is not the exported symbol (mirroring render_stub).
    """
    match = re.search(r'(?<![A-Za-z0-9_])%s(?![A-Za-z0-9_])'
                      % re.escape(target), sig_line)
    if match is None:
        return None
    sig = sig_line[:match.start()] + 'dt_target' + sig_line[match.end():]
    ret = sig_line[:match.start()].strip()
    params = sig[sig.find('(') + 1:sig.rfind(')')]
    call_names = None
    if sret_abi:
        # A C++ aggregate return lowers to a hidden first result pointer.
        # Exposing that pointer keeps both sides on the object-level ABI and
        # lets the C++ driver register the actual result object.
        old_params = params.strip()
        params = (('%s* ikos_dt_sret, %s' % (ret, old_params))
                  if old_params and old_params != 'void'
                  else '%s* ikos_dt_sret' % ret)
        sig = 'void dt_target(%s)' % params
        ret = 'void'
    if params.strip() in ('', 'void'):
        names = []  # () and (void) both mean zero parameters
    else:
        names = [p.strip().split()[-1].rstrip('[]')
                 for p in params.split(',') if p.strip()]
        call_names = (['ikos_dt_sret'] + names[1:]
                      if sret_abi else names)
    suffix_decl = (' __asm__("%s");' % call_label) if call_label else ';'
    decl = '%s %s(%s)%s' % (ret, call_ident,
                            params.replace(' dt_target', ''), suffix_decl)
    forwarded = call_names if call_names is not None else names
    body = ('return %s(%s);' % (call_ident, ', '.join(forwarded))
            if ret != 'void' else
            '%s(%s);' % (call_ident, ', '.join(forwarded)))
    prelude = ''.join('%s\n' % line for line in typedef_lines)
    if prelude:
        prelude += '\n'
    text = ('%s%s\n%s\n{\n    %s\n}\n' % (prelude, decl, sig, body))
    path = os.path.join(rundir, 'dt_%s_wrapper.c' % suffix)
    with open(path, 'w') as stream:
        stream.write(text)
    return path


def _uses_sret_return(info):
    """Whether the original LLVM declaration uses a hidden sret pointer."""
    line = getattr(info, 'line', '') or ''
    return ' sret(' in line


def _real_link_inputs(opt, plan):
    """(real symbol, extra link args) for one function's real side.

    Prefers the memo `_ensure_real_link` left on opt (map hit plus any
    probe-verified repair); falls back to the raw map so a battery that
    reaches _build without the probe still resolves -- implicit
    libc/libm is the (symbol, []) floor.
    """
    memo = getattr(opt, '_real_link_args', None) or {}
    entry = memo.get(plan.name)
    if entry is not None:
        return entry
    hit = reallibs.lookup(getattr(opt, 'real_lib_map', None), plan)
    if hit is None:
        return plan.name, []
    return hit


def _build(kit, opt, driver_path, stub_src, ident, plan, rundir, parts=None,
           info=None, scenario_lib=False):
    clang = kit.settings.clang()
    common = ['-O0', '-g', '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=0',
              '-I', os.path.join(DT_RUNTIME_DIR, 'include')]
    sanitize = [] if getattr(opt, 'dt_no_sanitize', False) else \
        ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    real_bin = os.path.join(rundir, 'real.bin')
    stub_bin = os.path.join(rundir, 'stub.bin')
    # the real side links the resolved real implementation: an in-tree
    # archive, a configured dependency library, or implicit libc/libm
    real_symbol, extra_libs = _real_link_inputs(opt, plan)
    # the driver only ever declares and calls the dt_target alias; BOTH
    # sides link a generated one-line wrapper (own translation unit, the
    # only declaration of the callee name it contains) -- the IR types can
    # never clash with a system header on either side.  The rename target
    # is the stub's C identifier (what sig_line actually spells); the real
    # wrapper forwards to the resolved real symbol (mangled for C++,
    # __isoc99_* for glibc-renamed calls -- exactly what the library
    # exports), the stub wrapper to the stub's identifier.
    if parts is not None:
        if '...' in parts.signature_line:
            # varargs cannot be forwarded through a C wrapper (no va_list
            # form of the target): a deterministic, honestly-reported skip
            return None, None, ('variadic signature %s cannot be forwarded'
                                ' through the dt_target wrapper; gate'
                                ' skipped' % parts.signature_line.strip())
        real_ident, real_label = kit.stub.c_function_name(real_symbol)
        _ident2, stub_label = kit.stub.c_function_name(plan.name)
        sret_abi = is_cxx_function(plan) and _uses_sret_return(info)
        real_wrap = _write_dt_wrapper(real_ident, real_label,
                                      parts.signature_line, ident, rundir,
                                      'real', parts.typedef_lines, sret_abi)
        stub_wrap = _write_dt_wrapper(ident, stub_label,
                                      parts.signature_line, ident, rundir,
                                      'stub', parts.typedef_lines, sret_abi)
        if real_wrap is None or stub_wrap is None:
            return None, None, ('cannot rename %s to dt_target in %s'
                                % (ident, parts.signature_line.strip()))
        real_extra = [real_wrap]
        stub_extra = [stub_wrap]
    else:
        real_extra = []
        stub_extra = []
    cxx = is_cxx_function(plan)
    sret_abi = cxx and _uses_sret_return(info)
    driver_input = (['-x', 'c++', driver_path, '-x', 'none']
                    if cxx else [driver_path])
    cxx_libs = ['-lstdc++'] if cxx else []
    real_cmd = ([clang] + common + sanitize + real_extra +
                driver_input + [os.path.join(DT_RUNTIME_DIR, 'dt_real.c'),
                 '-o', real_bin, '-lm'] + extra_libs + cxx_libs)
    stub_libs = []
    if scenario_lib and extra_libs:
        # Tier-2 scenario: the stub side links the real library too, so
        # the driver's library-internal setup calls run the REAL code on
        # both sides; only the target stays a stub.  The stub object comes
        # first on the link line and --allow-multiple-definition makes ld
        # keep that first definition when an archive member pulled for a
        # setup symbol also defines the target (verified by the test
        # suite).  For a shared library the executable's own definition
        # interposes without any flag.
        stub_libs = ['-Wl,--allow-multiple-definition'] + list(extra_libs)
    stub_cmd = ([clang] + common + stub_extra +
                driver_input + [stub_src,
                 os.path.join(DT_RUNTIME_DIR, 'recorder.c'),
                 os.path.join(DT_RUNTIME_DIR, 'dt_real.c'),
                 '-o', stub_bin, '-lm'] + stub_libs + cxx_libs)
    for cmd in (real_cmd, stub_cmd):
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE,
                              universal_newlines=True, errors='replace')
        if proc.returncode != 0:
            return None, None, 'clang failed: %s' % proc.stderr[-500:]
    return real_bin, stub_bin, None


#############################
# Driver + inputs artifacts #
#############################

def _driver_compile_check(src, clang, cxx=False):
    """clang -fsyntax-only the driver against dt.h; -> error text or None.

    driver_guard only checks the source shape; a driver that passes it can
    still fail to compile.  Feed that error back so the model repairs it
    instead of the whole gate being skipped.  Compiled under the dt_target
    alias, exactly like _build does (without -Ddt_target the call would be
    an undeclared identifier).
    """
    fd, tmp = tempfile.mkstemp(suffix='.c')
    try:
        with os.fdopen(fd, 'w') as stream:
            stream.write(src)
        cmd = [clang, '-fsyntax-only', '-Ddt_target=__dt_compile_alias',
               '-I', os.path.join(DT_RUNTIME_DIR, 'include')]
        if cxx:
            cmd += ['-x', 'c++', '-std=c++17']
        cmd.append(tmp)
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE,
                              universal_newlines=True, errors='replace')
        if proc.returncode != 0:
            return proc.stderr[-500:]
        return None
    finally:
        os.unlink(tmp)


def _driver_sret_guard(src):
    """Reject a C++ driver that hides the LLVM sret result pointer."""
    if not re.search(r'\bvoid\s+dt_target\s*\(', src):
        return ('the LLVM sret driver must declare void dt_target with'
                ' `result` as its first pointer parameter')
    if '__dt_buf("result", &result, sizeof result)' not in src:
        return ('the LLVM sret driver must register the default-constructed'
                ' result object with __dt_buf before __dt_call')
    if not re.search(r'\bdt_target\s*\(\s*&result\b', src):
        return ("the LLVM sret driver must pass &result as dt_target's"
                ' first argument and ignore the void return')
    return None


def _valid_link_args(args):
    """Grammar gate for model-proposed link inputs.

    Only additions the linker understands as library inputs are ever
    accepted: .a/.so/.o paths and -l/-L/-Wl,-rpath flags.  Anything else
    (source files, -o, macros, shell) is rejected and burns a retry.
    """
    for arg in args:
        if not isinstance(arg, str) or not arg or any(c.isspace()
                                                      for c in arg):
            return False
        if arg.startswith(('-l', '-L')) and len(arg) > 2:
            continue
        if arg.startswith('-Wl,-rpath,') \
                and len(arg) > len('-Wl,-rpath,'):
            continue
        if not arg.startswith('-') and re.search(r'\.(a|o|so(\.\d+)*)$',
                                                 arg):
            continue
        return False
    return True


LINK_REPAIR_PROMPT = """A C probe that only takes the address of the symbol \
`%s` (declared `extern void %s(void);`, never called, so no ABI is \
involved) fails to build or run:

    clang -O0 probe.c -o probe.bin -lm%s

Diagnostics (last line shown to you):
%s

Diagnose the missing link input -- most often a static archive's \
transitive dependencies (e.g. libxml2.a also needs -lz -llzma, openssl's \
libssl.a needs libcrypto.a) or a shared object whose directory needs an \
rpath.  Reply with JSON only, no prose:
{"link_args": ["<one flag or path per element>", ...]}
The elements are APPENDED to the link line; propose only .a/.so/.o \
paths, -l flags, -L paths and -Wl,-rpath,<dir>.  Empty list if you cannot \
diagnose it."""


def _ask_link_repair(complete, plan, symbol, args, detail):
    """One model round for additional link inputs; -> list or None."""
    shown = detail[-500:] if detail else detail
    message = LINK_REPAIR_PROMPT % (symbol, symbol,
                                    (' ' + ' '.join(args)) if args else '',
                                    shown or '(no diagnostics)')
    try:
        content, _usage = complete([{'role': 'user',
                                     'content': message}])
    except Exception as error:
        if getattr(error, 'is_fn_budget', False):
            raise  # a budget cut is not a harness failure
        return None
    text = _strip_fences(content.strip())
    start, end = text.find('{'), text.rfind('}')
    if start < 0 or end <= start:
        return None
    try:
        data = json.loads(text[start:end + 1])
    except ValueError:
        return None
    extra = data.get('link_args')
    if not isinstance(extra, list) or not _valid_link_args(extra):
        return None
    return extra


def _ensure_real_link(opt, plan, info, complete, cache, rundir, clang):
    """Resolve and probe-verify the real-side link inputs of one function.

    Runs BEFORE any driver/inputs LLM artifact is bought: a real
    implementation that cannot be linked makes the whole battery
    impossible, so it must be discovered while it is still free.  The
    address-only probe (link AND run) validates the map's answer and
    surfaces rpath/DT_NEEDED gaps; a failed probe feeds the diagnostics
    to the model to propose additional link inputs (bounded retries,
    grammar-gated).  The winning combination is memoized on opt for
    _build and cached under the responses cache so later runs skip the
    model call.  -> None on success, or an error string (honest skip).
    """
    memo = getattr(opt, '_real_link_args', None)
    if memo is None:
        memo = {}
        setattr(opt, '_real_link_args', memo)
    if plan.name in memo:
        return None
    hit = reallibs.lookup(getattr(opt, 'real_lib_map', None), plan)
    symbol = hit[0] if hit else plan.name
    args = list(hit[1]) if hit else []
    key = dt_cache_key('linkrepair', opt, plan, info)
    cached = cache.get(key) if cache is not None else None
    if cached and cached.get('link_args'):
        candidate = list(cached['link_args'])
        ok, _detail = reallibs.probe_link(clang, symbol, candidate, rundir)
        if ok:
            memo[plan.name] = (symbol, candidate)
            return None
    retries = _dt(opt, 'link_retries')
    detail = ''
    for attempt in range(retries + 1):
        ok, detail = reallibs.probe_link(clang, symbol, args, rundir)
        if ok:
            memo[plan.name] = (symbol, args)
            if cache is not None and attempt > 0:
                cache[key] = {'link_args': args, 'model': opt.model,
                              'time': int(time.time())}
            return None
        if attempt == retries:
            break
        extra = _ask_link_repair(complete, plan, symbol, args, detail)
        if extra is None:
            break
        args = args + extra
    return ('the real implementation of %s cannot be linked%s (%s):'
            ' differential gate skipped'
            % (symbol,
               ' with %s' % ' '.join(args) if args else '',
               (detail or 'unknown link failure')[-300:]))


def _obtain_artifacts(opt, plan, info, parts, complete, cache, rundir,
                      clang, scenario=False, lib_functions=(),
                      evidence=None, kit=None):
    """-> (driver_path, inputs, error); error set means harness failure.

    scenario=True buys a scenario-mode driver (setup constructor calls
    before the target; its own cache keys, the addendum system prompt and
    the widened guard).  lib_functions are the Tier-2 library setup names
    the guard and prompt additionally admit; evidence carries the previous
    failure's crash digest into the first request.  kit (when the caller
    has one) supplies the exact IR-name mapping for the probe wrapper.

    A BARE driver is real-side pre-probed before it is returned: when no
    input would give the battery a usable real observation (the top
    failure class -- regexec without regcomp, fdopen without open), the
    scenario ladder runs here, so every downstream consumer (check_stub
    AND the co-evolution game, which re-judges with the same driver every
    round) gets consistent artifacts.
    """
    link_error = _ensure_real_link(opt, plan, info, complete, cache,
                                   rundir, clang)
    if link_error:
        return None, None, link_error
    if parts is not None and '...' in parts.signature_line:
        # the same honest skip _build reports, hoisted BEFORE the driver
        # and inputs are bought: a variadic target can never reach the
        # battery, so its LLM artifacts would be pure waste
        return None, None, ('variadic signature %s cannot be forwarded'
                            ' through the dt_target wrapper; gate'
                            ' skipped' % parts.signature_line.strip())
    kind = 'driver'
    if scenario:
        kind = 'driver-scenario-lib' if lib_functions \
            else 'driver-scenario'
    driver_key = dt_cache_key(kind, opt, plan, info)
    inputs_key = dt_cache_key('inputs' + kind[len('driver'):],
                              opt, plan, info)

    driver_src = None
    cxx = is_cxx_function(plan)
    sret_abi = cxx and _uses_sret_return(info)
    if driver_key in cache and not opt.force:
        driver_src = cache[driver_key]['body']
        if sret_abi:
            reason = _driver_sret_guard(driver_src)
            if reason is not None:
                return None, None, ('the cached C++ driver violates the LLVM'
                                    ' sret contract: %s' % reason)

    if driver_src is None:
        system = (CXX_DRIVER_SYSTEM_PROMPT if cxx
                  else DRIVER_SYSTEM_PROMPT)
        user = (cxx_driver_user_prompt(plan, parts) if cxx
                else driver_user_prompt(plan, parts))
        if scenario:
            system += scenario_addendum(lib_functions)
            if evidence:
                user += ('\n\nThe previous bare driver failed to produce'
                         ' one usable run of the real function:\n%s\n\n'
                         'Build the missing state with the setup'
                         ' whitelist.' % evidence)
        messages = [{'role': 'system', 'content': system},
                    {'role': 'user',
                     'content': user}]
        for _rnd in range(_dt(opt, 'driver_retries')):
            try:
                content, _usage = complete(messages)
            except Exception as error:  # llm_client.LLMError without import
                if getattr(error, 'is_fn_budget', False):
                    raise  # a budget cut is not a harness failure
                return None, None, 'the driver request failed: %s' % error
            src = _strip_fences(content.strip())
            reason = driver_guard(src, 'dt_target', scenario=scenario,
                                  extra_setup=lib_functions)
            if reason is None and sret_abi:
                reason = _driver_sret_guard(src)
            if reason is None:
                # the guard passed: also require the driver to compile
                reason = _driver_compile_check(src, clang, cxx=cxx)
            if reason is None:
                driver_src = src
                cache[driver_key] = {'body': src, 'model': opt.model,
                                     'time': int(time.time())}
                break
            messages.append({'role': 'assistant', 'content': src})
            messages.append({'role': 'user',
                             'content': dt_rejection_prompt(reason)})
        if driver_src is None:
            return None, None, ('the model could not produce an acceptable'
                                ' driver')

    driver_path = os.path.join(rundir, 'driver.cc' if cxx else 'driver.c')
    with open(driver_path, 'w') as stream:
        stream.write(driver_src)

    inputs = None
    if inputs_key in cache and not opt.force:
        inputs = cache[inputs_key].get('inputs')
    if not inputs:
        messages = [{'role': 'system', 'content': INPUTS_SYSTEM_PROMPT},
                    {'role': 'user',
                     'content': inputs_user_prompt(plan, _dt(opt, 'inputs'),
                                                   driver_src)}]
        try:
            content, _usage = complete(messages)
        except Exception as error:
            if getattr(error, 'is_fn_budget', False):
                raise  # a budget cut is not a harness failure
            return None, None, 'the inputs request failed: %s' % error
        inputs = _parse_inputs(content, _dt(opt, 'inputs'))
        if inputs:
            cache[inputs_key] = {'body': content, 'inputs': inputs,
                                 'model': opt.model,
                                 'time': int(time.time())}
    if not inputs:
        return None, None, 'no usable test inputs'
    inputs = inputs + _edge_inputs(inputs)
    inputs = inputs[:_dt(opt, 'inputs') + 8]
    if scenario or not _dt(opt, 'scenario') or parts is None:
        return driver_path, inputs, None
    # ---- real-side pre-probe: the scenario ladder on a washout ----
    probe = _real_side_probe(opt, plan, info, parts, driver_path, inputs,
                             rundir, clang, kit)
    if probe is None:
        return driver_path, inputs, None
    usable, evidence, good = probe
    if usable:
        return driver_path, _inputs_first(inputs, good), None
    if kit is not None and getattr(kit, 'log', None) is not None:
        kit.log.info('%s: bare driver produced no usable real input;'
                     ' escalating to a scenario driver' % plan.name)
    return _obtain_scenario_artifacts(opt, plan, info, parts, complete,
                                      cache, rundir, clang, kit, evidence)


def _parse_inputs(content, want):
    text = _strip_fences(content.strip())
    start, end = text.find('['), text.rfind(']')
    if start < 0 or end <= start:
        return []
    try:
        data = json.loads(text[start:end + 1])
    except ValueError:
        return []
    result = []
    for entry in data if isinstance(data, list) else []:
        if not isinstance(entry, dict):
            continue
        argv = entry.get('argv')
        if isinstance(argv, list) and argv \
                and all(isinstance(t, (str, int, float)) for t in argv):
            result.append({'argv': [str(t) for t in argv],
                           'stdin': str(entry.get('stdin', ''))[:4096]})
        if len(result) >= want:
            break
    return result


def _edge_inputs(inputs):
    """deterministic edge variants of the model's own argv shapes."""
    extra = []
    seen = {tuple(entry['argv']) for entry in inputs}
    base = min(inputs, key=lambda entry: len(entry['argv']))
    for pos in range(min(2, len(base['argv']))):
        for token in DT_EDGE_TOKENS:
            argv = list(base['argv'])
            argv[pos] = token
            if tuple(argv) not in seen:
                seen.add(tuple(argv))
                extra.append({'argv': argv, 'stdin': ''})
    return extra[:6]
