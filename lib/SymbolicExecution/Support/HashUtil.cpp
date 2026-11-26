#include "SymbolicExecution/Support/HashUtil.h"

#include <sstream>

namespace SymbolicExecution {

size_t hashCombine(const std::vector<size_t> &HashVals) {
  size_t Seed = 0;
  for (size_t Value : HashVals) {
    Seed ^= Value + 0x9e3779b97f4a7c15ULL + (Seed << 6) + (Seed >> 2);
  }
  return Seed;
}

std::string ptrToString(const void *Ptr) {
  std::ostringstream OS;
  OS << Ptr;
  return OS.str();
}

} // namespace SymbolicExecution
