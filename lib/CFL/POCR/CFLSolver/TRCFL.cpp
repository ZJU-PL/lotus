//
// Created by kisslune on 6/27/23.
//

#include "CFL/POCR/CFLSolver/CFLSolver.h"

using namespace SVF;


void TRCFL::processCFLItem(CFLItem item)
{
    /// Process other items
    for (Label newTy : unarySumm(item.label()))
        if (checkAndAddEdge(item.src(), item.dst(), newTy))
            pushIntoWorklist(item.src(), item.dst(), newTy);

    for (auto& iter : cflData()->getSuccs(item.dst()))
    {
        Label rty = iter.first;
        for (Label newTy : binarySumm(item.label(), rty))
        {
            NodeBS diffDsts;
            if (grammar()->isTransitive(newTy.first))   // transitive
            {
                diffDsts = computeDiffEdges(item.src(), iter.second, newTy);
                if (newTy.first == item.label().first && newTy.first == rty.first)      // secondary
                    // TODO: not push into worklist?
                    secondaryData.addEdges(item.src(), diffDsts, newTy);
                else    // primary
                    cflData()->addEdges(item.src(), diffDsts, newTy);
            }
            else    // non-transitive
                diffDsts = checkAndAddEdges(item.src(), iter.second, newTy);

            for (NodeID diffDst : diffDsts)
                pushIntoWorklist(item.src(), diffDst, newTy);
        }
    }

    for (auto& iter : cflData()->getPreds(item.src()))
    {
        Label lty = iter.first;
        for (Label newTy : binarySumm(lty, item.label()))
        {
            NodeBS diffSrcs;
            if (grammar()->isTransitive(newTy.first))   /// transitive
            {
                diffSrcs = computeDiffEdges(iter.second, item.dst(), newTy);
                if (newTy.first == item.label().first && newTy.first == lty.first)      // secondary
                    secondaryData.addEdges(diffSrcs, item.dst(), newTy);
                else
                    cflData()->addEdges(diffSrcs, item.dst(), newTy);
            }
            else    // non-transitive
                diffSrcs = checkAndAddEdges(iter.second, item.dst(), newTy);

            for (NodeID diffSrc : diffSrcs)
                pushIntoWorklist(diffSrc, item.dst(), newTy);
        }
    }
}


/*!
 * All the transitive items generated during ordinary solving is regarded as primary
 */
bool TRCFL::pushIntoWorklist(NodeID src, NodeID dst, Label ty, bool isPrimary)
{
    return CFLBase::pushIntoWorklist(src, dst, ty, isPrimary);
}


NodeBS TRCFL::computeDiffEdges(NodeID src, const NodeBS& dstSet, Label lbl)
{
    if (!lbl.first)
        return emptyBS;

    NodeBS retSet;
    stat->checks += dstSet.count();
    retSet.intersectWithComplement(dstSet, cflData()->getSuccs(src, lbl));
    retSet.intersectWithComplement(retSet, secondaryData.getSuccs(src, lbl));
    return retSet;
}


NodeBS TRCFL::computeDiffEdges(const NodeBS& srcSet, NodeID dst, Label lbl)
{
    if (!lbl.first)
        return emptyBS;

    NodeBS retSet;
    stat->checks += srcSet.count();
    retSet.intersectWithComplement(srcSet, cflData()->getPreds(dst, lbl));
    retSet.intersectWithComplement(retSet, secondaryData.getPreds(dst, lbl));
    return retSet;
}


void TRCFL::countSumEdges()
{
    /// calculate summary edges
    stat->numOfSumEdges = 0;
    for (auto it1 = cflData()->begin(); it1 != cflData()->end(); ++it1)
        for (auto& it2 : it1->second)
            stat->numOfSumEdges += it2.second.count();

//    for (auto it1 = secondaryData.begin(); it1 != secondaryData.end(); ++it1)
//        for (auto& it2 : it1->second)
//            stat->numOfSumEdges += it2.second.count();

    /// calculate S edges
    stat->sEdgeSet.clear();
    for (auto& it1 : cflData()->getSuccMap())
        for (auto& it2 : it1.second)
            if (grammar()->isCountSymbol(it2.first.first))
                stat->sEdgeSet[it1.first] |= it2.second;

    for (auto& it1 : secondaryData.getSuccMap())
        for (auto& it2 : it1.second)
            if (grammar()->isCountSymbol(it2.first.first))
                stat->sEdgeSet[it1.first] |= it2.second;

    for (auto& it : stat->sEdgeSet)
        it.second.reset(it.first);

    stat->numOfCountEdges = 0;
    for (auto& it1 : stat->sEdgeSet)
        stat->numOfCountEdges += it1.second.count();
}