#ifndef DATAFLOW_GVFA_GLOBALVALUEFLOWANALYSIS_H
#define DATAFLOW_GVFA_GLOBALVALUEFLOWANALYSIS_H

#include <map>
#include <set>
#include <vector>
#include <memory>
#include <chrono>
#include <mutex>

#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/raw_ostream.h>

#include "Alias/DyckAA/DyckVFG.h"
#include "Alias/DyckAA/DyckAliasAnalysis.h"
#include "Alias/DyckAA/DyckModRefAnalysis.h"

using namespace llvm;

// Forward declarations
class VulnerabilityChecker;

// Vulnerability source/sink types
using ValueSitePairType = std::pair<const Value *, int>;
using VulnerabilitySourcesType = std::map<ValueSitePairType, int>;
using VulnerabilitySinksType = std::map<const Value *, std::set<const Value *> *>;

/**
 * Global Value Flow Analysis using Dyck VFG
 * 
 * This class performs value flow analysis using the Dyck Value Flow Graph
 * to detect potential vulnerabilities by tracking data flow from sources
 * to sinks.
 */
class DyckGlobalValueFlowAnalysis {
public:
    long AllQueryCounter = 0;
    long SuccsQueryCounter = 0;
    long SnapshotedOnlineTime = 0;

private:
    /// Reachability maps for optimization
    std::map<const Value *, int> ReachabilityMap;
    std::map<const Value *, int> BackwardReachabilityMap;
    
    /// All-pairs reachability maps for comprehensive analysis
    std::map<const Value *, std::set<const Value *>> AllReachabilityMap;
    std::map<const Value *, std::set<const Value *>> AllBackwardReachabilityMap;
    
    /// Call site indexing for context sensitivity
    std::map<const CallInst *, int> CallSiteIndexMap;
    std::map<std::pair<const CallInst *, const Function *>, int> CallSiteCalleePairIndexMap;
    
    /// Core analysis components
    DyckVFG *VFG = nullptr;
    DyckAliasAnalysis *DyckAA = nullptr;
    DyckModRefAnalysis *DyckMRA = nullptr;
    Module *M = nullptr;
    
    /// Vulnerability sources and sinks
    VulnerabilitySourcesType Sources;
    std::vector<std::pair<const Value *, int>> SourcesVec;
    VulnerabilitySinksType Sinks;
    
    /// Vulnerability checker interface
    std::unique_ptr<VulnerabilityChecker> VulnChecker;

public:
    DyckGlobalValueFlowAnalysis(Module *M, DyckVFG *VFG, DyckAliasAnalysis *DyckAA, 
                               DyckModRefAnalysis *DyckMRA);
    
    ~DyckGlobalValueFlowAnalysis();
    
    /// Set vulnerability checker
    void setVulnerabilityChecker(std::unique_ptr<VulnerabilityChecker> checker);
    
    /// Main analysis entry point
    void run();
    
    /// Query interfaces
    int reachable(const Value *V, int Mask);
    bool backwardReachable(const Value *V);
    bool srcReachable(const Value *V, const Value *Src) const;
    bool backwardReachableSink(const Value *V);
    bool backwardReachableAllSinks(const Value *V);
    
    /// Performance monitoring
    void printOnlineQueryTime(llvm::raw_ostream &O, const char *Title = "[Online]") const;

private:
    /// Analysis algorithms
    void optimizedRun();
    void comprehensiveRun();
    
    /// Forward analysis
    void optimizedForwardRun(std::vector<std::pair<const Value *, int>> &Sources);
    void comprehensiveForwardRun(std::vector<std::pair<const Value *, int>> &Sources);
    
    /// Backward analysis
    void optimizedBackwardRun();
    void comprehensiveBackwardRun();
    
    /// Source extension (alias analysis integration)
    void extendSources(std::vector<std::pair<const Value *, int>> &Sources);
    
    /// Slicing algorithms
    void forwardSlicing(const Value *Node, int Mask);
    void backwardSlicing(const Value *Node);
    void comprehensiveForwardSlicing(const Value *Node, const Value *Src);
    void comprehensiveBackwardSlicing(const Value *Node, const Value *Sink);
    
    /// Online slicing for queries
    bool onlineSlicing(const Value *Target);
    bool onlineForwardSlicing(const Value *Node, 
                             std::map<const Value *, int> &localCountMap);
    bool onlineBackwardSlicing(const Value *Node, const Value *Target,
                              std::map<const Value *, int> &localCountMap);
    
    /// Counting helpers for reachability tracking
    int count(const Value *V, int Mask);
    bool count(const Value *V);
    int countConst(const Value *V, int Mask) const;
    int backwardCount(const Value *V);
    int backwardCountConst(const Value *V) const;
    
    bool allCount(const Value *V, const Value *Src);
    bool allCountConst(const Value *V, const Value *Src) const;
    bool allBackwardCount(const Value *V, const Value *Sink);
    bool allBackwardCountConst(const Value *V, const Value *Sink) const;
    
    /// Call site management
    int getCallSiteID(const CallInst *CI);
    int getCallSiteID(const CallInst *CI, const Function *Callee);
    
    /// Helper methods
    void processCallSite(const CallInst *CI, const Value *Node, int Mask);
    void processReturnSite(const ReturnInst *RI, const Value *Node, int Mask);
    bool isValueFlowEdge(const Value *From, const Value *To) const;
    
    /// VFG navigation helpers
    std::vector<const Value *> getSuccessors(const Value *V) const;
    std::vector<const Value *> getPredecessors(const Value *V) const;
};

/**
 * Simple vulnerability checker interface
 */
class VulnerabilityChecker {
public:
    virtual ~VulnerabilityChecker() = default;
    
    /// Get vulnerability sources from the module
    virtual void getSources(Module *M, VulnerabilitySourcesType &Sources) = 0;
    
    /// Get vulnerability sinks from the module  
    virtual void getSinks(Module *M, VulnerabilitySinksType &Sinks) = 0;
    
    /// Check if a value transfer is valid for this vulnerability type
    virtual bool isValidTransfer(const Value *From, const Value *To) const = 0;
    
    /// Get vulnerability category
    virtual std::string getCategory() const = 0;
};

/**
 * Null pointer vulnerability checker
 */
class NullPointerVulnerabilityChecker : public VulnerabilityChecker {
public:
    void getSources(Module *M, VulnerabilitySourcesType &Sources) override;
    void getSinks(Module *M, VulnerabilitySinksType &Sinks) override;
    bool isValidTransfer(const Value *From, const Value *To) const override;
    std::string getCategory() const override { return "NullPointer"; }
};

/**
 * Taint vulnerability checker
 */
class TaintVulnerabilityChecker : public VulnerabilityChecker {
public:
    void getSources(Module *M, VulnerabilitySourcesType &Sources) override;
    void getSinks(Module *M, VulnerabilitySinksType &Sinks) override;
    bool isValidTransfer(const Value *From, const Value *To) const override;
    std::string getCategory() const override { return "Taint"; }
};

#endif // DATAFLOW_GVFA_GLOBALVALUEFLOWANALYSIS_H