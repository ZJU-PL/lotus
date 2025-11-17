#pragma once

#include <iostream>
#include <memory>

#ifndef NDEBUG
#include <csignal>
#include "Analysis/Sprattus/repr.h"
#endif

#define DEBUG_TYPE "sprattus"

#include <llvm/Config/llvm-config.h>
#include <llvm/Support/Compiler.h>
#include <llvm/ADT/StringRef.h>

// LLVM version compatibility - Lotus supports LLVM 12 and 14
#if LLVM_VERSION_MAJOR == 12
#define LLVM12 1
#elif LLVM_VERSION_MAJOR == 14
#define LLVM14 1
#else
#error Unsupported version of LLVM (only 12 and 14 are supported)
#endif

// Dynamic analysis is disabled in Lotus integration
#undef ENABLE_DYNAMIC

// forward declarations for llvm classes
namespace llvm
{
    class Value;
    class Function;
    class Module;
    class Instruction;
}

namespace sprattus
{
using std::unique_ptr;
using std::shared_ptr;
using std::make_unique;
using std::make_shared;

using std::endl;

// forward declarations
class AbstractValue;
class Analyzer;
class ConcreteState;
class Fragment;
class FragmentDecomposition;
class InstructionSemantics;
class Semantics;
class ValueMapping;
class PrettyPrinter;

extern std::ostream vout;
extern bool VerboseEnable;

struct VOutBlock {
    VOutBlock(std::string name) { vout << name << " {{{" << endl; }

    ~VOutBlock()
    {
        vout << endl
             << "}}}" << endl;
    }
};

[[noreturn]] void panic(const std::string& in);

std::string escapeJSON(const std::string&);
std::string escapeHTML(const std::string&);

/**
 * Retrieves source file path for a give LLVM function.
 *
 * Returns full path to an existing source file or an empty string if sources
 * cannot be found.
 */
std::string getFunctionSourcePath(const llvm::Function*);

unique_ptr<llvm::Module> loadModule(std::string file_name);

bool isInSSAForm(llvm::Function* function);
}
