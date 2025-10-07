
/*
 *  Popeye lifts protocol source code in C to its specification in BNF
 *  Copyright (C) 2021 Qingkai Shi <qingkaishi@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Affero General Public License as published
 *  by the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Affero General Public License for more details.
 *
 *  You should have received a copy of the GNU Affero General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


#ifndef SUPPORT_DEBUG_H
#define SUPPORT_DEBUG_H

#include <llvm/Support/raw_ostream.h>

#ifdef NDEBUG
#define POPEYE_WARN(X)
#else
#define POPEYE_WARN(X) llvm::outs() << "[WARN] " << X << "\n"
#endif

#define POPEYE_INFO(X) llvm::outs() << "[INFO] " << X << "\n"

/// @{
bool isPopeyeCurrentDebugType(const char *Type);

#define POPEYE_DEBUG_WITH_TYPE(TYPE, X)                                        \
  do { if (PopeyeDebugFlag && isPopeyeCurrentDebugType(TYPE)) { X; } \
  } while (false)

#define POPEYE_DEBUG(X) POPEYE_DEBUG_WITH_TYPE(DEBUG_TYPE, X)

extern bool PopeyeDebugFlag;
/// @}

#endif //SUPPORT_DEBUG_H
