# IKOS Stub Tool (automatic assembly tool for external-function stubs)

`ikos-stub` automatically assembles "program bitcode + a stub library" into a "single
analyzable bitcode". It automates the manual four-step flow in the "Model library
functions" section of analyzer/README.md (write stubs.c → clang compile → llvm-link →
ikos analysis), and makes the **source** of the stubs a pluggable contract: one
directory, one `.c` file per function — hand-written, copy-pasted, or produced by any
generator, stubs can all be attached directly.

It connects with `ikos-api-census` (see [API_CENSUS.md](API_CENSUS.md)): census answers
"which external functions are not modeled" (the spec-generation to-do list), and
`ikos-stub` answers "once we have a stub, how to use it correctly".

## Why it is needed (pitfalls of the manual flow)

Every step of the manual flow has constraints that can only be discovered by hitting
them:

1. **Compile command**: must carry `-D__IKOS__ -D_FORTIFY_SOURCE=0 -O0 -Xclang -disable-O0-optnone`
   and the `-isystem` path to intrinsic.h; miss one and you get empty semantics or
   `__*_chk` variants with the wrong name;
2. **llvm-link does not verify signatures**: declare `@f(i8*)` paired with define
   `@f(i32*)` — llvm-link **exits with code 0**, the link succeeds, and the call site
   is silently rewritten to
   `call void bitcast (void (i32*)* @f to void (i8*)*)(i8* null)` — the stub receives
   its arguments with the wrong type, and **the analysis result silently goes wrong
   with no warning at all**. This is the core reason this tool exists: the `link`
   subcommand compares the signatures on both sides itself and hard-errors on a
   mismatch;
3. **Variant symbols do not line up**: glibc headers rewrite `fopen` into `fopen64` and
   `scanf` into `__isoc99_scanf`, so a stub written under the source name will not link
   (or worse, silently misalign when the signature happens to be compatible);
4. **Transcribing skeleton signatures is error-prone**: structs passed by value are
   flattened into scalars in IR, large struct returns become an `sret` pointer
   parameter, and C++ named types even appear in the quoted form
   `%"class.std::..."`; reverse-engineering a C signature from a declare line by eye is
   both slow and error-prone.

## Three subcommands

```sh
ikos-stub check   [--stub-dir <dir>] [--tsv <file>] [--all] <bitcode>...
ikos-stub scaffold -o <dir> [--force] <bitcode>...
ikos-stub link    --stub-dir <dir> -o <out.bc> [--fail-missing] <bitcode>...
```

| Subcommand | Role |
| --- | --- |
| `check` | report a disposition decision for each external function (a dry run of `link`), always exits 0 |
| `scaffold` | generate signature-bearing stub skeletons for missing functions (placeholder body + `__ikos_*` quick-reference comment) |
| `link` | compile the selected stubs (ikos clang flags) → llvm-link → signature/symbol verification twice, before and after linking |

Typical workflow:

```console
$ ikos-stub check mixed.bc
| ir name | source name | rule | decision | detail |
| --- | --- | --- | --- | --- |
| my_cbrt |  |  | missing | no stub file |
| my_hash |  |  | missing | no stub file |
| my_scan |  |  | missing | no stub file |

| external | builtin | stubbed | missing | skipped |
| --- | --- | --- | --- | --- |
| 4 | 1 | 0 | 3 | 0 |

$ ikos-stub scaffold -o stubs mixed.bc      # generate skeletons, fill semantics, delete placeholder lines
$ ikos-stub link --stub-dir stubs -o mixed.stubbed.bc mixed.bc
$ ikos mixed.stubbed.bc                     # "unknown extern" warnings should disappear
```

## Stub library contract

- In the directory, **one `.c` file per function**, the file name = the symbol name it
  defines (the IR name): `fgets.c`, `__memcpy_chk.c`, `_ZNSt....c` (a C++ mangled name
  is a legal C identifier, handled as-is);
- Illegal characters in file names (`\x01`, `$`, etc.) are replaced with `_` and an
  8-hex-digit crc32 suffix is appended to prevent collisions; `scaffold` and `link`
  use the same `stub_key()` rule, guaranteeing they are inverses of each other;
- Optional `manifest.tsv`: `<stub-name> TAB <IR-name> [TAB <IR-name>...]`, lines
  starting with `#` are comments. The manifest mounts one stub onto additional IR
  names via the `-D<stub-name>=<IR-name>` macro rename — an explicit intent that takes
  priority over all heuristics (used for fortify variants, hand-written stubs that need
  `__asm__` labels, etc.);
