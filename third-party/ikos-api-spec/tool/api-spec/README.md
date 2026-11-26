# IKOS API Spec (LLM external-function stub generation tool)

`ikos-api-spec` uses an LLM to generate stub bodies for **unmodeled external functions**: given bitcode, it
renders a deterministic signature skeleton for each `missing` external function, has the model fill in only the
function body (expressing semantics with `__ikos_*` intrinsics), then — after closed-loop verification via
**compile, signature, real-library differential testing, and a coevolutionary game** — installs the result into
the stub library directory. Artifacts are compatible with `ikos-stub link` (see [doc/STUB_TOOL.md](../../doc/STUB_TOOL.md)).

Semantics (which `__ikos_*` to use, preconditions, side effects, return ranges) are decided by the model;
everything that would **fail silently** stays in deterministic machinery. Differential execution is sandboxed:
native execution requires Bubblewrap (`bwrap`), and the real side, stub side, and IKOS witness binaries run with
PID/network/IPC/UTS isolation, a read-only root filesystem, writable paths limited to the run directory, plus
rlimits/timeout. This came from an E1 incident where unsandboxed real-side execution killed the graphical
session and experiment process together — `kill`/`signal`-class functions must not get the host PID space.
For `dbus_`/`g_bus_`/`sd_bus_` targets, a one-shot private D-Bus daemon is started inside the same sandbox per
run (see "Private D-Bus session inside the sandbox").

It is one link in the spec pipeline:

| Tool | Responsibility |
| --- | --- |
| `ikos-api-census` | Census: which external functions are unmodeled (the to-do list) |
| **`ikos-api-spec`** | **Generate: LLM produces a verifiable stub body for missing functions** |
| `ikos-api-spec prefetch` | Prefetch: 0 LLM; install already-verified stubs from the stub database |
| `ikos-stub link` | Assemble: compile + llvm-link + signature/symbol double verification |
| `ikos` / `ikos-report` | Analyze the resulting bc, confirm the unknown-extern warning disappears |

## Running

Standalone script, not part of the analyzer's CMake build; any python3 works (the LLM client uses only the
standard library):

```sh
python3 tool/api-spec/ikos-api-spec.py generate [options] <bitcode>...
```

The tool derives the install prefix from `ikos-stub` on PATH, locates the `ikos` python package, and reuses its
internal functions (`_scan_and_plan`, `render_stub`, `signature_key`, …) so disposition decisions and the skeleton
are exactly consistent with `ikos-stub`; otherwise pass `--ikos-prefix` or `--site-packages`. After modifying
`analyzer/python/ikos/stub.py` in the source tree, reinstall/sync the Python package in `ikos-api-spec-install`
before batch experiments — the CLI and unit tests may use new logic while `generate` still bootstraps to the old
installed `ikos.stub`. `bwrap` must be on PATH for OS-level isolation; when absent, native differential and IKOS
witness execution both fail-closed as harness failure. D-Bus targets additionally use `dbus-run-session`; if that
is missing the tool warns once and falls back to the ordinary sandbox (never to the host bus), and such targets
typically stay missing.

## Common options

| Option | Default | Description |
| --- | --- | --- |
| `--provider` | - | `openai` / `deepseek` / `glm` built-in presets: base URL, key env var, default model (see next section) |
| `-o, --stub-dir` | `stubs` | stub library output directory |
| `--only` | all missing | generate only the specified functions (comma-separated, IR or source names); a name not in the bc, in the builtin whitelist, or already stubbed is reported with a way out (builtin → `--force-stub`) |
| `--model` | provider default | model name on the endpoint; required without `--provider` |
| `--base-url` | preset, else official OpenAI | any OpenAI-compatible endpoint (vLLM, Ollama compatibility layer) |
| `--api-key-env` | preset, else `IKOS_API_KEY`→`OPENAI_API_KEY` | env var holding the key; local endpoints can have no key |
| `--temperature` | preset, else 0.2 | OpenAI reasoning models do not send it; GLM fixes it at 1.0 |
| `--reasoning-effort` | not sent | reasoning tier for always-thinking models (`reasoning_effort`; GLM accepts `low`/`high`/`max`). The endpoint default can think for minutes with zero bytes during non-streaming, so set `--timeout` large (default 600s); use this only when speed is needed — `low` trades first-shot quality for speed, reliability still comes from the repair loop and gates |
| `--max-retries` | 3 | repair rounds per function (1 = single shot); with `--co-rounds` forms the bounded iteration budget; installs the last reliable passing body when exhausted |
| `--max-functions` | unlimited | budget cap |
| `--force-stub` | - | make builtin-whitelist functions also go through generation |
| `--cache-dir` / `--no-cache` | `~/.cache/ikos-api-spec` | response cache |
| `--db-dir` / `--no-db` | `~/.cache/ikos-api-spec/stubdb` | stub database for cross-program reuse (see below); `--no-db` neither reads nor writes |
| `--domain` | `all` | targeted generation by numerical abstract domain (ikos `-d` tokens; `all` = dual admission, sound across backends) |
| `--save-failed` | off | save the last candidate of a failed function to `<cache-dir>/failed/` |
| `--no-difftest` | off | turn off the differential gate (on by default) |
| `--no-self-check` | off | turn off the stub-body self-check gate (on by default: ikos re-analyzes each candidate passing compile+signature; a definite error in the stub file rejects it; harness failures do not falsely reject) |
| `--allow-unembeddable` | - | let a special-control-flow function go through LLM generation (repeatable; setjmp/longjmp, C++ exception runtime, vfork/clone report `unembeddable` by default, zero LLM budget) |
| `--repair-hint` | - | inject outer-audit findings into the first-shot body prompt (repeatable; enters the response cache key) |
| `--dt-no-scenario` | off | turn off the scenario-driver upgrade ladder (on by default: when the real side has no usable input, retry with a libc setup sequence) |
| `--dt-scenario-lib` | off | extend scenario setup to in-library functions (experimental; stub side links the real library, stub symbol wins) |
| `--real-libs` | implicit libc/libm | real-library map for the real side (TSV, repeatable, first wins; generate with `ikos-api-spec real-libs`) |
| `--dt-inputs` | 6 | LLM-proposed test inputs per function (tool appends deterministic edge inputs) |
| `--dt-schedules` | 48 | cap on stub-side nondeterministic schedules per input |
| `--dt-driver-retries` | 3 | repair rounds after a driver is rejected |
| `--dt-link-retries` | 2 | LLM repair rounds for real-side link failure (transitive dependency args, whitelist validation) |
| `--dt-timeout` | 5 | seconds per driver run |
| `--dt-no-sanitize` | off | do not attach ASan/UBSan on the real side (crashes recognized by signal only) |
| `--dt-no-ikos` | off | turn off the ikos battery (analyzer as judge; on by default; off falls back to pure concrete simulation) |
| `--dt-ikos-cpu` / `--dt-ikos-wall` | 10 / 60 | CPU / wall seconds per ikos judge run |
| `--dt-ikos-domain` | derived | ikos judge domain (`all`→`interval-congruence`, else follows `--domain`; a relational domain makes `assume_mem_rel` actually compute) |
| `--dt-report` | - | summarize per-input judgments as JSON |
| `--dt-keep-temps` | off | keep differential build/run temp directories |
| `--co-rounds` | 3 | rounds of the two-agent game (each = A's input move + B's body move, two alternating phases); 0 = single-shot differential gate; hands back the last body that passed the battery |
| `--co-a-policy` / `--co-b-policy` | greedy | strategy update: greedy (payoff-driven) / random / fixed (ablation arms) |
| `--co-attack-batch` | 3 | attack inputs A proposes per round |
| `--co-lambda` / `--co-mu` / `--co-alpha` / `--co-beta` / `--co-epsilon` / `--co-tau` | 0.1 / 0.2 / 0 / 0 / 0.02 / −ε | potential/payoff coefficients. Default α=β=0 ⇒ identical-payoff exact-potential game (theorem-covered); α/β>0 are δ-approximation ablation arms; τ is the acceptance threshold (default −ε = ε-tolerance better response; τ=0 strict). Used for RQ3 sweeps |
| `--co-report` | - | summarize the game trajectory (Φ curve, per-strategy record) as JSON |

