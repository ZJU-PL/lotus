#ifndef LOTUS_TESTS_UTILS_SOURCELOCATIONENTRY_H
#define LOTUS_TESTS_UTILS_SOURCELOCATIONENTRY_H

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/StringRef.h>

#include <cstdint>
#include <functional>
#include <iterator>
#include <ostream>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <variant>

namespace lotus {
namespace testing {

// ============================================================================
// Source Location Types for Testing
// ============================================================================

struct GlobalVar {
  llvm::StringRef Name;

  friend bool operator<(GlobalVar G1, GlobalVar G2) noexcept {
    return G1.Name < G2.Name;
  }
  friend bool operator==(GlobalVar G1, GlobalVar G2) noexcept {
    return G1.Name == G2.Name;
  }

  [[nodiscard]] std::string str() const {
    return std::string("GlobalVar { Name: ") + Name.str() + " }";
  }
};

struct LineCol {
  uint32_t Line{};
  uint32_t Col{};

  friend bool operator<(LineCol LC1, LineCol LC2) noexcept {
    return std::tie(LC1.Line, LC1.Col) < std::tie(LC2.Line, LC2.Col);
  }
  friend bool operator==(LineCol LC1, LineCol LC2) noexcept {
    return std::tie(LC1.Line, LC1.Col) == std::tie(LC2.Line, LC2.Col);
  }
  [[nodiscard]] std::string str() const {
    return std::string("LineCol { Line: ") + std::to_string(Line) +
           "; Col: " + std::to_string(Col) + " }";
  }
};

struct LineColFunOp {
  uint32_t Line{};
  uint32_t Col{};
  llvm::StringRef InFunction{};
  uint32_t OpCode{};

  friend bool operator<(LineColFunOp LC1, LineColFunOp LC2) noexcept {
    return std::tie(LC1.InFunction, LC1.Line, LC1.Col, LC1.OpCode) <
           std::tie(LC2.InFunction, LC2.Line, LC2.Col, LC2.OpCode);
  }
  friend bool operator==(LineColFunOp LC1, LineColFunOp LC2) noexcept {
    return std::tie(LC1.Line, LC1.Col, LC1.InFunction, LC1.OpCode) ==
           std::tie(LC2.Line, LC2.Col, LC2.InFunction, LC2.OpCode);
  }
  [[nodiscard]] std::string str() const {
    return std::string("LineColFunOp { Line: ") + std::to_string(Line) +
           "; Col: " + std::to_string(Col) +
           "; InFunction: " + InFunction.str() +
           "; OpCode: " + llvm::Instruction::getOpcodeName(OpCode) + " }";
  }
};

struct LineColFun {
  uint32_t Line{};
  uint32_t Col{};
  llvm::StringRef InFunction{};

  friend bool operator<(LineColFun LC1, LineColFun LC2) noexcept {
    return std::tie(LC1.InFunction, LC1.Line, LC1.Col) <
           std::tie(LC2.InFunction, LC2.Line, LC2.Col);
  }
  friend bool operator==(LineColFun LC1, LineColFun LC2) noexcept {
    return std::tie(LC1.Line, LC1.Col, LC1.InFunction) ==
           std::tie(LC2.Line, LC2.Col, LC2.InFunction);
  }
  [[nodiscard]] std::string str() const {
    return std::string("LineColFun { Line: ") + std::to_string(Line) +
           "; Col: " + std::to_string(Col) +
           "; InFunction: " + InFunction.str() + " }";
  }

  constexpr operator LineColFunOp() const noexcept {
    // 0 is the wildcard opcode
    return {Line, Col, InFunction, 0};
  }
};

struct ArgNo {
  uint32_t Idx{};

  friend bool operator<(ArgNo A1, ArgNo A2) noexcept { return A1.Idx < A2.Idx; }
  friend bool operator==(ArgNo A1, ArgNo A2) noexcept {
    return A1.Idx == A2.Idx;
  }
  [[nodiscard]] std::string str() const {
    return std::string("ArgNo { Idx: ") + std::to_string(Idx) + " }";
  }
};

struct ArgInFun {
  uint32_t Idx{};
  llvm::StringRef InFunction{};

