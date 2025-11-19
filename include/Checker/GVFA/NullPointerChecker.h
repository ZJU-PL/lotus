#ifndef CHECKER_GVFA_NULLPOINTERCHECKER_H
#define CHECKER_GVFA_NULLPOINTERCHECKER_H

#include "Checker/GVFA/GVFAVulnerabilityChecker.h"

// Forward declarations
class NullCheckAnalysis;
class ContextSensitiveNullCheckAnalysis;

/**
 * Null pointer dereference vulnerability checker
 * 
 * This checker identifies potential null pointer dereference vulnerabilities
 * by finding sources where null values can be introduced and sinks where
 * pointer dereferences occur. Can optionally use NullCheckAnalysis to filter
 * out false positives by proving pointers are not null.
 */
class NullPointerChecker : public GVFAVulnerabilityChecker {
private:
    NullCheckAnalysis *NCA = nullptr;
    ContextSensitiveNullCheckAnalysis *CSNCA = nullptr;
    
public:
    void getSources(Module *M, VulnerabilitySourcesType &Sources) override;
    void getSinks(Module *M, VulnerabilitySinksType &Sinks) override;
    bool isValidTransfer(const Value *From, const Value *To) const override;
    std::string getCategory() const override { return "NullPointer"; }
    
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
    
    /// Set NullCheckAnalysis to improve precision
    void setNullCheckAnalysis(NullCheckAnalysis *NCA) { this->NCA = NCA; }
    
    /// Set context-sensitive NullCheckAnalysis to improve precision
    void setContextSensitiveNullCheckAnalysis(ContextSensitiveNullCheckAnalysis *CSNCA) { 
        this->CSNCA = CSNCA; 
    }
    
    /// Check if a pointer is proven to be non-null at an instruction
    bool isProvenNonNull(const Value *Ptr, const Instruction *Inst) const;
};

#endif // CHECKER_GVFA_NULLPOINTERCHECKER_H

