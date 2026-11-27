#include "Checker/GSAF/Engine/Summaries.h"
#include "IR/GVFG/GuardedValueFlowTrace.h"

#include <limits>
#include <mutex>

#include <llvm/ADT/SmallPtrSet.h>

namespace lotus::gsaf {

SummaryCacheItem::SummaryCacheItem(SMTExpr *constraints, std::string suffix,
                                   int depth)
    : constraints(constraints), suffix(std::move(suffix)), depth(depth) {}

std::pair<SMTExpr, bool>
SummaryCacheItem::getSMTExprFromCache(SMTFactory *Fctry,
                                      SMTRenamingAdvisor *Advisor) const {
  SummaryBase::SMTReadLock();
  SMTExpr Translate = Fctry->translate(*constraints);
  SummaryBase::SMTReadUnlock();

  SMTExprVec TempVec = Fctry->createEmptySMTExprVec();
  TempVec.push_back(Translate);
  std::unordered_map<std::string, SMTExpr> Mapping;
  auto RenamedResult = Fctry->rename(TempVec, suffix, Mapping, Advisor);
  return std::make_pair(RenamedResult.first.toAndExpr(), RenamedResult.second);
}

static SMTFactory *getSummarySMTFactory() {
  static SMTFactory Sfty;
  return &Sfty;
}

static std::mutex SMTLock;

SummaryBase::SummaryBase(llvm::Function *Func, SummaryType Ty, unsigned Depth)
    : Type(Ty), F(Func), InlineDepth(Depth), VulnerabilityMask(-1) {}

SummaryBase::~SummaryBase() {
  SMTWriteLock();

  for (auto &Item : NonSymDepsCache) {
    delete Item.constraints;
  }

  for (auto &Item : SymbDepsCache) {
    delete Item.constraints;
  }

  SMTWriteUnlock();
}

void SummaryBase::SMTReadLock() { SMTLock.lock(); }

void SummaryBase::SMTWriteLock() { SMTLock.lock(); }

void SummaryBase::SMTReadUnlock() { SMTLock.unlock(); }

void SummaryBase::SMTWriteUnlock() { SMTLock.unlock(); }

SummaryBase::SummaryType SummaryBase::getType() const { return Type; }

void SummaryBase::setInputs(
    const std::unordered_set<const gvfg::GuardedValueFlowNode *> &Ins) {
  Inputs = Ins;
}

const std::unordered_set<const gvfg::GuardedValueFlowNode *> &
SummaryBase::getInputs() {
  return Inputs;
}

void SummaryBase::addNonSymDeps(SummaryCacheItem Item) {
  SMTWriteLock();
  if (Item.suffix == "") {
    Item.constraints =
        new SMTExpr(getSummarySMTFactory()->translate(*Item.constraints));
    NonSymDepsCache.push_back(Item);
  } else {
    auto iter = NonSymSummary.find(Item.suffix);
    if (iter == NonSymSummary.end()) {
      auto *NewExpr =
          new SMTExpr(getSummarySMTFactory()->createBoolVal(true));
      NonSymDepsCache.push_back(
          SummaryCacheItem(NewExpr, Item.suffix, Item.depth));
      iter = NonSymSummary.insert(std::make_pair(Item.suffix, NewExpr)).first;
    }

    auto *Expr = iter->second;
    *Expr = *Expr && getSummarySMTFactory()->translate(*Item.constraints);
  }
  SMTWriteUnlock();
}

void SummaryBase::addSymbDeps(SummaryCacheItem Item) {
  SMTWriteLock();
  auto iter = SymbSummary.find(Item.suffix);
  if (iter == SymbSummary.end()) {
    auto *NewExpr = new SMTExpr(getSummarySMTFactory()->createBoolVal(true));
    SymbDepsCache.push_back(SummaryCacheItem(NewExpr, Item.suffix, Item.depth));
    iter = SymbSummary.insert(std::make_pair(Item.suffix, NewExpr)).first;
  }

  auto *Expr = iter->second;
  *Expr = *Expr && getSummarySMTFactory()->translate(*Item.constraints);
  SMTWriteUnlock();
}

void SummaryBase::mergeDisjunctiveConstraints(
    const std::vector<const SummaryBase *> &Group,
    SMTRenamingAdvisor *Advisor) {
  if (Group.empty())
    return;

  SMTWriteLock();
  SMTFactory *SF = getSummarySMTFactory();

  // Identify deferred callee items (non-empty-suffix NonSymDepsCache and
  // SymbDepsCache) that appear in EVERY summary of Group so we can factor them
  // out of the disjunction (A /\ C1) \/ (A /\ C2) == A /\ (C1 \/ C2) without
  // eager renaming or AST duplication.
  using DeferredKey = std::tuple<bool, Z3_ast, std::string>;
  auto collectDeferredKeys = [](const SummaryBase *S) {
    std::set<DeferredKey> Keys;
    if (!S)
      return Keys;
    for (const SummaryCacheItem &Item : S->NonSymDepsCache) {
      if (Item.suffix.empty() || !Item.constraints)
        continue;
      if (Item.constraints->isApp() && Item.constraints->isTrue())
        continue;
      Keys.emplace(false, static_cast<Z3_ast>(Item.constraints->getZ3Expr()),
                   Item.suffix);
    }
    for (const SummaryCacheItem &Item : S->SymbDepsCache) {
      if (!Item.constraints)
        continue;
      if (Item.constraints->isApp() && Item.constraints->isTrue())
        continue;
      Keys.emplace(true, static_cast<Z3_ast>(Item.constraints->getZ3Expr()),
                   Item.suffix);
    }
    return Keys;
  };

  std::set<DeferredKey> CommonDeferred = collectDeferredKeys(Group.front());
  for (size_t I = 1; I < Group.size() && !CommonDeferred.empty(); ++I) {
    auto NextKeys = collectDeferredKeys(Group[I]);
    for (auto It = CommonDeferred.begin(); It != CommonDeferred.end();) {
      if (!NextKeys.count(*It))
        It = CommonDeferred.erase(It);
      else
        ++It;
    }
  }

  SMTExprVec DisjVec = SF->createEmptySMTExprVec();
  llvm::SmallPtrSet<Z3_ast, 32> SeenDisj;
  bool CollapsedTrue = false;
  bool UsedRename = false;
  int MinDepth = std::numeric_limits<int>::max();

  auto appendItemToConj = [&](const SummaryCacheItem &Item, bool IsSymb,
                              SMTExprVec &ConjVec,
                              llvm::SmallPtrSetImpl<Z3_ast> &SeenConj,
                              bool &DisjunctFalse) {
    if (!Item.constraints)
      return;
    SMTExpr E = *Item.constraints;
    if (E.isApp()) {
      if (E.isTrue())
        return;
      if (E.isFalse()) {
        DisjunctFalse = true;
        return;
      }
    }
    if ((IsSymb || !Item.suffix.empty()) &&
        CommonDeferred.count(DeferredKey{
            IsSymb, static_cast<Z3_ast>(E.getZ3Expr()), Item.suffix})) {
      return;
    }
    if (Item.depth < MinDepth)
      MinDepth = Item.depth;
    if (!Item.suffix.empty()) {
      UsedRename = true;
      SMTExprVec Tmp = SF->createEmptySMTExprVec();
      Tmp.push_back(E);
      std::unordered_map<std::string, SMTExpr> Mapping;
      auto Renamed = SF->rename(Tmp, Item.suffix, Mapping, Advisor);
      E = Renamed.first.toAndExpr();
      if (E.isApp()) {
        if (E.isTrue())
          return;
        if (E.isFalse()) {
          DisjunctFalse = true;
          return;
        }
      }
    }
    Z3_ast Ast = static_cast<Z3_ast>(E.getZ3Expr());
    if (SeenConj.insert(Ast).second)
      ConjVec.push_back(E);
  };

  for (const SummaryBase *S : Group) {
    if (!S)
      continue;
    SMTExprVec ConjVec = SF->createEmptySMTExprVec();
    llvm::SmallPtrSet<Z3_ast, 16> SeenConj;
    bool DisjunctFalse = false;
    for (const SummaryCacheItem &Item : S->NonSymDepsCache) {
      appendItemToConj(Item, false, ConjVec, SeenConj, DisjunctFalse);
      if (DisjunctFalse)
        break;
    }
    if (!DisjunctFalse) {
      for (const SummaryCacheItem &Item : S->SymbDepsCache) {
        appendItemToConj(Item, true, ConjVec, SeenConj, DisjunctFalse);
        if (DisjunctFalse)
          break;
      }
    }
    if (DisjunctFalse)
      continue;
    if (ConjVec.empty()) {
      CollapsedTrue = true;
      break;
    }
    SMTExpr OneDisj = ConjVec.toAndExpr();
    if (OneDisj.isApp()) {
      if (OneDisj.isTrue()) {
        CollapsedTrue = true;
        break;
      }
      if (OneDisj.isFalse())
        continue;
    }
    Z3_ast DisjAst = static_cast<Z3_ast>(OneDisj.getZ3Expr());
    if (SeenDisj.insert(DisjAst).second)
      DisjVec.push_back(OneDisj);
  }

  if (UsedRename)
    SF->clearRenamingCache();

  for (auto &Item : NonSymDepsCache)
    delete Item.constraints;
  NonSymDepsCache.clear();
  NonSymSummary.clear();
  for (auto &Item : SymbDepsCache)
    delete Item.constraints;
  SymbDepsCache.clear();
  SymbSummary.clear();

  if (MinDepth == std::numeric_limits<int>::max())
    MinDepth = 0;

  SMTExpr MergedLocal = CollapsedTrue
                            ? SF->createBoolVal(true)
                            : (DisjVec.empty() ? SF->createBoolVal(false)
                                               : DisjVec.toOrExpr());
  NonSymDepsCache.push_back(
      SummaryCacheItem(new SMTExpr(MergedLocal), "", MinDepth));

  if (const SummaryBase *Rep = Group.front(); Rep && !CommonDeferred.empty()) {
    for (const SummaryCacheItem &Item : Rep->NonSymDepsCache) {
      if (Item.suffix.empty() || !Item.constraints)
        continue;
      if (!CommonDeferred.count(DeferredKey{
              false, static_cast<Z3_ast>(Item.constraints->getZ3Expr()),
              Item.suffix}))
        continue;
      auto *Copy = new SMTExpr(*Item.constraints);
      NonSymDepsCache.push_back(SummaryCacheItem(Copy, Item.suffix, Item.depth));
      NonSymSummary[Item.suffix] = Copy;
    }
    for (const SummaryCacheItem &Item : Rep->SymbDepsCache) {
      if (!Item.constraints)
        continue;
      if (!CommonDeferred.count(DeferredKey{
              true, static_cast<Z3_ast>(Item.constraints->getZ3Expr()),
              Item.suffix}))
        continue;
      auto *Copy = new SMTExpr(*Item.constraints);
      SymbDepsCache.push_back(SummaryCacheItem(Copy, Item.suffix, Item.depth));
      SymbSummary[Item.suffix] = Copy;
    }
  }
  SMTWriteUnlock();
}

const std::vector<SummaryCacheItem> &SummaryBase::getSymbDepsCache() {
  return SymbDepsCache;
}

const std::vector<SummaryCacheItem> &SummaryBase::getNonSymDepsCache() {
  return NonSymDepsCache;
}

llvm::Function *SummaryBase::getFunction() { return F; }

unsigned SummaryBase::getInlineDepth() { return InlineDepth; }

void SummaryBase::setVulnerabilityMask(int M) { VulnerabilityMask = M; }

int SummaryBase::getVulnerabilityMask() { return VulnerabilityMask; }

llvm::raw_ostream &operator<<(llvm::raw_ostream &Out, const SummaryBase &N) {
  Out << "************************************\n";
  Out << "* Summary Info: \n";
  Out << "* Type: ";
  switch (N.getType()) {
  case SummaryBase::ST_InputSummary:
    Out << "Input\n";
    break;
  case SummaryBase::ST_OutputSummary:
    Out << "Output\n";
    break;
  case SummaryBase::ST_TransferSummary:
    Out << "Transfer\n";
    break;
  case SummaryBase::ST_SourceSummary:
    Out << "Source\n";
    break;
  case SummaryBase::ST_SymbolicSummary:
    Out << "Symbolic\n";
    break;
  }
  Out << "* Function: " << N.F->getName() << "\n";
  Out << "* Inline Depth: " << N.InlineDepth << "\n";
  Out << "* Used Arguments: \n";
  for (const auto *In : N.Inputs) {
    Out << "* \t" << In->getDescription() << "\n";
  }
  Out << "* Mask: " << N.VulnerabilityMask << "\n";
  Out << "************************************";
  return Out;
}

TraceSummary::~TraceSummary() = default;
} // namespace lotus::gsaf
