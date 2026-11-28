#include "IR/PDG/Rules/RuleEngine.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"

#include "IR/PDG/Analyses/ApiAnalysis.h"
#include "IR/PDG/Analyses/ArithmeticAnalysis.h"
#include "IR/PDG/Analyses/BoundsAnalysis.h"
#include "IR/PDG/Analyses/CallContractAnalysis.h"
#include "IR/PDG/Analyses/LifetimeAnalysis.h"
#include "IR/PDG/Analyses/ProtocolAnalysis.h"
#include "IR/PDG/Analyses/StateAnalysis.h"
#include "IR/PDG/Core/Graph.h"
#include "IR/PDG/Models/FunctionFacts.h"
#include "IR/PDG/Models/LibraryModels.h"
#include "IR/PDG/Models/ValueFacts.h"
#include "IR/PDG/Query/DataFlowQuery.h"

#include <algorithm>
#include <functional>
#include <set>
#include <stdexcept>
#include <unordered_set>

using namespace llvm;

namespace pdg {


namespace {

Optional<TaintDomain> taintDomain(const std::string &id) {
  if (id == "cpp/wordexp-injection")
    return TaintDomain::Wordexp;
  if (id == "cpp/uncontrolled-process-operation")
    return TaintDomain::Process;
  if (id == "cpp/command-line-injection")
    return TaintDomain::Command;
  if (id == "cpp/sql-injection")
    return TaintDomain::Sql;
  if (id == "cpp/path-injection")
    return TaintDomain::Path;
  if (id == "cpp/uncontrolled-allocation-size")
    return TaintDomain::Allocation;
  if (id == "cpp/tainted-format-string" || id == "cpp/non-constant-format")
    return TaintDomain::Format;
  return None;
}

bool badCopySize(const CallBase &call, const Function &function,
                 FunctionFacts &facts) {
  auto model = LibraryModels::copy(function);
  if (!model)
    return false;
  const Value *destination = ValueFacts::argument(call, 0);
  const Value *source = ValueFacts::argument(call, model->source);
  const Value *size = ValueFacts::argument(call, model->size);
  if (!destination || !source || !size ||
      !destination->getType()->isPointerTy())
    return false;
  if (auto count = ValueFacts::integer(*size)) {
    // A negative signed value or an unusually wide integer is not a proved
    // positive character count. Avoid overflow in byte-count multiplication.
    if (count->isNegative() || count->getActiveBits() > 64)
      return false;
    auto bytes = ValueFacts::objectBytes(*destination);
    if (!bytes || *bytes == 0 || function.arg_empty())
      return false;
    Type *param_type = function.getFunctionType()->getParamType(0);
    if (!param_type->isPointerTy())
      return false;
    Type *element = param_type->getPointerElementType();
    if (!element->isIntegerTy())
      return false;
    uint64_t char_bytes =
        call.getModule()->getDataLayout().getTypeAllocSize(element);
    return char_bytes && count->getZExtValue() > *bytes / char_bytes;
  }
  const Value *length_source = ValueFacts::lengthSource(*size);
  return length_source && source->getType()->isPointerTy() &&
         facts.equivalent(*length_source, *source) &&
         !facts.equivalent(*destination, *source);
}

Optional<uint64_t> argumentInteger(const CallBase &call, unsigned index) {
  const Value *arg = ValueFacts::argument(call, index);
  if (!arg)
    return None;
  auto integer = ValueFacts::integer(*arg);
  if (!integer || integer->getActiveBits() > 64)
    return None;
  return integer->getZExtValue();
}

bool createsFile(const CallBase &call, const Function &target,
                 unsigned &mode_index) {
  unsigned flags_index = 1;
  if (ValueFacts::hasLibraryName(target, "openat", true))
    flags_index = 2;
  else if (!ValueFacts::hasLibraryName(target, "open", true) &&
           !ValueFacts::hasLibraryName(target, "_open", true) &&
           !ValueFacts::hasLibraryName(target, "_wopen", true))
    return false;
  mode_index = flags_index + 1;
  auto flags = argumentInteger(call, flags_index);
  if (!flags)
    return false;
  for (const char *name : {"O_CREAT", "O_TMPFILE"}) {
    auto mask = ValueFacts::macroInteger(*call.getModule(), name);
    if (mask && *mask && (*flags & *mask) == *mask)
      return true;
  }
  return false;
}

Optional<uint64_t> localUmask(const CallBase &site) {
  Optional<uint64_t> mask = 0;
  bool local = false, other_block = false;
  for (const BasicBlock &block : *site.getFunction()) {
    for (const Instruction &inst : block) {
      if (&inst == &site)
        break;
      const auto *call = dyn_cast<CallBase>(&inst);
      const Function *target = call ? ValueFacts::callee(*call) : nullptr;
      if (!target)
        continue;
      const std::string name = ValueFacts::functionBaseName(*target);
      if (name != "umask" && name != "_umask" && name != "_umask_s")
        continue;
      if (&block == site.getParent()) {
        local = true;
        mask = argumentInteger(*call, 0);
      } else
        other_block = true;
    }
  }
  return other_block && !local ? None : mask;
}

bool worldWritable(const CallBase &call, const Function &target) {
  const std::string name = ValueFacts::functionBaseName(target);
  Optional<uint64_t> mode;
  if (name == "chmod" || name == "fchmod" || name == "_chmod" ||
      name == "_wchmod") {
    mode = argumentInteger(call, 1);
    return mode && (*mode & 2);
  }
  if (name == "creat")
    mode = argumentInteger(call, 1);
  else if (name == "fopen" || name == "fsopen" || name == "fopen_s") {
    unsigned index = name == "fopen_s" ? 2 : 1;
    const Value *arg = ValueFacts::argument(call, index);
    auto format = arg ? ValueFacts::constantString(*arg) : None;
    if (!format || format->empty() ||
        ((*format)[0] != 'w' && (*format)[0] != 'a'))
      return false;
    mode = name == "fopen_s" && format->find('u') == std::string::npos ? 0600
                                                                       : 0666;
  } else {
    unsigned index = 0;
    if (!createsFile(call, target, index))
      return false;
    mode = index >= call.arg_size() ? Optional<uint64_t>(~uint64_t(0))
                                    : argumentInteger(call, index);
  }
  auto mask = localUmask(call);
  return mode && mask && (*mode & ~*mask & 2);
}

AllocationKind allocationOrigin(const Value &value, const Instruction &site,
                                FunctionFacts &facts,
                                std::vector<Node *> &evidence,
                                ProgramGraph &graph) {
  if (!value.getType()->isPointerTy())
    return AllocationKind::Unknown;
  SmallVector<const Value *, 8> origins;
  getUnderlyingObjects(&value, origins);
  AllocationKind result = AllocationKind::Unknown;
  for (const Value *origin : origins) {
    const auto *call = dyn_cast<CallBase>(origin);
    const Function *target = call ? ValueFacts::callee(*call) : nullptr;
    if (!target || call->getFunction() != site.getFunction() ||
        !facts.reaches(*call, site))
      return AllocationKind::Unknown;
    auto kind = LibraryModels::allocation(*target);
    if (kind == AllocationKind::Unknown ||
        (result != AllocationKind::Unknown && kind != result))
      return AllocationKind::Unknown;
    result = kind;
    if (Node *node = graph.getNode(const_cast<Value &>(*origin)))
      evidence.push_back(node);
  }
  return result;
}

// Exact direct call-graph SCC computed once per module via iterative Tarjan's
// algorithm. A direct call `caller -> callee` participates in a direct or
// mutual recursion cycle iff `caller` and `callee` belong to the same SCC.
class ModuleCallSCC {
public:
  explicit ModuleCallSCC(const Module &module) {
    llvm::DenseMap<const Function *, llvm::SmallVector<const Function *, 8>>
        adj;
    std::vector<const Function *> funcs;
    for (const Function &fn : module) {
      funcs.push_back(&fn);
      if (fn.isDeclaration())
        continue;
      auto &callees = adj[&fn];
      llvm::SmallPtrSet<const Function *, 8> seen;
      for (const BasicBlock &bb : fn) {
        for (const Instruction &inst : bb) {
          if (const auto *call = dyn_cast<CallBase>(&inst)) {
            if (const Function *callee = ValueFacts::callee(*call)) {
              if (seen.insert(callee).second)
                callees.push_back(callee);
            }
          }
        }
      }
    }
    llvm::DenseMap<const Function *, unsigned> index_map;
    llvm::DenseMap<const Function *, unsigned> lowlink;
    llvm::DenseSet<const Function *> on_stack;
    std::vector<const Function *> tarjan_stack;
    unsigned next_index = 0;
    unsigned next_scc = 0;
    struct Frame {
      const Function *fn;
      size_t edge_idx;
    };
    std::vector<Frame> call_stack;
    for (const Function *root : funcs) {
      if (index_map.count(root))
        continue;
      index_map[root] = lowlink[root] = next_index++;
      tarjan_stack.push_back(root);
      on_stack.insert(root);
      call_stack.push_back({root, 0});
      while (!call_stack.empty()) {
        auto &frame = call_stack.back();
        const Function *v = frame.fn;
        auto adj_it = adj.find(v);
        if (adj_it != adj.end() && frame.edge_idx < adj_it->second.size()) {
          const Function *w = adj_it->second[frame.edge_idx++];
          if (!index_map.count(w)) {
            index_map[w] = lowlink[w] = next_index++;
            tarjan_stack.push_back(w);
            on_stack.insert(w);
            call_stack.push_back({w, 0});
          } else if (on_stack.count(w)) {
            lowlink[v] = std::min(lowlink[v], index_map[w]);
          }
        } else {
          if (lowlink[v] == index_map[v]) {
            unsigned id = next_scc++;
            while (true) {
              const Function *w = tarjan_stack.back();
              tarjan_stack.pop_back();
              on_stack.erase(w);
              scc_id_[w] = id;
              if (w == v)
                break;
            }
          }
          const Function *finished = v;
          call_stack.pop_back();
          if (!call_stack.empty()) {
            const Function *parent = call_stack.back().fn;
            lowlink[parent] = std::min(lowlink[parent], lowlink[finished]);
          }
        }
      }
    }
  }

