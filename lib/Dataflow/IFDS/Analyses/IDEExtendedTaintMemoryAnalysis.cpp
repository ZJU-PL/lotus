#include "Dataflow/IFDS/Analyses/IDEExtendedTaintAnalysis.h"

#include <algorithm>
#include <iterator>

#include <llvm/IR/Instructions.h>

namespace ifds {

namespace {
TaintConfig default_memory_taint_config() {
  TaintConfig config;
  config.sources = {"recv", "read", "fgets", "getline", "getenv"};
  config.sanitizers = {"sanitize", "escape", "strncpy_s", "memset_s"};
  return config;
}

bool is_pointer_value(const llvm::Value *value) {
  return value && value->getType()->isPointerTy();
}
} // namespace

IDEAliasAwareExtendedTaintAnalysis::IDEAliasAwareExtendedTaintAnalysis(
    unsigned path_bound, bool strong_updates)
    : m_model(std::make_shared<const TaintConfig>(default_memory_taint_config())),
      m_factory(nullptr), m_path_bound(std::max(1u, path_bound)),
      m_strong_updates(strong_updates) {}

IDEAliasAwareExtendedTaintAnalysis::IDEAliasAwareExtendedTaintAnalysis(
    const TaintConfig &config, unsigned path_bound, bool strong_updates)
    : m_model(std::make_shared<const TaintConfig>(config)), m_factory(nullptr),
      m_path_bound(std::max(1u, path_bound)), m_strong_updates(strong_updates) {}

const TaintConfig &IDEAliasAwareExtendedTaintAnalysis::model() const {
  auto session = analysis_session();
  return session && session->taint_config() ? *session->taint_config()
                                            : *m_model;
}

void IDEAliasAwareExtendedTaintAnalysis::configure_module(
    const llvm::Module &module) {
  if (!m_factory.data_layout())
    m_factory.set_data_layout(module.getDataLayout());
}

IDEAliasAwareExtendedTaintAnalysis::Fact
IDEAliasAwareExtendedTaintAnalysis::make_fact(const llvm::Value *value) const {
  if (!value)
    return zero_fact();
  return m_factory.create(value, m_path_bound);
}

bool IDEAliasAwareExtendedTaintAnalysis::is_source_function(
    const llvm::Function *callee) const {
  if (!callee)
    return false;
  const auto name = callee->getName().str();
  const auto &taint_model = model();
  if (taint_model.sources.count(name) != 0 ||
      taint_model.argument_source_functions.count(name) != 0)
    return true;
  const auto *config = function_config(callee);
  return config && config->has_source_specs();
}

bool IDEAliasAwareExtendedTaintAnalysis::is_sanitizer_function(
    const llvm::Function *callee) const {
  return callee && model().sanitizers.count(callee->getName().str()) != 0;
}

const FunctionTaintConfig *
IDEAliasAwareExtendedTaintAnalysis::function_config(
    const llvm::Function *callee) const {
  if (!callee)
    return nullptr;
  return model().get_function_config(callee->getName().str());
}

void IDEAliasAwareExtendedTaintAnalysis::add_configured_source_facts(
    const llvm::CallBase *call, const llvm::Function *callee,
    FactSet &out) const {
  if (!call || !callee)
    return;

  const auto *config = function_config(callee);
  if (!config || !config->has_source_specs()) {
    if (is_source_function(callee) && !call->getType()->isVoidTy())
      out.insert(make_fact(call));
    return;
  }

  for (const auto &spec : config->source_specs) {
    if (spec.location == TaintSpec::RET) {
      if (!call->getType()->isVoidTy())
        out.insert(make_fact(call));
      continue;
    }

    if (spec.location == TaintSpec::ARG) {
      if (spec.arg_index < 0 ||
          spec.arg_index >= static_cast<int>(call->arg_size()))
        continue;
      const auto *arg = call->getArgOperand(spec.arg_index);
      if (spec.access_mode == TaintSpec::VALUE || arg->getType()->isPointerTy())
        out.insert(make_fact(arg));
      continue;
    }

    const unsigned first = spec.arg_index < -1
                               ? 0u
                               : static_cast<unsigned>(spec.arg_index + 1);
    for (unsigned i = first; i < call->arg_size(); ++i) {
      const auto *arg = call->getArgOperand(i);
      if (spec.access_mode == TaintSpec::VALUE || arg->getType()->isPointerTy())
        out.insert(make_fact(arg));
    }
  }
}

void IDEAliasAwareExtendedTaintAnalysis::add_configured_pipe_facts(
    const llvm::CallBase *call, const llvm::Function *callee, const Fact &fact,
    FactSet &out) const {
  if (!call || !callee || fact.is_zero())
    return;
  const auto *config = function_config(callee);
  if (!config || !config->has_pipe_specs())
    return;

  auto matches_from = [&](const TaintSpec &spec) {
    auto matches_arg = [&](unsigned index) {
      if (index >= call->arg_size())
        return false;
      const auto *arg = call->getArgOperand(index);
      if (spec.access_mode == TaintSpec::VALUE)
        return fact_matches_value(fact, arg);
      return fact_may_alias_value(fact, arg);
    };

    if (spec.location == TaintSpec::ARG)
      return spec.arg_index >= 0 &&
             matches_arg(static_cast<unsigned>(spec.arg_index));
    if (spec.location == TaintSpec::AFTER_ARG) {
      const unsigned first = spec.arg_index < -1
                                 ? 0u
                                 : static_cast<unsigned>(spec.arg_index + 1);
      for (unsigned i = first; i < call->arg_size(); ++i)
        if (matches_arg(i))
          return true;
      return false;
    }
    return !call->getType()->isVoidTy() && fact_matches_value(fact, call);
  };

  for (const auto &pipe : config->pipe_specs) {
    if (!matches_from(pipe.from))
      continue;

    if (pipe.to.location == TaintSpec::RET) {
      if (!call->getType()->isVoidTy())
        out.insert(make_fact(call));
      continue;
    }

    auto add_arg = [&](unsigned index) {
      if (index < call->arg_size())
        out.insert(make_fact(call->getArgOperand(index)));
    };
    if (pipe.to.location == TaintSpec::ARG) {
      if (pipe.to.arg_index >= 0)
        add_arg(static_cast<unsigned>(pipe.to.arg_index));
    } else {
      const unsigned first = pipe.to.arg_index < -1
                                 ? 0u
                                 : static_cast<unsigned>(pipe.to.arg_index + 1);
      for (unsigned i = first; i < call->arg_size(); ++i)
        add_arg(i);
    }
  }
}

bool IDEAliasAwareExtendedTaintAnalysis::fact_matches_value(
    const Fact &fact, const llvm::Value *value) const {
  if (fact.is_zero() || !value)
    return false;
  return fact.equivalent(make_fact(value));
}

bool IDEAliasAwareExtendedTaintAnalysis::fact_may_alias_value(
    const Fact &fact, const llvm::Value *value) const {
  if (fact.is_zero() || !value)
    return false;
  const Fact other = make_fact(value);
  if (fact.equivalent(other))
    return true;
  if (!is_pointer_value(fact.base()) || !is_pointer_value(other.base()))
    return false;
  auto *aa = alias_analysis();
  return aa && aa->isInitialized() && aa->mayAlias(fact.base(), other.base());
}

bool IDEAliasAwareExtendedTaintAnalysis::is_identity_flow(
    const llvm::Instruction *inst, const llvm::Instruction *,
    const Fact &fact) const {
  if (!inst)
    return true;
  // The zero fact drives source summaries and configured PIPE models.  It
  // must visit every instruction even when the ordinary fact flow is an
  // identity; otherwise sparse execution can skip source calls entirely.
  if (fact.is_zero())
    return false;
  if (llvm::isa<llvm::StoreInst>(inst) || llvm::isa<llvm::LoadInst>(inst) ||
      llvm::isa<llvm::PHINode>(inst) || llvm::isa<llvm::CallBase>(inst))
    return false;
  for (const llvm::Use &operand : inst->operands()) {
    if (fact_matches_value(fact, operand.get()))
      return false;
  }
  return true;
}

bool IDEAliasAwareExtendedTaintAnalysis::is_identity_edge(
    const llvm::Instruction *inst, const llvm::Instruction *succ,
    const Fact &fact) const {
  return is_identity_flow(inst, succ, fact);
}

IDEAliasAwareExtendedTaintAnalysis::FactSet
IDEAliasAwareExtendedTaintAnalysis::normal_flow(const llvm::Instruction *stmt,
                                                const llvm::Instruction *,
                                                const Fact &fact) {
  FactSet out;
  if (!stmt) {
    out.insert(fact);
    return out;
  }
  if (fact.is_zero()) {
    out.insert(fact);
    return out;
  }

  if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(stmt)) {
    const bool writes_fact = fact_may_alias_value(fact, store->getPointerOperand());
    // A strong update removes the old memory fact when a store definitely
    // reaches it. If the stored value is tainted, generate its new location.
    if (!(m_strong_updates && writes_fact &&
          fact.must_alias(make_fact(store->getPointerOperand()),
                          alias_analysis()))) {
      out.insert(fact);
    }
    if (fact_matches_value(fact, store->getValueOperand())) {
      out.insert(make_fact(store->getPointerOperand()));
    }
    return out;
  }

