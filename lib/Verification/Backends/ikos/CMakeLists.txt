# Keep IKOS's dependency discovery and compile definitions local to its targets.
set(IKOS_ROOT "${PROJECT_SOURCE_DIR}/third-party/ikos-api-spec")
include(Sources.cmake)
if(LOTUS_CUSTOM_BOOST_ROOT)
  set(BOOST_ROOT "${LOTUS_CUSTOM_BOOST_ROOT}")
endif()
find_package(Boost 1.65 REQUIRED COMPONENTS filesystem thread)
find_package(TBB REQUIRED CONFIG)
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
list(PREPEND CMAKE_MODULE_PATH "${IKOS_ROOT}/cmake")
find_package(GMP REQUIRED)
if(LOTUS_IKOS_ENABLE_APRON)
  find_package(APRON REQUIRED)
endif()

add_library(CanaryIKOSCore INTERFACE)
target_include_directories(CanaryIKOSCore SYSTEM INTERFACE
  "${IKOS_ROOT}/core/include" ${GMP_INCLUDE_DIR} ${GMPXX_INCLUDE_DIR})
target_link_libraries(CanaryIKOSCore INTERFACE
  Boost::boost ${GMPXX_LIB} ${GMP_LIB} TBB::tbb Threads::Threads)
if(LOTUS_IKOS_ENABLE_APRON)
  target_compile_definitions(CanaryIKOSCore INTERFACE HAS_APRON)
  target_include_directories(CanaryIKOSCore SYSTEM INTERFACE ${APRON_INCLUDE_DIRS})
  target_link_libraries(CanaryIKOSCore INTERFACE ${APRON_LIBRARIES})
endif()

add_library(CanaryIKOSAR STATIC ${IKOS_AR_SOURCES})
target_include_directories(CanaryIKOSAR PUBLIC "${IKOS_ROOT}/ar/include")
target_link_libraries(CanaryIKOSAR PUBLIC CanaryIKOSCore)

llvm_map_components_to_libnames(ikos_llvm_libs
  aggressiveinstcombine analysis asmparser bitreader bitwriter codegen core
  coroutines instcombine instrumentation ipo irreader mc objcarcopts scalaropts
  support target transformutils vectorize)
add_library(CanaryIKOSFrontend STATIC ${IKOS_FRONTEND_SOURCES})
target_include_directories(CanaryIKOSFrontend PUBLIC "${IKOS_ROOT}/frontend/llvm/include")
target_link_libraries(CanaryIKOSFrontend PUBLIC CanaryIKOSAR ${ikos_llvm_libs}
  Boost::filesystem Boost::thread)

add_library(CanaryIKOSPreprocessor STATIC ${IKOS_PP_SOURCES})
target_include_directories(CanaryIKOSPreprocessor PUBLIC "${IKOS_ROOT}/frontend/llvm/include")
target_link_libraries(CanaryIKOSPreprocessor PUBLIC CanaryIKOSCore ${ikos_llvm_libs})

add_executable(ikos-analyzer ${IKOS_ANALYZER_SOURCES})
target_include_directories(ikos-analyzer PRIVATE "${IKOS_ROOT}/analyzer/include")
target_link_libraries(ikos-analyzer PRIVATE CanaryIKOSFrontend SQLite::SQLite3)
add_executable(ikos-pp "${IKOS_ROOT}/frontend/llvm/src/ikos_pp.cpp")
target_link_libraries(ikos-pp PRIVATE CanaryIKOSPreprocessor Boost::filesystem)
add_executable(ikos-import "${IKOS_ROOT}/frontend/llvm/src/ikos_import.cpp")
target_link_libraries(ikos-import PRIVATE CanaryIKOSFrontend)

