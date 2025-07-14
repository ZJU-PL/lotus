/*
 *  Canary features a fast unification-based alias analysis for C programs
 *  Copyright (C) 2021 Qingkai Shi <qingkaishi@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Affero General Public License as published
 *  by the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Affero General Public License for more details.
 *
 *  You should have received a copy of the GNU Affero General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef NULLPOINTER_NULLFLOWANALYSIS_H
#define NULLPOINTER_NULLFLOWANALYSIS_H

#include <llvm/Pass.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include "Alias/DyckAA/DyckVFG.h"
#include "Alias/DyckAA/DyckAliasAnalysis.h"

using namespace llvm;

class NullFlowAnalysis : public ModulePass {
private:
    DyckAliasAnalysis *DAA;

    DyckVFG *VFG;

    std::set<std::pair<DyckVFGNode *, DyckVFGNode *>> NonNullEdges;

    std::map<Function *, std::set<std::pair<DyckVFGNode *, DyckVFGNode *>>> NewNonNullEdges;

    std::set<DyckVFGNode *> NonNullNodes;

public:
    static char ID;

    NullFlowAnalysis();

    ~NullFlowAnalysis() override;

    bool runOnModule(Module &M) override;

    void getAnalysisUsage(AnalysisUsage &AU) const override;

public:
    /// return true if some changes happen
    /// return false if nothing is changed
    bool recompute(std::set<Function *> &);

    /// update NewNonNullEdges so that we can call recompute()
    /// @{
    void add(Function *, Value *, Value *);

    void add(Function *, CallInst *, unsigned K); // for call

    void add(Function *, Value *); // for return
    /// @}

    /// return true if the input value is not null
    bool notNull(Value *) const;
};

#endif // NULLPOINTER_NULLFLOWANALYSIS_H
