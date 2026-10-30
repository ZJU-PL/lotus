/*
 *
 * Author: rainoftime
 */
#include "Dataflow/APA/Analyses/Inter/AffineEqualities.h"

#include "Dataflow/APA/Domains/AffineTransfer.h"
#include "Dataflow/APA/LLVM/InterProblem.h"
#include "Dataflow/APA/Solver/Inter/ContextSolver.h"

#include <algorithm>
#include <unordered_set>

#include <llvm/ADT/APInt.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>

namespace elimination {

namespace {

using D = AffineRelationDomain;
using Relation = D::value_type;

struct AffineInterAnalysisTypes {
  using n_t = llvm::Instruction *;
  using fact_t = Relation;
  struct transfer_t {
    llvm::Instruction *inst = nullptr;
    llvm::Instruction *succ = nullptr;
  };
  using f_t = llvm::Function *;
  using i_t = dataflow::controlflow::InterCFG;
  using abstract_domain_t = AffineRelationDomain;
};

constexpr unsigned kDefaultInterAffineEqualitiesCallStringLength = 2;

class InterAffineEqualitiesProblem
    : public LLVMInterEliminationProblem<AffineInterAnalysisTypes>,
      private AffineTransferBuilder {
public:
  using transfer_t = typename AffineInterAnalysisTypes::transfer_t;

  explicit InterAffineEqualitiesProblem(
      llvm::Module &M, InterAffineVocabularyMode VocabularyMode,
      std::size_t MaxTrackedValues,
      const dataflow::controlflow::InterCFG *ICF = nullptr)
      : LLVMInterEliminationProblem<AffineInterAnalysisTypes>(
            findEntryPoints(M), ICF),
        VocabularyMode(VocabularyMode), MaxTrackedValues(MaxTrackedValues) {
    buildVocabulary(M);
    D::configure(&Vocabulary);
  }

  transfer_t edgeTransfer(n_t Src, n_t Dst) const override {
    return transfer_t{Src, Dst};
  }

  fact_t applyTransfer(const transfer_t &T, const fact_t &In) const override {
    if (T.inst == nullptr)
      return In;

    Relation out = In;
    if (!llvm::isa<llvm::PHINode>(T.inst) && instructionHasEffect(*T.inst))
      out = D::extend(instructionTransfer(*T.inst), out);

    if (auto *Succ = T.succ; Succ != nullptr && edgeHasCondition(*T.inst))
      out = D::extend(edgeTransferRelation(*T.inst, *Succ), out);
    if (auto *Succ = T.succ; Succ != nullptr && edgeEntersPhi(*T.inst, *Succ))
      out = D::extend(phiTransferForEdge(*T.inst, *Succ), out);
    return out;
  }

  n_t transferNode(const transfer_t &T) const override { return T.inst; }
  n_t transferSuccessor(const transfer_t &T) const override { return T.succ; }

  fact_t normalFlow(n_t Inst, const fact_t &In) override {
    return applyTransfer(edgeTransfer(Inst, n_t{}), In);
  }

  fact_t callFlow(n_t CallSite, f_t Callee, const fact_t &In) override {
    auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallSite);
    if (Call == nullptr || Callee == nullptr)
      return D::zero();
    return D::project(D::extend(callEntryTransfer(*Call, *Callee), In));
  }

  fact_t returnFlow(n_t CallSite, f_t Callee, n_t ExitStmt, n_t RetSite,
                    const fact_t &In) override {
    return returnFlowWithCallerFact(CallSite, Callee, ExitStmt, RetSite, In,
                                    D::identity());
  }

  fact_t returnFlowWithCallerFact(n_t CallSite, f_t Callee, n_t ExitStmt,
                                  n_t RetSite, const fact_t &CalleeExit,
                                  const fact_t &CallerFact) override {
    (void)RetSite;
    auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallSite);
    if (Call == nullptr || Callee == nullptr || ExitStmt == nullptr)
      return CallerFact;