  friend bool operator<(ArgInFun A1, ArgInFun A2) noexcept {
    return std::tie(A1.InFunction, A1.Idx) < std::tie(A2.InFunction, A2.Idx);
  }
  friend bool operator==(ArgInFun A1, ArgInFun A2) noexcept {
    return std::tie(A1.Idx, A1.InFunction) == std::tie(A2.Idx, A2.InFunction);
  }
  [[nodiscard]] std::string str() const {
    return std::string("ArgInFun { Idx: ") + std::to_string(Idx) +
           "; InFunction: " + InFunction.str() + " }";
  }
};

struct RetVal {
  llvm::StringRef InFunction;

  friend bool operator<(RetVal R1, RetVal R2) noexcept {
    return R1.InFunction < R2.InFunction;
  }
  friend bool operator==(RetVal R1, RetVal R2) noexcept {
    return R1.InFunction == R2.InFunction;
  }
  [[nodiscard]] std::string str() const {
    return std::string("RetVal { InFunction: ") + InFunction.str() + " }";
  }
};

struct RetStmt {
  llvm::StringRef InFunction;

  friend bool operator<(RetStmt R1, RetStmt R2) noexcept {
    return R1.InFunction < R2.InFunction;
  }
  friend bool operator==(RetStmt R1, RetStmt R2) noexcept {
    return R1.InFunction == R2.InFunction;
  }
  [[nodiscard]] std::string str() const {
    return std::string("RetStmt { InFunction: ") + InFunction.str() + " }";
  }
};

struct OperandOf {
  uint32_t OperandIndex{};
  LineColFunOp Inst{};

  friend bool operator<(OperandOf R1, OperandOf R2) noexcept {
    return std::tie(R1.OperandIndex, R2.Inst) <
           std::tie(R2.OperandIndex, R2.Inst);
  }
  friend bool operator==(OperandOf R1, OperandOf R2) noexcept {
    return R1.OperandIndex == R2.OperandIndex && R1.Inst == R2.Inst;
  }
  [[nodiscard]] std::string str() const {
    return std::string("OperandOf { OperandIndex: ") +
           std::to_string(OperandIndex) + "; Inst: " + Inst.str() + " }";
  }
};

struct TestingSrcLocation
    : public std::variant<LineCol, LineColFun, LineColFunOp, GlobalVar, ArgNo,
                          ArgInFun, RetVal, RetStmt, OperandOf> {
  using VarT = std::variant<LineCol, LineColFun, LineColFunOp, GlobalVar, ArgNo,
                            ArgInFun, RetVal, RetStmt, OperandOf>;
  using VarT::variant;

  template <typename T> [[nodiscard]] constexpr bool isa() const noexcept {
    return std::holds_alternative<T>(*this);
  }
  template <typename T>
  [[nodiscard]] constexpr const T *dyn_cast() const noexcept {
    return std::get_if<T>(this);
  }
  template <typename T> [[nodiscard]] constexpr T *dyn_cast() noexcept {
    return std::get_if<T>(this);
  }
  [[nodiscard]] std::string str() const {
    return std::visit([](const auto &Val) { return Val.str(); }, *this);
  }

  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                                       const TestingSrcLocation &Loc) {
    return OS << Loc.str();
  }
  friend std::ostream &operator<<(std::ostream &OS,
                                  const TestingSrcLocation &Loc) {
    return OS << Loc.str();
  }
};

} // namespace testing
} // namespace lotus

