#ifndef CHECKER_GVFA_USEOFUNINITIALIZEDVARIABLECHECKER_H
#define CHECKER_GVFA_USEOFUNINITIALIZEDVARIABLECHECKER_H

#include "Checker/GVFA/GVFAVulnerabilityChecker.h"

/**
 * @class UseOfUninitializedVariableChecker
 * @brief Detects use of uninitialized variables
 * 
 * Tracks flow from undefined values (undef, uninitialized allocas) 
 * to uses that may produce undefined behavior.
 */
class UseOfUninitializedVariableChecker : public GVFAVulnerabilityChecker {
public:
    UseOfUninitializedVariableChecker() = default;
    ~UseOfUninitializedVariableChecker() override = default;
    
    void getSources(Module *M, VulnerabilitySourcesType &Sources) override;
    void getSinks(Module *M, VulnerabilitySinksType &Sinks) override;
    bool isValidTransfer(const Value *From, const Value *To) const override;
    std::string getCategory() const override { return "Use of Uninitialized Variable"; }
    int registerBugType() override;
    void reportVulnerability(int bugTypeId, const Value *Source, 
                           const Value *Sink, 
                           const std::set<const Value *> *SinkInsts) override;
    int detectAndReport(Module *M, DyckGlobalValueFlowAnalysis *GVFA, 
                       bool contextSensitive = false, 
                       bool verbose = false) override;
};

#endif // CHECKER_GVFA_USEOFUNINITIALIZEDVARIABLECHECKER_H

