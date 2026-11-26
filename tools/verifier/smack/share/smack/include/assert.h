//
// This file is distributed under the MIT License. See LICENSE for details.
//
#pragma once
#include <smack.h>

#define assert(EX)                                                             \
  do {                                                                         \
    if (!(EX))                                                                 \
      __VERIFIER_assert(0);                                                    \
  } while (0)
