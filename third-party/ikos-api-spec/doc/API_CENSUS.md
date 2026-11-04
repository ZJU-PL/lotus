# IKOS API Census

`ikos-api-census` is a lightweight IR-level census tool: it disassembles the
project's compiled LLVM bitcode (`.bc`) with `llvm-dis` and counts how many
functions the project **defines** and **which external library functions it
declares**, then classifies each external function against IKOS's builtin
modeling layer as **modeled** or **unmodeled**. It performs no semantic
analysis, builds no call graph, and counts neither call sites nor call
frequencies.

It targets the pre-study stage of "library-function spec generation": the
**unmodeled list is the generation todo list**, and sorting it by the number of
projects that reference each name yields the modeling priority.

## Why at the IR level

IR is precisely the **mounting layer** — the names the census sees are the names
the analyzer will see:

- glibc's preprocessor rename/redirect has already happened: the LFS variants
  under `_FILE_OFFSET_BITS=64` (`fopen`→`fopen64`, `stat`→`stat64`), the
  C99/C23 binary-compatibility aliases (`fscanf`→`__isoc99_fscanf`,
  `strtol`→`__isoc23_strtol`), the fortify variants (`__memcpy_chk`), and macro
  expansions (`assert`→`__assert_fail`, `isdigit`→`__ctype_b_loc`,
  `errno`→`__errno_location`). The source-name/mounted-name mismatch does not
  exist at the IR level;
- no source-parsing blind spots: ctags/regex are misled by same-named macros or
  struct members (curl's `#define free curl_dbg_free` removes `free` from a
  source list); the IR-level `declare` is the compiler's final word;
