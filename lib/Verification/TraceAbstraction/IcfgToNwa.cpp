#include "Verification/TraceAbstraction/IcfgToNwa.h"

#include "IR/ICFG/ICFGBuilder.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

#include <llvm/IR/IntrinsicInst.h>

namespace lotus::verification::traceabstraction {
namespace {
bool isError(const llvm::Instruction *instruction) {
  auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(instruction);
  if (!call)
    return false;
  auto *f = llvm::dyn_cast<llvm::Function>(
      call->getCalledOperand()->stripPointerCasts());
  return f && (f->getName() == "__assert_fail" || f->getName() == "abort" ||
               f->getName() == "__VERIFIER_error");
}

ProgramAutomaton buildGraph(const ICFG &icfg, const llvm::Function *entry,
                            const ICFGNode *errorNode,
                            const llvm::Instruction *targetError,
                            nwa::Context context) {
  ProgramAutomaton result;
  nwa::Alphabet alphabet;
  std::vector<nwa::Transition> transitions;
  std::vector<bool> initial, final;
  auto state = [&](bool start = false, bool error = false) {
    auto id = static_cast<nwa::State>(initial.size());
    if (id >= context->maxStates)
      throw nwa::ResourceLimit();
    initial.push_back(start);
    final.push_back(error);
    return id;
  };
  using Key = std::tuple<const llvm::Instruction *, const llvm::BasicBlock *,
                         const llvm::Function *, nwa::LetterKind, bool, bool>;
  std::map<Key, nwa::Letter> letters;
  auto edge = [&](nwa::State from, nwa::State to, Statement s,
                  nwa::State hierarchy = nwa::Bottom) {
    Key key{s.instruction, s.successor, s.callee,
            s.kind,        s.error,     s.initialize};
    auto it = letters.find(key);
    nwa::Letter letter;
    if (it == letters.end()) {
      letter = alphabet.intern("stmt" + std::to_string(letters.size()), s.kind);
      letters.emplace(key, letter);
      result.statements.push_back(s);
    } else {
      letter = it->second;
    }
    if (transitions.size() >= context->maxTransitions)
      throw nwa::ResourceLimit();
    transitions.push_back({from, letter, to, hierarchy});
  };
  std::vector<const ICFGNode *> nodes;
  bool hasGlobal = false;
  bool hasEntries = false;
  bool foundError = !errorNode;
  bool foundTarget = !targetError;
  for (const auto &item : icfg) {
    nodes.push_back(item.second);
    hasGlobal |= llvm::isa<GlobalInitBlockNode>(item.second);
    hasEntries |= llvm::isa<FunEntryBlockNode>(item.second);
    foundError |= item.second == errorNode;
  }
  if (!foundError)
    throw std::invalid_argument("error node does not belong to ICFG");
  std::sort(nodes.begin(), nodes.end(),
            [](auto *a, auto *b) { return a->getId() < b->getId(); });
  std::set<const llvm::Function *> functions;
  for (auto *node : nodes) {
    context->check();
    bool start = entry ? llvm::isa<FunEntryBlockNode>(node) &&
                             node->getFunction() == entry
                 : hasGlobal  ? llvm::isa<GlobalInitBlockNode>(node)
                 : hasEntries ? llvm::isa<FunEntryBlockNode>(node)
                              : llvm::isa<IntraBlockNode>(node) &&
                                    node->getBasicBlock() ==
                                        &node->getFunction()->getEntryBlock();
    result.nodeStates[node] = state(start, node == errorNode);
    if (node->getFunction() && functions.insert(node->getFunction()).second)
      result.functions.push_back(node->getFunction());
  }
  // A valid target may be after a noreturn call and consequently have no
  // reachable instruction fragment in the ICFG.
  if (targetError)
    foundTarget = functions.count(targetError->getFunction());
  // Expand instruction ranges without changing the caller's LLVM module.
  std::unordered_map<const ICFGNode *, nwa::State> ends;
  std::unordered_map<const ICFGNode *, const llvm::Instruction *> last;
  std::set<const ICFGNode *> stopped;
  for (auto *node : nodes) {
    context->check();
    auto current = result.nodeStates.at(node);
    const llvm::Instruction *inst = nullptr;
    if (llvm::isa<IntraBlockNode>(node)) {
      inst = node->getBasicBlock()->getFirstNonPHI();
      if (targetError && llvm::isa<llvm::PHINode>(targetError) &&
          targetError->getParent() == node->getBasicBlock())
        inst = targetError;
    } else if (auto *ret = llvm::dyn_cast<CallRetBlockNode>(node))
      inst = ret->getCallSite()->getNextNode();
    else if (llvm::isa<CallUnwindBlockNode>(node) ||
             llvm::isa<FunUnwindExitBlockNode>(node))
      throw std::invalid_argument("exceptional ICFG flow is not supported");
    while (inst) {
      context->check();
      bool error =
          targetError ? inst == targetError : !errorNode && isError(inst);
      foundTarget |= inst == targetError;
      if (error) {
        auto errorState = state(false, true);
        edge(current, errorState,
             {inst, nullptr, nullptr, nwa::LetterKind::Internal, true});
        stopped.insert(node);
        break;
      }
      if (llvm::isa<llvm::CallBase>(inst) || inst->isTerminator())
        break;
      auto next = state();
      edge(current, next, {inst});
      current = next;
      inst = inst->getNextNode();
    }
    ends[node] = current;
    last[node] = inst;
  }
  if (!foundTarget)
    throw std::invalid_argument("target instruction does not belong to ICFG");
  if (std::find(initial.begin(), initial.end(), true) == initial.end())
    throw std::invalid_argument("ICFG has no entry state");
  // The hierarchy is the state immediately BEFORE the matching call.
  std::unordered_map<const llvm::Instruction *, nwa::State> callers;
  for (auto *node : nodes)
    for (auto *e : node->getOutEdges())
      if (e->getEdgeKind() == ICFGEdge::CallCF)
        callers[e->getCallSite()] = ends.at(node);
  for (auto *node : nodes) {
    context->check();
    if (stopped.count(node))
      continue;
    for (auto *e : node->getOutEdges()) {
      Statement s;
      nwa::State hierarchy = nwa::Bottom;
      switch (e->getEdgeKind()) {
      case ICFGEdge::IntraCF:
        s.instruction = last.at(node);
        if (llvm::isa<IntraBlockNode>(e->getDstNode()) && s.instruction)
          s.successor = e->getDstNode()->getBasicBlock();
        break;
      case ICFGEdge::CallCF:
        s = {e->getCallSite(), nullptr, e->getDstNode()->getFunction(),
             nwa::LetterKind::Call};
        break;
      case ICFGEdge::RetCF:
        if (!callers.count(e->getCallSite()))
          throw std::invalid_argument("ICFG return has no matching call");
        hierarchy = callers.at(e->getCallSite());
        s = {e->getCallSite(), nullptr, node->getFunction(),
             nwa::LetterKind::Return};
        break;
      case ICFGEdge::CallToRetCF: {
        auto *summary = llvm::cast<CallToRetCFGEdge>(e);
        if (!summary->hasUnresolvedCallee())
          continue; // Never bypass the body of a resolved call.
        s.instruction = e->getCallSite();
        break;
      }
      default:
        throw std::invalid_argument("unsupported ICFG edge kind");
      }
      edge(ends.at(node), result.nodeStates.at(e->getDstNode()), s, hierarchy);
    }
  }
  // Keep the proof's precondition true; encode module/global initialization
  // as an explicit first letter, as an ICFG global-initialization action.
  auto root = state(true);
  for (nwa::State i = 0; i < root; ++i)
    if (initial[i]) {
      initial[i] = false;
      Statement init;
      init.initialize = true;
      edge(root, i, init);
    }
  result.automaton = std::make_shared<nwa::NestedWordAutomaton>(alphabet);
  for (std::size_t i = 0; i < initial.size(); ++i)
    result.automaton->addState(initial[i], final[i]);
  for (const auto &t : transitions)
    result.automaton->addTransition(t.source, t.letter, t.target, t.hierarchy);
  return result;
}
} // namespace

ProgramAutomaton IcfgToNwa::build(const llvm::Function &function,
                                  const llvm::Instruction *targetError,
                                  nwa::Context context) {
  if (function.isDeclaration() || !function.getParent())
    throw std::invalid_argument("verification requires a function definition");
  auto graph = std::make_shared<ICFG>();
  ICFGBuilder builder(graph.get());
  builder.build(const_cast<llvm::Module *>(function.getParent()));
  auto result = buildGraph(*graph, &function, nullptr, targetError, context);
  result.ownedIcfg = std::move(graph);
  return result;
}

ProgramAutomaton IcfgToNwa::build(const ICFG &icfg, const ICFGNode *errorNode,
                                  nwa::Context context) {
  return buildGraph(icfg, nullptr, errorNode, nullptr, context);
}
} // namespace lotus::verification::traceabstraction
