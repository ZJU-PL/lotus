# Verifier umbrella CMake for tools under tools/verifier
#
# Exposes toggles for the various verifier toolchains and delegates
# to their existing CMake configurations within this directory.
#
# This file is intended to be included from the top-level with:
#   add_subdirectory(tools/verifier)
#
# Available subprojects:
#   - sprattus      (BUILD_SPRATTUS)
#   - clam          (ENABLE_CLAM defined at top-level)
#   - horn-ice      (BUILD_HORN_ICE)
#   - seahorn       (ENABLE_SEAHORN, optional)

# Sprattus
option(BUILD_SPRATTUS "Build Sprattus verifier tools" ON)
if(BUILD_SPRATTUS)
    add_subdirectory(sprattus)
endif()

# CLAM tools (clam, clam-diff, clam-pp)
if(ENABLE_CLAM)
    add_subdirectory(clam)
endif()

# Seahorn tools (optional)
if(ENABLE_SEAHORN)
    add_subdirectory(seahorn)
endif()

# horn-ice
option(BUILD_HORN_ICE "Build ICE learning for CHC and Boogie" OFF)
if(BUILD_HORN_ICE)
    add_subdirectory(horn-ice)
endif()


