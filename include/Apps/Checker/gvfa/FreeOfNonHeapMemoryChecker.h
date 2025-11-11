#ifndef CHECKER_GVFA_FREEOFNONHEAPMEMORYCHECKER_H
#define CHECKER_GVFA_FREEOFNONHEAPMEMORYCHECKER_H

#include "Apps/Checker/gvfa/GVFAVulnerabilityChecker.h"

/**
 * @class FreeOfNonHeapMemoryChecker
 * @brief Detects attempts to free non-heap memory (stack/global)
 * 
 * Tracks flow from stack allocations (alloca) and global variables 
 * to free() calls, which is undefined behavior.
 */
class FreeOfNonHeapMemoryChecker : public GVFAVulnerabilityChecker {
public:
    FreeOfNonHeapMemoryChecker() = default;
    ~FreeOfNonHeapMemoryChecker() override = default;
    
    void getSources(Module *M, VulnerabilitySourcesType &Sources) override;
    void getSinks(Module *M, VulnerabilitySinksType &Sinks) override;
    bool isValidTransfer(const Value *From, const Value *To) const override;
    std::string getCategory() const override { return "Free of Non-Heap Memory"; }
    int registerBugType() override;
    void reportVulnerability(int bugTypeId, const Value *Source, 
                           const Value *Sink, 
                           const std::set<const Value *> *SinkInsts) override;
    int detectAndReport(Module *M, DyckGlobalValueFlowAnalysis *GVFA, 
                       bool contextSensitive = false, 
                       bool verbose = false) override;
};

#endif // CHECKER_GVFA_FREEOFNONHEAPMEMORYCHECKER_H