## Model providers (OpenAI / DeepSeek / GLM)

All three vendors' OpenAI-compatible endpoints are built in as `--provider` presets. Supply the key either in
`HARDCODED_API_KEYS` at the top of [llm_client.py](llm_client.py) (takes precedence, plaintext — do not commit)
or in the corresponding environment variable (if neither has a key, it errors out naming the variable):

| provider | base URL | key env var (first match) | default model | other common models |
| --- | --- | --- | --- | --- |
| `openai` | `https://api.openai.com/v1` | `OPENAI_API_KEY` | `gpt-5.6-terra` | `gpt-5.6-sol`, `gpt-5.6-luna` |
| `deepseek` | `https://api.deepseek.com/v1` | `DEEPSEEK_API_KEY` | `deepseek-flash` | `deepseek-v4-pro`, `deepseek-v4-flash` |
| `glm` | `https://open.bigmodel.cn/api/paas/v4` | `GLM_API_KEY`, `ZHIPUAI_API_KEY`, `ZAI_API_KEY` | `glm-5.3` | `glm-5.3-flash`, `glm-5.2` |

```console
$ export DEEPSEEK_API_KEY=sk-...      # or OPENAI_API_KEY / GLM_API_KEY;
                                       # or fill HARDCODED_API_KEYS directly
$ ikos-stub check prog.bc             # first see what's missing
$ python3 tool/api-spec/ikos-api-spec.py prefetch -o stubs prog.bc   # 0 LLM: reuse stubdb
$ python3 tool/api-spec/ikos-api-spec.py generate -o stubs --provider deepseek prog.bc
$ ikos-stub link --stub-dir stubs -o prog.stubbed.bc prog.bc
$ ikos prog.stubbed.bc                # unknown-extern warning should disappear

# generate for a single function only:
$ python3 tool/api-spec/ikos-api-spec.py generate -o stubs --provider glm \
      --only sin prog.bc              # builtin-whitelist names additionally need --force-stub <name>
```

`--model` switches models at any time (e.g. `--provider glm --model glm-5.3-flash`); model names iterate quickly,
so a "model not found" means check vendor docs. Bare `--base-url`/`--model` without `--provider` is unchanged,
for vLLM/Ollama/self-hosted endpoints (key from `IKOS_API_KEY`/`OPENAI_API_KEY`; local endpoints can omit it).
Vendor adaptation: GPT-5.x reasoning models do not accept `temperature` (not sent by default); GLM-5.3 always
thinks (`reasoning_content` returned separately) with default `temperature` 1.0, and can think ten-plus minutes
per call — set `--timeout` large enough (short timeouts kill slow thinking into a retry loop); use
`--reasoning-effort low` only when speed matters.

## v1 design: LLM semantics, deterministic verification

1. **Signature is non-negotiable**: `render_stub` renders the C signature/typedefs from IR `declare` lines
   (byval/sret, `__asm__` labels, `p0..` names); the model returns only body statements. llvm-link silently
   bitcasts on signature mismatch, so the signature is never given to the model — the core reason `ikos-stub` exists.
2. **Lexical guard**: `#include`/`#define`/`__asm__`/`static`/`typedef`/`extern` or placeholder markers are rejected without compiling.
3. **Compile gate**: compile with ikos clang flags (`-D__IKOS__ -D_FORTIFY_SOURCE=0 -O0 -Xclang -disable-O0-optnone` + intrinsic.h `-isystem`); stderr feeds back for repair.
4. **Signature gate**: compiled `define` line and program `declare` line compared exactly via `signature_key`.
5. **noreturn / special-control-flow**: noreturn bodies (name table or attribute) must end with `__ikos_assume(0);`; setjmp/C++ exception functions report `unembeddable` without entering the model.
6. **Stub-body self-check gate**: after signature, ikos analyzes the stub itself; a definite error in the stub file is sent back for repair.
7. Any gate failure is fed back to the model for up to `--max-retries` rounds; one function's failure does not affect others.

### C++ external functions and C ABI adaptation

C++ symbols/types enter IR as mangled/quoted forms (`_ZNSt...`, `%"class.std::basic_string"*`). The skeleton
parser now handles quoted LLVM local types, return `align N`, `dereferenceable(N)` / `dereferenceable_or_null(N)`,
multi-level function pointers, and packed structs; previously such functions failed with
`cannot scaffold: declaration line cannot be parsed` (e.g. Bitcoin). C ABI rendering maps C++ named types to
stable `ikos_cxx_<hash>` tags, forward-declares pointer-only types, recursively rebuilds only by-value types from
the IR type table, renders `i1` as `_Bool`, and gives compilable ABI-equivalent forms for void-returning and
unnamed-parameter variadic callbacks. Original C++ names and generated tags normalize to the same signature key
(so `ikos-stub link` still verifies the ABI exactly; `signext`/`zeroext` on `i1/i8` are C-ABI-equivalent).
Explicit inexpressibility boundary: small non-trivial C++ `sret` returns, `inalloca`, and unnamed-parameter
external variadics are rejected at scaffold with `unembeddable` rather than silently mis-linking. Prompt v9 tells
the model that C++ functions expose the LLVM C ABI: `p0` of a non-static member is usually `this`;
constructors/destructors and output objects must be conservatively havoc'd; reference/object-pointer returns
prefer a parameter pointer or NULL; expressible aggregate returns need full-object havoc; never call the C++
runtime, allocate, or construct C++ objects. Regression: `tests/test_stub.py` covers quoted types, return attrs,
function pointers, packed structs, tag matching, and `sret` rejection; offline `bitcoin-cli` bitcode rendered,
compiled, and signature-matched 380/385 external functions (0 compile failures / 0 signature mismatches).

