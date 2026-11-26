#pragma once

#include "llvm/ADT/Optional.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include "IR/PDG/Models/FindingTypes.h"

#include <cstdint>
#include <memory>

namespace pdg {

struct TaintFlowResult;

enum class StringTermination { Unknown, Terminated, Unproven };

/// Content facts immediately before a particular instruction, with lengths in
/// bytes excluding the terminator. Unproven means a tracked local source has
/// no established terminator; Unknown includes escaped or unmodeled contents.
struct StringBounds {
  StringTermination termination = StringTermination::Unknown;
  llvm::Optional<uint64_t> minimum_bytes;
  llvm::Optional<uint64_t> maximum_bytes;
  bool value_flow = false;
  std::vector<const llvm::Instruction *> evidence;
};

/// Formatting output bounds include the terminating character. The limited
/// float estimate follows CodeQL's convention of capping each %f conversion
/// at eight characters, so callers can distinguish float-specific findings.
struct FormatBounds {
  bool output_buffer = false;
  llvm::Optional<unsigned> explicit_limit_argument;
  llvm::Optional<uint64_t> maximum_bytes;
  llvm::Optional<uint64_t> maximum_bytes_without_large_floats;
  bool value_flow = false;
  bool floating_conversion = false;
  std::vector<unsigned> unbounded_string_arguments;
};

/// Bounds refer to the closest statically identified subobject, not necessarily
/// the enclosing allocation. A pointer one past the end is valid to construct;
/// BoundsAnalysis reports it only when a nonempty access dereferences it.
struct MemoryRegion {
  const llvm::Value *allocation = nullptr;
  uint64_t capacity = 0;
  int64_t offset = 0;
  bool heap = false;
  bool static_character_array = false;
};

struct BufferAccess {
  const llvm::Instruction *site = nullptr;
  const llvm::Value *pointer = nullptr;
  uint64_t bytes = 0;
  bool writes = false;
  MemoryRegion region;
  bool outOfBounds() const;
};

struct BoundsAnalysisResult {
  std::vector<AnalysisFinding> findings;
};

/// PDG-native object/subobject and access semantics, built solely on LLVM
/// alias analysis, dominators, and MemorySSA. No external bug engine is used.
/// Results remain valid while the module is unchanged. Unknown offsets and
/// sizes stay unknown; dependence reachability is not a bounds proof.
class BoundsAnalysis {
public:
  BoundsAnalysis();
  explicit BoundsAnalysis(llvm::Module &module);
  ~BoundsAnalysis();
  llvm::Optional<MemoryRegion> region(const llvm::Value &pointer) const;
  std::vector<BufferAccess> accesses(const llvm::Instruction &site) const;
  StringBounds stringBounds(const llvm::Value &pointer,
                            const llvm::Instruction &at) const;
  FormatBounds formatBounds(const llvm::CallBase &call) const;
  std::vector<AnalysisFinding> analyze() const;
  static const std::vector<AnalysisRuleDescriptor> &catalog();
  static bool requiresTaint(const std::string &id);
  BoundsAnalysisResult analyze(const llvm::Module &module,
                            const TaintFlowResult *taint = nullptr) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace pdg
