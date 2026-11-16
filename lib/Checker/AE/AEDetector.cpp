//===- AEDetector.cpp -- Vulnerability Detectors--------------------//
//
// Migrated from SVF's AE engine to Lotus.
//
//===----------------------------------------------------------------------===//

#include "Checker/AE/AEDetector.h"

#include "Checker/AE/AbsExtAPI.h"
#include "Checker/AE/AbstractInterpretation.h"
#include "Checker/Framework/BugReport.h"
#include "Checker/Framework/BugReportMgr.h"
#include "Checker/Framework/BugTypes.h"

#include <algorithm>
#include <limits>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus {
namespace analysis {

namespace {
const llvm::Value *stripPointerProjections(const llvm::Value *value) {
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
}

IntervalValue byteCountToMaxAccessOffset(const IntervalValue &byteCount) {
  return byteCount - IntervalValue(1);
}

bool isZeroByteCount(const IntervalValue &byteCount) {
  return byteCount.is_numeral() && byteCount.getIntNumeral() == 0;
}

IntervalValue getCallArgumentInterval(AbstractState &as,
                                      const llvm::Value *argument) {
  if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(argument))
    return IntervalValue(constant->getSExtValue());
  uint32_t valueId = AbstractInterpretation::getValueIdStatic(argument);
  if (as.inVarToValTable(valueId))
    return as[valueId].getInterval();
  return IntervalValue::top();
}

bool hasNullOrInvalidTarget(AbstractState &as, uint32_t ptrId) {
  if (!as.inVarToAddrsTable(ptrId))
    return false;
  for (uint32_t addr : as[ptrId].getAddrs()) {
    if (AbstractState::isNullMem(addr) || AbstractState::isInvalidMem(addr))
      return true;
  }
  return false;
}

bool intervalIsOutsideObject(const IntervalValue &offset, uint32_t objectSize,
                             bool checkLowerBound = true) {
  if (objectSize == 0 || offset.isBottom())
    return false;
  // getIntNumeral() maps infinities to INT64_MIN/MAX respectively.
  return (checkLowerBound && offset.lb().getIntNumeral() < 0) ||
         offset.ub().getIntNumeral() >= static_cast<int64_t>(objectSize);
}

uint32_t getRemainingByteCapacity(
    AbstractState &as, uint32_t ptrId,
    const std::map<uint32_t, IntervalValue> &offsetFromBaseByObjId) {
  if (!as.inVarToAddrsTable(ptrId))
    return 0;

  bool foundConcreteTarget = false;
  uint32_t minRemaining = std::numeric_limits<uint32_t>::max();

  for (uint32_t addr : as[ptrId].getAddrs()) {
    if (AbstractState::isInvalidMem(addr) || AbstractState::isNullMem(addr)) {
      return 0;
    }

    const uint32_t objId = as.getIDFromAddr(addr);
    const uint32_t objSize = as.getObjSize(objId);
    if (objSize == 0)
      continue;

    int64_t offset = 0;
    auto offsetIt = offsetFromBaseByObjId.find(objId);
    if (offsetIt != offsetFromBaseByObjId.end()) {
      const IntervalValue &baseOffset = offsetIt->second;
      if (baseOffset.is_infinite()) {
        return 0;
      }
      offset = std::max<int64_t>(0, baseOffset.ub().getIntNumeralOrZero());
    }

    uint32_t remaining = objSize;
    if (offsetIt != offsetFromBaseByObjId.end() &&
        offset < static_cast<int64_t>(objSize)) {
      remaining = objSize - static_cast<uint32_t>(offset);
    }
    minRemaining = std::min(minRemaining, remaining);
    foundConcreteTarget = true;
  }

  if (!foundConcreteTarget)
    return 0;
  return minRemaining;
}

std::vector<uint32_t> getExtAPIPointerArgIndices(const llvm::CallBase *call,
                                                 const llvm::Function *callee,
                                                 AEExtAPI *utils) {
  std::vector<uint32_t> pointerArgs;
  if (!call || !callee || !utils) {
    return pointerArgs;
  }

  const std::string funcName = callee->getName().str();
  const bool isIconvLike = funcName == "iconv" || funcName == "libiconv";

  for (const std::string &annotation : utils->getExtFuncAnnotations(callee)) {
    if (annotation.find("MEMCPY") != std::string::npos) {
      if (isIconvLike) {
        pointerArgs.insert(pointerArgs.end(), {1, 2, 3, 4});
      } else {
        if (call->arg_size() > 0 &&
            call->getArgOperand(0)->getType()->isPointerTy()) {
          pointerArgs.push_back(0);
        }
        if (call->arg_size() > 1 &&
            call->getArgOperand(1)->getType()->isPointerTy()) {
          pointerArgs.push_back(1);
        }
      }
    } else if (annotation.find("MEMSET") != std::string::npos) {
      pointerArgs.push_back(0);
    } else if (annotation.find("STRCPY") != std::string::npos ||
               annotation.find("STRCAT") != std::string::npos) {
      pointerArgs.push_back(0);
      pointerArgs.push_back(1);
    } else if (annotation.find("RECV") != std::string::npos) {
      if (call->arg_size() > 1 &&
          call->getArgOperand(1)->getType()->isPointerTy()) {
        pointerArgs.push_back(1);
      }
    } else if (annotation.find("STRLEN") != std::string::npos ||
               annotation.find("STRCHR") != std::string::npos ||
               annotation.find("STRSTR") != std::string::npos) {
      if (call->arg_size() > 0 &&
          call->getArgOperand(0)->getType()->isPointerTy()) {
        pointerArgs.push_back(0);
      }
      if (annotation.find("STRSTR") != std::string::npos &&
          call->arg_size() > 1 &&
          call->getArgOperand(1)->getType()->isPointerTy()) {
        pointerArgs.push_back(1);
      }
    } else if (annotation.find("FGETS") != std::string::npos ||
               annotation.find("FREAD") != std::string::npos ||
               annotation.find("FWRITE") != std::string::npos) {
      if (call->arg_size() > 0 &&
          call->getArgOperand(0)->getType()->isPointerTy()) {
        pointerArgs.push_back(0);
      }
    } else if (annotation.find("SCANF") != std::string::npos) {
      uint32_t outputStart = 1;
      if (funcName.find("fscanf") != std::string::npos ||
          funcName.find("sscanf") != std::string::npos) {
        outputStart = 2;
        if (call->arg_size() > 0 &&
            call->getArgOperand(0)->getType()->isPointerTy()) {
          pointerArgs.push_back(0);
        }
      }
      for (uint32_t i = outputStart; i < call->arg_size(); ++i) {
        if (call->getArgOperand(i)->getType()->isPointerTy())
          pointerArgs.push_back(i);
      }
    } else if (annotation.find("SNPRINTF") != std::string::npos) {
      if (call->arg_size() > 2 &&
          call->getArgOperand(2)->getType()->isPointerTy()) {
        pointerArgs.push_back(2);
      }
    } else if (annotation.find("PRINTF") != std::string::npos) {
      if (funcName.find("sprintf") != std::string::npos ||
          funcName.find("sprintf_chk") != std::string::npos) {
        if (call->arg_size() > 0 &&
            call->getArgOperand(0)->getType()->isPointerTy()) {
          pointerArgs.push_back(0);
        }
        if (call->arg_size() > 1 &&
            call->getArgOperand(1)->getType()->isPointerTy()) {
          pointerArgs.push_back(1);
        }
      } else if (funcName.find("fprintf") != std::string::npos) {
        if (call->arg_size() > 1 &&
            call->getArgOperand(1)->getType()->isPointerTy()) {
          pointerArgs.push_back(1);
        }
      } else if (call->arg_size() > 0 &&
                 call->getArgOperand(0)->getType()->isPointerTy()) {
        pointerArgs.push_back(0);
      }
    } else if (annotation.find("ITOA") != std::string::npos) {
      if (call->arg_size() > 1 &&
          call->getArgOperand(1)->getType()->isPointerTy()) {
        pointerArgs.push_back(1);
      }
    } else if (annotation.find("STRTO") != std::string::npos ||
               annotation.find("ENV") != std::string::npos) {
      if (call->arg_size() > 0 &&
          call->getArgOperand(0)->getType()->isPointerTy()) {
        pointerArgs.push_back(0);
      }
    }
  }

  auto addPointerArg = [&](uint32_t index) {
    if (call->arg_size() > index &&
        call->getArgOperand(index)->getType()->isPointerTy()) {
      pointerArgs.push_back(index);
    }
  };
  if (funcName == "memcmp" || funcName == "strcmp" || funcName == "strncmp") {
    addPointerArg(0);
    addPointerArg(1);
  } else if (funcName == "memchr" || funcName == "puts" ||
             funcName == "fputs") {
    addPointerArg(0);
  } else if (funcName == "strdup" || funcName == "strndup") {
    addPointerArg(0);
  } else if (funcName == "strxfrm" || funcName == "wcsxfrm") {
    addPointerArg(0);
    addPointerArg(1);
  } else if (funcName == "strcoll" || funcName == "strpbrk" ||
             funcName == "strspn" || funcName == "strcspn") {
    addPointerArg(0);
    addPointerArg(1);
  } else if (funcName == "posix_memalign" || funcName == "asprintf" ||
             funcName == "vasprintf") {
    addPointerArg(0);
  } else if (funcName == "getline" || funcName == "getdelim") {
    addPointerArg(0);
    addPointerArg(1);
  } else if (funcName == "read" || funcName == "pread" || funcName == "write" ||
             funcName == "pwrite" || funcName == "send" ||
             funcName == "sendto" || funcName == "recvfrom" ||
             funcName == "readlink") {
    addPointerArg(1);
  }

  std::sort(pointerArgs.begin(), pointerArgs.end());
  pointerArgs.erase(std::unique(pointerArgs.begin(), pointerArgs.end()),
                    pointerArgs.end());
  return pointerArgs;
}

int getOrRegisterAEBugType(AEDetector::DetectorKind kind) {
  BugReportMgr &mgr = BugReportMgr::get_instance();
  switch (kind) {
  case AEDetector::BUF_OVERFLOW: {
    int id = mgr.find_bug_type("AE Buffer Overflow");
    if (id < 0) {
      id = mgr.register_bug_type("AE Buffer Overflow", BugDescription::BI_HIGH,
                                 BugDescription::BC_SECURITY,
                                 "CWE-120, CWE-122");
    }
    return id;
  }
  case AEDetector::NULL_DEREF: {
    int id = mgr.find_bug_type("AE Null Dereference");
    if (id < 0) {
      id = mgr.register_bug_type("AE Null Dereference", BugDescription::BI_HIGH,
                                 BugDescription::BC_SECURITY, "CWE-476");
    }
    return id;
  }
  case AEDetector::DIV_ZERO: {
    int id = mgr.find_bug_type("AE Divide By Zero");
    if (id < 0) {
      id = mgr.register_bug_type("AE Divide By Zero", BugDescription::BI_HIGH,
                                 BugDescription::BC_SECURITY, "CWE-369");
    }
    return id;
  }
  case AEDetector::INT_OVERFLOW: {
    int id = mgr.find_bug_type("AE Integer Overflow");
    if (id < 0) {
      id = mgr.register_bug_type("AE Integer Overflow", BugDescription::BI_HIGH,
                                 BugDescription::BC_SECURITY, "CWE-190");
    }
    return id;
  }
  case AEDetector::USE_AFTER_FREE: {
    int id = mgr.find_bug_type("AE Use After Free");
    if (id < 0) {
      id = mgr.register_bug_type("AE Use After Free", BugDescription::BI_HIGH,
                                 BugDescription::BC_SECURITY, "CWE-416");
    }
    return id;
  }
  case AEDetector::INVALID_FREE: {
    int id = mgr.find_bug_type("AE Invalid Free");
    if (id < 0) {
      id = mgr.register_bug_type("AE Invalid Free", BugDescription::BI_HIGH,
                                 BugDescription::BC_SECURITY, "CWE-590");
    }
    return id;
  }
  case AEDetector::MEMORY_LEAK: {
    int id = mgr.find_bug_type("AE Memory Leak");
    if (id < 0) {
      id = mgr.register_bug_type("AE Memory Leak", BugDescription::BI_MEDIUM,
                                 BugDescription::BC_PERFORMANCE, "CWE-401");
    }
    return id;
  }
  default:
    break;
  }
  return -1;
}

void emitAEBugReport(AEDetector::DetectorKind kind,
                     const llvm::Instruction *inst,
                     const std::string &message) {
  int tyId = getOrRegisterAEBugType(kind);
  if (tyId < 0)
    return;

  BugReport *report = new BugReport(tyId);
  report->append_step(const_cast<llvm::Instruction *>(inst), message);
  BugReportMgr::get_instance().insert_report(tyId, report, true);
}
} // namespace

