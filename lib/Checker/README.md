# Checker Library

Bug checkers and reporting infrastructure.

**Status**: Most checkers are unstable; many were migrated from other projects (notably SVF) and may require further integration work.

| Subdir | Purpose | Status |
|--------|---------|--------|
| **AE** | Abstract Execution (abstract interpretation engine). Migrated from SVF's AE engine. | ⚠️ Migrated (from SVF), unstable |
| **Concurrency** | Thread-safety: Atomicity, ConditionVariable, DataRace, Deadlock, LockMismatch, and Infer-inspired Starvation critical-pair summaries. | ⚠️ Unstable |
| **FiTx** | Detectors: double-free, double-lock/unlock, leak, null-ptr, ref/ unref, UAF, use-before-init. | ✅ Stable |
| **KINT** | Integer bug detection. Taint analysis, SMT (Z3). | ⚠️ Unstable |
| **Pulse** | Biabductive analysis (Infer Pulse-style). Witnessable bugs, disjunctive domain, loop abstraction, and TOPL temporal properties. | ⚠️ Migrated (from Infer), Unstable |
| **Saber** | Source-sink bug detector. Migrated from SVF's SABER engine. Checkers: memory leak, double-free, file operations (fopen/fclose), and context-bounded use-after-free candidates. | ⚠️ Migrated (from SVF), unstable |
| **GSAF** | Generalized Static Analysis Framework: graph/GVFG-based taint and UAF traversal, SMT path encoding, composite trace summaries. | ⚠️ Unstable |
| **TypeQualifier** | Flow- and context-sensitive type-qualifier inference and checking based on pointer analysis. | ⚠️ Unstable |
| **Tooling** | Command-line options and reporting utilities for checker tools. | ✅ Stable |
| **Framework** | Shared checker infrastructure: registry, driver, declarative specs, diagnostics, and reporting (BugReport, BugReportMgr, BugTypes, SARIF, SuppressionManager). | ✅ Stable |

Security-oriented side-channel components such as Spectre live under
`lib/Security`, not in the checker layer.
