###############################################################################
#
# IKOS API census
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
# PARTICULAR PURPOSE, OR FREEDOM FROM INFRINGEMENT, ANY WARRANTY THAT THE
# SUBJECT SOFTWARE WILL BE ERROR FREE, ANY WARRANTY THAT, IF PROVIDED, THE
# DOCUMENTATION WILL CONFORM TO THE SUBJECT SOFTWARE.  NO GOVERNMENT
# AGENCY, NOR ANY EMPLOYEE THEREOF, ASSUMES ANY LEGAL LIABILITY OR
# RESPONSIBILITY FOR THE ACCURACY, COMPLETENESS, OR USEFULNESS OF ANY
# INFORMATION, APPARATUS, PRODUCT, OR PROCESS DISCLOSED, OR REPRESENTS THAT
# ITS USE WOULD NOT INFRINGE PRIVATELY OWNED RIGHTS.
#
# Neither the United States Government, nor any employee thereof, makes any
# warranty, express or implied, including the warranty of merchantability,
# fitness for a particular purpose, or non-infringement of any kind, or
# assumes any legal liability for information, apparatus, product, or
# process disclosed, or represents that its use would necessarily not
# infringe privately owned rights.
#
# IN NO EVENT SHALL THE UNITED STATES GOVERNMENT OR ITS CONTRACTORS OR
# SUBCONTRACTORS BE LIABLE FOR ANY CLAIMS, DAMAGES OR OTHER LIABILITY,
# WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
# OR IN CONNECTION WITH THE SUBJECT SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SUBJECT SOFTWARE.
#
# Waiver and Indemnity:  RECIPIENT AGREES TO WAIVE AND AND RELEASE THE
# UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS, AND ANY
# EMPLOYEE THEREOF, FROM ANY AND ALL CLAIMS, DEMANDS OR DAMAGES, INCLUDING
# EXTRA COSTS THAT MAY ARISE FROM THE USE OF THIS SOFTWARE.
#
###############################################################################
"""Function and external API census for library-function spec research.

This module implements the ``ikos-api-census`` command line tool.  It counts,
purely at the LLVM IR level (no semantics, no call graph), the functions a
project defines and the external library APIs it calls, and classifies those
APIs against IKOS's builtin library-function name table:

* ``ir``    counts over compiled LLVM bitcode (.bc),
* ``names`` prints the builtin name table itself.

Every module is disassembled once with llvm-dis: ``define`` lines give the
project's functions, ``declare`` lines the calls the linker must resolve
elsewhere, and global ``alias``/``ifunc`` definitions count as defines (clang
emits e.g. a C1 constructor as an alias of C2 when the class has no virtual
bases).  Across a project's modules, external = declares - defines, so
functions living in another translation unit (or a linked-in static library)
count as internal.

The IR level is also the mounting level: glibc header redirections
(``_FILE_OFFSET_BITS=64`` ``fopen``->``fopen64``, ``__isoc99_*``,
``__isoc23_*``), fortify variants and macro expansions are already resolved,
so the census sees exactly the names the analyzer will see.

A called external function is *modeled* iff its name appears in the
``if (name == ...)`` chain of ``LibraryFunctionImporter::function()`` in
``frontend/llvm/src/import/library_function.cpp`` -- the exact mounting layer
any pre-built library function spec must align with.  Unmodeled names are the
to-do list for spec generation.

Unmodeled names are also reverse-mapped to their source-level spellings
(``fopen64``->``fopen``, ``__isoc99_fscanf``->``fscanf``, C++ mangled names
demangled): specs are written and man pages are looked up against source
names, not mounted names.

See doc/API_CENSUS.md for the full documentation.
"""
import argparse
import os
import os.path
import re
import shutil
import subprocess
import sys
from multiprocessing import Pool

from ikos import args
from ikos import colors
from ikos import log
from ikos import settings


#########################
# Library function table #
#########################