The differential gate's real-side driver switches to C++17 for mangled `_Z*` symbols while the skeleton/stub side
stay C ABI. The driver constructs real C++ objects (`std::string`, containers, project types), never disguises raw
bytes as objects, registers pointer/reference inputs with `__dt_buf`, and keeps them alive until `__dt_end()`;
the wrapper prelude carries the skeleton's `ikos_cxx_*`/aggregate typedefs across translation units. The C++
two-sided build links `libstdc++`; `__cxa_*` C ABI runtime helpers still use the C driver. LLVM `sret` returns are
exposed via a hidden result pointer: both sides declare `void dt_target(Result*, ...)`, the driver default-constructs
and registers `result` and passes `&result` first, and every scalar parameter maps to its own argv token (unsigned
via `strtoull`) so branch exploration is possible. The stub locally constructs the result, `__ikos_forget_mem`s the
whole object, and returns; a return lacking full-object havoc is `unsound` regardless of observed bytes. This is
not conflating the hidden `sret` result with an input pointer: the latter's points-to/offset/nullity/write set are
already judged, whereas `result` is a compiler-hidden output object (SSO / fresh heap / private fields / padding)
whose full-object havoc is a **reliability floor** that B may only tighten within real evidence; deleting it in
`shrink-havoc` is immediately judged `unsound` and rolled back. If project headers are unavailable, objects cannot
be legally constructed, or returns/parameters exceed the C ABI boundary, the harness fails and the function stays
missing — a stub passing only syntax/compile is never installed.

### Prompt essentials

The system prompt fixes: the soundness principle (a stub is the **only** semantics of a function, must
over-approximate, missing side effects = unsound, havoc when in doubt), the `__ikos_*` helper semantics table,
the four-step recipe (preconditions → write side effects → nondeterministic return → `__ikos_assume`
postconditions), the official fgets example, hard constraints (C99, only `stddef.h` identifiers + helpers, use
only `p0..`), and C++ C-ABI object semantics. The user prompt gives, per function, IR name, source name
(man-page semantics anchor), declare line, C signature line, and typedefs. When changing the prompt, bump
`PROMPT_VERSION` in the module; old caches auto-invalidate.

The stub-body DSL has 12 helpers: `__ikos_assert`, `__ikos_assume`, `__ikos_nondet_int/uint`,
`__ikos_check_mem_access`, `__ikos_check_string_access`, `__ikos_assume_mem_size`, `__ikos_forget_mem`,
`__ikos_abstract_mem`, `__ikos_assume_mem_value`, `__ikos_assume_mem_rel`, `__ikos_watch_mem`.
`__ikos_assume_mem_rel(p1,n1,p2,n2,kind)` preserves content knowledge without havoc and establishes, for
corresponding 8-byte signed chunks, an `==`/`<=`/`>=` relation per `kind`; it declares no new write side effect
(to make two outputs "unknown but related", first `__ikos_forget_mem`, then add the relation) and may only be used
when the API guarantees such a chunk relation, never on opaque structs/undefined-layout buffers.
`__ikos_watch_mem` is a debug observation point (no havoc). Analyzer debug/partitioning intrinsics are not part of
the DSL. Current prompt revision `10-semantic-helpers`; `tests/test_difftest.py` checks the prompt, canonical
intrinsic declarations, the differential shadow header, and the recorder together.

**Artifacts and ownership**: one `.c` per function (filename = `stub_key(IR name)`), first line provenance
comment `/* generated by ikos-api-spec (model: ...) */`, directly accepted by `ikos-stub link`. That comment is an
ownership marker — a pre-existing file not generated by this tool is never overwritten, even with `--force`.
The response cache stores `responses.json` keyed by hash of `(PROMPT_VERSION, model, IR name, declare line)`; a
hit skips the API call but still runs compile + signature verification. `--force` ignores cache reads.

## Differential verification gate

Compile and signature gates verify only mechanical correctness; a semantically too-strong stub passes them. The
differential gate makes the LLM also generate a small driver and test inputs, compiles the driver twice (once
against the real library with ASan/UBSan, once against the stub + recorder), and compares per input whether the
stub is an **over-approximation**. Real libraries resolve through real-lib maps (in-tree, system, libc/libm). A
judged unsound counterexample enters the same `--max-retries` repair loop.

**Strict installation rule**: only a body whose differential/game status is `pass` is installed into stubdb;
`harness-error`, driver-generation failure, real-library link failure, no usable input, etc. are `failed` (or
`no-real-lib` before generation) and the function stays missing. `--no-difftest` is only a debug/ablation mode and
its artifacts do not enter stubdb. The deterministic equilibrium recheck before the game ends is also fail-closed
(boundary battery failure ⇒ `harness-error`, old `pass` cannot be reused).

### Judgment rules (mirror the IKOS abstract model)

Every judge rule corresponds to the analyzer's real abstract-domain semantics, not an invented criterion. From
2026-09-29 this table is the per-input fallback layer: when the ikos battery is present, numerical judgments use
the analyzer body's provability evidence (PROVED exclusion ⇒ unsound; REFUTED ⇒ return-axis sound), and this
table's Python simulation only handles inputs where ikos has no evidence.

| Abstraction | IKOS semantics | Judgment rule |
| --- | --- | --- |
| Integer | MachineInt domain, backend via `-d`: interval join = convex hull; congruence join = congruence class | **Dual admission**: real return must lie in both convex hull and congruence closure (gcd of pairwise differences). `{-1,1}` vs real 0 is unsound under congruence; complete `{-1,0,1}` (gcd=1) is sound across domains |
| Pointer | uninit × nullity × points-to (block set/TOP) × offset interval | **Per-component**: real NULL ⇒ stub reaches NULL; real points to block j ⇒ stub reaches same block (different block = unsound); offset takes joined hull, degrading sound\* where congruence excludes out-of-closure offsets |
| fresh block (non-allocator) | caller-unnamable pointer created in library, strictly sound only with points-to TOP (not in stub vocabulary) | **sound\* (block-level gap)**: requires non-NULL capability; argument-block aliasing sanctioned by the official fgets pattern, annotated per input |
| allocator-like return | "take a size, return fresh storage" (`_TIFFmalloc`, `g_malloc`) | stub must use `__ikos_malloc(size)`: IKOS side reuses the libc malloc model for a fresh block, recorder logs `MALLOC addr size` → `FRESH`; any argument-block/stack/integer-cast non-NULL return is unsound |
| memory content | per-block cells; `forget_mem` erases pointer set, `abstract_mem` preserves | every byte the real side **wrote** must be covered by stub havoc; bytes storing a **pointer** must use `forget_mem` not `abstract_mem` |
| C++ `sret` aggregate return | return object is memory pointed to by hidden result pointer | driver registers real `result`; stub must havoc the complete object (full-object `forget_mem` maps to hidden result write set); zero/narrow content is not allowed |
| float | initialization state only, no numerical abstraction | vacuous judgment (note recorded; preconditions/write side effects still checked) |
| precondition | assert/check_mem proven at call site | assert failure on real-accepted input / rejecting NULL / claiming out-of-bounds = unsound-precondition; assert varying with nondet = warning factory |

