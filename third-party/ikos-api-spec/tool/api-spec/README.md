# IKOS API Spec (LLM external-function stub generation tool)

`ikos-api-spec` uses a large language model to automatically generate stub function bodies for **unmodeled external functions**: given program
bitcode, it renders a deterministic signature skeleton for each `missing` external function, has the model fill in only the function body (expressing
semantics with `__ikos_*` intrinsics), and then — after closed-loop verification via **compile, signature, real-library differential testing, and coevolutionary
game** — installs the result into the stub library directory. The artifacts are fully compatible with
`ikos-stub link` (see [doc/STUB_TOOL.md](../../doc/STUB_TOOL.md)).

The tool's generation pipeline does not simply request output from the model and trust it: candidate stubs are linked against real implementations for **sandboxed differential
verification**. Native execution requires Bubblewrap; the real side, stub side, and IKOS witness binaries run on the local machine
and by default get
PID/network/IPC/UTS isolation, a read-only root filesystem, and writable paths limited to the current run directory, on top of
rlimit/timeout. Thus, even if the function under test involves process control, signals, filesystem, or network side effects,
differential execution is confined to a temporary test environment and does not directly touch the host workspace or desktop session.
This design comes from a real lesson of the E1 experiment: when the sandbox was not enabled, an incident in which real-side differential execution, the graphical session, and the
experiment process all died at the same time exposed the risk of local execution side effects spreading. Although the signal source could not at the time be
uniquely attributed to any single function under test, real implementations like `kill`/`signal` really should not be handed the host PID
space; the sandbox confines such operations to an isolated namespace and avoids a recurrence of the same class of experiment accident.

For targets like `dbus_`/`g_bus_`/`sd_bus_` that depend on a message bus, differential execution also starts, inside the same
sandbox, a one-shot private D-Bus daemon for each run: the real implementation gets a usable session
bus, sockets and runtime files stay in the current run directory, and the host session bus is never touched (mechanism in
the "Private D-Bus session inside the sandbox" section).

It is one middle link of the spec pipeline:

| Tool | Responsibility |
| --- | --- |
| `ikos-api-census` | Census: which external functions are unmodeled (the to-do list) |
| **`ikos-api-spec`** | **Generate: LLM produces a verifiable stub body for missing functions** |
| `ikos-api-spec prefetch` | Prefetch: 0 LLM; install already-verified stubs for missing functions from the stub database (what is missing is left to generate) |
| `ikos-stub link` | Assemble: compile + llvm-link + signature/symbol double verification |
| `ikos` / `ikos-report` | Analyze the resulting bc, confirm the unknown-extern warning disappears |

## Running

A standalone script, not part of the analyzer's CMake build. Any python3 works (the LLM client uses only
the standard library):

```sh
python3 tool/api-spec/ikos-api-spec.py generate [options] <bitcode>...
```

The tool automatically derives the install prefix from `ikos-stub` on PATH, locates the `ikos` python package and reuses its internal
functions (`_scan_and_plan`, `render_stub`, `signature_key`, etc.), so disposition decisions and the skeleton
are exactly consistent with `ikos-stub`. If it cannot be found, specify `--ikos-prefix` or `--site-packages`.
Development note: after modifying `analyzer/python/ikos/stub.py` in the source tree, you need to reinstall/sync
the Python package in `ikos-api-spec-install` before running batch experiments; otherwise the CLI and unit tests may
already be using the new logic, while the generate process still bootstraps to the old installed `ikos.stub`.
When differential verification is enabled, Bubblewrap must be installed (the command is `bwrap`; on Debian/Ubuntu it is the
`bubblewrap` package) to get OS-level isolation. When `bwrap` is not found on PATH, native differential execution
and IKOS witness execution both fail-closed and are recorded as harness failure; batch experiments and environments
that do not trust the library under test should not bypass this requirement.

Generating differential evidence for the D-Bus-prefixed targets described above also invokes `dbus-run-session` (on Debian/Ubuntu
it usually comes with `dbus`/`dbus-bin`). When that command is missing, the tool warns once and falls back to the ordinary sandbox —
it never falls back to the host bus; such targets will typically fail because the real side has no usable input and remain missing.

## Common options

| Option | Default | Description |
| --- | --- | --- |
| `--provider` | - | `openai` / `deepseek` / `glm` built-in presets: base URL, key environment variable, and default model all in place (see the next section) |
| `-o, --stub-dir` | `stubs` | stub library output directory |
| `--only` | all missing | generate only the specified functions (comma-separated, IR names or source names both work); when a name is not in the bc, belongs to the builtin whitelist, or already has a stub, each name is reported with a way out (builtin → `--force-stub`) |
| `--model` | provider default | model name on the endpoint; required when there is no `--provider` |
| `--base-url` | provider preset, otherwise the official OpenAI one | any OpenAI-compatible endpoint (vLLM, Ollama compatibility layer likewise) |
| `--api-key-env` | provider preset, otherwise `IKOS_API_KEY`→`OPENAI_API_KEY` | environment variable holding the key; local endpoints can have no key |
| `--temperature` | provider preset, otherwise 0.2 | OpenAI reasoning models do not send this parameter, GLM fixes it at 1.0 |
| `--reasoning-effort` | not sent | reasoning tier for always-on thinking models (`reasoning_effort`; GLM accepts `low`/`high`/`max`). The endpoint's default tier silently thinks for minutes on hard problems — zero bytes during non-streaming requests; the first thing to do is set `--timeout` large enough (default 600 seconds), since a too-short timeout will kill legitimate slow thinking into a retry loop; use this option only when you really need speed: `low` compresses a single call to seconds, trading first-shot quality for speed, while the repair loop and verification gates guarantee the artifact's reliability |
| `--max-retries` | 3 | repair rounds per function (1 = single shot); together with `--co-rounds` it forms the **bounded iteration budget**; when exhausted, install the last reliable version that passed the battery |
| `--max-functions` | unlimited | budget cap |
| `--force-stub` | - | make functions already modeled in the builtin whitelist also go through generation |
| `--cache-dir` / `--no-cache` | `~/.cache/ikos-api-spec` | response cache |
| `--db-dir` / `--no-db` | `~/.cache/ikos-api-spec/stubdb` | stub database: reuse verified stub bodies across programs (see the next section); `--no-db` neither reads nor writes |
| `--domain` | `all` | targeted generation by numerical abstract domain (tokens consistent with ikos `-d`; default `all` = dual admission, sound across backends; experiments did not target, see the "Abstract domains and stub portability" section) |
| `--save-failed` | off | save the last candidate of a failed function to `<cache-dir>/failed/` |
| `--no-difftest` | off | turn off the differential-verification gate (see the next section; on by default) |
| `--no-self-check` | off | turn off the stub-body self-check gate (on by default: each candidate that passes compile + signature is analyzed once more by ikos on its own, and a definite error in the stub file rejects that body; harness failures do not falsely reject) |
| `--allow-unembeddable` | - | let a particular special-control-flow function go through LLM generation (repeatable; setjmp/longjmp family, C++ exception runtime, vfork/clone directly report `unembeddable` by default, zero LLM budget) |
| `--repair-hint` | - | inject discovery text from an outer audit (e.g. auditgate) into the first-shot body prompt (repeatable; enters the response cache key; cache shape unchanged when there is no hint) |
| `--dt-no-scenario` | off | turn off the scenario-driver upgrade ladder (on by default: when the real side has no contract-usable input, retry with a scenario driver using a libc setup call sequence) |
| `--dt-scenario-lib` | off | extend scenario setup to in-library functions (experimental; the stub side links the real library and the stub symbol wins) |
| `--real-libs` | implicit libc/libm | real-library map for the real side of the differential gate (TSV, repeatable, first wins; when not passed, only libc/libm can be resolved and the remaining functions are skipped as `no-real-lib` before generation). Generate: `ikos-api-spec real-libs` |
| `--dt-inputs` | 6 | number of LLM-proposed test inputs per function (the tool appends deterministic edge inputs) |
| `--dt-schedules` | 48 | cap on stub-side non-deterministic schedules per input |
| `--dt-driver-retries` | 3 | repair rounds after a driver is rejected |
| `--dt-link-retries` | 2 | LLM repair rounds for real-side link failure (static-library transitive dependency argument completion, argument whitelist validation) |
| `--dt-timeout` | 5 | seconds per driver run |
| `--dt-no-sanitize` | off | do not attach ASan/UBSan on the real side (crashes are recognized by signal only) |
| `--dt-no-ikos` | off | turn off the ikos battery (the analyzer itself as judge; on by default; when turned off, falls back to pure concrete simulation, the old per-input per-byte behavior) |
| `--dt-ikos-cpu` / `--dt-ikos-wall` | 10 / 60 | CPU seconds / wall-clock seconds per ikos judge run |
| `--dt-ikos-domain` | derived | ikos judge domain (default `all`→`interval-congruence`, otherwise follows `--domain`; a relational domain makes `assume_mem_rel` actually compute) |
| `--dt-report` | - | summarize per-input judgments as a JSON report |
| `--dt-keep-temps` | off | keep the build/run temporary directories of differential testing |
| `--co-rounds` | 3 | rounds of the two-agent game on top of the differential gate; each round = A's input move + B's body move, two alternating phases (battery settlement between phases); 0 = fall back to the single-shot differential gate; when the budget ends, hand back the last body that passed the battery (reliable floor, see the next section) |
| `--co-a-policy` / `--co-b-policy` | greedy | strategy update policy: greedy (payoff-driven) / random / fixed (ablation arms) |
| `--co-attack-batch` | 3 | number of attack inputs A proposes per round |
| `--co-lambda` / `--co-mu` / `--co-alpha` / `--co-beta` / `--co-epsilon` / `--co-tau` | 0.1 / 0.2 / 0 / 0 / 0.02 / −ε | potential-function and payoff coefficients. Default α=β=0 ⇒ identical-payoff exact-potential game (**the default configuration is within theorem coverage**, see "Formal model"); α/β>0 are δ-approximation ablation arms; τ is the move acceptance threshold (a body revision with u_B > τ is kept, otherwise rolled back and marked `-rejected`; default τ = −ε, i.e. an ε-tolerance better response — a revision that does not degrade beyond the equilibrium tolerance is kept, a revision manufacturing an inconsistency signal is necessarily rolled back, 0 is the strict arm). Used for RQ3 parameter sweep |
| `--co-report` | - | summarize the game trajectory (Φ curve, per-strategy record) as a JSON report |

