# CFL GTest sources share one binary. Optional certificate sources and their
# adapter are included only when Boost headers are available.
add_subdirectory(Classical)
add_subdirectory(ConditionalDyck)
add_subdirectory(CSIndex)
add_subdirectory(DynamicDyck)
add_subdirectory(InterleavedDyck)

set(cfl_certificate_libs)
if(TARGET CanaryCFLCertificateAdapter)
  add_subdirectory(Certificates)
  list(APPEND cfl_certificate_libs CanaryCFLCertificateAdapter)
endif()

add_lotus_collected_test_suite(cfl_tests
  TIMEOUT 240
  LINK_LIBS
    ${cfl_certificate_libs}
    CanaryConditionalDyck
    CanaryDynamicDyck
    CanaryInterleavedDyckLCL
    CanaryInterleavedDyckILP
    CanaryInterleavedDyckSPDS
    CanaryInterleavedDyckAffineSPDS
    CanaryInterleavedDyckUnary
    CanaryInterleavedDyckStagedBounds
    CanaryInterleavedDyckMCFL
    CanaryClassicalCFL
    CanaryClassicalCFLAliasClient
    CanaryClassicalCFLValueFlowClient
    CanaryCSIndexSCS
  COMPILE_DEFINITIONS
    INTERLEAVED_DYCK_DATASET_DIR="${CMAKE_SOURCE_DIR}/benchmarks/real-world/CFL/InterleavedDyck/taint"
    MCFL_DATASET_DIR="${CMAKE_SOURCE_DIR}/benchmarks/real-world/CFL/InterleavedDyck/taint")
