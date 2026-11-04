"""Real-library resolution for the differential gate's real side.

The battery's real binary must link the REAL implementation of the
function under test.  Historically that meant implicit libc plus -lm and
nothing else, so every non-libc external gate-skipped and its stubs
shipped unverified.  This module turns "where is the real
implementation?" into data:

  * map files -- TSV `symbol<TAB>link args`, '#' comments; empty args
    mean the symbol is linked implicitly (libc/libm).  Ordered lists of
    maps give priority: earlier files win.
  * the in-tree scan -- native .a/.so found in the project's own tree
    are the exact versions the bitcode was built against; .a is
    preferred (no runtime lookup), .so is linked by absolute real path
    plus an rpath so the loader does not fall back to the system copy.
  * the system map -- a configured, ordered list of dependency libraries
    (real-libs.conf) resolved through the ldconfig cache to absolute
    paths; version-skewed versus the build container but perfectly
    capable of exposing unsound stub semantics.
  * libc/libm are always resolvable: with no maps at all, the loader
    nms the ldconfig-resolved libc.so.6/libm.so.6, so the strict gate
    (skip functions with no real implementation) is the zero-config
    default.

Nothing here calls an LLM or links a driver: the module only produces
and consumes plain data plus `nm`, keeping every decision reproducible
and auditable by hand.
"""

import os
import re
import shlex
import subprocess

# nm types that denote a linkable definition: global text, weak, IFUNC
NM_TYPES = ('T', 'W', 'i')

# directories never worth scanning for native libraries
SKIP_DIRS = {'.git', 'stubs', 'experiment-results', 'ablation'}

_SO_RE = re.compile(r'\.so(\.\d+)*$')
_C_IDENT_RE = re.compile(r'^[A-Za-z_]\w*$')


def _silent(_message):
    pass


#############################
# Map files: load / lookup #
#############################

def load_map(paths, warn=None):
    """{symbol: [link args]} from an ordered list of map files.

    Earlier files win (in-tree before system); unreadable files warn and
    are skipped -- a missing map degrades toward the honest no-real-lib
    skip, never toward an unverified stub.
    """
    warn = warn or _silent
    table = {}
    for path in paths or []:
        try:
            with open(path) as stream:
                lines = stream.read().splitlines()
        except OSError as error:
            warn('real-libs: cannot read map %s: %s' % (path, error))
            continue
        for line in lines:
            if not line.strip() or line.lstrip().startswith('#'):
                continue
            fields = line.split('\t', 1)
            symbol = fields[0].strip()
            rest = fields[1].strip() if len(fields) > 1 else ''
            args = shlex.split(rest) if rest else []
            if symbol and symbol not in table:
                table[symbol] = args
    return table


def lookup(table, plan):
    """(symbol, link args) for a stub plan, or None when unresolvable.

    The IR name comes first: it is exactly what nm reports and what the
    real library exports (C++ symbols stay mangled -- the demangled
    source spelling is not a linkable C name).  The source spelling only
    serves renamed libc calls whose map was written by source name.
    """
    if not table:
        return None
    for name in (plan.name, plan.source_name):
        if name in table:
            return name, list(table[name])
    return None


def resolve(opt, log=None):
    """The full resolution table for one generate run.

    Explicit --real-libs maps in order, then the implicit libc/libm set
    (nm of the ldconfig-resolved libc.so.6/libm.so.6) so the strict gate
    holds with zero configuration.  -> dict; on total resolution failure
    an empty table comes back and everything non-libc reads as
    no-real-lib (fail closed, loudly).
    """
    warn = log.warning if log else _silent
    table = load_map(getattr(opt, 'real_libs', None), warn)
    if table:
        log.info('real-lib maps: %d symbol(s) from %s'
                 % (len(table), ', '.join(getattr(opt, 'real_libs'))))
    cache = ldconfig_cache()
    implicit = {}
    for soname in ('libc.so.6', 'libm.so.6'):
        path = cache.get(soname)
        if path is None:
            warn('real-libs: %s not in the ldconfig cache; the strict gate'
                 ' degrades to no-real-lib for everything else' % soname)
            continue
        try:
            implicit.update((sym, [])
                            for sym in nm_symbols(path, dynamic=True))
        except OSError as error:
            warn('real-libs: cannot nm %s: %s' % (path, error))
    log.info('real-libs: implicit libc/libm set: %d symbol(s)'
             % len(implicit))
    for symbol, args in implicit.items():
        table.setdefault(symbol, args)
    return table


