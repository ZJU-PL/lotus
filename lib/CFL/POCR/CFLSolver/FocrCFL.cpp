//
// Created by kisslune on 30/5/23.
//

#include "CFL/POCR/CFLSolver/CFLSolver.h"

using namespace SVF;


void FocrCFL::initSolver()
{
    StdCFL::initSolver();
    /// Initialize ECG
    for (auto lbl : grammar()->transitiveSymbols)
        ecgs[lbl] = new ECG();
    /// Create ECG nodes
    for (auto it = graph()->begin(); it != graph()->end(); ++it)
    {
        for (auto lbl : grammar()->transitiveSymbols)
        {
            NodeID nId = it->first;
            ecgs[lbl]->addNode(nId);
        }
    }
    /// Remove transitive rules from binary-summarization list
    for (auto lbl : grammar()->transitiveSymbols)
        grammar()->binaryRules[std::make_pair(lbl, lbl)].erase(lbl);
}


void FocrCFL::processCFLItem(CFLItem item)
{
    /// Process primary transitive items
    if (grammar()->isTransitive(item.label().first) && isPrimary(item))
    {
        procPrimaryItem(item);
        return;
    }

    /// Process other items
    for (Label newTy : unarySumm(item.label()))
        if (checkAndAddEdge(item.src(), item.dst(), newTy))
            pushIntoWorklist(item.src(), item.dst(), newTy);

    for (auto& iter : cflData()->getSuccs(item.dst()))
    {
        Label rty = iter.first;
        for (Label newTy : binarySumm(item.label(), rty))
            if (newTy == item.label() && grammar()->isTransitive(rty.first))
            {
                /// X ::= X A
                ECGNode* dst = ecgs[rty.first]->getNode(item.dst());
                checkSuccs(newTy, item.src(), dst);
            }
            else
            {
                NodeBS diffDsts = checkAndAddEdges(item.src(), iter.second, newTy);
                for (NodeID diffDst : diffDsts)
                    pushIntoWorklist(item.src(), diffDst, newTy);
            }
    }

    for (auto& iter : cflData()->getPreds(item.src()))
    {
        Label lty = iter.first;
        for (Label newTy : binarySumm(lty, item.label()))
            if (newTy == item.label() && grammar()->isTransitive(lty.first))
            {
                /// X ::= A X
                ECGNode* src = ecgs[lty.first]->getNode(item.src());
                checkPreds(newTy, src, item.dst());
            }
            else
            {
                NodeBS diffSrcs = checkAndAddEdges(iter.second, item.dst(), newTy);
                for (NodeID diffSrc : diffSrcs)
                    pushIntoWorklist(diffSrc, item.dst(), newTy);
            }
    }
}


/*!
 * Transitive items generated in this methods are marked as secondary
 */
void FocrCFL::procPrimaryItem(CFLItem item)
{
    CFGSymbTy symb = item.label().first;
    NodeID src = item.src();
    NodeID dst = item.dst();

    if (ecgs[symb]->isReachable(src, dst))
        return;

    std::unordered_map<NodeID, NodeBS>* newEdgeMapPtr;

    if (ecgs[symb]->isReachable(dst, src))    // src --> dst is a back edge
        newEdgeMapPtr = &ecgs[symb]->insertBackEdge(src, dst);
    else
        newEdgeMapPtr = &ecgs[symb]->insertForwardEdge(src, dst);

    for (auto& it : *newEdgeMapPtr)
    {
        cflData()->addEdges(it.first, it.second, item.label());
        for (auto newDst : it.second)
            pushIntoWorklist(it.first, newDst, item.label(), false);
    }
}


void FocrCFL::checkPreds(Label newLbl, ECGNode* src, NodeID dst)
{
    for (auto& pred : src->predecessors)
        if (checkAndAddEdge(pred->id, dst, newLbl))
        {
            pushIntoWorklist(pred->id, dst, newLbl);
            checkPreds(newLbl, pred, dst);
        }
}


void FocrCFL::checkSuccs(Label newLbl, NodeID src, ECGNode* dst)
{
    for (auto& succ : dst->successors)
        if (checkAndAddEdge(src, succ->id, newLbl))
        {
            pushIntoWorklist(src, succ->id, newLbl);
            checkSuccs(newLbl, src, succ);
        }
}


/*!
 * All the transitive items generated during ordinary solving is regarded as primary
 */
bool FocrCFL::pushIntoWorklist(NodeID src, NodeID dst, Label ty, bool isPrimary)
{
    return CFLBase::pushIntoWorklist(src, dst, ty, isPrimary);
}


void FocrCFL::countSumEdges()
{
    /// calculate checks
    for (auto it : ecgs)
        stat->checks += it.second->checks;

    StdCFL::countSumEdges();
}


/* ------------------- Methods of TRFocrCFL ----------------- */


void TRFocrCFL::procPrimaryItem(CFLItem item)
{
    CFGSymbTy symb = item.label().first;
    NodeID src = item.src();
    NodeID dst = item.dst();

    if (ecgs[symb]->isReachable(src, dst))
        return;

    std::unordered_map<NodeID, NodeBS>* newEdgeMapPtr;

    if (ecgs[symb]->isReachable(dst, src))    // src --> dst is a back edge
        newEdgeMapPtr = &ecgs[symb]->insertBackEdge(src, dst);
    else
        newEdgeMapPtr = &ecgs[symb]->insertForwardEdge(src, dst);

    for (auto& it : *newEdgeMapPtr)
    {
        // TODO: secondary edge in -tr
        secondaryData.addEdges(it.first, it.second, item.label());
        for (auto newDst : it.second)
            pushIntoWorklist(it.first, newDst, item.label(), false);
    }
}


NodeBS TRFocrCFL::checkAndAddEdges(NodeID src, const NodeBS& dstSet, Label lbl)
{
    /// Here will be no secondary edges generated
    if (grammar()->isTransitive(lbl.first))
    {
        NodeBS diffDsts;
        stat->checks += dstSet.count();
        diffDsts.intersectWithComplement(dstSet, cflData()->getSuccs(src, lbl));
        diffDsts.intersectWithComplement(diffDsts, secondaryData.getSuccs(src, lbl));
        cflData()->addEdges(src, diffDsts, lbl);
        return diffDsts;
    }
    return StdCFL::checkAndAddEdges(src, dstSet, lbl);
}


NodeBS TRFocrCFL::checkAndAddEdges(const NodeBS& srcSet, NodeID dst, Label lbl)
{
    /// Here will be no secondary edges generated
    if (grammar()->isTransitive(lbl.first))
    {
        NodeBS diffSrcs;
        stat->checks += srcSet.count();
        diffSrcs.intersectWithComplement(srcSet, cflData()->getPreds(dst, lbl));
        diffSrcs.intersectWithComplement(diffSrcs, secondaryData.getPreds(dst, lbl));
        cflData()->addEdges(diffSrcs, dst, lbl);
        return diffSrcs;
    }
    return StdCFL::checkAndAddEdges(srcSet, dst, lbl);
}


void TRFocrCFL::countSumEdges()
{
    /// calculate checks
    for (auto it : ecgs)
        stat->checks += it.second->checks;

    /// calculate summary edges
    stat->numOfSumEdges = 0;
    for (auto it1 = cflData()->begin(); it1 != cflData()->end(); ++it1)
        for (auto& it2 : it1->second)
            stat->numOfSumEdges += it2.second.count();

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