# path of library_function.cpp, relative to the ikos source tree root
LIBRARY_FUNCTION_CPP_REL = os.path.join('frontend',
                                        'llvm',
                                        'src',
                                        'import',
                                        'library_function.cpp')

# builtin table sections switch on these guards (NOT on comments: the libcpp
# block has no comment marker)
GUARD_RE = re.compile(r'this->_enable_(ikos|libc|libcpp)\b')
NAME_EQ_RE = re.compile(r'name == "([^"]+)"')


def load_known_apis(cpp_path):
    """Extract the builtin library function name table.

    Returns a dict {'ikos': set(names), 'libc': ..., 'libcpp': ...}.
    """
    with open(cpp_path, 'r') as f:
        src = f.read()
    table = {'ikos': set(), 'libc': set(), 'libcpp': set()}
    section = None
    for line in src.splitlines():
        guard = GUARD_RE.search(line)
        if guard:
            section = guard.group(1)
            continue
        for name in NAME_EQ_RE.findall(line):
            if section:
                table[section].add(name)
    return table


def library_function_cpp_candidates():
    """Candidate paths for library_function.cpp, best guess first."""
    # settings.SRC_ROOT is the analyzer source directory, baked in at
    # configure time; works from a source checkout, a build tree and the
    # installed venv copy alike.
    yield os.path.abspath(
        os.path.join(settings.SRC_ROOT, os.pardir, LIBRARY_FUNCTION_CPP_REL))
    # module-relative fallback, for a source checkout without settings
    yield os.path.abspath(
        os.path.join(os.path.dirname(__file__),
                     os.pardir,
                     os.pardir,
                     os.pardir,
                     LIBRARY_FUNCTION_CPP_REL))


def resolve_known_apis(names_from):
    """Load the builtin name table or die with an actionable message."""
    if names_from:
        candidates = [names_from]
    else:
        candidates = list(library_function_cpp_candidates())
    for path in candidates:
        if not os.path.isfile(path):
            continue
        try:
            return load_known_apis(path), path
        except OSError as e:
            log.warning('cannot read %s: %s' % (path, e))
    log.error('could not find %s' % LIBRARY_FUNCTION_CPP_REL)
    log.error('looked in: %s' % ', '.join(candidates))
    log.error('use --names-from <file> to specify its path')
    sys.exit(1)


def category_of(table):
    """Build a {name: category} dict from the name table."""
    return {name: category
            for category, names in table.items()
            for name in names}


####################
# Tool resolution #
####################

def find_llvm_dis():
    """Return the llvm-dis binary path, or None."""
    # prefer the llvm installation ikos was configured against, then PATH
    path = os.path.join(settings.LLVM_BIN_DIR, 'llvm-dis')
    if os.path.isfile(path) and os.access(path, os.X_OK):
        return path
    return shutil.which('llvm-dis')


#####################
# Worker machinery #
#####################

# resolved llvm-dis path, injected into workers through the Pool initializer
# (a library module must not resolve external tools at import time)
_LLVM_DIS = None


def _init_worker(llvm_dis):
    global _LLVM_DIS
    _LLVM_DIS = llvm_dis


def chunked(seq, size):
    return [seq[i:i + size] for i in range(0, len(seq), size)]


################
# IR census #
################

BITCODE_EXTENSIONS = {'.bc'}

# only categorically-irrelevant directories are skipped: unlike a source
# census, taking the union of declares/defines over every module cannot
# double-count, and build trees (build/, CMakeFiles/, ...) are exactly where
# cmake-style projects keep their bitcode.  textual .ll files are NOT
# collected: in trees that have them they are checked-in test IR (e.g. llvm's
# own llvm/test/), not build products.
EXCLUDE_DIRS = {'.git', 'node_modules'}

# first `@name` on a textual-IR define/declare/alias line; quoted names may
# contain spaces or escaped bytes (`@"\01_fopen"`)
DECLARE_NAME_RE = re.compile(r'@(?:\"([^\"]*)\"|([^\s(]+))')