#####################
# Symbol indexing #
#####################

def nm_symbols(path, dynamic):
    """Defined linkable symbols of one archive or shared object.

    Archives have no dynamic symbol table (plain `nm`), shared objects
    are indexed through `nm -D`.  Version suffixes are stripped and
    names stay raw -- mangled C++ spellings are the linkable ones.
    Raises OSError when nm itself cannot run; a non-zero nm exit (an
    LLVM bitcode archive, a stripped object) yields an empty set.
    """
    cmd = ['nm']
    if dynamic:
        cmd.append('-D')
    cmd += ['--defined-only', path]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE,
                          universal_newlines=True, errors='replace')
    if proc.returncode != 0:
        return set()
    symbols = set()
    for line in proc.stdout.splitlines():
        fields = line.split()
        # `0000000000000000 T LZ4_compress_HC` / `... i memcpy`;
        # archive member headers (`lib.a(foo.o):`) never match this shape
        if len(fields) >= 3 and re.match(r'^[0-9A-Fa-f]+$', fields[0]) \
                and fields[1] in NM_TYPES:
            symbols.add(fields[2].split('@')[0])
    return symbols


def is_shared(path):
    return bool(_SO_RE.search(path))


def so_link_args(path):
    """Link a shared object by its absolute real path plus an rpath.

    Linking the real file pins the version the map was generated from;
    without the rpath the loader would resolve the embedded SONAME
    against the system copy at runtime -- compile-time in-tree and
    runtime system is the worst of both.
    """
    real = os.path.realpath(path)
    return [real, '-Wl,-rpath,' + os.path.dirname(real)]


def archive_link_args(path):
    """Static archives link by absolute path with no runtime lookup."""
    return [os.path.realpath(path)]


def link_args_for(path):
    if is_shared(path):
        return so_link_args(path)
    return archive_link_args(path)


#############################
# In-tree project scanning #
#############################

def scan_project(directory, warn=None):
    """{symbol: [link args]} over the native libraries of a project tree.

    Ordering is the priority: .a before .so (static archives carry no
    runtime lookup), then shallower paths, then lexicographic -- all
    deterministic so a rescan reproduces the same map.  A symbol found
    in several libraries resolves to the first and is reported once.
    """
    warn = warn or _silent
    candidates = []
    for root, dirs, files in os.walk(directory):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
        for fname in sorted(files):
            if fname.endswith('.a') or _SO_RE.search(fname):
                candidates.append(os.path.join(root, fname))
    seen = set()
    unique = []
    for path in candidates:
        real = os.path.realpath(path)
        if real not in seen:
            seen.add(real)
            unique.append(path)
    ordered = sorted(unique,
                     key=lambda p: (0 if p.endswith('.a') else 1,
                                    p.count(os.sep), p))
    table = {}
    duplicates = []
    for path in ordered:
        try:
            symbols = nm_symbols(path, dynamic=is_shared(path))
        except OSError as error:
            warn('real-libs: cannot nm %s: %s' % (path, error))
            continue
        args = link_args_for(path)
        for symbol in sorted(symbols):
            if symbol in table:
                if not duplicates:
                    duplicates.append(symbol)
                continue
            table[symbol] = list(args)
    if duplicates:
        warn('real-libs: %d symbol(s) (e.g. %s) defined by more than one'
             ' in-tree library; the higher-priority one wins'
             % (len(duplicates), ', '.join(duplicates[:3])))
    return table


#############################
# System dependency layer #
#############################

