# The certificate kernels are header-only and need only Boost.Multiprecision.
# Find headers here even when all Boost-dependent verification tools are off.
if(LOTUS_CUSTOM_BOOST_ROOT)
  set(BOOST_ROOT "${LOTUS_CUSTOM_BOOST_ROOT}")
endif()
find_package(Boost 1.65 QUIET)
if(NOT Boost_FOUND)
  message(STATUS "Boost headers not found; skipping CFL Certificates")
  return()
endif()
add_library(CanaryCFLCertificates INTERFACE)

add_library(CanaryCFLCertificateAdapter STATIC LotusAdapter.cpp)
target_link_libraries(CanaryCFLCertificateAdapter PUBLIC
  CanaryCFLCertificates CanaryClassicalCFL)