# llvm intrinsics and compiler/analysis runtime hooks are resolved before the
# library function importer ever sees a call -- they are not library APIs of
# the analyzed program and would only pollute the unmodeled to-do list
INSTRUMENTATION_PREFIXES = ('llvm.',
                            '__sanitizer_cov_',
                            '__asan_',
                            '__msan_',
                            '__tsan_',
                            '__ubsan_',
                            '__llvm_')


def find_bitcode(root):
    """Every .bc/.ll file under root, skipping build/vendor directories."""
    out = []
    for directory, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
        for filename in files:
            ext = os.path.splitext(filename)[1]
            if ext in BITCODE_EXTENSIONS:
                out.append(os.path.join(directory, filename))
    return out


def collect_function_name(line, names):
    """Add the function name of one textual-IR define/declare line."""
    match = DECLARE_NAME_RE.search(line)
    if not match:
        return
    name = match.group(1)
    if name is None:
        name = match.group(2)
    if name.startswith(INSTRUMENTATION_PREFIXES):
        return
    # the textual form `"\01foo"` escapes the interceptor prefix byte that
    # the builtin name table spells `"\x01foo"`
    if name.startswith('\\01'):
        name = '\\x01' + name[3:]
    names.add(name)


def ir_lines(path):
    """Iterate the textual-IR lines of one bitcode file.

    llvm-dis writes to stdout, so nothing larger than one line is ever held
    in memory.
    """
    try:
        proc = subprocess.Popen([_LLVM_DIS, path, '-o', '-'],
                                stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL,
                                universal_newlines=True,
                                errors='replace')
    except OSError as e:
        log.warning('could not run %s: %s' % (_LLVM_DIS, e))
        return
    for line in proc.stdout:
        yield line
    proc.stdout.close()
    if proc.wait() != 0:
        log.warning('%s: llvm-dis failed' % path)


def disassemble_names(path):
    """(defined, declared) function names in one bitcode file."""
    defines = set()
    declares = set()
    for line in ir_lines(path):
        if line.startswith('define'):
            collect_function_name(line, defines)
        elif line.startswith('declare'):
            collect_function_name(line, declares)
        elif line.startswith('@') and (' alias ' in line or
                                       ' ifunc ' in line):
            # a global alias/ifunc defines its symbol without a `define`
            # line -- e.g. clang emits a C1 constructor as an alias of C2
            # when the class has no virtual bases
            collect_function_name(line, defines)
    return defines, declares


def ir_chunk(paths):
    """Collect defined/declared function names from a chunk of files."""
    defines = set()
    declares = set()
    for path in paths:
        path_defines, path_declares = disassemble_names(path)
        defines |= path_defines
        declares |= path_declares
    return defines, declares


def scan_ir(files, pool):
    """IR census of every bitcode file of a project."""
    defines = set()
    declares = set()
    for batch_defines, batch_declares in pool.map(ir_chunk,
                                                  chunked(files, 32)):
        defines |= batch_defines
        declares |= batch_declares
    return {
        'bc_files': len(files),
        'functions': len(defines),
        # a declare that another module defines is a call into the rest of
        # the project (another TU, a linked-in static library), not a
        # library API
        'external': sorted(declares - defines),
    }


############################
# IR name -> source name #
############################

# The IR census reports mounted names (what LibraryFunctionImporter sees);
# specs and man pages are written against source-level spellings.  These
# rules reverse the renames glibc headers and the compiler apply between
# source and bitcode.  Ordered: exact table first, then one-shot patterns.

# (source_name, rule) for names whose source form is not derivable by a
# suffix/prefix rule
SPECIAL_SOURCE_NAMES = {
    '__ctype_b_loc': ('is*', 'ctype-table'),
    '__ctype_tolower_loc': ('tolower', 'ctype-table'),
    '__ctype_toupper_loc': ('toupper', 'ctype-table'),
    '__errno_location': ('errno', 'errno-var'),
    '__assert_fail': ('assert', 'assert-macro'),
    '__xpg_basename': ('basename', 'xpg'),
    '__fdelt_chk': ('FD_SET/FD_CLR', 'fdset-macro'),
}