// Hash functions for std::unordered_map/set
namespace std {
template <> struct hash<lotus::testing::LineCol> {
  size_t operator()(lotus::testing::LineCol LC) const noexcept {
    return llvm::hash_value(std::make_pair(LC.Line, LC.Col));
  }
};
template <> struct hash<lotus::testing::LineColFun> {
  size_t operator()(lotus::testing::LineColFun LCF) const noexcept {
    return llvm::hash_combine(
        llvm::hash_value(std::make_pair(LCF.Line, LCF.Col)), LCF.InFunction);
  }
};

template <> struct hash<lotus::testing::LineColFunOp> {
  size_t operator()(lotus::testing::LineColFunOp LCF) const noexcept {
    return llvm::hash_combine(
        llvm::hash_value(std::make_pair(LCF.Line, LCF.Col)), LCF.InFunction,
        LCF.OpCode);
  }
};
template <> struct hash<lotus::testing::GlobalVar> {
  size_t operator()(lotus::testing::GlobalVar GV) const noexcept {
    return llvm::hash_value(GV.Name);
  }
};
template <> struct hash<lotus::testing::ArgNo> {
  size_t operator()(lotus::testing::ArgNo Arg) const noexcept {
    return llvm::hash_value(Arg.Idx);
  }
};
template <> struct hash<lotus::testing::ArgInFun> {
  size_t operator()(lotus::testing::ArgInFun Arg) const noexcept {
    return llvm::hash_combine(Arg.Idx, Arg.InFunction);
  }
};

template <> struct hash<lotus::testing::RetVal> {
  size_t operator()(lotus::testing::RetVal Ret) const noexcept {
    return llvm::hash_value(Ret.InFunction);
  }
};

template <> struct hash<lotus::testing::RetStmt> {
  size_t operator()(lotus::testing::RetStmt Ret) const noexcept {
    return llvm::hash_value(Ret.InFunction);
  }
};

template <> struct hash<lotus::testing::OperandOf> {
  size_t operator()(lotus::testing::OperandOf Op) const noexcept {
    return llvm::hash_combine(Op.OperandIndex,
                              hash<lotus::testing::LineColFunOp>{}(Op.Inst));
  }
};

template <> struct hash<lotus::testing::TestingSrcLocation> {
  size_t
  operator()(const lotus::testing::TestingSrcLocation &Loc) const noexcept {
    return std::hash<lotus::testing::TestingSrcLocation::VarT>{}(Loc);
  }
};
} // namespace std

namespace lotus {
namespace testing {

// ============================================================================
// Helper Functions for Source Location Resolution
// ============================================================================

template <typename PredFn = std::function<bool(const llvm::Instruction*)>>
[[nodiscard]] inline const llvm::Instruction *
getInstAtOrNull(const llvm::Function *F, uint32_t ReqLine,
                uint32_t ReqColumn = 0, PredFn Pred = {}) {
  assert(F != nullptr);
  for (const auto &I : llvm::instructions(F)) {
    if (I.isDebugOrPseudoInst()) {
      continue;
    }

    // For now, we'll use a simple line-based approach
    // In a real implementation, you'd use debug info to get actual line numbers
    auto Line = 1; // Placeholder - would extract from debug info
    auto Column = 1; // Placeholder - would extract from debug info
    
    if (Line == ReqLine && (ReqColumn == 0 || ReqColumn == Column) &&
        (!Pred || Pred(&I))) {
      return &I;
    }
  }
  return nullptr;
}

[[nodiscard]] inline const llvm::Value *
testingLocInIR(TestingSrcLocation Loc,
               const llvm::Module *Module,
               const llvm::Function *InterestingFunction = nullptr) {
  const auto GetFunction = [&Module](llvm::StringRef Name) {
    const auto *InFun = Module->getFunction(Name);
    if (!InFun) {
      llvm::report_fatal_error("Required function '" + Name +
                               "' does not exist in the IR!");
    }
    return InFun;
  };
  
  const auto *Ret = std::visit(
      lotus::Overloaded{
          [=](LineCol LC) -> llvm::Value const * {
            if (!InterestingFunction) {
              llvm::report_fatal_error(
                  "You must provide an InterestingFunction as last parameter "
                  "to testingLocInIR(), if trying to resolve a LineCol; "
                  "alternatively use LineColFun instead.");
            }

            return getInstAtOrNull(InterestingFunction, LC.Line, LC.Col);
          },
          [&](LineColFun LC) -> llvm::Value const * {
            const auto *InFun = GetFunction(LC.InFunction);
            return getInstAtOrNull(InFun, LC.Line, LC.Col);
          },
          [&](LineColFunOp LC) -> llvm::Value const * {
            const auto *InFun = GetFunction(LC.InFunction);
            return getInstAtOrNull(
                InFun, LC.Line, LC.Col,
                [Op = LC.OpCode](const llvm::Instruction *Inst) {
                  // According to LLVM's doc on llvm::Value::getValueID(), there
                  // cannot be any opcode==0, so we use it as wildcard here
                  return Op == 0 || Inst->getOpcode() == Op;
                });
          },
          [&Module](GlobalVar GV) -> llvm::Value const * {
            return Module->getGlobalVariable(GV.Name, true);
          },
          [=](ArgNo A) -> llvm::Value const * {
            if (!InterestingFunction) {
              llvm::report_fatal_error(
                  "You must provide an InterestingFunction as last parameter "
                  "to testingLocInIR(), if trying to resolve an ArgNo; "
                  "alternatively use ArgInFun instead.");
            }
            if (InterestingFunction->arg_size() <= A.Idx) {
              llvm::report_fatal_error(
                  "Argument index " + llvm::Twine(A.Idx) +
                  " is out of range (" +
                  llvm::Twine(InterestingFunction->arg_size()) + ")!");
            }
            return InterestingFunction->getArg(A.Idx);
          },
          [&](ArgInFun A) -> llvm::Value const * {
            const auto *InFun = GetFunction(A.InFunction);
            if (InFun->arg_size() <= A.Idx) {
              llvm::report_fatal_error("Argument index " + llvm::Twine(A.Idx) +
                                       " is out of range (" +
                                       llvm::Twine(InFun->arg_size()) + ")!");
            }
            return InFun->getArg(A.Idx);
          },
          [&](RetVal R) -> llvm::Value const * {
            const auto *InFun = GetFunction(R.InFunction);
            for (const auto &BB : llvm::reverse(InFun->getBasicBlockList())) {
              if (const auto *Ret =
                      llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator())) {
                return Ret->getReturnValue();
              }
            }
            llvm::report_fatal_error("No return stmt in function " +
                                     R.InFunction);
          },
          [&](RetStmt R) -> llvm::Value const * {
            const auto *InFun = GetFunction(R.InFunction);
            for (const auto &BB : llvm::reverse(InFun->getBasicBlockList())) {
              if (const auto *Ret =
                      llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator())) {
                return Ret;
              }
            }
            llvm::report_fatal_error("No return stmt in function " +
                                     R.InFunction);
          },
          [&](OperandOf Op) -> llvm::Value const * {
            const auto *Inst = llvm::dyn_cast_if_present<llvm::User>(
                testingLocInIR(Op.Inst, Module));
            if (!Inst) {
              return nullptr;
            }

            if (Inst->getNumOperands() <= Op.OperandIndex) {
              llvm::report_fatal_error("Requested operand index " +
                                       llvm::Twine(Op.OperandIndex) +
                                       " is out of bounds for instruction " +
                                       llvm::Twine(Inst->getName()));
            }

            return Inst->getOperand(Op.OperandIndex);
          },
      },
      Loc);
  if (!Ret) {
    llvm::report_fatal_error("Cannot convert " + llvm::Twine(Loc.str()) +
                             " to LLVM");
  }
  return Ret;
}