void AEDetector::addEventToTrace(AEBugEventType type,
                                 const llvm::Instruction *inst,
                                 const std::string &desc) {
  eventTrace.emplace_back(type, inst, desc);
}

void AEDetector::clearEventTrace() { eventTrace.clear(); }

/// @brief Detects buffer overflow issues for a given instruction.
///
/// This function handles GEP (GetElementPtr) instructions to detect potential
/// buffer overflows by comparing the access offset against the object size.
///
/// @param as Reference to the abstract state containing object sizes and
/// addresses.
/// @param inst Pointer to the instruction to analyze (must be a GEP
/// instruction).
void BufOverflowDetector::detect(AbstractState &as,
                                 const llvm::Instruction *inst) {
  // Check for buffer overflow in GEP instructions
  if (const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(inst)) {
    uint32_t ptrId =
        AbstractInterpretation::getValueIdStatic(gep->getPointerOperand());
    uint32_t lhsId = AbstractInterpretation::getValueIdStatic(gep);
    if (as.inVarToAddrsTable(ptrId)) {
      // Get GEP addresses
      AddressValue gepAddrs = as[lhsId].getAddrs();
      AddressValue objAddrs = as[ptrId].getAddrs();
      IntervalValue offset = as.getByteOffset(gep);

      // Track offset for this GEP first
      IntervalValue accumulatedOffset = offset;

      // Check if the pointer operand is itself a GEP (nested GEP case)
      // If so, accumulate the offset from the previous GEP
      if (const auto *prevGep = llvm::dyn_cast<llvm::GetElementPtrInst>(
              gep->getPointerOperand())) {
        if (hasGepObjOffsetFromBase(prevGep)) {
          IntervalValue prevOffset = getGepObjOffsetFromBase(prevGep);
          accumulatedOffset = prevOffset + offset;
        }
      }

      // Store the accumulated offset for this GEP
      addToGepObjOffsetFromBase(gep, accumulatedOffset);

      // Update GEP offset tracking (for compatibility with existing code)
      updateGepObjOffsetFromBase(as, gepAddrs, objAddrs, accumulatedOffset);

      // Record the absolute end offset of the selected subobject. Struct-field
      // GEPs can select an array/struct member whose size is smaller than the
      // enclosing aggregate; subsequent GEPs through that field must honor the
      // member boundary.
      uint32_t resultCapacity = 0;
      for (uint32_t objAddr : objAddrs) {
        uint32_t baseObjId = as.getIDFromAddr(objAddr);
        uint32_t baseCapacity = as.getObjSize(baseObjId);
        auto capIt = gepObjCapacityByObjId.find(baseObjId);
        if (capIt != gepObjCapacityByObjId.end())
          baseCapacity = std::min(baseCapacity, capIt->second);
        if (baseCapacity > 0)
          resultCapacity = resultCapacity == 0
                               ? baseCapacity
                               : std::min(resultCapacity, baseCapacity);
      }
      if (gep->getSourceElementType()->isStructTy() && offset.is_numeral()) {
        llvm::Type *resultType = gep->getResultElementType();
        const auto *resultArrayType =
            llvm::dyn_cast_or_null<llvm::ArrayType>(resultType);
        const bool flexibleArray =
            resultArrayType && resultArrayType->getNumElements() == 0;
        if (resultType && resultType->isSized() && !flexibleArray) {
          uint64_t fieldEnd =
              static_cast<uint64_t>(std::max<int64_t>(
                  0, accumulatedOffset.getIntNumeralOrZero())) +
              gep->getModule()->getDataLayout().getTypeAllocSize(resultType);
          uint32_t fieldCapacity =
              fieldEnd > std::numeric_limits<uint32_t>::max()
                  ? std::numeric_limits<uint32_t>::max()
                  : static_cast<uint32_t>(fieldEnd);
          resultCapacity = resultCapacity == 0
                               ? fieldCapacity
                               : std::min(resultCapacity, fieldCapacity);
        }
      }
      if (resultCapacity > 0) {
        for (uint32_t gepAddr : gepAddrs) {
          uint32_t gepObjId = as.getIDFromAddr(gepAddr);
          auto [it, inserted] =
              gepObjCapacityByObjId.emplace(gepObjId, resultCapacity);
          if (!inserted)
            it->second = std::min(it->second, resultCapacity);
        }
      }

      for (auto addr : as[ptrId].getAddrs()) {
        uint32_t objId = as.getIDFromAddr(addr);
        // Compute access offset per object to preserve field/object
        // sensitivity.
        IntervalValue accessOffset = getAccessOffset(as, objId, gep);
        uint32_t objSize = as.getObjSize(objId);
        auto capacityIt = gepObjCapacityByObjId.find(objId);
        if (capacityIt != gepObjCapacityByObjId.end())
          objSize = std::min(objSize, capacityIt->second);
        if (objSize == 0 && as.hasKnownObjSize(objId)) {
          AEException bug("Buffer access outside zero-sized object");
          addBugToReporter(bug, inst);
          continue;
        }
        bool hasKnownBaseOffset = gepObjOffsetFromBaseByObjId.count(objId) != 0;
        const llvm::Value *base = gep->getPointerOperand();
        while (const auto *gepOp = llvm::dyn_cast<llvm::GEPOperator>(base))
          base = gepOp->getPointerOperand();
        hasKnownBaseOffset = hasKnownBaseOffset ||
                             llvm::isa<llvm::AllocaInst>(base) ||
                             llvm::isa<llvm::GlobalValue>(base);
        const bool checkLowerBound = hasKnownBaseOffset;

        if (intervalIsOutsideObject(accessOffset, objSize, checkLowerBound)) {
          AEException bug("Buffer access outside object: offset [" +
                          accessOffset.toString() + "] exceeds object size " +
                          std::to_string(objSize));
          addBugToReporter(bug, inst);
        }
      }
    }
  }

  auto detectDirectMemoryAccess = [&](const llvm::Value *pointer,
                                      llvm::Type *valueType,
                                      const char *description) {
    if (!pointer || !valueType || !valueType->isSized())
      return;

    // Accesses through an explicit GEP are diagnosed at that GEP, which has
    // more precise provenance information.
    const llvm::Value *base = pointer->stripPointerCasts();
    if (llvm::isa<llvm::GEPOperator>(pointer) ||
        llvm::isa<llvm::GEPOperator>(base))
      return;

    uint32_t ptrId = AbstractInterpretation::getValueIdStatic(pointer);
    if (!as.inVarToAddrsTable(ptrId))
      return;

    for (uint32_t addr : as[ptrId].getAddrs()) {
      // Null, invalid, and freed pointers belong to the dedicated detectors.
      if (AbstractState::isNullMem(addr) || AbstractState::isInvalidMem(addr) ||
          as.isFreedMem(addr)) {
        return;
      }
    }

    uint64_t accessSize =
        inst->getModule()->getDataLayout().getTypeStoreSize(valueType);
    if (accessSize == 0)
      return;
    if (!canSafelyAccessMemory(as, ptrId,
                               byteCountToMaxAccessOffset(IntervalValue(
                                   static_cast<int64_t>(accessSize))))) {
      AEException bug(std::string("Buffer overflow at ") + description);
      addBugToReporter(bug, inst);
    }
  };

  if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(inst))
    detectDirectMemoryAccess(load->getPointerOperand(), load->getType(),
                             "load");
  else if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(inst))
    detectDirectMemoryAccess(store->getPointerOperand(),
                             store->getValueOperand()->getType(), "store");
  else if (const auto *atomic = llvm::dyn_cast<llvm::AtomicRMWInst>(inst))
    detectDirectMemoryAccess(atomic->getPointerOperand(),
                             atomic->getValOperand()->getType(), "atomicrmw");
  else if (const auto *cmpxchg = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(inst))
    detectDirectMemoryAccess(cmpxchg->getPointerOperand(),
                             cmpxchg->getCompareOperand()->getType(),
                             "cmpxchg");

  if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(inst))
    detectConstantExprAccess(as, inst, load->getPointerOperand());
  else if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(inst))
    detectConstantExprAccess(as, inst, store->getPointerOperand());

  // Check for buffer overflow in external API calls
  // Use annotation-based classification from AEExtAPI
  if (const auto *call = llvm::dyn_cast<llvm::CallBase>(inst)) {
    if (const llvm::Function *direct = call->getCalledFunction()) {
      AEExtAPI *utils = AbstractInterpretation::getAEInstance().getUtils();
      if (std::getenv("LOTUS_AE_DEBUG_SNPRINTF") &&
          direct->getName().contains("snprintf")) {
        llvm::errs() << "[SNP-CALL] " << direct->getName()
                     << " decl=" << direct->isDeclaration() << " type="
                     << (utils ? static_cast<int>(utils->getExtAPIType(direct))
                               : -1)
                     << "\n";
      }
      if (direct->isDeclaration() &&
          (utils == nullptr ||
           utils->getExtAPIType(direct) == AEExtAPI::MEMCPY ||
           utils->getExtAPIType(direct) == AEExtAPI::MEMSET ||
           utils->getExtAPIType(direct) == AEExtAPI::STRCPY ||
           utils->getExtAPIType(direct) == AEExtAPI::STRCAT ||
           utils->getExtAPIType(direct) == AEExtAPI::SNPRINTF ||
           utils->getExtAPIType(direct) == AEExtAPI::FGETS ||
           utils->getExtAPIType(direct) == AEExtAPI::FREAD ||
           utils->getExtAPIType(direct) == AEExtAPI::FWRITE ||
           utils->getExtAPIType(direct) == AEExtAPI::RECV ||
           direct->getName() == "read" || direct->getName() == "pread" ||
           direct->getName() == "write" || direct->getName() == "pwrite" ||
           direct->getName() == "send" || direct->getName() == "sendto" ||
           direct->getName() == "recvfrom" || direct->getName() == "readlink" ||
           direct->getName() == "strxfrm" || direct->getName() == "wcsxfrm")) {
        detectExtAPI(as, call, direct);
        return;
      }
    }

    // Get all possible callees (direct + indirect resolved via PTA)
    AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
    std::vector<const llvm::Function *> callees = ae.getCallees(call);

    for (const llvm::Function *callee : callees) {
      if (!callee)
        continue;
      AEExtAPI *utils = ae.getUtils();
      if (utils) {
        AEExtAPI::ExtAPIType extType = utils->getExtAPIType(callee);
        if (extType == AEExtAPI::MEMCPY || extType == AEExtAPI::MEMSET ||
            extType == AEExtAPI::STRCPY || extType == AEExtAPI::STRCAT ||
            extType == AEExtAPI::SNPRINTF || extType == AEExtAPI::FGETS ||
            extType == AEExtAPI::FREAD || extType == AEExtAPI::FWRITE ||
            extType == AEExtAPI::RECV || callee->getName() == "read" ||
            callee->getName() == "pread" || callee->getName() == "write" ||
            callee->getName() == "pwrite" || callee->getName() == "send" ||
            callee->getName() == "sendto" || callee->getName() == "recvfrom" ||
            callee->getName() == "readlink" || callee->getName() == "strxfrm" ||
            callee->getName() == "wcsxfrm") {
          detectExtAPI(as, call, callee);
          break;
        }
      } else {
        // Fallback to string matching if utils not available
        std::string funName = callee->getName().str();
        if (funName.find("memcpy") != std::string::npos ||
            funName.find("memmove") != std::string::npos ||
            funName.find("memset") != std::string::npos ||
            funName.find("strcpy") != std::string::npos ||
            funName.find("strcat") != std::string::npos ||
            funName.find("strncpy") != std::string::npos ||
            funName.find("strncat") != std::string::npos) {
          detectExtAPI(as, call, callee);
          break;
        }
      }
    }
  }
}

