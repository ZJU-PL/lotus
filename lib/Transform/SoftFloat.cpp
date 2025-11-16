// SoftFloat pass replaces floating-point instructions with calls to
// the corresponding libgcc library functions. This is useful for targets
// that don't have native floating-point support.
#include "Transform/SoftFloat.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {

// Get the libgcc type suffix for a given LLVM type.
// Returns a string identifier used in libgcc function names.
const char* typeName(Type* T) {
  if (T->isFloatTy()) {
    return "sf";
  } else if (T->isDoubleTy()) {
    return "df";
  } else if (auto IntTy = dyn_cast<IntegerType>(T)) {
    switch (IntTy->getBitWidth()) {
      case 32: return "si";
      case 64: return "di";
      case 128: return "ti";
    }
  }
  llvm::errs() << "unsupported type: " << *T << "\n";
  abort();
}

// Get the declaration of a libgcc conversion function.
// The name will consist of `Prefix`, the `libgcc` identifiers of `ArgTy` and `RetTy`, and `Suffix`.
FunctionCallee getConvert(Module* M,
    StringRef Prefix, StringRef Suffix, Type* ArgTy, Type* RetTy) {
  std::string Name(Prefix);
  Name.append(typeName(ArgTy));
  Name.append(typeName(RetTy));
  Name.append(Suffix.str());

  FunctionType *FT = FunctionType::get(RetTy, {ArgTy}, false);
  return M->getOrInsertFunction(Name, FT);
}

void replaceConvert(Module* M, Instruction& I, StringRef Prefix, StringRef Suffix, bool Signed) {
  Value* Arg = I.getOperand(0);

  Type* ArgTy = Arg->getType();
  Type* RetTy = I.getType();
  Type* OrigRetTy = RetTy;

  if (auto ArgIntTy = dyn_cast<IntegerType>(ArgTy)) {
    if (ArgIntTy->getBitWidth() < 32) {
      // Extend argument before conversion.
      ArgTy = IntegerType::get(M->getContext(), 32);
      if (Signed) {
        Arg = new SExtInst(Arg, ArgTy, I.getName(), &I);
      } else {
        Arg = new ZExtInst(Arg, ArgTy, I.getName(), &I);
      }
    }
  }

  if (auto RetIntTy = dyn_cast<IntegerType>(RetTy)) {
    if (RetIntTy->getBitWidth() < 32) {
      RetTy = IntegerType::get(M->getContext(), 32);
    }
  }

  FunctionCallee Func = getConvert(M, Prefix, Suffix, ArgTy, RetTy);
  Value* Args[1] = { Arg };
  CallInst* Call = CallInst::Create(Func, Args, I.getName(), &I);

  Value* Result = Call;
  if (RetTy != OrigRetTy) {
    Result = new TruncInst(Result, OrigRetTy, I.getName(), &I);
  }

  I.replaceAllUsesWith(Result);
}

// Get the declaration of a function of type `T -> T`.  The name will be
// `BaseName` plus an appropriate suffix based on `T`.
FunctionCallee getUnary(Module* M, StringRef BaseName, Type* T) {
  std::string Name(BaseName);
  Name.append(typeName(T));
  Name.append("2");

  FunctionType *FT = FunctionType::get(T, {T}, false);
  return M->getOrInsertFunction(Name, FT);
}

void replaceUnary(Module* M, Instruction& I, StringRef BaseName) {
  Value* Arg = I.getOperand(0);
  FunctionCallee Func = getUnary(M, BaseName, Arg->getType());
  Value* Args[1] = { Arg };
  CallInst* Call = CallInst::Create(Func, Args, I.getName(), &I);
  I.replaceAllUsesWith(Call);
}

// Get the declaration of a function of type `T -> T -> T`.  The name will be
// `BaseName` plus an appropriate suffix based on `T`.
FunctionCallee getBinary(Module* M, StringRef BaseName, Type* T) {
  std::string Name(BaseName);
  Name.append(typeName(T));
  Name.append("3");

  FunctionType *FT = FunctionType::get(T, {T, T}, false);
  return M->getOrInsertFunction(Name, FT);
}