    Relation returned =
        D::extend(callReturnTransfer(*Call, *Callee, ExitStmt), CalleeExit);
    auto LocalIt = FunctionLocals.find(Call->getFunction());
    const std::vector<const llvm::Value *> &locals =
        LocalIt != FunctionLocals.end() ? LocalIt->second : EmptyLocals;
    return D::mergePreservingLocals(CallerFact, returned, locals);
  }

  fact_t callToRetFlow(n_t CallSite, n_t RetSite,
                       const std::vector<f_t> &Callees,
                       const fact_t &In) override {
    (void)RetSite;
    if (CallSite == nullptr)
      return In;
    auto *Call = llvm::dyn_cast<llvm::CallBase>(CallSite);
    if (Call == nullptr || Call->getType()->isVoidTy() ||
        !D::isTrackedValue(Call))
      return In;
    if (Callees.empty())
      return D::extend(D::makeForget(Call), In);
    return D::extend(D::makeForget(Call), In);
  }

  std::unordered_map<n_t, fact_t> initialSeeds() override {
    std::unordered_map<n_t, fact_t> Seeds;
    for (auto *Entry : getEntryPoints()) {
      if (Entry == nullptr || Entry->empty())
        continue;
      Seeds[&*Entry->getEntryBlock().begin()] = D::identity();
    }
    return Seeds;
  }

  std::size_t vocabularySize() const { return Vocabulary.values.size(); }

private:
  AffineRelationVocabulary Vocabulary;
  std::unordered_map<const llvm::Function *, std::vector<const llvm::Value *>>
      FunctionLocals;
  std::vector<const llvm::Value *> EmptyLocals;
  InterAffineVocabularyMode VocabularyMode;
  std::size_t MaxTrackedValues = 0;

  static std::vector<llvm::Function *> findEntryPoints(llvm::Module &M) {
    std::vector<llvm::Function *> Entries;
    if (auto *Main = M.getFunction("main"); Main != nullptr && !Main->empty())
      Entries.push_back(Main);
    if (Entries.empty()) {
      std::unordered_set<const llvm::Function *> Called;
      for (auto &F : M) {
        if (F.isDeclaration())
          continue;
        for (auto &BB : F) {
          for (auto &I : BB) {
            auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
            auto *Callee =
                Call != nullptr ? Call->getCalledFunction() : nullptr;
            if (Callee != nullptr && !Callee->isDeclaration())
              Called.insert(Callee);
          }
        }
      }
      for (auto &F : M) {
        if (!F.isDeclaration() && !Called.count(&F)) {
          Entries.push_back(&F);
        }
      }
    }
    if (Entries.empty()) {
      for (auto &F : M) {
        if (!F.isDeclaration())
          Entries.push_back(&F);
      }
    }
    return Entries;
  }

