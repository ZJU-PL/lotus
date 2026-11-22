#pragma once

#include "abst.h"

struct OtherConcrete : Abstract {
  void foo(int &i) override;
};