void replaceBinary(Module* M, Instruction& I, StringRef BaseName) {
  Value* Arg1 = I.getOperand(0);
  Value* Arg2 = I.getOperand(1);
  FunctionCallee Func = getBinary(M, BaseName, Arg1->getType());
  Value* Args[2] = { Arg1, Arg2 };
  CallInst* Call = CallInst::Create(Func, Args, I.getName(), &I);
  I.replaceAllUsesWith(Call);
}

// Get the declaration of a function of type `T -> T -> i32`.  The name will be
// `BaseName` plus an appropriate suffix based on `T`.
FunctionCallee getCompare(Module* M, StringRef BaseName, Type* T) {
  std::string Name(BaseName);
  Name.append(typeName(T));
  Name.append("2");

  Type* I32Type = IntegerType::get(M->getContext(), 32);
  FunctionType *FT = FunctionType::get(I32Type, {T, T}, false);
  return M->getOrInsertFunction(Name, FT);
}

Value* getFCmpReplacement(Module* M, FCmpInst& I) {
  // Integer comparison to perform on the result of `__cmpXf2`.
  CmpInst::Predicate ICmp;
  // How to handle unordered values.  `true` means `unord || cmp <=> 0`;
  // `false` means `!unord && cmp <=> 0`.
  bool AllowUnord;

  switch (I.getPredicate()) {
    case CmpInst::FCMP_FALSE:
      return ConstantInt::getFalse(M->getContext());

    case CmpInst::FCMP_OEQ:
      AllowUnord = false;
      ICmp = CmpInst::ICMP_EQ;
      break;
    case CmpInst::FCMP_OGT:
      AllowUnord = false;
      ICmp = CmpInst::ICMP_SGT;
      break;
    case CmpInst::FCMP_OGE:
      AllowUnord = false;
      ICmp = CmpInst::ICMP_SGE;
      break;
    case CmpInst::FCMP_OLT:
      AllowUnord = false;
      ICmp = CmpInst::ICMP_SLT;
      break;
    case CmpInst::FCMP_OLE:
      AllowUnord = false;
      ICmp = CmpInst::ICMP_SLE;
      break;
    case CmpInst::FCMP_ONE:
      AllowUnord = false;
      ICmp = CmpInst::ICMP_NE;
      break;
    case CmpInst::FCMP_ORD:
      AllowUnord = false;
      ICmp = CmpInst::BAD_ICMP_PREDICATE;
      break;

    case CmpInst::FCMP_UNO:
      AllowUnord = true;
      ICmp = CmpInst::BAD_ICMP_PREDICATE;
      break;
    case CmpInst::FCMP_UEQ:
      AllowUnord = true;
      ICmp = CmpInst::ICMP_EQ;
      break;
    case CmpInst::FCMP_UGT:
      AllowUnord = true;
      ICmp = CmpInst::ICMP_SGT;
      break;
    case CmpInst::FCMP_UGE:
      AllowUnord = true;
      ICmp = CmpInst::ICMP_SGE;
      break;
    case CmpInst::FCMP_ULT:
      AllowUnord = true;
      ICmp = CmpInst::ICMP_SLT;
      break;
    case CmpInst::FCMP_ULE:
      AllowUnord = true;
      ICmp = CmpInst::ICMP_SLE;
      break;
    case CmpInst::FCMP_UNE:
      AllowUnord = true;
      ICmp = CmpInst::ICMP_NE;
      break;

    case CmpInst::FCMP_TRUE:
      return ConstantInt::getTrue(M->getContext());

    default:
      llvm::errs() << "bad fcmp predicate: " << I.getPredicate() << "\n";
      abort();
  }

  Value* Arg1 = I.getOperand(0);
  Value* Arg2 = I.getOperand(1);
  Value* Args[2] = { Arg1, Arg2 };

  Type* I32Type = IntegerType::get(M->getContext(), 32);
  Value* Zero = ConstantInt::get(I32Type, 0);

  FunctionCallee UnordFunc = getCompare(M, "__unord", Arg1->getType());
  Value* UnordValue = CallInst::Create(UnordFunc, Args, I.getName(), &I);
  Value* UnordFlag = new ICmpInst(&I, CmpInst::ICMP_NE, UnordValue, Zero);

  if (ICmp == CmpInst::BAD_ICMP_PREDICATE) {
    // This sentinel value indicates only `UnordFlag` should be considered.
    if (AllowUnord) {
      return UnordFlag;
    } else {
      return BinaryOperator::CreateNot(UnordFlag, I.getName(), &I);
    }
  }

  FunctionCallee CmpFunc = getCompare(M, "__cmp", Arg1->getType());
  Value* CmpValue = CallInst::Create(CmpFunc, Args, I.getName(), &I);
  Value* CmpFlag = new ICmpInst(&I, ICmp, CmpValue, Zero);

  if (AllowUnord) {
    return BinaryOperator::Create(Instruction::Or, UnordFlag, CmpFlag, I.getName(), &I);
  } else {
    Value* OrdFlag = BinaryOperator::CreateNot(UnordFlag, I.getName(), &I);
    return BinaryOperator::Create(Instruction::And, OrdFlag, CmpFlag, I.getName(), &I);
  }
}

