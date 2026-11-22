#pragma once

#include "base.h"

struct child : base {
  virtual int foo() override;
  virtual int bar() override;
  int other();
};