## Model providers (OpenAI / DeepSeek / GLM)

The OpenAI-compatible endpoints of all three vendors are built in as `--provider` presets. There are two ways to supply the key: **fill it directly into
the `HARDCODED_API_KEYS` dictionary at the top of [llm_client.py](llm_client.py)** (takes precedence over environment
variables; stored in plaintext, be careful not to commit it), or configure it in the corresponding environment variable (if a provider is selected and there is no
key in either place, it errors out directly and tells you which variable to export):

| provider | base URL | key environment variable (first match in order) | default model | other common models |
| --- | --- | --- | --- | --- |
| `openai` | `https://api.openai.com/v1` | `OPENAI_API_KEY` | `gpt-5.6-terra` | `gpt-5.6-sol`, `gpt-5.6-luna` |
| `deepseek` | `https://api.deepseek.com/v1` | `DEEPSEEK_API_KEY` | `deepseek-flash` | `deepseek-v4-pro`, `deepseek-v4-flash` |
| `glm` | `https://open.bigmodel.cn/api/paas/v4` | `GLM_API_KEY`, `ZHIPUAI_API_KEY`, `ZAI_API_KEY` | `glm-5.3` | `glm-5.3-flash`, `glm-5.2` |

```console
$ export DEEPSEEK_API_KEY=sk-...      # or OPENAI_API_KEY / GLM_API_KEY;
                                       # you can also skip export and fill HARDCODED_API_KEYS directly
$ ikos-stub check prog.bc             # first see what's missing
$ python3 tool/api-spec/ikos-api-spec.py prefetch -o stubs prog.bc
                                       # 0 LLM: install what's already in the stub database first
$ python3 tool/api-spec/ikos-api-spec.py generate -o stubs --provider deepseek prog.bc
                                       # the rest goes through generation (installed into the db on success)
$ ikos-stub link --stub-dir stubs -o prog.stubbed.bc prog.bc
$ ikos prog.stubbed.bc                # the unknown-extern warning should disappear

# generate for a single function only (instead of all missing functions in the bc):
$ python3 tool/api-spec/ikos-api-spec.py generate -o stubs --provider glm \
      --only sin prog.bc               # builtin-whitelist functions additionally need --force-stub <name>
```

`--model` can switch to a specific model at any time (e.g. `--provider glm --model glm-5.3-flash`); model names iterate
quickly, and the names in the presets are just the current defaults — if you get "model not found", check the vendor docs and change the name.
The bare `--base-url`/`--model` mode without `--provider` is kept unchanged, for vLLM, Ollama
compatibility layers, or any self-hosted endpoint (key taken from `IKOS_API_KEY`/`OPENAI_API_KEY`; local endpoints can omit it).

Vendor behavior differences are already adapted: GPT-5.x reasoning models do not accept the `temperature` parameter (this provider
does not send it by default); GLM-5.3 always thinks and cannot be turned off (`reasoning_content` is returned separately and does not affect body
extraction), with default `temperature` 1.0 consistent with the official examples. GLM's default reasoning tier can silently think ten-plus minutes on a single
call for complex functions (zero bytes during non-streaming), and the remedy is to **set the LLM request timeout
large enough**: `--timeout` (`SPEC_TIMEOUT` in the batch scripts, default 600 seconds); a timeout
shorter than the thinking time will kill legitimate slow thinking into a retry loop; use
`--reasoning-effort low` to compress thinking time only when you really need speed (first-shot quality traded for speed; artifact reliability is still guaranteed by the repair loop
and verification gates).

## v1 design: fully LLM-based, but signature and verification are deterministic

Semantic decisions (which `__ikos_*` to select, preconditions, side effects, return-value ranges) are all handed to the model;
everything that would **fail silently** stays in the deterministic machinery:

1. **Signature is non-negotiable**: the C signature and struct typedefs are rendered by `render_stub` from the IR `declare`
   lines (byval/sret handling, `__asm__` labels, `p0, p1, ...` parameter names); the model only
   returns the function-body statements, which are spliced into the skeleton. llvm-link does not verify signatures and silently bitcasts call sites
   on mismatch, so the signature is never handed to the model (this is the core reason `ikos-stub` exists, and the same applies here);
2. **Lexical guard**: if the body contains `#include`/`#define`/`__asm__`/`static`/
   `typedef`/`extern` or a placeholder marker, it is rejected without compiling;
3. **Compile gate**: the candidate is compiled with the ikos clang flags (`-D__IKOS__ -D_FORTIFY_SOURCE=0 -O0
   -Xclang -disable-O0-optnone` + the intrinsic.h `-isystem`), stderr is fed back to
   the model for repair;
4. **Signature gate**: the compiled artifact's `define` line and the program's `declare` line are compared exactly via
   `signature_key` (the same ruler as `ikos-stub link`);
5. **noreturn enforcement and special-control-flow rejection**: the body of a noreturn function (by name table or declare attribute)
   must end with `__ikos_assume(0);`; setjmp/C++ exception class functions directly report
   `unembeddable` and do not enter the model (see the "Analysis-side quality gates" section);
6. **Stub-body self-check gate**: after the signature gate, ikos analyzes the stub itself on its own, and a definite
   error in the stub file is sent back for repair (see the "Analysis-side quality gates" section);
7. If any gate fails → the error is fed back to the model as a new turn of the conversation, up to `--max-retries` rounds;
   a single function's failure does not affect other functions in the batch.

### C++ external functions and C ABI adaptation (2026-10-05)

After C++ source enters LLVM IR, function symbols and type names become mangled / quoted forms, e.g.
`_ZNSt...` and `%"class.std::basic_string"*`. The old skeleton parser matched the unquoted
`%class.Foo` syntax, which caused a large number of functions in C++ heavy projects like Bitcoin to
report `cannot scaffold: declaration line cannot be parsed` before even reaching the model. This enhancement brings C++ external
functions into the same "deterministic signature + machine verification" pipeline:

- **IR syntax**: parse quoted LLVM local type names, return-value `align N`,
  `dereferenceable(N)` / `dereferenceable_or_null(N)`, multi-level function pointers (e.g.
  `i32 (...)**`), and packed structs (`<{ ... }>`).
- **C ABI rendering**: C++ named types are mapped to stable, legal C struct/union tags; types referenced only through
  pointers get forward declarations, and only types used by value are recursively rebuilt to full layout from the IR type table.
  `i1` renders as `_Bool`; callbacks returning `void` and unnamed-parameter variadic callbacks also have compilable
  ABI-equivalent expressions.
- **Signature equivalence**: original C++ type names and the generated `ikos_cxx_<hash>` tag are normalized to the same
  signature key, so `ikos-stub link` still exactly verifies the ABI rather than loosening type checking; the
  `signext/zeroext` difference of `i1/i8` is treated as C-ABI-equivalent.
- **Explicit inexpressibility boundary**: small non-trivial C++ `sret` return objects cannot reliably reproduce
  their return ABI in C; the tool rejects them at the scaffold stage with a reason, rather than silently mis-linking after generation;
  `inalloca` and unnamed-parameter external variadic functions still need to be written by hand.
- **Prompt v9**: the system prompt makes clear that C++ functions expose the LLVM C ABI: the
  `p0` of a non-static member function is usually `this`; constructors/destructors and output objects must be conservatively havoc'd; reference/object-pointer returns
  prefer a parameter pointer or NULL; a local result for an expressible aggregate return must have full-object havoc;
  and it must not call the C++ runtime, allocate, or construct C++ objects.

Regression validation: added `tool/api-spec/tests/test_stub.py` covering quoted types, return attributes,
function pointers, packed structs, C++ tag matching, and `sret` rejection; the offline self-check of real `bitcoin-cli` bitcode
rendered, compiled, and signature-matched 380 of 385 external functions, 0 compile failures /
0 signature mismatches, the remaining 5 being the explicit rejection boundaries described above.

### C++ real-side differential driver (2026-10-06)

The skeleton and stub side continue to use the C ABI; the differential gate's real-side driver automatically switches to C++17 based on the mangled symbol
`_Z*`. The driver must construct real C++ objects (such as `std::string`,
containers, and project source types), and must not disguise raw bytes as C++ objects; objects passed in through pointers/references
are registered with `__dt_buf` and kept alive until `__dt_end()`. The wrapper forwarding layer also carries the skeleton-generated
`ikos_cxx_*` / aggregate typedef prelude, so that the `dt_target` declaration and the renamed real-implementation
wrapper do not lose ABI types when they are in two translation units. The C++ two-sided build links
`libstdc++`; `__cxa_*`-style C ABI runtime helpers still use the C driver.

LLVM `sret` aggregate returns cannot be disguised as ordinary source-level C++ return values by the driver. The differential
wrapper explicitly exposes the hidden result pointer: both the real side and the stub side declare
`void dt_target(Result*, ...)`; the C++ driver first default-constructs and registers `result`, then passes
`&result` as the first argument. Every scalar parameter must be mapped to a separate argv token (unsigned
integers parsed with `strtoull`), and parameters like `pos/count` must not all be hardcoded, otherwise the attacker
cannot explore real branches. The stub-body convention is to locally construct the result, `__ikos_forget_mem` the entire object
(or an equivalent conservative havoc), and return; the differential judge will judge an aggregate return lacking full-object
havoc as `unsound`, and will not pass it even if no byte difference is observed on the current input.

This is not conflating the hidden `sret` result with an ordinary input pointer. The points-to/offset/nullity of ordinary pointer parameters,
the output write set, and the content axis are already judged by the differential game; what `result` additionally
requires is a **reliability floor**: it is a compiler-hidden output object that may be a combination of SSO, fresh heap
allocation, library-private fields, and padding, which cannot be deduced from input pointer relations and cannot be substituted with a zero
value. Full-result havoc first declares "the entire object the caller receives is unknown"; only then can party B tighten it
within the range covered by real evidence, using primitives like `__ikos_assume_mem_value`. If the game's
`shrink-havoc` removes that floor, party A adding inputs that expose a non-zero/new write set immediately judges `unsound` and rolls back
or relaxes.