  bool isRecursiveCall(const Function *caller, const Function *callee) const {
    if (!caller || !callee)
      return false;
    if (caller == callee)
      return true;
    auto it_a = scc_id_.find(caller);
    auto it_b = scc_id_.find(callee);
    return it_a != scc_id_.end() && it_b != scc_id_.end() &&
           it_a->second == it_b->second;
  }

private:
  llvm::DenseMap<const Function *, unsigned> scc_id_;
};

enum class InlineRuleKind {
  None,
  DangerousFunctionOverflow,
  PotentiallyDangerousFunction,
  UseOfJmp,
  BadStrncpySize,
  Taint,
  ReturnStackAllocatedMemory,
  UseOfRecursion,
  JplRecursion,
  WrongNumberFormatArguments,
  TooManyFormatArguments,
  MemoryUnsafeFunctionScan,
  IncorrectlyCheckedScanf,
  NewFreeMismatch,
  NewArrayDeleteMismatch,
  NewDeleteArrayMismatch,
  UnsafeStrncat,
  OpenCallWithModeArgument,
  WorldWritableFileCreation,
  InsecureGenerationOfFilename,
  CurlDisabledSsl,
  ReturnValueIgnored,
  AllocaInLoop,
  IncompleteParityCheck,
  SignedOverflowCheck,
  EqualityOnFloats,
  LossyPointerCast,
  DeadCodeFunction,
};

InlineRuleKind classifyInlineRule(const std::string &id) {
  if (id == "cpp/dangerous-function-overflow")
    return InlineRuleKind::DangerousFunctionOverflow;
  if (id == "cpp/potentially-dangerous-function")
    return InlineRuleKind::PotentiallyDangerousFunction;
  if (id == "cpp/power-of-10/use-of-jmp")
    return InlineRuleKind::UseOfJmp;
  if (id == "cpp/bad-strncpy-size")
    return InlineRuleKind::BadStrncpySize;
  if (taintDomain(id))
    return InlineRuleKind::Taint;
  if (id == "cpp/return-stack-allocated-memory")
    return InlineRuleKind::ReturnStackAllocatedMemory;
  if (id == "cpp/power-of-10/use-of-recursion")
    return InlineRuleKind::UseOfRecursion;
  if (id == "cpp/jpl-c/recursion")
    return InlineRuleKind::JplRecursion;
  if (id == "cpp/wrong-number-format-arguments")
    return InlineRuleKind::WrongNumberFormatArguments;
  if (id == "cpp/too-many-format-arguments")
    return InlineRuleKind::TooManyFormatArguments;
  if (id == "cpp/memory-unsafe-function-scan")
    return InlineRuleKind::MemoryUnsafeFunctionScan;
  if (id == "cpp/incorrectly-checked-scanf")
    return InlineRuleKind::IncorrectlyCheckedScanf;
  if (id == "cpp/new-free-mismatch")
    return InlineRuleKind::NewFreeMismatch;
  if (id == "cpp/new-array-delete-mismatch")
    return InlineRuleKind::NewArrayDeleteMismatch;
  if (id == "cpp/new-delete-array-mismatch")
    return InlineRuleKind::NewDeleteArrayMismatch;
  if (id == "cpp/unsafe-strncat")
    return InlineRuleKind::UnsafeStrncat;
  if (id == "cpp/open-call-with-mode-argument")
    return InlineRuleKind::OpenCallWithModeArgument;
  if (id == "cpp/world-writable-file-creation")
    return InlineRuleKind::WorldWritableFileCreation;
  if (id == "cpp/insecure-generation-of-filename")
    return InlineRuleKind::InsecureGenerationOfFilename;
  if (id == "cpp/curl-disabled-ssl")
    return InlineRuleKind::CurlDisabledSsl;
  if (id == "cpp/return-value-ignored")
    return InlineRuleKind::ReturnValueIgnored;
  if (id == "cpp/alloca-in-loop")
    return InlineRuleKind::AllocaInLoop;
  if (id == "cpp/incomplete-parity-check")
    return InlineRuleKind::IncompleteParityCheck;
  if (id == "cpp/signed-overflow-check")
    return InlineRuleKind::SignedOverflowCheck;
  if (id == "cpp/equality-on-floats")
    return InlineRuleKind::EqualityOnFloats;
  if (id == "cpp/lossy-pointer-cast")
    return InlineRuleKind::LossyPointerCast;
  if (id == "cpp/dead-code-function")
    return InlineRuleKind::DeadCodeFunction;
  return InlineRuleKind::None;
}

std::string additionalRule(InlineRuleKind kind, const Instruction &inst,
                           const CallBase *call, const Function *target,
                           const std::function<FunctionFacts &()> &getFacts,
                           const ModuleCallSCC *call_scc, ProgramGraph &graph,
                           std::vector<Node *> &evidence) {
  if (call) {
    if (!target)
      return "";
    if (kind == InlineRuleKind::UseOfRecursion ||
        kind == InlineRuleKind::JplRecursion) {
      bool rec = call_scc ? call_scc->isRecursiveCall(call->getFunction(),
                                                      target)
                          : getFacts().recursive(*call);
      if (rec)
        return "This call participates in a direct or mutual recursion cycle.";
      return "";
    }
    if (kind == InlineRuleKind::WrongNumberFormatArguments ||
        kind == InlineRuleKind::TooManyFormatArguments ||
        kind == InlineRuleKind::MemoryUnsafeFunctionScan) {
      auto model = LibraryModels::format(*target);
      if (!model)
        return "";
      const Value *argument = ValueFacts::argument(*call, model->format);
      auto literal = argument ? ValueFacts::constantString(*argument) : None;
      auto format = literal ? parseFormat(*literal, model->scanf) : None;
      if (!format)
        return "";
      if (kind == InlineRuleKind::MemoryUnsafeFunctionScan)
        return model->scanf && format->unbounded_string
                   ? "Dangerous unbounded scanf string conversion."
                   : "";
      if (model->scanf || call->arg_size() < model->first_argument)
        return "";
      unsigned given = call->arg_size() - model->first_argument;
      if ((kind == InlineRuleKind::WrongNumberFormatArguments &&
           given < format->required_arguments) ||
          (kind == InlineRuleKind::TooManyFormatArguments &&
           given > format->required_arguments))
        return "Format expects " + std::to_string(format->required_arguments) +
               " arguments but given " + std::to_string(given) + ".";
      return "";
    }
    if (kind == InlineRuleKind::IncorrectlyCheckedScanf) {
      auto model = LibraryModels::format(*target);
      if (model && model->scanf && getFacts().onlyZeroChecked(*call) &&
          !ValueFacts::macroDefined(*call->getModule(), "_LINUX_KERNEL_H") &&
          !ValueFacts::macroDefined(*call->getModule(),
                                    "_LINUX_KERNEL_SPRINTF_H_"))
        return "The result of scanf is only checked against 0, but it can also "
               "return EOF.";
      return "";
    }
    if (kind == InlineRuleKind::NewFreeMismatch ||
        kind == InlineRuleKind::NewArrayDeleteMismatch ||
        kind == InlineRuleKind::NewDeleteArrayMismatch) {
      auto release = LibraryModels::release(*target);
      const Value *pointer = ValueFacts::argument(*call, 0);
      if (release == ReleaseKind::Unknown || !pointer)
        return "";
      auto origin =
          allocationOrigin(*pointer, inst, getFacts(), evidence, graph);
      bool incompatible =
          (origin == AllocationKind::Malloc && release != ReleaseKind::Free) ||
          ((origin == AllocationKind::New ||
            origin == AllocationKind::NewArray) &&
           release == ReleaseKind::Free);
      if ((kind == InlineRuleKind::NewFreeMismatch && incompatible) ||
          (kind == InlineRuleKind::NewArrayDeleteMismatch &&
           origin == AllocationKind::NewArray &&
           release == ReleaseKind::Delete) ||
          (kind == InlineRuleKind::NewDeleteArrayMismatch &&
           origin == AllocationKind::New &&
           release == ReleaseKind::DeleteArray))
        return "Allocation and deallocation families do not match.";
      return "";
    }
    if (kind == InlineRuleKind::UnsafeStrncat &&
        (ValueFacts::hasLibraryName(*target, "strncat", true, true) ||
         ValueFacts::hasLibraryName(*target, "wcsncat", true, true))) {
      const Value *destination = ValueFacts::argument(*call, 0),
                  *size = ValueFacts::argument(*call, 2);
      const auto *sub = dyn_cast_or_null<BinaryOperator>(size);
      if (!destination || !sub || sub->getOpcode() != Instruction::Sub)
        return "";
      auto capacity = ValueFacts::objectBytes(*destination);
      auto bound = ValueFacts::integer(*sub->getOperand(0));
      const auto *length = dyn_cast<CallBase>(sub->getOperand(1));
      const Value *source =
          length ? ValueFacts::lengthSource(*length) : nullptr;
      if (capacity && bound && source && bound->getActiveBits() <= 64 &&
          !bound->isNegative() && target->arg_size() > 0 &&
          target->getFunctionType()->getParamType(0)->isPointerTy()) {
        Type *element =
            target->getFunctionType()->getParamType(0)->getPointerElementType();
        if (element->isIntegerTy()) {
          uint64_t bytes =
              call->getModule()->getDataLayout().getTypeAllocSize(element);
          if (bytes && bound->getZExtValue() == *capacity / bytes &&
              getFacts().equivalent(*source, *destination))
            return "Potentially unsafe strncat: remaining capacity omits the "
                   "terminator byte.";
        }
      }
      return "";
    }
    if (kind == InlineRuleKind::OpenCallWithModeArgument) {
      unsigned mode = 0;
      if (createsFile(*call, *target, mode) && call->arg_size() <= mode)
        return "This creates a file without providing a mode argument.";
      return "";
    }
    if (kind == InlineRuleKind::WorldWritableFileCreation &&
        worldWritable(*call, *target))
      return "This file mode permits writes by other users.";
    if (kind == InlineRuleKind::InsecureGenerationOfFilename &&
        (ValueFacts::hasLibraryName(*target, "tmpnam", true) ||
         ValueFacts::hasLibraryName(*target, "tmpnam_s", true) ||
         ValueFacts::hasLibraryName(*target, "tmpnam_r", true))) {
      FunctionFacts &facts = getFacts();
      for (const BasicBlock &block : *call->getFunction())
        for (const Instruction &other : block)
          if (const auto *next = dyn_cast<CallBase>(&other))
            if (const Function *next_target = ValueFacts::callee(*next))
              for (const char *safe :
                   {"mktemp", "mkstemp", "mkstemps", "mkdtemp"})
                if (ValueFacts::hasLibraryName(*next_target, safe, true) &&
                    facts.blocksConnected(*call->getParent(), block))
                  return "";
      return "Generating a temporary filename does not atomically create the "
             "file.";
    }
    if (kind == InlineRuleKind::CurlDisabledSsl &&
        ValueFacts::hasLibraryName(*target, "curl_easy_setopt", true)) {
      auto option = argumentInteger(*call, 1),
           setting = argumentInteger(*call, 2);
      if (option && setting && (*option == 81 || *option == 64) &&
          *setting == 0)
        return "This call disables TLS certificate or hostname verification.";
      return "";
    }
    if (kind == InlineRuleKind::ReturnValueIgnored &&
        ValueFacts::hasLibraryName(*target, "fgets", true) && call->use_empty())
      return "Result of fgets is ignored; it should always be checked.";
    return "";
  }
  if (kind == InlineRuleKind::AllocaInLoop) {
    if (const auto *allocation = dyn_cast<AllocaInst>(&inst))
      if (getFacts().repeatedStackAllocation(*allocation))
        return "Stack allocation is inside a potentially large loop.";
    return "";
  }
  if (const auto *cmp = dyn_cast<ICmpInst>(&inst)) {
    if (kind == InlineRuleKind::IncompleteParityCheck && cmp->isEquality()) {
      const auto *rem = dyn_cast<BinaryOperator>(cmp->getOperand(0));
      const auto *right = dyn_cast<ConstantInt>(cmp->getOperand(1));
      if (rem && rem->getOpcode() == Instruction::SRem && right &&
          right->equalsInt(1))
        if (const auto *divisor = dyn_cast<ConstantInt>(rem->getOperand(1)))
          if (divisor->equalsInt(2) &&
              !getFacts().nonNegative(*rem->getOperand(0), inst))
            return "This parity test fails for negative odd numbers.";
      return "";
    }
    if (kind == InlineRuleKind::SignedOverflowCheck && !cmp->isEquality()) {
      for (unsigned side = 0; side < 2; ++side) {
        const auto *add = dyn_cast<BinaryOperator>(cmp->getOperand(side));
        if (add && add->getOpcode() == Instruction::Add &&
            add->hasNoSignedWrap()) {
          FunctionFacts &facts = getFacts();
          if (facts.mayOverflowPositively(*add) &&
              (facts.equivalent(*add->getOperand(0),
                                *cmp->getOperand(1 - side)) ||
               facts.equivalent(*add->getOperand(1),
                                *cmp->getOperand(1 - side))))
            return "Testing for signed overflow after an nsw addition may "
                   "produce undefined results.";
        }
      }
      return "";
    }
  }
  if (kind == InlineRuleKind::EqualityOnFloats) {
    if (const auto *cmp = dyn_cast<FCmpInst>(&inst))
      if (cmp->isEquality() && !isa<Constant>(cmp->getOperand(0)) &&
          !isa<Constant>(cmp->getOperand(1)) &&
          !getFacts().equivalent(*cmp->getOperand(0), *cmp->getOperand(1)))
        return "Equality checks on floating point values can yield unexpected "
               "results.";
    return "";
  }
  if (kind == InlineRuleKind::LossyPointerCast) {
    if (const auto *cast = dyn_cast<PtrToIntInst>(&inst)) {
      if (!cast->getType()->isIntegerTy())
        return "";
      unsigned bits = cast->getType()->getIntegerBitWidth();
      if (bits != 1 &&
          bits < cast->getModule()->getDataLayout().getPointerTypeSizeInBits(
                     cast->getPointerOperand()->getType())) {
        for (const User *user : cast->users())
          if (const auto *op = dyn_cast<BinaryOperator>(user))
            if (op->getOpcode() == Instruction::And)
              return "";
        return "Pointer is converted to an integer narrower than the target "
               "pointer width.";
      }
    }
    return "";
  }
  if (kind == InlineRuleKind::DeadCodeFunction) {
    const Function *function = inst.getFunction();
    if (&inst == &function->getEntryBlock().front() &&
        function->hasLocalLinkage() && function->use_empty()) {
      const std::string name = ValueFacts::functionBaseName(*function);
      if (!StringRef(name).startswith("operator") &&
          !StringRef(function->getName()).contains("C1") &&
          !StringRef(function->getName()).contains("C2") &&
          !StringRef(function->getName()).contains("D1") &&
          !StringRef(function->getName()).contains("D2"))
        return "Dead code: this local function is never referenced.";
    }
    return "";
  }
  return "";
}

} // namespace

RuleEngineResult RuleEngine::analyze(const std::vector<std::string> &rule_ids,
                                   const PDGCriteria &criteria,
                                   const PDGQueryOptions &options,
                                   const Module *module,
                                   const TaintPolicy &taint_policy,
                                   const RuleEnginePolicy &rule_policy) const {
  RuleEngineResult result;
  std::set<std::string> selected(rule_ids.begin(), rule_ids.end());
  for (const auto &id : selected)
    if (std::none_of(catalog().begin(), catalog().end(),
                     [&](const RuleDescriptor &rule) { return rule.id == id; }))
      throw std::invalid_argument("Unknown PDG rule: " + id);
  std::unordered_set<std::string> selected_rule_ids;
  for (const RuleDescriptor &rule : catalog())
    if (selected.empty() || selected.count(rule.id)) {
      result.rules.push_back(rule);
      selected_rule_ids.insert(rule.id);
    }

  const bool use_whole_graph =
      criteria.empty() && options.scope.kind == PDGQueryScope::Kind::WholeGraph;
  PDGQueryResult::NodeSet nodes;
  if (!use_whole_graph) {
    nodes = nodesInScope(graph_, options.scope);
    if (!criteria.empty()) {
      auto resolved =
          PDGCriteriaResolver(graph_).resolve(criteria, options, module);
      result.diagnostics = resolved.diagnostics;
      PDGQueryResult::NodeSet intersection;
      std::set_intersection(nodes.begin(), nodes.end(), resolved.nodes.begin(),
                            resolved.nodes.end(),
                            std::inserter(intersection, intersection.end()));
      nodes = std::move(intersection);
    }
  }
  auto inScope = [&](Node *site) -> bool {
    if (!site)
      return false;
    return use_whole_graph || nodes.count(site) != 0;
  };

  // Traverse in module order for reproducible findings, independently of node
  // addresses. In a graph-only invocation, discover modules from its nodes.
  std::set<const Module *> modules;
  if (module)
    modules.insert(module);
  else if (use_whole_graph) {
    for (auto it = graph_.begin(); it != graph_.end(); ++it)
      if (Node *node = *it)
        if (const auto *inst = dyn_cast_or_null<Instruction>(node->getValue()))
          modules.insert(inst->getModule());
  } else {
    for (Node *node : nodes)
      if (node)
        if (const auto *inst = dyn_cast_or_null<Instruction>(node->getValue()))
          modules.insert(inst->getModule());
  }

  struct InlineRuleEntry {
    const RuleDescriptor *rule = nullptr;
    InlineRuleKind kind = InlineRuleKind::None;
    Optional<TaintDomain> taint_domain;
  };
  std::vector<InlineRuleEntry> all_inline_rules;
  std::vector<InlineRuleEntry> call_rules;
  std::vector<InlineRuleEntry> return_rules;
  std::vector<InlineRuleEntry> alloca_rules;
  std::vector<InlineRuleEntry> icmp_rules;
  std::vector<InlineRuleEntry> fcmp_rules;
  std::vector<InlineRuleEntry> ptrtoint_rules;
  bool has_dead_code_rule = false;
  bool needs_call_scc = false;
  for (const RuleDescriptor &rule : result.rules) {
    InlineRuleKind kind = classifyInlineRule(rule.id);
    if (kind == InlineRuleKind::None)
      continue;
    InlineRuleEntry entry{&rule, kind,
                          kind == InlineRuleKind::Taint ? taintDomain(rule.id)
                                                        : Optional<TaintDomain>()};
    all_inline_rules.push_back(entry);
    switch (kind) {
    case InlineRuleKind::ReturnStackAllocatedMemory:
      return_rules.push_back(entry);
      break;
    case InlineRuleKind::AllocaInLoop:
      alloca_rules.push_back(entry);
      break;
    case InlineRuleKind::IncompleteParityCheck:
    case InlineRuleKind::SignedOverflowCheck:
      icmp_rules.push_back(entry);
      break;
    case InlineRuleKind::EqualityOnFloats:
      fcmp_rules.push_back(entry);
      break;
    case InlineRuleKind::LossyPointerCast:
      ptrtoint_rules.push_back(entry);
      break;
    case InlineRuleKind::DeadCodeFunction:
      has_dead_code_rule = true;
      break;
    case InlineRuleKind::UseOfRecursion:
    case InlineRuleKind::JplRecursion:
      needs_call_scc = true;
      call_rules.push_back(entry);
      break;
    default:
      call_rules.push_back(entry);
      break;
    }
  }

  for (const Module *input : modules) {
    auto selected = [&](const auto &catalog) {
      return std::any_of(catalog.begin(), catalog.end(),
                         [&](const auto &candidate) {
                           return selected_rule_ids.count(candidate.id) != 0;
                         });
    };
    auto appendFindings = [&](const auto &findings) {
      for (const auto &finding : findings) {
        if (!finding.site || !selected_rule_ids.count(finding.rule_id))
          continue;
        Node *site = graph_.getNode(const_cast<Instruction &>(*finding.site));
        if (!inScope(site))
          continue;
        std::vector<Node *> evidence;
        for (const Instruction *instruction : finding.evidence)
          if (instruction &&
              graph_.hasNode(const_cast<Instruction &>(*instruction)))
            evidence.push_back(
                graph_.getNode(const_cast<Instruction &>(*instruction)));
        result.findings.push_back(
            {finding.rule_id, finding.message, site, evidence, {}});
      }
    };
    Optional<TaintFlowResult> taint;
    if (std::any_of(result.rules.begin(), result.rules.end(),
                    [](const RuleDescriptor &rule) {
                      return bool(taintDomain(rule.id)) ||
                             BoundsAnalysis::requiresTaint(rule.id) ||
                             ArithmeticAnalysis::requiresTaint(rule.id);
                    })) {
      TaintPolicy policy = taint_policy;
      policy.include_nonconstant_sources |=
          selected_rule_ids.count("cpp/non-constant-format") != 0;
      if (options.limits.max_states)
        policy.max_steps = options.limits.max_states;
      taint = TaintAnalysis(graph_).analyze(*input, policy);
      result.diagnostics.explored_states += taint->diagnostics.explored_states;
      result.diagnostics.state_limit_hit |= taint->diagnostics.state_limit_hit;
      result.diagnostics.summary_cache_hits +=
          taint->diagnostics.summary_cache_hits;
      result.diagnostics.summary_cache_misses +=
          taint->diagnostics.summary_cache_misses;
      result.diagnostics.notes.insert(result.diagnostics.notes.end(),
                                      taint->diagnostics.notes.begin(),
                                      taint->diagnostics.notes.end());
    }
    if (selected(BoundsAnalysis::catalog()))
      appendFindings(
          BoundsAnalysis().analyze(*input, taint ? &*taint : nullptr).findings);
    if (selected(LifetimeAnalysis::catalog())) {
      size_t lifetime_budget = options.limits.max_states;
      if (rule_policy.lifetime_states_per_object)
        lifetime_budget = lifetime_budget
                              ? std::min(lifetime_budget,
                                         rule_policy.lifetime_states_per_object)
                              : rule_policy.lifetime_states_per_object;
      auto lifetime = LifetimeAnalysis().analyze(*input, lifetime_budget);
      appendFindings(lifetime.findings);
      result.diagnostics.explored_states += lifetime.explored_states;
      result.diagnostics.summary_cache_hits += lifetime.summary_cache_hits;
      result.diagnostics.summary_cache_misses += lifetime.summary_cache_misses;
      if (lifetime.matched_calls)
        result.diagnostics.notes.push_back(
            "Lifetime analysis matched " +
            std::to_string(lifetime.matched_calls) +
            " direct call(s) with callee resource effects.");
      if (lifetime.incomplete_objects) {
        result.diagnostics.state_limit_hit |= lifetime.state_limit_hit;
        result.diagnostics.notes.push_back(
            "Lifetime analysis incomplete for " +
            std::to_string(lifetime.incomplete_objects) +
            " object(s); leak conclusions were withheld.");
      }
    }
    if (selected(StateAnalysis::catalog())) {
      std::vector<std::string> ids;
      for (const auto &rule : result.rules)
        if (std::any_of(StateAnalysis::catalog().begin(),
                        StateAnalysis::catalog().end(),
                        [&](const auto &state_rule) {
                          return state_rule.id == rule.id;
                        }))
          ids.push_back(rule.id);
      auto state = StateAnalysis().analyze(*input, ids);
      appendFindings(state.findings);
      if (state.convergence_limit_hit) {
        result.diagnostics.state_limit_hit = true;
        result.diagnostics.notes.push_back(
            "State-query convergence limit reached; initialization/null "
            "results are incomplete.");
      }
    }
    if (selected(ArithmeticAnalysis::catalog()))
      appendFindings(ArithmeticAnalysis()
                         .analyze(*input, taint ? &*taint : nullptr)
                         .findings);
    if (selected(ApiAnalysis::catalog()))
      appendFindings(ApiAnalysis().analyze(*input).findings);
    if (selected(CallContractAnalysis::catalog()))
      appendFindings(CallContractAnalysis().analyze(*input).findings);
    if (selected(ProtocolAnalysis::catalog())) {
      size_t budget = rule_policy.protocol_states_per_search;
      if (options.limits.max_states)
        budget = budget ? std::min(budget, options.limits.max_states)
                        : options.limits.max_states;
      auto protocols = ProtocolAnalysis().analyze(*input, budget);
      appendFindings(protocols.findings);
      result.diagnostics.state_limit_hit |= protocols.incomplete;
      if (protocols.incomplete)
        result.diagnostics.notes.push_back(
            "Protocol exploration incomplete: state limit reached.");
    }
    if (all_inline_rules.empty())
      continue;
    Optional<ModuleCallSCC> call_scc;
    if (needs_call_scc)
      call_scc.emplace(*input);
    llvm::DenseMap<const Function *, std::string> base_name_cache;
    auto baseName = [&](const Function &fn) -> const std::string & {
      auto it = base_name_cache.find(&fn);
      if (it != base_name_cache.end())
        return it->second;
      return base_name_cache
          .try_emplace(&fn, ValueFacts::functionBaseName(fn))
          .first->second;
    };
    for (const Function &function : *input) {
      if (function.isDeclaration())
        continue;
      Optional<FunctionFacts> facts_opt;
      auto getFacts = [&]() -> FunctionFacts & {
        if (!facts_opt)
          facts_opt.emplace(const_cast<Function &>(function));
        return *facts_opt;
      };
      const Instruction *entry_inst = &function.getEntryBlock().front();
      for (const BasicBlock &block : function)
        for (const Instruction &inst : block) {
          const bool is_entry = (&inst == entry_inst);
          const auto *call = dyn_cast<CallBase>(&inst);
          const auto *ret = call ? nullptr : dyn_cast<ReturnInst>(&inst);
          const std::vector<InlineRuleEntry> *rules_to_check = nullptr;
          if (is_entry && has_dead_code_rule) {
            rules_to_check = &all_inline_rules;
          } else if (call) {
            if (!call_rules.empty())
              rules_to_check = &call_rules;
          } else if (ret) {
            if (!return_rules.empty())
              rules_to_check = &return_rules;
          } else if (isa<AllocaInst>(&inst)) {
            if (!alloca_rules.empty())
              rules_to_check = &alloca_rules;
          } else if (isa<ICmpInst>(&inst)) {
            if (!icmp_rules.empty())
              rules_to_check = &icmp_rules;
          } else if (isa<FCmpInst>(&inst)) {
            if (!fcmp_rules.empty())
              rules_to_check = &fcmp_rules;
          } else if (isa<PtrToIntInst>(&inst)) {
            if (!ptrtoint_rules.empty())
              rules_to_check = &ptrtoint_rules;
          }
          if (!rules_to_check)
            continue;
          Node *site = graph_.getNode(const_cast<Instruction &>(inst));
          if (!inScope(site))
            continue;
          const Function *target = call ? ValueFacts::callee(*call) : nullptr;
          if (call && !target && !(is_entry && has_dead_code_rule))
            continue;
          for (const InlineRuleEntry &entry : *rules_to_check) {
            const RuleDescriptor &rule = *entry.rule;
            std::string message;
            std::vector<Node *> evidence;
            std::vector<TaintOrigin> taint_origins;
            if (call) {
              if (!target) {
                // Only dead-code-function can fire on an indirect entry call.
                if (entry.kind == InlineRuleKind::DeadCodeFunction)
                  message = additionalRule(entry.kind, inst, nullptr, nullptr,
                                           getFacts,
                                           call_scc ? &*call_scc : nullptr,
                                           graph_, evidence);
                if (!message.empty())
                  result.findings.push_back(
                      {rule.id, message, site, evidence, taint_origins});
                continue;
              }
              if (entry.kind == InlineRuleKind::DangerousFunctionOverflow &&
                  ValueFacts::hasLibraryName(*target, "gets", true) &&
                  target->arg_size() == 1)
                message = "'gets' does not guard against buffer overflow.";
              else if (entry.kind ==
                       InlineRuleKind::PotentiallyDangerousFunction) {
                for (const char *name :
                     {"gmtime", "localtime", "ctime", "asctime"})
                  if (ValueFacts::hasLibraryName(*target, name))
                    message = std::string("Call to '") + name +
                              "' is potentially dangerous.";
              } else if (entry.kind == InlineRuleKind::UseOfJmp) {
                const std::string &fn_base = baseName(*target);
                for (const char *name :
                     {"setjmp", "longjmp", "sigsetjmp", "siglongjmp"})
                  if (fn_base == name)
                    message = std::string("The ") + name +
                              " function should not be used.";
              } else if (entry.kind == InlineRuleKind::BadStrncpySize &&
                         badCopySize(*call, *target, getFacts()))
                message = "Potentially unsafe copy: bound exceeds the "
                          "destination or uses the source length.";
              else if (entry.taint_domain) {
                const auto domain = entry.taint_domain;
                bool wrappers = rule.id != "cpp/non-constant-format" &&
                                *domain != TaintDomain::Process;
                for (unsigned index :
                     taint->sinkArguments(*call, *domain, wrappers)) {
                  bool forwarded = wrappers && taint->forwardedParameter(
                                                   *call, index, *domain);
                  if (*domain == TaintDomain::Allocation &&
                      index < call->arg_size() &&
                      getFacts().boundedAt(*call->getArgOperand(index), *call))
                    continue;
                  TaintChannel channel = *domain == TaintDomain::Allocation
                                             ? TaintChannel::Value
                                             : TaintChannel::Memory;
                  for (const auto &origin :
                       taint->origins(*call, index, channel)) {
                    // A wrapper may also introduce fresh input internally.
                    // Only caller-origin facts are represented by its outer
                    // role; local sources still need the actual sink report.
                    const Function *source_function = nullptr;
                    if (const auto *source =
                            dyn_cast_or_null<Instruction>(origin.source))
                      source_function = source->getFunction();
                    else if (const auto *source =
                                 dyn_cast_or_null<Argument>(origin.source))
                      source_function = source->getParent();
                    if (forwarded && source_function != call->getFunction())
                      continue;
                    if (*domain == TaintDomain::Allocation &&
                        origin.allocation_bounded)
                      continue;
                    if (origin.nonconstant_only &&
                        rule.id != "cpp/non-constant-format")
                      continue;
                    if (*domain == TaintDomain::Command &&
                        !origin.concatenation)
                      continue;
                    if (*domain == TaintDomain::Sql && origin.sql_sanitized)
                      continue;
                    auto annotated = origin;
                    annotated.sink_argument = index;
                    taint_origins.push_back(annotated);
                    if (origin.source &&
                        graph_.hasNode(const_cast<Value &>(*origin.source)))
                      evidence.push_back(
                          graph_.getNode(const_cast<Value &>(*origin.source)));
                    if (origin.concatenation &&
                        graph_.hasNode(
                            const_cast<Instruction &>(*origin.concatenation)))
                      evidence.push_back(graph_.getNode(
                          const_cast<Instruction &>(*origin.concatenation)));
                  }
                  if (std::any_of(taint_origins.begin(), taint_origins.end(),
                                  [&](const TaintOrigin &origin) {
                                    return origin.sink_argument == index;
                                  }))
                    message = std::string(
                                  *domain == TaintDomain::Allocation
                                      ? "PDG scalar flow reaches argument "
                                      : "PDG content flow reaches argument ") +
                              std::to_string(index) + " of " +
                              baseName(*target) +
                              (rule.id == "cpp/non-constant-format"
                                   ? " from a nonliteral source."
                                   : " from user input.");
                }
                if (*domain == TaintDomain::Allocation &&
                    !taint_origins.empty())
                  for (const auto &controller :
                       DataFlowQuery(graph_).immediateControllers(*site))
                    if (controller.predicate)
                      evidence.push_back(controller.predicate);
              } else if (entry.kind == InlineRuleKind::DeadCodeFunction) {
                message = additionalRule(entry.kind, inst, nullptr, nullptr,
                                         getFacts,
                                         call_scc ? &*call_scc : nullptr,
                                         graph_, evidence);
              } else {
                message = additionalRule(entry.kind, inst, call, target,
                                         getFacts,
                                         call_scc ? &*call_scc : nullptr,
                                         graph_, evidence);
              }
            } else if (entry.kind ==
                       InlineRuleKind::ReturnStackAllocatedMemory) {
              if (!ret || !ret->getReturnValue())
                continue;
              const std::string name = StringRef(baseName(function)).lower();
              if (name.find("stack") == std::string::npos &&
                  name.find("sp") == std::string::npos) {
                auto origins = ValueFacts::localStackOrigins(
                    *ret->getReturnValue(), function);
                if (!origins.empty()) {
                  message =
                      "May return stack-allocated memory from this function.";
                  for (const Value *origin : origins)
                    if (Node *node =
                            graph_.getNode(const_cast<Value &>(*origin)))
                      evidence.push_back(node);
                }
              }
            } else {
              message = additionalRule(entry.kind, inst, nullptr, nullptr,
                                       getFacts,
                                       call_scc ? &*call_scc : nullptr, graph_,
                                       evidence);
            }
            if (!message.empty())
              result.findings.push_back(
                  {rule.id, message, site, evidence, taint_origins});
          }
        }
    }
  }
  result.diagnostics.notes.push_back(
      "Coverage is recorded per rule; absence of findings does not establish "
      "safety outside that coverage.");
  return result;
}

} // namespace pdg