The `-d` backend is pluggable through `machine_int_domain.hpp` (21 domains, default interval); builtin models all
use the domain interface, and interval-specific reads like fgets's `int_to_interval().ub()` return top under
congruence (degrading is sound). Unmodeled functions (`strcmp`/`abs`/`memcmp`) go through `exec_unknown_call`
returning top. Our stubs use only `__ikos_*` intrinsics and are likewise domain-pluggable; the only pitfall is a
finite enumerated return set (congruence join = closure). `--domain` switches the judgment criterion to one
backend's join semantics.

### Execution mechanism

- Differential execution is wrapped in a **Bubblewrap sandbox** by default: separate PID/network/IPC/UTS
  namespaces, new session, read-only root, minimal `/proc`/`/dev`, only the run directory writable — so
  process-control/signal/network/file-write side effects stay confined. Without bwrap, native execution fails
  closed as harness failure (real function never runs bare).
- The driver contract goes over **fd 4** (`__dt_*` helpers shared by both sides), never stdout. Buffer registration
  is the boundary oracle: `__dt_buf`/`__dt_buf_out` (output buffers canary-filled first; 0xAA/0x55 double run
  catches "written value equals original").
- The stub side is compared on **claims, not content**: `difftest/recorder.c` logs `__ikos_*` calls to
  `NONDET`/`ASSERT`/`CHECK_MEM`/`HAVOC_FORGET|ABSTRACT`/`ASSUME_VAL`/`MALLOC` (addresses → (tag, offset)); the
  judge subtracts real writes from havoc claims. The shadow header
  `difftest/include/ikos/analyzer/intrinsic.h` turns all intrinsics (including macro-form `__ikos_assume`) into
  interceptable extern functions.
- A **schedule battery** drives nondet: all-0 probe learns K, then force-injects `{truncated(real return), ±1}`,
  boundaries, all-0/all-1, and 16 seeded LCGs, capped at `--dt-schedules`; violating `__ikos_assume` runs are
  discarded by analyzer pruning.
- Real-side crash (`DT1 call` without `DT1 ok`) = outside real precondition, n/a with zero weight; harness failure
  only warns and skips that gate, never falsely rejecting.
- Per-input verdict ∈ `sound / sound*(reason) / unsound(reason) / unverified / n/a`; artifacts in
  `<cache-dir>/difftest/<fn>/`; summary wording is "no unsound found across N inputs", never "the stub is sound".

### ikos battery (analyzer as judge)

The plain "stub side + judge" is concrete execution + Python simulation of domain semantics (hand-written convex
hull ∩ congruence closure). The ikos battery (`ikosjudge.py`) replaces simulation with the **real analyzer**:

- **Deterministic harness per input** (zero LLM): `main(void)`, scalar params = argv-token literals, pointer
  params point to literal-byte buffers (from the real snapshot), stub called once, each probed cell loaded into a
  named local, `__ikos_print_values` prints the interval / points-to/offset/nullity, then one `__ikos_assert(r != y)`
  / `assert(cell != b)` exclusion assertion per line (sqlite verdict 0=proved / 1=unknown / 2=refuted).
- **Compile chain**: harness and stub each compiled with ikos clang flags → each `opt -mem2reg` (at O0 the assume's
  refinement is not written back to the cell) → llvm-link → `ikos -a=dbg,prover -d <domain>`. This repo's
  `analyzer/include` takes precedence over the install prefix so a developing `__ikos_malloc` does not hit the
  old `intrinsic.h`.
- **Witness validation (safety core)**: argv→argument pinning is underdetermined, so the same harness source is
  compiled once more without `-D__IKOS__` (intrinsics become empty macros), linked against the real library and
  run natively. An exclusion PROVED is accepted only when the witness reproduces the real observation; a wrong
  pin discards that input's evidence (falls back to simulation), never false unsound. Witness uses the same
  Bubblewrap policy and a pinned cwd.
- **Judgment**: exclusion PROVED + witness match → unsound (reason cites ikos/domain); stub `__ikos_assert`
  REFUTED → unsound precondition; ret assertion REFUTED (stub can return the real value) → return-axis sound,
  skipping simulation `_phase_c`; all unreachable → unverified.
- **Points-to judgment (one-vote dimension)**: the witness's concrete target must fall inside the claim — block ∉
  set / offset outside interval / reversed nullity → unsound; non-allocator OTHER blocks use the fresh-block
  approximation; allocator targets require the `__ikos_malloc` fresh capability.
- **Game**: ikos intervals enter `spec_abstraction` (`ret_ikos`/`cells_ikos` axes, claimed hull prefers analyzer
  truth), new `cell-<tag>` component (mean slack from real bytes to claimed interval; violation = ikos-proven
  exclusion), and `ret-int` violation OR'd with the ikos exclusion flag.
- **Domain mapping**: `all` → `-d interval-congruence`; a specified domain uses that domain (relational domains make
  `assume_mem_rel` actually compute). Flags: `--dt-no-ikos`, `--dt-ikos-cpu` (10), `--dt-ikos-wall` (60),
  `--dt-ikos-domain`. Honest skips: aggregate returns, byval aggregates, float returns, buffers >4096B. Cost: a
  typical game ≈54 pairs, +1–2 minutes, with in-process caching by content hash.

### Analysis-side quality gates

The E2 experiment (30 real projects, 1,314 "newly confirmed errors" of which only 1 was a real defect; 378 definite
errors in stub source itself, 97.6% buffer overflow) exposed categories the existing gates could not stop, so three
deterministic gates plus a post-link audit were added:

- **Reinforced semantic rules**: return aliasing an input (fopen64 returning the path as-is), stack return
  (g_malloc `return &p0`), pointer/integer confusion (allocator non-fresh hard-rejected), and havoc/assume_mem_size
  crossing the registered buffer boundary (stat64 writing 256B into a 144B struct) are unsound per input.
