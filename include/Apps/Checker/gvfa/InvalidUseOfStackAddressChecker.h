#ifndef CHECKER_GVFA_INVALIDUSEOFSTACKADDRESSCHECKER_H
#define CHECKER_GVFA_INVALIDUSEOFSTACKADDRESSCHECKER_H

#include "Apps/Checker/gvfa/GVFAVulnerabilityChecker.h"

/**
 * @class InvalidUseOfStackAddressChecker
 * @brief Detects invalid use of stack addresses (return/escape from scope)
 * 
 * Tracks flow from stack allocations (alloca) to return instructions and
 * stores to global memory, which allows stack addresses to escape their scope.
 */
class InvalidUseOfStackAddressChecker : public GVFAVulnerabilityChecker {
public:
    InvalidUseOfStackAddressChecker() = default;
    ~InvalidUseOfStackAddressChecker() override = default;
    
    void getSources(Module *M, VulnerabilitySourcesType &Sources) override;
    void getSinks(Module *M, VulnerabilitySinksType &Sinks) override;
    bool isValidTransfer(const Value *From, const Value *To) const override;
    std::string getCategory() const override { return "Invalid Use of Stack Address"; }
    int registerBugType() override;
    void reportVulnerability(int bugTypeId, const Value *Source, 
                           const Value *Sink, 
                           const std::set<const Value *> *SinkInsts) override;
    int detectAndReport(Module *M, DyckGlobalValueFlowAnalysis *GVFA, 
                       bool contextSensitive = false, 
                       bool verbose = false) override;
};

#endif // CHECKER_GVFA_INVALIDUSEOFSTACKADDRESSCHECKER_H

