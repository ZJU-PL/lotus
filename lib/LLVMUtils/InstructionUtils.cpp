#include "LLVMUtils/InstructionUtils.h"
#include "LLVMUtils/Demangle.h"
#include <llvm/IR/CFG.h>
#include <set>

using namespace llvm;

// Returns true if the instruction operates on pointers.
bool InstructionUtils::isPointerInstruction(Instruction *I) {
  bool retVal = false;
  LoadInst *LI = dyn_cast<LoadInst>(I);
  if (LI) {
    retVal = true;
  }
  StoreInst *SI = dyn_cast<StoreInst>(I);
  if (SI) {
    retVal = true;
  }
  VAArgInst *VAAI = dyn_cast<VAArgInst>(I);
  if (VAAI) {
    retVal = true;
  }
  return retVal;
}

// Returns the line number of the instruction from debug info.
unsigned InstructionUtils::getLineNumber(Instruction &I) {

  const DebugLoc &currDC = I.getDebugLoc();
  if (currDC) {
    return currDC.getLine();
  }
  return -1;
}

// Returns the name of the instruction or "No Name" if unnamed.
std::string InstructionUtils::getInstructionName(Instruction *I) {
  if (I->hasName()) {
    std::string name = I->getName().str();
    // Demangle if the name is mangled
    return DemangleUtils::demangleWithCleanup(name);
  } else {
    return "No Name";
  }
}

// Returns the name of the value or "No Name" if unnamed.
std::string InstructionUtils::getValueName(Value *v) {
  if (v->hasName()) {
    std::string name = v->getName().str();
    // Demangle if the name is mangled
    return DemangleUtils::demangleWithCleanup(name);
  } else {
    return "No Name";
  }
}

// Escapes special characters in a string for JSON output.
std::string InstructionUtils::escapeJsonString(const std::string &input) {
  std::ostringstream ss;
  for (std::string::const_iterator iter = input.begin(); iter != input.end(); iter++) {
    switch (*iter) {
    case '\\':
      ss << "\\\\";
      break;
    case '"':
      ss << "\\\"";
      break;
    case '/':
      ss << "\\/";
      break;
    case '\b':
      ss << "\\b";
      break;
    case '\f':
      ss << "\\f";
      break;
    case '\n':
      ss << "\\n";
      break;
    case '\r':
      ss << "\\r";
      break;
    case '\t':
      ss << "\\t";
      break;
    default:
      ss << *iter;
      break;
    }
  }
  return ss.str();
}

// Escapes a Value as a string for JSON output.
std::string InstructionUtils::escapeValueString(Value *currInstr) {
  std::string str;
  llvm::raw_string_ostream rso(str);
  currInstr->print(rso);
  return escapeJsonString(rso.str());
}

// Recursively searches for debug location in predecessor blocks.
DILocation *getRecursiveDILoc(Instruction *currInst, std::string &funcFileName,
                              std::set<BasicBlock *> &visitedBBs) {
  DILocation *currIL = currInst->getDebugLoc().get();
  if (funcFileName.length() == 0) {
    return currIL;
  }
  if (currIL != nullptr && currIL->getFilename().equals(funcFileName)) {
    return currIL;
  }

  BasicBlock *currBB = currInst->getParent();
  if (visitedBBs.find(currBB) != visitedBBs.end()) {
    return nullptr;
  }
  for (auto &iu : currBB->getInstList()) {
    Instruction *currIterI = &iu;
    DILocation *currIteDL = currIterI->getDebugLoc();
    if (currIteDL != nullptr && currIteDL->getFilename().equals(funcFileName)) {
      return currIteDL;
    }
    if (currIterI == currInst) {
      break;
    }
  }

  visitedBBs.insert(currBB);

  for (auto it = pred_begin(currBB), et = pred_end(currBB); it != et; ++it) {
    BasicBlock *predecessor = *it;
    DILocation *currBBLoc = getRecursiveDILoc(predecessor->getTerminator(),
                                              funcFileName, visitedBBs);
    if (currBBLoc != nullptr) {
      return currBBLoc;
    }
  }
  return nullptr;
}

// Returns the filename associated with the function from debug info.
std::string getFunctionFileName(Function *F) {
  SmallVector<std::pair<unsigned, MDNode *>, 4> MDs;
  F->getAllMetadata(MDs);
  for (auto &MD : MDs) {
    if (MDNode *N = MD.second) {
      if (auto *subProgram = dyn_cast<DISubprogram>(N)) {
        return subProgram->getFilename().str();
      }
    }
  }
  return "";
}

// Returns the correct debug location for an instruction.
DILocation *InstructionUtils::getCorrectInstrLocation(Instruction *I) {
  DILocation *instrLoc = I->getDebugLoc().get();
  // BasicBlock *firstBB = &(I->getFunction()->getEntryBlock());
  // Instruction *firstInstr = firstBB->getFirstNonPHIOrDbg();

  // DILocation *firstIL = firstInstr->getDebugLoc().get();
  std::set<BasicBlock *> visitedBBs;
  std::string funcFileName = getFunctionFileName(I->getFunction());

  if (instrLoc != nullptr && instrLoc->getFilename().endswith(".c")) {
    return instrLoc;
  }

  if (instrLoc == nullptr || (funcFileName.length() > 0 &&
                              !instrLoc->getFilename().equals(funcFileName))) {
    // OK, the instruction is from the inlined function.
    visitedBBs.clear();
    DILocation *actualLoc = getRecursiveDILoc(I, funcFileName, visitedBBs);
    if (actualLoc != nullptr) {

      return actualLoc;
    }
  }

  return instrLoc;
}

// Returns the line number for an instruction using correct location.
int InstructionUtils::getInstrLineNumber(Instruction *I) {
  DILocation *targetLoc = InstructionUtils::getCorrectInstrLocation(I);
  if (targetLoc != nullptr) {
    return targetLoc->getLine();
  }
  return -1;
}