- **noreturn enforcement**: exit/abort/assert-fail/pthread_exit family (12 names) or a `noreturn` declare attribute
  ⇒ body must end with `__ikos_assume(0);`, lexically judged, fed into the repair loop; setjmp/longjmp, `__cxa_*`,
  vfork/clone (16 names) report `unembeddable` with zero LLM budget (`--allow-unembeddable <name>` exempts one by one).
- **Stub-body self-check**: every candidate passing compile+signature (and stubdb reuse entries) is analyzed once
  by ikos (`-a=boa,nullity,uva,prover,dfa`, nondet driver); definite errors located in the stub file are rejected,
  warnings only recorded. Input/return object sizes come from **cxprobe** (below), falling back to header mappings
  or a 128B buffer. `--no-self-check` disables. Verified: bzip2 `stat64` probe finds the real 144-byte `struct stat`,
  so `forget_mem(p1, 4096)` is `failed` and not installed.

Two tools close the loop outside `generate`:

- **`auditgate.py`** (post-link): compares baseline and stubbed programs' two IKOS analysis libraries; non-zero exit
  if stub-side definite errors exceed `--max-stub-errors` (default 0) or project-side unreachable growth exceeds
  `--max-unreach-delta` (default 5%). The latter prevents under-approximation stealing coverage: a getopt_long stub
  pruning ASCII 'o' made a whole `case 'o':` dead and memcached "lost" 74 baseline errors.
- **`auditloop.py`**: auditgate findings → aggregate by stub file (filename = `--only` name) → **evict that stubdb
  entry first**, then regenerate with the finding as `--repair-hint` (enters cache key) → re-audit until clean or
  `--max-rounds` (default 2); `--prune-failed` deletes still-violating stubs; `--json`/`--jobs` for report/parallelism.
  Unreachable-change rate is reported, not used as a hint (not attributable to one stub). Exit 0=clean / 1=violating
  / 2=tool failure.

**cxprobe** (`cxprobe.py`) parses all input/return types and sizes from bitcode: llvm-dis per-bc cache, recursive
named struct/union extraction, exact sizes via LLVM constant folding (GEP probe with original datalayout,
`opt` folds), and majority-vote call-site back-lookup for type-erased `i8*`/`void*` params. Verified on bzip2
`stat64` (144 bytes) and `getservbyname` return (32-byte `struct servent`). Selfcheck driver uses cxprobe > header
mapping > 128B fallback. Standalone: `python3 tool/api-spec/cxprobe.py X.bc <fn>...`.

### Scenario driver

The largest differential-battery failure class (55.9% in R16) is the real side having no contract-usable input
(`regexec` without `regcomp`, `fdopen` without `open`). The LLM may run a setup sequence immediately before exactly
one target call:

- **Upgrade ladder**: run the bare driver first; only after the real-side pre-probe (2 canaries × 2 repeats, all 4
  must be ok and consistent) condemns total wipeout does it regenerate in scenario mode and rerun; crash
  stderr/ASan summaries are appended to the regeneration prompt.
- **Tier 1** (on by default, `--dt-no-scenario` off): setup whitelist limited to libc constructors
  (`open/regcomp/socket/pipe/mkstemp/tmpfile/opendir/fopen/pthread_mutex_init/malloc/calloc`, 17 entries), usable
  on both sides, no link changes; ≤4 setup calls (budget 6), target still exactly once, products registered with
  `__dt_buf`/`__dt_buf_out`.
- **Tier 2** (experimental, `--dt-scenario-lib`): setup extends to in-library functions (e.g. `xmlInitParser`),
  candidates = cxprobe call-graph adjacency ∩ reallibs-resolvable (≤8); stub side also links the real library with
  stub object first + `-Wl,--allow-multiple-definition` so the stub symbol wins; setup is real on both sides.
- **Observation invariance**: the judge's surface is unchanged (target ret/wrote/events); setup only shapes inputs.
  Smoke (regexec @ haproxy): wipeout → pre-probe → `regcomp` → `__dt_buf` → `regexec` → `regfree`.

### Private D-Bus session inside the sandbox

bluez `dbus_*` functions (largest R18 residual cluster, 58) need a live session bus while the sandbox isolates
IPC. For names prefixed `dbus_`/`g_bus_`/`sd_bus_` (`DT_DEFAULTS['sandbox_dbus']` on), every run_once
(real/stub/probe) runs via `dbus-run-session --config-file <conf> --` inside bwrap: one private daemon per run,
socket at `<workdir>/xdg/bus`, `XDG_RUNTIME_DIR`/`TMPDIR` inside the sandbox, allow-all policy, socket never leaves
the sandbox. Missing `dbus-run-session` warns once and falls back to the ordinary sandbox (not fail-closed, but
still never touches the host bus). Compatible with the scenario ladder. Smoke (`dbus_bus_request_name` @
bluetoothctl): after `dbus_bus_get` setup, the target stably returns `DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER` on the
private bus across double canary.

### Security

The driver passes a deny-by-default guard (include/call whitelist, token bans `errno`/`__asm__`/`#pragma`/
`__attribute__`/`volatile`, target exactly once between contract lines), plus rlimits (CPU/AS 512MB/FSIZE 1MB/
NOFILE/NPROC), an empty temp cwd, env pruning, and a per-run timeout — all inside `bwrap --unshare-all`, forming
"static guard + resource limits + OS isolation"; without bwrap, native execution fails closed. This limits misuse
but is not an absolute boundary against kernel/container escapes. The real-side ASan build needs `setarch -R` on
high-entropy-ASLR kernels (handled automatically).

## Coevolutionary game (potential-game engine)

The differential gate guarantees **reliability** (over-approximation) but exerts no pressure on an "absurdly wide yet
consistent" spec. With two generative agents — **B** = stub-body generator, **A** = driver/input generator —
`--co-rounds` (default 3) turns their interaction into a **measurable repeated game** moving the artifact from
reliable to precise. **All metrics are computed by the deterministic harness; the LLM only selects strategies and
produces artifacts.** The soundness gate stays outside as a backstop (round-0 body must first pass the differential
gate; the final body must pass the battery again or enter the repair loop).

### Formal model

