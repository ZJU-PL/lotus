#include "IR/PDG/Rules/RuleEngine.h"

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
#include <set>
#include <stdexcept>

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

std::string additionalRule(const std::string &id, const Instruction &inst,
                           FunctionFacts &facts, ProgramGraph &graph,
                           std::vector<Node *> &evidence) {
  if (const auto *call = dyn_cast<CallBase>(&inst)) {
    const Function *target = ValueFacts::callee(*call);
    if (!target)
      return "";
    const std::string name = ValueFacts::functionBaseName(*target);
    if ((id == "cpp/power-of-10/use-of-recursion" ||
         id == "cpp/jpl-c/recursion") &&
        facts.recursive(*call))
      return "This call participates in a direct or mutual recursion cycle.";
    if (id == "cpp/wrong-number-format-arguments" ||
        id == "cpp/too-many-format-arguments" ||
        id == "cpp/memory-unsafe-function-scan") {
      auto model = LibraryModels::format(*target);
      if (!model)
        return "";
      const Value *argument = ValueFacts::argument(*call, model->format);
      auto literal = argument ? ValueFacts::constantString(*argument) : None;
      auto format = literal ? parseFormat(*literal, model->scanf) : None;
      if (!format)
        return "";
      if (id == "cpp/memory-unsafe-function-scan")
        return model->scanf && format->unbounded_string
                   ? "Dangerous unbounded scanf string conversion."
                   : "";
      if (model->scanf || call->arg_size() < model->first_argument)
        return "";
      unsigned given = call->arg_size() - model->first_argument;
      if ((id == "cpp/wrong-number-format-arguments" &&
           given < format->required_arguments) ||
          (id == "cpp/too-many-format-arguments" &&
           given > format->required_arguments))
        return "Format expects " + std::to_string(format->required_arguments) +
               " arguments but given " + std::to_string(given) + ".";
    }
    if (id == "cpp/incorrectly-checked-scanf") {
      auto model = LibraryModels::format(*target);
      if (model && model->scanf && facts.onlyZeroChecked(*call) &&
          !ValueFacts::macroDefined(*call->getModule(), "_LINUX_KERNEL_H") &&
          !ValueFacts::macroDefined(*call->getModule(),
                                    "_LINUX_KERNEL_SPRINTF_H_"))
        return "The result of scanf is only checked against 0, but it can also "
               "return EOF.";
    }
    if (id == "cpp/new-free-mismatch" ||
        id == "cpp/new-array-delete-mismatch" ||
        id == "cpp/new-delete-array-mismatch") {
      auto release = LibraryModels::release(*target);
      const Value *pointer = ValueFacts::argument(*call, 0);
      if (release == ReleaseKind::Unknown || !pointer)
        return "";
      auto origin = allocationOrigin(*pointer, inst, facts, evidence, graph);
      bool incompatible =
          (origin == AllocationKind::Malloc && release != ReleaseKind::Free) ||
          ((origin == AllocationKind::New ||
            origin == AllocationKind::NewArray) &&
           release == ReleaseKind::Free);
      if ((id == "cpp/new-free-mismatch" && incompatible) ||
          (id == "cpp/new-array-delete-mismatch" &&
           origin == AllocationKind::NewArray &&
           release == ReleaseKind::Delete) ||
          (id == "cpp/new-delete-array-mismatch" &&
           origin == AllocationKind::New &&
           release == ReleaseKind::DeleteArray))
        return "Allocation and deallocation families do not match.";
    }
    if (id == "cpp/unsafe-strncat" &&
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
              facts.equivalent(*source, *destination))
            return "Potentially unsafe strncat: remaining capacity omits the "
                   "terminator byte.";
        }
      }
    }
    if (id == "cpp/open-call-with-mode-argument") {
      unsigned mode = 0;
      if (createsFile(*call, *target, mode) && call->arg_size() <= mode)
        return "This creates a file without providing a mode argument.";
    }
    if (id == "cpp/world-writable-file-creation" &&
        worldWritable(*call, *target))
      return "This file mode permits writes by other users.";
    if (id == "cpp/insecure-generation-of-filename" &&
        (ValueFacts::hasLibraryName(*target, "tmpnam", true) ||
         ValueFacts::hasLibraryName(*target, "tmpnam_s", true) ||
         ValueFacts::hasLibraryName(*target, "tmpnam_r", true))) {
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
    if (id == "cpp/curl-disabled-ssl" &&
        ValueFacts::hasLibraryName(*target, "curl_easy_setopt", true)) {
      auto option = argumentInteger(*call, 1),
           setting = argumentInteger(*call, 2);
      if (option && setting && (*option == 81 || *option == 64) &&
          *setting == 0)
        return "This call disables TLS certificate or hostname verification.";
    }
    if (id == "cpp/return-value-ignored" &&
        ValueFacts::hasLibraryName(*target, "fgets", true) && call->use_empty())
      return "Result of fgets is ignored; it should always be checked.";
  }
  if (id == "cpp/alloca-in-loop")
    if (const auto *allocation = dyn_cast<AllocaInst>(&inst))
      if (facts.repeatedStackAllocation(*allocation))
        return "Stack allocation is inside a potentially large loop.";
  if (const auto *cmp = dyn_cast<ICmpInst>(&inst)) {
    if (id == "cpp/incomplete-parity-check" && cmp->isEquality()) {
      const auto *rem = dyn_cast<BinaryOperator>(cmp->getOperand(0));
      const auto *right = dyn_cast<ConstantInt>(cmp->getOperand(1));
      if (rem && rem->getOpcode() == Instruction::SRem && right &&
          right->equalsInt(1))
        if (const auto *divisor = dyn_cast<ConstantInt>(rem->getOperand(1)))
          if (divisor->equalsInt(2) &&
              !facts.nonNegative(*rem->getOperand(0), inst))
            return "This parity test fails for negative odd numbers.";
    }
    if (id == "cpp/signed-overflow-check" && !cmp->isEquality()) {
      for (unsigned side = 0; side < 2; ++side) {
        const auto *add = dyn_cast<BinaryOperator>(cmp->getOperand(side));
        if (add && add->getOpcode() == Instruction::Add &&
            add->hasNoSignedWrap() && facts.mayOverflowPositively(*add) &&
            (facts.equivalent(*add->getOperand(0),
                              *cmp->getOperand(1 - side)) ||
             facts.equivalent(*add->getOperand(1), *cmp->getOperand(1 - side))))
          return "Testing for signed overflow after an nsw addition may "
                 "produce undefined results.";
      }
    }
  }
  if (id == "cpp/equality-on-floats")
    if (const auto *cmp = dyn_cast<FCmpInst>(&inst))
      if (cmp->isEquality() && !isa<Constant>(cmp->getOperand(0)) &&
          !isa<Constant>(cmp->getOperand(1)) &&
          !facts.equivalent(*cmp->getOperand(0), *cmp->getOperand(1)))
        return "Equality checks on floating point values can yield unexpected "
               "results.";
  if (id == "cpp/lossy-pointer-cast")
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
  if (id == "cpp/dead-code-function") {
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
  for (const RuleDescriptor &rule : catalog())
    if (selected.empty() || selected.count(rule.id))
      result.rules.push_back(rule);

  auto nodes = nodesInScope(graph_, options.scope);
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

  // Traverse in module order for reproducible findings, independently of node
  // addresses. In a graph-only invocation, discover modules from its nodes.
  std::set<const Module *> modules;
  if (module)
    modules.insert(module);
  else
    for (Node *node : nodes)
      if (node)
        if (const auto *inst = dyn_cast_or_null<Instruction>(node->getValue()))
          modules.insert(inst->getModule());
  for (const Module *input : modules) {
    auto selected = [&](const auto &catalog) {
      return std::any_of(
          catalog.begin(), catalog.end(), [&](const auto &candidate) {
            return std::any_of(result.rules.begin(), result.rules.end(),
                               [&](const RuleDescriptor &rule) {
                                 return rule.id == candidate.id;
                               });
          });
    };
    auto appendFindings = [&](const auto &findings) {
      for (const auto &finding : findings) {
        if (!finding.site ||
            std::none_of(result.rules.begin(), result.rules.end(),
                         [&](const RuleDescriptor &rule) {
                           return rule.id == finding.rule_id;
                         }))
          continue;
        Node *site = graph_.getNode(const_cast<Instruction &>(*finding.site));
        if (!site || !nodes.count(site))
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
          std::any_of(result.rules.begin(), result.rules.end(),
                      [](const RuleDescriptor &rule) {
                        return rule.id == "cpp/non-constant-format";
                      });
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
    for (const Function &function : *input) {
      if (function.isDeclaration())
        continue;
      FunctionFacts facts(const_cast<Function &>(function));
      for (const BasicBlock &block : function)
        for (const Instruction &inst : block) {
          Node *site = graph_.getNode(const_cast<Instruction &>(inst));
          if (!site || !nodes.count(site))
            continue;
          for (const RuleDescriptor &rule : result.rules) {
            std::string message;
            std::vector<Node *> evidence;
            std::vector<TaintOrigin> taint_origins;
            if (const auto *call = dyn_cast<CallBase>(&inst)) {
              const Function *target = ValueFacts::callee(*call);
              if (!target)
                continue;
              if (rule.id == "cpp/dangerous-function-overflow" &&
                  ValueFacts::hasLibraryName(*target, "gets", true) &&
                  target->arg_size() == 1)
                message = "'gets' does not guard against buffer overflow.";
              if (rule.id == "cpp/potentially-dangerous-function")
                for (const char *name :
                     {"gmtime", "localtime", "ctime", "asctime"})
                  if (ValueFacts::hasLibraryName(*target, name))
                    message = std::string("Call to '") + name +
                              "' is potentially dangerous.";
              if (rule.id == "cpp/power-of-10/use-of-jmp")
                for (const char *name :
                     {"setjmp", "longjmp", "sigsetjmp", "siglongjmp"})
                  if (ValueFacts::functionBaseName(*target) == name)
                    message = std::string("The ") + name +
                              " function should not be used.";
              if (rule.id == "cpp/bad-strncpy-size" &&
                  badCopySize(*call, *target, facts))
                message = "Potentially unsafe copy: bound exceeds the "
                          "destination or uses the source length.";
              if (auto domain = taintDomain(rule.id)) {
                bool wrappers = rule.id != "cpp/non-constant-format" &&
                                *domain != TaintDomain::Process;
                for (unsigned index :
                     taint->sinkArguments(*call, *domain, wrappers)) {
                  bool forwarded = wrappers && taint->forwardedParameter(
                                                   *call, index, *domain);
                  if (*domain == TaintDomain::Allocation &&
                      index < call->arg_size() &&
                      facts.boundedAt(*call->getArgOperand(index), *call))
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
                              ValueFacts::functionBaseName(*target) +
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
              }
            }
            if (rule.id == "cpp/return-stack-allocated-memory") {
              const auto *ret = dyn_cast<ReturnInst>(&inst);
              if (!ret || !ret->getReturnValue())
                continue;
              const std::string name =
                  StringRef(ValueFacts::functionBaseName(function)).lower();
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
            }
            if (message.empty())
              message = additionalRule(rule.id, inst, facts, graph_, evidence);
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
