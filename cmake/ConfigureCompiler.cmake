set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions -frtti")

# Suppress noisy unused warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
  add_compile_options(
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-but-set-variable
    -Wreorder-ctor
    -Wignored-qualifiers
  )
  if(CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$")
    add_compile_options(-Wno-unused-private-field)
  endif()
endif()

if(APPLE)
  # BSD `ar` appends duplicate members when invoked with `q` against an
  # existing archive. Use `r` so repeated static-library link steps replace
  # members instead of corrupting the archive with duplicate objects.
  set(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> cr <TARGET> <LINK_FLAGS> <OBJECTS>")
  set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> cr <TARGET> <LINK_FLAGS> <OBJECTS>")
  include(CheckLinkerFlag)
  check_linker_flag(CXX "-Wl,-no_warn_duplicate_libraries" LOTUS_HAS_NO_WARN_DUPLICATE_LIBS)
  if(LOTUS_HAS_NO_WARN_DUPLICATE_LIBS)
    add_link_options("-Wl,-no_warn_duplicate_libraries")
  endif()
endif()