**Players/actions/information.** N = {A, B}. A's action a_t = (σᴬ_t, τ_t): strategy σᴬ ∈ Σ_A plus new inputs τ_t
(argv lists); B's b_t = (σᴮ_t, β_t): strategy σᴮ ∈ Σ_B plus a revised body β_t. Moves are **alternating** (A first):
A moves on the previous settlement → settle → B moves on a settlement already containing A's new inputs → settle,
two phases per round. The second mover sees the first mover's current-round consequences; the first mover's action
manifests through the next settlement — the information structure required by asynchronous better-response
dynamics. Settlement is perfect monitoring (full metrics enter the next mover's public prompt, no private info).

**State/deviation.** s_t = (T_t, I_t, Iᴬ_t, Iᴮ_t): input pool T (monotone non-decreasing, argv de-duplicated),
current body I, execution-data abstraction Iᴬ, spec abstraction Iᴮ (both lattice-valued over components C: ret-int
hull/congruence class, ret-ptr nullity/block class/per-block offset hull, per-tag write set):

```text
d(Iᴬ, Iᴮ) = (1/|C|) · Σ_{c∈C} [ dist_c + w_v · ν_c ]
```

`dist_c ∈ [0,1]` is normalized distance (boundary difference ÷ width / symmetric difference ÷ union / lattice
distance / congruence axis +0.5); `ν_c ∈ {0,1}` is the component's **inconsistency signal** (observation outside
the claimed admission set, same as the judge, incl. wide-hull exemption); `w_v = 8`, so `d ∈ [0, 9]`. w_v exceeds
all non-violation terms' dynamic range (each ≤1, λ·K and μ·Cov ≤~1) ⇒ **arithmetic lexicographic order**: any
inconsistency outweighs all tightening, invariant `w_v > λ·K_max + μ + 1`.

**Potential/payoffs.** Φ(s) = d(Iᴬ, Iᴮ) + λ·K(I) − μ·Cov(T), K = log-scale complexity, Cov = boundary coverage
(smaller is better); ΔΦ_t := Φ(s_t) − Φ(s_{t+1}):

```text
u_A(a_t) = ΔΦ_t + α · E(a_t),   E ≥ 0 first-discovery count
u_B(b_t) = ΔΦ_t − β · P(b_t),   P = max(0, K(s_{t+1}) − K(s_t))
```

**Default α = β = 0** (shipped): u_A = u_B = ΔΦ identically — an identical-interest game; −Φ is the **exact
potential** (Monderer & Shapley 1996) and telescopes, so over a finite horizon joint-payoff maximization =
final-potential minimization. An exact-potential game has the **finite improvement property (FIP)**: on the finite
state quotient, asynchronous better-response reaches a pure-strategy Nash equilibrium finitely, with no improvement
cycles. The implementation executes exactly this (A→settle→B→settle, never parallel). Acceptance does not rely on
model self-awareness: the τ rule prices B's revision before settlement, rolling back u_B ≤ τ bodies (trajectory
`-rejected`, reason backfilled). Default τ = −ε accepts **ε-tolerance better responses**; τ = 0 is the strict arm.
Evidence (A's inputs, battery injections) is **never rolled back** — an input exposing inconsistency raises Φ but
is the most valuable action, so rolling it back would hide unsound. Compound termination argument: evidence is
monotone and its abstraction quotient finite (component values bounded by bit-width/buffers, argv de-duplicated,
lexical guard limits vocabulary), each new evidence refines the quotient by ≤1 step; under fixed evidence an
accepted revision is no worse than tolerance (Φ bounded below, strictly decreasing on measurable improvement);
strict acyclicity holds only for τ = 0. "potential-stable" (|ΔΦ| < ε over a full A+B cycle) is the observable FIP form.

When α,β > 0, the bounded extra terms give a **δ-approximate potential game** (δ = max(α·E_max, β·P_max), no
tightness claimed): A's exploration reward can temporarily raise Φ (sawtooth, RQ3); the mixed motive is in the
perturbation (A rewards exploration, B penalizes complexity). Strategy update σ_{t+1} ∈ argmax_σ E[u | σ] uses
realized payoffs backfilled each phase (LLM over a finite menu; random/fixed are ablation controls). Equilibrium
notions (potential-stable, empirical ε-Nash — valid only over the **realized action set**, and vetoed by the
deterministic battery before declaration) and the budget floor are below.

### Formal objects → measurable instantiation

| Formal concept | Implementation ([coevolve.py](coevolve.py)) |
| --- | --- |
| `Iᴬ` execution-data abstraction | dual-canary observation per component: ret-int hull+gcd closure; ret-ptr nullity/block-class set/per-block offset hull; writes per-tag offset union (snapshot-failed `'?'` tags excluded); witness concrete ret on pinned inputs |
| `Iᴮ` spec abstraction | recorder's **surviving** claims under the battery (assume-pruned paths excluded), merged with ikos truth: `ret_ikos` (analyzer return interval, covers whole nondet space), `cells_ikos` (per-buffer 8-byte-window intervals), per-axis exclusion flags |
| `d(Iᴬ,Iᴮ)` | per-component normalized distance averaged (interval = boundary diff ÷ width; sets = symmetric diff ÷ union; nullity = lattice distance; congruence +0.5 when gcd(obs)>1 & gcd(claim)=1; `cell-<tag>` = mean slack to claimed window); inconsistency signal ×w_v=8, contribution bounded [0,9] |
| `Complexity(I)` | number of `__ikos_*` calls + integer constants + 2× ternary nesting depth, log2/8 normalized |
| `Coverage(T)` | fraction of claimed surface touched (ε_face = max(1, width/1024)): hull endpoints, NULL capability, offset ends, write-set edges |
| `ExplorationBonus` / `ComplexityPenalty` | first-discovery count / ΔComplexity (truncated ≥0); with α=β=0 they are tie-breakers, not payoff terms |
| coefficients | `--co-lambda/--co-mu/--co-alpha/--co-beta/--co-epsilon/--co-tau` all CLI-sweepable |
| τ rule | `measure()` before `settle()` prices B's revision; u_B > τ before booking (default −ε, τ=0 strict; w_v range suppression necessarily rejects signal-manufacturing revisions), else roll back and reuse the deterministic battery record, mark `-rejected` |

### Strategy space and round loop

Both sides hold a discrete strategy menu and the prompt gives the full payoff function, per-component Φ
decomposition, and historical record, asking for the highest-expected-payoff strategy:

- **A** (strategy id + argv JSON): `targeted-boundary`, `distribution-shift`, `shape-variant`, `replay-mutate`, `deterministic-sweep`;
- **B** (strategy id + new body): `tighten-interval`, `tighten-congruence` (complete enumeration needed; gcd=1 admits all, incomplete enumeration is an inconsistency signal), `shrink-havoc`, `drop-null-capability`, `simplify`, `widen-on-inconsistency` (forced after a signal);
- policies `--co-a-policy/--co-b-policy`: `greedy` (default, actual driver), `random` (RQ2 control), `fixed` (RQ1 control: A=`replay-mutate`, B=`tighten-interval`).
- Each settlement writes realized ΔΦ/u_A/u_B and per-strategy records atomically to `<cache-dir>/coevolve/<fn>/game.json`; `--co-report` saves a summary.

round 0 = the existing artifact (B's body₀ through the original pipeline, A's driver/initial inputs cached). Each
round: **A moves first** on the previous settlement, new inputs enter the pool, battery settles, evidence never
rolled back; **B moves second** on a settlement containing A's current inputs, priced before settlement (u_B > τ
booked, else roll back and mark `-rejected`). An unchanged phase reuses the previous deterministic battery result.
Dual build + dual canary + battery per phase, each settlement checkpointed atomically. Stopping is decided after a
settlement that covers exactly one A phase + one B phase with no inconsistency signal:

