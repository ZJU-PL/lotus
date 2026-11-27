#include "SymbolicExecution/Support/HashUtil.h"

#include <cstdint>
#include <cstdio>

namespace SymbolicExecution {

size_t hashCombine(const std::vector<size_t> &HashVals) {
  size_t Seed = 0;
  for (size_t Value : HashVals) {
    Seed ^= Value + 0x9e3779b97f4a7c15ULL + (Seed << 6) + (Seed >> 2);
  }
  return Seed;
}

std::string ptrToString(const void *Ptr) {
  char Buf[32];
  int Len =
      std::snprintf(Buf, sizeof(Buf), "0x%zx", reinterpret_cast<uintptr_t>(Ptr));
  return Len > 0 ? std::string(Buf, static_cast<size_t>(Len)) : std::string("0x0");
}

} // namespace SymbolicExecution
