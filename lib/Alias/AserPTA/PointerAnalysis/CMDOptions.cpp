//
// Created by peiming on 1/10/20.
//

#include <llvm/Support/CommandLine.h>

using namespace llvm;

cl::opt<bool> ConfigPrintConstraintGraph("consgraph", cl::desc("Dump Constraint Graph to dot file"));
cl::opt<bool> ConfigPrintCallGraph("callgraph", cl::desc("Dump Call Graph to dot file"));
cl::opt<bool> ConfigDumpPointsToSet("dump-pts", cl::desc("Dump the Points-to Set of every pointer"));
cl::opt<bool> ConfigUseOnTheFlyCallGraph(
    "on-the-fly-callgraph",
    cl::desc("Use on-the-fly call graph construction during pointer analysis"),
    cl::init(true));