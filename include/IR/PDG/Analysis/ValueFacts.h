#pragma once

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/Optional.h"
#include "llvm/IR/Instructions.h"

#include <string>
#include <vector>

namespace pdg {

/// Semantic facts shared by rule evaluation and Cypher properties. Unknown
/// facts stay unknown; textual LLVM dumps are never used to infer semantics.
class ValueFacts {
public:
  /// Unique targets through aliases, immutable pointer globals, unchanged
  /// local spills and agreeing select/phi alternatives. Ambiguity stays null.
  static const llvm::Function *callee(const llvm::CallBase &call);
  static const llvm::Value *argument(const llvm::CallBase &call,
                                     unsigned index);
  static llvm::Optional<llvm::APInt> integer(const llvm::Value &value);
  static llvm::Optional<uint64_t> objectBytes(const llvm::Value &pointer);
  static std::vector<const llvm::Value *>
  localStackOrigins(const llvm::Value &pointer, const llvm::Function &function);

  /// Recognizes strlen/wcslen(source) with constant affine offsets >= 0.
  /// This is expression provenance, not a runtime range or no-wrap proof.
  static const llvm::Value *lengthSource(const llvm::Value &value);
  static std::string functionBaseName(const llvm::Function &function);
  static llvm::Optional<std::string> constantString(const llvm::Value &value);
  /// Reads a unique integer macro definition from -fdebug-macro metadata.
  /// Unknown or conflicting definitions remain unknown; no host ABI guesses.
  static llvm::Optional<uint64_t> macroInteger(const llvm::Module &module,
                                               llvm::StringRef name);
  static bool macroDefined(const llvm::Module &module, llvm::StringRef name);
  static bool hasLibraryName(const llvm::Function &function,
                             const std::string &name, bool allow_std = false,
                             bool allow_bsl = false);
};

} // namespace pdg