- Scaffold-generated skeletons contain one line
  `/* IKOS-STUB-SCAFFOLD-PLACEHOLDER */`; `link` by default **refuses** to link files
  still containing this marker (`--allow-placeholder` unlocks it but emits an UNSOUND
  warning) — preventing a "placeholder body as real semantics" from being silently
  analyzed.

## Disposition decision (for each external IR name, first rule hit in order)

| Order | Condition | decision | Description |
| --- | --- | --- | --- |
| 1 | name is in the builtin whitelist (library_function.cpp) and not `--force-stub` | `builtin` | skip: the built-in semantics of `exec_intrinsic_call` are more precise |
| 2 | stub directory has a file with the exact IR name | `stub-exact` | compile and link directly |
| 3 | manifest covers that IR name | `stub-manifest` | `-D` macro rename mount |
| 4 | IR name starts with `\x01` (interceptor prefix) | `skip-interceptor` | not a C identifier; hand-written `__asm__` label stub + manifest |
| 5 | source name (census reverse map) has a stub file, and rule ∈ {lfs64, isoc99, isoc23} | `stub-rename` | same-signature variant, compile-time rename `-Dfgets64=fgets` |
| 6 | parameters contain `inalloca` (C++ non-trivial by-value) | `skip-inalloca` | not expressible in C; hand-write a C++ stub |
| 7 | otherwise | `missing` | check reports / scaffold generates skeleton / link warns or `--fail-missing` |

Variant strategy: fortify (`__f_chk`) has one extra parameter whose position varies by
function, so it is **not** mechanically renamed; it requires an exact-IR-name file
(scaffold generates a separate skeleton from the real IR signature); the five common
`_chk` (strcpy/memcpy/memmove/memset/strcat) are in fact already in the builtin table.

## Signature verification (on by default, hard error)

After compiling, `link` uses llvm-dis to read out the `define` line of each stub module
and compares it exactly against the program-side `declare` line after computing a
**signature comparison key**; after linking it verifies again that every symbol is a
`define` in the product, not a `declare`. Key rules:

- Scalars are compared exactly by bit width (`i32` ≠ `i64`); `byval/sret/inalloca`
  participate in the comparison. Most `align/dereferenceable/noundef/noalias` are
  annotational attributes and do not participate; `signext/zeroext` on `i1/i8` belong
  to the same C ABI and are treated as equivalent in the signature key, while
  extension attributes at other widths still participate;
- Typed pointers are compared by pointee: `i8*` ≠ `%struct.X*`; when the pointee is a
  named type it is compared by tag name with the `struct./class./union.` prefix
  stripped (a `struct Foo*` written in a C skeleton can match a C++ program's
  `%class.Foo*`). C++ quoted types with namespaces/templates (such as
  `%"class.std::basic_string"*`) map to a stable `ikos_cxx_<hash>` C tag; the
  signature key normalizes the original C++ name with that hash tag, but still
  requires layout/hierarchy and ABI equivalence. Apart from these explicit ABI
  normalizations the comparison has no relaxation whatsoever: llvm-link silently
  bitcasts call sites on declaration/definition signature conflicts (the stub body
  receives wrongly-typed parameters), so a stub whose signature differs (including a
  differing pointee literal name) is never hard-linked — cross-module literal-name
  drift (such as `%struct.anon.0.N` vs `%struct.__va_list_tag`) uses
  `--skip-mismatched` to drop that stub per-bc (the function remains external and is
  counted as unmodeled) rather than relaxing the comparison;
- vararg is compared separately; literal aggregates (`{ i32, i8* }`) are compared by
  recursive structure comparison.

On mismatch it prints both sides' source text and the stub source file path side by
side, and exits non-zero:

```console
[ERROR] my_hash: stub signature does not match the program declaration
[ERROR]   program: declare i32 @my_hash(i8* noundef, i32 noundef) #2
[ERROR]   stub:    define dso_local i32 @my_hash(i32 noundef %0, i32 noundef %1) ...
[ERROR]   stub file: stubs/my_hash.c
```