def ldconfig_cache():
    """{soname: absolute path} from the loader cache (x86-64 preferred)."""
    table = {}
    try:
        proc = subprocess.run(['ldconfig', '-p'], stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL,
                              universal_newlines=True, errors='replace',
                              timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return table
    for line in proc.stdout.splitlines():
        match = re.match(r'\s*(\S+)\s+\(([^)]*)\)\s+=>\s+(\S+)', line)
        if not match:
            continue
        soname, arch, path = match.groups()
        if soname in table and 'x86-64' not in arch:
            continue
        table[soname] = path
    return table


def parse_config(path):
    """[(label, soname-or-path, args)] from a dependency config.

    Lines are `label<TAB>target[<TAB>args]`; `-` as args means the
    symbol is already linked implicitly (libc, libm).  '#' comments.
    """
    entries = []
    with open(path) as stream:
        for line in stream.read().splitlines():
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            fields = [field.strip() for field in line.split('\t')]
            if len(fields) < 2 or not fields[1]:
                continue
            label, target = fields[0], fields[1]
            extra = fields[2] if len(fields) > 2 else ''
            if extra in ('', '-'):
                args = []
            else:
                args = shlex.split(extra)
            entries.append((label, target, args, extra == '-'))
    return entries


def build_system_map(config_path, warn=None):
    """{symbol: [link args]} over the configured dependency libraries.

    Config order is priority (first lib to define a symbol wins).
    Libraries resolve through the ldconfig cache to an absolute path --
    no -l flags, no dependence on -dev symlinks, the version is pinned
    by the map itself.
    """
    warn = warn or _silent
    cache = ldconfig_cache()
    table = {}
    for label, target, extra, implicit in parse_config(config_path):
        path = target
        if not (os.path.isabs(target) and os.path.exists(target)):
            path = cache.get(target)
        if path is None:
            warn('real-libs: %s (%s) not found; its functions read as'
                 ' no-real-lib until it is installed or the config is'
                 ' fixed' % (target, label))
            continue
        if implicit:
            args = []
        elif extra:
            args = list(extra)
        else:
            args = so_link_args(path)
        try:
            symbols = nm_symbols(path, dynamic=True)
        except OSError as error:
            warn('real-libs: cannot nm %s: %s' % (path, error))
            continue
        for symbol in sorted(symbols):
            if symbol not in table:
                table[symbol] = list(args)
    return table


#####################
# Map persistence #
#####################

def write_map(path, table, provenance=()):
    """Write a map atomically; args are shlex-quoted to round-trip."""
    lines = ['# ' + note for note in provenance]
    lines += ['%s\t%s' % (symbol, ' '.join(shlex.quote(arg)
                                           for arg in table[symbol]))
              for symbol in sorted(table)]
    tmp = path + '.tmp'
    with open(tmp, 'w') as stream:
        stream.write('\n'.join(lines) + '\n')
    os.replace(tmp, path)


#############################
# Linkability probe #
#############################

def probe_source(symbol):
    """C source whose main stores the symbol's address into a volatile
    sink.

    No prototype is imposed and nothing is called, so the probe is
    immune to ABI/signature questions.  The sink must be volatile: a
    plain `&sym == 0` is constant-folded by the compiler (a function
    address is never null) and emits no reference at all, which made
    unresolvable symbols probe "ok".
    """
    if _C_IDENT_RE.match(symbol):
        decl = 'extern void %s(void);' % symbol
        ref = symbol
    else:
        label = symbol.replace('\\', '\\\\').replace('"', '\\"')
        decl = ('extern void ikos_real_probe_sym(void)'
                ' __asm__("%s");' % label)
        ref = 'ikos_real_probe_sym'
    return ('%s\nvoid *volatile ikos_probe_sink;\n'
            'int main(void) { ikos_probe_sink = &%s; return 0; }\n'
            % (decl, ref))


def probe_link(clang, symbol, args, rundir, timeout=10):
    """Compile, link AND run the address-only probe for one symbol.

    -> (ok, detail).  The run step is what surfaces rpath/DT_NEEDED gaps
    that a pure link accepts; `main` returns 0 on a healthy probe (the
    address is taken, never dereferenced), anything else is a failure.
    """
    src = os.path.join(rundir, 'probe.c')
    binary = os.path.join(rundir, 'probe.bin')
    with open(src, 'w') as stream:
        stream.write(probe_source(symbol))
    cmd = [clang, '-O0', src, '-o', binary, '-lm'] + list(args or [])
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE,
                              universal_newlines=True, errors='replace')
    except OSError as error:
        return False, 'clang failed to run: %s' % error
    if proc.returncode != 0:
        # the informative line (`undefined reference to ...`) sits above
        # clang's own error summary: keep the last few lines
        tail = [line for line in proc.stderr.strip().splitlines()
                if line.strip()][-3:]
        return False, 'link failed: %s' % ' | '.join(tail)
    try:
        run = subprocess.run([binary], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE,
                             universal_newlines=True, errors='replace',
                             timeout=timeout)
    except subprocess.TimeoutExpired:
        return False, 'probe timed out after %ss (loader or constructors)' \
                      % timeout
    except OSError as error:
        return False, 'probe could not run: %s' % error
    if run.returncode != 0:
        return False, ('probe exited with %d: the library loads but its'
                       ' environment is unusable' % run.returncode)
    return True, 'linked and ran'
