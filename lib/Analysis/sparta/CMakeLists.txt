# Sparta core library - no LLVM-specific code
# IFDS has been moved to independent directory

# Create interface library for compatibility
add_library(SpartaLLVM INTERFACE)

# Set C++ standard
set_property(TARGET SpartaLLVM PROPERTY CXX_STANDARD 17)
set_property(TARGET SpartaLLVM PROPERTY CXX_STANDARD_REQUIRED ON)