- **potential-stable**: |Φ(s_t) − Φ(s_{t−1})| < ε within that cycle;
- **empirical ε-Nash** (honest: realized action set only): both |u| < ε and neither below its historical best − ε;
- budget `--co-rounds` exhausted (a vetoed equilibrium consumes that round's budget).

**The budget is the reliable floor**: a witness list records every body that passed the battery on the then-complete
pool; at exhaustion, re-verify newest→oldest on the **final** pool (earlier passes don't count) and hand back the
newest passer. The τ rule means the floor only fights "evidence grew and punctured a later tightening"; if even
repair fails, the function **installs a witness rather than reporting failed** — reliability before precision
(`game.json`'s `reliable_body`). An LLM outage mid-game degrades to a single-shot battery re-verification from
cached driver/inputs. The **equilibrium verification battery** (0 LLM): before declaring equilibrium, replay
edge-token variants via deterministic-sweep; an inconsistency signal **vetoes** equilibrium, merges the evidence,
and continues.

### The game over the pointer abstraction layer

| Component | d | A's attack | B's response |
| --- | --- | --- | --- |
| nullity | lattice distance | NULL-inducing inputs (empty string/error/NULL token) | drop-null-capability or keep |
| block set | symmetric difference | double buffers/different args exposing blocks or fresh | narrow claim (non-allocator fresh bidirectionally exempt, sound\*; allocator `FRESH` not removable) |
| offset (per block) | boundary difference | last-character/extreme-offset inputs | tighten-interval(offset) |
| writing pointer values | in write-set d | pointer-write-inducing inputs | shrink-havoc / forget-abstract choice |
| ret-int congruence | gcd mismatch +0.5 | hit congruence-class exclusion values | tighten-congruence (complete enumeration) |

### Ablation arms (RQ3, see exp/RUNBOOK.md)

- **RQ3.1 game contribution**: `--co-b-policy fixed` / `--co-rounds 0` controls, compare final d and tightening;
- **RQ3.2 potential vs random**: `--co-a-policy random` / `--co-b-policy random`, compare Φ convergence and signal-resolution rate;
- **RQ3.5 convergence**: game.json Φ/d/coverage trajectory (two phases/round) + ε/τ/α/β sweeps (α/β leave exact potential for δ-approximation);
- **precision/boundary coverage** (RQ2.2): final d, claims-hull width convergence, Coverage, veto count.

### The game's boundaries (stated honestly)

- empirical ε-Nash is valid only over the realized action set; default α=β=0 is the theorem-covered exact-potential
  game (τ=−ε accepts ε-tolerance responses, rejecting violating ones; τ=0 strict), approximate potential only in
  α,β>0 arms;
- equilibrium is **evidence-bounded** (by A's attacks + the battery not breaking it), not claimed optimal — the
  `{-1,1}` vs strcmp "equilibrium" vetoed by a one-token variant is the example;
- witness floor evidence = the complete battery of its own game (pools not unioned); `--co-rounds 0` has no witness;
- same-model self-play collusion risk: deterministic-sweep verification is the only incentive-independent defense;
  random/fixed arms double as controls;
- the battery's forced injection (real ±1) pushes claims toward real values, so a wide body has small d — d measures
  deviation **under battery driving**; metric-invisible tightening is kept by ε-tolerance exactly why default is
  τ=−ε rather than 0;
- non-allocator fresh blocks, float, errno side channel keep the differential gate's expressiveness gap; A's driver
  is not revised in-game; precondition slack is unobservable.

## Stub function database (cross-program reuse)

Evidence is **program-independent**: a body passing the battery on one program's `declare i64 @strtol(i8*, i8**, i32)`
is valid on any program with that declaration. `--db-dir` (default `~/.cache/ikos-api-spec/stubdb`) turns this into
reuse. **Identity = source name + signature key** (same ruler as `ikos-stub link`); entries generated with `--domain`
also carry a **domain axis** (portable entries use the bare key, targeted entries `signature key|domain=<name>` and
serve only same-domain queries).

Storage is one small shard per function, built for tens-of-thousands scale:

```text
stubdb/
├── entries/
│   ├── strtol.json      # all signature variants of this function -> entry (~KB scale)
│   ├── fopen64.json
│   └── ...
└── index.json           # legacy single-file layout: read-only compatibility, deletable once shadowed
```

- Rewrites touch only that function's shard, cost independent of DB size (500 functions don't rewrite the rest;
  single-file degrades to O(n²), GB-scale waste at 20k); a query reads one shard with no startup loading. Measured
  10k inserts in 0.7s.
- **git-friendly**: one file per function = incremental diffs, per-function merges, almost no conflicts; entries
  contain no keys, only body/declare/model name/verified flag/timestamp.

**generate**: after rendering the skeleton, check the DB before the response cache (a verified body for the same
declaration is stronger and free): hit → re-splice into the **current** skeleton, re-pass lexical/compile/signature
→ install as `reused` with 0 LLM calls; any gate failure warns and falls back to generation (the DB is a cache, not
an authority). The differential battery is **not rerun on reuse** — evidence is bound by declaration, not calling
program (trust boundary: same real libraries; confirm when sharing `--db-dir` across machines). Both install points
(main success and reliable floor) enter the DB; `verified` flag = whether the differential gate was on, ranking
verified(2) > compiled(1) (`--no-difftest`), higher overwrites, equal/lower keeps — the DB never degrades. Insertion
is best-effort (write failure warns only).

**prefetch** (0 LLM): analyze the bc's externs ahead of time — missing and in DB → install `reused`; not in DB →
`missing` ("run generate"); exit 1 if anything is missing.

```console
$ python3 tool/api-spec/ikos-api-spec.py prefetch -o stubs prog.bc
| strtol  |  | reused  | 0 | stubs/strtol.c (stub database: ...) |
| strtoul |  | missing | 0 | not in the stub database (run generate) |
```

**Boundaries**: single-writer (in-process serial writes, shard-level `os.replace` atomic flush; multi-process
writes overwrite each other at shard level, no concurrency claim; cross-person collaboration via git per-function
merges). Reusing a compiled entry trusts only the compile gate; verified evidence is bound to the generating
machine's real libraries. Storage is isolated behind [stubdb.py](stubdb.py), so a server backend can swap in later.

## Abstract domains and stub portability

Stubs use only `__ikos_*` intrinsics and are **naturally domain-pluggable** — the same stub works under any `-d`
backend (interval/congruence/dbm/apron-*, 21 domains), no per-backend regeneration. Generation defaults to **dual
admission** (convex hull ∩ congruence closure): the real return must lie in both, sound under any backend.
Experiments use `-d interval` for analysis but generation still uses dual admission. `--domain <name>` switches the
judgment to one backend and writes relational constraints for relational backends (not enabled in experiments;
expand via difftest.py's `admission_mode` / `relation_class`).

## Boundaries and blind spots (stated honestly)

**Generation limits**: `skip-interceptor` (`\x01` prefix) and `skip-inalloca` (C++ by-value non-trivial types) are
"cannot generate" and handwritten; small non-trivial C++ `sret` returns and unnamed-parameter external variadics
(e.g. `__gxx_personality_v0(...)`) report `unembeddable`; rare IR types (half/fp128/vector) cannot render a skeleton.
`__ikos_assume` float comparison does not shrink intervals (int can, double cannot) — still sound, no precision; the
prompt tells the model to express it as an over-approximate return (`return (double) __ikos_nondet_int()`; confirmed
by sin). v1 generates serially (rate-limit friendly); `ChatClient` itself is thread-safe.

**Real-library resolution**: three layers — project in-tree `.a`/`.so` (most faithful, `ikos-api-spec real-libs
<project>` → `<project>/real-libs.tsv`), system dependency whitelist (`real-libs.conf` + ldconfig, `--system-out`),
and implicit libc/libm. generate passes `--real-libs <map>` (repeatable, first wins). Unresolvable functions are
skipped before any token (`no-real-lib`); link failure goes through the LLM link-repair loop (`--dt-link-retries`),
else `failed` and missing — never installing an unverified stub. The wrapper rename fixed `__isoc99_*`/C++ mangled
real-side linking (these had never actually run the battery before). Bring-up in doc/REAL_LIBS_BRINGUP.md.

**Differential blind spots**:

- Testing is not proof: only covered inputs are judged. A wide-hull compound nondet return is not reached by
  battery driving (closure untrustworthy, admitted `sound*` only by interval); the ikos battery narrows this by
  covering the whole nondet space with verdict-bearing exclusion assertions.
- `__ikos_assume` integer shrinking is interval semantics: under congruence a non-singleton refine degrades to top,
  losing precision but directionally sound.
- Non-allocator fresh-block gap (strict-lattice unsound, toolchain-sanctioned `sound*`); allocator APIs must use
  `__ikos_malloc`. Float returns are vacuous. Writes through hidden/double-indirect and libc-internal pointers
  (FILE\*) are unobservable; checks on unregistered pointers only check NULL.
- Real libraries not ASan-built: mixed linking is legal (malloc/free still intercepted via PLT, canary valid) but
  out-of-bounds/UB **inside** the library is invisible; version drift at worst yields real-side crash/timeout →
  honest n/a/skip. `--dt-no-sanitize` is the overall floor.
- errno/locale/globals are side channels the stub cannot express (ikos likewise does not havoc globals for unknown externs).
- Double-canary blind spot: bytes the driver pre-initialized. `sret` returns rejected at scaffold; expressible ones
  judged by full-object havoc. An empty surviving run set first retries via argv seeds, then goes to manual review;
  seeds are limited by int32 nondet vocabulary (64-bit values beyond cannot be expressed). The token→parameter map
  is a driver heuristic; the ikos battery is also constrained by it, but witness validation limits a wrong pin to
  discarding that input's evidence. `assume_mem_value/rel` cell granularity is 8 bytes.

## Files

| File | Responsibility |
| --- | --- |
| `ikos-api-spec.py` | CLI + generation pipeline (skeleton split/splice, guard, noreturn/unembeddable, compile/signature gates, self-check gate, differential gate, cache, install; `real-libs`/`eval-real-libs` subcommands) |
| `tests/test_stub.py` | skeleton parse/render regression: quoted types, return attrs, function pointers, packed structs, C++ tag matching, `sret` rejection, clang compile validation |
| `tests/test_difftest.py` | differential regression: C++ ABI typedefs/prototypes/drivers, mangled-symbol selection, hidden-sret guard/full-result havoc, equilibrium battery fail-closed, no-install-on-harness-failure, E2 semantic gates, noreturn/unembeddable, self-check end-to-end, scenario ladder/Tier-2, private D-Bus (command + ping) |
| `llm_client.py` | OpenAI-compatible chat client, stdlib only, backoff retries; openai/deepseek/glm presets |
| `reallibs.py` | real-library resolution: in-tree scan (nm map), system whitelist (real-libs.conf + ldconfig), implicit libc/libm, address probe |
| `real-libs.conf` | system dependency-library whitelist (label/soname/args; libc/libm implicit) |
| `ikosjudge.py` | ikos battery: deterministic harness, compile chain (mem2reg+llvm-link), `ikos -a=dbg,prover`, stdout+sqlite parsing, witness, content-hash cache; selfcheck (`-a=boa,nullity,uva,prover,dfa`) |
| `auditgate.py` | post-link audit: compare baseline/stubbed IKOS libraries; non-zero exit on stub errors or unreachable growth; `python3 tool/api-spec/auditgate.py --help` |
| `auditloop.py` | outer repair loop: auditgate → evict stubdb entry → `--repair-hint` regeneration → re-audit; `--prune-failed`, `--json`, `--jobs` |
| `cxprobe.py` | call-context type probe: IR types + LLVM datalayout sizes from bitcode, majority-vote erased pointers; used by selfcheck and Tier-2, standalone CLI |
| `difftest.py` | differential gate: driver/inputs prompt, security guard, scenario ladder (libc whitelist / in-library + stub-side real-lib linking), private D-Bus, dual-build runner (`run_battery` judgment + abstraction + ikos hook), per-domain judge (`admission_mode`, `_ikos_phase`, 15 relational domains via `relation_class`), argv-seed retry, real-lib link resolution/repair, reports |
| `coevolve.py` | two-agent game (alternating moves): Iᴬ/Iᴮ abstraction (incl. ikos truth axes), d, `cell-<tag>` component, Φ, τ rule, strategy menu/ablation, equilibrium stopping + fail-closed battery, game.json |
| `stubdb.py` | stub database: per-shard JSON (name + signature key (+ domain) → body), verified>compiled ranking, shard-level atomic writes, legacy read-only compatibility |
| `difftest/dt.h` | `__dt_*` contract helper declarations (shared by dual build, observation over fd 4) |
| `difftest/dt_real.c` | contract implementation: registry/canary/snapshot/write-set diff |
| `difftest/recorder.c` | stub-side `__ikos_*` implementation: nondet schedule, assertion/check/havoc logging |
| `difftest/include/ikos/analyzer/intrinsic.h` | shadow header: intrinsics become interceptable extern functions (incl. macro-form assume) |
