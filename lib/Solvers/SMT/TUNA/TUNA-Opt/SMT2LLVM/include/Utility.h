#pragma once

#include "llvm/IR/PassManager.h"
#include <llvm/IR/Function.h>
#include "llvm/Passes/PassBuilder.h"
#include <iostream>
#include <string>

// 命令行解析相关的函数声明
void Help();
char *GetFlag(int argc, char *argv[], const std::string &flag);
bool HasFlag(int argc, char *argv[], const std::string &flag);

// 优化 Pass 相关的函数声明
unsigned short ParsePassesFromFile(const std::string &filename, llvm::FunctionPassManager &FPM, llvm::PassBuilder &PB);
unsigned short RunPasses(int *count, llvm::Function &function, llvm::FunctionPassManager &FPM);