ISOC99_RE = re.compile(r'__isoc99_(\w+)')
ISOC23_RE = re.compile(r'__isoc23_(\w+)')
FORTIFY_RE = re.compile(r'__(\w+)_chk')
LFS64_RE = re.compile(r'(.{2,})64')


def find_llvm_cxxfilt():
    """Return a demangler binary path (llvm-cxxfilt or c++filt), or None."""
    candidates = [os.path.join(settings.LLVM_BIN_DIR, 'llvm-cxxfilt'),
                  shutil.which('llvm-cxxfilt'),
                  shutil.which('c++filt')]
    for path in candidates:
        if path and os.path.isfile(path) and os.access(path, os.X_OK):
            return path
    return None


def demangle_names(names):
    """Demangle C++ names in one llvm-cxxfilt run; failures stay as-is.

    Returns {mangled: demangled}; names cxxfilt cannot demangle come back
    unchanged, which is what we want.
    """
    tool = find_llvm_cxxfilt()
    if not tool or not names:
        return {}
    try:
        proc = subprocess.run([tool, '-n'],
                              input='\n'.join(names),
                              stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL,
                              universal_newlines=True,
                              errors='replace')
    except OSError as e:
        log.warning('could not run %s: %s' % (tool, e))
        return {}
    out = proc.stdout.splitlines()
    if len(out) != len(names):
        return {}
    return dict(zip(names, out))


def reverse_source_names(names):
    """{ir_name: (source_name, rule)} for a collection of IR names.

    rule says how the source spelling was recovered ('' = identical).
    """
    demangled = demangle_names(
        [n for n in names if n.startswith(('_Z', '__Z'))])
    out = {}
    for name in names:
        if name in SPECIAL_SOURCE_NAMES:
            out[name] = SPECIAL_SOURCE_NAMES[name]
        elif name in demangled:
            out[name] = (demangled[name], 'cxx-mangled')
        elif name.startswith('\\x01'):
            # interceptor prefix (asan-style); strip "\x01" and one '_'
            rest = name[4:]
            if rest.startswith('_'):
                rest = rest[1:]
            out[name] = (rest or name, 'interceptor')
        else:
            for regex, rule in ((ISOC99_RE, 'isoc99'),
                                (ISOC23_RE, 'isoc23'),
                                (FORTIFY_RE, 'fortify'),
                                (LFS64_RE, 'lfs64')):
                match = regex.fullmatch(name)
                if match:
                    out[name] = (match.group(1), rule)
                    break
            else:
                out[name] = (name, '')
    return out


###########
# Output #
###########

def project_name(root):
    return os.path.basename(os.path.normpath(os.path.abspath(root)))


def check_roots(roots):
    """Check directories exist and have unique basenames, or die."""
    seen = {}
    for root in roots:
        if not os.path.isdir(root):
            log.error('%s: not a directory' % root)
            sys.exit(1)
        name = project_name(root)
        if name in seen:
            log.error("duplicate project name '%s':" % name)
            log.error('  %s' % seen[name])
            log.error('  %s' % root)
            log.error('result files are keyed by directory name;'
                      ' split the run or use --output-dir')
            sys.exit(1)
        seen[name] = root


