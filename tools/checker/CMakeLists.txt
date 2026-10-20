# Unified checker front-end.

include(GNUInstallDirs)

llvm_map_components_to_libnames(LLVM_LINK_COMPONENTS
  Analysis
  IRReader
  Passes
)

add_executable(lotus-check
  lotus-check-generic.cpp
  lotus-check-gsaf.cpp
  lotus-check-kint.cpp
  lotus-check-taint.cpp
  lotus-check-concur.cpp
  lotus-check-pulse.cpp
  lotus-check-fitx.cpp
  lotus-check-saber.cpp
  lotus-check-ae.cpp
  lotus-check-symex.cpp
)

target_link_libraries(lotus-check PRIVATE
  Annotation
  CanaryAliasAnalysisWrapper
  CanaryCheckerTooling
  CanaryKint
  CanarySymbolicExecution
  CheckerFramework
  GSAF
  ConcurrencyChecker
  FiTxChecker
  IFDS
  PulseChecker
  ${LLVM_LINK_COMPONENTS}
  ${Z3_LIBRARIES}
)

target_compile_definitions(lotus-check PRIVATE
  LOTUS_INSTALL_CHECKER_SPEC_DIR="${CMAKE_INSTALL_FULL_DATADIR}/lotus/checkers"
  LOTUS_SOURCE_CHECKER_SPEC_DIR="${PROJECT_SOURCE_DIR}/config/checkers"
)

install(DIRECTORY ${PROJECT_SOURCE_DIR}/config/checkers/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/lotus/checkers)
install(TARGETS lotus-check
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The unified frontend directly references the Saber and AE entry points and
# option accessors, so normal static-library extraction retains the required
# objects without platform-specific whole-archive linker flags.
target_link_libraries(lotus-check PRIVATE Saber AE)
