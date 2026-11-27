#pragma once

#include <string>

#include <llvm/ADT/DenseMap.h>
#include <llvm/Support/raw_ostream.h>

namespace llvm {
class ModuleSlotTracker;
class Value;
class Module;
} // namespace llvm

class Andersen;
class AndersenAAResult;

namespace sparrow_aa {

/// Precomputed display names for unnamed LLVM values in a module.
/// Avoids repeated ModuleSlotTracker::incorporateFunction() rescans when
/// printing points-to targets belonging to many different functions.
using ValueNameCache = llvm::DenseMap<const llvm::Value *, std::string>;

/// Build a ValueNameCache by scanning each function in @p M once.
ValueNameCache buildValueNameCache(const llvm::Module &M);

/// Print the points-to set for a given pointer value.
void printPointsToSet(const llvm::Value *V, Andersen &Anders,
                      llvm::raw_ostream &OS);

/// Print the points-to set using a shared ModuleSlotTracker.
void printPointsToSet(const llvm::Value *V, Andersen &Anders,
                      llvm::raw_ostream &OS, llvm::ModuleSlotTracker &MST);

/// Print the points-to set using a precomputed ValueNameCache (O(1) per value).
void printPointsToSet(const llvm::Value *V, Andersen &Anders,
                      llvm::raw_ostream &OS, const ValueNameCache &NameCache);

/// Perform and print alias queries between pointers in a module.
///
/// This function collects all pointer values in the module (globals and
/// instructions) and performs pairwise alias queries. It prints the results
/// and provides a summary of alias relationships (NoAlias, MayAlias,
/// MustAlias).
///
/// \param M The LLVM module to analyze
/// \param AAResult The Andersen alias analysis result
/// \param OS The output stream to write to
void performAliasQueries(llvm::Module &M, AndersenAAResult &AAResult,
                         llvm::raw_ostream &OS);

} // namespace sparrow_aa