  void buildVocabulary(llvm::Module &M) {
    std::unordered_set<const llvm::Function *> Reachable;
    const auto EntryPoints = findEntryPoints(M);
    std::unordered_set<const llvm::Function *> EntryFunctions(
        EntryPoints.begin(), EntryPoints.end());
    std::vector<llvm::Function *> Worklist = EntryPoints;
    while (!Worklist.empty()) {
      auto *F = Worklist.back();
      Worklist.pop_back();
      if (F == nullptr || F->isDeclaration() || !Reachable.insert(F).second)
        continue;
      for (auto &BB : *F) {
        for (auto &I : BB) {
          auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
          auto *Callee = Call != nullptr ? Call->getCalledFunction() : nullptr;
          if (Callee != nullptr && !Callee->isDeclaration())
            Worklist.push_back(Callee);
        }
      }
    }

    if (VocabularyMode == InterAffineVocabularyMode::AllScalars) {
      for (const auto &F : M) {
        if (F.isDeclaration() || !Reachable.count(&F))
          continue;
        for (const auto &Arg : F.args())
          if (isTrackedScalar(&Arg))
            Vocabulary.values.push_back(&Arg);
        for (const auto &BB : F) {
          for (const auto &I : BB) {
            if (!isTrackedScalar(&I))
              continue;
            Vocabulary.values.push_back(&I);
            Vocabulary.localValues.push_back(&I);
            FunctionLocals[&F].push_back(&I);
          }
        }
      }
      finalizeVocabulary();
      return;
    }

    std::unordered_set<const llvm::Value *> Relevant;
    std::vector<const llvm::Value *> SliceWorklist;
    auto seed = [&](const llvm::Value *Value) {
      if (Value == nullptr || !isTrackedScalar(Value))
        return;
      if (!llvm::isa<llvm::Instruction>(Value) &&
          !llvm::isa<llvm::Argument>(Value))
        return;
      SliceWorklist.push_back(Value);
    };

    for (const auto &F : M) {
      if (F.isDeclaration() || !Reachable.count(&F))
        continue;
      if (EntryFunctions.count(&F))
        for (const auto &Arg : F.args())
          seed(&Arg);
      for (const auto &BB : F) {
        for (const auto &I : BB) {
          if (I.isTerminator())
            for (const auto &Op : I.operands())
              seed(Op.get());
          if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I)) {
            if (isAssumeLikeCall(*Call) && !Call->arg_empty())
              seed(Call->getArgOperand(0));
            auto *Callee = Call->getCalledFunction();
            if (Callee == nullptr || Callee->isDeclaration())
              continue;
            auto *Formal = Callee->arg_begin();
            for (unsigned Index = 0;
                 Index < Call->arg_size() && Formal != Callee->arg_end();
                 ++Index, ++Formal) {
              if (!isTrackedScalar(&*Formal))
                continue;
              seed(&*Formal);
              seed(Call->getArgOperand(Index));
            }
          }
        }
      }
    }

    while (!SliceWorklist.empty()) {
      const auto *Value = SliceWorklist.back();
      SliceWorklist.pop_back();
      if (!Relevant.insert(Value).second)
        continue;
      if (const auto *Inst = llvm::dyn_cast<llvm::Instruction>(Value))
        for (const auto &Op : Inst->operands())
          seed(Op.get());
    }

    // Assign stable indices in module order after computing the slice.
    for (const auto &F : M) {
      if (F.isDeclaration() || !Reachable.count(&F))
        continue;
      for (const auto &Arg : F.args())
        if (Relevant.count(&Arg))
          Vocabulary.values.push_back(&Arg);
      for (const auto &BB : F) {
        for (const auto &I : BB) {
          if (!Relevant.count(&I))
            continue;
          Vocabulary.values.push_back(&I);
          Vocabulary.localValues.push_back(&I);
          FunctionLocals[&F].push_back(&I);
        }
      }
    }
    finalizeVocabulary();
  }

  void finalizeVocabulary() {
    if (MaxTrackedValues != 0 && Vocabulary.values.size() > MaxTrackedValues) {
      Vocabulary.values.resize(MaxTrackedValues);
      std::unordered_set<const llvm::Value *> Kept(Vocabulary.values.begin(),
                                                   Vocabulary.values.end());
      Vocabulary.localValues.erase(
          std::remove_if(
              Vocabulary.localValues.begin(), Vocabulary.localValues.end(),
              [&](const llvm::Value *Value) { return !Kept.count(Value); }),
          Vocabulary.localValues.end());
      for (auto &[Function, Locals] : FunctionLocals) {
        (void)Function;
        Locals.erase(std::remove_if(Locals.begin(), Locals.end(),
                                    [&](const llvm::Value *Value) {
                                      return !Kept.count(Value);
                                    }),
                     Locals.end());
      }
    }
    for (unsigned i = 0; i < Vocabulary.values.size(); ++i) {
      Vocabulary.indices[Vocabulary.values[i]] = i;
      Vocabulary.actualBitWidths[Vocabulary.values[i]] =
          Vocabulary.values[i]->getType()->getIntegerBitWidth();
    }
  }
};

} // namespace