- fast: it only runs `llvm-dis` (curl's 205 bitcode files take ~0.5 s), and does
  not depend on ctags.

The cost is needing bitcode up front: build normally with `ikos-scan`, compile
manually with `clang -emit-llvm -c`, or use existing OSS-Fuzz artifacts.

## Two subcommands

```sh
ikos-api-census ir [-o <dir>] [-j <n>] [--names-from <file>] <dir>...
ikos-api-census names [--names-from <file>]
```

| Subcommand | Role |
| --- | --- |
| `ir` | LLVM IR (`.bc`) census: `llvm-dis` to read `define`/`declare` |
| `names` | print IKOS's builtin library-function name table itself |

## Builtin name table and classification semantics

IKOS's builtin library-function modeling is mounted at the LLVM layer:
`LibraryFunctionImporter::function()` (`frontend/llvm/src/import/library_function.cpp`)
matches a hardcoded table by function **name** through an `if (name == "...")`
chain; a hit is converted into a builtin declaration such as `ar.libc.*`, whose
semantics the analyzer's `exec_intrinsic_call` provides.

`ikos-api-census`'s classification rule aligns with that:

> A called external function whose name is in the table = **modeled** (the
> analyzer hits it and applies builtin semantics); otherwise = **unmodeled**
> (spec-generation todo).

Current table size: **ikos 15 + libc 59 + libcpp 11 = 85 names**. Table entries
worth noting:

- interceptor variants such as `\x01_fopen` (control-byte prefix, extracted
  verbatim; the escaped textual-IR spelling `@"\01_fopen"` is normalized to the
  same form before comparison);
- fortify variants (`__memcpy_chk`) and `__isoc99_fscanf` — these appear in
  bitcode under their real names;
- blocks must be split by the `_enable_ikos/libc/libcpp` guards, not by
  comments: the libcpp block (`_Znwm`, `__cxa_*`, etc.) carries no comment
  marker.

Name-table resolution order: `--names-from` explicitly → the source-tree path
derived from `settings.SRC_ROOT` (hits the source tree / build tree / installed
copy alike) → module-relative path → **hard error**. Failing is preferred to
silently mislabeling every external function as unmodeled. If the install was
moved and the source tree deleted, point `--names-from` at a copy of
`library_function.cpp`.

After editing `library_function.cpp` in the ikos tree, re-running
`ikos-api-census names` resynchronizes automatically — no list is maintained by
hand. `names` stdout is a `category<TAB>name` table; the totals row at the end
goes to the log (also stdout by default); add `-q` for clean piped output.

## `ikos-api-census ir`: the IR-level census

It recursively collects `.bc` under each project directory (**only** `.git` and
`node_modules` are excluded) and runs `llvm-dis` on each module:

- **build trees must be included**: cmake-style projects keep their bitcode in
  `build/`/`CMakeFiles/` (all 1108 of opencv's `.bc` files); declare/define
  unioning has no double-count issue, so there is no reason to skip any build
  directory;
- **`.ll` is not collected**: `.ll` in corpora (e.g. llvm's own `llvm/test/`,
  nearly 30k files) is checked-in test IR, not build output — including it only
  pollutes the census; real output is always `.bc`. Convert with `llvm-as`
  first if needed.

- **`define` lines** = functions this module defines → project functions;
- **`declare` lines** = functions declared but not defined in this module →
  calls resolved externally at link time;
- **global `alias`/`ifunc` definition lines** = also count as in-project
  definitions: clang generates the C1 constructor of a class without virtual
  bases as an alias of C2 (`@...C1E... = alias ... @...C2E...`); that line is
  neither `define` nor `declare`, and not counting it would mislabel the
  `declare ...C1E...` in caller TUs as external;
- after unioning the whole tree, **external = declares − defines**: functions
  defined in another TU (or a linked-in static library) — e.g. the `BZ2_*`
  family that bzip2's tool TUs declare but `libbz2` provides — automatically
  count as in-project and stay out of the external list. Per-TU `.o.bc` and
  linked whole-program `.bc` are equivalent under union semantics.

Filtering rules:

- `llvm.*` intrinsics and compiler/instrumentation-runtime hooks
  (`__sanitizer_cov_*`, `__asan_*`, `__ubsan_*`, `__llvm_*`, etc.) — they are
  resolved inside the LLVM frontend or the analysis runtime, never go through
  `LibraryFunctionImporter`, and are not the program-under-test's library API;
  including them only pollutes the unmodeled todo list;
- the escaped textual-IR name `@"\01_fopen"` is normalized to the table's
  verbatim spelling `\x01_fopen`.

**Output** (current directory by default, `-o` to change):

- `ir-api-census-results.tsv`: one row per project
  (project/bc_files/functions/external_functions/modeled/unmodeled);
- `ir-api-census-results/<project>.external.txt`: all called externals (names
  only);
- `<project>.modeled.txt`: `category<TAB>name` (category is ikos/libc/libcpp);
- `<project>.unmodeled.txt`: the unmodeled list (names only, one per line) —
  the spec-generation todo list;
- `<project>.unmodeled.source.tsv`: the source-level spelling of each
  unmodeled name, `ir name<TAB>source name<TAB>rule` (see below).

```console
$ ikos-api-census ir /path/to/bzip2 /path/to/sqlite
[*] bzip2: 11 bitcode files, 118 functions, 41 external APIs
[*] sqlite: 11 bitcode files, 3626 functions, 145 external APIs
| project | bc files | functions | external APIs | modeled | unmodeled |
| --- | --- | --- | --- | --- | --- |
| sqlite | 11 | 3626 | 145 | 27 | 118 |
| bzip2 | 11 | 118 | 41 | 19 | 22 |
```

The bzip2 example shows the IR-level semantics: `fopen64`/`stat64`/`open64`/
`lstat64` enter the unmodeled list under their real mounted names (in source
they are `fopen`/`stat`/`open`/`lstat`, and `fopen` itself is in the table —
the versioned variants are precisely the builtin table's real coverage gap);
`__errno_location` and `__ctype_b_loc` also appear under their real names, with
the former counted modeled (the source layer never sees errno's function form
at all).

External dependency: only `llvm-dis` (from the LLVM bin directory recorded at
ikos configure time, PATH fallback; hard error if not found). Projects with no
bitcode are skipped with a warning.

## IR name → source name reverse mapping

The unmodeled list holds **mounted names** (real symbols in bitcode), but the
spec must be written in source spelling and man pages are looked up by source
name. `ir` therefore reverse-maps every unmodeled name and emits
`<project>.unmodeled.source.tsv`. Rules are tried in order; the first hit wins:

| Rule | Shape | Example |
| --- | --- | --- |
| literal table | fixed names not derivable by prefix/suffix | `__assert_fail`→`assert`, `__errno_location`→`errno`, `__ctype_b_loc`→`is*` |
| `cxx-mangled` | `_Z`/`__Z` prefix, batch-demangled with llvm-cxxfilt | `_ZNKSi6gcountEv`→`std::istream::gcount() const` |
| `interceptor` | `\x01` prefix, strip the prefix byte and one `_` | `\x01_fopen`→`fopen` |
| `isoc99` | `__isoc99_<f>`→`<f>` (scanf-family binary-compat rename) | `__isoc99_sscanf`→`sscanf` |
| `isoc23` | `__isoc23_<f>`→`<f>` (glibc≥2.38 C23 rename) | `__isoc23_strtol`→`strtol` |
| `fortify` | `__<f>_chk`→`<f>` (`_FORTIFY_SOURCE`) | `__memcpy_chk`→`memcpy` |
| `lfs64` | `<f>64`→`<f>` (`_FILE_OFFSET_BITS=64`) | `fopen64`→`fopen` |
| (empty) | name unchanged, both columns equal | `fwrite`→`fwrite` |

Notes:

- demangling depends on `llvm-cxxfilt` (also from the configured LLVM bin
  directory, PATH fallback, finally the system `c++filt`); if unavailable, C++
  names are treated as "unchanged" without error. If the demangle and cxxfilt
  line counts disagree, the whole mapping is abandoned and falls back to
  "unchanged" — better no mapping than a wrong pairing.
- `lfs64` is the only rule that can misfire (a non-LFS function whose name
  happens to end in `64`); 12 pairs across 4 projects were all true LFS
  variants in practice.
- The batch script (`run-api-census.sh`) aggregates two further views on top:
  `unmodeled-union-ir.mapped.tsv`
  (`ir name<TAB>source name<TAB>rule<TAB>project count<TAB>project list`,
  sorted by project count — which unmodeled library function each bc name
  corresponds to and how many projects use it, at a glance) and
  `unmodeled-src-union-ir.tsv` (folded by source name; `fopen64` and `fopen`,
  `__isoc99_fscanf` and `fscanf` fold to the same row — a modeling-priority
  table in man-page terms).

## Known boundaries

- **Count what the tree contains**: build-time tool/generator bitcode (e.g.
  sqlite's `mkkeywordhash.bc`), checked-in test `.bc` (llvm-project has ~300),
  and internal symbols of bundled third-party libraries (opencv's OpenEXR
  namespace — its defining TUs are not in the tree, so it counts as tree-level
  external) all enter the external list; multiple build configs mixed in one
  tree cannot be told apart.
- **Only calls actually compiled into bitcode are seen**: `#ifdef` branches and
  files not participating in the build are invisible (the source-level census
  upper bound is unavailable at IR level; conversely IR gives the exact
  "compiled into the artifact" fact).
- `functions` is an IR `define` count, a different metric from a source-level
  function count (inlining, unbuilt files, and macro-generated functions all
  diverge).
- Externality is **TU/module-level**: a per-TU `declare` does not reflect
  whether the linker finally resolves it from an external library (weak
  symbols, `--gc-sections`, and unlinked members of a `.a` are invisible).
- A name that is both `define`d and `declare`d (weak) counts as in-project;
  `alias`/`ifunc` definition lines likewise count as defines (the C1→C2
  constructor alias is the main source).

## Limitations and next steps

A purely IR-level census: no call counts, no call sites, no reachability. When
caller-level detail is needed, use a full call-graph analysis rather than this
tool.
