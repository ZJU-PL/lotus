#pragma once

#include "abst.h"
#include "src2.h"

struct Concrete : Abstract {
  void foo(int &i) override;
};

Abstract *give_me();
