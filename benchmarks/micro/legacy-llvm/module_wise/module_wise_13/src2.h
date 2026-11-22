#pragma once

#include "src1.h"

struct B : A {
  virtual int foo(int &i) override;
};
