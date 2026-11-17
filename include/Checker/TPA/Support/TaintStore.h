#pragma once

#include "Checker/TPA/Support/TaintMap.h"

namespace tpa {
class MemoryObject;
} // namespace tpa

namespace tpa::taint {

using TaintStore = TaintMap<const tpa::MemoryObject *>;

} // namespace tpa::taint