void replaceFCmp(Module* M, FCmpInst& I) {
  Value* V = getFCmpReplacement(M, I);
  I.replaceAllUsesWith(V);
}

// Helper function for soft float transformation
static bool softFloatInFunction(Function &F) {
    Module* M = F.getParent();
    std::vector<Instruction*> ToErase;
    for (BasicBlock& BB : F) {
      for (Instruction& I : BB) {
        if (auto Cast = dyn_cast<CastInst>(&I)) {
          switch (Cast->getOpcode()) {
            case Instruction::FPToUI:
              replaceConvert(M, I, "__fixuns", "", false);
              ToErase.push_back(&I);
              break;
            case Instruction::FPToSI:
              replaceConvert(M, I, "__fix", "", true);
              ToErase.push_back(&I);
              break;
            case Instruction::UIToFP:
              replaceConvert(M, I, "__floatun", "", false);
              ToErase.push_back(&I);
              break;
            case Instruction::SIToFP:
              replaceConvert(M, I, "__float", "", true);
              ToErase.push_back(&I);
              break;
            case Instruction::FPExt:
              replaceConvert(M, I, "__extend", "2", false);
              ToErase.push_back(&I);
              break;
            case Instruction::FPTrunc:
              replaceConvert(M, I, "__trunc", "2", false);
              ToErase.push_back(&I);
              break;
          }
        } else if (auto UnOp = dyn_cast<UnaryOperator>(&I)) {
          switch (UnOp->getOpcode()) {
            case Instruction::FNeg:
              replaceUnary(M, I, "__neg");
              ToErase.push_back(&I);
              break;
          }
        } else if (auto BinOp = dyn_cast<BinaryOperator>(&I)) {
          switch (BinOp->getOpcode()) {
            case Instruction::FAdd:
              replaceBinary(M, I, "__add");
              ToErase.push_back(&I);
              break;
            case Instruction::FSub:
              replaceBinary(M, I, "__sub");
              ToErase.push_back(&I);
              break;
            case Instruction::FMul:
              replaceBinary(M, I, "__mul");
              ToErase.push_back(&I);
              break;
            case Instruction::FDiv:
              replaceBinary(M, I, "__div");
              ToErase.push_back(&I);
              break;
          }
        } else if (auto FCmp = dyn_cast<FCmpInst>(&I)) {
          replaceFCmp(M, *FCmp);
        }
      }
    }

    for (Instruction* I : ToErase) {
      I->eraseFromParent();
    }

    return true;
}

}  // end of anonymous namespace

// New Pass Manager implementation
PreservedAnalyses SoftFloatPass::run(Function &F, FunctionAnalysisManager &) {
    bool Changed = softFloatInFunction(F);
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}