def write_ir_results(rows, known, output_dir):
    categories = category_of(known)
    tsv = os.path.join(output_dir, 'ir-api-census-results.tsv')
    outdir = os.path.join(output_dir, 'ir-api-census-results')
    os.makedirs(outdir, exist_ok=True)
    with open(tsv, 'w') as tsv_file:
        tsv_file.write('project\tbc_files\tfunctions\t'
                       'external_functions\tmodeled\tunmodeled\n')
        for name, data in rows:
            external = data['external']
            modeled = [n for n in external if n in categories]
            unmodeled = [n for n in external if n not in categories]
            tsv_file.write('%s\t%d\t%d\t%d\t%d\t%d\n'
                           % (name,
                              data['bc_files'],
                              data['functions'],
                              len(external),
                              len(modeled),
                              len(unmodeled)))
            with open(os.path.join(outdir, '%s.external.txt' % name),
                      'w') as list_file:
                list_file.write('\n'.join(external) + '\n')
            with open(os.path.join(outdir, '%s.modeled.txt' % name),
                      'w') as list_file:
                for n in modeled:
                    list_file.write('%s\t%s\n'
                                    % (categories.get(n, '?'), n))
            with open(os.path.join(outdir, '%s.unmodeled.txt' % name),
                      'w') as list_file:
                list_file.write('\n'.join(unmodeled) + '\n')
            # reverse mapping back to source-level spellings, for writing
            # specs against man pages instead of mounted names
            source_names = reverse_source_names(unmodeled)
            with open(os.path.join(outdir,
                                   '%s.unmodeled.source.tsv' % name),
                      'w') as list_file:
                for n in unmodeled:
                    source, rule = source_names[n]
                    list_file.write('%s\t%s\t%s\n' % (n, source, rule))
    return tsv, outdir


def print_ir_table(rows, known):
    categories = category_of(known)
    print('| project | bc files | functions | external APIs | modeled |'
          ' unmodeled |')
    print('| --- | --- | --- | --- | --- | --- |')
    total_functions = 0
    total_apis = 0
    for name, data in rows:
        external = data['external']
        modeled = len([n for n in external if n in categories])
        total_functions += data['functions']
        total_apis += len(external)
        print('| %s | %d | %d | %d | %d | %d |'
              % (name,
                 data['bc_files'],
                 data['functions'],
                 len(external),
                 modeled,
                 len(external) - modeled))
    print('| **total** (projects may overlap) | | **%d** | **%d** | | |'
          % (total_functions, total_apis))


##################
# Main for ir #
##################

def ir_census(opt, known):
    llvm_dis = find_llvm_dis()
    if not llvm_dis:
        log.error('could not find llvm-dis (looked in: %s and PATH)'
                  % settings.LLVM_BIN_DIR)
        log.error('llvm-dis ships with the llvm installation ikos was'
                  ' configured against')
        sys.exit(1)
    log.info('using llvm-dis: %s' % llvm_dis)

    roots = opt.dirs
    check_roots(roots)

    rows = []
    with Pool(processes=opt.jobs,
              initializer=_init_worker,
              initargs=(llvm_dis,)) as pool:
        for root in roots:
            files = find_bitcode(root)
            if not files:
                log.warning('%s: no bitcode found (build with ikos-scan or'
                            ' clang -emit-llvm first); skipped' % root)
                continue
            data = scan_ir(files, pool)
            rows.append((project_name(root), data))
            log.info('%s: %d bitcode files, %d functions, %d external APIs'
                     % (project_name(root),
                        data['bc_files'],
                        data['functions'],
                        len(data['external'])))
    if not rows:
        log.error('nothing to scan')
        sys.exit(1)

    rows.sort(key=lambda item: (-item[1]['functions'], item[0]))
    tsv, outdir = write_ir_results(rows, known, opt.output_dir)
    print_ir_table(rows, known)
    log.info('written: %s and %s/<project>.'
             '{external,modeled,unmodeled}.txt and'
             ' <project>.unmodeled.source.tsv' % (tsv, outdir))


def names_census(opt):
    known, path = resolve_known_apis(opt.names_from)
    for category in ('ikos', 'libc', 'libcpp'):
        for name in sorted(known[category]):
            print('%s\t%s' % (category, name))
    log.info('total: %d names (ikos %d, libc %d, libcpp %d), from %s'
             % (sum(len(v) for v in known.values()),
                len(known['ikos']),
                len(known['libc']),
                len(known['libcpp']),
                path))


########################
# Command line parsing #
########################