  if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(stmt)) {
    out.insert(fact);
    if (fact_may_alias_value(fact, load->getPointerOperand()))
      out.insert(make_fact(load));
    return out;
  }

  out.insert(fact);
  if (!stmt->getType()->isVoidTy()) {
    for (const llvm::Use &operand : stmt->operands()) {
      if (fact_matches_value(fact, operand.get())) {
        out.insert(make_fact(stmt));
        break;
      }
    }
  }
  return out;
}

IDEAliasAwareExtendedTaintAnalysis::FactSet
IDEAliasAwareExtendedTaintAnalysis::call_flow(const llvm::CallBase *call,
                                              const llvm::Function *callee,
                                              const Fact &fact) {
  FactSet out;
  if (fact.is_zero()) {
    out.insert(fact);
    return out;
  }
  if (!call || !callee || callee->isDeclaration()) {
    out.insert(fact);
    return out;
  }
  if (llvm::isa_and_nonnull<llvm::GlobalValue>(fact.base()))
    out.insert(fact);
  const auto *formal = callee->arg_begin();
  for (unsigned i = 0; formal != callee->arg_end() && i < call->arg_size();
       ++formal, ++i) {
    if (!fact_matches_value(fact, call->getArgOperand(i)))
      continue;
    out.insert(m_factory.with_transfer_to(fact,
                                          make_fact(call->getArgOperand(i)),
                                          &*formal));
  }
  return out;
}

