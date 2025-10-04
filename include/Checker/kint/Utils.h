#pragma once

#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/ADT/StringRef.h>

namespace kint {

/// \brief Utility functions for the Kint static analyzer
///
/// This namespace contains helper functions and utilities used throughout
/// the Kint static analysis framework for integer overflow detection.
namespace utils {

/// \brief Get a unique identifier for an LLVM instruction
///
/// This function extracts or generates a unique identifier for the given
/// instruction, which is used for tracking and debugging purposes in the
/// analysis framework.
///
/// \param Inst The LLVM instruction to get an ID for
/// \returns A unique integer identifier for the instruction
int64_t getInstructionId(llvm::Instruction *Inst);

} // namespace utils

/// \brief Simple wrapper for getInstructionId for compatibility with original implementation
int64_t get_id(llvm::Instruction *Inst);

} // namespace kint