The feasibility of `scaffold` rests on the same determinism: the skeleton C signature
is compiled by the **same clang-14**, so the IR shape round-trips exactly — small
struct by-value parameters are flattened back into a scalar sequence, anonymous
aggregate returns are restored into the literal `{ i32, i8* }`, and large struct
returns are restored into the `sret` shape (the skeleton recursively rebuilds the
struct typedef from the program's type table). C++ quoted named types are rebuilt into
a complete C struct only when the layout is needed for by-value; when only pointers
are used, an incomplete-type forward declaration is emitted. Opaque types likewise use
forward declarations. Signature verification is the backstop safety net.

## Link failure modes at a glance

| Failure | Handling |
| --- | --- |
| missing stub | default warning + hint of the scaffold command; `--fail-missing` hard error |
| placeholder not removed | hard error; `--allow-placeholder` unlocks + UNSOUND warning |
| two translation units collide on the same symbol | hard error, list the conflicting .c files |
| signature mismatch | hard error, print both sides' source side by side; `--skip-mismatched` drops that stub per-bc (function remains external); `--no-verify` skips (not recommended) |
| rename did not produce the expected symbol | hard error (stub file does not define the expected symbol) |
| symbol is still declare after linking | hard error (backstop) |
| program bc uses opaque pointers (compiled by clang ≥ 15) | warning: stubs are compiled by clang-14 typed pointers, mixed linking will necessarily fail; recompile the program with the ikos toolchain |
| clang / llvm-link fails | translate stderr into a hard error; `--save-temps`/`--temp-dir` keep the compile artifacts |
| interceptor / inalloca | warning + explanation (phase-one limitation) |

## Known limitations

- **interceptor symbols** (the `\x01` prefix, macOS) need `__asm__` labels; phase one
  only recognizes and prompts, does not auto-generate; hand-write and then mount via
  manifest;
- **inalloca parameters** (C++ non-trivial types passed by value) cannot be expressed
  in C, `skip-inalloca` prompts for a hand-written C++ stub;
- small non-trivial C++ `sret` return objects cannot be reliably expressed as a C
  by-value return; scaffold explicitly rejects them; external variadic functions with
  no named parameters (such as `__gxx_personality_v0(...)`) also need hand-writing;
- **opaque-pointer programs** (clang ≥ 15 products) cannot be mixed-linked with
  typed-pointer stubs (this repo's LLVM 14); the tool detects this early and prompts
  to recompile;
- `scaffold` does not aim to cover all IR types: rare types such as half/fp128/vector
  are directly reported as "hand-write", rather than generating a signature that looks
  right but is actually wrong.

## Relationship with other tools

| Tool | Responsibility |
| --- | --- |
| `ikos-api-census` | census: which external functions are unmodeled (to-do list), IR name ↔ source name |
| `ikos-api-spec` (tool/api-spec) | generation: the LLM produces stub bodies for missing functions (deterministic signature rendering, compile/signature/real-library differential and game verification) |
| `ikos-stub` | utilization: stub library → verify → link into analyzable bc |
| `ikos` / `ikos-report` | analyze the product bc, inspect warning changes |

## Real-library resolution (real-lib maps, from 2026-09-29)

The real side of `ikos-api-spec`'s differential gate needs to link the **real
implementation** of the function under test, resolved into pure data (a TSV map, human
reviewable, hand-editable, deterministically consumed):

```sh
ikos-api-spec real-libs [--system-out <file>] [--force] <project-dir>...   # scan and build the map
ikos-api-spec real-libs --probe <symbol> --map <map.tsv>               # link + load self-verification
ikos-api-spec eval-real-libs <project-dir>...                          # full external-function resolvability census
ikos-api-spec generate --real-libs <proj>/real-libs.tsv \
                       --real-libs <root>/system-real-libs.tsv ...     # earlier wins
```

Three-tier resolution (by fidelity): project in-tree `.a/.so` (same origin as the
bitcode; `.a` preferred, `.so` absolute path + rpath) → system dependency library
whitelist (`tool/api-spec/real-libs.conf`, located via ldconfig, absolute path pinning
the version) → implicit libc/libm (zero configuration). Functions that cannot be
resolved are **strictly skipped** in generate (status `no-real-lib`, 0 tokens); on link
failure it first goes through the LLM link-repair loop (`--dt-link-retries`, default 2,
parameter whitelist validation), and if it still fails the function fails and stays
missing. Under the strict install rules, only a body that `pass`es the
differential/game is installed into stubdb; passing compile/signature or a harness
skip is not semantic verification. `_Z*` C++ symbols use a C++17 real-side driver to
construct legal C++ objects, while the stub side still goes through the deterministic
C ABI and IKOS intrinsics. Expressible LLVM `sret` aggregate returns are explicitly
forwarded through the hidden result pointer by a wrapper; the driver constructs and
registers the real result object, and scalar parameters come from argv. The stub must
conservatively havoc the complete result object; in the game, deleting that havoc is
judged unsound and rolled back/relaxed. This rule is not a pointer-game skip: ordinary
input pointers are still judged by points-to/offset/nullity/write-set, and `result`
is also registered into the battery and produces `writes-result`,
`content-value-result`, and other components. The difference is that the hidden `sret`
result is an output object; full-object havoc is the soundness floor before game
tightening, avoiding mis-attributing SSO, fresh allocation, private fields, or padding
to input-pointer relations or zero values.
Long-tail bring-up (building the real libraries from source) is in
[REAL_LIBS_BRINGUP.md](REAL_LIBS_BRINGUP.md).
Native execution of the real side, the stub side, and the IKOS witness uniformly
requires Bubblewrap: the root filesystem is read-only, only the current run directory
is writable, and the working directory is pinned inside that run directory; when
Bubblewrap is missing, native execution fails closed and does not degrade into running
the real function bare.

## The ikos battery (from 2026-09-29: the analyzer itself as judge)

On the stub side of the differential gate, in addition to concrete execution, **each
input is also analyzed once with real ikos**: a deterministic harness (literal pinning
of arguments) → `__ikos_print_values` reads the return value and the abstract
interval of the per-pointer-argument points-to domain cell, plus the pointer's
points-to/offset/nullity → `__ikos_assert` excludes assertions (sqlite verdict
0=proved/1=unknown/2=refuted). **Witness verification** guarantees that pinning
mismatches only lose evidence and never misjudge (the same source without `__IKOS__`
is linked against the real library and run once; a PROVED is only accepted if the
witness reproduces the real observation). The points-to set/offset/nullity that a
pointer points to is a single-veto judgment dimension (the real concrete target must
fall within the declaration). The ikos interval and the per-buffer `cell-<tag>`
components enter the game Φ/d. `--dt-no-ikos` returns to pure simulation. Details are
in tool/api-spec/README.md "ikos battery".

## Analysis-side quality gates (from 2026-10-09, pipeline epoch 5)

The E2 R16 per-site audit (see exp/E2-NEWLY-CONFIRMED-TRIAGE.md) proved that the
differential/game gates verify **behavioral equivalence under concrete execution**,
while the abstract analyzer consumes a different set of properties — pointer
freshness, stack escape, output-buffer writes, havoc size, noreturn, return-domain
non-pruning. The gap between the two kinds of verification is the source of all major
R16 errors (of 1,314 "newly confirmed errors", only 1 was confirmed as a real defect;
the stub source itself had 378 definite errors). Epoch 5 adds this set of **abstract
dimension checks** into the generation pipeline, all as hard gates or mandatory
constraints with an E2 case provenance:

**New differential verdict rules** (`tool/api-spec/difftest.py`, the verdict flows back
into the repair loop for the LLM):

| Rule | Verdict | E2 provenance |
| --- | --- | --- |
| return aliases an input parameter | stub returns an in buffer while the real side does not → unsound (strchr/strcpy shape exempted: real same-tag or out buffer) | `fopen64` stub returns the path parameter, `getservbyname` stub returns `"ssh"` |
| return stack address | return falls within ±1MB of the target frame (recorder `FRAME` landmark event) → unsound | `g_malloc` stub `return &p0` |
| allocator returns non-fresh | for an allocator-like target, any reachable non-NULL return is not a recorder-matched `MALLOC` block → unsound; `__ikos_malloc(size)` and matched → sound | `_TIFFmalloc` stub returns the allocation size value |
| pointer/integer confusion (non-allocator) | all reachable returns <0x10000 → note (does not veto) | other small-integer-cast pointers |
| havoc out of bounds | the stub's havoc claim exceeds the registered buffer length → unsound | `stat64` stub forgets 256B into a 144B object |
| assume_mem_size overestimate | claimed size on an out buffer > what the caller provides → unsound | same family |
| return domain too narrow | reachable interval width < 1/16 of the type's full width and the observation touches the edge → narrow-range note | `getopt_long` r∈[-1,98] prunes 'o'=111 |

The contract-log `bufreg` line gains in/out columns (`__dt_buf`/`__dt_buf_out`),
consumed via the parallel channel `obs['bufinout']`, without changing the existing
bufregs shape.

**allocator modeling (from 2026-10-09)**: `__ikos_malloc(n)` maps to libc `malloc` in
the IKOS compile branch, so it enters the analyzer's existing dynamic-allocation site
and allocation-size model rather than being an ordinary integer-cast pointer; the
differential recorder branch actually allocates n bytes and records
`MALLOC <addr> <n>`. `is_malloc_like()` currently conservatively covers APIs whose
source name ends with `malloc` and which return a pointer (`malloc`, `_TIFFmalloc`,
`g_malloc`, `g_try_malloc`, etc.); `calloc`/`realloc`/`g_malloc_n` are not yet
covered, because they additionally require zero-initialization, old-block transfer, and
multiplication-overflow semantics respectively. For a matched target, a real non-NULL
return is interpreted as fresh, and any reachable non-null stub return that is not a
matched fresh block is hard-rejected; the OTHER fresh-block waiver for ordinary opaque
APIs applies only to non-allocators.

**New gates in the main pipeline** (`tool/api-spec/ikos-api-spec.py`, `ikosjudge.py`):

- **noreturn enforcement**: `NORETURN_NAMES` (the exit/err/abort family, 12 of them) or
  a declare containing the `noreturn` attribute → the body must end with
  `__ikos_assume(0)`, after which there must be no reachable return; wired into the
  generation main loop, the game materialize, and `_db_reuse` (E2 provenance: an exit
  stub missing noreturn produced a false use-after-free/double-free in genconf);
- **stub-body IKOS self-check** (`ikosjudge.selfcheck`, `--no-self-check` disables):
  each candidate, after compiling, runs `ikos -a=boa,nullity,uva,prover,dfa` with a
  nondet driver; the stub's own definite errors enter the repair loop; **object sizes
  of pointer parameters and the return value are provided by cxprobe** (see the next
  section), harness failures do not reject;
- **unembeddable extension**: `UNEMBEDDABLE_NAMES` (the setjmp/longjmp family, C++
  exception builtins, vfork/clone, 16 total) short-circuit to `unembeddable` with zero
  LLM budget, `--allow-unembeddable <name>` exempts (E2 provenance: the libjpeg
  `_setjmp` stub wired up a spurious error path);
- **`--repair-hint <text>`**: a channel for the outer audit to inject into the
  first-shot prompt (enters the response cache key; with no hint the key shape is
  unchanged and existing cache entries remain valid).

**cxprobe: a full-type call-context probe** (`tool/api-spec/cxprobe.py`)

For each external function to be stubbed in the bc, parse **the types and sizes of all
input parameters and the return value** from the bitcode itself: llvm-dis per-bc disk
cache (LLVM version in the key); named struct/union types are recursively extracted;
sizes are computed exactly via LLVM constant folding (the probe .ll carries the
original datalayout, `getelementptr T, T* null, i32 1` + `ptrtoint`, `opt` folds and
reads the integer — alignment/nesting/arrays are all handled by the data layout);
parameters and returns with erased types such as `i8*`/`void*` use **call-site
reverse lookup** (bitcast/GEP tracking + majority vote). Verified: bzip2 `stat64`
parameter = 144-byte `struct stat` (bc-typedef), ssh-keysign `getservbyname` return =
32-byte `struct servent`. The selfcheck driver allocates objects by cxprobe > header
mapping > 128B fallback — a bad stub like `forget_mem(p1, 4096)` is convicted
**at generation time, per single function, in seconds** (re-smoke: the old-style
stat64 bad stub status `failed`, not installed).

**auditgate: post-link audit gate** (`tool/api-spec/auditgate.py`)

A link + analyze gate on the whole-program bc: dual thresholds of the stub-side
definite error count (default 0) and the project-side unreachable change rate (default
5%), exit 0/1/2 + JSON report; the smoke test correctly convicts the stat64 256B/144B
write and the bzip2recover reachability change. Its role is a CI-style after-the-fact
gate and paper data collection, not a replacement for the generation-time closed loop.

**auditloop: the outer repair loop** (`tool/api-spec/auditloop.py`)

Closes the auditgate's stub-side definite errors back into the generator: aggregates
findings per stub file (the file name is the `--only` function name), first evicts
that function's stubdb entry (whole-program falsification outranks the battery that
originally admitted it), then regenerates with the finding as `--repair-hint` and
re-reviews, until clean or `--max-rounds` is exhausted; `--prune-failed` deletes stubs
that still violate — unmodeled is better than a stub carrying falsified semantics. The
unreachable change rate is only reported, not used as a repair hint (not yet
attributable to a single stub).

## Scenario driver (from 2026-10-09, differential input-surface upgrade)

The largest differential-battery failure class (55.9% in R16) is the real side having
no contract-usable input: a bare driver calls the target function directly, and the
real implementation, lacking initialization state, crashes/times out/is unstable on
the spot (`regexec` lacks `regcomp`, `fdopen` lacks `open`). The scenario driver lets
the LLM execute a setup call sequence to construct legal state **immediately before
the exactly-one target call**:

- **Upgrade ladder**: by default run the bare driver first (zero extra cost for easy
  functions); only after the real-side pre-probe (2 canaries × 2 repeats per input,
  all 4 runs must be ok and return consistently to count as usable — the flaky verdict
  of a garbage-state driver cannot rely on a single sample) condemns all of them does
  it regenerate the driver in scenario mode and rerun the battery; scenario-crash
  stderr/ASan summaries are appended to the regeneration prompt;
- **Tier 1 (on by default, `--dt-no-scenario` disables)**: the setup whitelist is
  limited to libc constructors (`open/regcomp/socket/pipe/mkstemp/tmpfile/opendir/
  fopen/pthread_mutex_init/malloc/calloc` and others, 17 entries, injected into the
  prompt with usage notes), natively available on both sides, zero link changes; setup
  ≤4 calls (guard budget 6), the target call still exactly once, setup products
  registered with `__dt_buf/__dt_buf_out` before the target call;
- **Tier 2 (experimental, `--dt-scenario-lib` enables)**: setup extends to in-library
  functions (such as `xmlInitParser`), candidates = cxprobe call-graph adjacency ∩
  reallibs resolvable set (≤8); the stub side is reworked to **also link the real
  library** — the stub object first + `-Wl,--allow-multiple-definition` lets the
  stub's target symbol win, and setup is the real implementation on both sides (verified
  by both unit tests and end-to-end linking);