void BufOverflowDetector::detectConstantExprAccess(
    AbstractState &as, const llvm::Instruction *inst,
    const llvm::Value *pointer) {
  if (!pointer || !llvm::isa<llvm::ConstantExpr>(pointer))
    return;

  const llvm::DataLayout &dl = inst->getModule()->getDataLayout();
  const llvm::Value *base = pointer;
  IntervalValue totalOffset(0);
  bool sawGep = false;

  while (const auto *gep = llvm::dyn_cast<llvm::GEPOperator>(base)) {
    llvm::APInt constantOffset(64, 0);
    if (!gep->accumulateConstantOffset(dl, constantOffset))
      return;
    totalOffset = totalOffset + IntervalValue(constantOffset.getSExtValue());
    base = gep->getPointerOperand();
    sawGep = true;
  }
  if (!sawGep)
    return;

  uint32_t baseId = AbstractInterpretation::getValueIdStatic(base);
  if (!as.inVarToAddrsTable(baseId))
    return;

  for (uint32_t addr : as[baseId].getAddrs()) {
    uint32_t objId = as.getIDFromAddr(addr);
    uint32_t objSize = as.getObjSize(objId);
    if (intervalIsOutsideObject(totalOffset, objSize)) {
      AEException bug("Buffer access outside object: offset [" +
                      totalOffset.toString() + "] exceeds object size " +
                      std::to_string(objSize));
      addBugToReporter(bug, inst);
      return;
    }
  }
}

void BufOverflowDetector::handleStubFunctions(const llvm::CallBase *call) {
  if (!call->getCalledFunction())
    return;

  std::string funcName = call->getCalledFunction()->getName().str();

  if (funcName == "SAFE_BUFACCESS") {
    AbstractInterpretation::getAEInstance().markCheckpointChecked(call);
    AbstractInterpretation::getAEInstance().checkpoints.erase(call);
    if (call->arg_size() < 2)
      return;

    AbstractState &as =
        AbstractInterpretation::getAEInstance().getAbsStateFromTrace(call);

    uint32_t ptrId =
        AbstractInterpretation::getValueIdStatic(call->getArgOperand(0));
    uint32_t sizeId =
        AbstractInterpretation::getValueIdStatic(call->getArgOperand(1));

    IntervalValue size = as[sizeId].getInterval();
    if (size.isBottom()) {
      size = IntervalValue(0, 0);
    }

    bool isSafe = canSafelyAccessMemory(as, ptrId, size);

    if (isSafe) {
      llvm::outs()
          << "success: expected safe buffer access at SAFE_BUFACCESS - "
          << *call << "\n";
    } else {
      llvm::errs() << "failure: unexpected buffer overflow at SAFE_BUFACCESS\n";
      assert(false && "SAFE_BUFACCESS checkpoint failed");
    }
  } else if (funcName == "UNSAFE_BUFACCESS") {
    AbstractInterpretation::getAEInstance().markCheckpointChecked(call);
    AbstractInterpretation::getAEInstance().checkpoints.erase(call);
    if (call->arg_size() < 2)
      return;

    AbstractState &as =
        AbstractInterpretation::getAEInstance().getAbsStateFromTrace(call);

    uint32_t ptrId =
        AbstractInterpretation::getValueIdStatic(call->getArgOperand(0));
    uint32_t sizeId =
        AbstractInterpretation::getValueIdStatic(call->getArgOperand(1));

    IntervalValue size = as[sizeId].getInterval();
    if (size.isBottom()) {
      if (const auto *ci =
              llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1))) {
        size = IntervalValue(ci->getSExtValue(), ci->getSExtValue());
      } else {
        size = IntervalValue::top();
      }
    }

    bool isSafe = canSafelyAccessMemory(as, ptrId, size);

    if (!isSafe) {
      llvm::outs() << "success: expected buffer overflow at UNSAFE_BUFACCESS - "
                   << *call << "\n";
    } else {
      llvm::errs() << "failure: buffer overflow expected at UNSAFE_BUFACCESS, "
                      "but none detected\n";
      assert(false && "UNSAFE_BUFACCESS checkpoint failed");
    }
  }
}

void BufOverflowDetector::reportBug() {
  if (!instToBugInfo.empty()) {
    llvm::errs() << "###################### Buffer Overflow ("
                 << instToBugInfo.size() << " found) ######################\n";
    for (const auto &it : instToBugInfo) {
      llvm::errs() << it.second << "\n";
    }
  }
}

void BufOverflowDetector::reset() {
  clearEventTrace();
  bugLoc.clear();
  instToBugInfo.clear();
  gepObjOffsetFromBase.clear();
  gepObjOffsetFromBaseByObjId.clear();
  gepObjCapacityByObjId.clear();
}

void BufOverflowDetector::addBugToReporter(const AEException &e,
                                           const llvm::Instruction *inst) {
  std::string loc;
  if (const llvm::DILocation *debugLoc = inst->getDebugLoc()) {
    loc = debugLoc->getFilename().str() + ":" +
          std::to_string(debugLoc->getLine()) + ":" +
          std::to_string(debugLoc->getColumn());
  } else {
    loc = "unknown location";
  }

  if (bugLoc.find(loc) != bugLoc.end())
    return;

  bugLoc.insert(loc);
  instToBugInfo[inst] = std::string(e.what()) + " @ " + loc;
  emitAEBugReport(kind, inst, std::string(e.what()));
}

bool BufOverflowDetector::canSafelyAccessMemory(AbstractState &as,
                                                uint32_t ptrId,
                                                const IntervalValue &len) {
  auto checkAccessAgainstBase = [&](uint32_t basePtrId,
                                    const IntervalValue &accessOffset) {
    if (!as.inVarToAddrsTable(basePtrId))
      return true;

    const AbstractValue &baseVal = as[basePtrId];
    if (!baseVal.isAddr())
      return true;

    for (uint32_t addr : baseVal.getAddrs()) {
      if (AbstractState::isInvalidMem(addr) || AbstractState::isNullMem(addr))
        return false;

      uint32_t objId = as.getIDFromAddr(addr);
      uint32_t objSize = as.getObjSize(objId);
      auto capacityIt = gepObjCapacityByObjId.find(objId);
      if (capacityIt != gepObjCapacityByObjId.end())
        objSize = std::min(objSize, capacityIt->second);
      if (objSize == 0 && as.hasKnownObjSize(objId))
        return false;
      if (intervalIsOutsideObject(accessOffset, objSize)) {
        return false;
      }
    }
    return true;
  };

  const llvm::Value *ptrVal =
      AbstractInterpretation::getAEInstance().getValueFromIdStatic(ptrId);
  if (ptrVal) {
    const llvm::Value *baseVal = ptrVal;
    IntervalValue accumulatedOffset(0);
    bool hasExplicitOffset = false;
    uint32_t subobjectCapacity = 0;

    while (baseVal) {
      if (const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(baseVal)) {
        accumulatedOffset = accumulatedOffset + as.getByteOffset(gep);
        if (gep->getSourceElementType()->isStructTy() &&
            accumulatedOffset.is_numeral()) {
          llvm::Type *resultType = gep->getResultElementType();
          const auto *resultArrayType =
              llvm::dyn_cast_or_null<llvm::ArrayType>(resultType);
          const bool flexibleArray =
              resultArrayType && resultArrayType->getNumElements() == 0;
          if (resultType && resultType->isSized() && !flexibleArray) {
            uint64_t fieldEnd =
                static_cast<uint64_t>(
                    std::max<int64_t>(0, accumulatedOffset.getIntNumeral())) +
                gep->getModule()->getDataLayout().getTypeAllocSize(resultType);
            uint32_t capacity = fieldEnd > std::numeric_limits<uint32_t>::max()
                                    ? std::numeric_limits<uint32_t>::max()
                                    : static_cast<uint32_t>(fieldEnd);
            subobjectCapacity = subobjectCapacity == 0
                                    ? capacity
                                    : std::min(subobjectCapacity, capacity);
          }
        }
        baseVal = gep->getPointerOperand();
        hasExplicitOffset = true;
        continue;
      }
      if (const auto *bitcast =
              llvm::dyn_cast<llvm::BitCastOperator>(baseVal)) {
        baseVal = bitcast->getOperand(0);
        continue;
      }
      if (const auto *addrspaceCast =
              llvm::dyn_cast<llvm::AddrSpaceCastOperator>(baseVal)) {
        baseVal = addrspaceCast->getOperand(0);
        continue;
      }
      break;
    }

    if (hasExplicitOffset && baseVal) {
      if (subobjectCapacity > 0)
        return !intervalIsOutsideObject(accumulatedOffset + len,
                                        subobjectCapacity);
      uint32_t basePtrId = AbstractInterpretation::getValueIdStatic(baseVal);
      return checkAccessAgainstBase(basePtrId, accumulatedOffset + len);
    }
  }

  if (!as.inVarToAddrsTable(ptrId))
    return true;

  const AbstractValue &absVal = as[ptrId];
  if (!absVal.isAddr())
    return true;

  for (const auto &addr : absVal.getAddrs()) {
    if (AbstractState::isInvalidMem(addr))
      return false;
    if (AbstractState::isNullMem(addr))
      return false;

    uint32_t objId = as.getIDFromAddr(addr);
    uint32_t objSize = as.getObjSize(objId);
    auto capacityIt = gepObjCapacityByObjId.find(objId);
    if (capacityIt != gepObjCapacityByObjId.end())
      objSize = std::min(objSize, capacityIt->second);
    if (objSize == 0 && as.hasKnownObjSize(objId))
      return false;
    uint32_t effectiveCapacity = objSize;
    auto offsetIt = gepObjOffsetFromBaseByObjId.find(objId);
    if (offsetIt != gepObjOffsetFromBaseByObjId.end()) {
      int64_t baseOffset =
          std::max<int64_t>(0, offsetIt->second.ub().getIntNumeralOrZero());
      if (baseOffset < static_cast<int64_t>(objSize)) {
        effectiveCapacity = objSize - static_cast<uint32_t>(baseOffset);
      }
    }

    if (effectiveCapacity > 0 &&
        intervalIsOutsideObject(len, effectiveCapacity)) {
      return false;
    }
  }

  return true;
}

