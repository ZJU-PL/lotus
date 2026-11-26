# Inclusion-Based Pointer Analyses

Every subdirectory under `lib/Alias/InclusionBased/` (and its matching header tree
under `include/Alias/InclusionBased/`) corresponds to a standalone **engine**.
Because most engines span multiple context-sensitivity configurations (e.g., `0-CFA`
and `k-CFA` inside the same solver), physical directories are organized by engine
while precision selection is exposed through this guide and the `AAConfig` presets
in `AliasAnalysisWrapper`.

## Quick Selection by Precision Tier

| Precision Tier | Recommended Engines | Wrapper Preset (`AAConfig`) |
| --- | --- | --- |
| **FICI** (Flow-Insensitive, Context-Insensitive) | `SparrowAA` (`k=0`), `AserPTA` (`ci`), `CclyzerAA` (`insensitive`) | `AAConfig::FICI()` (`"fici"`) |
| **FICS** (Flow-Insensitive, Context-Sensitive) | `AserPTA` (`1-cfa`/`2-cfa`/`origin`), `SparrowAA` (`k-cfa`), `CclyzerAA` (`k-cfa`/`k-caller`) | `AAConfig::FICS(k)` (`"fics"`, `"fics-2cfa"`) |
| **FSCI** (Flow-Sensitive, Context-Insensitive) | `SparsePTA` (`fspta` / `vfspta`), `ValueFlowPTA` (`vfpta`), `TPA` (`k=0`), `BootstrapAA` (`--all-contexts`) | `AAConfig::FSCI()` (`"fsci"`) |
| **FSCS** (Flow-Sensitive, Context-Sensitive) | `TPA` (`k-cfa` / `selective`), `BootstrapAA` (guarded summaries), `LotusAA` (summaries) | `AAConfig::FSCS(k)` (`"fscs"`, `"fscs-2cfa"`) |
| **Path-Sensitive** | `LotusAA` (TUNA must-kill forests / guarded heap), `BootstrapAA` (`--path-sensitive` ROBDD) | Direct API / CLI |

## Engine Matrix

| Directory | Engines / Modes | Flow | Context | Field | Path | Representation / Solver | CMake Target (`Alias`) | CLI Tool | Wrapper |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| [`SparrowAA`](SparrowAA/README.md) | Andersen | FI | CI, `k-CFA` | FI | ❌ | Constraint graph (HVN/HU/HCD/LCD) | `Andersen` (`Lotus::SparrowAA`) | `lotus-alias-sparrow-aa` | ✅ |
| [`AserPTA`](AserPTA/README.md) | Basic / Wave / Deep / PartialUpdate | FI | CI, `k-CFA`, Origin | FI / FS | ❌ | Constraint graph + OTF CG | `AserPTA` (`Lotus::AserPTA`) | `lotus-alias-aser-aa` | ❌ |
| [`CclyzerAA`](CclyzerAA/README.md) | cclyzer++ (`subset` / `unification`) | FI | CI, `k-CFA`, `k-Caller` | FS | ❌ | Datalog (Soufflé, optional) | `LotusCclyzerAA` (`Lotus::CclyzerAA`) | `lotus-alias-cclyzer-aa` | Optional |
| [`SparsePTA`](SparsePTA/README.md) | `FlowSensitivePTA` (`fspta`), `VersionedFlowSensitivePTA` (`vfspta`) | FS | CI | FS | ❌ | SVFG + MemorySSA / object versions | `SparsePTA` (`Lotus::SparsePTA`, `FlowSensitivePTA`) | `lotus-alias-fspta` | ❌ |
| [`ValueFlowPTA`](ValueFlowPTA/README.md) | `ValueFlowPTA` (`vfpta`) | FS | CI | FI | ❌ | Direct IR VFG (no SVFG / pre-analysis) | `ValueFlowPTA` (`Lotus::ValueFlowPTA`) | `lotus-alias-fspta --analysis=vfpta` | ❌ |
| [`TPA`](TPA/README.md) | `SemiSparsePointerAnalysis` | FS | CI, `k-CFA`, Adaptive | Partial | ❌ | Semi-sparse CFG (`Env` / `Store`) | `FSCSPointerAnalysis` (`Lotus::TPA`) | `lotus-alias-tpa` | ✅ |
| [`LotusAA`](LotusAA/README.md) | `LotusAA` | FS | CS (summaries) | FS | ✅ | Bottom-up summaries + guarded heap | `LotusAA` (`Lotus::LotusAA`) | `lotus-alias-lotus-aa` | ❌ |
| [`BootstrapAA`](BootstrapAA/README.md) | GuardedSummary / Tabulation | FS | CI (`AllContexts`), CS (exact path) | FS | Optional | Partitioning + guarded backward summaries | `CanaryBootstrapAA` (`Lotus::BootstrapAA`) | `lotus-alias-bootstrap` | ❌ |

## Notes on Choosing Between Overlapping Engines

- **Fast whole-program FI baseline**:
  - Choose **`SparrowAA`** for a lightweight, field-insensitive Andersen solver with HVN/HU/HCD/LCD simplifications or binary CSR constraint export, or when using `AliasAnalysisWrapper`.
  - Choose **`AserPTA`** when you need field sensitivity, on-the-fly call-graph construction, origin sensitivity, or result caching (also the default pre-analysis used by `SVFGBuilder`).
  - Choose **`CclyzerAA`** for Datalog-backed Soufflé analyses (`-DLOTUS_ENABLE_CCLYZER=ON`).
- **Flow-sensitive, context-insensitive (FSCI)**:
  - Choose **`SparsePTA`** (`FlowSensitivePTA` or `VersionedFlowSensitivePTA`) when field sensitivity, SVFG/MemorySSA integration, parallel evaluation, or concurrency (`FSMPTA`) is needed.
  - Choose **`ValueFlowPTA`** when you want a self-contained, field-insensitive Li et al. (ESEC/FSE'11) solver directly over LLVM IR with no SVFG or flow-insensitive pre-analysis dependency.
- **Flow- and context-sensitive (FSCS)**:
  - Choose **`TPA`** for semi-sparse `k-CFA` / selective context sensitivity, wrapper integration, or `TPATaint` clients.
  - Choose **`BootstrapAA`** for bootstrapped Steensgaard/Andersen-clustered guarded summaries with explicit call-path or all-context queries and optional ROBDD path sensitivity.
  - Choose **`LotusAA`** for summary-based, field- and path-sensitive analysis with TUNA must-kill strong updates and GVFG integration.
