//
// Created by kisslune on 3/13/22.
//

#include "CFL/POCR/VFA/VFAnalysis.h"

using namespace SVF;

void PocrVFA::initSolver()
{
    for (CFLEdge* edge : graph()->getIVFGEdges())
    {
        NodeID srcId = edge->getSrcID();
        NodeID dstId = edge->getDstID();

        if (edge->getEdgeKind() == IVFG::DirectVF)
        {
            cflData()->addEdge(srcId, dstId, Label(a, 0));
            pushIntoWorklist(srcId, dstId, Label(a, 0));
        }
        if (edge->getEdgeKind() == IVFG::CallVF)
            cflData()->addEdge(srcId, dstId, Label(call, edge->getEdgeIdx()));
        if (edge->getEdgeKind() == IVFG::RetVF)
            cflData()->addEdge(srcId, dstId, Label(ret, edge->getEdgeIdx()));
    }

    for (auto it = graph()->begin(); it != graph()->end(); ++it)
    {
        NodeID nId = it->first;
        hybridData.addInd(nId, nId);
        matchCallRet(nId, nId);
    }
}


void PocrVFA::solve()
{
    while (!isWorklistEmpty())
    {
        CFLItem item = popFromWorklist();
        auto& newEdgeMap = hybridData.addArc(item.src(), item.dst());

        for (auto& it1 : newEdgeMap)
            for (auto newDst : it1.second)
                matchCallRet(it1.first, newDst);    // it1.first == newSrc
    }
}


/*!
 * Matching call A ret
 */
void PocrVFA::matchCallRet(NodeID u, NodeID v)
{
    /// vertical handling of matched parentheses
    for (auto& srcIt : cflData()->getPreds(u))
    {
        if (srcIt.first.first == call)
            for (auto& dstIt : cflData()->getSuccs(v))
            {
                if (dstIt.first.first == ret && srcIt.first.second == dstIt.first.second)
                    for (NodeID srcTgt : srcIt.second)
                        for (NodeID dstTgt : dstIt.second)
                        {
                            stat->checks++;
                            pushIntoWorklist(srcTgt, dstTgt, Label(A, 0));
                        }
            }
    }
}


void PocrVFA::addCl(NodeID u, u32_t idx, TreeNode* vNode)
{
    NodeID v = vNode->id;
    if (!checkAndAddEdge(u, v, Label(Cl, idx)))
        return;

    for (auto child : vNode->children)
        addCl(u, idx, child);
}


void PocrVFA::countSumEdges()
{
    /// calculate checks
    stat->checks += hybridData.checks;

    /// calculate summary edges
    for (auto& it1 : cflData()->getSuccMap())
        for (auto& it2 : it1.second)
            if (it2.first.first == call)
            {
                for (NodeID dst : it2.second)
                    addCl(it1.first, it2.first.second, hybridData.getNode(dst, dst));
            }

    VFAnalysis::countSumEdges();

    for (auto& iter : hybridData.indMap)
    {
        stat->numOfSumEdges += iter.second.size();
        stat->numOfSEdges += iter.second.size();
    }
}