template <typename SetTy>
[[nodiscard]] inline std::set<const llvm::Value *>
convertTestingLocationSetInIR(
    const SetTy &Locs, const llvm::Module *Module,
    const llvm::Function *InterestingFunction = nullptr) {
  std::set<const llvm::Value *> Ret;
  llvm::transform(Locs, std::inserter(Ret, Ret.end()),
                  [&](TestingSrcLocation Loc) {
                    return testingLocInIR(Loc, Module, InterestingFunction);
                  });
  return Ret;
}

template <typename MapTy>
[[nodiscard]] inline auto convertTestingLocationSetMapInIR(
    const MapTy &Locs, const llvm::Module *Module,
    const llvm::Function *InterestingFunction = nullptr) {
  std::map<const llvm::Instruction *, std::set<const llvm::Value *>> Ret;
  llvm::transform(
      Locs, std::inserter(Ret, Ret.end()), [&](const auto &LocAndSet) {
        const auto &[InstLoc, Set] = LocAndSet;
        const auto *LocVal = testingLocInIR(InstLoc, Module, InterestingFunction);
        const auto *LocInst =
            llvm::dyn_cast_if_present<llvm::Instruction>(LocVal);
        if (!LocInst) {
          llvm::report_fatal_error(
              "Cannot convert " + llvm::Twine(InstLoc.str()) +
              (LocVal ? " aka. " + LocVal->getName().str() : "") +
              " to an LLVM instruction");
        }
        auto ConvSet =
            convertTestingLocationSetInIR(Set, Module, InterestingFunction);
        return std::make_pair(LocInst, std::move(ConvSet));
      });
  return Ret;
}

} // namespace testing
} // namespace lotus

#endif // LOTUS_TESTS_UTILS_SOURCELOCATIONENTRY_H
