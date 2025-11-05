# Set output directories to keep build artifacts organized
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Ensure object files stay in the build directory
set(CMAKE_CXX_OUTPUT_EXTENSION_REPLACE ON)

# Add subdirectories
add_subdirectory(hice-dt)
add_subdirectory(chc_verifier)
