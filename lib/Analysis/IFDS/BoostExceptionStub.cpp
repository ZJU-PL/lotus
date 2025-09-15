/*
 * Simple stub implementation for boost::throw_exception when BOOST_NO_EXCEPTIONS is defined
 * This allows sparta to compile without full boost exception support
 */

#include <exception>
#include <iostream>
#include <cstdlib>

namespace boost {

void throw_exception(const std::exception& e) {
    std::cerr << "Exception: " << e.what() << std::endl;
    std::abort();
}

} // namespace boost
