// Sprattus static analysis tool
#include "Analysis/Sprattus/utils.h"
#include "Analysis/Sprattus/Analyzer.h"
#include "Analysis/Sprattus/Config.h"
#include "Analysis/Sprattus/DomainConstructor.h"
#include "Analysis/Sprattus/FragmentDecomposition.h"
#include "Analysis/Sprattus/FunctionContext.h"
#include "Analysis/Sprattus/ModuleContext.h"
#include "Analysis/Sprattus/PrettyPrinter.h"
#include "Analysis/Sprattus/repr.h"
#include "Analysis/Sprattus/domains/MemRegions.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <set>

using namespace llvm;
using namespace sprattus;
static void listConfigurationFiles() {
    const char* configDirs[] = {"../config/sprattus", "../../config/sprattus", 
                                "../../../config/sprattus", "./config/sprattus"};
    
    outs() << "Available configuration files:\n";
    bool foundAny = false;
    for (const char* dir : configDirs) {
        std::error_code EC;
        sys::fs::directory_iterator DirIt(dir, EC), DirEnd;
        if (EC) continue;
        
        SmallVector<std::string, 32> configs;
        for (; DirIt != DirEnd && !EC; DirIt.increment(EC)) {
            StringRef path = DirIt->path();
            if (path.endswith(".conf")) {
                configs.push_back(path.str());
                foundAny = true;
            }
        }
        if (!configs.empty()) {
            std::sort(configs.begin(), configs.end());
            for (const auto& cfg : configs) outs() << "  " << cfg << "\n";
        }
    }
    if (!foundAny) {
        outs() << "No configuration files found in config/sprattus/\n";
    }
    outs() << "\nSee config/sprattus/README.md for details.\n";
}

static cl::opt<std::string> InputFilename(cl::Positional,
                                         cl::desc("<input bitcode file>"),
                                         cl::value_desc("bitcode"));

static cl::opt<std::string> ConfigFile(
    "config",
    cl::desc("Configuration file (see config/sprattus/ for examples)"),
    cl::value_desc("file"));

static cl::opt<std::string> FunctionName(
    "function", 
    cl::desc("Function to analyze (default: main or first function)"),
    cl::value_desc("name"));

static cl::opt<std::string> AbstractDomainName(
    "abstract-domain",
    cl::desc("Abstract domain (use --list-domains for available options)"),
    cl::value_desc("domain"));

static cl::opt<bool> Verbose(
    "verbose",
    cl::desc("Enable verbose output"),
    cl::init(false));

static cl::opt<bool> ListFunctions(
    "list-functions",
    cl::desc("List all functions in the module"),
    cl::init(false));

static cl::opt<bool> ListDomains(
    "list-domains",
    cl::desc("List all available abstract domains"),
    cl::init(false));

static cl::opt<bool> ListConfigs(
    "list-configs",
    cl::desc("List available configuration files"),
    cl::init(false));

static cl::opt<bool> ShowAllBlocks(
    "show-all-blocks",
    cl::desc("Show analysis results for all basic blocks"),
    cl::init(false));

static cl::opt<bool> ShowExitBlocks(
    "show-exit-blocks",
    cl::desc("Show analysis results at exit blocks (return statements)"),
    cl::init(false));

static cl::opt<std::string> FragmentStrategy(
    "fragment-strategy",
    cl::desc("Fragment strategy (Edges|Function|Headers|Body|Backedges)"),
    cl::value_desc("strategy"));

static cl::opt<std::string> MemoryModelVariant(
    "memory-model",
    cl::desc("Memory model (NoMemory|BlockModel|Aligned|LittleEndian)"),
    cl::value_desc("variant"));

static cl::opt<int> WideningDelay("widening-delay",
    cl::desc("Iterations before widening"), cl::init(-1));

static cl::opt<int> WideningFrequency("widening-frequency",
    cl::desc("Widen every N iterations"), cl::init(-1));

static cl::opt<bool> CheckAssertions(
    "check-assertions",
    cl::desc("Check for possibly violated assertions"),
    cl::init(false));

static cl::opt<bool> CheckMemSafety(
    "check-memsafety",
    cl::desc("Check for possibly invalid memory accesses (requires RTTI)"),
    cl::init(false));

