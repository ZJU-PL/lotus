set (USED_LIBS
  seahorn.LIB
  SeaInstrumentation
  SeaTransformsScalar
  SeaTransformsUtils
  SeaAnalysis
  SeaSmt
  ${SEA_DSA_LIBS}
  SeaSupport
  ${LLVM_SEAHORN_LIBS}
  # SeaLlvmIpo  # Not available - from llvm-seahorn which wasn't migrated
  ${CLAM_LIBS}
  ${Z3_LIBRARIES}
  ${GMP_LIB}
  ${RT_LIB}
  )


set(LLVM_LINK_COMPONENTS
  BitWriter
  IRReader
  IPO
  ScalarOpts
  Instrumentation
  Core
  # XXX not clear why these last two are required
  CodeGen
  ObjCARCOpts
  TransformUtils
  Analysis
  Target)
add_executable(seahorn seahorn.cpp)
set_target_properties(seahorn PROPERTIES LINK_FLAGS "${LINK_FLAGS} -Wl,-search_paths_first")
# Link LLVM components first to establish base dependencies
llvm_config (seahorn ${LLVM_LINK_COMPONENTS})
# Then link our custom libraries
target_link_libraries (seahorn PRIVATE ${USED_LIBS})
install(TARGETS seahorn RUNTIME DESTINATION bin)

if (SEAHORN_STATIC_EXE)
  set (CMAKE_EXE_LINKER_FLAGS "-static -static-libgcc -static-libstdc++")
  set_target_properties (seahorn PROPERTIES LINK_SEARCH_START_STATIC ON)
  set_target_properties (seahorn PROPERTIES LINK_SEARCH_END_STATIC ON)
endif()

