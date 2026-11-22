#pragma once

#include "src1.h"
// similar to module_wise_13 for testing projects with
// overlaping functions and types
struct B : A {
  virtual int foo(int &i) override;
};
