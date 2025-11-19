#ifndef CHECKER_GVFA_USEAFTERFREECHECKER_H
#define CHECKER_GVFA_USEAFTERFREECHECKER_H

#include "Checker/GVFA/GVFAVulnerabilityChecker.h"

/**
 * Use-after-free vulnerability checker
 * 
 * This checker identifies potential use-after-free vulnerabilities by finding
 * sources where memory is freed and sinks where that memory is subsequently
 * accessed through loads, stores, or other operations.
 */
class UseAfterFreeChecker : public GVFAVulnerabilityChecker {
public:
    void getSources(Module *M, VulnerabilitySourcesType &Sources) override;
    void getSinks(Module *M, VulnerabilitySinksType &Sinks) override;
    bool isValidTransfer(const Value *From, const Value *To) const override;
    std::string getCategory() const override { return "UseAfterFree"; }
    
    /// Register bug type with BugReportMgr
    int registerBugType() override;
    
    /// Report a vulnerability to BugReportMgr
    void reportVulnerability(int bugTypeId, const Value *Source, 
                            const Value *Sink,
                            const std::set<const Value *> *SinkInsts) override;
    
    /// High-level detection method
    int detectAndReport(Module *M, DyckGlobalValueFlowAnalysis *GVFA,
                       bool contextSensitive = false, 
                       bool verbose = false) override;
};

#endif // CHECKER_GVFA_USEAFTERFREECHECKER_H