int main(int argc, char** argv) {
    cl::ParseCommandLineOptions(argc, argv, 
        "Sprattus Static Analyzer - Abstract Interpretation for LLVM IR\n");
    VerboseEnable = Verbose;

    if (ListConfigs) {
        listConfigurationFiles();
        return 0;
    }

    if (ListDomains) {
        const auto& domains = DomainConstructor::all();
        if (domains.empty()) {
            outs() << "No abstract domains registered.\n";
            return 0;
        }
        outs() << "Available abstract domains:\n";
        for (const auto& domain : domains) {
            outs() << "  " << domain.name();
            if (!domain.description().empty())
                outs() << " - " << domain.description();
            outs() << "\n";
        }
        return 0;
    }

    if (InputFilename.empty()) {
        errs() << "Error: input bitcode file required.\n";
        return 1;
    }

    LLVMContext context;
    SMDiagnostic err;
    std::unique_ptr<Module> module = parseIRFile(InputFilename, err, context);
    if (!module) {
        err.print(argv[0], errs());
        return 1;
    }

    if (ListFunctions) {
        outs() << "Functions in module:\n";
        for (auto& F : *module)
            if (!F.isDeclaration())
                outs() << "  " << F.getName() << "\n";
        return 0;
    }

    Function* targetFunc = nullptr;
    if (FunctionName.empty()) {
        targetFunc = module->getFunction("main");
        if (!targetFunc)
            for (auto& F : *module)
                if (!F.isDeclaration()) {
                    targetFunc = &F;
                    break;
                }
    } else {
        targetFunc = module->getFunction(FunctionName);
    }

    if (!targetFunc) {
        errs() << "Error: Function '" 
               << (FunctionName.empty() ? "main" : FunctionName.getValue())
               << "' not found\n";
        return 1;
    }

    try {
        configparser::Config config(ConfigFile.getValue());

        if (!FragmentStrategy.getValue().empty())
            config.set("FragmentDecomposition", "Strategy",
                       FragmentStrategy.getValue());
        if (!MemoryModelVariant.getValue().empty())
            config.set("MemoryModel", "Variant", MemoryModelVariant.getValue());
        if (WideningDelay >= 0)
            config.set("Analyzer", "WideningDelay", WideningDelay.getValue());
        if (WideningFrequency >= 0)
            config.set("Analyzer", "WideningFrequency",
                       WideningFrequency.getValue());

        const auto& allDomains = DomainConstructor::all();
        DomainConstructor domain;
        std::string domainSource;
        bool fallbackToFirst = false;

        if (!AbstractDomainName.getValue().empty()) {
            auto it = std::find_if(
                allDomains.begin(), allDomains.end(), [&](const auto& d) {
                    return d.name() == AbstractDomainName.getValue();
                });
            if (it != allDomains.end()) {
                domain = *it;
                domainSource = "command line";
            }
        } else {
            DomainConstructor configDomain(config);
            if (!configDomain.isInvalid() || allDomains.empty()) {
                domain = configDomain;
                domainSource =
                    ConfigFile.empty() && !std::getenv("SPRATTUS_CONFIG")
                        ? "built-in defaults"
                        : "config";
            } else if (!allDomains.empty()) {
                domain = allDomains.front();
                fallbackToFirst = true;
                domainSource = "first registered";
            }
        }

        if (domain.isInvalid()) {
            if (AbstractDomainName.empty())
                errs() << "Error: no abstract domains registered.\n";
            else
                errs() << "Error: unknown domain '"
                       << AbstractDomainName.getValue()
                       << "'. Use --list-domains.\n";
            return 1;
        }

        std::string configSource;
        if (!ConfigFile.empty()) {
            configSource = ConfigFile.getValue();
        } else if (const char* env = std::getenv("SPRATTUS_CONFIG")) {
            configSource = std::string(env) + " (SPRATTUS_CONFIG)";
        } else {
            configSource = "<built-in defaults>";
        }

        const bool usingBuiltInDefaults = configSource == "<built-in defaults>";

        auto classifyOrigin = [&](bool setViaCLI) -> std::string {
            if (setViaCLI)
                return "command line";
            if (usingBuiltInDefaults)
                return "default";
            return "config";
        };

        std::string fragmentStrategyValue =
            config.get<std::string>("FragmentDecomposition", "Strategy",
                                    "Function");
        std::string fragmentOrigin = classifyOrigin(
            !FragmentStrategy.getValue().empty());

        std::string analyzerVariant =
            config.get<std::string>("Analyzer", "Variant",
                                    "UnilateralAnalyzer");
        bool incremental =
            config.get<bool>("Analyzer", "Incremental", true);
        int wideningDelay =
            config.get<int>("Analyzer", "WideningDelay", 1);
        int wideningFrequency =
            config.get<int>("Analyzer", "WideningFrequency", 1);
        std::string wideningOrigin =
            classifyOrigin(WideningDelay >= 0 || WideningFrequency >= 0);

        std::string memoryVariant =
            config.get<std::string>("MemoryModel", "Variant", "NoMemory");
        int addressBits =
            config.get<int>("MemoryModel", "AddressBits", -1);
        std::string memoryOrigin =
            classifyOrigin(!MemoryModelVariant.getValue().empty());

        outs() << "Effective configuration:\n";
        outs() << "  Config source: " << configSource << "\n";
        outs() << "  Abstract domain (" << domainSource;
        if (fallbackToFirst)
            outs() << ", fallback";
        if (domainSource == "built-in defaults")
            outs() << ", default";
        outs() << "): " << domain.name() << "\n";
        outs() << "  Fragment strategy: " << fragmentStrategyValue << " ("
               << fragmentOrigin << ")\n";
        outs() << "  Analyzer: " << analyzerVariant
               << (incremental ? " [incremental]" : " [non-incremental]")
               << "\n";
        outs() << "  Widening delay/frequency: " << wideningDelay << "/"
               << wideningFrequency << " (" << wideningOrigin << ")\n";
        outs() << "  Memory model: " << memoryVariant;
        if (addressBits >= 0)
            outs() << " (address bits=" << addressBits << ")";
        outs() << " (" << memoryOrigin << ")\n\n";

        outs() << "Analyzing function: " << targetFunc->getName() << "\n";

        if (!isInSSAForm(targetFunc)) {
            errs() << "Warning: Not in SSA form. Run mem2reg pass first.\n";
        }

        ModuleContext mctx(module.get(), config);
        FunctionContext fctx(targetFunc, &mctx);
        auto fragments = FragmentDecomposition::For(fctx);
        auto analyzer = Analyzer::New(fctx, fragments, domain);

        // Check assertions if requested
        if (CheckAssertions) {
            int num_violations = 0;
            for (auto& bb : *targetFunc) {
                for (auto& instr : bb) {
                    if (llvm::isa<llvm::CallInst>(instr)) {
                        auto& call = llvm::cast<llvm::CallInst>(instr);
                        auto* calledFunc = call.getCalledFunction();
                        if (calledFunc && calledFunc->getName().str() == "__assert_fail") {
                            if (!analyzer->at(&bb)->isBottom()) {
                                num_violations++;
                                PrettyPrinter pp(true);
                                analyzer->at(&bb)->prettyPrint(pp);
                                outs() << "\nViolated assertion at "
                                      << bb.getName().str()
                                      << ". Computed result:\n"
                                      << pp.str() << "\n";
                            }
                        }
                    }
                }
            }
            if (num_violations) {
                outs() << "===================================================="
                      << "====================\n"
                      << "  " << num_violations << " violated assertion"
                      << ((num_violations == 1) ? "" : "s") << " detected.\n";
            } else {
                outs() << "No violated assertions detected.\n";
            }
            return (num_violations < 128) ? num_violations : 1;
        }

        // Check memory safety if requested
        // NOTE: This feature requires RTTI to be enabled (dynamic_cast)
        // Currently disabled as RTTI is not enabled in this build
        if (CheckMemSafety) {          
            // Memory safety checking code (requires RTTI):
            int num_violations = 0;
            // Track already-reported (pointer, basic block) pairs to avoid
            // flooding the user with redundant messages for the same location.
            std::set<std::pair<const llvm::Value*, const llvm::BasicBlock*>>
                reported_invalid;
            for (auto& bb : *targetFunc) {
                bool contains_mem_op = false;
                for (auto& inst : bb) {
                    if (llvm::isa<llvm::StoreInst>(inst) ||
                        llvm::isa<llvm::LoadInst>(inst))
                        contains_mem_op = true;
                }
                if (!contains_mem_op)
                    continue;
                    
                std::vector<const AbstractValue*> vals;
                analyzer->after(&bb)->gatherFlattenedSubcomponents(&vals);
                for (auto& instr : bb) {
                    llvm::Value* ptr = nullptr;
                    if (auto as_store = llvm::dyn_cast<llvm::StoreInst>(&instr)) {
                        ptr = as_store->getPointerOperand();
                    }
                    if (auto as_load = llvm::dyn_cast<llvm::LoadInst>(&instr)) {
                        ptr = as_load->getPointerOperand();
                    }
                    if (ptr) {
                        bool is_okay = false;
                        for (auto v : vals) {
                            if (auto as_vr = dynamic_cast<
                                    const sprattus::domains::ValidRegion*>(v)) {
                                if (as_vr->getRepresentedPointer() == ptr &&
                                    as_vr->isValid()) {
                                    is_okay = true;
                                    break;
                                }
                            }
                        }
                        if (!is_okay) {
                            auto key = std::make_pair(ptr, &bb);
                            auto inserted = reported_invalid.insert(key).second;
                            if (inserted) {
                                num_violations++;
                                outs() << "Possibly invalid memory access to "
                                      << repr(ptr) << " at "
                                      << bb.getName().str() << "\n";
                            }
                        } else {
                            outs() << "Definitely valid memory access to "
                                  << repr(ptr) << " at " << bb.getName().str()
                                  << "\n";
                        }
                    }
                }
            }
            if (num_violations) {
                outs() << "\n"
                      << "===================================================="
                      << "====================\n"
                      << " " << num_violations
                      << " possibly invalid memory access"
                      << ((num_violations == 1) ? "" : "es") << " detected.\n";
            } else {
                outs() << "\n"
                      << "===================================================="
                      << "====================\n"
                      << "No possibly invalid memory accesses detected.\n";
            }
            return (num_violations < 128) ? num_violations : 1;
            
        }

        // Show entry point results
        auto* entryResult = analyzer->at(&targetFunc->getEntryBlock());
        PrettyPrinter entryPp(true);
        entryResult->prettyPrint(entryPp);
        outs() << "\nAnalysis result at entry:\n" << entryPp.str() << "\n";

        // Show results for all blocks if requested
        if (ShowAllBlocks) {
            outs() << "\nAnalysis results for all basic blocks:\n";
            for (auto& BB : *targetFunc) {
                outs() << "\n--- Basic block: " << BB.getName() << " ---\n";
                
                // Results at the beginning of the block (after phi nodes)
                auto* atResult = analyzer->at(&BB);
                PrettyPrinter atPp(true);
                atResult->prettyPrint(atPp);
                outs() << "At beginning:\n" << atPp.str() << "\n";
                
                // Results after the block
                auto* afterResult = analyzer->after(&BB);
                PrettyPrinter afterPp(true);
                afterResult->prettyPrint(afterPp);
                outs() << "After execution:\n" << afterPp.str() << "\n";
            }
        }

        // Show exit block results if requested
        if (ShowExitBlocks) {
            outs() << "\nAnalysis results at exit blocks:\n";
            for (auto& BB : *targetFunc) {
                // Check if this is an exit block (ends with return)
                if (llvm::isa<llvm::ReturnInst>(BB.getTerminator())) {
                    outs() << "\n--- Exit block: " << BB.getName() << " ---\n";
                    auto* exitResult = analyzer->after(&BB);
                    PrettyPrinter exitPp(true);
                    exitResult->prettyPrint(exitPp);
                    outs() << exitPp.str() << "\n";
                }
            }
        }

        outs() << "Analysis completed successfully.\n";

    } catch (const std::exception& e) {
        errs() << "Error during analysis: " << e.what() << "\n";
        return 1;
    }
    return 0;
}