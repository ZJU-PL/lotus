#ifndef ANALYSIS_GVFA_GLOBALVALUEFLOWANALYSIS_H
#define ANALYSIS_GVFA_GLOBALVALUEFLOWANALYSIS_H

#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>
#include <queue>

#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/raw_ostream.h>

#include "Alias/DyckAA/DyckVFG.h"
#include "Alias/DyckAA/DyckAliasAnalysis.h"
#include "Alias/DyckAA/DyckModRefAnalysis.h"
#include "Analysis/GVFA/VulnerabilityCheckers.h"

using namespace llvm;

// Hash function for pair<const CallInst *, const Function *>
namespace std {
    template<>
    struct hash<std::pair<const CallInst *, const Function *>> {
        size_t operator()(const std::pair<const CallInst *, const Function *> &p) const {
            return std::hash<const CallInst *>()(p.first) ^ 
                   (std::hash<const Function *>()(p.second) << 1);
        }
    };
}

// Vulnerability source/sink types
using ValueSitePairType = std::pair<const Value *, int>;
using VulnerabilitySourcesType = std::map<ValueSitePairType, int>;
using VulnerabilitySinksType = std::map<const Value *, std::set<const Value *> *>;

/**
 * Global Value Flow Analysis using Dyck VFG
 * 
 * Tracks data flow from vulnerability sources to sinks.
 */
class DyckGlobalValueFlowAnalysis {
public:
    long AllQueryCounter = 0;
    long SuccsQueryCounter = 0;
    long SnapshotedOnlineTime = 0;

private:
    // Reachability maps
    std::unordered_map<const Value *, int> ReachabilityMap;
    std::unordered_map<const Value *, int> BackwardReachabilityMap;
    
    // All-pairs reachability maps
    std::unordered_map<const Value *, std::unordered_set<const Value *>> AllReachabilityMap;
    std::unordered_map<const Value *, std::unordered_set<const Value *>> AllBackwardReachabilityMap;
    
    // Call site indexing
    std::unordered_map<const CallInst *, int> CallSiteIndexMap;
    std::unordered_map<std::pair<const CallInst *, const Function *>, int> CallSiteCalleePairIndexMap;
    
    // Core components
    DyckVFG *VFG = nullptr;
    DyckAliasAnalysis *DyckAA = nullptr;
    DyckModRefAnalysis *DyckMRA = nullptr;
    Module *M = nullptr;
    
    // Sources and sinks
    VulnerabilitySourcesType Sources;
    std::vector<std::pair<const Value *, int>> SourcesVec;
    VulnerabilitySinksType Sinks;
    
    // Vulnerability checker
    std::unique_ptr<VulnerabilityChecker> VulnChecker;

public:
    DyckGlobalValueFlowAnalysis(Module *M, DyckVFG *VFG, DyckAliasAnalysis *DyckAA, 
                               DyckModRefAnalysis *DyckMRA);
    
    ~DyckGlobalValueFlowAnalysis();
    
    // Public interface
    void setVulnerabilityChecker(std::unique_ptr<VulnerabilityChecker> checker);
    void run();
    
    // Query interfaces
    int reachable(const Value *V, int Mask);
    bool backwardReachable(const Value *V);
    bool srcReachable(const Value *V, const Value *Src) const;
    bool backwardReachableSink(const Value *V);
    bool backwardReachableAllSinks(const Value *V);
    
    // CFL reachability
    bool cflReachable(const Value *From, const Value *To) const;
    bool cflBackwardReachable(const Value *From, const Value *To) const;
    bool contextSensitiveReachable(const Value *From, const Value *To) const;
    bool contextSensitiveBackwardReachable(const Value *From, const Value *To) const;
    
    // Utilities
    void printOnlineQueryTime(llvm::raw_ostream &O, const char *Title = "[Online]") const;
    VulnerabilityChecker* getVulnerabilityChecker() const { return VulnChecker.get(); }

private:
    // Analysis algorithms
    void optimizedRun();
    void detailedRun();
    void optimizedForwardRun(std::vector<std::pair<const Value *, int>> &Sources);
    void detailedForwardRun(std::vector<std::pair<const Value *, int>> &Sources);
    void optimizedBackwardRun();
    void detailedBackwardRun();
    void extendSources(std::vector<std::pair<const Value *, int>> &Sources);
    
    // Reachability algorithms
    void forwardReachability(const Value *Node, int Mask);
    void backwardReachability(const Value *Node);
    void detailedForwardReachability(const Value *Node, const Value *Src);
    void detailedBackwardReachability(const Value *Node, const Value *Sink);
    bool onlineReachability(const Value *Target);
    bool onlineForwardReachability(const Value *Node, std::unordered_set<const Value *> &visited);
    bool onlineBackwardReachability(const Value *Node, const Value *Target, std::unordered_set<const Value *> &visited);
    
    // Helper functions
    int count(const Value *V, int Mask);
    bool count(const Value *V);
    int backwardCount(const Value *V);
    bool allCount(const Value *V, const Value *Src);
    bool allBackwardCount(const Value *V, const Value *Sink);
    int getCallSiteID(const CallInst *CI);
    int getCallSiteID(const CallInst *CI, const Function *Callee);
    void processCallSite(const CallInst *CI, const Value *Node, int Mask, std::queue<std::pair<const Value *, int>> &WorkQueue);
    void processReturnSite(const ReturnInst *RI, const Value *Node, int Mask, std::queue<std::pair<const Value *, int>> &WorkQueue);
    bool isValueFlowEdge(const Value *From, const Value *To) const;
    std::vector<const Value *> getSuccessors(const Value *V) const;
    std::vector<const Value *> getPredecessors(const Value *V) const;
    void initializeCFLAnalyzer();
    bool performCFLReachabilityQuery(const Value *From, const Value *To, bool Forward) const;
    bool cflReachabilityQuery(const Value *From, const Value *To, bool Forward) const;
    int getValueNodeID(const Value *V) const;
};

#endif // ANALYSIS_GVFA_GLOBALVALUEFLOWANALYSIS_H