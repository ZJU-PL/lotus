#pragma once

namespace tpa::taint {
class DefUseFunction;
} // namespace tpa::taint

namespace util {
namespace io {

bool writeDotFile(const char *filePath,
                  const tpa::taint::DefUseFunction &duFunc);

} // namespace io
} // namespace util
