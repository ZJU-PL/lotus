// Boost requires the application to provide boost::throw_exception when
// BOOST_NO_EXCEPTIONS is defined. ConfigureBoost.cmake defines that macro
// whenever the LLVM toolchain was built without exception handling (the
// distro LLVM packages on Linux report LLVM_ENABLE_EH=OFF). The certificate
// kernels include Boost.Multiprecision, whose headers reference the hook, so
// consumers would otherwise fail to link with an undefined reference to
// boost::throw_exception.
//
// Lotus itself is compiled with exceptions enabled, but Boost's own error
// paths stay disabled here. Report the diagnostic and abort, matching the
// usual Boost guidance for a no-exceptions build.

#include <boost/throw_exception.hpp>
#include <boost/version.hpp>

#include <cstdio>
#include <cstdlib>
#include <exception>

#if defined(BOOST_NO_EXCEPTIONS)

namespace boost {
namespace {

[[noreturn]] void reportAndAbort(std::exception const &e) {
  std::fputs("Boost exception: ", stderr);
  std::fputs(e.what(), stderr);
  std::fputc('\n', stderr);
  std::abort();
}

} // namespace

void throw_exception(std::exception const &e) { reportAndAbort(e); }

// The source_location overload only exists since Boost 1.74.
#if BOOST_VERSION >= 107400
void throw_exception(std::exception const &e, source_location const &) {
  reportAndAbort(e);
}
#endif

} // namespace boost

#endif
