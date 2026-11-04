/**
 * @file TaintSignature.h
 * @brief Taint signature management for TypeQualifier analysis
 *
 * This file provides the TaintSignature class for managing taint
 * signatures used to identify sensitive data flow in the TypeQualifier
 * analysis framework. It uses LLVM's SpecialCaseList for configuration.
 *
 * @ingroup TypeQualifier
 */

#pragma once
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SpecialCaseList.h"

using namespace llvm;

class TaintSignature {
  std::unique_ptr<llvm::SpecialCaseList> SCL;

public:
  TaintSignature(StringRef SignaturePath);

  bool IsSensitiveFunction(StringRef FuncName) const;
  bool IsSensitiveFunctionArg(StringRef FuncName, unsigned ArgIndex) const;

  bool isFunctionInSig(bool IsSink, StringRef FuncName) const;
  bool isFunctionArgInSig(bool IsSink, StringRef FuncName,
                          unsigned ArgIndex) const;
  bool isFuctionRetInSig(bool IsSink, StringRef FuncName) const;
  bool isSensitiveStruct(StringRef StructId) const;
};