void BufOverflowDetector::initExtAPIBufOverflowCheckRules() {
  // Memory copy functions - check both destination and source
  extAPIBufOverflowCheckRules["llvm.memcpy.p0i8.p0i8.i64"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["llvm.memcpy.p0.p0.i64"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["llvm.memcpy.p0i8.p0i8.i32"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["llvm.memcpy.p0i8.p0i8.i16"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["llvm.memcpy.p0i8.p0i8.i8"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["llvm.memcpy"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["llvm.memmove"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["llvm.memmove.p0i8.p0i8.i64"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["llvm.memmove.p0.p0.i64"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["llvm.memmove.p0i8.p0i8.i32"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["__memcpy_chk"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["memmove"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["bcopy"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["memccpy"] = {{0, 3}, {1, 3}};
  extAPIBufOverflowCheckRules["__memmove_chk"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["__bcopy"] = {{0, 2}, {1, 2}};

  // Memory set functions - check destination
  extAPIBufOverflowCheckRules["llvm.memset.p0i8.i32"] = {{0, 2}};
  extAPIBufOverflowCheckRules["llvm.memset.p0i8.i64"] = {{0, 2}};
  extAPIBufOverflowCheckRules["llvm.memset.p0.i64"] = {{0, 2}};
  extAPIBufOverflowCheckRules["llvm.memset.p0i8.i8"] = {{0, 2}};
  extAPIBufOverflowCheckRules["llvm.memset"] = {{0, 2}};
  extAPIBufOverflowCheckRules["__memset_chk"] = {{0, 2}};
  extAPIBufOverflowCheckRules["wmemset"] = {{0, 2}};
  extAPIBufOverflowCheckRules["bzero"] = {{0, 1}};

  // String copy functions - check destination and source
  extAPIBufOverflowCheckRules["strcpy"] = {{0, 1}};
  extAPIBufOverflowCheckRules["strncpy"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["stpcpy"] = {{0, 1}};
  extAPIBufOverflowCheckRules["strcat"] = {{0, 1}};
  extAPIBufOverflowCheckRules["strncat"] = {{0, 2}};
  extAPIBufOverflowCheckRules["__strcpy_chk"] = {{0, 1}};
  extAPIBufOverflowCheckRules["__strncpy_chk"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["__strcat_chk"] = {{0, 1}};
  extAPIBufOverflowCheckRules["__strncat_chk"] = {{0, 2}};
  extAPIBufOverflowCheckRules["strxfrm"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["wcsxfrm"] = {{0, 2}, {1, 2}};

  // Wide string functions
  extAPIBufOverflowCheckRules["wcscpy"] = {{0, 1}};
  extAPIBufOverflowCheckRules["wcsncpy"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["wcscat"] = {{0, 1}};
  extAPIBufOverflowCheckRules["wcsncat"] = {{0, 2}};
  extAPIBufOverflowCheckRules["__wcscpy_chk"] = {{0, 1}};
  extAPIBufOverflowCheckRules["__wcsncpy_chk"] = {{0, 2}, {1, 2}};
  extAPIBufOverflowCheckRules["__wcscat_chk"] = {{0, 1}};
  extAPIBufOverflowCheckRules["__wcsncat_chk"] = {{0, 2}};

  // I/O functions
  extAPIBufOverflowCheckRules["fgets"] = {{0, 2}};
  extAPIBufOverflowCheckRules["fread"] = {{0, 2}};
  extAPIBufOverflowCheckRules["fwrite"] = {{0, 2}};
  extAPIBufOverflowCheckRules["recv"] = {{1, 2}};
  extAPIBufOverflowCheckRules["__recv"] = {{1, 2}};
  extAPIBufOverflowCheckRules["read"] = {{1, 2}};
  extAPIBufOverflowCheckRules["pread"] = {{1, 2}};
  extAPIBufOverflowCheckRules["write"] = {{1, 2}};
  extAPIBufOverflowCheckRules["pwrite"] = {{1, 2}};
  extAPIBufOverflowCheckRules["send"] = {{1, 2}};
  extAPIBufOverflowCheckRules["sendto"] = {{1, 2}};
  extAPIBufOverflowCheckRules["recvfrom"] = {{1, 2}};
  extAPIBufOverflowCheckRules["readlink"] = {{1, 2}};

  // iconv
  extAPIBufOverflowCheckRules["iconv"] = {{1, 2}, {3, 4}};
}

void BufOverflowDetector::detectExtAPI(AbstractState &as,
                                       const llvm::CallBase *call,
                                       const llvm::Function *callee) {
  callee = callee ? callee : call->getCalledFunction();
  if (!callee)
    return;

  std::string funcName = callee->getName().str();

  // First, check for BUF_CHECK annotations from AEExtAPI
  AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
  AEExtAPI *utils = ae.getUtils();
  if (utils) {
    std::vector<std::string> annotations = utils->getExtFuncAnnotations(callee);
    for (const auto &annotation : annotations) {
      if (annotation.find("BUF_CHECK:") == 0) {
        // Parse BUF_CHECK:ArgN,ArgM format
        std::string args = annotation.substr(10);
        size_t commaPos = args.find(',');
        if (commaPos != std::string::npos) {
          try {
            uint32_t bufArg = std::stoi(args.substr(0, commaPos));
            uint32_t sizeArg = std::stoi(args.substr(commaPos + 1));

            if (call->arg_size() > bufArg && call->arg_size() > sizeArg) {
              uint32_t bufId = AbstractInterpretation::getValueIdStatic(
                  call->getArgOperand(bufArg));
              uint32_t lenId = AbstractInterpretation::getValueIdStatic(
                  call->getArgOperand(sizeArg));

              IntervalValue len;
              if (const auto *csize = llvm::dyn_cast<llvm::ConstantInt>(
                      call->getArgOperand(sizeArg))) {
                len = IntervalValue(csize->getSExtValue());
              } else {
                if (!as.inVarToValTable(lenId))
                  continue;
                len = as[lenId].getInterval();
              }
              if (isZeroByteCount(len))
                continue;
              if (hasNullOrInvalidTarget(as, bufId))
                continue;
              if (!canSafelyAccessMemory(as, bufId,
                                         byteCountToMaxAccessOffset(len))) {
                AEException bug("Buffer overflow in " + funcName +
                                ": access length " + len.toString() +
                                " may exceed buffer bounds");
                addBugToReporter(bug, call);
              }
            }
          } catch (const std::invalid_argument &) {
            // Skip malformed BUF_CHECK annotation
            continue;
          } catch (const std::out_of_range &) {
            // Skip BUF_CHECK annotation with out-of-range values
            continue;
          }
        }
      }
    }
  }

  // Fallback to rules map
  if ((funcName.find("fread") != std::string::npos ||
       funcName.find("fwrite") != std::string::npos) &&
      call->arg_size() >= 3) {
    uint32_t bufId =
        AbstractInterpretation::getValueIdStatic(call->getArgOperand(0));
    IntervalValue total = getCallArgumentInterval(as, call->getArgOperand(1)) *
                          getCallArgumentInterval(as, call->getArgOperand(2));
    if (total.isBottom())
      return;
    if (isZeroByteCount(total))
      return;
    if (hasNullOrInvalidTarget(as, bufId))
      return;
    if (!canSafelyAccessMemory(as, bufId, byteCountToMaxAccessOffset(total))) {
      AEException bug("Buffer overflow in " + funcName +
                      ": total access size " + total.toString() +
                      " exceeds buffer bounds");
      addBugToReporter(bug, call);
    }
    return;
  }

  auto it = extAPIBufOverflowCheckRules.find(funcName);
  if (it != extAPIBufOverflowCheckRules.end()) {
    for (const auto &arg : it->second) {
      if (call->arg_size() <= arg.first || call->arg_size() <= arg.second)
        continue;

      uint32_t bufId = AbstractInterpretation::getValueIdStatic(
          call->getArgOperand(arg.first));
      uint32_t lenId = AbstractInterpretation::getValueIdStatic(
          call->getArgOperand(arg.second));

      IntervalValue len;
      if (const auto *csize = llvm::dyn_cast<llvm::ConstantInt>(
              call->getArgOperand(arg.second))) {
        len = IntervalValue(csize->getSExtValue());
      } else {
        if (!as.inVarToValTable(lenId))
          continue;
        len = as[lenId].getInterval();
      }
      if (isZeroByteCount(len))
        continue;
      if (hasNullOrInvalidTarget(as, bufId))
        continue;
      if (!canSafelyAccessMemory(as, bufId, byteCountToMaxAccessOffset(len))) {
        AEException bug("Buffer overflow in " + funcName + ": access length " +
                        len.toString() + " may exceed buffer bounds");
        addBugToReporter(bug, call);
      }
    }
  }

  // Check for string functions
  if (funcName.find("snprintf") != std::string::npos ||
      funcName.find("vsnprintf") != std::string::npos) {
    if (call->arg_size() >= 2) {
      uint32_t bufId =
          AbstractInterpretation::getValueIdStatic(call->getArgOperand(0));
      IntervalValue len;
      if (const auto *ci =
              llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1))) {
        len = IntervalValue(ci->getSExtValue());
      } else {
        uint32_t lenId =
            AbstractInterpretation::getValueIdStatic(call->getArgOperand(1));
        if (as.inVarToValTable(lenId))
          len = as[lenId].getInterval();
      }
      if (std::getenv("LOTUS_AE_DEBUG_SNPRINTF")) {
        llvm::errs() << "[SNPRINTF] " << funcName << " buf=" << bufId
                     << " hasBuf=" << as.inVarToAddrsTable(bufId)
                     << " len=" << len.toString() << " safe="
                     << canSafelyAccessMemory(as, bufId,
                                              byteCountToMaxAccessOffset(len))
                     << "\n";
      }
      if (!isZeroByteCount(len) && !len.isBottom() &&
          !hasNullOrInvalidTarget(as, bufId) &&
          !canSafelyAccessMemory(as, bufId, byteCountToMaxAccessOffset(len))) {
        AEException bug("Buffer overflow in " + funcName + ": size limit " +
                        len.toString() + " exceeds destination buffer bounds");
        addBugToReporter(bug, call);
      }
    }
  } else if (funcName.find("strcpy") != std::string::npos) {
    detectStrcpy(as, call, callee);
  } else if (funcName.find("strcat") != std::string::npos ||
             funcName.find("strncat") != std::string::npos) {
    detectStrcat(as, call, callee);
  }
}

bool BufOverflowDetector::detectStrcpy(AbstractState &as,
                                       const llvm::CallBase *call,
                                       const llvm::Function *callee) {
  (void)callee;
  if (call->arg_size() < 2)
    return true;

  uint32_t dstId =
      AbstractInterpretation::getValueIdStatic(call->getArgOperand(0));
  uint32_t srcId =
      AbstractInterpretation::getValueIdStatic(call->getArgOperand(1));

  if (!as.inVarToAddrsTable(dstId) || !as.inVarToAddrsTable(srcId))
    return true;

  // Get source string length using getStrlen utility
  AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
  AEExtAPI *utils = ae.getUtils();
  if (!utils) {
    // Fallback to conservative estimate if utils not available
    IntervalValue srcLen(0, 1024);
    uint32_t dstSize =
        getRemainingByteCapacity(as, dstId, gepObjOffsetFromBaseByObjId);
    if (dstSize > 0 &&
        srcLen.ub().getIntNumeral() > static_cast<int64_t>(dstSize)) {
      AEException bug("Buffer overflow in strcpy: source string length may "
                      "exceed destination buffer size");
      addBugToReporter(bug, call);
      return false;
    }
    return true;
  }

  IntervalValue srcLen = utils->getStrlen(as, srcId);

  uint32_t dstSize =
      getRemainingByteCapacity(as, dstId, gepObjOffsetFromBaseByObjId);

  // Check if source string length exceeds destination buffer size
  // Account for null terminator: need dstSize >= srcLen + 1
  if (dstSize > 0) {
    int64_t requiredSize = srcLen.ub().getIntNumeral() + 1;
    if (requiredSize > static_cast<int64_t>(dstSize)) {
      AEException bug("Buffer overflow in strcpy: source string length [" +
                      srcLen.toString() +
                      "] + 1 exceeds destination buffer "
                      "size " +
                      std::to_string(dstSize));
      addBugToReporter(bug, call);
      return false;
    }
  }

  return true;
}

bool BufOverflowDetector::detectStrcat(AbstractState &as,
                                       const llvm::CallBase *call,
                                       const llvm::Function *callee) {
  if (call->arg_size() < 2)
    return true;

  uint32_t dstId =
      AbstractInterpretation::getValueIdStatic(call->getArgOperand(0));
  uint32_t srcId =
      AbstractInterpretation::getValueIdStatic(call->getArgOperand(1));

  if (!as.inVarToAddrsTable(dstId) || !as.inVarToAddrsTable(srcId))
    return true;

  // Get string lengths using getStrlen utility
  AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
  AEExtAPI *utils = ae.getUtils();
  if (!utils) {
    // Fallback to conservative estimate if utils not available
    IntervalValue dstLen(0, 512);
    IntervalValue srcLen(0, 512);
    uint32_t dstSize =
        getRemainingByteCapacity(as, dstId, gepObjOffsetFromBaseByObjId);
    IntervalValue totalLen = dstLen + srcLen;
    if (dstSize > 0 &&
        totalLen.ub().getIntNumeral() > static_cast<int64_t>(dstSize)) {
      AEException bug(
          "Buffer overflow in strcat: concatenated string may exceed "
          "destination buffer size");
      addBugToReporter(bug, call);
      return false;
    }
    return true;
  }

  IntervalValue dstLen = utils->getStrlen(as, dstId);
  IntervalValue srcLen = utils->getStrlen(as, srcId);

  uint32_t dstSize =
      getRemainingByteCapacity(as, dstId, gepObjOffsetFromBaseByObjId);

  // Check if concatenated string length exceeds destination buffer size.
  // strncat-like APIs use the explicit length bound instead of strlen(src).
  IntervalValue totalLen = dstLen + srcLen;
  if (callee) {
    const std::string funcName = callee->getName().str();
    if (funcName.find("strncat") != std::string::npos &&
        call->arg_size() >= 3) {
      uint32_t lenId =
          AbstractInterpretation::getValueIdStatic(call->getArgOperand(2));
      if (as.inVarToValTable(lenId)) {
        totalLen = dstLen + as[lenId].getInterval();
      } else if (const auto *ci = llvm::dyn_cast<llvm::ConstantInt>(
                     call->getArgOperand(2))) {
        totalLen = dstLen + IntervalValue(ci->getSExtValue());
      }
    }
  }

  // Account for null terminator: need dstSize >= totalLen + 1
  if (dstSize > 0) {
    int64_t requiredSize = totalLen.ub().getIntNumeral() + 1;
    if (requiredSize > static_cast<int64_t>(dstSize)) {
      AEException bug(
          "Buffer overflow in strcat: concatenated string length [" +
          totalLen.toString() +
          "] + 1 exceeds destination buffer "
          "size " +
          std::to_string(dstSize));
      addBugToReporter(bug, call);
      return false;
    }
  }

  return true;
}

/// @brief Detects null pointer dereference issues.
///
/// Checks load instructions, store instructions, and GEP instructions for
/// potential null pointer dereferences by analyzing whether pointers can
/// point to null or invalid memory.
///
/// @param as Reference to the abstract state.
/// @param inst Pointer to the instruction to analyze.
void NullptrDerefDetector::detect(AbstractState &as,
                                  const llvm::Instruction *inst) {
  auto hasDefiniteNonNullBase = [](const llvm::Value *ptrVal) -> bool {
    const llvm::Value *base = ptrVal;
    while (true) {
      if (const auto *gep = llvm::dyn_cast<llvm::GEPOperator>(base)) {
        base = gep->getPointerOperand();
        continue;
      }
      if (const auto *cast = llvm::dyn_cast<llvm::BitCastOperator>(base)) {
        base = cast->getOperand(0);
        continue;
      }
      if (const auto *asc = llvm::dyn_cast<llvm::AddrSpaceCastOperator>(base)) {
        base = asc->getOperand(0);
        continue;
      }
      break;
    }
    return llvm::isa<llvm::AllocaInst>(base) ||
           llvm::isa<llvm::GlobalValue>(base);
  };

  // Check for null pointer dereference in load instructions
  if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(inst)) {
    if (hasDefiniteNonNullBase(load->getPointerOperand()))
      return;
    uint32_t ptrId =
        AbstractInterpretation::getValueIdStatic(load->getPointerOperand());

    // Check if pointer operand is a null constant
    const llvm::Value *ptrVal = load->getPointerOperand();
    bool isNullConstant = llvm::isa<llvm::ConstantPointerNull>(ptrVal);

    bool isSafe = canSafelyDerefPtr(as, ptrId);
    if (!isSafe) {
      std::string bugMsg = "Null pointer dereference at load instruction";
      if (isNullConstant) {
        bugMsg += " (null constant)";
      }
      AEException bug(bugMsg);
      addBugToReporter(bug, inst);
    }
  }

  // Check for null pointer dereference in store instructions
  if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(inst)) {
    if (hasDefiniteNonNullBase(store->getPointerOperand()))
      return;
    uint32_t ptrId =
        AbstractInterpretation::getValueIdStatic(store->getPointerOperand());

    // Check if pointer operand is a null constant
    const llvm::Value *ptrVal = store->getPointerOperand();
    bool isNullConstant = llvm::isa<llvm::ConstantPointerNull>(ptrVal);

    bool isSafe = canSafelyDerefPtr(as, ptrId);
    if (!isSafe) {
      std::string bugMsg = "Null pointer dereference at store instruction";
      if (isNullConstant) {
        bugMsg += " (null constant)";
      }
      AEException bug(bugMsg);
      addBugToReporter(bug, inst);
    }
  }

  // Check for null pointer dereference in GEP instructions
  if (const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(inst)) {
    if (hasDefiniteNonNullBase(gep->getPointerOperand()))
      return;
    uint32_t ptrId =
        AbstractInterpretation::getValueIdStatic(gep->getPointerOperand());
    if (!canSafelyDerefPtr(as, ptrId)) {
      AEException bug("Null pointer dereference at GEP instruction");
      addBugToReporter(bug, inst);
    }
  }

  if (const auto *atomic = llvm::dyn_cast<llvm::AtomicRMWInst>(inst)) {
    if (!hasDefiniteNonNullBase(atomic->getPointerOperand())) {
      uint32_t ptrId =
          AbstractInterpretation::getValueIdStatic(atomic->getPointerOperand());
      if (!canSafelyDerefPtr(as, ptrId)) {
        AEException bug("Null pointer dereference at atomicrmw instruction");
        addBugToReporter(bug, inst);
      }
    }
  } else if (const auto *cmpxchg =
                 llvm::dyn_cast<llvm::AtomicCmpXchgInst>(inst)) {
    if (!hasDefiniteNonNullBase(cmpxchg->getPointerOperand())) {
      uint32_t ptrId = AbstractInterpretation::getValueIdStatic(
          cmpxchg->getPointerOperand());
      if (!canSafelyDerefPtr(as, ptrId)) {
        AEException bug("Null pointer dereference at cmpxchg instruction");
        addBugToReporter(bug, inst);
      }
    }
  }

  if (const auto *call = llvm::dyn_cast<llvm::CallBase>(inst)) {
    AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
    std::vector<const llvm::Function *> callees = ae.getCallees(call);

    for (const llvm::Function *callee : callees) {
      if (!callee)
        continue;
      AEExtAPI *utils = ae.getUtils();
      bool shouldCheck = false;
      const std::string funcName = callee->getName().str();
      const bool nameBasedCheck =
          funcName == "memcmp" || funcName == "strcmp" ||
          funcName == "strncmp" || funcName == "memchr" || funcName == "puts" ||
          funcName == "fputs" || funcName == "strdup" ||
          funcName == "strndup" || funcName == "strxfrm" ||
          funcName == "wcsxfrm" || funcName == "strcoll" ||
          funcName == "strpbrk" || funcName == "strspn" ||
          funcName == "strcspn" || funcName == "posix_memalign" ||
          funcName == "asprintf" || funcName == "vasprintf" ||
          funcName == "getline" || funcName == "getdelim" ||
          funcName == "read" || funcName == "pread" || funcName == "write" ||
          funcName == "pwrite" || funcName == "send" || funcName == "sendto" ||
          funcName == "recvfrom" || funcName == "readlink";

      if (utils) {
        AEExtAPI::ExtAPIType extType = utils->getExtAPIType(callee);
        shouldCheck =
            (extType == AEExtAPI::MEMCPY || extType == AEExtAPI::MEMSET ||
             extType == AEExtAPI::STRCPY || extType == AEExtAPI::STRCAT ||
             extType == AEExtAPI::RECV || extType == AEExtAPI::FGETS ||
             extType == AEExtAPI::FREAD || extType == AEExtAPI::FWRITE ||
             extType == AEExtAPI::STRLEN || extType == AEExtAPI::STRCHR ||
             extType == AEExtAPI::STRSTR || extType == AEExtAPI::SCANF ||
             extType == AEExtAPI::PRINTF || extType == AEExtAPI::SNPRINTF ||
             extType == AEExtAPI::ITOA || extType == AEExtAPI::STRTO ||
             extType == AEExtAPI::ENV || nameBasedCheck);
      } else {
        // Fallback to string matching
        shouldCheck =
            (funcName.find("memcpy") != std::string::npos ||
             funcName.find("memset") != std::string::npos ||
             funcName.find("strcpy") != std::string::npos ||
             funcName.find("strcat") != std::string::npos ||
             funcName.find("recv") != std::string::npos ||
             funcName.find("fread") != std::string::npos ||
             funcName.find("fwrite") != std::string::npos ||
             funcName.find("fgets") != std::string::npos ||
             funcName.find("strlen") != std::string::npos ||
             funcName.find("strchr") != std::string::npos ||
             funcName.find("strstr") != std::string::npos ||
             funcName.find("scanf") != std::string::npos ||
             funcName.find("printf") != std::string::npos || nameBasedCheck);
      }

      if (shouldCheck) {
        detectExtAPI(as, call, callee);
        break;
      }
    }
  }
}

void NullptrDerefDetector::detectExtAPI(AbstractState &as,
                                        const llvm::CallBase *call,
                                        const llvm::Function *callee) {
  callee = callee ? callee : call->getCalledFunction();
  if (!callee)
    return;

  AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
  AEExtAPI *utils = ae.getUtils();

  std::vector<uint32_t> pointerArgs =
      getExtAPIPointerArgIndices(call, callee, utils);

  if (pointerArgs.empty()) {
    return;
  }

  for (uint32_t argIdx : pointerArgs) {
    if (call->arg_size() <= argIdx)
      continue;
    uint32_t argId =
        AbstractInterpretation::getValueIdStatic(call->getArgOperand(argIdx));
    if (!canSafelyDerefPtr(as, argId)) {
      AEException bug("Null pointer dereference in " + callee->getName().str() +
                      " argument " + std::to_string(argIdx));
      addBugToReporter(bug, call);
    }
  }
}

void NullptrDerefDetector::handleStubFunctions(const llvm::CallBase *call) {
  if (!call->getCalledFunction())
    return;

  std::string funcName = call->getCalledFunction()->getName().str();

  if (funcName == "SAFE_LOAD") {
    AbstractInterpretation::getAEInstance().markCheckpointChecked(call);
    AbstractInterpretation::getAEInstance().checkpoints.erase(call);
    if (call->arg_size() < 1)
      return;

    AbstractState &as =
        AbstractInterpretation::getAEInstance().getAbsStateFromTrace(call);
    uint32_t ptrId =
        AbstractInterpretation::getValueIdStatic(call->getArgOperand(0));

    bool isSafe = canSafelyDerefPtr(as, ptrId);
    if (isSafe) {
      llvm::outs() << "success: expected safe dereference at SAFE_LOAD - "
                   << *call << "\n";
    } else {
      llvm::errs() << "failure: unexpected null dereference at SAFE_LOAD\n";
      assert(false && "SAFE_LOAD checkpoint failed");
    }
  } else if (funcName == "UNSAFE_LOAD") {
    AbstractInterpretation::getAEInstance().markCheckpointChecked(call);
    AbstractInterpretation::getAEInstance().checkpoints.erase(call);
    if (call->arg_size() < 1)
      return;

    AbstractState &as =
        AbstractInterpretation::getAEInstance().getAbsStateFromTrace(call);
    uint32_t ptrId =
        AbstractInterpretation::getValueIdStatic(call->getArgOperand(0));

    bool isSafe = canSafelyDerefPtr(as, ptrId);
    if (!isSafe) {
      llvm::outs() << "success: expected null dereference at UNSAFE_LOAD - "
                   << *call << "\n";
    } else {
      llvm::errs() << "failure: null dereference expected at UNSAFE_LOAD, but "
                      "none detected\n";
      assert(false && "UNSAFE_LOAD checkpoint failed");
    }
  }
}

void NullptrDerefDetector::reportBug() {
  if (!instToBugInfo.empty()) {
    llvm::errs() << "###################### Nullptr Dereference ("
                 << instToBugInfo.size() << " found) ######################\n";
    for (const auto &it : instToBugInfo) {
      llvm::errs() << it.second << "\n";
    }
  } else {
    llvm::errs() << "###################### Nullptr Dereference (0 found) "
                    "######################\n";
  }
}

void NullptrDerefDetector::reset() {
  clearEventTrace();
  bugLoc.clear();
  instToBugInfo.clear();
}

void NullptrDerefDetector::addBugToReporter(const AEException &e,
                                            const llvm::Instruction *inst) {
  std::string loc;
  if (const llvm::DILocation *debugLoc = inst->getDebugLoc()) {
    loc = debugLoc->getFilename().str() + ":" +
          std::to_string(debugLoc->getLine()) + ":" +
          std::to_string(debugLoc->getColumn());
  } else {
    // Fallback location for stripped/no-debug IR: use a stable
    // per-instruction textual key so multiple unknown-location bugs do not
    // collapse into one.
    std::string instStr;
    llvm::raw_string_ostream os(instStr);
    inst->print(os);
    os.flush();
    const llvm::Function *func = inst->getFunction();
    const llvm::BasicBlock *bb = inst->getParent();
    loc = (func ? func->getName().str() : "unknown_function") +
          "::" + (bb && bb->hasName() ? bb->getName().str() : "unknown_bb") +
          "::" + std::to_string(inst->getOpcode()) + "::" + instStr;
  }

  if (bugLoc.find(loc) != bugLoc.end())
    return;

  bugLoc.insert(loc);
  instToBugInfo[inst] = std::string(e.what()) + " @ " + loc;
  emitAEBugReport(kind, inst, std::string(e.what()));
}

void DivZeroDetector::detect(AbstractState &as, const llvm::Instruction *inst) {
  const auto *bin = llvm::dyn_cast<llvm::BinaryOperator>(inst);
  if (!bin)
    return;

  switch (bin->getOpcode()) {
  case llvm::Instruction::SDiv:
  case llvm::Instruction::UDiv:
  case llvm::Instruction::FDiv:
  case llvm::Instruction::SRem:
  case llvm::Instruction::URem:
  case llvm::Instruction::FRem:
    break;
  default:
    return;
  }

  if (!mayDivideByZero(as, bin->getOperand(1)))
    return;

  AEException bug("Possible divide by zero");
  addBugToReporter(bug, inst);
}

void DivZeroDetector::handleStubFunctions(const llvm::CallBase *call) {
  const llvm::Function *callee = call->getCalledFunction();
  if (!callee)
    return;

  const std::string funcName = callee->getName().str();
  if (funcName != "SAFE_DIVZERO" && funcName != "UNSAFE_DIVZERO")
    return;

  AbstractInterpretation::getAEInstance().markCheckpointChecked(call);
  AbstractInterpretation::getAEInstance().checkpoints.erase(call);

  if (call->arg_empty())
    return;

  const llvm::Value *divisor =
      call->getArgOperand(call->arg_size() >= 2 ? 1 : 0);
  AbstractState &as =
      AbstractInterpretation::getAEInstance().getAbsStateFromTrace(call);
  bool unsafe = mayDivideByZero(as, divisor);

  if (funcName == "SAFE_DIVZERO") {
    if (unsafe) {
      llvm::errs() << "failure: unexpected divide-by-zero at SAFE_DIVZERO\n";
      assert(false && "SAFE_DIVZERO checkpoint failed");
    }
    llvm::outs() << "success: expected safe divide-by-zero checkpoint at "
                 << "SAFE_DIVZERO - " << *call << "\n";
    return;
  }

  if (!unsafe) {
    llvm::errs() << "failure: divide-by-zero expected at UNSAFE_DIVZERO, "
                    "but none detected\n";
    assert(false && "UNSAFE_DIVZERO checkpoint failed");
  }
  llvm::outs() << "success: expected divide-by-zero at UNSAFE_DIVZERO - "
               << *call << "\n";
}

void DivZeroDetector::reportBug() {
  if (instToBugInfo.empty())
    return;

  llvm::errs() << "###################### Divide By Zero ("
               << instToBugInfo.size() << " found) ######################\n";
  for (const auto &it : instToBugInfo) {
    llvm::errs() << it.second << "\n";
  }
}

void DivZeroDetector::reset() {
  AEDetector::reset();
  bugLoc.clear();
  instToBugInfo.clear();
}

bool DivZeroDetector::mayDivideByZero(AbstractState &as,
                                      const llvm::Value *divisor) const {
  if (!divisor)
    return false;

  uint32_t divisorId = AbstractInterpretation::getValueIdStatic(divisor);
  if (!as.inVarToValTable(divisorId)) {
    if (const auto *ci = llvm::dyn_cast<llvm::ConstantInt>(divisor)) {
      as[divisorId] = AbstractValue(IntervalValue(ci->getSExtValue()));
    } else if (const auto *cfp = llvm::dyn_cast<llvm::ConstantFP>(divisor)) {
      double num = cfp->getValueAPF().convertToDouble();
      as[divisorId] = AbstractValue(
          IntervalValue(static_cast<int64_t>(num), static_cast<int64_t>(num)));
    }
  }
  if (!as.inVarToValTable(divisorId))
    return true;

  return as[divisorId].getInterval().contains(0);
}

void DivZeroDetector::addBugToReporter(const AEException &e,
                                       const llvm::Instruction *inst) {
  std::string loc;
  if (const llvm::DILocation *debugLoc = inst->getDebugLoc()) {
    loc = debugLoc->getFilename().str() + ":" +
          std::to_string(debugLoc->getLine());
  } else {
    std::string instStr;
    llvm::raw_string_ostream os(instStr);
    os << *inst;
    os.flush();
    const llvm::Function *func = inst->getFunction();
    const llvm::BasicBlock *bb = inst->getParent();
    loc = (func ? func->getName().str() : "unknown_function") +
          "::" + (bb && bb->hasName() ? bb->getName().str() : "unknown_bb") +
          "::" + std::to_string(inst->getOpcode()) + "::" + instStr;
  }

  if (!bugLoc.insert(loc).second)
    return;

  instToBugInfo[inst] = std::string(e.what()) + " @ " + loc;
  emitAEBugReport(kind, inst, std::string(e.what()));
}

void OverflowDetector::detect(AbstractState &as,
                              const llvm::Instruction *inst) {
  if (!mayOverflow(as, inst))
    return;

  AEException bug("Possible integer overflow");
  addBugToReporter(bug, inst);
}

void OverflowDetector::handleStubFunctions(const llvm::CallBase *call) {
  const llvm::Function *callee = call->getCalledFunction();
  if (!callee)
    return;

  const std::string funcName = callee->getName().str();
  if (funcName != "SAFE_OVERFLOW" && funcName != "UNSAFE_OVERFLOW")
    return;

  AbstractInterpretation::getAEInstance().markCheckpointChecked(call);
  AbstractInterpretation::getAEInstance().checkpoints.erase(call);

  if (funcName == "SAFE_OVERFLOW") {
    llvm::outs() << "success: overflow checkpoint acknowledged at "
                 << "SAFE_OVERFLOW - " << *call << "\n";
  } else {
    llvm::outs() << "success: overflow checkpoint acknowledged at "
                 << "UNSAFE_OVERFLOW - " << *call << "\n";
  }
}

void OverflowDetector::reportBug() {
  if (instToBugInfo.empty())
    return;

  llvm::errs() << "###################### Integer Overflow ("
               << instToBugInfo.size() << " found) ######################\n";
  for (const auto &it : instToBugInfo) {
    llvm::errs() << it.second << "\n";
  }
}

void OverflowDetector::reset() {
  AEDetector::reset();
  bugLoc.clear();
  instToBugInfo.clear();
}

bool OverflowDetector::mayOverflow(AbstractState &as,
                                   const llvm::Instruction *inst) const {
  const auto *bin = llvm::dyn_cast<llvm::BinaryOperator>(inst);
  if (!bin || !bin->getType()->isIntegerTy())
    return false;

  switch (bin->getOpcode()) {
  case llvm::Instruction::Add:
  case llvm::Instruction::Sub:
  case llvm::Instruction::Mul:
  case llvm::Instruction::Shl:
    break;
  default:
    return false;
  }

  uint32_t lhsId = AbstractInterpretation::getValueIdStatic(inst);
  auto it = as.getVarToVal().find(lhsId);
  if (it == as.getVarToVal().end() || !it->second.isInterval())
    return false;

  AEExtAPI *utils = AbstractInterpretation::getAEInstance().getUtils();
  if (!utils)
    return false;

  IntervalValue typeRange =
      utils->getRangeLimitFromType(const_cast<llvm::Type *>(bin->getType()));
  if (typeRange.isTop())
    return false;

  return !typeRange.contain(it->second.getInterval());
}

void OverflowDetector::addBugToReporter(const AEException &e,
                                        const llvm::Instruction *inst) {
  std::string loc;
  if (const llvm::DILocation *debugLoc = inst->getDebugLoc()) {
    loc = debugLoc->getFilename().str() + ":" +
          std::to_string(debugLoc->getLine());
  } else {
    std::string instStr;
    llvm::raw_string_ostream os(instStr);
    os << *inst;
    os.flush();
    const llvm::Function *func = inst->getFunction();
    const llvm::BasicBlock *bb = inst->getParent();
    loc = (func ? func->getName().str() : "unknown_function") +
          "::" + (bb && bb->hasName() ? bb->getName().str() : "unknown_bb") +
          "::" + std::to_string(inst->getOpcode()) + "::" + instStr;
  }

  if (!bugLoc.insert(loc).second)
    return;

  instToBugInfo[inst] = std::string(e.what()) + " @ " + loc;
  emitAEBugReport(kind, inst, std::string(e.what()));
}

bool NullptrDerefDetector::canSafelyDerefPtr(AbstractState &as,
                                             uint32_t ptrId) {
  // Special case: if ptrId is 0 (NullPtr), check if it's a null constant
  // Check if the value exists in the abstract state
  bool hasValue = (as._varToAbsVal.find(ptrId) != as._varToAbsVal.end());

  // If ptrId is 0 (null pointer constant) and not in state, it's null
  if (ptrId == 0 && !hasValue) {
    return false; // Null pointer cannot be safely dereferenced
  }

  AbstractValue absVal = as[ptrId];

  // Uninitialized value cannot be dereferenced
  if (isUninit(absVal))
    return false;

  // Interval value (non-addr) is safe
  if (!absVal.isAddr())
    return true;

  // Check each address
  for (const auto &addr : absVal.getAddrs()) {
    if (AbstractState::isNullMem(addr))
      return false;
    // Black-hole/invalid and freed addresses are not evidence of null.
    // Use-after free and invalid-free findings are diagnosed by their dedicated
    // detectors.
    if (as.isFreedMem(addr))
      return false;
    if (AbstractState::isInvalidMem(addr))
      continue;
  }

  return true;
}

IntervalValue
BufOverflowDetector::getAccessOffset(AbstractState &as, uint32_t objId,
                                     const llvm::GetElementPtrInst *gep) {
  // Get the offset from the GEP instruction
  IntervalValue offset = as.getByteOffset(gep);

  // Direct GEP chains retain their accumulated offset in the per-instruction
  // cache.
  if (const auto *prevGep =
          llvm::dyn_cast<llvm::GetElementPtrInst>(gep->getPointerOperand())) {
    if (hasGepObjOffsetFromBase(prevGep)) {
      IntervalValue prevOffset = getGepObjOffsetFromBase(prevGep);
      return prevOffset + offset;
    }
  }

  // The pointer operand may have come through a load or select. In that case
  // its provenance is only available through the object it names: add the
  // object's offset-from-base to this GEP's local byte offset.
  IntervalValue stateOffset;
  if (as.getGepObjOffsetFromBase(objId, stateOffset))
    return stateOffset + offset;
  auto objIt = gepObjOffsetFromBaseByObjId.find(objId);
  if (objIt != gepObjOffsetFromBaseByObjId.end()) {
    return objIt->second + offset;
  }

  return offset;
}

void BufOverflowDetector::updateGepObjOffsetFromBase(AbstractState &as,
                                                     AddressValue gepAddrs,
                                                     AddressValue objAddrs,
                                                     IntervalValue offset) {
  // Preserve SVF-like object-based propagation:
  // - Base object: gep offset = current offset
  // - GEP object:  gep offset = base offset + current offset
  for (const auto &objAddr : objAddrs) {
    uint32_t baseObjId = as.getIDFromAddr(objAddr);
    IntervalValue baseOffset(0, 0);
    auto baseIt = gepObjOffsetFromBaseByObjId.find(baseObjId);
    if (baseIt != gepObjOffsetFromBaseByObjId.end()) {
      baseOffset = baseIt->second;
    }

    IntervalValue accumulated = baseOffset + offset;
    for (const auto &gepAddr : gepAddrs) {
      uint32_t gepObjId = as.getIDFromAddr(gepAddr);
      auto it = gepObjOffsetFromBaseByObjId.find(gepObjId);
      if (it == gepObjOffsetFromBaseByObjId.end()) {
        gepObjOffsetFromBaseByObjId.emplace(gepObjId, accumulated);
      } else {
        it->second.join_with(accumulated);
      }
    }
  }
}

void BufOverflowDetector::addToGepObjOffsetFromBase(
    const llvm::GetElementPtrInst *gep, const IntervalValue &offset) {
  gepObjOffsetFromBase[gep] = offset;
}

bool BufOverflowDetector::hasGepObjOffsetFromBase(
    const llvm::GetElementPtrInst *gep) const {
  return gepObjOffsetFromBase.find(gep) != gepObjOffsetFromBase.end();
}

IntervalValue BufOverflowDetector::getGepObjOffsetFromBase(
    const llvm::GetElementPtrInst *gep) const {
  auto it = gepObjOffsetFromBase.find(gep);
  if (it != gepObjOffsetFromBase.end()) {
    return it->second;
  }
  return IntervalValue(0, 0);
}

//===----------------------------------------------------------------------===//
// UseAfterFreeDetector Implementation
//===----------------------------------------------------------------------===//

void UseAfterFreeDetector::detect(AbstractState &as,
                                  const llvm::Instruction *inst) {
  // Check for loads from freed memory
  if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(inst)) {
    uint32_t ptrId =
        AbstractInterpretation::getValueIdStatic(load->getPointerOperand());

    if (mayAccessFreedMem(as, ptrId)) {
      addEventToTrace(AEBugEventType::LOAD, inst, "Load from freed memory");
      AEException bug("Use-after-free: load from freed memory");
      addBugToReporter(bug, inst);
    }
  }

  // Check for stores to freed memory
  if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(inst)) {
    uint32_t ptrId =
        AbstractInterpretation::getValueIdStatic(store->getPointerOperand());

    if (mayAccessFreedMem(as, ptrId)) {
      addEventToTrace(AEBugEventType::STORE, inst, "Store to freed memory");
      AEException bug("Use-after-free: store to freed memory");
      addBugToReporter(bug, inst);
    }
  }

  // Check for GEP on freed pointers
  if (const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(inst)) {
    uint32_t ptrId =
        AbstractInterpretation::getValueIdStatic(gep->getPointerOperand());

    if (mayAccessFreedMem(as, ptrId)) {
      addEventToTrace(AEBugEventType::DEREF, inst, "GEP on freed memory");
      AEException bug("Use-after-free: GEP on freed memory");
      addBugToReporter(bug, inst);
    }
  }
}

void UseAfterFreeDetector::handleStubFunctions(const llvm::CallBase *call) {
  (void)call;
  // Track allocation/free events
}

void UseAfterFreeDetector::reportBug() {
  if (!instToBugInfo.empty()) {
    llvm::errs() << "###################### Use-After-Free ("
                 << instToBugInfo.size() << " found) ######################\n";
    for (const auto &it : instToBugInfo) {
      llvm::errs() << it.second << "\n";
    }
  }
}

void UseAfterFreeDetector::reset() {
  clearEventTrace();
  bugLoc.clear();
  instToBugInfo.clear();
}

bool UseAfterFreeDetector::mayAccessFreedMem(AbstractState &as,
                                             uint32_t ptrId) {
  if (!as.inVarToAddrsTable(ptrId))
    return false;

  const AbstractValue &absVal = as[ptrId];
  if (!absVal.isAddr())
    return false;

  for (const auto &addr : absVal.getAddrs()) {
    if (as.isFreedMem(addr))
      return true;
  }

  return false;
}

void UseAfterFreeDetector::addBugToReporter(const AEException &e,
                                            const llvm::Instruction *inst) {
  std::string loc;
  if (const llvm::DILocation *debugLoc = inst->getDebugLoc()) {
    loc = debugLoc->getFilename().str() + ":" +
          std::to_string(debugLoc->getLine());
  } else {
    std::string instStr;
    llvm::raw_string_ostream os(instStr);
    inst->print(os);
    os.flush();
    const llvm::Function *func = inst->getFunction();
    const llvm::BasicBlock *bb = inst->getParent();
    loc = (func ? func->getName().str() : "unknown_function") +
          "::" + (bb && bb->hasName() ? bb->getName().str() : "unknown_bb") +
          "::" + std::to_string(inst->getOpcode()) + "::" + instStr;
  }

  if (bugLoc.find(loc) != bugLoc.end())
    return;

  bugLoc.insert(loc);
  instToBugInfo[inst] = std::string(e.what()) + " @ " + loc;
  emitAEBugReport(kind, inst, std::string(e.what()));
}

//===----------------------------------------------------------------------===//
// InvalidFreeDetector Implementation
//===----------------------------------------------------------------------===//

void InvalidFreeDetector::detect(AbstractState &as,
                                 const llvm::Instruction *inst) {
  // Check for free() calls
  if (const auto *call = llvm::dyn_cast<llvm::CallBase>(inst)) {
    if (const llvm::Function *callee = call->getCalledFunction()) {
      std::string funcName = callee->getName().str();
      if (funcName == "free" || funcName == "cfree" ||
          funcName == "malloc_free") {
        if (call->arg_size() >= 1) {
          uint32_t ptrId =
              AbstractInterpretation::getValueIdStatic(call->getArgOperand(0));

          if (!isValidFree(as, ptrId)) {
            addEventToTrace(AEBugEventType::FREE, inst,
                            "Invalid free detected");
            AEException bug("Invalid free: freeing invalid memory");
            addBugToReporter(bug, inst);
          }
        }
      }
    }
  }
}

void InvalidFreeDetector::handleStubFunctions(const llvm::CallBase *call) {
  (void)call;
  // Track allocation/free events
}

void InvalidFreeDetector::reportBug() {
  if (!instToBugInfo.empty()) {
    llvm::errs() << "###################### Invalid Free ("
                 << instToBugInfo.size() << " found) ######################\n";
    for (const auto &it : instToBugInfo) {
      llvm::errs() << it.second << "\n";
    }
  }
}

void InvalidFreeDetector::reset() {
  clearEventTrace();
  bugLoc.clear();
  instToBugInfo.clear();
}

bool InvalidFreeDetector::isValidFree(AbstractState &as, uint32_t ptrId) {
  // If we don't track this pointer, assume valid to avoid false positives
  // (e.g. malloc-returned ptr not in var-to-addrs table from external/stub)
  if (!as.inVarToAddrsTable(ptrId))
    return true;

  const AbstractValue &absVal = as[ptrId];
  if (!absVal.isAddr())
    return false;

  for (const auto &addr : absVal.getAddrs()) {
    // free(NULL) is defined as a no-op in C/C++.
    if (AbstractState::isNullMem(addr))
      continue;
    if (AbstractState::isInvalidMem(addr))
      return false;
    const uint32_t objId = as.getIDFromAddr(addr);
    // Only heap allocation bases are legal free targets.
    if (!as.isHeapObject(objId))
      return false;
    // Check for double-free (already freed)
    if (as.isFreedMem(addr))
      return false;
  }

  return true;
}

void InvalidFreeDetector::addBugToReporter(const AEException &e,
                                           const llvm::Instruction *inst) {
  std::string loc;
  if (const llvm::DILocation *debugLoc = inst->getDebugLoc()) {
    loc = debugLoc->getFilename().str() + ":" +
          std::to_string(debugLoc->getLine());
  } else {
    std::string instStr;
    llvm::raw_string_ostream os(instStr);
    inst->print(os);
    os.flush();
    const llvm::Function *func = inst->getFunction();
    const llvm::BasicBlock *bb = inst->getParent();
    loc = (func ? func->getName().str() : "unknown_function") +
          "::" + (bb && bb->hasName() ? bb->getName().str() : "unknown_bb") +
          "::" + std::to_string(inst->getOpcode()) + "::" + instStr;
  }

  if (bugLoc.find(loc) != bugLoc.end())
    return;

  bugLoc.insert(loc);
  instToBugInfo[inst] = std::string(e.what()) + " @ " + loc;
  emitAEBugReport(kind, inst, std::string(e.what()));
}

//===----------------------------------------------------------------------===//
// MemLeakDetector Implementation
//===----------------------------------------------------------------------===//

void MemLeakDetector::detect(AbstractState &as, const llvm::Instruction *inst) {
  // Track allocations (malloc, calloc, new, etc.)
  if (const auto *call = llvm::dyn_cast<llvm::CallBase>(inst)) {
    AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
    std::vector<const llvm::Function *> callees = ae.getCallees(call);

    for (const llvm::Function *callee : callees) {
      if (!callee)
        continue;

      std::string funcName = callee->getName().str();

      // Check if this is an allocation function
      if (funcName == "malloc" || funcName == "calloc" ||
          funcName == "realloc" || funcName == "_Znwm" ||
          funcName == "_Znam" || // operator new, new[]
          funcName == "strdup" || funcName == "strndup") {

        uint32_t retId = AbstractInterpretation::getValueIdStatic(call);
        if (as.inVarToAddrsTable(retId)) {
          AddressValue addrs = as[retId].getAddrs();
          for (uint64_t addr : addrs.getVals()) {
            uint32_t objId = as.getIDFromAddr(addr);
            if (objId != AbstractState::NullPtr &&
                objId != AbstractState::BlkPtr) {
              trackAllocation(objId, call);
            }
          }
        }
      }

      // Check if object escapes via external function call
      for (unsigned i = 0; i < call->arg_size(); ++i) {
        const llvm::Value *arg = call->getArgOperand(i);
        if (arg->getType()->isPointerTy()) {
          uint32_t argId = AbstractInterpretation::getValueIdStatic(arg);
          if (as.inVarToAddrsTable(argId)) {
            AddressValue addrs = as[argId].getAddrs();
            for (uint64_t addr : addrs.getVals()) {
              uint32_t objId = as.getIDFromAddr(addr);
              // Mark as escaped if passed to external function
              if (!callee->isDeclaration() || funcName == "free" ||
                  funcName == "_ZdlPv" || funcName == "_ZdaPv") {
                // Don't mark as escaped for free/delete
                continue;
              }
              escapedObjects.insert(objId);
            }
          }
        }
      }
    }
  }

  // Check for leaks at function return points
  if (const auto *ret = llvm::dyn_cast<llvm::ReturnInst>(inst)) {
    auto escapesFunction = [&](uint32_t objId) -> bool {
      if (ret->getReturnValue() &&
          ret->getReturnValue()->getType()->isPointerTy()) {
        uint32_t retId =
            AbstractInterpretation::getValueIdStatic(ret->getReturnValue());
        if (as.inVarToAddrsTable(retId)) {
          for (uint64_t addr : as[retId].getAddrs().getVals()) {
            if (as.getIDFromAddr(addr) == objId) {
              return true;
            }
          }
        }
      }

      for (const auto &pair : as.getVarToVal()) {
        const llvm::Value *root =
            AbstractInterpretation::getAEInstance().getValueFromIdStatic(
                pair.first);
        if (!root || !llvm::isa<llvm::GlobalValue>(root) ||
            !pair.second.isAddr()) {
          continue;
        }
        for (uint64_t addr : pair.second.getAddrs().getVals()) {
          if (as.getIDFromAddr(addr) == objId) {
            return true;
          }
        }
      }

      for (const auto &pair : as.getLocToVal()) {
        const llvm::Value *memObj =
            AbstractInterpretation::getAEInstance().getValueFromIdStatic(
                pair.first);
        if (!memObj || !llvm::isa<llvm::GlobalValue>(memObj) ||
            !pair.second.isAddr()) {
          continue;
        }
        for (uint64_t addr : pair.second.getAddrs().getVals()) {
          if (as.getIDFromAddr(addr) == objId) {
            return true;
          }
        }
      }

      return false;
    };

    // Get all allocated objects
    std::set<uint32_t> allocatedObjs;
    for (const auto &pair : objToAllocSite) {
      allocatedObjs.insert(pair.first);
    }

    // Remove freed objects
    for (uint32_t freedObj : as._freedAddrs) {
      allocatedObjs.erase(freedObj);
    }

    // Remove escaped objects (may be freed elsewhere)
    for (uint32_t escapedObj : escapedObjects) {
      allocatedObjs.erase(escapedObj);
    }

    // Check if remaining objects are reachable from live pointers
    for (uint32_t objId : allocatedObjs) {
      if (!escapesFunction(objId)) {
        // Found a leak
        const llvm::Instruction *allocSite = objToAllocSite[objId];
        addEventToTrace(AEBugEventType::ALLOC, allocSite,
                        "Memory allocated here");
        addEventToTrace(AEBugEventType::RETURN, ret, "Memory leaked at return");

        AEException bug(
            "Memory leak: allocated memory not freed and not reachable");
        addBugToReporter(bug, allocSite);
      }
    }
  }

  // Also check for leaks when pointer goes out of scope (store overwrites
  // last reference)
  if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(inst)) {
    if (store->getValueOperand()->getType()->isPointerTy()) {
      const llvm::Value *root =
          stripPointerProjections(store->getPointerOperand());
      if (llvm::isa<llvm::Argument>(root) ||
          llvm::isa<llvm::GlobalValue>(root)) {
        uint32_t storedId =
            AbstractInterpretation::getValueIdStatic(store->getValueOperand());
        if (as.inVarToAddrsTable(storedId)) {
          for (uint64_t addr : as[storedId].getAddrs().getVals()) {
            uint32_t objId = as.getIDFromAddr(addr);
            if (objToAllocSite.count(objId)) {
              escapedObjects.insert(objId);
            }
          }
        }
      }
    }

    const llvm::Value *ptr = store->getPointerOperand();

    // Check if we're overwriting the last reference to an allocated object
    uint32_t ptrId = AbstractInterpretation::getValueIdStatic(ptr);
    if (!as.inVarToAddrsTable(ptrId))
      return;

    for (uint64_t slotAddr : as[ptrId].getAddrs().getVals()) {
      uint32_t slotObjId = as.getIDFromAddr(slotAddr);
      if (!as.inAddrToAddrsTable(slotObjId))
        continue;

      AddressValue oldAddrs = as.load(slotAddr).getAddrs();
      for (uint64_t addr : oldAddrs.getVals()) {
        uint32_t objId = as.getIDFromAddr(addr);

        // Check if this object was allocated and not freed
        if (objToAllocSite.count(objId) && !as._freedAddrs.count(objId) &&
            !escapedObjects.count(objId)) {

          // Check if this is the last reference
          if (!isReachableFromLivePointers(as, objId)) {
            const llvm::Instruction *allocSite = objToAllocSite[objId];
            addEventToTrace(AEBugEventType::ALLOC, allocSite,
                            "Memory allocated here");
            addEventToTrace(AEBugEventType::STORE, store,
                            "Last reference overwritten");

            AEException bug("Memory leak: last reference to allocated memory "
                            "overwritten");
            addBugToReporter(bug, allocSite);
          }
        }
      }
    }
  }
}

void MemLeakDetector::handleStubFunctions(const llvm::CallBase *call) {
  // Handle stub functions that may allocate or free memory
  const llvm::Function *callee = call->getCalledFunction();
  if (!callee)
    return;

  std::string funcName = callee->getName().str();

  // Mark objects as escaped if returned from function
  if (funcName.find("SAFE_ALLOC") != std::string::npos) {
    uint32_t retId = AbstractInterpretation::getValueIdStatic(call);
    // Track this as an allocation that escapes
    escapedObjects.insert(retId);
  }
}

void MemLeakDetector::reportBug() {
  if (instToBugInfo.empty())
    return;

  llvm::outs() << "\n=== Memory Leak Detection Results ===\n";
  llvm::outs() << "Total leaks found: " << instToBugInfo.size() << "\n\n";

  for (const auto &pair : instToBugInfo) {
    llvm::outs() << pair.second << "\n";
  }
}

void MemLeakDetector::reset() {
  AEDetector::reset();
  objToAllocSite.clear();
  escapedObjects.clear();
  bugLoc.clear();
  instToBugInfo.clear();
}

void MemLeakDetector::trackAllocation(uint32_t objId,
                                      const llvm::Instruction *allocSite) {
  objToAllocSite[objId] = allocSite;
}

bool MemLeakDetector::isReachableFromLivePointers(AbstractState &as,
                                                  uint32_t objId) {
  // Check if objId is reachable from any variable in the abstract state

  // Check all variables
  for (const auto &pair : as.getVarToVal()) {
    const AbstractValue &val = pair.second;
    if (val.isAddr()) {
      AddressValue addrs = val.getAddrs();
      for (uint64_t addr : addrs.getVals()) {
        if (as.getIDFromAddr(addr) == objId) {
          return true; // Found a reference
        }
      }
    }
  }

  // Check all memory locations
  for (const auto &pair : as.getLocToVal()) {
    const AbstractValue &val = pair.second;
    if (val.isAddr()) {
      AddressValue addrs = val.getAddrs();
      for (uint64_t addr : addrs.getVals()) {
        if (as.getIDFromAddr(addr) == objId) {
          return true; // Found a reference in memory
        }
      }
    }
  }

  return false; // No references found
}

bool MemLeakDetector::objectEscapes(uint32_t objId) {
  return escapedObjects.count(objId) > 0;
}

void MemLeakDetector::addBugToReporter(const AEException &e,
                                       const llvm::Instruction *inst) {
  std::string loc;
  if (const llvm::DILocation *debugLoc = inst->getDebugLoc()) {
    loc = debugLoc->getFilename().str() + ":" +
          std::to_string(debugLoc->getLine());
  } else {
    loc = "unknown location";
  }

  if (bugLoc.find(loc) != bugLoc.end())
    return;

  bugLoc.insert(loc);
  instToBugInfo[inst] = std::string(e.what()) + " @ " + loc;
  emitAEBugReport(kind, inst, std::string(e.what()));
}

} // namespace analysis
} // namespace lotus
