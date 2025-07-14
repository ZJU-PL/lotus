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

#ifndef NULLPOINTER_LOCALNULLCHECKANALYSIS_H
#define NULLPOINTER_LOCALNULLCHECKANALYSIS_H

#include <llvm/ADT/BitVector.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instruction.h>
#include <llvm/Pass.h>
#include <set>
#include <unordered_map>

#include "Alias/DyckAA/DyckValueFlowAnalysis.h"
#include "Dataflow/NullPointer/NullEquivalenceAnalysis.h"
#include "Dataflow/NullPointer/NullFlowAnalysis.h"

using namespace llvm;

typedef std::pair<Instruction *, unsigned> Edge;

class LocalNullCheckAnalysis {
private:
    /// Mapping an instruction to a mask, if ith bit of mask is set, it must not be null pointer
    std::unordered_map<Instruction *, int32_t> InstNonNullMap;

    /// Ptr -> ID
    std::unordered_map<Value *, size_t> PtrIDMap;

    /// Edge -> a BitVector, in which if IDth bit is set, the corresponding ptr is not null
    std::map<Edge, BitVector> DataflowFacts;

    /// unreachable edges collected during nca
    std::set<Edge> UnreachableEdges;

    /// The function we analyze
    Function *F;

    /// ptr groups
    NullEquivalenceAnalysis NEA;

    /// nfa
    NullFlowAnalysis *NFA;

    /// dt
    DominatorTree DT;

public:
    LocalNullCheckAnalysis(NullFlowAnalysis* NFA, Function *F);

    ~LocalNullCheckAnalysis();

    /// \p Ptr must be an operand of \p Inst
    /// return true if \p Ptr at \p Inst may be a null pointer
    bool mayNull(Value *Ptr, Instruction *Inst);

    void run();

private:
    void nca();

    void merge(std::vector<Edge> &, BitVector &);

    void transfer(Edge, const BitVector &, BitVector &);

    void tag();

    void init();

    void label();

    void label(Edge);
};

#endif //NULLPOINTER_LOCALNULLCHECKANALYSIS_H
