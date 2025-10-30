//
// Created by kisslune on 3/13/22.
//

#include "CFL/POCR/VFA/VFAnalysis.h"

using namespace SVF;


void GspanVFA::initSolver()
{
    for (CFLEdge* edge: graph()->getIVFGEdges())
    {
        if (edge->getEdgeKind() == IVFG::DirectVF)
        {
            checkAndAddEdge(edge->getSrcID(), edge->getDstID(), std::make_pair(a, 0));
        }
        else if (edge->getEdgeKind() == IVFG::CallVF)
        {
            u32_t offset = edge->getEdgeIdx();
            checkAndAddEdge(edge->getSrcID(), edge->getDstID(), std::make_pair(call, offset));
        }
        else if (edge->getEdgeKind() == IVFG::RetVF)
        {
            u32_t offset = edge->getEdgeIdx();
            checkAndAddEdge(edge->getSrcID(), edge->getDstID(), std::make_pair(ret, offset));
        }
    }

    for (auto nIter = graph()->begin(); nIter != graph()->end(); ++nIter)
    {
        NodeID nodeId = nIter->first;
        checkAndAddEdge(nodeId, nodeId, std::make_pair(A, 0));
    }
}


void GspanVFA::solve()
{
    reanalyze = false;
    CFLData resData;

    for (auto& srcIter: cflData()->getSuccMap())
    {
        NodeID src = srcIter.first;

        // old + new
        for (auto& tyIter: oldData()->getSuccs(src))
        {
            Label lty = tyIter.first;
            for (NodeID oldDst1: tyIter.second)
            {
                // new
                for (auto& tyIter2: cflData()->getSuccs(oldDst1))
                {
                    for (Label newTy: binarySumm(lty, tyIter2.first))
                        if (newTy.first && (resData.getSuccs(src, newTy) |= tyIter2.second))
                            stat->checks += tyIter2.second.count();       // stat
                }
            }
        }

        // new + old and new
        for (auto tyIter: cflData()->getSuccs(src))
        {
            Label lty = tyIter.first;
            for (Label newTy: unarySumm(lty))
                for (NodeID newDst1: tyIter.second)
                {
                    if (newTy.first && resData.getSuccs(src, newTy).test_and_set(newDst1))
                        stat->checks++;       // stat
                    // old
                    for (auto& tyIter2: oldData()->getSuccs(newDst1))
                    {
                        for (Label newTy: binarySumm(lty, tyIter2.first))
                            if (newTy.first && (resData.getSuccs(src, newTy) |= tyIter2.second))
                                stat->checks += tyIter2.second.count();       // stat
                    }
                    // new
                    for (auto& tyIter2: cflData()->getSuccs(newDst1))
                    {
                        for (Label newTy: binarySumm(lty, tyIter2.first))
                            if (newTy.first && (resData.getSuccs(src, newTy) |= tyIter2.second))
                                stat->checks += tyIter2.second.count();       // stat
                    }
                }
        }

        // update old and new
        for (auto& tyIter: cflData()->getSuccs(src))
        {
            oldData()->getSuccs(src, tyIter.first) |= tyIter.second;
            tyIter.second.clear();
        }
        for (auto& tyIter: resData.getSuccs(src))
        {
            cflData()->getSuccs(src, tyIter.first) = tyIter.second;
            cflData()->getSuccs(src, tyIter.first).intersectWithComplement(oldData()->getSuccs(src, tyIter.first));
            if (!cflData()->getSuccs(src, tyIter.first).empty())
                reanalyze = true;
        }
    }
}


void GspanVFA::countSumEdges()
{
    stat->numOfSumEdges = 0;
    std::set<u32_t> s = {A, Cl};

    for (auto iter1 = oldData()->begin(); iter1 != oldData()->end(); ++iter1)
    {
        for (auto& iter2: iter1->second)
        {
            if (s.find(iter2.first.first) != s.end())
                stat->numOfSumEdges += iter2.second.count();
        }
    }
}
