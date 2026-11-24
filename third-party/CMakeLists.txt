if(LOTUS_ENABLE_SPARTA)
  # Header-only abstract interpretation primitives.
  add_subdirectory(SPARTA)
endif()

# Third-party libraries (CUDD, WPDS, spdlog, Mosaic)
add_subdirectory(CUDD)
add_subdirectory(WPDS)
add_subdirectory(spdlog)
add_subdirectory(Mosaic)
if(LOTUS_ENABLE_FLOW_CUTTER)
  add_subdirectory(flow-cutter-pace17)
  add_subdirectory(flow-cutter-pace20)
endif()
if(LOTUS_ENABLE_DEMAND_APA)
  add_subdirectory(buddy-2.4)
endif()
if(LOTUS_ENABLE_HORN_ICE)
  add_subdirectory(horn-ice)
endif()
if(LOTUS_ENABLE_WALI_OPENNWA)
  # Keep the larger WALi/OpenNWA distribution opt-in. Lotus already builds its
  # legacy WPDS subset above, so this only exposes the full WALi::wali target
  # when explicitly requested.
  set(WALI_BUILD_SHARED OFF CACHE BOOL "Build WALi libraries as shared libraries" FORCE)
  set(WALI_BUILD_TOOLS OFF CACHE BOOL "Build OpenNWA command-line tools" FORCE)
  set(WALI_BUILD_ADDONS OFF CACHE BOOL "Build optional WALi add-on libraries" FORCE)
  set(WALI_BUILD_EXAMPLES OFF CACHE BOOL "Build WALi example programs" FORCE)
  add_subdirectory(WALi-OpenNWA EXCLUDE_FROM_ALL)
endif()
if(LOTUS_ENABLE_FPSOLVE)
  # Vendor FPsolve as a library dependency without pulling in its optional
  # test/tool surface or extra external packages by default.
  set(FPSOLVE_BUILD_TOOLS OFF CACHE BOOL "Build FPsolve command-line tools" FORCE)
  set(FPSOLVE_BUILD_TESTS OFF CACHE BOOL "Build FPsolve unit tests" FORCE)
  set(USE_GENEPI OFF CACHE BOOL "Use genepi and semilinear-set NDD support" FORCE)
  set(USE_LIBFA OFF CACHE BOOL "Use libfa and lossy finite automata support" FORCE)
  add_subdirectory(fpsolve EXCLUDE_FROM_ALL)
endif()
if(LOTUS_ENABLE_SEAL)
  # Seal symbolic automata lifter for stateful systems.
  add_subdirectory(seal EXCLUDE_FROM_ALL)
endif()
if(LOTUS_ENABLE_PDAAAL)
  # PDAAAL: weighted pushdown-system reachability (post*/pre*/dual*) from
  # Aalborg University. Builds only the pdaaal library target; tests,
  # benchmarks, and the stand-alone binary are suppressed. Its internal
  # header-only dependencies (ptrie, PEGTL, Abseil, nlohmann_json) are
  # fetched via FetchContent when PDAAAL_GetDependencies=ON (the default).
  # PDAAAL also requires Boost headers; ensure Boost is configured beforehand
  # (LOTUS_ENABLE_CLAM, LOTUS_ENABLE_SEAHORN, or LOTUS_DOWNLOAD_BOOST suffice,
  # but callers that enable only PDAAAL must arrange Boost separately or rely
  # on LOTUS_DOWNLOAD_BOOST).
  set(PDAAAL_VERSION 1.1.0 CACHE STRING "PDAAAL library version" FORCE)
  set(PDAAAL_BuildTests   OFF CACHE BOOL "Build PDAAAL unit tests"           FORCE)
  set(PDAAAL_Build_Main   OFF CACHE BOOL "Build PDAAAL stand-alone binary"   FORCE)
  set(PDAAAL_BuildBundle  OFF CACHE BOOL "Build PDAAAL as a bundled binary"  FORCE)
  set(PDAAAL_BuildStatic  OFF CACHE BOOL "Build PDAAAL as a fully static binary" FORCE)
  set(PDAAAL_GetDependencies ON CACHE BOOL "Fetch PDAAAL header-only deps via FetchContent" FORCE)
  add_subdirectory(PDAAAL EXCLUDE_FROM_ALL)
endif()
