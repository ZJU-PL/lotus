#include "Checker/AE/AbstractStateStore.h"

#include <llvm/ADT/StringRef.h>

namespace lotus::analysis {

const char *toString(StateLayout layout) {
  switch (layout) {
  case StateLayout::Dense:
    return "dense";
  case StateLayout::SemiSparse:
    return "semi-sparse";
  case StateLayout::Sparse:
    return "sparse";
  }
  return "dense";
}

bool parseStateLayout(llvm::StringRef name, StateLayout &layout) {
  if (name == "dense") {
    layout = StateLayout::Dense;
    return true;
  }
  if (name == "semi-sparse") {
    layout = StateLayout::SemiSparse;
    return true;
  }
  if (name == "sparse") {
    layout = StateLayout::Sparse;
    return true;
  }
  return false;
}

} // namespace lotus::analysis
