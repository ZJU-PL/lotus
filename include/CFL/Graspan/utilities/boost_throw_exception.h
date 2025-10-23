// Minimal boost::throw_exception implementation for graspan
// This provides a simple implementation that avoids the need for boost_exception library

#include <exception>
#include <iostream>
#include <cstdlib>

namespace boost {

// Basic implementation that just handles the simple case
inline void throw_exception(const std::exception& e) {
    std::cerr << "Boost exception: " << e.what() << std::endl;
    std::abort();
}

// Forward declaration for source_location (it may not be available in all boost versions)
struct source_location;

// Implementation for the source_location version
inline void throw_exception(const std::exception& e, const source_location& loc) {
    std::cerr << "Boost exception: " << e.what() << std::endl;
    std::abort();
}

} // namespace boost
