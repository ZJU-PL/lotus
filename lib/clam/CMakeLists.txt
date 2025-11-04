# CLAM Analysis Library for Lotus
# Integrated from clam-dev14

# CRAB_ROOT should be set by parent CMakeLists.txt
if(NOT CRAB_ROOT)
  message(FATAL_ERROR "CRAB_ROOT not set. This should be configured in the main CMakeLists.txt")
endif()

if(EXISTS "${CRAB_ROOT}/include")
  message(STATUS "CLAM: Using CRAB at ${CRAB_ROOT}")
else()
  message(FATAL_ERROR "CLAM: CRAB not found at ${CRAB_ROOT}")
endif()

# Include CLAM headers
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../include/clam)

# Include sea-dsa headers (located in Alias/seadsa)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../include/Alias)

# Build CLAM Analysis library
add_library(ClamAnalysis STATIC
  CfgBuilder.cc
  CfgBuilderEmitters.cc  
  CfgBuilderLit.cc
  CfgBuilderMemRegions.cc
  CfgBuilderUtils.cc
  Clam.cc
  ClamQueryCache.cc
  NameValues.cc
  RegisterAnalysis.cc
  SeaDsaHeapAbstraction.cc
  SeaDsaHeapAbstractionUtils.cc
  SeaDsaToRegion.cc
  CrabDomainParser.cc
  crab/path_analysis/path_analyzer.cc
  crab/output/crabir/cfg_printer.cc
  crab/output/json/write_json.cc
  crab/output/json/read_json.cc
  crab/domains/intervals.cc
  crab/domains/boxes.cc
  crab/domains/dis_intervals.cc
  crab/domains/oct.cc
  crab/domains/pk.cc
  crab/domains/pk_pplite.cc  
  crab/domains/ric.cc
  crab/domains/sign_constant.cc
  crab/domains/split_dbm.cc
  crab/domains/split_oct.cc
  crab/domains/terms_dis_intervals.cc
  crab/domains/terms_intervals.cc
  crab/domains/terms_zones.cc
  crab/domains/wrapped_intervals.cc
  Support/BoostException.cc
  Support/CFGPrinter.cc
  Properties/MemoryCheckUtils.cc
  Properties/NullCheck.cc
  Properties/UafCheck.cc
  Properties/BndCheck.cc
)

# Set include directories for this target
target_include_directories(ClamAnalysis PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../include/Alias
  ${CRAB_ROOT}/include
)

# Link against Crab library
target_link_libraries(ClamAnalysis PUBLIC
  Crab
)

# Add optimizer if available
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Optimizer/Optimizer.cc")
  add_library(ClamOptimizer STATIC
    Optimizer/Optimizer.cc
  )
  
  target_include_directories(ClamOptimizer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CRAB_ROOT}/include
    #${LLVM_INCLUDE_DIRS}
  )
  
  target_link_libraries(ClamOptimizer PUBLIC
    ClamAnalysis
  )
  
  set_target_properties(ClamOptimizer PROPERTIES
    POSITION_INDEPENDENT_CODE ON
  )
endif()

message(STATUS "CLAM Analysis library configured")

# Add Transforms subdirectory (LLVM preprocessing passes)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Transforms/CMakeLists.txt")
  add_subdirectory(Transforms)
  message(STATUS "CLAM Transforms (LLVM passes) configured")
endif()
