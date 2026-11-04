# Compile each public header as the first and only include of a separate TU.
# Linking them together also catches header-defined symbols with non-inline
# linkage; unity builds and PCHs must not mask missing includes.
set(demand_apa_headers
  Algebra.h
  Algorithm.h
  ApaInstance.h
  BpReader.h
  CentroidPreprocessing.h
  Comparison.h
  DemandOmp.h
  FunctionSummaries.h
  IfdsReader.h
  Interprocedural.h
  Intraprocedural.h
  LCA.h
  Naive.h
  PrePreprocess.h
  Project.h
  Reader.h
  RegEx.h
  SCC.h
  SPReader.h
  SameBagPreprocessing.h
  SparseTable.h
  Support.h
  Tarjan.h
  TreeDecomposition.h
)
set(header_checks)
foreach(header IN LISTS demand_apa_headers)
  set(source "${CMAKE_CURRENT_BINARY_DIR}/headers/${header}.cpp")
  # file(CONFIGURE) instead of file(GENERATE): these files are consumed by a
  # target in the parent directory, and file(GENERATE) only writes its output
  # during the generate phase, after the parent's source-existence check has
  # already failed on a fresh build directory.
  file(CONFIGURE OUTPUT "${source}"
    CONTENT "#include \"Dataflow/DemandAPA/${header}\"\n")
  list(APPEND header_checks "${source}")
endforeach()

set_source_files_properties(${header_checks} HeaderIntegrationTest.cpp HeaderOrderPeer.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON SKIP_UNITY_BUILD_INCLUSION ON)

lotus_collect_test_sources(dataflow_tests
  HeaderIntegrationTest.cpp HeaderOrderPeer.cpp ${header_checks})