IDEAliasAwareExtendedTaintAnalysis::FactSet
IDEAliasAwareExtendedTaintAnalysis::return_flow(
    const llvm::CallBase *call, const llvm::Instruction *,
    const llvm::Instruction *, const llvm::Function *callee,
    const Fact &exit_fact, const Fact &call_fact) {
  FactSet out;
  if (!call)
    return out;
  if (!call_fact.is_zero())
    out.insert(call_fact);
  if (!callee || callee->isDeclaration())
    return out;

  // Map memory facts rooted at formal pointer parameters back to their actual
  // arguments.  This is the interprocedural part of the XTaint memory domain:
  // a tainted store through `%formal` must remain visible through the caller's
  // `%actual` location after the return.
  auto formal = callee->arg_begin();
  for (unsigned i = 0;
       formal != callee->arg_end() && i < call->arg_size(); ++formal, ++i) {
    const auto *actual = call->getArgOperand(i);
    const auto formal_fact = make_fact(&*formal);
    if (exit_fact.base() == formal || exit_fact.equivalent(formal_fact))
      out.insert(m_factory.with_transfer_from(exit_fact, make_fact(actual)));
  }

  if (exit_fact.base() && llvm::isa<llvm::GlobalValue>(exit_fact.base()))
    out.insert(exit_fact);
  if (call->getType()->isVoidTy())
    return out;
  for (const llvm::BasicBlock &block : *callee) {
    const auto *ret = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator());
    if (ret && ret->getReturnValue() &&
        exit_fact.equivalent(make_fact(ret->getReturnValue()))) {
      out.insert(make_fact(call));
      break;
    }
  }
  return out;
}

IDEAliasAwareExtendedTaintAnalysis::FactSet
IDEAliasAwareExtendedTaintAnalysis::call_to_return_flow(
    const llvm::CallBase *call, const llvm::Instruction *,
    llvm::ArrayRef<const llvm::Function *> callees, const Fact &fact) {
  FactSet out;
  if (!call || fact.is_zero()) {
    out.insert(fact);
    return out;
  }
  bool killed = false;
  if (m_strong_updates) {
    for (const llvm::Function *callee : callees) {
      if (!is_sanitizer_function(callee))
        continue;
      for (const llvm::Use &arg : call->args()) {
        if (fact_may_alias_value(fact, arg.get())) {
          killed = true;
          break;
        }
      }
      if (killed)
        break;
    }
  }
  if (!killed)
    out.insert(fact);
  for (const llvm::Function *callee : callees) {
    if (fact.is_zero())
      add_configured_source_facts(call, callee, out);
    else
      add_configured_pipe_facts(call, callee, fact, out);
  }
  // A sanitizer summary supplies the call fact with a Sanitized edge
  // function.  Do not also create an identity call-to-return fact for the
  // same strong update, or the tainted and sanitized values would immediately
  // join back to Tainted.
  if (call && !call->getType()->isVoidTy() && !killed)
    out.insert(make_fact(call));
  return out;
}

