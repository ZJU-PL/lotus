find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
set(LOTUS_IKOS_SOURCE_DIR "${PROJECT_SOURCE_DIR}/third-party/ikos-api-spec")
set(LOTUS_IKOS_LLVM_BIN "${LOTUS_LLVM_TOOLS_BINARY_DIR}" CACHE PATH
    "LLVM 14 bin directory for the IKOS Python tools")

# Require a complete, matching toolchain rather than silently mixing system
# clang with the LLVM version used by Lotus.
foreach(tool clang llvm-dis llvm-link opt)
  set(executable "${LOTUS_IKOS_LLVM_BIN}/${tool}")
  if(NOT EXISTS "${executable}")
    message(FATAL_ERROR "IKOS API-spec requires ${executable}; set LOTUS_IKOS_LLVM_BIN to a complete LLVM 14 bin directory")
  endif()
  execute_process(COMMAND "${executable}" --version
    OUTPUT_VARIABLE version ERROR_VARIABLE error RESULT_VARIABLE result)
  if(NOT result EQUAL 0 OR NOT version MATCHES "version 14\\.")
    message(FATAL_ERROR "IKOS API-spec requires LLVM 14: ${executable}: ${version}${error}")
  endif()
endforeach()

foreach(IKOS_COMMAND stub api-census api-spec)
  set(launcher "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/lotus-ikos-${IKOS_COMMAND}")
  configure_file(lotus-ikos.in "${launcher}" @ONLY)
  install(PROGRAMS "${launcher}" DESTINATION bin COMPONENT ikos-api-spec)
endforeach()

if(LOTUS_ENABLE_IKOS)
  include("${PROJECT_SOURCE_DIR}/lib/Verification/Backends/ikos/Install.cmake")
  # Restrict clangd's header-command inference to IKOS translation units.
  # This target reads CMake's generated database and compiles no source files.
  add_custom_target(ikos-index
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/update_index.py"
      --build-dir "${CMAKE_BINARY_DIR}" --source-dir "${PROJECT_SOURCE_DIR}"
    COMMENT "Refreshing the IKOS clangd compilation database"
    VERBATIM)
  # The umbrella target makes the Python driver and all its native helpers
  # available even when Lotus's default library build is disabled.
  add_custom_target(lotus-ikos ALL DEPENDS ikos-analyzer ikos-pp ikos-import)
  foreach(entry "lotus-ikos|analyzer:main" "ikos|analyzer:main"
                "ikos-report|report:main" "ikos-view|view:main"
                "ikos-config|settings:main" "ikos-scan|scan:scan_main"
                "ikos-scan-extract|scan:extract_main"
                "ikos-scan-cc|compile:cc" "ikos-scan-c++|compile:c++"
                "ikos-stub|stub" "ikos-api-census|api-census")
    string(REPLACE "|" ";" fields "${entry}")
    list(GET fields 0 name)
    list(GET fields 1 IKOS_COMMAND)
    set(launcher "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${name}")
    configure_file(lotus-ikos.in "${launcher}" @ONLY)
    install(PROGRAMS "${launcher}" DESTINATION bin COMPONENT ikos)
  endforeach()
  # A standalone IKOS component install must include the Python pipeline too.
  install(DIRECTORY "${LOTUS_IKOS_SOURCE_DIR}/"
    DESTINATION share/lotus/ikos-api-spec COMPONENT ikos
    PATTERN "__pycache__" EXCLUDE PATTERN "*.pyc" EXCLUDE
    PATTERN "compile_commands.json" EXCLUDE)
  foreach(command stub api-census api-spec)
    install(PROGRAMS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/lotus-ikos-${command}"
      DESTINATION bin COMPONENT ikos)
  endforeach()
endif()

install(DIRECTORY "${LOTUS_IKOS_SOURCE_DIR}/"
  DESTINATION share/lotus/ikos-api-spec COMPONENT ikos-api-spec
  PATTERN "__pycache__" EXCLUDE PATTERN "*.pyc" EXCLUDE
  PATTERN "compile_commands.json" EXCLUDE)
