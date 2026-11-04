/**
 * @file Common.h
 * @brief Common utilities and type definitions for TypeQualifier analysis
 *
 * This file provides common utilities, type definitions, and helper functions
 * used throughout the TypeQualifier analysis framework. It includes definitions
 * for global context management, utility classes for value and type
 * manipulation, and commonly used container types.
 *
 * @ingroup TypeQualifier
 */

#pragma once
#include "Checker/TypeQualifier/FunctionSummary.h"

#include <llvm/Support/raw_ostream.h>

/*#define HT_LOG(lv, stmt) \
        do {
\
                if (VerboseLevel >= lv) \
                        errs() << stmt;
\ } while(0)

#define AA_LOG(stmt) HT_LOG(2, stmt)*/
#define OP errs()

#include <unordered_map>

#define FUNCTION_TIMER()
