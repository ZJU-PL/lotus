//===- AbstractInterpretation.cpp -- Abstract Execution-------------//
//
// Migrated from SVF's AE engine to Lotus.
//
//===----------------------------------------------------------------------===//

#include "Checker/AE/AbstractInterpretation.h"

#include "Checker/AE/AbsExtAPI.h"
#include "Checker/AE/RelationSolver.h"
#include "Checker/AE/SVFIRWrapper.h"
#include "Checker/Framework/BugReportMgr.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "IR/SVFG/SVFGNode.h"
#include "Solvers/SMT/LIBSMT/Z3Expr.h"

// AserPTA includes
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Context/NoCtx.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Graph/CallGraph.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/LanguageModel/DefaultLangModel/DefaultLangModel.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/FSMemModel.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/PointerAnalysisPass.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/PointsTo/BitVectorPTS.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/WavePropagation.h"

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <memory>
#include <queue>
#include <set>
#include <stack>
#include <unordered_map>
#include <unordered_set>

#include <llvm/ADT/GraphTraits.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus {
namespace analysis {

namespace {

bool resolveGlobalAddress(const llvm::Constant *constant,
                          const llvm::DataLayout &dl,
                          const llvm::Value *&baseValue, int64_t &byteOffset) {
  if (!constant)
    return false;

  if (const auto *alias = llvm::dyn_cast<llvm::GlobalAlias>(constant)) {
    if (const llvm::Constant *aliasee = alias->getAliasee()) {
      return resolveGlobalAddress(aliasee, dl, baseValue, byteOffset);
    }
    return false;
  }

  if (const auto *gv = llvm::dyn_cast<llvm::GlobalValue>(constant)) {
    baseValue = gv;
    byteOffset = 0;
    return true;
  }

  const auto *ce = llvm::dyn_cast<llvm::ConstantExpr>(constant);
  if (!ce)
    return false;

  switch (ce->getOpcode()) {
  case llvm::Instruction::BitCast:
  case llvm::Instruction::AddrSpaceCast:
    return resolveGlobalAddress(llvm::cast<llvm::Constant>(ce->getOperand(0)),
                                dl, baseValue, byteOffset);
  case llvm::Instruction::GetElementPtr: {
    const llvm::Constant *baseConst =
        llvm::cast<llvm::Constant>(ce->getOperand(0));
    if (!resolveGlobalAddress(baseConst, dl, baseValue, byteOffset)) {
      return false;
    }

    llvm::APInt offset(64, 0, /*isSigned=*/true);
    llvm::Instruction *gepInst = ce->getAsInstruction();
    const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(gepInst);
    const bool ok = gep && gep->accumulateConstantOffset(dl, offset);
    if (gepInst) {
      gepInst->deleteValue();
    }
    if (!ok) {
      return false;
    }
    byteOffset += offset.getSExtValue();
    return true;
  }
  default:
    return false;
  }
}

AbstractValue evaluateConstantInitializer(AbstractState &state,
                                          const llvm::Constant *init,
                                          const llvm::DataLayout &dl) {
  if (!init) {
    return AbstractValue(IntervalValue::top());
  }

  if (llvm::isa<llvm::ConstantPointerNull>(init)) {
    return AbstractValue(AddressValue(NullMemAddr));
  }

  if (const auto *ci = llvm::dyn_cast<llvm::ConstantInt>(init)) {
    return AbstractValue(IntervalValue(ci->getSExtValue()));
  }

  if (const auto *cfp = llvm::dyn_cast<llvm::ConstantFP>(init)) {
    double val = cfp->getValueAPF().convertToDouble();
    return AbstractValue(
        IntervalValue(static_cast<int64_t>(val), static_cast<int64_t>(val)));
  }

  const llvm::Value *baseValue = nullptr;
  int64_t byteOffset = 0;
  if (resolveGlobalAddress(init, dl, baseValue, byteOffset)) {
    uint32_t baseId = AbstractInterpretation::getValueIdStatic(baseValue);
    if (byteOffset <= 0) {
      return AbstractValue(
          AddressValue(AddressValue::getVirtualMemAddress(baseId)));
    }

    AddressValue fieldAddrs =
        state.getGepObjAddrs(baseId, IntervalValue(byteOffset));
    if (!fieldAddrs.isBottom()) {
      return AbstractValue(fieldAddrs);
    }
    return AbstractValue(
        AddressValue(AddressValue::getVirtualMemAddress(baseId)));
  }

  return AbstractValue(IntervalValue::top());
}

void storeInitializerValue(AbstractState &state, uint32_t baseValueId,
                           uint32_t byteOffset, bool useFieldObject,
                           const AbstractValue &value) {
  AddressValue targetAddrs;
  if (useFieldObject || byteOffset != 0) {
    targetAddrs = state.getGepObjAddrs(baseValueId, IntervalValue(byteOffset));
  } else if (state.inVarToAddrsTable(baseValueId)) {
    targetAddrs = state[baseValueId].getAddrs();
  }

  for (uint32_t addr : targetAddrs) {
    state.store(addr, value);
  }
}

void materializeGlobalInitializer(AbstractState &state, uint32_t baseValueId,
                                  uint32_t byteOffset, bool useFieldObject,
                                  const llvm::Constant *init,
                                  const llvm::DataLayout &dl) {
  if (!init)
    return;

  if (llvm::isa<llvm::ConstantAggregateZero>(init)) {
    if (init->getType()->isAggregateType()) {
      if (const auto *structTyConst =
              llvm::dyn_cast<llvm::StructType>(init->getType())) {
        auto *structTy = const_cast<llvm::StructType *>(structTyConst);
        const llvm::StructLayout *layout = dl.getStructLayout(structTy);
        for (unsigned i = 0; i < structTy->getNumElements(); ++i) {
          llvm::Constant *fieldInit =
              llvm::Constant::getNullValue(structTy->getElementType(i));
          materializeGlobalInitializer(state, baseValueId,
                                       byteOffset + layout->getElementOffset(i),
                                       true, fieldInit, dl);
        }
        return;
      }
      if (const auto *arrayTy =
              llvm::dyn_cast<llvm::ArrayType>(init->getType())) {
        llvm::Type *elemTy = arrayTy->getElementType();
        uint64_t elemSize =
            elemTy->isSized() ? dl.getTypeAllocSize(elemTy) : uint64_t{1};
        for (uint64_t i = 0; i < arrayTy->getNumElements(); ++i) {
          llvm::Constant *elemInit = llvm::Constant::getNullValue(elemTy);
          materializeGlobalInitializer(state, baseValueId,
                                       byteOffset +
                                           static_cast<uint32_t>(i * elemSize),
                                       true, elemInit, dl);
        }
        return;
      }
      if (const auto *vecTy =
              llvm::dyn_cast<llvm::FixedVectorType>(init->getType())) {
        llvm::Type *elemTy = vecTy->getElementType();
        uint64_t elemSize =
            elemTy->isSized() ? dl.getTypeAllocSize(elemTy) : uint64_t{1};
        for (unsigned i = 0; i < vecTy->getNumElements(); ++i) {
          llvm::Constant *elemInit = llvm::Constant::getNullValue(elemTy);
          materializeGlobalInitializer(state, baseValueId,
                                       byteOffset +
                                           static_cast<uint32_t>(i * elemSize),
                                       true, elemInit, dl);
        }
        return;
      }
    }
    storeInitializerValue(state, baseValueId, byteOffset, useFieldObject,
                          evaluateConstantInitializer(state, init, dl));
    return;
  }

  if (const auto *structInit = llvm::dyn_cast<llvm::ConstantStruct>(init)) {
    auto *structTy = const_cast<llvm::StructType *>(structInit->getType());
    const llvm::StructLayout *layout = dl.getStructLayout(structTy);
    for (unsigned i = 0; i < structInit->getNumOperands(); ++i) {
      const llvm::Constant *fieldInit =
          llvm::cast<llvm::Constant>(structInit->getOperand(i));
      materializeGlobalInitializer(state, baseValueId,
                                   byteOffset + layout->getElementOffset(i),
                                   true, fieldInit, dl);
    }
    return;
  }

  if (const auto *arrayInit = llvm::dyn_cast<llvm::ConstantArray>(init)) {
    llvm::Type *elemTy = arrayInit->getType()->getElementType();
    uint64_t elemSize =
        elemTy->isSized() ? dl.getTypeAllocSize(elemTy) : uint64_t{1};
    for (unsigned i = 0; i < arrayInit->getNumOperands(); ++i) {
      const llvm::Constant *elemInit =
          llvm::cast<llvm::Constant>(arrayInit->getOperand(i));
      materializeGlobalInitializer(
          state, baseValueId, byteOffset + static_cast<uint32_t>(i * elemSize),
          true, elemInit, dl);
    }
    return;
  }

  if (const auto *vecInit = llvm::dyn_cast<llvm::ConstantVector>(init)) {
    llvm::Type *elemTy = vecInit->getType()->getElementType();
    uint64_t elemSize =
        elemTy->isSized() ? dl.getTypeAllocSize(elemTy) : uint64_t{1};
    for (unsigned i = 0; i < vecInit->getNumOperands(); ++i) {
      const llvm::Constant *elemInit =
          llvm::cast<llvm::Constant>(vecInit->getOperand(i));
      materializeGlobalInitializer(
          state, baseValueId, byteOffset + static_cast<uint32_t>(i * elemSize),
          true, elemInit, dl);
    }
    return;
  }

  storeInitializerValue(state, baseValueId, byteOffset, useFieldObject,
                        evaluateConstantInitializer(state, init, dl));
}

AbstractValue unknownValueForType(llvm::Type *type) {
  if (type && type->isPointerTy()) {
    return AbstractValue(AddressValue(
        AbstractState::getVirtualMemAddress(AbstractState::BlkPtr)));
  }
  return AbstractValue(IntervalValue::top());
}

void materializeConstantValue(AbstractState &as, const llvm::Value *val,
                              uint32_t valueId) {
  if (!val)
    return;

  if (as.inVarToValTable(valueId) || as.inVarToAddrsTable(valueId))
    return;

  if (const auto *ci = llvm::dyn_cast<llvm::ConstantInt>(val)) {
    as[valueId] = AbstractValue(IntervalValue(ci->getSExtValue()));
    return;
  }

  if (const auto *cfp = llvm::dyn_cast<llvm::ConstantFP>(val)) {
    double num = cfp->getValueAPF().convertToDouble();
    as[valueId] = AbstractValue(
        IntervalValue(static_cast<int64_t>(num), static_cast<int64_t>(num)));
    return;
  }

  if (llvm::isa<llvm::ConstantPointerNull>(val)) {
    as[valueId] = AbstractValue(AddressValue(NullMemAddr));
    return;
  }

  const auto *constant = llvm::dyn_cast<llvm::Constant>(val);
  if (constant && val->getType()->isPointerTy()) {
    llvm::Module *module = AbstractInterpretation::getAEInstance().getModule();
    if (!module)
      return;

    const llvm::Value *base = nullptr;
    int64_t byteOffset = 0;
    if (!resolveGlobalAddress(constant, module->getDataLayout(), base,
                              byteOffset))
      return;
    uint32_t baseId = AbstractInterpretation::getValueIdStatic(base);
    if (!as.inVarToAddrsTable(baseId))
      return;
    AddressValue addrs = as.getGepObjAddrs(baseId, IntervalValue(byteOffset));
    as[valueId] = AbstractValue(addrs);
  }
}

const llvm::Value *resolveAggregateElement(const llvm::Value *aggregate,
                                           llvm::ArrayRef<unsigned> indices) {
  if (!aggregate)
    return nullptr;

  if (const auto *insert = llvm::dyn_cast<llvm::InsertValueInst>(aggregate)) {
    llvm::ArrayRef<unsigned> insertedIndices = insert->getIndices();
    const bool insertedPathIsPrefix =
        insertedIndices.size() <= indices.size() &&
        std::equal(insertedIndices.begin(), insertedIndices.end(),
                   indices.begin());
    if (insertedPathIsPrefix) {
      if (insertedIndices.size() == indices.size())
        return insert->getInsertedValueOperand();
      return resolveAggregateElement(insert->getInsertedValueOperand(),
                                     indices.slice(insertedIndices.size()));
    }
    return resolveAggregateElement(insert->getAggregateOperand(), indices);
  }

  if (const auto *insert = llvm::dyn_cast<llvm::InsertElementInst>(aggregate)) {
    if (indices.size() == 1) {
      if (const auto *lane =
              llvm::dyn_cast<llvm::ConstantInt>(insert->getOperand(1))) {
        if (lane->getZExtValue() == indices.front())
          return insert->getOperand(2);
      }
    }
    return resolveAggregateElement(insert->getOperand(0), indices);
  }

  const auto *constant = llvm::dyn_cast<llvm::Constant>(aggregate);
  if (!constant)
    return nullptr;
  for (unsigned index : indices) {
    constant = constant->getAggregateElement(index);
    if (!constant)
      return nullptr;
  }
  return constant;
}

std::pair<uint64_t, uint64_t> unsignedIntervalBounds(const IntervalValue &value,
                                                     unsigned bits) {
  if (value.isBottom())
    return {1, 0};
  if (bits > 64 || value.is_infinite()) {
    const uint64_t maxValue =
        bits >= 64
            ? std::numeric_limits<uint64_t>::max()
            : static_cast<uint64_t>((static_cast<__uint128_t>(1) << bits) - 1);
    return {0, maxValue};
  }

  const __uint128_t modulus = static_cast<__uint128_t>(1) << bits;
  const uint64_t maxValue = static_cast<uint64_t>(modulus - 1);
  const __int128 lb = value.lb().getIntNumeral();
  const __int128 ub = value.ub().getIntNumeral();
  if (static_cast<__uint128_t>(ub - lb) >= modulus - 1)
    return {0, maxValue};
  const auto normalize = [&](const __int128 bound) {
    const __int128 mod = static_cast<__int128>(modulus);
    return static_cast<uint64_t>((bound % mod + mod) % mod);
  };
  const uint64_t lower = normalize(lb), upper = normalize(ub);
  return lower <= upper ? std::make_pair(lower, upper)
                        : std::make_pair(uint64_t{0}, maxValue);
}

IntervalValue signedIntervalFromUnsigned(uint64_t lower, uint64_t upper,
                                         unsigned bits) {
  // Boolean SSA values use 0/1; other integers use signed bit-pattern values.
  if (bits == 1)
    return IntervalValue(static_cast<int64_t>(lower),
                         static_cast<int64_t>(upper));
  const uint64_t sign = uint64_t{1} << (bits - 1);
  if (lower < sign && upper >= sign) {
    if (bits == 64)
      return IntervalValue(std::numeric_limits<int64_t>::min(),
                           std::numeric_limits<int64_t>::max());
    return IntervalValue(-static_cast<int64_t>(sign),
                         static_cast<int64_t>(sign - 1));
  }
  return IntervalValue(llvm::APInt(bits, lower).getSExtValue(),
                       llvm::APInt(bits, upper).getSExtValue());
}

IntervalValue unsignedBinaryInterval(unsigned opcode, const IntervalValue &lhs,
                                     const IntervalValue &rhs, unsigned bits) {
  if (lhs.isBottom() || rhs.isBottom())
    return IntervalValue::bottom();
  if (bits > 64)
    return IntervalValue::top();
  const auto a = unsignedIntervalBounds(lhs, bits);
  const auto b = unsignedIntervalBounds(rhs, bits);
  const uint64_t max = bits == 64 ? std::numeric_limits<uint64_t>::max()
                                  : (uint64_t{1} << bits) - 1;
  if (opcode == llvm::Instruction::LShr) {
    if (b.first >= bits)
      return signedIntervalFromUnsigned(0, max, bits);
    const uint64_t lastShift = std::min<uint64_t>(b.second, bits - 1);
    return signedIntervalFromUnsigned(a.first >> lastShift, a.second >> b.first,
                                      bits);
  }
  if (b.first == 0)
    return signedIntervalFromUnsigned(0, max, bits);
  if (opcode == llvm::Instruction::UDiv)
    return signedIntervalFromUnsigned(a.first / b.second, a.second / b.first,
                                      bits);
  if (a.first == a.second && b.first == b.second)
    return signedIntervalFromUnsigned(a.first % b.first, a.first % b.first,
                                      bits);
  return signedIntervalFromUnsigned(0, std::min(a.second, b.second - 1), bits);
}

bool unsignedPredicatePossible(int predicate, const IntervalValue &lhs,
                               const IntervalValue &rhs, unsigned bits) {
  const auto [lhsMin, lhsMax] = unsignedIntervalBounds(lhs, bits);
  const auto [rhsMin, rhsMax] = unsignedIntervalBounds(rhs, bits);
  switch (predicate) {
  case llvm::CmpInst::ICMP_UGT:
    return lhsMax > rhsMin;
  case llvm::CmpInst::ICMP_UGE:
    return lhsMax >= rhsMin;
  case llvm::CmpInst::ICMP_ULT:
    return lhsMin < rhsMax;
  case llvm::CmpInst::ICMP_ULE:
    return lhsMin <= rhsMax;
  default:
    return true;
  }
}

void clearAEBugReports() {
  static constexpr const char *AEBugTypeNames[] = {
      "AE Buffer Overflow",  "AE Null Dereference", "AE Divide By Zero",
      "AE Integer Overflow", "AE Use After Free",   "AE Invalid Free",
      "AE Memory Leak"};

  BugReportMgr &mgr = BugReportMgr::get_instance();
  std::vector<int> ids;
  for (const char *name : AEBugTypeNames) {
    int id = mgr.find_bug_type(name);
    if (id >= 0) {
      ids.push_back(id);
    }
  }
  mgr.clear_reports_for_types(ids);
}

bool isAEQuietMode() {
  const char *env = std::getenv("LOTUS_AE_QUIET");
  return env != nullptr && env[0] != '\0' && env[0] != '0';
}

} // namespace

// Opaque pointer implementation for AserPTA
using PTASolver = aser::WavePropagation<aser::DefaultLangModel<
    aser::NoCtx, aser::FSMemModel<aser::NoCtx>, aser::BitVectorPTS>>;
using PTAPass = PointerAnalysisPass<PTASolver>;

// Forward declaration wrapper
class AbstractInterpretation::PTAPtr {
public:
  std::unique_ptr<PTAPass> pass;
  PTAPtr() : pass(std::make_unique<PTAPass>()) {}
  ~PTAPtr() = default;
};

class AbstractInterpretation::SVFGSparseContext {
public:
  std::unique_ptr<ICFG> icfg;
  std::unique_ptr<SVFGBuilder> builder;
  SVFG *graph = nullptr;
};

AbstractInterpretation::AbstractInterpretation()
    : module_(nullptr), pta_(std::make_unique<PTAPtr>()),
      recursionMode_(WIDEN_NARROW), nextValueId_(3) {
  stat = new AEStat(this);
  utils = nullptr;
  callGraph_ = nullptr;
}

AbstractInterpretation::~AbstractInterpretation() {
  delete stat;
  delete utils;
  delete svfir_;
  svfir_ = nullptr;
  // Clean up WTO objects
  for (auto &pair : funcToWTO_) {
    delete pair.second;
  }
  funcToWTO_.clear();
}

void AbstractInterpretation::resetAnalysisState() {
  // Clear all state to prevent leakage between module analyses while
  // preserving the configured detector set and user-selected options.
  stateStore_.clear();
  globalState = AbstractState();
  checkpoints.clear();
  callSiteStack.clear();
  checkedCheckpoints_.clear();
  delete svfir_;
  svfir_ = nullptr;

  // Clear value ID mappings
  valueToIdMap_.clear();
  idToValueMap_.clear();
  gepFieldObjMap_.clear();
  nextGepFieldId_ = 0x00800000;
  gepObjOffsetFromBaseByObjId_.clear();
  // Reserve low IDs used by special memory sentinels:
  //   0 -> null pointer
  //   2 -> InvalidMemAddr internal ID
  // Start from 3 so regular values never collide with sentinels.
  nextValueId_ = 3;

  // Clear call graph and recursion tracking
  recursiveFuns_.clear();
  callSiteRecursionDepth_.clear();
  nonRecursiveCallSites_.clear();
  recursiveSccIdMap_.clear();
  recursiveSccMembers_.clear();
  rootSccEntries_.clear();
  dominatorTrees_.clear();
  cycleHeadToCycle_.clear();
  cycleValueIds_.clear();
  retSlotIds_.clear();
  svfgSparse_.reset();
  sparseObjectDefs_.clear();
  svfgSparseLoadQueries_ = 0;
  analyzedFunctions_.clear();

  // Clean up WTO objects
  for (auto &pair : funcToWTO_) {
    delete pair.second;
  }
  funcToWTO_.clear();

  // Reset statistics
  if (stat) {
    stat->count = 0;
    stat->generalNumMap.clear();
    stat->timeStatMap.clear();
    stat->startTime = 0;
    stat->endTime = 0;
    stat->stmtCount = 0;
    stat->icfgNodeNum = 0;
    stat->funcNum = 0;
    stat->extCallSiteNum = 0;
    stat->nonExtCallSiteNum = 0;
  }

  for (auto &detector : detectors) {
    if (detector) {
      detector->reset();
    }
  }

  // Recreate the pointer-analysis pass instead of reusing it.
  //
  // `PointerAnalysisPass::analyze` short-circuits when the module pointer and
  // entry name match the ones it already analyzed. When a previous module has
  // been freed and the next one is allocated at the same address, that check
  // hands back the *stale* analysis of the old module: its objects point into
  // freed memory. Recreating the pass makes every run analyze its own module.
  if (pta_)
    pta_->pass.reset(new PTAPass());
  ptaReady_ = false;
}

void AbstractInterpretation::reset() {
  resetAnalysisState();

  recursionMode_ = WIDEN_NARROW;
  widenDelay_ = 3;
  strictCheckpoint_ = true;
  enableBufOverflowCheck_ = false;
  enableNullDerefCheck_ = false;
  enableDivZeroCheck_ = false;
  enableOverflowCheck_ = false;
  enableMemLeakCheck_ = false;
  analyzeAllFunctions_ = false;
  printStats_ = false;
  detectors.clear();
}

void *AbstractInterpretation::getPTAPass() const {
  if (ptaReady_ && pta_) {
    return pta_->pass.get();
  }
  return nullptr;
}

uint32_t AbstractInterpretation::getOrCreateGepFieldObjId(uint32_t baseObjId,
                                                          int64_t offset) {
  const auto key = std::make_pair(baseObjId, offset);
  auto it = gepFieldObjMap_.find(key);
  if (it != gepFieldObjMap_.end())
    return it->second;
  if (nextGepFieldId_ >= 0x01000000)
    return baseObjId;
  const uint32_t newId = nextGepFieldId_++;
  gepFieldObjMap_.emplace(key, newId);
  return newId;
}

void AbstractInterpretation::recordGepObjOffsetFromBase(
    uint32_t objId, const IntervalValue &offset) {
  auto [it, inserted] = gepObjOffsetFromBaseByObjId_.emplace(objId, offset);
  if (!inserted)
    it->second.join_with(offset);
}

bool AbstractInterpretation::getGepObjOffsetFromBase(
    uint32_t objId, IntervalValue &offset) const {
  auto it = gepObjOffsetFromBaseByObjId_.find(objId);
  if (it == gepObjOffsetFromBaseByObjId_.end())
    return false;
  offset = it->second;
  return true;
}

void AbstractInterpretation::runOnModule(llvm::Module *module) {
  resetAnalysisState();
  clearAEBugReports();

  module_ = module;

  auto pruneDetector = [&](AEDetector::DetectorKind kind, bool enabled) {
    if (enabled)
      return;
    detectors.erase(
        std::remove_if(detectors.begin(), detectors.end(),
                       [&](const std::unique_ptr<AEDetector> &detector) {
                         return detector && detector->getKind() == kind;
                       }),
        detectors.end());
  };
  pruneDetector(AEDetector::DIV_ZERO, enableDivZeroCheck_);
  pruneDetector(AEDetector::INT_OVERFLOW, enableOverflowCheck_);
  pruneDetector(AEDetector::MEMORY_LEAK, enableMemLeakCheck_);

  const bool hasDefinedFunction =
      std::any_of(module->begin(), module->end(), [](const llvm::Function &F) {
        return !F.isDeclaration() && !F.isIntrinsic();
      });
  if (!hasDefinedFunction) {
    if (!isAEQuietMode()) {
      llvm::errs() << "AE: module contains no function definitions\n";
    }
    return;
  }

  // Always run pointer analysis (required for AE)
  pta_->pass->analyze(module);
  ptaReady_ = true;

  // Create SVFIRWrapper for PTA-based queries (like SVF's SVFIR)
  svfir_ = new SVFIRWrapper(getPTAPass(), module_);

  if (stateStore_.layout() == StateLayout::Sparse)
    initializeSVFGSparse(module);

  // Initialize global state with SVFIRWrapper
  globalState.svfir_ = svfir_;

  // Delete old utils if exists
  if (utils) {
    delete utils;
  }
  utils = new AEExtAPI(stateStore_);

  auto hasDetectorKind = [&](AEDetector::DetectorKind kind) {
    return std::any_of(detectors.begin(), detectors.end(),
                       [&](const std::unique_ptr<AEDetector> &detector) {
                         return detector && detector->getKind() == kind;
                       });
  };

  if (enableDivZeroCheck_ && !hasDetectorKind(AEDetector::DIV_ZERO)) {
    addDetector(std::make_unique<DivZeroDetector>());
  }
  if (enableOverflowCheck_ && !hasDetectorKind(AEDetector::INT_OVERFLOW)) {
    addDetector(std::make_unique<OverflowDetector>());
  }
  if (enableBufOverflowCheck_ && !hasDetectorKind(AEDetector::BUF_OVERFLOW)) {
    addDetector(std::make_unique<BufOverflowDetector>());
  }
  if (enableNullDerefCheck_ && !hasDetectorKind(AEDetector::NULL_DEREF)) {
    addDetector(std::make_unique<NullptrDerefDetector>());
  }
  if (enableMemLeakCheck_ && !hasDetectorKind(AEDetector::MEMORY_LEAK)) {
    addDetector(std::make_unique<MemLeakDetector>());
  }

  stat->startClk();
  collectCheckPoint();
  analyse();
  checkPointAllSet();
  stat->endClk();

  stat->finializeStat();
  if (!isAEQuietMode() && printStats_) {
    stat->performStat();
    const AbstractStateStore::Statistics &storeStats = stateStore_.stats();
    llvm::errs() << "\n======= Abstract state store =======\n"
                 << "layout                     : "
                 << toString(stateStore_.layout()) << "\n"
                 << "states retained            : " << stateStore_.size()
                 << "\n"
                 << "states pinned              : " << stateStore_.pinnedCount()
                 << "\n"
                 << "states created             : " << storeStats.created
                 << "\n"
                 << "states released            : " << storeStats.released
                 << "\n"
                 << "peak states                : " << storeStats.peak << "\n";
  }

  for (auto &detector : detectors)
    detector->reportBug();

  if (!isAEQuietMode()) {
    llvm::outs().flush();
    llvm::errs().flush();
  }
}

/// Program entry
void AbstractInterpretation::analyse() {
  startTimeBudget();

  // Initialize call graph SCC detection
  initCallGraphSCC();

  // Initialize WTO for all functions
  initWTO();

  handleGlobalNode();

  // Collect module-level statistics once (static counts).
  for (const llvm::Function &func : *module_) {
    if (func.isDeclaration()) {
      continue;
    }
    stat->funcNum++;
    for (const llvm::BasicBlock &bb : func) {
      for (const llvm::Instruction &inst : bb) {
        stat->icfgNodeNum++;
        const auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
        if (!call) {
          continue;
        }
        if (isExtCall(call)) {
          stat->extCallSiteNum++;
        } else {
          stat->nonExtCallSiteNum++;
        }
      }
    }
  }

  if (!analyzeAllFunctions_) {
    const llvm::Function *mainFunc = module_->getFunction("main");
    if (!mainFunc || mainFunc->isDeclaration())
      mainFunc = module_->getFunction("svf.main");
    if (mainFunc && !mainFunc->isDeclaration())
      handleFunction(mainFunc);
    return;
  }

  // Select roots of the resolved call graph, not every definition. Otherwise
  // a helper visited before its caller is analyzed with unconstrained inputs.
  for (const llvm::Function *func : rootSccEntries_) {
    if (timeBudgetExhausted())
      return;
    handleFunction(func);
  }
}

/// Handle global node
void AbstractInterpretation::handleGlobalNode() {
  const llvm::DataLayout &dl = module_->getDataLayout();

  for (auto &global : module_->globals()) {
    uint32_t globalId = getValueId(&global);
    uint32_t globalAddr = AddressValue::getVirtualMemAddress(globalId);

    // In LLVM IR a global variable is used as its address.
    globalState[globalId] = AbstractValue(AddressValue(globalAddr));
    if (global.getValueType()->isSized()) {
      globalState.setObjSize(globalId,
                             dl.getTypeAllocSize(global.getValueType()));
    }

    if (global.hasInitializer() && !global.isDeclaration()) {
      materializeGlobalInitializer(globalState, globalId, 0,
                                   global.getValueType()->isAggregateType(),
                                   global.getInitializer(), dl);
    }
  }

  // Initialize null pointer constant (ID 0) in global state
  globalState[0] = AbstractValue(AddressValue(NullMemAddr));
  globalState[AbstractState::BlkPtr] = AbstractValue(IntervalValue::top());
}

void AbstractInterpretation::initCallGraphSCC() {
  // Build call graph using AserPTA (includes indirect calls)
  std::unordered_map<const llvm::Function *,
                     std::vector<const llvm::Function *>>
      callGraph;

  // Get call graph from AserPTA
  const aser::CallGraph<aser::NoCtx> *cg = nullptr;
  if (ptaReady_ && pta_ && pta_->pass) {
    cg = pta_->pass->getPTA()->getCallGraph();
  }

  // Build a map: LLVM Function -> set of callee Functions
  std::unordered_map<const llvm::Function *, std::set<const llvm::Function *>>
      funcToCallees;

  // First, collect all direct calls from LLVM IR (for completeness)
  for (const llvm::Function &F : *module_) {
    if (F.isDeclaration())
      continue;
    for (const llvm::BasicBlock &BB : F) {
      for (const llvm::Instruction &I : BB) {
        const llvm::CallBase *cb = llvm::dyn_cast<llvm::CallBase>(&I);
        if (!cb)
          continue;
        if (const llvm::Function *direct = cb->getCalledFunction()) {
          if (!direct->isDeclaration()) {
            funcToCallees[&F].insert(direct);
          }
        }
      }
    }
  }

  // Now enrich with AserPTA's call graph (includes indirect calls)
  // Iterate over all call graph nodes to find indirect calls and their targets
  if (cg) {
    for (auto nodeIt = cg->begin(); nodeIt != cg->end(); ++nodeIt) {
      const auto *cgNode = *nodeIt;
      if (!cgNode)
        continue;

      // Process indirect call nodes to find resolved targets
      if (cgNode->isIndirectCall()) {
        if (auto *indCall = cgNode->getTargetFunPtr()) {
          const llvm::Instruction *callInst = indCall->getCallSite();
          if (callInst && callInst->getFunction()) {
            const llvm::Function *callerFunc = callInst->getFunction();
            if (!callerFunc->isDeclaration()) {
              // Get all resolved targets for this indirect call
              for (auto *resolved : indCall->getResolvedNode()) {
                if (resolved && !resolved->isIndirectCall()) {
                  if (auto *targetFun = resolved->getTargetFun()) {
                    const llvm::Function *calleeFunc = targetFun->getFunction();
                    if (calleeFunc && !calleeFunc->isDeclaration()) {
                      funcToCallees[callerFunc].insert(calleeFunc);
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  // Convert set-based map to vector-based map for SCC algorithm
  for (const auto &pair : funcToCallees) {
    callGraph[pair.first] = std::vector<const llvm::Function *>(
        pair.second.begin(), pair.second.end());
  }

  // Tarjan's SCC algorithm (iterative version to avoid stack overflow)
  std::unordered_map<const llvm::Function *, uint32_t> index, lowlink;
  std::stack<const llvm::Function *> stk;
  std::unordered_set<const llvm::Function *> onStack;
  uint32_t nextIndex = 0;
  std::vector<std::set<const llvm::Function *>> sccs;

  struct Frame {
    const llvm::Function *f;
    std::vector<const llvm::Function *>::const_iterator it;
    std::vector<const llvm::Function *>::const_iterator end;
  };
  std::stack<Frame> worklist;

  auto visit = [&](const llvm::Function *root) {
    if (index.count(root))
      return;
    worklist.push({root, {}, {}});
    while (!worklist.empty()) {
      Frame &frame = worklist.top();
      const llvm::Function *f = frame.f;
      if (!index.count(f)) {
        index[f] = lowlink[f] = nextIndex++;
        stk.push(f);
        onStack.insert(f);
        auto cgIt = callGraph.find(f);
        if (cgIt != callGraph.end()) {
          frame.it = cgIt->second.begin();
          frame.end = cgIt->second.end();
        } else {
          frame.it = frame.end = {};
        }
      }
      bool pushed = false;
      while (frame.it != frame.end) {
        const llvm::Function *callee = *frame.it;
        ++frame.it;
        if (!index.count(callee)) {
          worklist.push({callee, {}, {}});
          pushed = true;
          break;
        } else if (onStack.count(callee)) {
          lowlink[f] = std::min(lowlink[f], index[callee]);
        }
      }
      if (pushed)
        continue;
      worklist.pop();
      if (!worklist.empty()) {
        const llvm::Function *parent = worklist.top().f;
        lowlink[parent] = std::min(lowlink[parent], lowlink[f]);
      }
      if (lowlink[f] == index[f]) {
        std::set<const llvm::Function *> scc;
        const llvm::Function *w;
        do {
          w = stk.top();
          stk.pop();
          onStack.erase(w);
          scc.insert(w);
        } while (w != f);
        sccs.push_back(std::move(scc));
      }
    }
  };

  // Visit all functions
  for (const llvm::Function &F : *module_) {
    if (F.isDeclaration())
      continue;
    visit(&F);
  }

  // Select root SCCs after resolving both direct and indirect call edges.
  std::unordered_map<const llvm::Function *, size_t> sccIds;
  for (size_t i = 0; i < sccs.size(); ++i)
    for (const llvm::Function *func : sccs[i])
      sccIds[func] = i;
  std::unordered_set<size_t> hasCaller;
  for (const auto &entry : callGraph) {
    for (const llvm::Function *callee : entry.second) {
      if (sccIds.at(entry.first) != sccIds.at(callee))
        hasCaller.insert(sccIds.at(callee));
    }
  }
  std::unordered_set<size_t> selected;
  for (const llvm::Function &func : *module_) {
    if (func.isDeclaration())
      continue;
    const size_t id = sccIds.at(&func);
    if (hasCaller.count(id) || !selected.insert(id).second)
      continue;
    const llvm::Function *entry = &func;
    for (const llvm::Function *member : sccs[id]) {
      if (member->getName() == "main") {
        entry = member;
        break;
      }
      if (member->getName() == "svf.main")
        entry = member;
    }
    rootSccEntries_.push_back(entry);
  }

  // Identify recursive SCCs: multi-function SCCs + self-recursive functions.
  std::unordered_set<size_t> recursiveSccIds;
  for (size_t i = 0; i < sccs.size(); ++i) {
    if (sccs[i].size() > 1) {
      // Multi-function SCC (mutual recursion)
      recursiveSccIds.insert(i);
    } else if (sccs[i].size() == 1) {
      // Check for self-recursion (self-edge in call graph)
      const llvm::Function *f = *sccs[i].begin();
      auto it = callGraph.find(f);
      if (it != callGraph.end()) {
        for (const llvm::Function *callee : it->second) {
          if (callee == f) {
            recursiveSccIds.insert(i);
            break;
          }
        }
      }
    }
  }

  // Populate recursiveFuns_ set
  for (size_t i = 0; i < sccs.size(); ++i) {
    if (recursiveSccIds.count(i)) {
      uint32_t rid = static_cast<uint32_t>(i);
      auto &members = recursiveSccMembers_[rid];
      for (const llvm::Function *f : sccs[i]) {
        recursiveFuns_.insert(f);
        recursiveSccIdMap_[f] = rid;
        members.push_back(f);
      }
      // SCC detection walks pointer-keyed maps, so member order can change
      // between processes. Fixpoint iteration (and therefore the def-site
      // layout, which overwrites shared values) must not depend on that.
      std::sort(members.begin(), members.end(),
                [](const llvm::Function *lhs, const llvm::Function *rhs) {
                  return lhs->getName() < rhs->getName();
                });
    }
  }

  // Track entry calls (calls from outside SCC to inside SCC) - these are NOT
  // recursive callsites. This matches SVF's nonRecursiveCallSites tracking.
  // We identify entry calls by finding call sites where:
  // 1. The caller is outside the SCC
  // 2. The callee is inside the SCC
  for (size_t i = 0; i < sccs.size(); ++i) {
    if (!recursiveSccIds.count(i))
      continue; // Only process recursive SCCs

    const std::set<const llvm::Function *> &scc = sccs[i];

    // Find all call sites that call into this SCC from outside
    for (const llvm::Function &F : *module_) {
      if (F.isDeclaration())
        continue;

      // Check if this function is outside the SCC
      bool callerInSCC = scc.count(&F) > 0;
      if (callerInSCC)
        continue; // Skip if caller is already in the SCC

      for (const llvm::BasicBlock &BB : F) {
        for (const llvm::Instruction &I : BB) {
          const llvm::CallBase *cb = llvm::dyn_cast<llvm::CallBase>(&I);
          if (!cb)
            continue;

          // For direct calls, check if callee is in the SCC
          if (const llvm::Function *direct = cb->getCalledFunction()) {
            if (!direct->isDeclaration() && scc.count(direct) > 0) {
              // This is an entry call from outside SCC to inside SCC
              // Mark it as non-recursive call site (entry point)
              nonRecursiveCallSites_.insert({cb, direct});
            }
          }
          // For indirect calls, resolve via AserPTA and check if targets are in
          // SCC
          else {
            for (const llvm::Function *indirectCallee : getCallees(cb)) {
              if (indirectCallee && !indirectCallee->isDeclaration() &&
                  scc.count(indirectCallee) > 0) {
                nonRecursiveCallSites_.insert({cb, indirectCallee});
              }
            }
          }
        }
      }
    }
  }

  // Track recursive call sites (for statistics/debugging)
  for (const llvm::Function &F : *module_) {
    if (F.isDeclaration())
      continue;
    for (const llvm::BasicBlock &BB : F) {
      for (const llvm::Instruction &I : BB) {
        const llvm::CallBase *cb = llvm::dyn_cast<llvm::CallBase>(&I);
        if (!cb)
          continue;
        if (const llvm::Function *callee = cb->getCalledFunction()) {
          if (recursiveFuns_.count(&F) && recursiveFuns_.count(callee) &&
              &F != callee) {
            // This is a recursive call site (within same SCC)
            callSiteRecursionDepth_[cb] = 0; // Initialize depth
          } else if (recursiveFuns_.count(callee) && &F == callee) {
            // Self-recursive call
            callSiteRecursionDepth_[cb] = 0;
          }
        }
      }
    }
  }
}

void AbstractInterpretation::initWTO() {
  // Build WTO for each function in the module
  for (auto &func : *module_) {
    if (func.isDeclaration())
      continue;

    // Build WTO for this function
    ICFGWTO *wto = new ICFGWTO(&func);
    funcToWTO_[&func] = wto;

    // Collect cycle heads for efficient lookup
    collectCycleHeads(wto->getComponents(), &func);
  }
}

void AbstractInterpretation::collectCycleHeads(
    const std::list<const ICFGWTOComp *> &comps, const llvm::Function *func) {
  for (const ICFGWTOComp *comp : comps) {
    if (const ICFGCycleWTO *cycle = llvm::dyn_cast<ICFGCycleWTO>(comp)) {
      // Map cycle head (entry) to the cycle
      cycleHeadToCycle_[cycle->getEntry()] = cycle;
      // Recursively collect nested cycle heads
      collectCycleHeads(cycle->getComponents(), func);
    }
  }
}

void AbstractInterpretation::handleFunction(const llvm::Function *func) {
  if (!func || func->isDeclaration())
    return;
  analyzedFunctions_.insert(func);

  // Get WTO for this function
  auto it = funcToWTO_.find(func);
  if (it == funcToWTO_.end()) {
    // No WTO available - fall back to simple worklist
    std::set<const llvm::BasicBlock *> visited;
    std::set<const llvm::BasicBlock *> inProgress;
    std::vector<const llvm::BasicBlock *> worklist;
    worklist.push_back(&func->getEntryBlock());

    while (!worklist.empty()) {
      const llvm::BasicBlock *bb = worklist.back();
      worklist.pop_back();

      if (visited.count(bb))
        continue;
      if (inProgress.count(bb)) {
        continue;
      }

      inProgress.insert(bb);
      visited.insert(bb);

      if (!mergeStatesFromPredecessors(bb))
        continue;

      for (auto &inst : *bb) {
        if (!handleInstruction(&inst)) {
          return;
        }
      }

      const llvm::Instruction *term = bb->getTerminator();
      for (unsigned i = 0; i < term->getNumSuccessors(); ++i) {
        worklist.push_back(term->getSuccessor(i));
      }

      inProgress.erase(bb);
    }
    return;
  }

  // Use WTO-based traversal
  ICFGWTO *wto = it->second;
  const std::list<const ICFGWTOComp *> &components = wto->getComponents();

  // Process WTO components in order
  for (const ICFGWTOComp *comp : components) {
    if (const ICFGSingletonWTO *singleton =
            llvm::dyn_cast<ICFGSingletonWTO>(comp)) {
      handleSingletonWTO(singleton->getBlock());
    } else if (const ICFGCycleWTO *cycle = llvm::dyn_cast<ICFGCycleWTO>(comp)) {
      handleCycleWTO(cycle);
    }
  }
}

void AbstractInterpretation::handleSingletonWTO(const llvm::BasicBlock *bb) {
  stat->getBlockTrace()++;

  // Merge states from predecessors
  if (!mergeStatesFromPredecessors(bb))
    return;

  // Process all instructions in the block
  for (auto &inst : *bb) {
    if (!handleInstruction(&inst)) {
      return;
    }
  }

  stat->countStateSize();
}

/// Handle WTO cycle (loop or recursive function) using widening/narrowing
/// iteration.
///
/// Widening is applied at the cycle head to ensure termination of the analysis.
/// The cycle head's abstract state is iteratively updated until a fixpoint is
/// reached.
///
/// == What is being widened ==
/// The abstract state at the cycle head node, which includes:
/// - Variable values (intervals) that may change across loop iterations
/// - For example, a loop counter `i` starting at 0 and incrementing each
/// iteration
///
/// == Regular loops (non-recursive functions) ==
/// All modes (TOP/WIDEN_ONLY/WIDEN_NARROW) behave the same for regular loops:
/// 1. Widening phase: Iterate until the cycle head state stabilizes
///    Example: for(i=0; i<100; i++) -> i widens to [0, +inf]
/// 2. Narrowing phase: Refine the over-approximation from widening
///    Example: [0, +inf] narrows to [0, 100] using loop condition
///
/// == Recursive function cycles ==
/// Behavior depends on recursionMode_:
///
/// - TOP mode:
///     Does not iterate. Calls recursiveCallPass() to set all stores and
///     return value to TOP immediately. This is the most conservative but
///     fastest. Example:
///       int factorial(int n) { return n <= 1 ? 1 : n * factorial(n-1); }
///       factorial(5) -> returns [-inf, +inf]
///
/// - WIDEN_ONLY mode:
///     Widening phase only, no narrowing for recursive functions.
///     The recursive function body is analyzed with widening until fixpoint.
///     Example:
///       int factorial(int n) { return n <= 1 ? 1 : n * factorial(n-1); }
///       factorial(5) -> returns [10000, +inf] (widened upper bound)
///
/// - WIDEN_NARROW mode:
///     Both widening and narrowing phases for recursive functions.
///     After widening reaches fixpoint, narrowing refines the result.
///     Example:
///       int factorial(int n) { return n <= 1 ? 1 : n * factorial(n-1); }
///       factorial(5) -> returns [10000, 10000] (precise after narrowing)

void AbstractInterpretation::handleCycleWTO(const ICFGCycleWTO *cycle) {
  const llvm::BasicBlock *cycleHead = cycle->getEntry();
  const llvm::Function *func = cycleHead->getParent();

  // TOP mode for recursive function cycles: use recursiveCallPass to set
  // all stores and return value to TOP, maintaining original semantics
  if (recursionMode_ == TOP && isRecursiveFun(func)) {
    // Find the call site that entered this function
    if (!callSiteStack.empty()) {
      const llvm::CallBase *callNode = callSiteStack.back();
      bool entersThisFunction = false;
      for (const llvm::Function *resolved : getCallees(callNode)) {
        if (resolved == func) {
          entersThisFunction = true;
          break;
        }
      }
      if (entersThisFunction) {
        if (isRecursiveCallSite(callNode, func)) {
          recursiveCallPass(callNode, func);
          return;
        }

        // Entry calls into a recursive SCC should still execute the function
        // body once. Only the inner recursive callsites are summarized to TOP.
        handleSingletonWTO(cycleHead);
        for (const ICFGWTOComp *comp : cycle->getComponents()) {
          if (const ICFGSingletonWTO *singleton =
                  llvm::dyn_cast<ICFGSingletonWTO>(comp)) {
            handleSingletonWTO(singleton->getBlock());
          } else if (const ICFGCycleWTO *subCycle =
                         llvm::dyn_cast<ICFGCycleWTO>(comp)) {
            handleCycleWTO(subCycle);
          }
        }
        return;
      }
    }
  }

  // WIDEN_ONLY / WIDEN_NARROW modes: iterate until fixpoint
  bool increasing = true;
  const uint32_t widenDelay = widenDelay_;
  const bool useCarriedHeadState = stateStore_.usesDefSiteVars();
  struct SuppressionGuard {
    bool &flag;
    bool previous;
    explicit SuppressionGuard(bool &f) : flag(f), previous(f) {}
    ~SuppressionGuard() { flag = previous; }
  } suppressionGuard{suppressDetectors_};
  AbstractState carriedHeadState;
  bool hasCarriedHeadState = false;
  auto publishCycleHeadState = [&](const AbstractState &state) {
    // `state` is the widened/narrowed state after interpreting the complete
    // cycle-head block. In SVF the cycle head is one ICFG node, so writing the
    // new state at that node is sufficient. Lotus uses LLVM basic blocks, whose
    // later instructions are separate state keys; publish the result to the
    // block entry (for the next iteration) and terminator (for the body) so the
    // body observes the widened phi values instead of the pre-widening
    // terminator snapshot.
    if (cycleHead->empty())
      return;
    stateStore_.set(&cycleHead->front(), state);
    stateStore_.set(cycleHead->getTerminator(), state);
  };

  for (uint32_t curIter = 0;; curIter++) {
    if (timeBudgetExhausted())
      break;
    // Get the abstract state before processing the cycle head
    AbstractState prevHeadState;
    const std::unordered_set<uint32_t> *cycleVals = nullptr;
    if (stateStore_.usesDefSiteVars())
      cycleVals = cycleValueIds(cycle);

    // Try to get state from first instruction of cycle head
    if (!cycleHead->empty()) {
      const llvm::Instruction *firstInst = &cycleHead->front();
      if (useCarriedHeadState && hasCarriedHeadState) {
        prevHeadState = carriedHeadState;
      } else if (const AbstractState *headState = stateStore_.find(firstInst)) {
        prevHeadState = *headState;
        // Snapshot the previous head state before interpreting the head. In the
        // def-site layout a state copy aliases the run-wide value table, so
        // materializing it later would observe values written by this iteration
        // and compare the iteration with itself.
        if (stateStore_.usesDefSiteVars())
          prevHeadState.materializeVarTable(cycleVals);
      }
    }

    // Process the cycle head node
    handleSingletonWTO(cycleHead);

    // Get current state after processing
    AbstractState curHeadState;
    if (!cycleHead->empty()) {
      const llvm::Instruction *firstInst = &cycleHead->front();
      if (const AbstractState *headState = stateStore_.find(firstInst)) {
        curHeadState = *headState;
      }
    }

    // Def-site layout: widen only the values defined inside the cycle. They are
    // pulled out of the run-wide table into these two snapshots, widened here,
    // and scattered back by the `set()` below -- SVF's gather/scatter around
    // `widenCycleState` (see `SparseAbstractInterpretation::widenCycleState`).
    if (stateStore_.usesDefSiteVars()) {
      curHeadState.materializeVarTable(cycleVals);
    }

    // Match SVF: apply widening/narrowing at cycle head before processing body.
    suppressDetectors_ =
        suppressionGuard.previous || (curIter >= widenDelay && increasing);
    if (curIter >= widenDelay) {
      if (increasing) {
        // Apply widening operator
        AbstractState widenedState = prevHeadState.widening(curHeadState);
        // Update state at cycle head
        publishCycleHeadState(widenedState);
        if (useCarriedHeadState) {
          carriedHeadState = widenedState;
          hasCarriedHeadState = true;
        }
        curHeadState = widenedState; // Update for comparison

        // Check if widening fixpoint reached
        if (widenedState == prevHeadState) {
          // Widening fixpoint reached, switch to narrowing phase
          increasing = false;
        }
      } else {
        // Narrowing phase - check if narrowing should be applied
        const llvm::Function *func = cycleHead->getParent();
        if (!shouldApplyNarrowing(func)) {
          break;
        }

        // Apply narrowing
        AbstractState narrowedState = prevHeadState.narrowing(curHeadState);

        // Update state at cycle head
        publishCycleHeadState(narrowedState);
        if (useCarriedHeadState) {
          carriedHeadState = narrowedState;
          hasCarriedHeadState = true;
        }
        curHeadState = narrowedState; // Update for comparison

        // Check if narrowing fixpoint reached
        if (narrowedState == prevHeadState) {
          // Narrowing fixpoint reached, exit loop
          break;
        }
      }
    } else {
      if (useCarriedHeadState) {
        carriedHeadState = curHeadState;
        hasCarriedHeadState = true;
      }
    }

    // Process cycle body components
    for (const ICFGWTOComp *comp : cycle->getComponents()) {
      if (const ICFGSingletonWTO *singleton =
              llvm::dyn_cast<ICFGSingletonWTO>(comp)) {
        handleSingletonWTO(singleton->getBlock());
      } else if (const ICFGCycleWTO *subCycle =
                     llvm::dyn_cast<ICFGCycleWTO>(comp)) {
        // Handle nested cycle recursively
        handleCycleWTO(subCycle);
      }
    }
  }
}

bool AbstractInterpretation::mergeStatesFromPredecessors(
    const llvm::BasicBlock *bb) {
  std::vector<AbstractState> workList;
  AbstractState preAs;

  for (auto predIt = llvm::pred_begin(bb), predEnd = llvm::pred_end(bb);
       predIt != predEnd; ++predIt) {
    const llvm::BasicBlock *pred = *predIt;

    // Get state from predecessor block (stored at terminator or block)
    AbstractState tmpEs;
    bool foundState = false;

    // Try to get state from terminator instruction
    const llvm::Instruction *term = pred->getTerminator();
    if (const AbstractState *termState = stateStore_.find(term)) {
      tmpEs = *termState;
      foundState = true;
    } else {
      // Try to get state from first instruction of block
      if (!pred->empty()) {
        if (const AbstractState *frontState =
                stateStore_.find(&pred->front())) {
          tmpEs = *frontState;
          foundState = true;
        }
      }
    }

    if (foundState) {
      // Branch refinement mutates this snapshot, so it must not alias the
      // run-wide def-site table.
      tmpEs.materializeVarTable();
      // Check if this is a conditional branch and if the edge is feasible
      if (const llvm::BranchInst *branch =
              llvm::dyn_cast<llvm::BranchInst>(term)) {
        if (branch->isConditional() &&
            branch->getSuccessor(0) != branch->getSuccessor(1)) {
          // Determine which successor this edge leads to
          bool isTrueEdge = branch->getSuccessor(0) == bb;
          if (isBranchFeasible(branch, tmpEs, isTrueEdge)) {
            workList.push_back(tmpEs);
          }
        } else {
          workList.push_back(tmpEs);
        }
      } else if (const llvm::SwitchInst *switchInst =
                     llvm::dyn_cast<llvm::SwitchInst>(term)) {
        if (isSwitchSuccessorFeasible(switchInst, bb, tmpEs))
          workList.push_back(tmpEs);
      } else {
        // Other terminator types (unconditional branches, returns, etc.)
        workList.push_back(tmpEs);
      }
    }
  }

  if (workList.empty()) {
    // Entry blocks have no predecessors; seed them with global state.
    if (bb == &bb->getParent()->getEntryBlock()) {
      if (!bb->empty()) {
        const llvm::Instruction *firstInst = &bb->front();
        AbstractState entryState = globalState;
        if (const AbstractState *existing = stateStore_.find(firstInst)) {
          // Preserve callsite-seeded entry state (args/params) and merge
          // globals.
          entryState.joinWith(*existing);
        }
        if (callSiteStack.empty()) {
          for (const llvm::Argument &arg : bb->getParent()->args()) {
            uint32_t argId = getValueId(&arg);
            auto it = entryState._varToAbsVal.find(argId);
            if (it == entryState._varToAbsVal.end() ||
                (it->second.getInterval().isBottom() &&
                 it->second.getAddrs().isBottom())) {
              entryState[argId] = unknownValueForType(arg.getType());
            }
          }
        }
        stateStore_.set(firstInst, entryState);
      }
      return true;
    }

    // No predecessors with state - initialize empty state
    if (!bb->empty()) {
      stateStore_.set(&bb->front(), AbstractState());
    }
    return false;
  }

  // Join all feasible predecessor states
  preAs = workList[0];
  for (size_t i = 1; i < workList.size(); ++i) {
    preAs.joinWith(workList[i]);
  }

  // Store state at first instruction of block
  if (!bb->empty()) {
    stateStore_.set(&bb->front(), preAs);
  }
  return true;
}

bool AbstractInterpretation::handleInstruction(const llvm::Instruction *inst) {
  stat->stmtCount++;
  // Checking the clock per instruction is too expensive; sample every 256.
  if ((stat->stmtCount & 0xFFu) == 0 && timeBudgetExhausted())
    return false;
  this->currentInstruction_ = inst;

  const llvm::BasicBlock *bb = inst->getParent();
  const llvm::Instruction *prevInst = nullptr;
  for (const llvm::Instruction &I : *bb) {
    if (&I == inst)
      break;
    prevInst = &I;
  }

  // Intra-block flow is a state transformation, not a once-per-instruction
  // initialization. A block may be revisited through a backedge, so every
  // instruction after the block entry must be refreshed from the current state
  // of its predecessor on *every* visit. Keeping the state created by the first
  // visit makes loop-carried values stale (for example, a[i] would keep seeing
  // the first iteration's i).
  if (prevInst) {
    if (const AbstractState *prevState = stateStore_.find(prevInst))
      stateStore_.inherit(inst, *prevState);
    else
      stateStore_.inherit(inst, AbstractState());
  } else if (!stateStore_.contains(inst)) {
    // The block-entry state is normally installed by
    // mergeStatesFromPredecessors. Keep the fallback for callers that interpret
    // a block directly without running that merge.
    bool predInherited = false;
    auto predIt = llvm::pred_begin(bb);
    if (predIt != llvm::pred_end(bb)) {
      const llvm::Instruction *predTerm = (*predIt)->getTerminator();
      if (const AbstractState *predState = stateStore_.find(predTerm)) {
        stateStore_.inherit(inst, *predState);
        predInherited = true;
      }
    }
    if (!predInherited)
      stateStore_.inherit(inst, AbstractState());
  }

  AbstractState &as = stateStore_.getOrCreate(inst);
  // Pin the states a later reader may observe: call sites (checkpoints,
  // external API models, detector queries), block terminators (successor joins)
  // and allocas (object-size bookkeeping). Everything else stays transient.
  if (llvm::isa<llvm::CallBase>(inst) || inst->isTerminator() ||
      llvm::isa<llvm::AllocaInst>(inst))
    stateStore_.pin(inst);

  // Initialize null pointer constant in abstract state if not already present
  // NullPtr is 0, which is the ID assigned to ConstantPointerNull values
  if (as._varToAbsVal.find(0) == as._varToAbsVal.end()) {
    // Null pointer constant (ID 0) should be represented as NullMemAddr
    // (0x7f000000)
    as[0] = AbstractValue(AddressValue(NullMemAddr));
  }

  switch (inst->getOpcode()) {
  case llvm::Instruction::Alloca:
    updateStateOnAddr(llvm::cast<llvm::AllocaInst>(inst));
    break;
  case llvm::Instruction::Add:
  case llvm::Instruction::FAdd:
  case llvm::Instruction::Sub:
  case llvm::Instruction::FSub:
  case llvm::Instruction::Mul:
  case llvm::Instruction::FMul:
  case llvm::Instruction::SDiv:
  case llvm::Instruction::UDiv:
  case llvm::Instruction::FDiv:
  case llvm::Instruction::SRem:
  case llvm::Instruction::URem:
  case llvm::Instruction::FRem:
  case llvm::Instruction::Shl:
  case llvm::Instruction::LShr:
  case llvm::Instruction::AShr:
  case llvm::Instruction::And:
  case llvm::Instruction::Or:
  case llvm::Instruction::Xor:
    updateStateOnBinary(llvm::cast<llvm::BinaryOperator>(inst));
    break;
  case llvm::Instruction::FNeg:
    updateStateOnFNeg(llvm::cast<llvm::UnaryOperator>(inst));
    break;
  case llvm::Instruction::AtomicRMW:
  case llvm::Instruction::AtomicCmpXchg:
    as[getValueId(inst)] = unknownValueForType(inst->getType());
    break;
  case llvm::Instruction::ICmp:
  case llvm::Instruction::FCmp:
    updateStateOnCmp(llvm::cast<llvm::CmpInst>(inst));
    break;
  case llvm::Instruction::Load:
    updateStateOnLoad(llvm::cast<llvm::LoadInst>(inst));
    break;
  case llvm::Instruction::Store:
    updateStateOnStore(llvm::cast<llvm::StoreInst>(inst));
    break;
  case llvm::Instruction::GetElementPtr:
    updateStateOnGep(llvm::cast<llvm::GetElementPtrInst>(inst));
    break;
  case llvm::Instruction::Call:
  case llvm::Instruction::Invoke:
  case llvm::Instruction::CallBr:
    updateStateOnCall(llvm::cast<llvm::CallBase>(inst));
    break;
  case llvm::Instruction::Select:
    updateStateOnSelect(llvm::cast<llvm::SelectInst>(inst));
    break;
  case llvm::Instruction::PHI:
    updateStateOnPhi(llvm::cast<llvm::PHINode>(inst));
    break;
  // Aggregate operations (equivalent to SVF's CopyStmt)
  case llvm::Instruction::ExtractValue:
    updateStateOnExtractValue(llvm::cast<llvm::ExtractValueInst>(inst));
    break;
  case llvm::Instruction::InsertValue:
    updateStateOnInsertValue(llvm::cast<llvm::InsertValueInst>(inst));
    break;
  case llvm::Instruction::ExtractElement:
    updateStateOnExtractElement(llvm::cast<llvm::ExtractElementInst>(inst));
    break;
  case llvm::Instruction::InsertElement:
    updateStateOnInsertElement(llvm::cast<llvm::InsertElementInst>(inst));
    break;
  case llvm::Instruction::ShuffleVector:
    updateStateOnShuffleVector(llvm::cast<llvm::ShuffleVectorInst>(inst));
    break;
  case llvm::Instruction::Freeze:
    updateStateOnCopy(inst, inst->getOperand(0));
    break;
  case llvm::Instruction::VAArg:
    updateStateOnCopy(inst, inst->getOperand(0));
    break;
  case llvm::Instruction::Ret:
    updateStateOnRet(llvm::cast<llvm::ReturnInst>(inst));
    break;
  case llvm::Instruction::Trunc:
  case llvm::Instruction::ZExt:
  case llvm::Instruction::SExt:
  case llvm::Instruction::FPToUI:
  case llvm::Instruction::FPToSI:
  case llvm::Instruction::UIToFP:
  case llvm::Instruction::SIToFP:
  case llvm::Instruction::FPTrunc:
  case llvm::Instruction::FPExt:
  case llvm::Instruction::PtrToInt:
  case llvm::Instruction::IntToPtr:
  case llvm::Instruction::BitCast:
  case llvm::Instruction::AddrSpaceCast:
    updateStateOnCast(llvm::cast<llvm::CastInst>(inst));
    break;
  default:
    break;
  }

  if (!suppressDetectors_) {
    for (auto &detector : detectors)
      detector->detect(as, inst);
  }

  as.commitPendingFrees();

  this->currentInstruction_ = nullptr;

  return true;
}

void AbstractInterpretation::updateStateOnAddr(const llvm::AllocaInst *addr) {
  uint32_t addrId = getValueId(addr);
  // Use improved version that tracks VLA sizes from abstract state
  // Note: We use the state at the alloca instruction itself
  uint32_t objSize = stateStore_.getOrCreatePinned(addr).getAllocaInstByteSize(
      addr, stateStore_.getOrCreatePinned(addr));

  AbstractValue av(AddressValue(AddressValue::getVirtualMemAddress(addrId)));
  stateStore_.getOrCreate(addr)[addrId] = av;
  stateStore_.getOrCreate(addr).setObjSize(addrId, objSize);
}

void AbstractInterpretation::updateStateOnBinary(
    const llvm::BinaryOperator *binary) {
  uint32_t lhsId = getValueId(binary);
  uint32_t op0Id = getValueId(binary->getOperand(0));
  uint32_t op1Id = getValueId(binary->getOperand(1));

  AbstractState &as = stateStore_.getOrCreate(binary);

  materializeConstantValue(as, binary->getOperand(0), op0Id);
  materializeConstantValue(as, binary->getOperand(1), op1Id);

  // Initialize operands if not present
  if (!as.inVarToValTable(op0Id))
    as[op0Id] = AbstractValue(IntervalValue::top());
  if (!as.inVarToValTable(op1Id))
    as[op1Id] = AbstractValue(IntervalValue::top());

  IntervalValue val0 = as[op0Id].getInterval();
  IntervalValue val1 = as[op1Id].getInterval();
  IntervalValue result;

  auto getNonNegativeConstant = [](const llvm::Value *value,
                                   uint64_t &constant) -> bool {
    const auto *ci = llvm::dyn_cast<llvm::ConstantInt>(value);
    if (!ci || ci->isNegative())
      return false;
    constant = ci->getZExtValue();
    return constant <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  };

  switch (binary->getOpcode()) {
  case llvm::Instruction::Add:
  case llvm::Instruction::FAdd:
    result = val0 + val1;
    break;
  case llvm::Instruction::Sub:
  case llvm::Instruction::FSub:
    result = val0 - val1;
    break;
  case llvm::Instruction::Mul:
  case llvm::Instruction::FMul:
    result = val0 * val1;
    break;
  case llvm::Instruction::SDiv:
  case llvm::Instruction::FDiv:
    if (val1.contains(0)) {
      // Division by zero - result is top
      result = IntervalValue::top();
    } else {
      result = val0 / val1;
    }
    break;
  case llvm::Instruction::UDiv:
  case llvm::Instruction::URem:
  case llvm::Instruction::LShr:
    result =
        binary->getType()->isIntegerTy()
            ? unsignedBinaryInterval(binary->getOpcode(), val0, val1,
                                     binary->getType()->getIntegerBitWidth())
            : IntervalValue::top();
    break;
  case llvm::Instruction::SRem: {
    uint64_t divisor = 0;
    if (getNonNegativeConstant(binary->getOperand(1), divisor) &&
        divisor != 0) {
      const int64_t bound = static_cast<int64_t>(divisor - 1);
      result = IntervalValue(-bound, bound);
      break;
    }
    if (val1.contains(0)) {
      // Modulo by zero - result is top
      result = IntervalValue::top();
    } else {
      result = val0 % val1;
    }
    break;
  }
  case llvm::Instruction::FRem:
    if (val1.contains(0)) {
      // Modulo by zero - result is top
      result = IntervalValue::top();
    } else {
      result = val0 % val1;
    }
    break;
  case llvm::Instruction::Shl:
    result = val0 << val1;
    break;
  case llvm::Instruction::AShr:
    result = val0 >> val1;
    break;
  case llvm::Instruction::And: {
    uint64_t mask = 0;
    if (getNonNegativeConstant(binary->getOperand(1), mask) ||
        getNonNegativeConstant(binary->getOperand(0), mask)) {
      result = IntervalValue(0, static_cast<int64_t>(mask));
    } else {
      result = val0 & val1;
    }
    break;
  }
  case llvm::Instruction::Or:
    result = val0 | val1;
    break;
  case llvm::Instruction::Xor:
    result = val0 ^ val1;
    break;
  default:
    result = IntervalValue::top();
    break;
  }

  as[lhsId] = AbstractValue(result);
}

void AbstractInterpretation::updateStateOnFNeg(const llvm::UnaryOperator *neg) {
  uint32_t lhsId = getValueId(neg);
  uint32_t opId = getValueId(neg->getOperand(0));
  AbstractState &as = stateStore_.getOrCreate(neg);
  materializeConstantValue(as, neg->getOperand(0), opId);

  if (!as.inVarToValTable(opId)) {
    as[lhsId] = AbstractValue(IntervalValue::top());
    return;
  }

  const IntervalValue value = as[opId].getInterval();
  if (value.isBottom()) {
    as[lhsId] = AbstractValue(IntervalValue::bottom());
    return;
  }
  as[lhsId] = AbstractValue(IntervalValue(-value.ub(), -value.lb()));
}

void AbstractInterpretation::updateStateOnCmp(const llvm::CmpInst *cmp) {
  uint32_t resId = getValueId(cmp);
  uint32_t op0Id = getValueId(cmp->getOperand(0));
  uint32_t op1Id = getValueId(cmp->getOperand(1));

  AbstractState &as = stateStore_.getOrCreate(cmp);

  materializeConstantValue(as, cmp->getOperand(0), op0Id);
  materializeConstantValue(as, cmp->getOperand(1), op1Id);

  // Handle address comparisons
  if (as.inVarToAddrsTable(op0Id) && as.inVarToAddrsTable(op1Id)) {
    AddressValue addrOp0 = as[op0Id].getAddrs();
    AddressValue addrOp1 = as[op1Id].getAddrs();
    IntervalValue resVal;
    const bool unknownTarget = addrOp0.empty() || addrOp1.empty() ||
                               addrOp0.contains(InvalidMemAddr) ||
                               addrOp1.contains(InvalidMemAddr);
    const bool mayEqual = addrOp0.hasIntersect(addrOp1);
    const bool mustEqual = !unknownTarget && mayEqual && addrOp0.size() == 1 &&
                           addrOp1.size() == 1;

    switch (cmp->getPredicate()) {
    case llvm::CmpInst::ICMP_EQ:
      if (mustEqual) {
        resVal =
            IntervalValue(static_cast<int64_t>(1), static_cast<int64_t>(1));
      } else if (unknownTarget || mayEqual) {
        resVal =
            IntervalValue(static_cast<int64_t>(0), static_cast<int64_t>(1));
      } else {
        resVal =
            IntervalValue(static_cast<int64_t>(0), static_cast<int64_t>(0));
      }
      break;
    case llvm::CmpInst::ICMP_NE:
      if (mustEqual) {
        resVal =
            IntervalValue(static_cast<int64_t>(0), static_cast<int64_t>(0));
      } else if (unknownTarget || mayEqual) {
        resVal =
            IntervalValue(static_cast<int64_t>(0), static_cast<int64_t>(1));
      } else {
        resVal =
            IntervalValue(static_cast<int64_t>(1), static_cast<int64_t>(1));
      }
      break;
    default:
      resVal = IntervalValue(static_cast<int64_t>(0), static_cast<int64_t>(1));
      break;
    }
    as[resId] = AbstractValue(resVal);
    return;
  }

  // Handle null pointer comparisons
  if (op0Id == AbstractState::NullPtr || op1Id == AbstractState::NullPtr) {
    IntervalValue resVal = (as[op0Id].equals(as[op1Id])) ? IntervalValue(1, 1)
                                                         : IntervalValue(0, 0);
    as[resId] = AbstractValue(resVal);
    return;
  }

  // Handle interval comparisons
  if (!as.inVarToValTable(op0Id))
    as[op0Id] = AbstractValue(IntervalValue::top());
  if (!as.inVarToValTable(op1Id))
    as[op1Id] = AbstractValue(IntervalValue::top());

  IntervalValue val0 = as[op0Id].getInterval();
  IntervalValue val1 = as[op1Id].getInterval();
  IntervalValue resVal;

  const auto predicate = cmp->getPredicate();
  const bool isUnsignedCmp = predicate == llvm::CmpInst::ICMP_UGT ||
                             predicate == llvm::CmpInst::ICMP_UGE ||
                             predicate == llvm::CmpInst::ICMP_ULT ||
                             predicate == llvm::CmpInst::ICMP_ULE;
  if (isUnsignedCmp && cmp->getOperand(0)->getType()->isIntegerTy()) {
    const unsigned bits = cmp->getOperand(0)->getType()->getIntegerBitWidth();
    const bool mayBeTrue =
        unsignedPredicatePossible(predicate, val0, val1, bits);
    const bool mayBeFalse = unsignedPredicatePossible(
        llvm::CmpInst::getInversePredicate(predicate), val0, val1, bits);
    if (!mayBeTrue)
      resVal = IntervalValue(0, 0);
    else if (!mayBeFalse)
      resVal = IntervalValue(1, 1);
    else
      resVal = IntervalValue(0, 1);
    as[resId] = AbstractValue(resVal);
    return;
  }

  // getIntNumeral() asserts on infinity; use conservative [0,1] when bounds
  // are unbounded
  if (val0.is_infinite() || val1.is_infinite()) {
    as[resId] = AbstractValue(IntervalValue(0, 1));
    return;
  }

  switch (cmp->getPredicate()) {
  case llvm::CmpInst::ICMP_EQ:
  case llvm::CmpInst::FCMP_OEQ:
  case llvm::CmpInst::FCMP_UEQ:
    if (val0.is_numeral() && val1.is_numeral() && val0.equals(val1)) {
      resVal = IntervalValue(1, 1);
    } else if (!val0.hasIntersect(val1)) {
      resVal = IntervalValue(0, 0);
    } else {
      resVal = IntervalValue(0, 1);
    }
    break;
  case llvm::CmpInst::ICMP_NE:
  case llvm::CmpInst::FCMP_ONE:
  case llvm::CmpInst::FCMP_UNE:
    if (val0.is_numeral() && val1.is_numeral() && !val0.equals(val1)) {
      resVal = IntervalValue(1, 1);
    } else if (val0.is_numeral() && val1.is_numeral() && val0.equals(val1)) {
      resVal = IntervalValue(0, 0);
    } else if (!val0.hasIntersect(val1)) {
      resVal = IntervalValue(1, 1);
    } else {
      resVal = IntervalValue(0, 1);
    }
    break;
  case llvm::CmpInst::ICMP_SLT:
  case llvm::CmpInst::ICMP_ULT:
  case llvm::CmpInst::FCMP_OLT:
  case llvm::CmpInst::FCMP_ULT:
    if (val0.ub().getIntNumeral() < val1.lb().getIntNumeral()) {
      resVal = IntervalValue(1, 1);
    } else if (val0.lb().getIntNumeral() >= val1.ub().getIntNumeral()) {
      resVal = IntervalValue(0, 0);
    } else {
      resVal = IntervalValue(0, 1);
    }
    break;
  case llvm::CmpInst::ICMP_SLE:
  case llvm::CmpInst::ICMP_ULE:
  case llvm::CmpInst::FCMP_OLE:
  case llvm::CmpInst::FCMP_ULE:
    if (val0.ub().getIntNumeral() <= val1.lb().getIntNumeral()) {
      resVal = IntervalValue(1, 1);
    } else if (val0.lb().getIntNumeral() > val1.ub().getIntNumeral()) {
      resVal = IntervalValue(0, 0);
    } else {
      resVal = IntervalValue(0, 1);
    }
    break;
  case llvm::CmpInst::ICMP_SGT:
  case llvm::CmpInst::ICMP_UGT:
  case llvm::CmpInst::FCMP_OGT:
  case llvm::CmpInst::FCMP_UGT:
    if (val0.lb().getIntNumeral() > val1.ub().getIntNumeral()) {
      resVal = IntervalValue(1, 1);
    } else if (val0.ub().getIntNumeral() <= val1.lb().getIntNumeral()) {
      resVal = IntervalValue(0, 0);
    } else {
      resVal = IntervalValue(0, 1);
    }
    break;
  case llvm::CmpInst::ICMP_SGE:
  case llvm::CmpInst::ICMP_UGE:
  case llvm::CmpInst::FCMP_OGE:
  case llvm::CmpInst::FCMP_UGE:
    if (val0.lb().getIntNumeral() >= val1.ub().getIntNumeral()) {
      resVal = IntervalValue(1, 1);
    } else if (val0.ub().getIntNumeral() < val1.lb().getIntNumeral()) {
      resVal = IntervalValue(0, 0);
    } else {
      resVal = IntervalValue(0, 1);
    }
    break;
  case llvm::CmpInst::FCMP_FALSE:
    resVal = IntervalValue(0, 0);
    break;
  case llvm::CmpInst::FCMP_TRUE:
    resVal = IntervalValue(1, 1);
    break;
  default:
    resVal = IntervalValue(0, 1);
    break;
  }

  as[resId] = AbstractValue(resVal);
}

void AbstractInterpretation::updateStateOnLoad(const llvm::LoadInst *load) {
  uint32_t lhsId = getValueId(load);
  uint32_t ptrId = getValueId(load->getPointerOperand());

  AbstractState &as = stateStore_.getOrCreate(load);
  materializeConstantValue(as, load->getPointerOperand(), ptrId);
  AbstractValue loadedVal = as.loadValue(ptrId);
  if (stateStore_.layout() == StateLayout::Sparse) {
    AbstractValue svfgVal = getSVFGSparseLoadValue(load);
    const bool stateBottom =
        loadedVal.getInterval().isBottom() && loadedVal.getAddrs().isBottom();
    const bool svfgBottom =
        svfgVal.getInterval().isBottom() && svfgVal.getAddrs().isBottom();
    if (stateBottom && !svfgBottom) {
      loadedVal = svfgVal;
    }
  }

  as[lhsId] = loadedVal;
}

void AbstractInterpretation::updateStateOnStore(const llvm::StoreInst *store) {
  uint32_t ptrId = getValueId(store->getPointerOperand());
  uint32_t valId = getValueId(store->getValueOperand());

  AbstractState &as = stateStore_.getOrCreate(store);
  materializeConstantValue(as, store->getPointerOperand(), ptrId);
  materializeConstantValue(as, store->getValueOperand(), valId);
  if (stateStore_.layout() == StateLayout::Sparse) {
    auto it = as.getVarToVal().find(valId);
    sparseObjectDefs_[store] =
        it != as.getVarToVal().end()
            ? it->second
            : unknownValueForType(store->getValueOperand()->getType());
  }

  // A store is a strong update only when the pointer names a single,
  // non-summary object; otherwise the new value has to be joined in. This is
  // SVF's isStrongUpdate rule (AbstractStateManager.cpp).
  AbstractState::StoreKind kind = AbstractState::StoreKind::Weak;
  if (as.inVarToAddrsTable(ptrId) &&
      isStrongUpdateForStore(as[ptrId].getAddrs(), as))
    kind = AbstractState::StoreKind::Strong;

  as.storeValue(ptrId, valId, kind);
}

void AbstractInterpretation::updateStateOnCast(const llvm::CastInst *cast) {
  uint32_t lhsId = getValueId(cast);
  uint32_t opId = getValueId(cast->getOperand(0));
  AbstractState &as = stateStore_.getOrCreate(cast);
  materializeConstantValue(as, cast->getOperand(0), opId);

  if (as.inVarToAddrsTable(opId)) {
    // Pointer cast - preserve address
    as[lhsId] = as[opId];
    return;
  }

  if (!as.inVarToValTable(opId)) {
    as[lhsId] = AbstractValue(IntervalValue::top());
    return;
  }

  IntervalValue srcVal = as[opId].getInterval();
  if (srcVal.isBottom()) {
    as[lhsId] = AbstractValue(IntervalValue::bottom());
    return;
  }

  llvm::Type *srcType = cast->getOperand(0)->getType();
  llvm::Type *dstType = cast->getDestTy();

  // Helper function for zero extension (matches SVF's getZExtValue)
  auto getZExtValue = [&](const IntervalValue &val,
                          llvm::Type *srcType) -> IntervalValue {
    if (!srcType->isIntegerTy())
      return IntervalValue::top();

    unsigned bits = srcType->getIntegerBitWidth();
    if (val.is_infinite()) {
      if (bits >= 64)
        return IntervalValue::top();
      const uint64_t upper = (uint64_t{1} << bits) - 1;
      return IntervalValue(0, static_cast<int64_t>(upper));
    }

    if (val.is_numeral()) {
      int64_t numVal = val.getIntNumeral();
      if (bits == 8) {
        int8_t signed_i8_value = static_cast<int8_t>(numVal);
        uint32_t unsigned_value = static_cast<uint8_t>(signed_i8_value);
        return IntervalValue(unsigned_value, unsigned_value);
      } else if (bits == 16) {
        int16_t signed_i16_value = static_cast<int16_t>(numVal);
        uint32_t unsigned_value = static_cast<uint16_t>(signed_i16_value);
        return IntervalValue(unsigned_value, unsigned_value);
      } else if (bits == 32) {
        int32_t signed_i32_value = static_cast<int32_t>(numVal);
        uint32_t unsigned_value = static_cast<uint32_t>(signed_i32_value);
        return IntervalValue(unsigned_value, unsigned_value);
      } else if (bits == 64) {
        int64_t signed_i64_value = static_cast<int64_t>(numVal);
        return IntervalValue(signed_i64_value, signed_i64_value);
      }
    }

    if (bits < 64) {
      int64_t lb = val.lb().getIntNumeral();
      int64_t ub = val.ub().getIntNumeral();
      const int64_t modulus = int64_t{1} << bits;
      if (lb >= 0)
        return val;
      if (ub < 0)
        return IntervalValue(modulus + lb, modulus + ub);
      // The interval crosses the signed boundary; its unsigned image contains
      // both small non-negative values and values near 2^bits.
      return IntervalValue(0, modulus - 1);
    }
    return IntervalValue::top();
  };

  // Helper function for truncation (matches SVF's getTruncValue)
  auto getTruncValue = [&](const IntervalValue &val,
                           llvm::Type *dstType) -> IntervalValue {
    if (val.isBottom())
      return val;

    if (!dstType->isIntegerTy())
      return IntervalValue::top();

    const unsigned dstBits = dstType->getIntegerBitWidth();
    if (val.is_infinite()) {
      if (dstBits >= 64)
        return IntervalValue::top();
      const int64_t minSigned = -(int64_t{1} << (dstBits - 1));
      const int64_t maxSigned = (int64_t{1} << (dstBits - 1)) - 1;
      return IntervalValue(minSigned, maxSigned);
    }
    if (dstBits >= 64)
      return val;

    const int64_t intLb = val.lb().getIntNumeral();
    const int64_t intUb = val.ub().getIntNumeral();
    auto truncateNum = [&](int64_t value) {
      llvm::APInt source(64, static_cast<uint64_t>(value),
                         /*isSigned=*/false);
      return source.trunc(dstBits).sext(64).getSExtValue();
    };
    auto fullRange = [&]() {
      const int64_t minSigned = -(int64_t{1} << (dstBits - 1));
      const int64_t maxSigned = (int64_t{1} << (dstBits - 1)) - 1;
      return IntervalValue(minSigned, maxSigned);
    };

    if (val.is_numeral())
      return IntervalValue(truncateNum(intLb));

    const __int128 span =
        static_cast<__int128>(intUb) - static_cast<__int128>(intLb);
    const __int128 modulus = static_cast<__int128>(1) << dstBits;
    if (span + 1 >= modulus)
      return fullRange();

    const int64_t truncatedLb = truncateNum(intLb);
    const int64_t truncatedUb = truncateNum(intUb);
    if (truncatedLb <= truncatedUb)
      return IntervalValue(truncatedLb, truncatedUb);
    return fullRange();
  };

  auto getSExtValue = [&](const IntervalValue &val,
                          llvm::Type *srcType) -> IntervalValue {
    if (!srcType->isIntegerTy())
      return IntervalValue::top();
    const unsigned bits = srcType->getIntegerBitWidth();
    if (val.is_infinite()) {
      if (bits >= 64)
        return IntervalValue::top();
      const int64_t minSigned = -(int64_t{1} << (bits - 1));
      const int64_t maxSigned = (int64_t{1} << (bits - 1)) - 1;
      return IntervalValue(minSigned, maxSigned);
    }
    if (val.is_numeral()) {
      llvm::APInt source(bits, static_cast<uint64_t>(val.getIntNumeral()),
                         /*isSigned=*/false);
      return IntervalValue(source.sext(64).getSExtValue());
    }

    if (bits >= 64)
      return val;
    const int64_t minSigned = -(int64_t{1} << (bits - 1));
    const int64_t maxSigned = (int64_t{1} << (bits - 1)) - 1;
    if (val.lb().getIntNumeral() >= minSigned &&
        val.ub().getIntNumeral() <= maxSigned)
      return val;
    return IntervalValue(minSigned, maxSigned);
  };

  // Handle integer casts
  if (srcType->isIntegerTy() && dstType->isIntegerTy()) {
    if (cast->getOpcode() == llvm::Instruction::Trunc) {
      // Truncate: reduce precision with overflow detection
      as[lhsId] = AbstractValue(getTruncValue(srcVal, dstType));
    } else if (cast->getOpcode() == llvm::Instruction::ZExt) {
      // Zero extend: convert signed to unsigned properly
      as[lhsId] = AbstractValue(getZExtValue(srcVal, srcType));
    } else if (cast->getOpcode() == llvm::Instruction::SExt) {
      as[lhsId] = AbstractValue(getSExtValue(srcVal, srcType));
    } else {
      as[lhsId] = AbstractValue(srcVal);
    }
  } else if (cast->getOpcode() == llvm::Instruction::PtrToInt) {
    // Pointer to integer: lose precision
    as[lhsId] = AbstractValue(IntervalValue::top());
  } else if (cast->getOpcode() == llvm::Instruction::IntToPtr) {
    // Integer to pointer: create address (insert nullptr)
    // Similar to SVF: no explicit assignment, address will be set elsewhere
    as[lhsId] = AbstractValue(AddressValue());
  } else if (cast->getOpcode() == llvm::Instruction::BitCast) {
    // Bitcast: preserve value
    if (as.inVarToAddrsTable(opId)) {
      as[lhsId] = as[opId];
    } else {
      // Do nothing for non-address bitcasts (matches SVF behavior)
      as[lhsId] = AbstractValue(srcVal);
    }
  } else if (cast->getOpcode() == llvm::Instruction::FPToSI ||
             cast->getOpcode() == llvm::Instruction::FPToUI ||
             cast->getOpcode() == llvm::Instruction::SIToFP ||
             cast->getOpcode() == llvm::Instruction::UIToFP) {
    // Floating point conversions: preserve interval (matches SVF)
    as[lhsId] = AbstractValue(srcVal);
  } else if (cast->getOpcode() == llvm::Instruction::FPTrunc ||
             cast->getOpcode() == llvm::Instruction::FPExt) {
    // Floating point truncation/extension: preserve interval (matches SVF)
    as[lhsId] = AbstractValue(srcVal);
  } else {
    // Other casts: preserve value
    as[lhsId] = AbstractValue(srcVal);
  }
}

void AbstractInterpretation::updateStateOnCall(const llvm::CallBase *call) {
  std::vector<const llvm::Function *> callees = getCallees(call);
  if (callees.empty()) {
    if (const llvm::Function *direct = call->getCalledFunction()) {
      callees.push_back(direct);
    } else {
      return;
    }
  }

  std::vector<const llvm::Function *> extCallees;
  std::vector<const llvm::Function *> intCallees;
  for (const llvm::Function *callee : callees) {
    if (!callee)
      continue;
    if (callee->isDeclaration()) {
      extCallees.push_back(callee);
    } else {
      intCallees.push_back(callee);
    }
  }

  if (!stateStore_.contains(call))
    return;

  AbstractState &callSlot = stateStore_.getOrCreate(call);
  const AbstractState baseState = callSlot;
  AbstractState joinedState = baseState;
  bool hasJoinedState = false;

  if (!extCallees.empty()) {
    callSlot = baseState;
    for (const llvm::Use &arg : call->args())
      materializeConstantValue(callSlot, arg.get(), getValueId(arg.get()));
    handleExtCall(call, extCallees);
    joinedState = callSlot;
    hasJoinedState = true;
  }

  if (!intCallees.empty()) {
    callSlot = baseState;
    handleFunCall(call, intCallees);
    if (hasJoinedState) {
      joinedState.joinWith(callSlot);
    } else {
      joinedState = callSlot;
      hasJoinedState = true;
    }
  }

  callSlot = hasJoinedState ? joinedState : baseState;
}

void AbstractInterpretation::updateStateOnRet(const llvm::ReturnInst *ret) {
  if (ret->getReturnValue()) {
    uint32_t retId = getValueId(ret->getReturnValue());
    AbstractState &as = stateStore_.getOrCreate(ret);
    stateStore_.pin(ret);
    materializeConstantValue(as, ret->getReturnValue(), retId);
    stateStore_.getOrCreate(ret)[returnSlotId(ret)] = as[retId];
  }
}

void AbstractInterpretation::updateStateOnCallPE(const llvm::CallBase *call,
                                                 const llvm::Function *callee,
                                                 AbstractState &callState) {
  if (!callee || callee->isDeclaration())
    return;

  for (unsigned i = 0; i < call->arg_size() && i < callee->arg_size(); ++i) {
    const llvm::Value *argOp = call->getArgOperand(i);
    uint32_t argId = getValueId(argOp);
    uint32_t paramId = getValueId(callee->getArg(i));
    materializeConstantValue(callState, argOp, argId);
    AbstractValue argVal;
    bool hasArgVal = false;

    auto argIt = callState._varToAbsVal.find(argId);
    if (argIt != callState._varToAbsVal.end()) {
      argVal = argIt->second;
      hasArgVal = true;
    }

    if (!hasArgVal) {
      if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(argOp)) {
        uint32_t basePtrId = getValueId(load->getPointerOperand());
        argVal = callState.loadValue(basePtrId);
        if (!argVal.getAddrs().isBottom() || !argVal.getInterval().isBottom()) {
          hasArgVal = true;
        }
      }
    }

    if (!hasArgVal && llvm::isa<llvm::ConstantPointerNull>(argOp)) {
      argVal = AbstractValue(AddressValue(NullMemAddr));
      hasArgVal = true;
    }

    if (hasArgVal) {
      callState[paramId] = argVal;
    }
  }
}

void AbstractInterpretation::updateStateOnRetPE(const llvm::ReturnInst *ret,
                                                const llvm::CallBase *call,
                                                AbstractState &callerState) {
  if (!ret->getReturnValue() || !call)
    return;

  uint32_t retId = getValueId(ret->getReturnValue());
  uint32_t callId = getValueId(call);

  AbstractState &calleeState = stateStore_.getOrCreate(ret);
  auto retIt = calleeState._varToAbsVal.find(retId);
  if (retIt != calleeState._varToAbsVal.end()) {
    if (callerState._varToAbsVal.count(callId) == 0) {
      callerState[callId] = retIt->second;
    } else {
      callerState[callId].join_with(retIt->second);
    }
  }
}

void AbstractInterpretation::updateStateOnCopy(const llvm::Value *dst,
                                               const llvm::Value *src) {
  const llvm::Instruction *curInst = getCurrentInstruction();
  if (!curInst)
    return;

  uint32_t dstId = getValueId(dst);
  uint32_t srcId = getValueId(src);

  AbstractState &as = stateStore_.getOrCreate(curInst);
  materializeConstantValue(as, src, srcId);

  if (as.inVarToValTable(srcId)) {
    as[dstId] = as[srcId];
  } else if (as.inVarToAddrsTable(srcId)) {
    as[dstId] = as[srcId];
  } else {
    as[dstId] = unknownValueForType(dst->getType());
  }
}

const llvm::Instruction *AbstractInterpretation::getCurrentInstruction() const {
  return this->currentInstruction_;
}

void AbstractInterpretation::updateStateOnGep(
    const llvm::GetElementPtrInst *gep) {
  uint32_t lhsId = getValueId(gep);
  uint32_t ptrId = getValueId(gep->getPointerOperand());

  AbstractState &as = stateStore_.getOrCreate(gep);
  materializeConstantValue(as, gep->getPointerOperand(), ptrId);

  // Use getElementIndex (like SVF) instead of getByteOffset: getElementIndex
  // clamps with meet_with([0, MaxFieldLimit]) so never returns infinity,
  // avoiding getIntNumeral() assertion when offset has infinite bounds.
  IntervalValue offset = as.getElementIndex(gep);

  if (as.inVarToAddrsTable(ptrId)) {
    AddressValue gepAddrs = as.getGepObjAddrs(ptrId, offset, gep);
    as[lhsId] = AbstractValue(gepAddrs);
  } else {
    as[lhsId] = AbstractValue(AddressValue());
  }
}

void AbstractInterpretation::updateStateOnSelect(
    const llvm::SelectInst *select) {
  uint32_t lhsId = getValueId(select);
  uint32_t condId = getValueId(select->getCondition());
  uint32_t trueId = getValueId(select->getTrueValue());
  uint32_t falseId = getValueId(select->getFalseValue());

  AbstractState &as = stateStore_.getOrCreate(select);

  materializeConstantValue(as, select->getCondition(), condId);
  materializeConstantValue(as, select->getTrueValue(), trueId);
  materializeConstantValue(as, select->getFalseValue(), falseId);

  auto hasValue = [&](uint32_t id) {
    return as.inVarToValTable(id) || as.inVarToAddrsTable(id);
  };
  auto unknown = [&]() { return unknownValueForType(select->getType()); };

  if (as.inVarToValTable(condId)) {
    IntervalValue cond = as[condId].getInterval();
    if (cond.is_numeral()) {
      uint32_t selectedId = cond.getIntNumeral() != 0 ? trueId : falseId;
      as[lhsId] = hasValue(selectedId) ? as[selectedId] : unknown();
      return;
    }
  }

  // A missing arm is unknown, not bottom: silently dropping it can make a
  // later branch unreachable or turn the selected pointer into an uninitialized
  // value. Join both arms when both are available.
  if (!hasValue(trueId) || !hasValue(falseId)) {
    as[lhsId] = unknown();
    return;
  }
  AbstractValue result = as[trueId];
  result.join_with(as[falseId]);
  as[lhsId] = result;
}

void AbstractInterpretation::updateStateOnPhi(const llvm::PHINode *phi) {
  uint32_t resId = getValueId(phi);
  AbstractState &as = stateStore_.getOrCreate(phi);
  AbstractValue rhs;
  bool first = true;
  bool incomplete = false;

  for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
    llvm::Value *val = phi->getIncomingValue(i);
    uint32_t curId = getValueId(val);
    const llvm::BasicBlock *opBB = phi->getIncomingBlock(i);

    // Get state from the predecessor block (stored at terminator or first inst)
    AbstractState opAs;
    bool foundState = false;

    const llvm::Instruction *term = opBB->getTerminator();
    if (const AbstractState *termState = stateStore_.find(term)) {
      opAs = *termState;
      foundState = true;
    } else if (!opBB->empty()) {
      if (const AbstractState *frontState = stateStore_.find(&opBB->front())) {
        opAs = *frontState;
        foundState = true;
      }
    }

    if (foundState) {
      // Check if this edge is feasible (for conditional branches and switches)
      bool edgeFeasible = true;

      if (const llvm::BranchInst *branch =
              llvm::dyn_cast<llvm::BranchInst>(term)) {
        if (branch->isConditional() &&
            branch->getSuccessor(0) != branch->getSuccessor(1)) {
          // Determine which successor this edge leads to
          bool isTrueEdge = branch->getSuccessor(0) == phi->getParent();
          AbstractState testState = opAs;
          // Refinement would otherwise write through to the def-site table.
          testState.materializeVarTable();
          edgeFeasible = isBranchFeasible(branch, testState, isTrueEdge);
          if (edgeFeasible) {
            opAs = testState;
          }
        }
      } else if (const llvm::SwitchInst *switchInst =
                     llvm::dyn_cast<llvm::SwitchInst>(term)) {
        AbstractState testState = opAs;
        testState.materializeVarTable();
        edgeFeasible =
            isSwitchSuccessorFeasible(switchInst, phi->getParent(), testState);
        if (edgeFeasible)
          opAs = testState;
      }

      if (!edgeFeasible) {
        continue; // Skip infeasible edge
      }

      materializeConstantValue(opAs, val, curId);

      if (opAs.inVarToValTable(curId) || opAs.inVarToAddrsTable(curId)) {
        if (first) {
          rhs = opAs[curId];
          first = false;
        } else {
          rhs.join_with(opAs[curId]);
        }
      } else if (llvm::isa<llvm::UndefValue>(val) ||
                 llvm::isa<llvm::PoisonValue>(val)) {
        // An explicit undef/poison value is unknown. A missing materialized
        // value can instead be a normal transient state during loop fixpoint
        // iteration and must not permanently widen the PHI.
        incomplete = true;
      }
    }
  }

  if (incomplete) {
    as[resId] = unknownValueForType(phi->getType());
  } else if (!first) {
    as[resId] = rhs;
  } else {
    as[resId] = unknownValueForType(phi->getType());
  }
}

void AbstractInterpretation::updateStateOnExtractValue(
    const llvm::ExtractValueInst *extract) {
  uint32_t lhsId = getValueId(extract);
  const llvm::Value *aggregate = extract->getAggregateOperand();
  uint32_t aggId = getValueId(aggregate);

  AbstractState &as = stateStore_.getOrCreate(extract);

  llvm::SmallVector<unsigned, 4> indices(extract->idx_begin(),
                                         extract->idx_end());
  if (const llvm::Value *selected =
          resolveAggregateElement(aggregate, indices)) {
    uint32_t selectedId = getValueId(selected);
    materializeConstantValue(as, selected, selectedId);
    if (as.inVarToValTable(selectedId) || as.inVarToAddrsTable(selectedId)) {
      as[lhsId] = as[selectedId];
      return;
    }
  }

  // ExtractValue extracts a field from an aggregate value
  // For abstract interpretation, we conservatively propagate the value
  // In SVF, this would be a CopyStmt from aggregate field to result
  if (as.inVarToValTable(aggId)) {
    updateStateOnCopy(extract, aggregate);
  } else if (as.inVarToAddrsTable(aggId)) {
    updateStateOnCopy(extract, aggregate);
  } else {
    as[lhsId] = unknownValueForType(extract->getType());
  }
}

void AbstractInterpretation::updateStateOnInsertValue(
    const llvm::InsertValueInst *insert) {
  uint32_t lhsId = getValueId(insert);
  uint32_t aggId = getValueId(insert->getAggregateOperand());
  uint32_t valId = getValueId(insert->getInsertedValueOperand());

  AbstractState &as = stateStore_.getOrCreate(insert);
  materializeConstantValue(as, insert->getAggregateOperand(), aggId);
  materializeConstantValue(as, insert->getInsertedValueOperand(), valId);

  // InsertValue inserts a value into an aggregate and returns the new aggregate
  // For abstract interpretation, we join the aggregate and inserted value
  // In SVF, this would be a CopyStmt that combines values
  AbstractValue result;
  bool hasValue = false;

  if (as.inVarToValTable(aggId) || as.inVarToAddrsTable(aggId)) {
    updateStateOnCopy(insert, insert->getAggregateOperand());
    result = as[lhsId];
    hasValue = true;
  }

  if (as.inVarToValTable(valId) || as.inVarToAddrsTable(valId)) {
    if (hasValue) {
      result.join_with(as[valId]);
    } else {
      result = as[valId];
      hasValue = true;
    }
  }

  if (hasValue) {
    as[lhsId] = result;
  } else {
    as[lhsId] = unknownValueForType(insert->getType());
  }
}

void AbstractInterpretation::updateStateOnExtractElement(
    const llvm::ExtractElementInst *extract) {
  uint32_t lhsId = getValueId(extract);
  const llvm::Value *vector = extract->getVectorOperand();
  uint32_t vecId = getValueId(vector);

  AbstractState &as = stateStore_.getOrCreate(extract);

  if (const auto *index =
          llvm::dyn_cast<llvm::ConstantInt>(extract->getIndexOperand())) {
    unsigned lane = static_cast<unsigned>(index->getZExtValue());
    if (const llvm::Value *selected = resolveAggregateElement(vector, {lane})) {
      uint32_t selectedId = getValueId(selected);
      materializeConstantValue(as, selected, selectedId);
      if (as.inVarToValTable(selectedId) || as.inVarToAddrsTable(selectedId)) {
        as[lhsId] = as[selectedId];
        return;
      }
    }
  }

  // ExtractElement extracts an element from a vector
  // For abstract interpretation, we propagate the vector's abstract value
  // In SVF, this would be a CopyStmt from vector element to result
  if (as.inVarToValTable(vecId)) {
    updateStateOnCopy(extract, vector);
  } else if (as.inVarToAddrsTable(vecId)) {
    updateStateOnCopy(extract, vector);
  } else {
    as[lhsId] = unknownValueForType(extract->getType());
  }
}

void AbstractInterpretation::updateStateOnInsertElement(
    const llvm::InsertElementInst *insert) {
  uint32_t lhsId = getValueId(insert);
  uint32_t vecId = getValueId(insert->getOperand(0));
  uint32_t valId = getValueId(insert->getOperand(2));

  AbstractState &as = stateStore_.getOrCreate(insert);
  materializeConstantValue(as, insert->getOperand(0), vecId);
  materializeConstantValue(as, insert->getOperand(2), valId);

  // InsertElement inserts a value into a vector and returns the new vector
  // For abstract interpretation, we join the vector and inserted value
  // In SVF, this would be a CopyStmt that combines values
  AbstractValue result;
  bool hasValue = false;

  if (as.inVarToValTable(vecId) || as.inVarToAddrsTable(vecId)) {
    updateStateOnCopy(insert, insert->getOperand(0));
    result = as[lhsId];
    hasValue = true;
  }

  if (as.inVarToValTable(valId) || as.inVarToAddrsTable(valId)) {
    if (hasValue) {
      result.join_with(as[valId]);
    } else {
      result = as[valId];
      hasValue = true;
    }
  }

  if (hasValue) {
    as[lhsId] = result;
  } else {
    as[lhsId] = unknownValueForType(insert->getType());
  }
}

void AbstractInterpretation::updateStateOnShuffleVector(
    const llvm::ShuffleVectorInst *shuffle) {
  uint32_t lhsId = getValueId(shuffle);
  uint32_t v1Id = getValueId(shuffle->getOperand(0));
  uint32_t v2Id = getValueId(shuffle->getOperand(1));

  AbstractState &as = stateStore_.getOrCreate(shuffle);
  materializeConstantValue(as, shuffle->getOperand(0), v1Id);
  materializeConstantValue(as, shuffle->getOperand(1), v2Id);

  const auto *firstConstant =
      llvm::dyn_cast<llvm::Constant>(shuffle->getOperand(0));
  const auto *secondConstant =
      llvm::dyn_cast<llvm::Constant>(shuffle->getOperand(1));
  if (firstConstant && secondConstant) {
    AbstractValue resolved;
    bool firstLane = true;
    bool complete = true;
    const auto *sourceType = llvm::dyn_cast<llvm::FixedVectorType>(
        shuffle->getOperand(0)->getType());
    const unsigned sourceWidth = sourceType ? sourceType->getNumElements() : 0;
    for (int mask : shuffle->getShuffleMask()) {
      if (mask < 0)
        continue;
      const auto *source = static_cast<unsigned>(mask) < sourceWidth
                               ? firstConstant
                               : secondConstant;
      const unsigned lane = static_cast<unsigned>(mask) % sourceWidth;
      const llvm::Constant *element = source->getAggregateElement(lane);
      if (!element) {
        complete = false;
        break;
      }
      uint32_t elementId = getValueId(element);
      materializeConstantValue(as, element, elementId);
      if (!as.inVarToValTable(elementId) && !as.inVarToAddrsTable(elementId)) {
        complete = false;
        break;
      }
      if (firstLane) {
        resolved = as[elementId];
        firstLane = false;
      } else {
        resolved.join_with(as[elementId]);
      }
    }
    if (complete && !firstLane) {
      as[lhsId] = resolved;
      return;
    }
  }

  // ShuffleVector shuffles elements from two vectors into a new vector
  // For abstract interpretation, we join both vectors' abstract values
  // In SVF, this would be a CopyStmt that combines values from both sources
  AbstractValue result;
  bool hasValue = false;

  if (as.inVarToValTable(v1Id) || as.inVarToAddrsTable(v1Id)) {
    updateStateOnCopy(shuffle, shuffle->getOperand(0));
    result = as[lhsId];
    hasValue = true;
  }

  if (as.inVarToValTable(v2Id) || as.inVarToAddrsTable(v2Id)) {
    if (hasValue) {
      result.join_with(as[v2Id]);
    } else {
      result = as[v2Id];
      hasValue = true;
    }
  }

  if (hasValue) {
    as[lhsId] = result;
  } else {
    as[lhsId] = unknownValueForType(shuffle->getType());
  }
}

uint32_t AbstractInterpretation::getValueId(const llvm::Value *val) {
  if (!val)
    return 0;

  if (llvm::isa<llvm::ConstantPointerNull>(val)) {
    idToValueMap_[0] = val;
    return 0;
  }

  // Check if we already have an ID for this value
  auto it = valueToIdMap_.find(val);
  if (it != valueToIdMap_.end()) {
    return it->second;
  }

  // Assign a new stable ID
  uint32_t id = nextValueId_++;
  valueToIdMap_[val] = id;
  idToValueMap_[id] = val;

  return id;
}

const llvm::Value *AbstractInterpretation::getValueFromId(uint32_t id) const {
  auto it = idToValueMap_.find(id);
  if (it != idToValueMap_.end()) {
    return it->second;
  }
  return nullptr;
}

bool AbstractInterpretation::isExtCall(const llvm::CallBase *callNode) {
  for (const llvm::Function *callee : getCallees(callNode)) {
    if (callee && callee->isDeclaration()) {
      return true;
    }
  }
  return false;
}

void AbstractInterpretation::handleExtCall(
    const llvm::CallBase *callNode,
    const std::vector<const llvm::Function *> &callees) {
  const AbstractState *traceIt = stateStore_.find(callNode);
  if (!traceIt)
    return;

  callSiteStack.push_back(callNode);
  AbstractState &callSlot = stateStore_.getOrCreate(callNode);
  const AbstractState baseState = callSlot;
  AbstractState joinedState = baseState;
  bool hasJoinedState = false;

  if (callees.empty()) {
    if (const llvm::Function *direct = callNode->getCalledFunction()) {
      callSlot = baseState;
      utils->handleExtAPI(callNode, direct);
      joinedState = callSlot;
      hasJoinedState = true;
    }
  } else {
    for (const llvm::Function *callee : callees) {
      if (!callee || !callee->isDeclaration())
        continue;
      callSlot = baseState;
      utils->handleExtAPI(callNode, callee);
      if (hasJoinedState) {
        joinedState.joinWith(callSlot);
      } else {
        joinedState = callSlot;
        hasJoinedState = true;
      }
    }
  }

  if (hasJoinedState) {
    callSlot = joinedState;
  } else {
    callSlot = baseState;
  }

  for (auto &detector : detectors) {
    detector->handleStubFunctions(callNode);
  }
  callSiteStack.pop_back();
}

bool AbstractInterpretation::isRecursiveFun(const llvm::Function *fun) const {
  // Use the recursiveFuns_ set populated by initCallGraphSCC()
  return recursiveFuns_.count(fun) > 0;
}

bool AbstractInterpretation::isRecursiveCall(const llvm::CallBase *callNode) {
  const llvm::Function *callfun = callNode->getCalledFunction();
  if (!callfun)
    return false;
  return isRecursiveFun(callfun);
}

void AbstractInterpretation::handleCallSite(const llvm::CallBase *call) {
  updateStateOnCall(call);
}

void AbstractInterpretation::handleFunCall(
    const llvm::CallBase *callNode,
    const std::vector<const llvm::Function *> &callees) {
  if (callees.empty())
    return;

  if (!stateStore_.contains(callNode))
    return;
  AbstractState &callSlot = stateStore_.getOrCreate(callNode);

  callSiteStack.push_back(callNode);

  bool gotReturn = false;
  AbstractValue joinedReturn;
  bool gotPostState = false;
  AbstractState joinedPostState;

  for (const llvm::Function *callee : callees) {
    if (!callee || callee->isDeclaration())
      continue;

    // Build call state and map actuals to formals.
    AbstractState callState = callSlot;
    // Formal parameters are activation-specific. A recursive callee reuses the
    // same parameter value IDs, so writing them into the run-wide def-site
    // table would make an inner activation overwrite its caller's arguments.
    callState.materializeVarTable();
    updateStateOnCallPE(callNode, callee, callState);

    const llvm::BasicBlock &entryBlock = callee->getEntryBlock();
    if (!entryBlock.empty()) {
      const llvm::Instruction *entryInst = &entryBlock.front();
      if (const AbstractState *entryIt = stateStore_.find(entryInst)) {
        AbstractState merged = *entryIt;
        merged.joinWith(callState);
        stateStore_.set(entryInst, merged);
      } else {
        stateStore_.set(entryInst, callState);
      }
    }

    // Recursive callsite inside SCC: do not recurse.
    // Instead, feed argument state to the callee entry and consume the current
    // callee summary (if any), matching SVF's interprocedural cycle feedback.
    if (isRecursiveCallSite(callNode, callee)) {
      if (recursionMode_ == TOP && isRecursiveFun(callee)) {
        recursiveCallPass(callNode, callee);
        continue;
      }

      AbstractValue calleeReturn;
      bool calleeHasReturn = collectCalleeReturnValue(callee, calleeReturn);
      if (calleeHasReturn) {
        if (!gotReturn) {
          joinedReturn = calleeReturn;
          gotReturn = true;
        } else {
          joinedReturn.join_with(calleeReturn);
        }
      }
      continue;
    }

    // Analyze callee.
    if (isRecursiveFun(callee)) {
      handleRecursiveSCC(callee);
    } else {
      handleFunction(callee);
    }

    // Propagate all concrete return edges to caller state (RetPE equivalent).
    for (const llvm::BasicBlock &bb : *callee) {
      const auto *retInst =
          llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator());
      if (!retInst || !retInst->getReturnValue())
        continue;
      updateStateOnRetPE(retInst, callNode, callSlot);
    }

    for (const llvm::BasicBlock &bb : *callee) {
      const auto *retInst =
          llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator());
      if (!retInst)
        continue;
      const AbstractState *retIt = stateStore_.find(retInst);
      if (!retIt)
        continue;

      AbstractState postState = *retIt;
      if (!callNode->getType()->isVoidTy()) {
        auto rvIt = postState._varToAbsVal.find(returnSlotId(retInst));
        if (rvIt != postState._varToAbsVal.end()) {
          postState[getValueId(callNode)] = rvIt->second;
        }
      }

      if (!gotPostState) {
        joinedPostState = postState;
        gotPostState = true;
      } else {
        joinedPostState.joinWith(postState);
      }
    }

    AbstractValue calleeReturn;
    bool calleeHasReturn = collectCalleeReturnValue(callee, calleeReturn);

    if (recursionMode_ == TOP && isRecursiveFun(callee)) {
      setTopToObjInRecursion(callNode, callee);
      calleeReturn = AbstractValue(IntervalValue::top());
      calleeHasReturn = true;
    }

    if (calleeHasReturn) {
      if (!gotReturn) {
        joinedReturn = calleeReturn;
        gotReturn = true;
      } else {
        joinedReturn.join_with(calleeReturn);
      }
    }
  }

  if (!callSiteStack.empty() && callSiteStack.back() == callNode) {
    if (AbstractState *it = stateStore_.find(callNode)) {
      if (gotPostState) {
        *it = joinedPostState;
      }
      if (callNode->getType()->isVoidTy() == false && gotReturn) {
        uint32_t lhsId = getValueId(callNode);
        (*it)[lhsId] = joinedReturn;
      }
    }
  }

  callSiteStack.pop_back();
}

bool AbstractInterpretation::collectCalleeReturnValue(
    const llvm::Function *callee, AbstractValue &joinedReturn) const {
  bool calleeHasReturn = false;
  for (const llvm::BasicBlock &bb : *callee) {
    const auto *ret = llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator());
    if (!ret || !ret->getReturnValue())
      continue;
    const AbstractState *retIt = stateStore_.find(ret);
    if (!retIt)
      continue;
    auto rvIt = retIt->_varToAbsVal.find(returnSlotId(ret));
    if (rvIt == retIt->_varToAbsVal.end())
      continue;
    if (!calleeHasReturn) {
      joinedReturn = rvIt->second;
      calleeHasReturn = true;
    } else {
      joinedReturn.join_with(rvIt->second);
    }
  }
  return calleeHasReturn;
}

void AbstractInterpretation::handleRecursiveSCC(const llvm::Function *seed) {
  auto idIt = recursiveSccIdMap_.find(seed);
  if (idIt == recursiveSccIdMap_.end()) {
    handleFunction(seed);
    return;
  }
  auto membersIt = recursiveSccMembers_.find(idIt->second);
  if (membersIt == recursiveSccMembers_.end() || membersIt->second.empty()) {
    handleFunction(seed);
    return;
  }

  // Function-level WTOs do not capture interprocedural recursive cycles.
  // Iterate over SCC entry summaries here so recursive summary propagation
  // converges, while each function still uses its own WTO for intra-procedural
  // cycles.
  if (recursionMode_ == TOP) {
    for (const llvm::Function *func : membersIt->second) {
      if (!func || func->isDeclaration() || func->empty())
        continue;
      handleFunction(func);
    }
    return;
  }

  bool increasing = true;
  for (uint32_t iter = 0;; ++iter) {
    if (timeBudgetExhausted())
      break;
    bool stable = true;

    for (const llvm::Function *func : membersIt->second) {
      if (!func || func->isDeclaration() || func->empty())
        continue;

      const llvm::Instruction *entryInst = &func->getEntryBlock().front();
      AbstractState prevEntry;
      bool hadPrevEntry = false;
      if (const AbstractState *beforeIt = stateStore_.find(entryInst)) {
        prevEntry = *beforeIt;
        hadPrevEntry = true;
      }

      handleFunction(func);

      if (!stateStore_.contains(entryInst))
        continue;
      AbstractState &afterState = stateStore_.getOrCreate(entryInst);

      if (iter >= widenDelay_) {
        if (increasing) {
          if (hadPrevEntry) {
            // Work on copies: a store-owned state must be written through the
            // store so that the def-site table stays in sync.
            AbstractState curState = afterState;
            if (stateStore_.usesDefSiteVars()) {
              prevEntry.materializeVarTable();
              curState.materializeVarTable();
            }
            AbstractState widened = prevEntry.widening(curState);
            stateStore_.set(entryInst, widened);
            if (widened != prevEntry) {
              stable = false;
            }
          } else {
            stable = false;
          }
        } else {
          if (!shouldApplyNarrowing(func)) {
            continue;
          }
          if (hadPrevEntry) {
            AbstractState curState = afterState;
            if (stateStore_.usesDefSiteVars()) {
              prevEntry.materializeVarTable();
              curState.materializeVarTable();
            }
            AbstractState narrowed = prevEntry.narrowing(curState);
            stateStore_.set(entryInst, narrowed);
            if (narrowed != prevEntry) {
              stable = false;
            }
          } else {
            stable = false;
          }
        }
      } else if (!hadPrevEntry || afterState != prevEntry) {
        stable = false;
      }
    }

    if (iter < widenDelay_) {
      if (stable) {
        break;
      }
      continue;
    }

    if (increasing) {
      if (!stable) {
        continue;
      }
      if (shouldApplyNarrowing(seed)) {
        increasing = false;
        continue;
      }
      break;
    }

    if (stable) {
      break;
    }
  }
}

/// Check if a call is a recursive callsite (within same SCC, not entry call
/// from outside) This matches SVF's isRecursiveCallSite logic
bool AbstractInterpretation::isRecursiveCallSite(
    const llvm::CallBase *callNode, const llvm::Function *callee) const {
  // Only callsites to recursive functions can be recursive callsites.
  if (!callee || !isRecursiveFun(callee))
    return false;
  // If this call site is marked as non-recursive (entry call), it's not a
  // recursive callsite.
  return nonRecursiveCallSites_.find({callNode, callee}) ==
         nonRecursiveCallSites_.end();
}

bool AbstractInterpretation::skipRecursiveCall(const llvm::CallBase *callNode) {
  for (const llvm::Function *callee : getCallees(callNode)) {
    if (!callee)
      continue;
    if (!isRecursiveFun(callee))
      return false;
    if (!isRecursiveCallSite(callNode, callee))
      return false;
  }
  return true;
}

std::vector<const llvm::Function *>
AbstractInterpretation::getCallees(const llvm::CallBase *callNode) const {
  std::vector<const llvm::Function *> result;
  if (!callNode)
    return result;

  auto collectFunctionsFromValue =
      [&](const llvm::Value *value, std::set<const llvm::Function *> &targets,
          auto &&collectRef, std::set<const llvm::Value *> &visited) -> void {
    if (!value || !visited.insert(value).second)
      return;

    value = value->stripPointerCasts();
    if (const auto *fun = llvm::dyn_cast<llvm::Function>(value)) {
      targets.insert(fun);
      return;
    }

    if (const auto *ce = llvm::dyn_cast<llvm::ConstantExpr>(value)) {
      for (const llvm::Use &operand : ce->operands()) {
        collectRef(operand.get(), targets, collectRef, visited);
      }
      return;
    }

    if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(value)) {
      const llvm::Value *ptr = load->getPointerOperand()->stripPointerCasts();
      if (const auto *gv = llvm::dyn_cast<llvm::GlobalVariable>(ptr)) {
        if (gv->hasInitializer()) {
          collectRef(gv->getInitializer(), targets, collectRef, visited);
        }
      }
      for (const llvm::User *user : ptr->users()) {
        if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(user)) {
          if (store->getPointerOperand()->stripPointerCasts() == ptr) {
            collectRef(store->getValueOperand(), targets, collectRef, visited);
          }
        }
      }
      return;
    }

    if (const auto *phi = llvm::dyn_cast<llvm::PHINode>(value)) {
      for (const llvm::Value *incoming : phi->incoming_values()) {
        collectRef(incoming, targets, collectRef, visited);
      }
      return;
    }

    if (const auto *select = llvm::dyn_cast<llvm::SelectInst>(value)) {
      collectRef(select->getTrueValue(), targets, collectRef, visited);
      collectRef(select->getFalseValue(), targets, collectRef, visited);
    }
  };

  // Direct call.
  if (const llvm::Function *direct = callNode->getCalledFunction()) {
    result.push_back(direct);
    return result;
  }

  // Indirect call: collect all resolved targets from PTA call graph.
  if (!ptaReady_ || !pta_ || !pta_->pass)
    return result;

  const auto *cg = pta_->pass->getPTA()->getCallGraph();
  if (!cg)
    return result;

  std::set<const llvm::Function *> uniqueTargets;
  for (auto nodeIt = cg->begin(); nodeIt != cg->end(); ++nodeIt) {
    const auto *cgNode = *nodeIt;
    if (!cgNode || !cgNode->isIndirectCall())
      continue;
    auto *indCall = cgNode->getTargetFunPtr();
    if (!indCall)
      continue;
    if (indCall->getCallSite() != callNode)
      continue;

    for (const auto *resolvedNode : indCall->getResolvedNode()) {
      if (!resolvedNode || resolvedNode->isIndirectCall())
        continue;
      if (const auto *targetFun = resolvedNode->getTargetFun()) {
        const llvm::Function *calleeFunc = targetFun->getFunction();
        if (calleeFunc) {
          uniqueTargets.insert(calleeFunc);
        }
      }
    }

    for (auto succIt = cgNode->succ_begin(); succIt != cgNode->succ_end();
         ++succIt) {
      const auto *calleeNode = *succIt;
      if (!calleeNode || calleeNode->isIndirectCall())
        continue;
      if (const auto *targetFun = calleeNode->getTargetFun()) {
        const llvm::Function *calleeFunc = targetFun->getFunction();
        if (calleeFunc) {
          uniqueTargets.insert(calleeFunc);
        }
      }
    }
  }

  if (uniqueTargets.empty()) {
    if (const AbstractState *state = stateStore_.find(callNode)) {
      uint32_t calleeId =
          getValueIdStatic(callNode->getCalledOperand()->stripPointerCasts());
      auto calleeIt = state->_varToAbsVal.find(calleeId);
      if (calleeIt != state->_varToAbsVal.end() && calleeIt->second.isAddr()) {
        for (uint32_t addr : calleeIt->second.getAddrs()) {
          uint32_t objId = state->getIDFromAddr(addr);
          if (const auto *fun =
                  llvm::dyn_cast_or_null<llvm::Function>(getValueFromId(objId)))
            uniqueTargets.insert(fun);
        }
      }
    }
  }

  if (uniqueTargets.empty()) {
    std::set<const llvm::Value *> visited;
    collectFunctionsFromValue(callNode->getCalledOperand(), uniqueTargets,
                              collectFunctionsFromValue, visited);
  }

  result.assign(uniqueTargets.begin(), uniqueTargets.end());
  return result;
}

const llvm::Function *
AbstractInterpretation::getCallee(const llvm::CallBase *callNode) {
  std::vector<const llvm::Function *> callees = getCallees(callNode);
  if (callees.empty())
    return nullptr;
  return callees.front();
}

uint32_t
AbstractInterpretation::returnSlotId(const llvm::Instruction *ret) const {
  const auto it = retSlotIds_.find(ret);
  if (it != retSlotIds_.end())
    return it->second;
  const uint32_t id = nextValueId_++;
  retSlotIds_[ret] = id;
  valueToIdMap_[ret] = id;
  return id;
}

bool AbstractInterpretation::isLocalOfRecursiveFun(uint32_t objId) const {
  const llvm::Value *obj = getValueFromId(objId);
  if (!obj)
    return false;
  const llvm::Function *fun = nullptr;
  if (const auto *inst = llvm::dyn_cast<llvm::Instruction>(obj))
    fun = inst->getFunction();
  else
    fun = llvm::dyn_cast<llvm::Function>(obj);
  return fun && isRecursiveFun(fun);
}

bool AbstractInterpretation::isArrayBaseObject(uint32_t objId) const {
  const auto *alloca =
      llvm::dyn_cast_or_null<llvm::AllocaInst>(getValueFromId(objId));
  // A GEP field object has its own (synthetic) id and no defining value, so it
  // is not an array base; only the aggregate itself is a summary.
  return alloca && alloca->getAllocatedType()->isArrayTy();
}

bool AbstractInterpretation::isStrongUpdateForStore(
    const AddressValue &targets, const AbstractState &state) const {
  if (!state.allowsStrongUpdate(targets))
    return false;
  // The remaining SVF conditions need call-graph / type information.
  const uint32_t objId = AddressValue::getInternalID(*targets.begin());
  if (isLocalOfRecursiveFun(objId))
    return false;
  if (isArrayBaseObject(objId))
    return false;
  return true;
}

const std::unordered_set<uint32_t> *
AbstractInterpretation::cycleValueIds(const ICFGCycleWTO *cycle) {
  const auto cached = cycleValueIds_.find(cycle);
  if (cached != cycleValueIds_.end())
    return cached->second.empty() ? nullptr : &cached->second;

  std::unordered_set<uint32_t> ids;
  bool needsWholeTable = false;
  // Walk the cycle's components, descending into nested cycles: every
  // instruction defined in the cycle's blocks owns a value that may change
  // between iterations.
  auto collectBlock = [&](const llvm::BasicBlock *bb) {
    if (!bb)
      return;
    for (const llvm::Instruction &inst : *bb) {
      // A recursive call drags the callee's values into the fixpoint; they live
      // outside this cycle's blocks, so widen with the whole table instead of a
      // restricted snapshot.
      if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        if (const llvm::Function *callee = call->getCalledFunction()) {
          if (isRecursiveFun(callee))
            needsWholeTable = true;
        }
      }
      if (!inst.getType()->isVoidTy())
        ids.insert(getValueId(&inst));
    }
  };
  std::function<void(const ICFGWTOComp *)> visit =
      [&](const ICFGWTOComp *comp) {
        if (!comp)
          return;
        if (const auto *singleton =
                llvm::dyn_cast<lotus::analysis::ICFGSingletonWTO>(comp)) {
          collectBlock(singleton->getBlock());
          return;
        }
        if (const auto *nested =
                llvm::dyn_cast<lotus::analysis::ICFGCycleWTO>(comp)) {
          collectBlock(nested->getEntry());
          for (const ICFGWTOComp *inner : nested->getComponents())
            visit(inner);
        }
      };
  collectBlock(cycle->getEntry());
  visit(cycle);
  if (needsWholeTable)
    ids.clear();
  const auto inserted = cycleValueIds_.emplace(cycle, std::move(ids)).first;
  return inserted->second.empty() ? nullptr : &inserted->second;
}

void AbstractInterpretation::initializeSVFGSparse(llvm::Module *module) {
  svfgSparse_ = std::make_unique<SVFGSparseContext>();
  svfgSparse_->icfg = std::make_unique<ICFG>();
  ICFGBuilder icfgBuilder(svfgSparse_->icfg.get());
  icfgBuilder.build(module);

  SVFGBuilderConfig config;
  config.pointerOnly = false;
  config.buildMSSA = true;
  config.includeGlobals = true;
  config.resolveIndirectCalls = true;
  svfgSparse_->builder = std::make_unique<SVFGBuilder>(config);
  svfgSparse_->graph = svfgSparse_->builder->build(svfgSparse_->icfg.get());
}

bool AbstractInterpretation::isSVFGBackedSparseActive() const {
  return stateStore_.layout() == StateLayout::Sparse && svfgSparse_ &&
         svfgSparse_->graph;
}

AbstractValue
AbstractInterpretation::getSVFGSparseLoadValue(const llvm::LoadInst *load) {
  AbstractValue result;
  if (!svfgSparse_ || !svfgSparse_->graph)
    return result;
  ++svfgSparseLoadQueries_;

  SVFGNode *loadNode = svfgSparse_->graph->getDef(load);
  if (!loadNode)
    return result;

  std::unordered_set<SVFGNode *> visited;
  std::function<void(SVFGNode *, unsigned)> collect = [&](SVFGNode *node,
                                                          unsigned depth) {
    if (!node || depth > 64 || !visited.insert(node).second)
      return;
    if (const auto *store = llvm::dyn_cast<StoreSVFGNode>(node)) {
      const llvm::Instruction *storeInst = store->getInstruction();
      auto def = sparseObjectDefs_.find(storeInst);
      if (def != sparseObjectDefs_.end())
        result.join_with(def->second);
      return;
    }
    for (const SVFGEdge *edge : node->getInEdges()) {
      if (!edge || !(edge->isMemoryEdge() || edge->isPhiEdge() ||
                     edge->isIndirectEdge()))
        continue;
      collect(edge->getSrcNode(), depth + 1);
    }
  };
  collect(loadNode, 0);

  return result;
}

void AbstractInterpretation::startTimeBudget() {
  timedOut_ = false;
  if (timeLimitSeconds_ > 0)
    deadline_ = std::chrono::steady_clock::now() +
                std::chrono::seconds(timeLimitSeconds_);
}

bool AbstractInterpretation::timeBudgetExhausted() {
  if (timedOut_)
    return true;
  if (timeLimitSeconds_ <= 0)
    return false;
  if (std::chrono::steady_clock::now() < deadline_)
    return false;
  timedOut_ = true;
  llvm::errs() << "AE: analysis budget of " << timeLimitSeconds_
               << "s exhausted; reporting the findings collected so far\n";
  return true;
}

bool AbstractInterpretation::shouldApplyNarrowing(const llvm::Function *fun) {
  // Non-recursive functions (regular loops): always apply narrowing.
  if (!fun || !isRecursiveFun(fun))
    return true;

  // Recursive functions: mode-dependent.
  switch (recursionMode_) {
  case TOP:
    return false;
  case WIDEN_ONLY:
    return false;
  case WIDEN_NARROW:
    return true;
  default:
    return false;
  }
}

void AbstractInterpretation::collectCheckPoint() {
  // Matching SVF's checkpoint names
  std::set<std::string> ae_checkpoint_names = {"svf_assert", "svf_assert_eq"};

  // Buffer overflow checkpoint names (enabled by enableBufOverflowCheck_)
  std::set<std::string> buf_checkpoint_names = {"UNSAFE_BUFACCESS",
                                                "SAFE_BUFACCESS"};

  // Null dereference checkpoint names (enabled by enableNullDerefCheck_)
  std::set<std::string> nullptr_checkpoint_names = {"UNSAFE_LOAD", "SAFE_LOAD"};

  // Division by zero checkpoint names (enabled by enableDivZeroCheck_)
  std::set<std::string> divzero_checkpoint_names = {"UNSAFE_DIVZERO",
                                                    "SAFE_DIVZERO"};

  // Integer overflow checkpoint names (enabled by enableOverflowCheck_)
  std::set<std::string> overflow_checkpoint_names = {"UNSAFE_OVERFLOW",
                                                     "SAFE_OVERFLOW"};

  for (auto &func : *module_) {
    for (auto &bb : func) {
      for (auto &inst : bb) {
        if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
          if (const llvm::Function *callee = call->getCalledFunction()) {
            std::string funName = callee->getName().str();

            // Always collect svf_assert checkpoints
            if (ae_checkpoint_names.count(funName)) {
              checkpoints.insert(call);
              continue;
            }

            // Collect buffer overflow checkpoints if enabled
            if (enableBufOverflowCheck_ &&
                buf_checkpoint_names.count(funName)) {
              checkpoints.insert(call);
              continue;
            }

            // Collect null dereference checkpoints if enabled
            if (enableNullDerefCheck_ &&
                nullptr_checkpoint_names.count(funName)) {
              checkpoints.insert(call);
              continue;
            }

            // Collect division by zero checkpoints if enabled
            if (enableDivZeroCheck_ &&
                divzero_checkpoint_names.count(funName)) {
              checkpoints.insert(call);
              continue;
            }

            // Collect integer overflow checkpoints if enabled
            if (enableOverflowCheck_ &&
                overflow_checkpoint_names.count(funName)) {
              checkpoints.insert(call);
              continue;
            }
          }
        }
      }
    }
  }

  // Initialize checked checkpoints set
  checkedCheckpoints_.clear();
}

void AbstractInterpretation::checkPointAllSet() {
  if (checkpoints.empty())
    return;

  // Count unchecked checkpoints
  std::vector<const llvm::CallBase *> unchecked;
  for (const auto *call : checkpoints) {
    if (!checkedCheckpoints_.count(call)) {
      unchecked.push_back(call);
    }
  }

  if (unchecked.empty())
    return;

  std::string msg = "At least one checkpoint has not been checked (" +
                    std::to_string(unchecked.size()) + " remaining):\n";
  for (const auto *call : unchecked) {
    std::string callStr;
    llvm::raw_string_ostream os(callStr);
    os << *call;
    os.flush();
    msg += "  " + callStr + "\n";
  }

  if (strictCheckpoint_) {
    llvm::report_fatal_error(llvm::StringRef(msg));
  } else {
    llvm::errs() << "Warning: " << msg;
  }
}

void AbstractInterpretation::recursiveCallPass(const llvm::CallBase *callNode,
                                               const llvm::Function *callee) {
  if (!callee || !isRecursiveFun(callee))
    return;

  // Get the abstract state at the call site
  AbstractState &callState = stateStore_.getOrCreate(callNode);

  // Set all stores in the recursive function to TOP
  setTopToObjInRecursion(callNode, callee);

  if (!callNode->getType()->isVoidTy() && !callNode->getType()->isPointerTy() &&
      !callNode->getType()->isAggregateType()) {
    uint32_t lhsId = getValueId(callNode);
    callState[lhsId] = AbstractValue(IntervalValue::top());
  }
}

void AbstractInterpretation::setTopToObjInRecursion(
    const llvm::CallBase *callNode, const llvm::Function *callee) {
  if (!callee || !isRecursiveFun(callee))
    return;

  AbstractState &callState = stateStore_.getOrCreate(callNode);
  auto stripPointerProjections = [](const llvm::Value *value) {
    while (value) {
      if (const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(value)) {
        value = gep->getPointerOperand();
        continue;
      }
      if (const auto *bitcast = llvm::dyn_cast<llvm::BitCastOperator>(value)) {
        value = bitcast->getOperand(0);
        continue;
      }
      if (const auto *addrspaceCast =
              llvm::dyn_cast<llvm::AddrSpaceCastOperator>(value)) {
        value = addrspaceCast->getOperand(0);
        continue;
      }
      break;
    }
    return value;
  };

  auto topifyReachableMemory = [&](const llvm::Value *actualPtr) {
    uint32_t ptrId = getValueId(actualPtr);
    if (!callState.inVarToAddrsTable(ptrId))
      return;
    for (uint32_t addr : callState[ptrId].getAddrs()) {
      if (AbstractState::isNullMem(addr) || AbstractState::isInvalidMem(addr))
        continue;
      uint32_t objId = callState.getIDFromAddr(addr);
      if (!callState.inAddrToValTable(objId))
        continue;
      AbstractValue &stored = callState.load(addr);
      if (stored.isInterval()) {
        stored.getInterval().set_to_top();
      }
    }
  };

  for (const llvm::BasicBlock &bb : *callee) {
    for (const llvm::Instruction &inst : bb) {
      const auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
      if (!store)
        continue;

      const llvm::Value *storedVal = store->getValueOperand();
      if (storedVal->getType()->isPointerTy() ||
          storedVal->getType()->isVoidTy())
        continue;

      const llvm::Value *base =
          stripPointerProjections(store->getPointerOperand());
      if (const auto *arg = llvm::dyn_cast<llvm::Argument>(base)) {
        if (arg->getParent() == callee &&
            arg->getArgNo() < callNode->arg_size()) {
          topifyReachableMemory(callNode->getArgOperand(arg->getArgNo()));
        }
        continue;
      }

      if (const auto *global = llvm::dyn_cast<llvm::GlobalValue>(base)) {
        topifyReachableMemory(global);
      }
    }
  }
}

// AEStat implementation (matching SVF's AEStat)
AEStat::AEStat(AbstractInterpretation *ae) : _ae(ae) {}

void AEStat::countStateSize() {
  if (count == 0) {
    // Initialize accumulators for computing averages
    generalNumMap["ES_Var_AVG_Num"] = 0.0;
    generalNumMap["ES_Loc_AVG_Num"] = 0.0;
    generalNumMap["ES_Var_Addr_AVG_Num"] = 0.0;
    generalNumMap["ES_Loc_Addr_AVG_Num"] = 0.0;
  }
  ++count;

  // Collect state size information from the abstract interpretation
  // These values are accumulated during analysis
}

void AEStat::finializeStat() {
  if (count > 0) {
    // Compute averages
    generalNumMap["ES_Var_AVG_Num"] /= count;
    generalNumMap["ES_Loc_AVG_Num"] /= count;
    generalNumMap["ES_Var_Addr_AVG_Num"] /= count;
    generalNumMap["ES_Loc_Addr_AVG_Num"] /= count;
  }

  // Set total statement count (matching SVF's SVF_STMT_NUM)
  generalNumMap["Stmt_Num"] = stmtCount;

  // Set ICFG node count (matching SVF's ICFG_Node_Num)
  generalNumMap["ICFG_Node_Num"] = icfgNodeNum;

  // Set function count (matching SVF's Func_Num)
  generalNumMap["Func_Num"] = funcNum;

  // Set external call site count (matching SVF's EXT_CallSite_Num)
  generalNumMap["EXT_CallSite_Num"] = extCallSiteNum;

  // Set non-external call site count (matching SVF's NonEXT_CallSite_Num)
  generalNumMap["NonEXT_CallSite_Num"] = nonExtCallSiteNum;
}

void AEStat::performStat() {
  unsigned field_width = 30;

  llvm::outs() << "\n************************\n";
  llvm::outs()
      << "################ Abstract Execution Statistics ###############\n";

  // Print general statistics (matching SVF format)
  for (const auto &item : generalNumMap) {
    std::string name = item.first;
    // Pad name to field_width, left-aligned
    if (name.length() < field_width) {
      name.append(field_width - name.length(), ' ');
    }
    llvm::outs() << "  " << name << item.second << "\n";
  }

  llvm::outs() << "-------------------------------------------------------\n";

  // Print time statistics (matching SVF format)
  for (const auto &item : timeStatMap) {
    std::string name = item.first;
    // Pad name to field_width, left-aligned
    if (name.length() < field_width) {
      name.append(field_width - name.length(), ' ');
    }
    llvm::outs() << "  " << name << item.second << "\n";
  }

  llvm::outs() << "Memory usage: " << memUsage << "\n";
  llvm::outs() << "#######################################################\n";
  llvm::outs().flush();
}

void AEStat::startClk() {
  startTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count();
}

void AEStat::endClk() {
  endTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
  if (startTime > 0) {
    double duration = (endTime - startTime) / 1000.0; // Convert to seconds
    timeStatMap["Total_Time"] = duration;
  }
}

bool AbstractInterpretation::isBranchFeasible(const llvm::BranchInst *branch,
                                              AbstractState &as,
                                              bool isTrueEdge) {
  if (!branch->isConditional()) {
    return true;
  }

  llvm::Value *cond = branch->getCondition();

  // Try to extract comparison from condition
  if (const llvm::CmpInst *cmp = llvm::dyn_cast<llvm::CmpInst>(cond)) {
    // Use the full predicate handling for cmp instructions
    return isCmpBranchFeasible(cmp, isTrueEdge, as, branch);
  }

  // For non-cmp conditions (e.g., phi, select), do simple check
  uint32_t condId = getValueId(cond);
  materializeConstantValue(as, cond, condId);

  if (!as.inVarToValTable(condId)) {
    // Unknown condition - assume feasible
    return true;
  }

  IntervalValue condVal = as[condId].getInterval();

  if (condVal.isBottom()) {
    return false;
  }

  // Check if condition can be true/false based on the edge
  if (isTrueEdge) {
    // True edge: condition must be able to be non-zero.
    // Any value except exactly 0 is true in LLVM branch semantics.
    return !condVal.is_zero();
  } else {
    // False edge: condition must be able to be zero
    return condVal.contains(0);
  }
}

// Refine an extension and its source together. Separate sext/zext instructions
// of the same SSA source must see the same branch fact, even without a backing
// alloca after mem2reg. Do not invert truncation: it is not injective.
bool AbstractInterpretation::refineIntegerExtensions(
    const llvm::Value *value, const IntervalValue &range, AbstractState &as,
    const llvm::Instruction *guard) {
  std::queue<std::pair<const llvm::Value *, IntervalValue>> pending;
  std::unordered_set<const llvm::Value *> seen;
  pending.push({value, range});

  auto sourceOf = [](const llvm::Value *v) -> const llvm::Value * {
    if (const auto *cast = llvm::dyn_cast<llvm::CastInst>(v)) {
      if ((cast->getOpcode() == llvm::Instruction::SExt ||
           cast->getOpcode() == llvm::Instruction::ZExt) &&
          cast->getDestTy()->getIntegerBitWidth() <= 64)
        return cast->getOperand(0);
    }
    if (const auto *freeze = llvm::dyn_cast<llvm::FreezeInst>(v))
      if (freeze->getType()->isIntegerTy())
        return freeze->getOperand(0);
    return nullptr;
  };

  while (!pending.empty()) {
    const auto item = pending.front();
    pending.pop();
    const llvm::Value *current = item.first;
    const uint32_t id = getValueId(current);
    if (!current->getType()->isIntegerTy() || !as.inVarToValTable(id))
      continue;
    const IntervalValue old = as[id].getInterval();
    IntervalValue narrowed = old;
    narrowed.meet_with(item.second);
    if (narrowed.isBottom())
      return false;
    if (!seen.insert(current).second && narrowed.equals(old))
      continue;
    as[id] = AbstractValue(narrowed);

    if (const llvm::Value *source = sourceOf(current)) {
      IntervalValue inverse = narrowed;
      if (const auto *cast = llvm::dyn_cast<llvm::CastInst>(current)) {
        const unsigned bits = source->getType()->getIntegerBitWidth();
        const int64_t half = int64_t{1} << (bits - 1);
        if (cast->getOpcode() == llvm::Instruction::ZExt) {
          const __int128 modulus = static_cast<__int128>(1) << bits;
          inverse.meet_with(
              IntervalValue(0, static_cast<int64_t>(modulus - 1)));
          if (inverse.isBottom())
            return false;
          const int64_t lb = inverse.lb().getIntNumeral();
          const int64_t ub = inverse.ub().getIntNumeral();
          if (bits == 1 || ub < half) {
            // i1 values are represented as 0/1, including comparison results.
          } else if (lb >= half) {
            inverse = IntervalValue(static_cast<int64_t>(lb - modulus),
                                    static_cast<int64_t>(ub - modulus));
          } else {
            // Unsigned interval straddles the signed boundary: its inverse
            // is disjoint, so keep the full signed hull.
            inverse = IntervalValue(-half, half - 1);
          }
        } else {
          inverse.meet_with(IntervalValue(-half, half - 1));
          if (inverse.isBottom())
            return false;
          if (bits == 1)
            inverse = IntervalValue(-inverse.ub(), -inverse.lb());
        }
      }
      pending.push({source, inverse});
    }

    // Update already evaluated sibling extensions as well as future ones.
    for (const llvm::User *user : current->users()) {
      if (sourceOf(user) != current || !as.inVarToValTable(getValueId(user)))
        continue;
      auto &dt = dominatorTrees_[guard->getFunction()];
      if (!dt)
        dt = std::make_unique<llvm::DominatorTree>(
            *const_cast<llvm::Function *>(guard->getFunction()));
      // A non-dominating definition in the snapshot can be stale from a prior
      // loop iteration. It must not constrain the current PHI through its cast.
      if (!dt->dominates(llvm::cast<llvm::Instruction>(user), guard))
        continue;
      IntervalValue image = narrowed;
      if (const auto *cast = llvm::dyn_cast<llvm::CastInst>(user)) {
        const unsigned bits = current->getType()->getIntegerBitWidth();
        const int64_t half = int64_t{1} << (bits - 1);
        if (bits == 1) {
          image.meet_with(IntervalValue(0, 1));
          if (cast->getOpcode() == llvm::Instruction::SExt)
            image = IntervalValue(-image.ub(), -image.lb());
        } else {
          image.meet_with(IntervalValue(-half, half - 1));
          if (cast->getOpcode() == llvm::Instruction::ZExt) {
            const __int128 modulus = static_cast<__int128>(1) << bits;
            if (image.ub().getIntNumeral() < 0)
              image = IntervalValue(
                  static_cast<int64_t>(modulus + image.lb().getIntNumeral()),
                  static_cast<int64_t>(modulus + image.ub().getIntNumeral()));
            else if (image.lb().getIntNumeral() < 0)
              image = IntervalValue(0, static_cast<int64_t>(modulus - 1));
          }
        }
      }
      pending.push({user, image});
    }
  }
  return true;
}

// A branch fact about a load may refine the current memory only if the load
// names one concrete cell and that cell cannot have changed since the load.
void AbstractInterpretation::refineMemoryFromLoad(
    const llvm::Value *value, AbstractState &as,
    const llvm::Instruction *branchPoint) {
  while (true) {
    if (const auto *cast = llvm::dyn_cast<llvm::CastInst>(value)) {
      if ((cast->getOpcode() == llvm::Instruction::SExt ||
           cast->getOpcode() == llvm::Instruction::ZExt) &&
          cast->getDestTy()->getIntegerBitWidth() <= 64) {
        value = cast->getOperand(0);
        continue;
      }
    }
    if (const auto *freeze = llvm::dyn_cast<llvm::FreezeInst>(value)) {
      if (freeze->getType()->isIntegerTy()) {
        value = freeze->getOperand(0);
        continue;
      }
    }
    break;
  }
  const auto *load = llvm::dyn_cast<llvm::LoadInst>(value);
  if (!load || load->isVolatile() || load->isAtomic())
    return;
  const uint32_t loadId = getValueId(load);
  const uint32_t ptrId = getValueId(load->getPointerOperand());
  if (!as.inVarToAddrsTable(ptrId) ||
      !isStrongUpdateForStore(as[ptrId].getAddrs(), as) ||
      (!as.inVarToValTable(loadId) && !as.inVarToAddrsTable(loadId)))
    return;

  // Scan every CFG path backwards from the actual branch, including writes
  // between the comparison and its terminator. Any possible write blocks this
  // refinement; using a stale load to constrain a newer store is unsound.
  std::vector<const llvm::BasicBlock *> pending{branchPoint->getParent()};
  std::unordered_set<const llvm::BasicBlock *> visited;
  bool foundLoad = false;
  while (!pending.empty()) {
    const llvm::BasicBlock *block = pending.back();
    pending.pop_back();
    if (!visited.insert(block).second)
      continue;
    bool reachedLoad = false;
    for (auto it = block->rbegin(); it != block->rend(); ++it) {
      if (&*it == load) {
        foundLoad = reachedLoad = true;
        break;
      }
      if (it->mayWriteToMemory())
        return;
    }
    if (reachedLoad)
      continue;
    if (llvm::pred_empty(block))
      return;
    for (const llvm::BasicBlock *pred : llvm::predecessors(block))
      pending.push_back(pred);
  }
  if (!foundLoad)
    return;
  // Use the load's own (possibly inverse-refined) value, not the interval of
  // its extension. For example zext i8 -56 is 200, but the cell still holds
  // -56.
  const AbstractValue refined = as[loadId];
  as.store(*as[ptrId].getAddrs().begin(), refined);
}

bool AbstractInterpretation::isCmpBranchFeasible(
    const llvm::CmpInst *cmpInst, bool succ, AbstractState &as,
    const llvm::Instruction *branchPoint) {
  uint32_t op0Id = getValueId(cmpInst->getOperand(0));
  uint32_t op1Id = getValueId(cmpInst->getOperand(1));
  const llvm::Value *op0Val = cmpInst->getOperand(0);
  const llvm::Value *op1Val = cmpInst->getOperand(1);

  materializeConstantValue(as, op0Val, op0Id);
  materializeConstantValue(as, op1Val, op1Id);

  int32_t predicate = cmpInst->getPredicate();
  if (!succ) {
    auto reversePredMap = getReversePredicate();
    if (reversePredMap.find(predicate) != reversePredMap.end()) {
      predicate = reversePredMap[predicate];
    }
  }

  // Refine pointer-vs-null comparisons to reduce false positives in guarded
  // dereferences (e.g., if (p) { *p = ... }).
  if (predicate == llvm::CmpInst::ICMP_EQ ||
      predicate == llvm::CmpInst::ICMP_NE) {
    if (op0Id == AbstractState::NullPtr && op1Id == AbstractState::NullPtr) {
      return predicate == llvm::CmpInst::ICMP_EQ;
    }

    uint32_t ptrId = 0;
    bool hasNullOperand = false;
    if (op0Id == AbstractState::NullPtr) {
      hasNullOperand = true;
      ptrId = op1Id;
    } else if (op1Id == AbstractState::NullPtr) {
      hasNullOperand = true;
      ptrId = op0Id;
    }

    if (hasNullOperand && as.inVarToAddrsTable(ptrId)) {
      auto refineAddrSet =
          [&](AddressValue::AddrSet vals) -> AddressValue::AddrSet {
        if (predicate == llvm::CmpInst::ICMP_NE) {
          vals.erase(NullMemAddr); // true edge of (p != null)
        } else {
          if (vals.count(NullMemAddr) != 0 || vals.count(InvalidMemAddr) != 0) {
            vals = {NullMemAddr}; // true edge of (p == null)
          } else {
            vals.clear();
          }
        }
        return vals;
      };

      AddressValue::AddrSet refined =
          refineAddrSet(as[ptrId].getAddrs().getVals());
      if (refined.empty()) {
        return false;
      }
      as[ptrId] = AbstractValue(AddressValue(refined));

      const llvm::Value *ptrVal = (ptrId == op0Id) ? op0Val : op1Val;
      refineMemoryFromLoad(ptrVal, as, branchPoint);
      return true;
    }
  }

  if (as.inVarToAddrsTable(op0Id) || as.inVarToAddrsTable(op1Id)) {
    return true;
  }

  if (!as.inVarToValTable(op0Id) || !as.inVarToValTable(op1Id)) {
    return true;
  }

  IntervalValue val0 = as[op0Id].getInterval();
  IntervalValue val1 = as[op1Id].getInterval();
  if (val0.isBottom() || val1.isBottom()) {
    return false;
  }

  AbstractState newEs = as;
  uint32_t lhsId = op0Id;
  uint32_t rhsId = op1Id;

  bool b0 = val0.is_numeral();
  bool b1 = val1.is_numeral();

  const bool isUnsignedCmp = predicate == llvm::CmpInst::ICMP_UGT ||
                             predicate == llvm::CmpInst::ICMP_UGE ||
                             predicate == llvm::CmpInst::ICMP_ULT ||
                             predicate == llvm::CmpInst::ICMP_ULE;
  if (isUnsignedCmp && !(b0 && b1) &&
      cmpInst->getOperand(0)->getType()->isIntegerTy()) {
    const unsigned bits =
        cmpInst->getOperand(0)->getType()->getIntegerBitWidth();
    if (!unsignedPredicatePossible(predicate, val0, val1, bits))
      return false;

    const int64_t maxSigned =
        bits >= 64 ? std::numeric_limits<int64_t>::max()
                   : static_cast<int64_t>((int64_t{1} << (bits - 1)) - 1);
    const bool refinable = !val0.is_infinite() && !val1.is_infinite() &&
                           val0.lb().getIntNumeral() >= 0 &&
                           val0.ub().getIntNumeral() <= maxSigned &&
                           val1.lb().getIntNumeral() >= 0 &&
                           val1.ub().getIntNumeral() <= maxSigned;
    const bool unsignedUpperBound = !b0 && b1 &&
                                    (predicate == llvm::CmpInst::ICMP_ULT ||
                                     predicate == llvm::CmpInst::ICMP_ULE) &&
                                    val1.lb().getIntNumeral() >= 0;
    if (!refinable && !unsignedUpperBound)
      return true;
  }

  if (b0 && !b1) {
    std::swap(lhsId, rhsId);
    predicate = getSwitchLhsRhsPredicate()[predicate];
  } else if (!b0 && !b1) {
    if ((predicate == llvm::CmpInst::ICMP_SLT ||
         predicate == llvm::CmpInst::ICMP_SLE) &&
        !val1.ub().is_infinity()) {
      int64_t upper = val1.ub().getIntNumeral();
      if (predicate == llvm::CmpInst::ICMP_SLT) {
        if (upper == std::numeric_limits<int64_t>::min())
          return true;
        --upper;
      }
      IntervalValue refined = val0;
      refined.meet_with(IntervalValue(BoundedInt::minus_infinity(),
                                      BoundedInt(upper)));
      if (refined.isBottom())
        return false;
      if (!refineIntegerExtensions(op0Val, refined, as, cmpInst))
        return false;
      refineMemoryFromLoad(op0Val, as, branchPoint);
    }
    return true;
  } else if (b0 && b1) {
    if (!cmpInst->getOperand(0)->getType()->isIntegerTy())
      return true;

    const int64_t left = val0.getIntNumeral();
    const int64_t right = val1.getIntNumeral();
    const uint64_t uleft = static_cast<uint64_t>(left);
    const uint64_t uright = static_cast<uint64_t>(right);
    switch (predicate) {
    case llvm::CmpInst::ICMP_EQ:
      return left == right;
    case llvm::CmpInst::ICMP_NE:
      return left != right;
    case llvm::CmpInst::ICMP_SGT:
      return left > right;
    case llvm::CmpInst::ICMP_SGE:
      return left >= right;
    case llvm::CmpInst::ICMP_SLT:
      return left < right;
    case llvm::CmpInst::ICMP_SLE:
      return left <= right;
    case llvm::CmpInst::ICMP_UGT:
      return uleft > uright;
    case llvm::CmpInst::ICMP_UGE:
      return uleft >= uright;
    case llvm::CmpInst::ICMP_ULT:
      return uleft < uright;
    case llvm::CmpInst::ICMP_ULE:
      return uleft <= uright;
    default:
      return true;
    }
  }

  IntervalValue lhs = newEs[lhsId].getInterval();
  const IntervalValue rhs = newEs[rhsId].getInterval();

  switch (predicate) {
  case llvm::CmpInst::ICMP_EQ:
  case llvm::CmpInst::FCMP_OEQ:
  case llvm::CmpInst::FCMP_UEQ:
    lhs.meet_with(rhs);
    break;
  case llvm::CmpInst::ICMP_NE:
  case llvm::CmpInst::FCMP_ONE:
  case llvm::CmpInst::FCMP_UNE:
    break;
  case llvm::CmpInst::ICMP_SGT:
  case llvm::CmpInst::FCMP_OGT:
  case llvm::CmpInst::FCMP_UGT: {
    BoundedInt lb(rhs.lb().getIntNumeral() + 1);
    BoundedInt ub = BoundedInt::plus_infinity();
    lhs.meet_with(IntervalValue(lb, ub));
    break;
  }
  case llvm::CmpInst::ICMP_UGT: {
    int64_t rhsLb = rhs.lb().getIntNumeral();
    int64_t lower = std::max<int64_t>(0, rhsLb + 1);
    lhs.meet_with(
        IntervalValue(BoundedInt(lower), BoundedInt::plus_infinity()));
    break;
  }
  case llvm::CmpInst::ICMP_SGE:
  case llvm::CmpInst::FCMP_OGE:
  case llvm::CmpInst::FCMP_UGE: {
    BoundedInt lb(rhs.lb().getIntNumeral());
    BoundedInt ub = BoundedInt::plus_infinity();
    lhs.meet_with(IntervalValue(lb, ub));
    break;
  }
  case llvm::CmpInst::ICMP_UGE: {
    int64_t lower = std::max<int64_t>(0, rhs.lb().getIntNumeral());
    lhs.meet_with(
        IntervalValue(BoundedInt(lower), BoundedInt::plus_infinity()));
    break;
  }
  case llvm::CmpInst::ICMP_SLT:
  case llvm::CmpInst::FCMP_OLT:
  case llvm::CmpInst::FCMP_ULT: {
    BoundedInt lb = BoundedInt::minus_infinity();
    BoundedInt ub(rhs.ub().getIntNumeral() - 1);
    lhs.meet_with(IntervalValue(lb, ub));
    break;
  }
  case llvm::CmpInst::ICMP_ULT: {
    BoundedInt ub(rhs.ub().getIntNumeral() - 1);
    lhs.meet_with(IntervalValue(BoundedInt(0), ub));
    break;
  }
  case llvm::CmpInst::ICMP_SLE:
  case llvm::CmpInst::FCMP_OLE:
  case llvm::CmpInst::FCMP_ULE: {
    BoundedInt lb = BoundedInt::minus_infinity();
    BoundedInt ub(rhs.ub().getIntNumeral());
    lhs.meet_with(IntervalValue(lb, ub));
    break;
  }
  case llvm::CmpInst::ICMP_ULE: {
    BoundedInt ub(rhs.ub().getIntNumeral());
    lhs.meet_with(IntervalValue(BoundedInt(0), ub));
    break;
  }
  case llvm::CmpInst::FCMP_FALSE:
    break;
  case llvm::CmpInst::FCMP_TRUE:
    break;
  default:
    return true;
  }

  if (lhs.isBottom()) {
    return false;
  }
  newEs[lhsId] = AbstractValue(lhs);
  if (cmpInst->getOperand(0)->getType()->isIntegerTy() &&
      !refineIntegerExtensions(lhsId == op0Id ? op0Val : op1Val, lhs, newEs,
                               cmpInst))
    return false;

  refineMemoryFromLoad(lhsId == op0Id ? op0Val : op1Val, newEs, branchPoint);

  as = newEs;
  return true;
}

bool AbstractInterpretation::isSwitchSuccessorFeasible(
    const llvm::SwitchInst *switchInst, const llvm::BasicBlock *successor,
    AbstractState &as) {
  llvm::Value *condition = switchInst->getCondition();
  uint32_t condId = getValueId(condition);
  materializeConstantValue(as, condition, condId);

  if (!as.inVarToValTable(condId))
    return true;

  const IntervalValue switchCond = as[condId].getInterval();
  if (switchCond.isBottom())
    return false;
  if (switchCond.is_infinite())
    return true;

  const int64_t lower = switchCond.lb().getIntNumeral();
  const int64_t upper = switchCond.ub().getIntNumeral();
  const bool isDefault = successor == switchInst->getDefaultDest();
  bool hasCaseEdgeToSuccessor = false;
  uint32_t matchingCaseCount = 0;
  int64_t matchingCase = 0;
  uint32_t matchedCasesInRange = 0;

  for (auto caseIt = switchInst->case_begin(); caseIt != switchInst->case_end();
       ++caseIt) {
    const int64_t caseValue = caseIt->getCaseValue()->getSExtValue();
    const bool inRange = lower <= caseValue && caseValue <= upper;
    if (inRange)
      ++matchedCasesInRange;
    if (caseIt->getCaseSuccessor() != successor)
      continue;
    hasCaseEdgeToSuccessor = true;
    if (inRange) {
      ++matchingCaseCount;
      matchingCase = caseValue;
    }
  }

  if (!isDefault) {
    if (!hasCaseEdgeToSuccessor || matchingCaseCount == 0)
      return false;

    // Refine only when this destination has one case label. Several labels may
    // share a destination, in which case selecting one would be unsound.
    if (matchingCaseCount == 1) {
      IntervalValue caseVal(matchingCase, matchingCase);
      IntervalValue narrowed = switchCond;
      narrowed.meet_with(caseVal);
      if (narrowed.isBottom())
        return false;
      as[condId] = AbstractValue(narrowed);
      if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(condition)) {
        uint32_t ptrId = getValueId(load->getPointerOperand());
        if (as.inVarToAddrsTable(ptrId)) {
          for (uint32_t addr : as[ptrId].getAddrs()) {
            uint32_t objId = as.getIDFromAddr(addr);
            if (as.inAddrToValTable(objId))
              as.load(addr).getInterval().meet_with(caseVal);
          }
        }
      }
    }
    return true;
  }

  // If a case also targets the default block, the destination is reachable
  // without requiring the default edge itself.
  if (matchingCaseCount != 0)
    return true;

  // The default edge is feasible exactly when the finite condition range has
  // at least one value that no case label consumes.
  const __int128 valueCount =
      static_cast<__int128>(upper) - static_cast<__int128>(lower) + 1;
  return valueCount > static_cast<__int128>(matchedCasesInRange);
}

bool AbstractInterpretation::checkPathFeasibilityWithSolver(
    const AbstractState &as, const Z3Expr &pathConstraint) {
  // Use the relation solver to check if the path constraint is satisfiable
  // given the abstract state

  // Build gamma_hat expression from abstract state (constraints on variables)
  Z3Expr stateConstraints = relSolver_.gamma_hat(as);

  // Combine state constraints with path constraint
  Z3Expr combinedConstraint = stateConstraints && pathConstraint;

  // Check satisfiability using Z3 solver
  z3::solver &solver = Z3Expr::getSolver();
  z3::params params(Z3Expr::getContext());
  params.set(":timeout", static_cast<unsigned>(1000)); // 1 second timeout
  solver.set(params);

  solver.push();
  solver.add(combinedConstraint.getExpr());

  z3::check_result result = solver.check();
  solver.pop();

  // Path is feasible if satisfiable
  return result == z3::sat;
}

AbstractState AbstractInterpretation::computeRSY(const AbstractState &domain,
                                                 const Z3Expr &phi) {
  // Use the RelationSolver's RSY (Rohn/Speelpenning/Yudell) method
  // for more precise abstract interpretation with relational constraints
  // This helps refine the abstract state based on path constraints
  return relSolver_.RSY(domain, phi);
}

AbstractState
AbstractInterpretation::computeBilateral(const AbstractState &domain,
                                         const Z3Expr &phi) {
  // Use the RelationSolver's bilateral method for precise abstract
  // interpretation Bilateral narrowing combines abstract interpretation with
  // constraint solving to precisely refine the abstract state
  return relSolver_.bilateral(domain, phi);
}

} // namespace analysis
} // namespace lotus