- **Observation invariance**: the judge's verdict surface is unchanged (still the
  ret/wrote/events of the target call), setup only shapes the input. Smoke
  (regexec @ haproxy): bare driver all dead → pre-probe convicts → after upgrade the
  LLM writes the textbook `regcomp(&preg,…)` → `__dt_buf` registration → `regexec` →
  `regfree` sequence, and the failure reason changes from "real side has no usable
  input" to the normal semantic-counterexample repair loop.

## Sandboxed private D-Bus session (from 2026-10-09, differential environment-surface upgrade)

bluez's `dbus_*` functions (the largest R18 residual failure cluster, 58) need a live
D-Bus session bus, while the differential sandbox does network/IPC isolation.
Mechanism: for functions whose name prefix matches (`dbus_`/`g_bus_`/`sd_bus_`,
`DT_DEFAULTS['sandbox_dbus']` can disable), all of their run_once (real/stub/probe)
are changed to run **inside** bwrap via `dbus-run-session --config-file <conf> --` —
one private daemon per run, the socket explicitly placed at `<workdir>/xdg/bus` (a
bundled config avoids the TMPDIR behavior differences between dbus-run-session
versions), `XDG_RUNTIME_DIR` and `TMPDIR` point at writable directories inside the
sandbox; the socket does not leave the sandbox, zero host pollution, zero cross-run
interference. When the tool is missing it warns once and falls back to the original
behavior (does not fail closed). Naturally compatible with the scenario ladder
(`dbus_bus_get` as in-library setup shares the same private session with the target).
Decisive smoke (`dbus_bus_request_name` @ bluetoothctl): after the scenario driver
`dbus_bus_get` establishes the connection, the target on the sandbox private bus
stably returns `DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER` for both canaries — the real
libdbus really did take ownership of the name. Known limitations: ikosjudge's witness
run has no bus wrapper (its evidence is dropped per the existing fallback); libdbus
aborts directly on illegal arguments, so robustness to edge inputs is a game
input-quality concern, unrelated to this mechanism.

The complete background of the external-function handling mechanism (whitelist
modeling, conservative havoc, the `__ikos_*` semantics table) is in
[EXTERNAL_FUNCTIONS.md](EXTERNAL_FUNCTIONS.md) §5.