IDEAliasAwareExtendedTaintAnalysis::FactSet
IDEAliasAwareExtendedTaintAnalysis::initial_facts(const llvm::Function *main) {
  FactSet out;
  out.insert(zero_fact());
  if (!main)
    return out;
  for (const llvm::Argument &arg : main->args())
    out.insert(make_fact(&arg));
  return out;
}

IDEAliasAwareExtendedTaintAnalysis::IDEInitialSeeds
IDEAliasAwareExtendedTaintAnalysis::initial_ide_seeds(
    const llvm::Module &module) {
  configure_module(module);
  return this->lift_ifds_initial_seeds(module, bottom_value());
}

IDEAliasAwareExtendedTaintAnalysis::Value
IDEAliasAwareExtendedTaintAnalysis::join(const Value &v1, const Value &v2) const {
  if (v1.kind == Value::Bottom)
    return v2;
  if (v2.kind == Value::Bottom)
    return v1;
  if (v1.kind == v2.kind)
    return v1;
  if (v1.kind == Value::Top || v2.kind == Value::Top)
    return Value::top();
  if (v1.kind == Value::Tainted || v2.kind == Value::Tainted)
    return Value::tainted();
  return Value::top();
}

IDEAliasAwareExtendedTaintAnalysis::EdgeFunction
IDEAliasAwareExtendedTaintAnalysis::normal_edge_function(
    const llvm::Instruction *, const llvm::Instruction *, const Fact &,
    const Fact &) {
  return edge::identity<Value>();
}

IDEAliasAwareExtendedTaintAnalysis::EdgeFunction
IDEAliasAwareExtendedTaintAnalysis::call_edge_function(
    const llvm::CallBase *, const llvm::Function *, const Fact &, const Fact &) {
  return edge::identity<Value>();
}

IDEAliasAwareExtendedTaintAnalysis::EdgeFunction
IDEAliasAwareExtendedTaintAnalysis::return_edge_function(
    const llvm::CallBase *, const llvm::Function *, const llvm::Instruction *,
    const llvm::Instruction *, const Fact &, const Fact &) {
  return edge::identity<Value>();
}

IDEAliasAwareExtendedTaintAnalysis::EdgeFunction
IDEAliasAwareExtendedTaintAnalysis::call_to_return_edge_function(
    const llvm::CallBase *, const llvm::Instruction *,
    llvm::ArrayRef<const llvm::Function *>, const Fact &, const Fact &) {
  return edge::identity<Value>();
}

IDEAliasAwareExtendedTaintAnalysis::FactSet
IDEAliasAwareExtendedTaintAnalysis::summary_flow(
    const llvm::CallBase *call, const llvm::Function *callee, const Fact &fact) {
  FactSet out;
  if (!call || !callee)
    return out;
  if (is_source_function(callee) && fact.is_zero())
    add_configured_source_facts(call, callee, out);
  if (!fact.is_zero())
    add_configured_pipe_facts(call, callee, fact, out);
  if (is_sanitizer_function(callee) && !fact.is_zero()) {
    bool matches = false;
    for (const llvm::Use &arg : call->args())
      matches |= fact_may_alias_value(fact, arg.get());
    if (matches && !call->getType()->isVoidTy())
      out.insert(make_fact(call));
  }
  return out;
}

IDEAliasAwareExtendedTaintAnalysis::EdgeFunction
IDEAliasAwareExtendedTaintAnalysis::summary_edge_function(
    const llvm::CallBase *, const llvm::Function *callee,
    const llvm::Instruction *, const Fact &, const Fact &) {
  if (is_source_function(callee))
    return edge::constant<Value>(Value::tainted());
  if (is_sanitizer_function(callee))
    return edge::constant<Value>(Value::sanitized());
  return edge::identity<Value>();
}

} // namespace ifds
