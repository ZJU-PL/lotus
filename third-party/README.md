# Third-party libraries

Vendored or in-tree copies of external dependencies used by this project.

| Library | Directory | Description | Upstream |
|--------|-----------|-------------|----------|
| **CUDD** | `CUDD/` | CU Decision Diagram package — BDD/ADD/ZDD manipulation | [ivmai/cudd](https://github.com/ivmai/cudd) (mirror); original by Fabio Somenzi, University of Colorado |
| **WPDS** | `WPDS/` | Weighted pushdown system library (WALi-style) for interprocedural dataflow | Wisconsin/GrammaTech WALi lineage; see e.g. [WALi-OpenNWA](https://github.com/WaliDev/WALi-OpenNWA) |
| **WALi/OpenNWA** | `WALi-OpenNWA/` | Full WALi weighted automata library and OpenNWA nested-word automata implementation | [WaliDev/WALi-OpenNWA](https://github.com/WaliDev/WALi-OpenNWA) |
| **PDAAAL** | `PDAAAL/` | Weighted pushdown-system reachability library (`post*`/`pre*`/`dual*`) with shortest/longest witness traces and JSON PDS/P-automata parsers | [DEIS-Tools/PDAAAL](https://github.com/DEIS-Tools/PDAAAL) (Aalborg University; ATVA 2022) |
| **spdlog** | `spdlog/` | Fast C++ logging library (header-only) | [gabime/spdlog](https://github.com/gabime/spdlog) |
| **Mosaic** | `Mosaic/` | Theory-modular CHC solver over bit-vectors and integer arithmetic; powers `lotus-verify-mosaic` | Mosaic 1.0 artifact (Rappoport, Grumberg, Vizel; ATVA 2026), MIT-licensed |
| **CRAB** | `crab/` | Abstract interpretation library used by the vendored CLAM backend | [seahorn/crab](https://github.com/seahorn/crab) |
| **Verification backends** | `verification/` | Upstream-derived CLAM, SeaHorn, and SMACK sources | See `verification/README.md` |
| **FPsolve** | `fpsolve/` | Fixed-point solver based on Newton's method over omega-continuous semirings | TUM FPsolve project; see the vendored `fpsolve/README.md` |
| **Horn-ICE** | `horn-ice/` | Horn-ICE invariant/contract synthesis components and CHC verifier frontend | Adapted from [horn-ice/hice-dt](https://github.com/horn-ice/hice-dt) |
| **MDE** | `mde/` | Multilevel Deduplication Engine for compact data representation and cached set operations | Vendored MDE source; see `mde/README.md` |
| **Seal** | `seal/` | Symbolic automata tooling for stateful systems, including LLVM transforms and Z3 support code | Academic Seal artifact; see `seal/README.md` |
| **Stingx toolchain** | `stingx_toolchain/` | Local copy of the Stingx backend used by experimental invariant-generation scripts | Vendored Stingx backend source |
| **cclyzer++** | `cclyzerpp/` | Datalog-based scalable and precise pointer analysis for LLVM code (subset/unification, k-CFA) | [GaloisInc/cclyzerpp](https://github.com/GaloisInc/cclyzerpp) (BSD-3-Clause) |

## Usage

- **Include path**: The project adds `third-party/` to the global include path. Use `#include <spdlog/spdlog.h>`, `#include "CUDD/cudd.h"`, and `#include "WPDS/..."` as in the rest of the codebase.
- **CMake**: CUDD, WPDS, spdlog, and Mosaic are built via `third-party/CMakeLists.txt`; link targets `CanaryCUDD`, `wpds`, `wpds++` (and interfaces `ewpds`, `wpdsplusplus_util`), and `MosaicVerification` as needed. Mosaic exports its headers from `Mosaic/include/`, so consumers use `#include "Mosaic/..."`. WALi/OpenNWA is opt-in with `-DLOTUS_ENABLE_WALI_OPENNWA=ON` and exposes `WALi::wali`. FPsolve, Horn-ICE, Seal, and PDAAAL are opt-in with `-DLOTUS_ENABLE_FPSOLVE=ON`, `-DLOTUS_ENABLE_HORN_ICE=ON`, `-DLOTUS_ENABLE_SEAL=ON`, and `-DLOTUS_ENABLE_PDAAAL=ON` respectively. Enabling PDAAAL exposes the `pdaaal::pdaaal` CMake target and fetches its header-only dependencies (ptrie, PEGTL, Abseil, nlohmann_json) via FetchContent; it also requires Boost headers (≥ 1.70). MDE and the Stingx toolchain are vendored for local use but not wired into the build system. CRAB is configured from `cmake/ConfigureClamCrab.cmake` and is discovered from `third-party/crab/` by default. cclyzer++ is configured from `cmake/ConfigureCclyzerpp.cmake` and vendored under `third-party/cclyzerpp/` (enabled via `-DLOTUS_ENABLE_CCLYZER=ON` with Soufflé installed).

## Updating

When updating a vendored copy, preserve the layout and include paths above so existing `#include` directives and CMake targets continue to work.