This is not stuffing C++ syntax into an IKOS stub, but separating the language sides: the LLM driver uses source-level C++
types to construct legal arguments, while the stub still expresses abstract semantics using only IKOS intrinsics. If the headers for project-defined
types are unavailable, objects cannot be legally constructed, or non-trivial returns/parameters exceed the current C ABI expression boundary,
the differential harness fails; the function remains missing, and a stub that only passes syntax/compile is never installed.

### Prompt essentials

The system prompt is fixed to include: the soundness principle (a stub is the **only** semantics of a function in the analysis, it must over-approximate,
missing side effects = unsound, when in doubt havoc more), the `__ikos_*` helper semantics table, the four-step writing recipe
(preconditions → write side effects → nondeterministic return → `__ikos_assume` postconditions), the official fgets example,
hard constraints (C99, only `stddef.h` identifiers + helpers, use only the `p0..` parameters), and the
C ABI object semantics of C++. The user prompt gives, per function, the
IR name, source name (man-page semantics anchor), declare line, C signature line, and available typedefs. When changing the prompt,
bump `PROMPT_VERSION` in the module at the same time; old caches auto-invalidate.

The semantic helpers exposed by the stub-body DSL are fixed at 12: `__ikos_assert`, `__ikos_assume`,
`__ikos_nondet_int/uint`, `__ikos_check_mem_access`,
`__ikos_check_string_access`, `__ikos_assume_mem_size`,
`__ikos_forget_mem`, `__ikos_abstract_mem`, `__ikos_assume_mem_value`,
`__ikos_assume_mem_rel`, and `__ikos_watch_mem`. Of these,
`__ikos_assume_mem_rel(p1, n1, p2, n2, kind)` preserves existing
content knowledge without havoc, and establishes, for the corresponding 8-byte signed chunks, an `==`, `<=`, or `>=`
relation per `kind`; it does not by itself express a new write side effect. If two output buffers need to be "unknown but related", first
`__ikos_forget_mem`, then add the relation. It may only be used when the API explicitly guarantees such a chunk relation, and must not
be applied to opaque structs or buffers of undefined layout.
`__ikos_watch_mem` is only a debug observation point; it produces no havoc and cannot substitute for real write-side-effect modeling.
Analyzer debug/partitioning intrinsics are not part of the stub-body semantics DSL. The current prompt revision is
`10-semantic-helpers`; `tool/api-spec/tests/test_difftest.py` simultaneously checks the prompt,
canonical intrinsic declarations, the differential shadow header, and the recorder implementation, to avoid missing this again.

### Artifacts and ownership

- One `.c` per function (filename = `stub_key(IR name)`), with a provenance comment on the first line
  `/* generated by ikos-api-spec (model: ...) */`, no scaffold placeholder markers,
  directly accepted by `ikos-stub link`;
- The provenance comment doubles as an **ownership marker**: if the target file already exists and was not generated by this tool (handwritten / generated by
  others), it refuses to overwrite and warns regardless of `--force`;
- The LLM response cache stores `responses.json` keyed by the hash of `(PROMPT_VERSION, model, IR name, declare line)`;
  a hit skips the API call but still goes through compile + signature verification (locally free, catches
  toolchain drift). `--force` ignores cache reads.

## Differential verification gate (built into generate, on by default)

The compile gate and signature gate only verify mechanical correctness; a **semantically too strong** stub (missing a write side effect, too-narrow return
range, too-strong precondition) still passes them. The differential-verification gate adds this
layer after the signature gate and before installation: **the LLM additionally generates a small driver program and a set of test inputs**, the same driver is compiled twice
— once linked against the real library (with ASan/UBSan), once linked against the stub + recorder — comparing, per input, the real
behavior against the abstract behavior the stub claims, to judge whether the stub is an **over-approximation**. The real library is resolved through
real-lib maps (three layers: in-tree libraries, system dependency libraries, libc/libm). A judged
unsound counterexample enters the same repair loop as the compile/signature gates as the rejection reason
(sharing the `--max-retries` budget).

**Strict installation rule (from 2026-10-06)**: only a body whose differential/game gate status is `pass` can
be installed and enter stubdb; `harness-error`, driver failure to generate, real-library link failure, no usable
differential input, etc. are all recorded as `failed` (or `no-real-lib` before generation), and the function remains missing.
Passing compile and signature only means mechanical ABI correctness and is no longer sufficient for installation. `--no-difftest` is kept only as an
explicit debugging/ablation mode, and its artifacts do not enter stubdb.

The deterministic equilibrium recheck before the game ends is likewise fail-closed: if the boundary battery build or execution fails,
the game summary is recorded as `harness-error`, and the old input pool's `pass` cannot be reused to install the stub.

On top of this, by default one more **two-agent coevolutionary game** is run
(see the "Coevolutionary game" section): the stub-body model and the input-generation model play against each other by a potential function, tightening
the sound spec to the evidence boundary; `--co-rounds 0` falls back to the single-shot differential gate.

### Judgments strictly mirror the IKOS abstract model

**From 2026-09-29, this table is the per-input fallback layer**: when the ikos battery (see the next section) is present, the numerical-dimension
judgment is given directly by the analyzer body's provability evidence (PROVED exclusion → unsound; REFUTED →
return axis sound), and the Python simulation in this table only takes over on inputs where ikos has no evidence. The table's criteria
are consistent with ikos's actual semantics (the measured evidence of the congruence example comes from the real analyzer):

Every judge rule corresponds to the analyzer's actual abstract-domain semantics (evidence: `value/abstract_domain.cpp`'s
domain stack), not a self-invented criterion:

