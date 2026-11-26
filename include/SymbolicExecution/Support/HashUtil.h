/** @file HashUtil.h @brief Hashing and pointer formatting helpers for symbolic execution. */
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace SymbolicExecution {

size_t hashCombine(const std::vector<size_t> &HashVals);
std::string ptrToString(const void *Ptr);

} // namespace SymbolicExecution
