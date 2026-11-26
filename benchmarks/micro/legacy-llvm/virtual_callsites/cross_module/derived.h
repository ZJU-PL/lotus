#pragma once

#include "base.h"

struct derived : base {
  void foo();
  virtual int bar();
};
