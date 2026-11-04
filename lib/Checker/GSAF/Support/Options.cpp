#include "Checker/GSAF/Support/Options.h"

#include "Checker/Framework/Subcommands.h"

#include <climits>

#include <llvm/Support/CommandLine.h>
namespace lotus {
namespace gsaf {
using namespace llvm;
std::string GSAFOptions::DebugFunction;
static cl::opt<std::string, true> DebugFunction(
    "gsaf.debug-func", cl::sub(lotus::checker::tooling::gsafSubCommand()),
    cl::desc("Debugging the specified function for path sensitive analysis."),
    cl::value_desc("Function name"), cl::location(GSAFOptions::DebugFunction),
    cl::init(""), cl::ReallyHidden);

bool GSAFOptions::DebugConstraints;
static cl::opt<bool, true> DebugConstraints(
    "gsaf.debug-constraints",
    cl::sub(lotus::checker::tooling::gsafSubCommand()),
    cl::desc("Printing constraints for debugging in path sensitive checker."),
    cl::location(GSAFOptions::DebugConstraints), cl::init(false),
    cl::ReallyHidden);

bool GSAFOptions::DebugTrace;
static cl::opt<bool, true> DebugTrace(
    "gsaf.debug-trace", cl::sub(lotus::checker::tooling::gsafSubCommand()),
    cl::desc("Printing traces for debugging in path sensitive checker."),
    cl::location(GSAFOptions::DebugTrace), cl::init(false), cl::ReallyHidden);

unsigned GSAFOptions::InlineDepth;
static cl::opt<unsigned, true> InlineDepth(
    "gsaf.inline-depth", cl::sub(lotus::checker::tooling::gsafSubCommand()),
    cl::desc("Set the depth for inlining callees"),
    cl::location(GSAFOptions::InlineDepth), cl::init(6), cl::Hidden);

int GSAFOptions::SolverVersion;
static cl::opt<int, true> SolverVersion(
    "gsaf.solver-version", cl::sub(lotus::checker::tooling::gsafSubCommand()),
    cl::desc("Using the solver that may generate redundant constraints."),
    cl::location(GSAFOptions::SolverVersion), cl::init(1), cl::Hidden);

bool GSAFOptions::EnableCSSymSummary;
static cl::opt<bool, true> EnableCSSymSummary(
    "gsaf.enable-cs-symbol", cl::sub(lotus::checker::tooling::gsafSubCommand()),
    cl::desc("Using the summary of relevant call sites."),
    cl::location(GSAFOptions::EnableCSSymSummary), cl::init(true), cl::Hidden);

unsigned GSAFOptions::InlineCSSymDepth;
static cl::opt<unsigned, true>
    InlineCSSymDepth("gsaf.inline-cs-symbol-depth",
                     cl::sub(lotus::checker::tooling::gsafSubCommand()),
                     cl::desc("Set the depth for inlining symbolic summary."),
                     cl::location(GSAFOptions::InlineCSSymDepth), cl::init(6),
                     cl::Hidden);

bool GSAFOptions::DotGVFGValFlow;
static cl::opt<bool, true> DotSkeleton(
    "gsaf.dot-value-flow", cl::sub(lotus::checker::tooling::gsafSubCommand()),
    cl::desc("Dotting the value flow for GVFG."),
    cl::location(GSAFOptions::DotGVFGValFlow), cl::init(false), cl::Hidden);

std::string GSAFOptions::CollectGVFGData;
static cl::opt<std::string, true> CollectGVFGData(
    "gsaf.gvfg-data", cl::sub(lotus::checker::tooling::gsafSubCommand()),
    cl::desc("Collecting the GVFG statistics.  If file name is not specified, "
             "use the name FunctionNodeData.json"),
    cl::location(GSAFOptions::CollectGVFGData),
    cl::init("FunctionNodeData.json"), cl::Hidden);

unsigned GSAFOptions::Timeout;
static cl::opt<unsigned, true> Timeout(
    "gsaf.timeout", cl::sub(lotus::checker::tooling::gsafSubCommand()),
    cl::desc(
        "Set the value (seconds) of timeout. The actual time for analyzing a "
        "function may exceed the value. The default value is 60s."),
    cl::location(GSAFOptions::Timeout), cl::init(60), cl::Hidden);

int GSAFOptions::MaxSummary;
static cl::opt<int, true>
    MaxSummaryCL("gsaf.restrict-summary-count",
                 cl::sub(lotus::checker::tooling::gsafSubCommand()),
                 cl::desc("Set the maximal summary that can exist in a single "
                          "function (-1 means no restriction)"),
                 cl::location(GSAFOptions::MaxSummary), cl::init(-1),
                 cl::ReallyHidden);

/*
 * If gsaf.enable-side-effect-source = true:
 *     side effect inputs of a function will be used to produce input summaries
 *     e.g.
 *          for function f (x) {}, not only x but also x->field will be used.
 *
 * If gsaf.enable-side-effect-source = false:
 *     only parameters of a function is used to produce input summaries
 */
bool GSAFOptions::EnableSideEffectSource;
static cl::opt<bool, true>
    EnableSideEffectSource("gsaf.enable-side-effect-source",
                           cl::sub(lotus::checker::tooling::gsafSubCommand()),
                           cl::desc("Enabling side-effect sources."),
                           cl::location(GSAFOptions::EnableSideEffectSource),
                           cl::init(false), cl::Hidden);

bool GSAFOptions::EnableArithmeticFlow = false;
static cl::opt<bool, true> arithmetic(
    "gsaf.arithmetic-flow", cl::sub(lotus::checker::tooling::gsafSubCommand()),
    cl::location(GSAFOptions::EnableArithmeticFlow), cl::init(false));
bool GSAFOptions::EnableHeapAllocFailure = false;
static cl::opt<bool, true>
    heap_failure("gsaf.heap-alloc-failure",
                 cl::sub(lotus::checker::tooling::gsafSubCommand()),
                 cl::location(GSAFOptions::EnableHeapAllocFailure),
                 cl::init(false));
bool GSAFOptions::EnableFileAllocFailure = false;
static cl::opt<bool, true>
    file_failure("gsaf.file-alloc-failure",
                 cl::sub(lotus::checker::tooling::gsafSubCommand()),
                 cl::location(GSAFOptions::EnableFileAllocFailure),
                 cl::init(false));
std::string GSAFOptions::MemorySpecPath;
static cl::opt<std::string, true>
    MemorySpecPath("gsaf.memory-spec",
                   cl::sub(lotus::checker::tooling::gsafSubCommand()),
                   cl::location(GSAFOptions::MemorySpecPath), cl::init(""));
std::string GSAFOptions::IOSpecPath;
static cl::opt<std::string, true>
    IOSpecPath("gsaf.io-spec",
               cl::sub(lotus::checker::tooling::gsafSubCommand()),
               cl::location(GSAFOptions::IOSpecPath), cl::init(""));
std::string GSAFOptions::BufferSpecPath;
static cl::opt<std::string, true>
    BufferSpecPath("gsaf.buffer-spec",
                   cl::sub(lotus::checker::tooling::gsafSubCommand()),
                   cl::location(GSAFOptions::BufferSpecPath), cl::init(""));
std::string GSAFOptions::TaintSpecPath;
static cl::opt<std::string, true>
    TaintSpecPath("gsaf.taint-spec",
                  cl::sub(lotus::checker::tooling::gsafSubCommand()),
                  cl::location(GSAFOptions::TaintSpecPath), cl::init(""));

} // namespace gsaf
} // namespace lotus
