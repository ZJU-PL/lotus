#include "Checker/GSAF/Engine/Summaries.h"
#include "IR/GVFG/GuardedValueFlowTrace.h"

#include <mutex>

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