def add_common_arguments(parser):
    parser.add_argument('-v',
                        dest='verbosity',
                        help='Increase verbosity',
                        action='count',
                        default=1)
    parser.add_argument('-q',
                        dest='verbosity',
                        help='Be quiet',
                        action='store_const',
                        const=0)
    parser.add_argument('--version',
                        action=args.VersionAction,
                        nargs=0,
                        help='Show ikos version')
    parser.add_argument('--color',
                        dest='color',
                        metavar='',
                        help=args.help('Enable terminal colors:',
                                       args.color_choices,
                                       args.default_color),
                        choices=args.choices(args.color_choices),
                        default=args.default_color)
    parser.add_argument('--log',
                        dest='log_level',
                        metavar='',
                        help=args.help('Log level:',
                                       args.log_levels,
                                       args.default_log_level),
                        choices=args.choices(args.log_levels),
                        default=None)


def add_census_arguments(parser):
    parser.add_argument('dirs',
                        nargs='+',
                        metavar='dir',
                        help='Project directory')
    parser.add_argument('-o',
                        '--output-dir',
                        dest='output_dir',
                        metavar='<dir>',
                        default='.',
                        help='Output directory')
    parser.add_argument('-j',
                        '--jobs',
                        dest='jobs',
                        type=args.Integer(min=1),
                        default=min(16, os.cpu_count() or 4),
                        help='Number of parallel jobs')
    parser.add_argument('--names-from',
                        dest='names_from',
                        metavar='<file>',
                        help='Path to library_function.cpp'
                             ' (default: auto-detect)')


def api_census_parse_arguments(argv):
    usage = '%(prog)s [options] <command> ...'
    description = ('Count functions and external library APIs over LLVM IR,'
                   ' and classify them against the ikos builtin library'
                   ' function table.')
    parser = argparse.ArgumentParser(usage=usage,
                                     description=description,
                                     formatter_class=argparse
                                     .RawTextHelpFormatter)
    parser.add_argument('--version',
                        action=args.VersionAction,
                        nargs=0,
                        help='Show ikos version')

    subparsers = parser.add_subparsers(dest='command', required=True)

    ir_parser = subparsers.add_parser(
        'ir',
        help='count over compiled LLVM bitcode',
        description='Count functions and external APIs over compiled LLVM'
                    ' bitcode (.bc) found in each directory -- including'
                    ' build trees, where cmake-style projects keep their'
                    ' bitcode -- disassembled with llvm-dis. define lines'
                    ' give the project functions, declare lines the external'
                    ' calls, alias/ifunc definitions count as defines;'
                    ' external = declares - defines across the whole'
                    ' tree. IR is the mounting level: glibc redirections'
                    ' (fopen64, __isoc99_*), fortify variants and macro'
                    ' expansions are already resolved, so the census sees'
                    ' exactly the names the analyzer sees.')
    ir_parser.set_defaults(census=ir_census)
    add_census_arguments(ir_parser)
    add_common_arguments(ir_parser)

    names_parser = subparsers.add_parser(
        'names',
        help='print the builtin library function name table',
        description='Print the builtin library function name table that'
                    ' ikos matches function names against'
                    ' (one category/name pair per line).')
    names_parser.set_defaults(census=names_census)
    names_parser.add_argument('--names-from',
                              dest='names_from',
                              metavar='<file>',
                              help='Path to library_function.cpp'
                                   ' (default: auto-detect)')
    add_common_arguments(names_parser)

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


def api_census_main(argv):
    # parse arguments
    opt = api_census_parse_arguments(argv[1:])

    # setup colors and logging
    colors.setup(opt.color, file=log.out)
    log.setup(opt.log_level)

    if opt.census is not names_census:
        # the classification table is required; fail early rather than
        # mislabel every API as unmodeled
        known, path = resolve_known_apis(opt.names_from)
        log.info('builtin name table: %d names from %s'
                 % (sum(len(v) for v in known.values()), path))
        opt.census(opt, known)
    else:
        opt.census(opt)


if __name__ == '__main__':
    api_census_main(sys.argv)