| Abstraction | IKOS semantics | Judgment rule |
| --- | --- | --- |
| Integer | MachineInt domain, backend selectable (`-d`): interval family join = convex hull; congruence family (congruence / interval-congruence etc.) join = congruence class | **Dual admission**: the real return value must lie in both the convex hull **and** the congruence closure (gcd of pairwise differences) of reachable stub returns. Measured evidence: `nondet ? 2 : 0` then `assert(x != 1)` is provable under `-d congruence`, not provable under interval — a stub enumerating `{-1,1}` while the real reachable is 0 is unsound in the congruence domain, and the gate rejects; `{-1,0,1}` complete enumeration (gcd=1 ⇒ closure = everything) is sound across all domains |
| Pointer | four components: uninit × nullity × points-to (block set or TOP) × offset interval (offset reuses the integer domain, likewise split by domain) | **Per-component**: real NULL ⇒ stub must reach NULL; real points to buffer j ⇒ stub must reach non-NULL and the **same block** (different block = unsound, the analyzer would place the alias on the wrong buffer); offset takes the joined convex hull, and when the congruence domain excludes out-of-closure offsets it degrades to sound\* (same criterion as the official fgets identity approximation) |
| fresh block (non-allocator) | a pointer created inside the library that the caller cannot name with an argument block (e.g. canonicalize's malloc) is strictly sound only with points-to TOP, which the stub vocabulary cannot express | **sound\* (block-level gap)**: requires non-NULL capability; argument-block aliasing is an approximation sanctioned by the official fgets pattern, reported per input with an annotation, not judged unsound |
| allocator-like return | APIs like `_TIFFmalloc`/`g_malloc` that "take a size, return fresh storage" | the stub must use `__ikos_malloc(size)`: the IKOS side reuses the libc `malloc` model to get a fresh dynamically-allocated block and allocation-size; the native-side recorder logs `MALLOC addr size` and classifies the matching return as `FRESH`. When real is non-NULL, any reachable argument-block/stack/integer-cast return is judged unsound; the NULL capability is still judged by real observation |
| memory content | per-block cell abstraction; `forget_mem` erases the pointer set (load can yield a valid pointer), `abstract_mem` preserves (typically ∅) | every byte the real side **wrote** must be covered by stub havoc (miss = unsound-missed-write); bytes that wrote a **pointer value** must use `forget_mem` rather than `abstract_mem` (otherwise a legal dereference is mis-reported as null-deref) |
| C++ `sret` aggregate return | the return object is the memory pointed to by the hidden result pointer; what the caller reads is the object content | the driver registers the real `result` object; the stub must cover the havoc of the complete result object (a full-object `forget_mem` of the local result can be mapped to the hidden result write set). No full-object havoc = unsound; approximating with only zero values / narrow content is not allowed |
| float | only an initialization state, no numerical abstraction | judgment vacuous (a note is recorded; preconditions/write side effects are still checked) |
| precondition | assert/check_mem are proven by the checker at the call site | assert failure on real-accepted input / rejecting NULL / claiming an out-of-bounds buffer = unsound-precondition; assert result varying with nondet = a warning factory |

**Multi-domain background** (the criterion of ikos's own builtins): the numerical backend is pluggable through the
polymorphic wrapper of `machine_int_domain.hpp` (`-d` has 21 domains total, default interval); builtin models' numerical
operations all go through the domain interface (`int_assign`/`int_add`/`join_with`), and individual interval-specific reads such as fgets's
`int_to_interval().ub()` (`numerical.hpp:3127`) return
top under the congruence backend → upper bound +∞ → no limit, **degrading is sound**; `__ikos_assume`'s macro expansion goes through the branch
Comparison to the domain-generic `int_add` (the congruence domain shrinks on `==`, and is a no-op on inequalities).
Functions ikos does not model (strcmp/abs/memcmp…) all go through `exec_unknown_call` returning top —
most conservative. Our stubs use only `__ikos_*` intrinsics and are likewise naturally domain-pluggable; the only domain-related
pitfall is the finite enumerated return set in the table above (congruence family join = congruence closure).
`--domain` (see the "Abstract domains and stub portability" section) can switch the judgment criterion to a single backend's
join semantics.

### Execution mechanism

- **Differential execution is by default wrapped in a Bubblewrap sandbox**: the real side, stub side, and IKOS witness binaries are all started with separate
  PID/network/IPC/UTS namespaces, enter a new session, and exit when the parent exits;
  the root filesystem is mounted read-only, `/proc`, `/dev` are rebuilt as a minimal runtime environment, and only the current
  differential run directory is writable. Side effects of process-control, signal, network, and file-write functions are thus confined
  to the process-under-test's own namespace and temporary directory. Without Bubblewrap, native execution
  fails closed, recorded as harness failure, never running the real function bare;
- **The driver contract goes over fd 4** (`__dt_*` helpers, shared by both sides), never touching stdout — the function under test's own
  prints cannot pollute the observation; buffer registration is the boundary oracle: `__dt_buf`/`__dt_buf_out` (pure-output
  buffers are filled with canary first; the 0xAA/0x55 double run catches "written value equals the original value");
- **The stub side is compared on claims, not content**: the `__ikos_*` calls in the stub are written by `difftest/recorder.c`
  into a log (`NONDET`/`ASSERT`/`CHECK_MEM`/`HAVOC_FORGET|ABSTRACT`/`ASSUME_VAL`/`MALLOC`,
  addresses resolved to (tag, offset)); the judge subtracts the real write set from the havoc claims. The shadow header
  `difftest/include/ikos/analyzer/intrinsic.h` turns all intrinsics (including the macro-form
  `__ikos_assume`) into interceptable extern functions;
- **A schedule battery drives nondet**: the all-0 probe learns the number of nondet calls K, then force-injects
  `{truncated(real return), ±1}` (pushing the stub near the real value), boundary values, all-0/all-1, and 16 seeded
  LCGs, capped at `--dt-schedules`; runs violating `__ikos_assume` are discarded by the analyzer's pruning semantics;
- **A real-side crash** (has `DT1 call` but no `DT1 ok`) = the input is outside the real precondition, recorded as n/a
  with zero weight; **harness failure** (driver can't be written/compile fails/contract broken) only warns and skips that gate,
  never falsely rejecting a possibly-correct stub;
- Per-input judgment ∈ `sound / sound*(reason) / unsound(reason) / unverified / n/a`;
  artifacts live in `<cache-dir>/difftest/<fn>/` (driver.c, inputs.json, report.json);
  the summary wording is "no unsound found across N inputs", never claiming "the stub is sound".

### ikos battery (from 2026-09-29, on by default: the analyzer itself as judge)

The "stub side + judge" above is **concrete execution + Python simulation** of ikos's domain semantics (convex hull ∩ congruence
closure hand-written math). The ikos battery replaces the simulation with the **real analyzer** (`tool/api-spec/ikosjudge.py`):

- **A deterministic harness per input** (zero LLM): `main(void)`, scalar parameters take argv-token
  literals, pointer parameters point to local buffers initialized with literal bytes (content taken from the real observation snapshot),
  and the stub is called once; after the call, each probed cell is loaded into a **named local** (debug print by source variable name),
  `__ikos_print_values` prints the abstract interval of the return value / per cell and the points-to/offset/
  nullity of pointers; then one `__ikos_assert(r != y)` / `assert(cell != b)` exclusion assertion per line.
- **Compile chain**: the harness and the stub are each compiled with the ikos clang flags → **each `opt -mem2reg`**
  (at O0 the assume's refinement lands in the loaded register and is not written back to the cell — same as
  the ikos-stub fix of 2026-09-29) → llvm-link → `ikos -a=dbg,prover -d <domain>`;
  the include order for the generator, self-check, and witness is that this repo's
  `analyzer/include` takes precedence over the install prefix, so a developing `__ikos_malloc` does not first hit the old
  `intrinsic.h`; `ikos-stub link` depends on the same header after the install side is synced;
  prints go to stdout (not level-gated), and assertion judgments enter the sqlite checks table
  (0=proved / 1=unknown / 2=refuted; kind 0 = unreachable).
- **Witness validation (witness, the safety core)**: pinning argv→actual arguments is underdetermined (the DT1 contract never records
  scalar argument values / the parameter↔buffer mapping), and a wrong pin would let ikos prove exclusion on a non-real input —
  a false unsound on a sound stub. The same harness source is compiled once more without `-D__IKOS__` (intrinsics
  become empty macros), linked against the **real library**, and run natively (`printf` under `#ifdef IKOSJUDGE_WITNESS` reports
  ret/cells/base addresses). The witness and the differential binary use the same Bubblewrap policy, and `cwd`
  is fixed in their own temporary directory — **an exclusion-assertion PROVED is only accepted when the witness reproduces the real observation**; a wrong pin
  degrades to discarding that input's evidence (per-input fallback to the simulation path), never false unsound.
- **Judgment** (`difftest._ikos_phase`, per input):
  - exclusion assertion PROVED + witness matches → **unsound** (reason cites ikos and the domain, manually reproducible);
  - a `__ikos_assert` in the stub REFUTED by ikos + witness ok → **unsound precondition too strong** (the analyzer's
    true semantics, reinforcing the recorder-side phase_a);
  - ret assertion REFUTED (the stub **can deterministically** return the real value) → return axis sound, **skips** the simulation
    `_phase_c` (the wide-hull under-drive's unverified disappears with it);
  - all unreachable → **unverified** (the analyzer prunes every path after the call).
- **Pointer points-to judgment (one-vote dimension)**: ikos's points-to set/offset interval/nullity are over-approximation claims, and the
  **witness's concrete pointing must fall inside the claim**: for the returned pointer and cells containing a pointer value
  (`T**` write-back, strdup/strtol-endptr class) three judgments — block ∉ set → unsound "wrong
  block (analyzer points-to)", offset out of interval → unsound, nullity reversed → unsound;
  non-allocator OTHER blocks go through the old classify_pointer fresh-block-gap approximation;
  allocator-like targets require the fresh dynamically-allocated capability produced by `__ikos_malloc`.
- **Game** (coevolve): ikos intervals enter `spec_abstraction` (`ret_ikos`/`cells_ikos`
  axes, the claimed hull preferring the analyzer's true value), a new component `cell-<tag>` (per-buffer point-domain precision:
  mean normalized slack from real bytes to the claimed interval, violation = ikos-proven exclusion),
  and `ret-int`'s violation is OR'd with the ikos exclusion flag — **both the return value and the pointer-parameter point domain participate
  in Φ/d with the analyzer's true values**.
- **Domain mapping**: default (`--domain all`) → `-d interval-congruence` (an exclusion proven under the reduced product
  = at least a true violation under that backend, portable); a specified domain → use that domain (a relational domain makes
  `__ikos_assume_mem_rel`'s polyhedral claims actually compute, projection = the printed interval).
- **Flags**: `--dt-no-ikos` (back to pure simulation, old per-byte behavior), `--dt-ikos-cpu` (10),
  `--dt-ikos-wall` (60), `--dt-ikos-domain`. Honest skips: aggregate returns, byval aggregate
  parameters, float returns (cells still judged), buffers >4096B (4096 before pinning).
- **Cost**: small harnesses are sub-second to seconds; a typical game ≈54 pairs (input×battery), with in-process caching
  (key = stub body + harness + domain content hash) absorbing identical-body re-judgment; a typical game adds about +1-2 minutes.

### Analysis-side quality gates (2026-10-09, added after the E2 audit)

The E2 experiment (30 real projects) exposed a batch of defective-stub categories that the existing gate structure could not stop, so the mainline
added three deterministic gates, plus one post-link audit tool:

- **Reinforced semantic rules for differential judgment**: return-value aliasing (an fopen64 stub returning the path argument as-is, the alias dangles after the caller
  frees it), stack return (a g_malloc stub `return &p0`), pointer/integer confusion
  (allocator-like targets (e.g. a `_TIFFmalloc` stub `(void*)p0`) hard-require a fresh block;
  other small-integer pointers keep a note), and havoc and assume_mem_size
  crossing the registered buffer boundary (a stat64 stub writing 256B into a 144B `struct stat`) judged unsound per input;
- **noreturn enforcement**: when an exit stub does not model noreturn, `if (n < 0) { free(x); exit(1); }`
  continues executing afterwards, manufacturing use-after-free/double-free false errors. For any function whose name hits the builtin table
  (exit/abort/__assert_fail/pthread_exit etc.) or whose declare line carries the `noreturn` attribute,
  the body must end with `__ikos_assume(0);` (previously perror-style side-effect modeling
  was allowed); lexically conservative judgment, the rejection reason enters the repair loop, and co-evolve revised bodies and stubdb reuse
  are subject to the same constraint;
- **Stub-body IKOS self-check**: E2 found a total of 378 stub-side definite errors across the 30 projects
  (97.6% buffer-overflow) — the fact that the stub itself is proven wrong by the analyzer is invisible to the differential gate. Now every
  candidate that passes compile + signature (and stubdb reuse entries) is analyzed once by ikos before the differential gate:
  a zero-LLM driver with nondet everywhere (scalar `__ikos_nondet_int`; the object sizes of pointer parameters and pointer return slots
  are provided by the **cxprobe call-context probe** — extracting the true types of all
  inputs/returns from bitcode disassembly, computing named-struct sizes exactly via LLVM constant folding (a GEP
  probe with the original datalayout), and majority-voting `i8*`/`void*` type-erased parameters by call-site back-lookup;
  when the probe is unavailable, fall back to header mappings such as sys/stat.h or a 128B buffer with full `__ikos_forget_mem`),
  `-a=boa,nullity,uva,prover,dfa`, default domain; any definite
  error whose source file is in the stub file itself is rejected, warnings are only recorded, and a harness that cannot run is recorded as no evidence, never falsely rejected
  (`--no-self-check` turns it off). Measured: bzip2's `stat64` probe finds the real `struct stat` =
  144 bytes, and the old-style `forget_mem(p1, 4096)` bad stub is `failed` at generation time and not installed.
  The probe itself is independently debuggable: `python3 tool/api-spec/cxprobe.py X.bc <fn>...`;

Special-control-flow functions no longer let the LLM generate freely: in E2, libjpeg's `_setjmp` stub connected a spurious
error path and manufactured a NULL false error. setjmp/longjmp family, C++ exception runtime
(`__cxa_*`/`_Unwind_Resume`), vfork/clone all report `unembeddable` on hit with zero LLM
budget; `--allow-unembeddable <name>` can be used to release them one by one for experiments.

The last gate after linking is **`auditgate.py`** (a standalone tool, not part of generate): compare the original
program and the `ikos-stub link`ed program's two IKOS analysis libraries; if the stub-side definite-error count exceeds
`--max-stub-errors` (default 0) or the project-side unreachable-statement growth exceeds
`--max-unreach-delta` (default 5%), it exits non-zero — the latter prevents under-approximation from stealing coverage: in E2
a getopt_long stub with a too-narrow return domain pruned the ASCII 'o' out of the reachable set, turning the whole `case 'o':` branch into dead
code, and memcached thereby "lost" 74 baseline errors (coverage loss disguised as precision gain).
Usage: `python3 tool/api-spec/auditgate.py --help`.

`auditloop.py` closes the loop on the first category of findings (stub-side definite errors) on top of auditgate:

1. Audit all input bitcode by the same criterion (baseline libraries reuse the adjacent `X.db`, computed on the fly when missing);
2. Aggregate stub-side definite errors by stub file into function-level findings (the filename is exactly the function name `--only` accepts);
3. **Evict the stubdb entry for that function first, then regenerate** — generate checks the database before the LLM, and
   the evidence of a whole-program-proven definite error outranks the battery that admitted the entry back then;
4. Inject the audit finding as `--repair-hint` into the regeneration (the hint enters the response cache key and will not
   hit the old body);
5. Re-audit until the stub side is clean or `--max-rounds` (default 2) is exhausted; `--prune-failed`
   deletes stub files that still violate — a function remaining unmodeled is better than entering the analysis with falsified semantics.

The unreachable change rate is only reported, not used as a repair hint: this kind of under-approximation signal cannot currently be reliably attributed to
a single stub. Exit code 0=clean / 1=still violating (or a stub was deleted) / 2=tool failure; `--json`
outputs the full round report, `--jobs` audits and repairs in parallel.

### Scenario driver (from 2026-10-09, differential input-surface upgrade)

The differential battery's largest failure class (R16 at 55.9%) is the real side having no contract-usable input: the bare
driver calls the target function directly, and a real implementation lacking initialization state crashes/times out/is unstable on the spot
(`regexec` lacks `regcomp`, `fdopen` lacks `open`). The scenario driver lets the LLM execute
a setup call sequence to construct legal state **immediately before exactly one target call**:

- **Upgrade ladder**: by default run the bare driver first (easy functions at zero extra cost); only after the real-side pre-probe
  (2 canary × 2 repetitions per input; all 4 runs ok and returns consistent to count as usable — a garbage-
  state driver's flaky judgment must not rely on a single sample) judges total wipeout does it regenerate
  the driver in scenario mode and rerun the battery; the scenario crash's stderr/ASan summary is attached to the regeneration prompt;
- **Tier 1 (on by default, `--dt-no-scenario` turns it off)**: the setup whitelist is limited to libc
  constructors (`open/regcomp/socket/pipe/mkstemp/tmpfile/opendir/fopen/
  pthread_mutex_init/malloc/calloc` etc., 17 entries, injected into the prompt with purpose notes),
  natively usable on both sides, zero link changes; setup ≤4 times (guard budget 6), the target call still exactly
  once, and setup products are registered with `__dt_buf/__dt_buf_out` before the target call;
- **Tier 2 (experimental, `--dt-scenario-lib` enables it)**: setup extends to in-library functions
  (e.g. `xmlInitParser`), candidates = cxprobe call-graph adjacency ∩ the reallibs-resolvable set
  (≤8); the stub side is modified to **also link the real library** — the stub object comes first +
  `-Wl,--allow-multiple-definition` makes the stub's target symbol win, and the setup is
  the real implementation on both sides (verified by unit tests and end-to-end linking);
- **Observation invariance**: the judge's judgment surface is unchanged (still the target call's ret/wrote/events);
  setup only shapes the input. Smoke test (regexec @ haproxy): bare driver total wipeout → pre-probe
  conviction → after upgrade the LLM writes the textbook `regcomp(&preg,…)` → `__dt_buf` registration →
  `regexec` → `regfree` sequence, and the failure reason changes from "real side has no usable input" to the normal
  semantic-counterexample repair loop.

### Private D-Bus session inside the sandbox (from 2026-10-09, differential environment-surface upgrade)

bluez's `dbus_*` functions (R18's largest residual failure cluster, 58) need a live D-Bus session
bus, while the differential sandbox does network/IPC isolation. Mechanism: for functions hitting the name prefix (`dbus_`/`g_bus_`/
`sd_bus_`, judged by IR name and source name; `DT_DEFAULTS['sandbox_dbus']` on by default),
all of their `run_once` (real/stub/probe) are executed **inside** bwrap via
`dbus-run-session --config-file <conf> --` — one private daemon per run,
the socket explicitly placed at `<workdir>/xdg/bus` (a bundled config avoids dbus-run-session's across-version
TMPDIR behavior differences), `XDG_RUNTIME_DIR` and `TMPDIR` point at `<workdir>/xdg`
(0700); the socket never leaves the sandbox — zero host pollution, zero cross-run interference. The config's bus policy is
allow-all: this bus is one-shot and unreachable from outside the sandbox.

Naturally compatible with the scenario ladder (`dbus_bus_get` as in-library setup shares the same private session with the target).
Decisive smoke test (`dbus_bus_request_name` @ bluetoothctl): after the scenario driver's
`dbus_bus_get` establishes the connection, the target stably returns
`DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER` on the sandbox's private bus across the double canary — real libdbus really owned the name.

Known boundaries: when `dbus-run-session` is missing, warn once and fall back to the ordinary sandbox (different from Bubblewrap's
fail-closed, but likewise never touches the host bus); ikosjudge's witness run does not carry the
bus wrapper, and its evidence is discarded by the existing fallback; libdbus directly aborts on illegal parameters, and edge-
input robustness belongs to the game input-quality category, unrelated to this mechanism.

### Security (LLM code compiled and executed on the local machine)

The driver first passes a **deny-by-default guard**: include whitelist, call whitelist, token bans
(`errno`/`__asm__`/`#pragma`/`__attribute__`/`volatile`), target function exactly once and between
the contract lines; execution adds rlimits (CPU/AS 512MB/FSIZE 1MB/NOFILE/NPROC), an empty temporary cwd,
environment-variable pruning, and a per-run timeout. The above rlimits run inside
`bwrap --unshare-all`'s namespace/filesystem isolation, forming a defense-in-depth of "static guard + resource limits +
OS isolation"; without Bubblewrap installed, native execution fails closed. The sandbox significantly
limits misuse of host resources, but it still should not be stated as an absolute security boundary that can withstand
kernel vulnerabilities or container escapes. The real-side ASan build needs a
`setarch -R` wrapper on high-entropy-ASLR kernels (already handled automatically).

## Coevolutionary game (potential-game engine)

The differential gate only guarantees **reliability** (the stub is an over-approximation); it exerts no pressure on a spec that is "absurdly wide yet yields no inconsistency signal".
The generate pipeline already has two generative agents — **B = stub
body generator** (whose inferred specs/invariants are the `__ikos_*` stub body) and **A = the differential gate's
driver/input generator** (which generates program-execution inputs and collects execution data). `--co-rounds` (default
3) establishes their interaction as a **measurable repeated game**, moving the artifact from "reliable" to "precise".
**All metrics (d/complexity/coverage/reward/payoff) are computed by the deterministic harness; the LLM only selects strategies
and only produces artifacts**; the soundness gate is always outside the game as a backstop (a round-0 body must first pass the differential gate to qualify
for the game; the final body must pass the battery again, otherwise it goes through the repair loop as usual).
`game.json`'s `stopped` distinguishes a convergence state (e.g. `potential-stable`) from round-budget exhaustion
(`budget`). Either way, the installation precondition is that the final complete input pool has no `unsound`; if the boundary
recheck itself fails, the whole thing is rejected for installation as `harness-error`.

### Formal model

**Players, actions, and information structure.** The player set N = {A, B}. A's action
a_t = (σ^A_t, τ_t): a discrete strategy σ^A_t ∈ Σ_A plus a batch of new inputs τ_t (argv lists);
B's action b_t = (σ^B_t, β_t): a discrete strategy σ^B_t ∈ Σ_B plus the revised function body β_t
(a sequence of `__ikos_*` statements). The stage game is **alternating moves** (A moves first): A moves on the previous settlement
→ battery settlement → B moves on a settlement that **already contains A's new inputs from this round** → battery settlement,
two phases per round. The second mover sees all the consequences of the first mover's current-round action; the first mover's action only
manifests through the next settlement — this is exactly the information structure required by asynchronous better-response dynamics. Settlement is **perfect monitoring**: every
settlement's full metrics enter the next mover's public prompt, no private information, no hidden actions.

**State and interval deviation.** s_t = (T_t, I_t, I^A_t, I^B_t): the input pool T (monotone non-decreasing,
argv de-duplicated), the current body I, the execution-data abstraction I^A, and the spec abstraction I^B. The two abstractions take values, isomorphically,
in a lattice over the component set C (ret-int's hull and congruence class, ret-ptr's nullity/block class/
per-block offset hull, per-tag write set). The interval deviation is

```text
d(I^A, I^B) = (1/|C|) · Σ_{c∈C} [ dist_c + w_v · ν_c ]
```

where dist_c ∈ [0,1] is the normalized distance (boundary difference ÷ claimed width / symmetric difference ÷ union / lattice distance /
congruence axis +0.5), ν_c ∈ {0,1} is that component's **inconsistency signal** — an observation falling outside the claimed admission
set, the same criterion as the judge (including the wide-hull exemption); w_v = 8, so d ∈ [0, 9]. w_v
is deliberately set above the dynamic range of all non-violation terms (each component's non-violation distance ≤ 1, and λ·K, μ·Cov are ≤ ~1 under any
lexically sane body) — **arithmetic lexicographic order**: any component's tightening is outweighed by a single
inconsistency signal, for both Φ and ΔΦ (configuration invariant: w_v > λ·K_max + μ + 1).

**Potential function and payoffs.** Φ(s) = d(I^A, I^B) + λ·K(I) − μ·Cov(T), where K is the log-scale
complexity and Cov is the boundary coverage of the claimed surface (smaller is better). Write ΔΦ_t := Φ(s_t) − Φ(s_{t+1})
(Φ decreasing is positive):

```text
u_A(a_t) = ΔΦ_t + α · E(a_t),   E ≥ 0 is the first-discovery count (new hull endpoints, new block classes, new write tags)
u_B(b_t) = ΔΦ_t − β · P(b_t),   P = max(0, K(s_{t+1}) − K(s_t))
```

**Default α = β = 0**: u_A = u_B = ΔΦ holds identically — this is the shipped default configuration, not a special case.
The exploration reward E degrades to A's **tie-breaking heuristic** (when payoffs tie, prefer inputs that touch an untouched surface; only the α > 0
ablation arm treats it as a payoff term, and that arm's user prompt explicitly notes "ExplorationBonus is
active").

**Potential structure (the default configuration is the configuration covered by the theorem).** Factory default α = β = 0: u_A = u_B =
ΔΦ holds identically, an identical-interest game: −Φ is the **exact potential function** (any
single player's unilateral deviation changes the payoff by exactly the potential change, Monderer & Shapley 1996), and along the trajectory
it telescopes — Σ_t ΔΦ_t = Φ(s_0) − Φ(s_T), so over a finite horizon joint-payoff maximization =
final-potential minimization. An exact-potential game has the **finite improvement property (FIP)**: on the finite state quotient, **asynchronous
better-response** dynamics reach a pure-strategy Nash equilibrium in finitely many steps, with no improvement cycles. The implementation executes exactly this
dynamic — A moves → settle → B moves → settle, the two sides never update in parallel, so the oscillation vulnerability unique
to parallel updates does not exist; and **acceptance does not rely on the model's self-awareness**: the τ acceptance rule prices
B's revision before settlement (measure before settle), and a revision with u_B ≤ τ is rolled back by the harness to the current
body (trajectory marked `-rejected`, the rollback reason backfilled into the next round's prompt). Default τ = −ε: an accepted
revision is an **ε-tolerance better response** — degradation no worse than the equilibrium tolerance is kept (the battery-surviving hull
under-estimates some tightenings; a tightening invisible to the metrics but real in the analyzer is kept by the tolerance), and τ = 0 is
the strict arm (only Φ-decreasing revisions are kept). The violation weight w_v = 8 exceeds the dynamic
range of all non-violation terms, guaranteeing that a "revision manufacturing an inconsistency signal" is priced far below any τ ≥ −ε, and is
rejected in every situation, with acceptance independent of the rest of the state.
The evidence side (A's inputs, the vetoing battery's injections) is **never rolled back**: an input exposing an inconsistency signal raises
Φ yet is the globally most valuable action, and rolling back evidence is equivalent to hiding unsound. Compound termination argument (stated
honestly): evidence is monotone non-decreasing, its abstraction quotient is finite (component values bounded by bit-width and registered buffers, argv
de-duplicated, body text unbounded but the lexical guard limits the vocabulary, the abstraction only looks at the finite quotient), each piece of new evidence
refines the quotient by at most one step; under fixed evidence, an accepted revision is no worse than the tolerance (Φ is bounded below, and strictly decreases when there is a
measurable improvement), the tolerance plateau is observably declared by the stopping criterion and bounded by the budget
— strict acyclicity holds only under the strict arm τ = 0. The "potential-stable"
criterion (|ΔΦ| < ε over a complete A+B cycle ≈ stagnation of the improvement path) is the observable
form of FIP.

**The approximate potential is only an ablation arm.** When α, β > 0, the additional terms are bounded (E, P bounded by the finite lattice and restricted vocabulary),
u_A, u_B deviate from ΔΦ by at most α·E_max and β·P_max respectively, hence a **δ-approximate potential
game** (δ = max(α·E_max, β·P_max), upper bound empirically small, no a priori tightness claimed): A's exploration
reward can temporarily raise Φ (sawtooth → decline, the subject of RQ3). The mixed motive lies precisely in the perturbation terms —
the main term is same-signed (jointly pressing d down), the additional terms are opposite-signed (A rewards exploration, B penalizes complexity); the perturbation arms'
user prompts explicitly note that the perturbation is active. Residual distance stated honestly: the LLM approximates the **proposal** (which of a finite
menu is the better response), while the **execution** dynamics are guaranteed by the acceptance rule to be an improvement no worse than the tolerance
(default τ = −ε, τ = 0 the strict arm); arrival is still
observably declared by the triple criterion of potential-stable / empirical ε-Nash / budget.

**Strategy update and equilibrium.** σ_{t+1} ∈ argmax_{σ∈Σ} E[u | σ] uses **realized payoffs** as the
signal (each phase's settlement backfills the next mover's prompt; the argmax is executed by the LLM over a finite menu,
the random / fixed arms are ablation controls). The equilibrium concepts (potential-stable, empirical ε-Nash — valid only over the **realized
action set**, and must be falsified by an incentive-independent deterministic verification battery before declaration) and the budget floor are in the
round-loop section below.

### Formal objects → measurable instantiation

| Formal concept | Implementation ([coevolve.py](coevolve.py)) |
| --- | --- |
| `I^A` execution-data interval abstraction | dual-canary contract observation per component: ret-int hull+gcd closure; ret-ptr nullity/block-class set/per-block offset hull; writes per-tag offset union (snapshot-failed `'?'` tags' three-state is not counted); witness version's concrete ret on pinned inputs |
| `I^B` spec interval abstraction | the recorder's **surviving** claims under the schedule battery are abstracted with the same structure (paths pruned by assume not counted) **merged with the ikos truth axis**: `ret_ikos` (the analyzer's computed return interval, the claimed hull prefers it — covers the entire nondet space rather than only the driven schedules), `cells_ikos` (per-buffer 8-byte-window point-domain intervals), and per-axis exclusion flags |
| `d(I^A, I^B)` | per-component normalized distance averaged: interval components = boundary difference `abs(lo_A−lo_B)+abs(hi_A−hi_B)` ÷ claimed width; set components (block set / write set) = symmetric difference ÷ union; nullity = lattice distance; congruence axis = +0.5 when observed gcd>1 while claimed gcd=1 (the target of `tighten-congruence`); point-domain component `cell-<tag>` = mean slack from real bytes to the claimed window interval (the width term prevents a [0,255] havoc from scoring 0 on a contained byte; violation = ikos-proven exclusion); **inconsistency signal** (observation outside the spec, including congruence-closure exclusion, wide-hull exemption, the same criterion as the judge) per component ×w_v=8, component contribution bounded [0,9] (w_v above all non-violation terms' dynamic range = arithmetic lexicographic order) |
| `Complexity(I)` | number of `__ikos_*` calls + number of integer constants + ternary nesting depth (×2), log2/8 normalization |
| `Coverage(T)` | fraction of the spec's claimed surface touched by execution data (ε_face = max(1, width/1024)): the hull's two endpoints, the NULL capability, per-block offset's two ends, write set's two edges |
| `ExplorationBonus(a)` | first-discovery count: new hull endpoints, new pointer classes, new write tags (with default α=0 it is A's tie-breaking heuristic, not entering the payoff) |
| `ComplexityPenalty(b)` | ΔComplexity (truncated ≥0; with default β=0 not entering the payoff) |
| coefficients | `--co-lambda/--co-mu/--co-alpha/--co-beta/--co-epsilon/--co-tau` all CLI-sweepable |
| τ acceptance rule | `measure()` before `settle()` prices B's candidate revision, u_B > τ before it is booked (default τ = −ε, ε-tolerance better response; a revision manufacturing an inconsistency signal is necessarily rejected by the w_v range suppression; τ = 0 strict arm); otherwise roll back the body, reuse the current battery record (the battery is deterministic), trajectory marked `-rejected` |

### Strategy space and strategy update

Both sides hold a discrete strategy menu; the prompt gives the **full payoff function + the per-component decomposition table of Φ + historical
record**, and asks for the strategy with the highest expected payoff to be selected and executed:

- **A** (output = strategy id + a batch of argv, JSON): `targeted-boundary` (hit the largest-
  distance component / untouched surface), `distribution-shift`, `shape-variant` (new argv shape),
  `replay-mutate` (history-guided), `deterministic-sweep` (boundary-value battery);
- **B** (output = strategy id + new body, same output contract): `tighten-interval`,
  `tighten-congruence` (**complete** finite enumeration, gcd=1 admits all; incomplete enumeration is
  an inconsistency signal under the congruence domain), `shrink-havoc`, `drop-null-capability`,
  `simplify`, `widen-on-inconsistency` (forced after an inconsistency signal appears);
- **The operationalization of strategy update** = three policies (`--co-a-policy/--co-b-policy`):
  `greedy` (default, the LLM selects a move by the payoff landscape — the game's actual driving algorithm), `random`
  (uniformly specified by the harness, RQ2 random-feedback control), `fixed` (constant strategy, RQ1 fixed-
  opponent control; A fixed to replay-mutate, B fixed to tighten-interval);
- Each round's settlement writes the realized ΔΦ/u_A/u_B and the per-strategy record table, atomically, to
  `<cache-dir>/coevolve/<fn>/game.json` (Φ curve, full component decomposition, both abstractions,
  final body); `--co-report` additionally saves a summary.

### Round loop and stopping conditions

round 0 = the existing artifact (B's body₀ goes through the original generation pipeline, A's driver + initial inputs use the cache);
each round has two **alternating** phases: **A moves first** — moves on the previous settlement, new inputs enter the pool (pool monotone non-decreasing,
argv de-duplicated), the battery settles and backfills, **evidence is never rolled back**; **B moves second** — revises
the body on a settlement that already contains A's current-round new inputs, pricing before settlement: only a revision with u_B > τ is booked (default τ =
−ε), otherwise the harness
rolls back to the current body and that phase settles as an unchanged state (trajectory marked `-rejected`, the rollback reason backfilled
into B's next-round prompt). A phase with unchanged state (A produced no new input / B's body unchanged or
rolled back) directly reuses the previous battery result (the battery is deterministic; rerunning yields the same record).
Dual build + dual canary + schedule battery execute per phase, each phase's settlement atomically writes a checkpoint.
The stopping decision is made after each settlement, and requires that **the most recent two settlements exactly cover one A phase and one B phase**
(a complete asynchronous cycle in which both sides failed to improve is required to allow declaration — one side's single quiet round is not enough),
plus the current absence of an inconsistency signal:

- **potential-stable**: within that cycle |Φ(s_t) − Φ(s_{t−1})| < ε;
- **empirical ε-Nash** (honest wording: valid only over the **realized action set**, not claiming true NE):
  both sides |u| < ε and neither is better than its own historical best realized payoff − ε;
- budget `--co-rounds` exhausted (one round = one A phase and one B phase; when an equilibrium is vetoed by the verification battery,
  that round's budget is consumed by the veto).

**The budget is the reliable floor**: perfect abstraction is unreachable, iteration must be bounded and the floor
reliable. Throughout the game, a witness list records **every body that passed the battery on the then-complete input pool**
(in acceptance order, consecutive duplicates collapsed); when the budget (`--co-rounds` × `--max-retries`,
both set on the command line) is exhausted and the final body still carries an inconsistency signal, the tool re-verifies, newest to oldest, on the **final input
pool** (the pool is monotone non-decreasing, so earlier passes don't count), and hands back the **newest passer** to the caller. The τ acceptance
rule guarantees that an over-tightened, rolled-back revision does not stay in the game, so the floor only needs to fight the one case "evidence grew and punctured a later
tightening"; when the repair loop also fails entirely, the function **installs a witness rather than reporting
failed** — iteration is bounded, and the returned body must have passed a complete differential battery at least once (**reliability before
precision**). game.json's `reliable_body` field records the floor. Likewise, an LLM
endpoint outage (network failure) mid-game does not puncture the run: it degrades to a single-shot differential gate re-verification of the candidate (driver/inputs
from the response cache, no need for the endpoint to be alive), losing only the game's precision rounds.

**Equilibrium verification battery** (guards against same-model self-play collusion / false equilibrium, 0 LLM cost, independent of any incentive): before declaring an equilibrium, replay the edge-token variants of the pool's recent shapes on the current driver with deterministic-sweep;
an inconsistency signal found → equilibrium **vetoed**, the new evidence merged into the input pool, the game continues
and re-judges (the veto consumes budget).

(The potential-structure argument — the exact potential / FIP / δ-approximate potential and the boundary of the convergence claims — is in the
"Formal model" section above.)

### The game over the pointer abstraction layer

The pointer's four components enter the game the same way as the numerical layer:

| Component | d | A's attack | B's response |
| --- | --- | --- | --- |
| nullity | lattice distance | inputs inducing NULL (empty string / error path / NULL token) | drop-null-capability (tighten) or keep |
| block set | symmetric difference | double buffers / different arguments exposing different blocks or fresh | narrow the block claim (non-allocator fresh bidirectionally exempt, keeping the sound\* criterion; an allocator's `FRESH` must not be removed) |
| offset (per block) | boundary difference | last-character / extreme-offset inputs | tighten-interval(offset) |
| writing pointer values | counted in the write-set d | inputs inducing pointer writes | shrink-havoc / forget-abstract choice |
| ret-int congruence axis | gcd mismatch +0.5 | hit congruence-class exclusion values | tighten-congruence (complete enumeration) |

### Ablation arms and paper data surface (corresponding to the RQ3 ablation in exp/RUNBOOK.md)

- **RQ3.1 the game's contribution** (coevolution vs single-agent/fixed opponent): `--co-b-policy fixed`
  (B constantly tighten-interval) and `--co-rounds 0` (no game) as controls, comparing final d
  and tightening magnitude;
- **RQ3.2 potential-driven vs random**: `--co-a-policy random` / `--co-b-policy
  random`, comparing Φ convergence speed and inconsistency-signal resolution rate;
- **RQ3.5 convergence**: game.json's Φ/d/coverage per-settlement trajectory (two phases per round, A/B) +
  ε/τ/α/β parameter sweep (`--co-epsilon`/`--co-tau`/`--co-alpha`/
  `--co-beta`; α/β leave the exact potential into the δ-approximation arm);
- **precision and boundary coverage** (the game side of RQ2.2's precision impact): final `d`, claims-hull
  width convergence ratio, Coverage, equilibrium veto count.

### The game's boundaries (stated honestly)

- empirical ε-Nash is valid only over the realized action set; default α=β=0 is the theorem-covered exact-potential game
  (default τ = −ε: accepted revisions are ε-tolerance better responses, violating revisions necessarily rejected; τ = 0 is the
  strict-improvement arm), the approximate potential appears only in the α,β>0 ablation arms;
- equilibrium is **evidence-bounded**: bounded by A's attacks + the verification battery not breaking it, not claiming the invariant is optimal
  (the `{-1,1}` "equilibrium" against strcmp in the tests, vetoed by the battery's one-token variant, is the example);
- the witness floor's evidence bound = the complete differential battery of the game it belongs to (cross-game input pools are not
  unioned); `--co-rounds 0` (single-shot gate) has no witness, behavior unchanged;
- the collusion risk of same-model self-play: the deterministic-sweep verification is the only incentive-independent defense;
  the random/fixed arms double as control baselines;
- the battery's forced injection (real value ±1) itself pushes the surviving claims toward the real value, so a "wide body"
  has a small d baseline — d measures the claim-observation deviation **under battery driving**; a metric-
  invisible tightening (surviving hull unchanged, claimed width narrowed) is accepted by the ε-tolerance, and its analyzer payoff d
  is invisible — this is exactly why the default is τ = −ε rather than 0;
- non-allocator fresh blocks, float values, and the errno side channel keep the differential gate's expressiveness-gap criterion; A's driver
  is not revised in-game; precondition slack is unobservable, not done this round.

## Stub function database (cross-program reuse)

The generated evidence is **program-independent**: a body that passed the differential battery on one program's
`declare i64 @strtol(i8*, i8**, i32)` is equally valid on any program holding the same
declaration. The stub database (`--db-dir`, default
`~/.cache/ikos-api-spec/stubdb`) turns this evidence into cross-program reuse. **Identity** =
**source name + signature key** (`signature_key`, the same ruler as `ikos-stub link`),
a variant signature never wrongly claims another's body; entries generated with `--domain` targeting additionally carry a **domain axis** (portable
entries occupy the bare key and are visible to any query; targeted entries occupy `signature key|domain=<name>` and only serve same-domain queries,
see the "Targeted generation by numerical abstract domain" section).

### Storage layout: per-function shards, built for tens-of-thousands scale

Analyzing multiple large projects accumulates thousands to tens of thousands of functions, so the database is **not** one large JSON file,
but one small shard per function:

```text
stubdb/
├── entries/
│   ├── strtol.json      # all signature variants of this function -> entry (~KB scale)
│   ├── fopen64.json
│   └── ...
└── index.json           # legacy single-file layout: read-only compatibility, deletable once shadowed by the shards
```

- **record only rewrites that function's own shard**, cost independent of database size — batch generation of 500
  functions does not rewrite the rest of the database (the single-file layout degrades to O(n²)
  full rewrites at this scale; at 20k entries one batch is GB-scale wasted writes); a query reads one shard, zero process-
  startup loading. Measured: 10k entries inserted in 0.7 seconds;
- **git-friendly**: one file per function = incremental diffs, per-function merges, almost no
  conflicts; point `--db-dir` at an in-repo directory to share with the team (entries contain no keys,
  only body/declare/model name/verified flag/timestamp).

### generate: check the db first, then generate, install into the db on success

- After rendering the skeleton, each function **checks the database first** (before the response cache read — a verified body for the same
  declaration is stronger evidence than one cached model reply, and it is zero-cost): hit →
  the body is re-spliced into the **current program's** skeleton, re-passes the lexical/compile/signature gates → installed, status
  `reused`, 0 LLM calls; any gate fails → warn and **fall back to normal generation**
  (the database is a cache, not an authority);
- The differential battery is **not rerun on reuse**: the evidence is bound by declaration, not by calling program. This is
  an honestly-stated trust boundary — the battery was run against this machine's real libraries, and reuse implicitly assumes the target
  program links the same library (holds within the same analysis pipeline; confirm yourself when sharing `--db-dir` across
  machines);
- Both install points enter the db (the main success path and the reliable-floor path): the `verified` flag = whether the differential
  gate was on. **Ranking**: verified(2) > compiled(1) (`--no-difftest`
  artifacts), a higher rank overwrites, an equal/lower rank keeps the existing entry — the database grows more trustworthy with use, never degrades;
- db insertion is best-effort: a database write failure only warns, never failing a function that passed all gates.

### prefetch: 0-LLM pre-fetch before analysis

```console
$ python3 tool/api-spec/ikos-api-spec.py prefetch -o stubs prog.bc
| strtol |  | reused | 0 | stubs/strtol.c (stub database: ...) |
| strtoul |  | missing | 0 | not in the stub database (run generate) |
```

Analyze the bc's external functions ahead of time: missing and in the database → install (`reused`); not in the database →
report `missing` (detail "not in the stub database (run generate)"). The entire process is
**0 LLM calls**; when anything is missing it exits 1 and suggests running generate to fill in,
then `ikos-stub link` assembles as usual.

### The database's boundaries (stated honestly)

- **Single-writer stance**: the same concurrency model as the response cache — in-process serial writes, shard-level
  `os.replace` atomic flush to disk; concurrent writes from multiple processes overwrite each other (shard-level; the legacy layout was
  whole-database-level), no claim of concurrency safety; cross-person collaboration via git (per-function-file merges, almost
  conflict-free);
- Reusing a compiled entry (a `--no-difftest` artifact) is equivalent to trusting only the compile gate, with semantic
  guarantees as configured at generation time; verified evidence is bound to the real libraries on the generating machine
  (trust boundary in the previous section);
- Storage is deliberately isolated behind [stubdb.py](stubdb.py)'s interface, so swapping in a server
  backend later won't touch the pipeline.

## Abstract domains and stub portability

The generated stubs express everything only with `__ikos_*` intrinsics and are **naturally domain-pluggable** — the same stub can be
called by any `-d` backend (interval / congruence / dbm / apron-* etc., 21 domains),
no need to regenerate per backend. Generation judgment defaults to **dual admission** (convex hull ∩ congruence closure):
the real return value must fall in both the convex hull and the congruence closure of reachable stub returns, guaranteeing the stub is sound under any backend;
experimental analysis uses `-d interval`, but the generation judgment is still dual admission (no `--domain` passed,
default `all`).

(The tool also supports `--domain <name>` targeted generation — switching the judgment criterion to a specific backend and
writing relational constraints for relational backends; experiments did not enable it; expand per difftest.py's
`admission_mode` / `relation_class` when needed.)

## Boundaries

- `skip-interceptor` (`\x01` prefix) and `skip-inalloca` (C++ by-value non-trivial types)
  are marked "cannot generate" in the summary table and must be handwritten (same criterion as `ikos-stub`);
- Small non-trivial C++ `sret` returns and unnamed-parameter external variadic functions (e.g.
  `__gxx_personality_v0(...)`) cannot be safely expressed by the current C skeleton; scaffold explicitly reports
  `unembeddable`;
- Rare IR types (half/fp128/vector) cannot render a skeleton, reported as `unembeddable`;
- **Real-implementation resolution (real-lib maps)**: the differential gate's real side needs to link the function under test's real
  implementation; resolution has three layers — the project's in-tree .a/.so (same source as the bitcode, most faithful,
  `ikos-api-spec real-libs <project>` generates `<project>/real-libs.tsv`), the system
  dependency-library whitelist (`real-libs.conf` + ldconfig, `--system-out` generates a shared
  system-real-libs.tsv), and implicit libc/libm (zero-config floor). generate passes
  `--real-libs <map>` (repeatable, first wins, in-tree first). **Strict gate**:
  functions whose real implementation cannot be resolved are skipped before spending any token (status
  `no-real-lib`; `--no-difftest` is not restricted) — an unverifiable stub is worse than an honest
  unmodeled. Link failure (e.g. static-library transitive dependencies) first goes through the LLM link-repair loop
  (`--dt-link-retries`); still failing, that function is `failed` and remains missing, never installing
  an unverified stub. From 2026-09-29, the wrapper rename fixed
  real-side linking of `__isoc99_*`/C++ mangled names (these functions had never actually
  run the battery before). Bring-up (building the real library from source for projects lacking a library) is in
  doc/REAL_LIBS_BRINGUP.md.
- `__ikos_assume`'s float comparison does not shrink the interval (int can, double cannot): a float postcondition
  written into the stub is still sound but adds no precision, and the prompt already tells the model to express it as an over-approximate return value instead (e.g.
  `return (double) __ikos_nondet_int()`; confirmed by the sin verification case);
- v1 generates functions serially one by one (rate-limit friendly); `llm_client.ChatClient` itself is thread-safe,
  and can be parallelized later.

### Differential verification's blind spots (stated honestly)

- Testing is not proof: it only covers the inputs under test; numerical returns are judged by "convex hull ∩ congruence closure" dual admission
  (covering the interval family and congruence family join semantics), but a **wide-hull** compound
  nondet return (e.g. two nondets concatenated) is not reached by battery driving, the closure is untrustworthy, and it is only
  admitted as sound\* by the interval (congruence-domain coverage unknown). **When the ikos battery is present this blind spot narrows**:
  abstract running naturally covers the entire nondet space, exclusion assertions with a verdict (PROVED/REFUTED)
  no longer depend on schedule driving, and the wide-hull under-drive's unverified disappears with it;
- `__ikos_assume`'s integer shrinking is **interval semantics**: under the congruence domain, a non-singleton refine
  degrades to top (`congruence.hpp`'s `refine(x, Interval)` degrades the interval to a congruence class),
  so assume only loses precision and is directionally sound; a nondet return narrowed by it is sound across all domains;
- The block-level gap of non-allocator fresh-block returns: on the strict lattice the stub is unsound, and argument-block aliasing is
  a toolchain-sanctioned approximation (judged sound\* and annotated per input); allocator-like APIs do not take
  this exemption and must be modeled as `__ikos_malloc` fresh blocks;
- Float returns are judged vacuous (ikos has no float value abstraction);
- Writes through hidden/double-indirect pointers and libc-internal pointers (FILE\* etc.) are unobservable; checks on unregistered pointers
  only check NULL;
- Real libraries not ASan-built (in-tree .a, system .so): mixed linking is legal, malloc/free are still intercepted through the PLT
  and the canary is valid, but **out-of-bounds/UB inside the library is invisible** (`--dt-no-sanitize`
  is the overall floor); version drift (system layer) at worst manifests as real-side crash/timeout → honest
  n/a or skip, never letting a bad stub through;
- errno/locale/global variables are side channels the stub cannot express (ikos likewise does not
  havoc globals for unknown externs, same criterion);
- Double-canary residual blind spot: bytes the driver itself pre-initialized; small still-inexpressible C++ `sret`
  returns are rejected at the scaffold stage (expressible large aggregate returns are judged by the full-object havoc rule
  in the table above);
  a stub with an empty surviving run set (assume prunes everything) first retries via argv seeds to restore decidability
  (see the "Targeted generation by numerical abstract domain" section), and only goes to manual review if still empty after retry;
- The retry's argv seeds are limited by the nondet vocabulary (int32 slots): 64-bit parameter values beyond that cannot be expressed by the seeds,
  keeping the unverified floor; the token→parameter-value mapping itself is a heuristic defined by the driver
  (an LLM artifact). The ikos battery is likewise constrained by the token→actual-argument heuristic, but **witness
  validation** limits the consequence of a wrong pin to discarding that input's evidence (never misjudging); `assume_mem_value/
  rel`'s cell refinement granularity is 8 bytes (lo/hi bit-width), and byte-level claims are only provable at the window
  granularity.

## Files

| File | Responsibility |
| --- | --- |
| `ikos-api-spec.py` | CLI + generation pipeline (skeleton split/splice, guard, noreturn enforcement and unembeddable short-circuit, compile and signature gates, stub-body self-check gate, differential-verification gate, cache, install; `real-libs`/`eval-real-libs` subcommands) |
| `tests/test_stub.py` | skeleton parse/render regression: LLVM quoted types, return attributes, multi-level function pointers, packed structs, C++ tag signature matching, `sret` rejection, and real clang compile validation |
| `tests/test_difftest.py` | differential regression: wrapper carrying C++ ABI typedefs, C++ reference prototypes, C++17 real-object driver compilation, mangled-symbol driver selection, hidden-sret wrapper/guard/full-result havoc, equilibrium battery fail-closed, prohibition of install on differential harness failure, E2 semantic-gate rules (aliasing/stack return/havoc out-of-bounds etc.), noreturn and unembeddable gates, stub-body self-check gate end-to-end (real ikos), scenario-driver upgrade ladder/Tier-2 linking, private D-Bus session inside the sandbox (command construction + end-to-end ping) |
| `llm_client.py` | OpenAI-compatible chat completions client, standard library only, with backoff retries; contains openai/deepseek/glm provider presets |
| `reallibs.py` | real-library resolution: in-tree scan (nm map building), system dependency-library whitelist (real-libs.conf + ldconfig), implicit libc/libm, address probe (link+load self-proof) |
| `real-libs.conf` | system dependency-library whitelist (label/soname/args ordered list; libc/libm are implicit rows) |
| `ikosjudge.py` | ikos battery: deterministic harness generation (8-byte window probe/pointer read/witness block), compile chain (mem2reg+llvm-link), `ikos -a=dbg,prover` invocation, stdout+sqlite dual parsing, witness run, content-hash cache; stub-body self-check (`selfcheck`: nondet-everywhere driver, `-a=boa,nullity,uva,prover,dfa`, definite-error judgment within the stub file) |
| `auditgate.py` | analysis-side audit gate (post-link): compare the two IKOS analysis libraries of the baseline and the stubbed program; non-zero exit when stub-side definite errors exceed the threshold or project-side unreachable growth exceeds the ratio; usage `python3 tool/api-spec/auditgate.py --help` |
| `auditloop.py` | outer repair loop: auditgate aggregates stub-side definite errors → evict stubdb entry → `--repair-hint` regeneration → re-audit until clean or budget exhausted; `--prune-failed` deletes still-violating stubs, `--json` outputs the round report |
| `cxprobe.py` | call-context full-type probe: parse external function parameter/return IR types and LLVM datalayout exact sizes from bitcode, majority-vote type-erased pointers by call-site back-lookup; used by selfcheck and Tier-2 scenario candidates, also standalone CLI debugging |
| `difftest.py` | differential-verification gate: driver/inputs prompt, security guard, scenario-driver upgrade ladder (libc setup whitelist / experimental in-library setup + stub-side real-library linking), private D-Bus session inside the sandbox, dual-build runner (`run_battery` per-input judgment + abstraction raw material + ikos battery hook), per-domain judge (`--domain` switches the single-domain admission criterion via `admission_mode`; `_ikos_phase` = analyzer-truth judgment: exclusion PROVED/stub assertion REFUTED/all unreachable/points-to·offset·nullity one-vote dimension), relational tier classification (`relation_class`, dbm/octagon/polyhedra/equalities 15 domains), argv-seed retry for empty alive, real-side link resolution and repair loop, reports |
| `coevolve.py` | two-agent coevolutionary game (potential-game engine, alternating moves): I^A/I^B abstraction (including the ikos truth axes `ret_ikos`/`cells_ikos`), interval deviation d, `cell-<tag>` point-domain component, potential function Φ, τ acceptance rule (constructive execution of ε-tolerance better response, violating revisions necessarily rejected), strategy menu and ablation arms, equilibrium stopping and fail-closed deterministic verification battery, game.json trajectory |
| `stubdb.py` | stub function database: per-function-sharded JSON storage (source name + signature key (+ domain axis) → body slot), verified>compiled ranking, shard-level atomic writes, tens-of-thousands scale, legacy single-file read-only compatibility; generate check/insert, prefetch 0-LLM pre-fetch |
| `difftest/dt.h` | `__dt_*` contract helper declarations (shared by the dual build, observation over fd 4) |
| `difftest/dt_real.c` | contract implementation: registry/canary/snapshot/write-set diff |
| `difftest/recorder.c` | stub-side `__ikos_*` implementation: nondet schedule, assertion/check/havoc logging (`assume_mem_rel` is an analyzer-side primitive; the stub side only needs to be linkable) |
| `difftest/include/ikos/analyzer/intrinsic.h` | shadow header: intrinsics become interceptable extern functions (including macro-form assume) |
