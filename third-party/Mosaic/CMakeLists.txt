add_library(MosaicVerification STATIC
  Bv2IntTranslator.cpp
  CHCParser.cpp
  InstCombiner.cpp
  Int2BvOverflowAnalyzer.cpp
  Int2BvPreprocessor.cpp
  Int2BvTranslator.cpp
  MosaicFixedpoint.cpp
  MosaicUtils.cpp
  Z3Utils.cpp)

target_include_directories(MosaicVerification PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(MosaicVerification PUBLIC Z3::z3)

# Z3_OP_RECURSIVE is missing from older system Z3 releases.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES "${Z3_INCLUDE_DIR}")
check_cxx_source_compiles(
  "#include <z3.h>\nint main() { Z3_decl_kind kind = Z3_OP_RECURSIVE; return kind == Z3_OP_UNINTERPRETED; }"
  LOTUS_Z3_HAS_OP_RECURSIVE)
unset(CMAKE_REQUIRED_INCLUDES)
if(LOTUS_Z3_HAS_OP_RECURSIVE)
  target_compile_definitions(MosaicVerification PRIVATE LOTUS_Z3_HAS_OP_RECURSIVE)
endif()

install(TARGETS MosaicVerification DESTINATION lib)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/Mosaic
  DESTINATION include)
