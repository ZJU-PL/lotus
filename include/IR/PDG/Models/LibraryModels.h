#pragma once

#include "llvm/ADT/Optional.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pdg {

struct BufferCopyModel {
  unsigned destination;
  unsigned source;
  unsigned size;
};

struct FormatModel {
  unsigned format;
  unsigned first_argument;
  bool scanf;
  bool wide = false;
};

enum class AllocationKind { Unknown, Malloc, New, NewArray };
enum class ReleaseKind { Unknown, Free, Delete, DeleteArray };

enum class TaintChannel { Value, Memory, ObjectContent };
enum class TaintDomain {
  Process,
  Command,
  Sql,
  Path,
  Format,
  Allocation,
  Wordexp
};
struct TaintEndpoint {
  int argument = -1; // -1 is the return value
  TaintChannel channel = TaintChannel::Memory;
  bool numeric = false;
};
struct TaintTransfer {
  TaintEndpoint input;
  TaintEndpoint output;
  bool concatenates = false;
  bool numeric = false;
  bool sql_barrier = false;
};
struct CallTaintModel {
  bool known = false;
  std::vector<TaintEndpoint> sources;
  std::vector<TaintTransfer> transfers;
  std::vector<TaintEndpoint> overwrites;
};

/// A sink is disabled when its flag argument provably contains any mask bit.
/// The same parameter roles can be instantiated at transparent wrapper calls.
struct TaintSinkGuard {
  unsigned argument = 0;
  unsigned flag_argument = 0;
  uint64_t disabled_by_any_set_bits = 0;
};

/// Shared argument roles and effects for semantic checks. Unknown APIs retain
/// unknown effects; no fallback infers a model from a substring in an IR dump.
class LibraryModels {
public:
  static llvm::Optional<BufferCopyModel> copy(const llvm::Function &function);
  static llvm::Optional<FormatModel> format(const llvm::Function &function);
  static bool readsOnly(const llvm::Function &function);
  static AllocationKind allocation(const llvm::Function &function);
  static ReleaseKind release(const llvm::Function &function);
  static llvm::Optional<unsigned> returnAlias(const llvm::CallBase &call);
  static CallTaintModel taint(const llvm::CallBase &call);
  static std::vector<unsigned> taintSinks(const llvm::CallBase &call,
                                          TaintDomain domain);
  static std::vector<TaintSinkGuard> taintSinkGuards(const llvm::CallBase &call,
                                                     TaintDomain domain);
  static bool sinkGuardDisabled(const llvm::CallBase &call,
                                const TaintSinkGuard &guard);
};

struct FormatFacts {
  struct Conversion {
    unsigned argument = 0; // one based among format arguments
    char conversion = 0;
    unsigned offset = 0;
    std::string length;
    llvm::Optional<unsigned> width;
    llvm::Optional<unsigned> precision;
    unsigned width_argument = 0;
    unsigned precision_argument = 0;
    bool allocated = false;
  };
  unsigned required_arguments = 0;
  bool unbounded_string = false;
  std::vector<Conversion> conversions;
};

/// Parses known printf/scanf directives, including positional arguments,
/// star width/precision and scanf assignment suppression. Unknown or malformed
/// directives return unknown rather than a misleading argument count.
llvm::Optional<FormatFacts> parseFormat(llvm::StringRef text, bool scanf);

} // namespace pdg
