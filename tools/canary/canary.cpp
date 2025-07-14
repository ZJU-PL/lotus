/*
 *  Canary features a fast unification-based alias analysis for C programs
 *  Copyright (C) 2021 Qingkai Shi <qingkaishi@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Affero General Public License as published
 *  by the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Affero General Public License for more details.
 *
 *  You should have received a copy of the GNU Affero General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "Dataflow/NullPointer/NullCheckAnalysis.h"
#include "Dataflow/NullPointer/ContextSensitiveNullCheckAnalysis.h"
#include "Support/RecursiveTimer.h"
#include "Support/Statistics.h"
#include "Transform/LowerConstantExpr.h"

#include <llvm/Bitcode/BitcodeWriterPass.h>
#include <llvm/IR/IRPrintingPasses.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/InitializePasses.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils.h>
#include <memory>

using namespace llvm;

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input bitcode file>"),
                                          cl::init("-"), cl::value_desc("filename"));

static cl::opt<std::string> OutputFilename("o", cl::desc("Override output filename"),
                                           cl::value_desc("filename"));

static cl::opt<bool> OutputAssembly("S", cl::desc("Write LLVM assembly instead of bitcode"),
                                   cl::init(false));

static cl::opt<bool> OnlyStatistics("s", cl::desc("Only output statistics"), cl::init(false));

static cl::opt<bool> RunContextSensitiveAnalysis("ctx", cl::desc("Run context-sensitive analysis"), 
                                               cl::init(false));

static cl::opt<unsigned> RecordSize("size", cl::desc("Size of context records"), cl::init(1));

int main(int argc, char **argv) {
    InitLLVM X(argc, argv);

    // Enable debug stream buffering.
    llvm::EnableDebugBuffering = true;

    // Call llvm_shutdown() on exit.
    llvm_shutdown_obj Y;

    // Print a stack trace if a fatal signal occurs.
    sys::PrintStackTraceOnErrorSignal(argv[0]);
    PrettyStackTraceProgram Z(argc, argv);

    // Initialize passes
    PassRegistry &Registry = *PassRegistry::getPassRegistry();
    initializeCore(Registry);
    initializeCoroutines(Registry);
    initializeScalarOpts(Registry);
    initializeIPO(Registry);
    initializeAnalysis(Registry);
    initializeTransformUtils(Registry);
    initializeInstCombine(Registry);
    initializeAggressiveInstCombine(Registry);
    initializeTarget(Registry);

    cl::ParseCommandLineOptions(argc, argv, "Bona soundly checks if a pointer may be nullptr.\n");

    SMDiagnostic Err;
    LLVMContext Context;
    std::unique_ptr<Module> M = parseIRFile(InputFilename.getValue(), Err, Context);
    if (!M) {
        Err.print(argv[0], errs());
        return 1;
    }

    if (verifyModule(*M, &errs())) {
        errs() << argv[0] << ": error: input module is broken!\n";
        return 1;
    } else {
        Statistics::run(*M);
        if (OnlyStatistics) return 0;
    }

    legacy::PassManager Passes;

    auto *TransformTimer = new RecursiveTimerPass("Transforming the bitcode");
    Passes.add(TransformTimer->start());
    Passes.add(createLowerAtomicPass());
    Passes.add(createLowerInvokePass());
    Passes.add(createPromoteMemoryToRegisterPass());
    Passes.add(createSCCPPass());
    Passes.add(createLoopSimplifyPass());
    Passes.add(new LowerConstantExpr());
    Passes.add(TransformTimer->done());
    if (!OutputAssembly.getValue()) {
        auto *AnalysisTimer = new RecursiveTimerPass("Analyzing the bitcode");
        Passes.add(AnalysisTimer->start());
        
        // Choose which analysis to run based on command line options
        if (RunContextSensitiveAnalysis) {
            errs() << "Running context-sensitive null check analysis\n";
            Passes.add(new ContextSensitiveNullCheckAnalysis());
        } else {
            errs() << "Running context-insensitive null check analysis\n";
            Passes.add(new NullCheckAnalysis());
        }
        
        Passes.add(AnalysisTimer->done());
    }

    std::unique_ptr<ToolOutputFile> Out;
    if (!OutputFilename.getValue().empty()) {
        std::error_code EC;
#if defined(LLVM12)
        Out = std::make_unique<ToolOutputFile>(OutputFilename, EC, sys::fs::F_None);
#elif defined(LLVM14)
        Out = std::make_unique<ToolOutputFile>(OutputFilename, EC, sys::fs::OF_None);
#else
    #error "Unsupported LLVM version"
#endif
        if (EC) {
            errs() << EC.message() << '\n';
            return 1;
        }

        if (OutputAssembly.getValue()) {
            Passes.add(createPrintModulePass(Out->os()));
        } else {
            Passes.add(createBitcodeWriterPass(Out->os()));
        }
    }

    Passes.run(*M);

    if (Out) Out->keep();

    return 0;
}