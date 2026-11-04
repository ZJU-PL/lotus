/**
 * @file Helper.h
 * @brief Helper functions for TypeQualifier analysis
 *
 * This file provides utility helper functions used by the TypeQualifier
 * analysis framework. It includes functions for GEP offset calculation, field
 * number determination, type compatibility checking, and string utilities.
 *
 * @ingroup TypeQualifier
 */

#pragma once

#include "Checker/TypeQualifier/StructAnalyzer.h"

#include <unordered_map>

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
// enum WarnType {FUNCTION_PTR, NORMAL_PTR, DATA, OTHER};
// std::unordered_map<int ,std::string> eToS = {{0, "FUNCTION_PTR"}, {1,
// "NORMAL_PTR"}, {2, "DATA"}, {3, "OTHER"}};
int64_t getGEPOffset(const llvm::Value *value,
                     const llvm::DataLayout *dataLayout);
unsigned offsetToFieldNum(const llvm::Value *ptr, int64_t offset,
                          const llvm::DataLayout *dataLayout,
                          const StructAnalyzer *structAnalyzer,
                          llvm::Module *module);
bool isCompatibleType(llvm::Type *T1, llvm::Type *T2,
                      llvm::Function *func = NULL, int argNo = -1);
std::vector<std::string> strSplit(std::string &str, const std::string pattern);
std::string getCurrentWorkingDir(void);

