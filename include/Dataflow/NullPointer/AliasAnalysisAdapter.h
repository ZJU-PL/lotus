/*
 *  Author: rainoftime
 *  Date: 2025-04
 *  Description: Adapter interface for alias analyses used by NullPointer analyses
 */

#ifndef NULLPOINTER_ALIASANALYSISADAPTER_H
#define NULLPOINTER_ALIASANALYSISADAPTER_H

#include <llvm/IR/Value.h>
#include <llvm/IR/Instruction.h>
#include <set>
#include <map>

using namespace llvm;

// Forward declarations for LLVM classes
namespace llvm {
class CFLSteensAAResult;
class CFLAndersAAResult;
class Module;
}

// Abstract interface for alias analysis adapters
class AliasAnalysisAdapter {
public:
    virtual ~AliasAnalysisAdapter() = default;

    // Return true if V1 may alias with V2 at the given instruction point
    virtual bool mayAlias(Value *V1, Value *V2, Instruction *InstPoint, 
                        bool IncludeI = true) = 0;
    
    // Return true if V may be null at the given instruction point
    virtual bool mayNull(Value *V, Instruction *InstPoint, 
                       bool BeforeInstruction = false) = 0;
                       
    // Factory method to create the appropriate adapter
    // The caller takes ownership of the returned object
    static AliasAnalysisAdapter* createAdapter(Module *M, Function *F);
};

// Adapter for DyckAA
class DyckAAAdapter : public AliasAnalysisAdapter {
private:
    Function *Fn; // Function this adapter analyzes

public:
    explicit DyckAAAdapter(Function *F);
    ~DyckAAAdapter();
    
    bool mayAlias(Value *V1, Value *V2, Instruction *InstPoint, 
                 bool IncludeI = true) override;
                 
    bool mayNull(Value *V, Instruction *InstPoint, 
                bool BeforeInstruction = false) override;
};

// Adapter for CFLAA
class CFLAAAdapter : public AliasAnalysisAdapter {
private:
    Module *ModuleRef;
    llvm::CFLSteensAAResult *SteensAAResult;
    llvm::CFLAndersAAResult *AndersAAResult;
    bool UseSteensgaard;

public:
    explicit CFLAAAdapter(Module *M, llvm::CFLSteensAAResult *SteensAA, 
                         llvm::CFLAndersAAResult *AndersAA, bool UseSteens);
    ~CFLAAAdapter();
    
    bool mayAlias(Value *V1, Value *V2, Instruction *InstPoint, 
                 bool IncludeI = true) override;
                 
    bool mayNull(Value *V, Instruction *InstPoint, 
                bool BeforeInstruction = false) override;
};

#endif // NULLPOINTER_ALIASANALYSISADAPTER_H 