InterAffineEqualitiesResult runInterElimAffineEqualities(llvm::Module &M,
    InterAffineEqualitiesOptions options) {
  (void)options.verbose;
  using Solver =
      InterEliminationSolver<AffineInterAnalysisTypes,
                             kDefaultInterAffineEqualitiesCallStringLength>;
  using ResultT = Solver::result_t;

  std::unique_ptr<dataflow::controlflow::LLVMInterCFG> ICF =
      std::make_unique<dataflow::controlflow::LLVMInterCFG>(&M);
  InterAffineEqualitiesProblem Problem(M, options.vocabulary,
                                       options.maxTrackedValues, ICF.get());
  if (options.verbose)
    llvm::errs() << "[inter-affine] tracked-values=" << Problem.vocabularySize()
                 << "\n";
  EliminationOptions ProcedureOptions;
  ProcedureOptions.Method = EliminationMethod::ADTSimple;
  if (options.ordering != OrderingPolicy::Default) {
    ProcedureOptions.Method = EliminationMethod::StateElimination;
  }
  ProcedureOptions.Ordering = options.ordering;
  ProcedureOptions.Order = options.order;
  Solver SolverInstance(Problem, ProcedureOptions);

  InterAffineEqualitiesResult Out;
  Out.vocabulary = *D::getVocabulary();
  Out.trackedValues = Problem.vocabularySize();
  auto Status = SolverInstance.solve();
  Out.status = Status;
  Out.diagnostics = SolverInstance.getDiagnostics();

  const ResultT *Result = SolverInstance.getResults();
  if (Result == nullptr)
    return Out;

  for (auto &F : M) {
    if (F.isDeclaration() || F.empty())
      continue;
    Relation Summary = D::zero();
    bool HaveSummary = false;
    for (auto *Exit : ICF->getExitPointsOf(&F)) {
      if (Exit == nullptr)
        continue;
      for (const auto &Key : Result->contextsForInstruction(Exit)) {
        const auto *Fact = Result->tryOUT(Key);
        if (Fact == nullptr)
          continue;
        Summary = HaveSummary ? D::combine(Summary, *Fact) : *Fact;
        HaveSummary = true;
      }
    }
    if (HaveSummary)
      Out.summaries.emplace(AffineFunctionKey{&F}, std::move(Summary));

    for (auto &BB : F) {
      auto *EntryInst = BB.empty() ? nullptr : &*BB.begin();
      if (EntryInst == nullptr)
        continue;
      Relation BlockRelation = D::zero();
      bool HaveBlock = false;

      for (auto *PredBB : llvm::predecessors(&BB)) {
        auto *PredTerm = PredBB->getTerminator();
        if (PredTerm == nullptr)
          continue;
        for (const auto &Key : Result->contextsForInstruction(PredTerm)) {
          const auto *Fact = Result->tryOUT(Key);
          if (Fact == nullptr)
            continue;
          Relation EdgeRelation = Problem.applyTransfer(
              Problem.edgeTransfer(PredTerm, EntryInst), *Fact);
          BlockRelation = HaveBlock ? D::combine(BlockRelation, EdgeRelation)
                                    : EdgeRelation;
          HaveBlock = true;
        }
      }

      if (!HaveBlock) {
        for (const auto &Key : Result->contextsForInstruction(EntryInst)) {
          const auto *Fact = Result->tryIN(Key);
          if (Fact == nullptr)
            continue;
          BlockRelation = HaveBlock ? D::combine(BlockRelation, *Fact) : *Fact;
          HaveBlock = true;
        }
      }
      if (HaveBlock)
        Out.blockRelations.emplace(AffineBlockKey{&BB},
                                   std::move(BlockRelation));
    }
  }
  return Out;
}

} // namespace elimination
