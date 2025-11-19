===========================
CFLAA — CFL-Based Alias AA
===========================

Overview
========

CFLAA is the **Context-Free Language Alias Analysis** previously shipped with
LLVM (up to LLVM 14). Lotus vendors this implementation to provide an
alternative, CFL-based alias analysis for experimentation and compatibility.

* **Location**: ``lib/Alias/CFLAA``
* **Origins**: Copied from LLVM 14.0.6 (removed in LLVM 15)
* **Role**: Reference implementation and baseline CFL-style analysis

Characteristics
===============

* Flow-insensitive and mostly context-insensitive.
* Uses CFL reachability over a labeled graph to compute aliasing.
* Integrates with the LLVM AliasAnalysis (AA) infrastructure.

Usage
=====

You typically do not invoke CFLAA directly. Instead, it is enabled via the
AA wrapper or through LLVM pass pipelines that request the CFL-based AA
result.

For most users, it serves as a **compatibility layer** or a comparative
baseline next to DyckAA and other more specialized analyses.


