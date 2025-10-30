//
// Created by kisslune on 3/20/22.
//

#include "CFL/POCR/AA/AliasAnalysis.h"

using namespace SVF;

void PocrAA::initSolver()
{
    /// init graph edges
    for (CFLEdge* edge : graph()->getPEGEdges())
    {
        NodeID srcId = edge->getSrcID();
        NodeID dstId = edge->getDstID();

        if (edge->getEdgeKind() == PEG::Asgn)
        {
            cflData()->addEdge(srcId, dstId, Label(a, 0));
            cflData()->addEdge(dstId, srcId, Label(abar, 0));
            pushIntoWorklist(edge->getSrcID(), edge->getDstID(), std::make_pair(a, 0));
        }
        if (edge->getEdgeKind() == PEG::Deref)
        {
            cflData()->addEdge(srcId, dstId, Label(d, 0));
            cflData()->addEdge(dstId, srcId, Label(dbar, 0));
        }
        if (edge->getEdgeKind() == PEG::Gep)
        {
            cflData()->addEdge(srcId, dstId, Label(f, edge->getEdgeIdx()));
            cflData()->addEdge(dstId, srcId, Label(fbar, edge->getEdgeIdx()));
        }
    }

    /// init hybrid data
    for (auto it = graph()->begin(); it != graph()->end(); ++it)
    {
        NodeID nId = it->first;
        hybridData.addInd(nId, nId);
        setV(nId, nId);
    }
}


void PocrAA::solve()
{
    while (!isWorklistEmpty())
    {
        CFLItem item = popFromWorklist();
        Label type = item.label();
        NodeID src = item.src();
        NodeID dst = item.dst();

        if (type.first == a)
        {
            hybridData.addArc(src, dst);
            for (NodeID vSrc : cflData()->getSuccs(src, Label(V, 0)))
                pushIntoWorklist(vSrc, dst, std::make_pair(V, 0));
        }
        else if (type.first == M)
        {
            setM(src, dst);
            addV(hybridData.getNode(src, src), hybridData.getNode(dst, dst));
        }
        else if (type.first == V)
            addV(hybridData.getNode(src, src), hybridData.getNode(dst, dst));
    }
}


void PocrAA::addV(TreeNode* u, TreeNode* v)
{
    if (!setV(u->id, v->id))
        return;

    for (TreeNode* vChild : v->children)
        addV(u, vChild);

    for (TreeNode* uChild : u->children)
        addV(uChild, v);
}


bool PocrAA::setV(NodeID src, NodeID dst)
{
    if (!checkAndAddEdge(src, dst, Label(V, 0)))
        return false;
    checkAndAddEdge(dst, src, Label(V, 0));

    /// solve the parentheses of d and f edges
    checkdEdges(src, dst);
    checkfEdges(src, dst);
    return true;
}


bool PocrAA::hasM(NodeID src, NodeID dst)
{
    stat->checks++;

    if (src == dst)
        return true;

    return cflData()->hasEdge(src, dst, Label(M, 0));
}


void PocrAA::setM(NodeID src, NodeID dst)
{
    checkAndAddEdge(src, dst, Label(M, 0));
    checkAndAddEdge(dst, src, Label(M, 0));
}


/*!
 * Matching parentheses dbar V d
 */
void PocrAA::checkdEdges(NodeID src, NodeID dst)
{
    for (NodeID srcTgt : cflData()->getSuccs(src, Label(d, 0)))
    {
        for (NodeID dstTgt : cflData()->getSuccs(dst, Label(d, 0)))
        {
            if (!hasM(srcTgt, dstTgt))
            {
                pushIntoWorklist(srcTgt, dstTgt, Label(M, 0));
                for (NodeID srcP : cflData()->getPreds(srcTgt, Label(a, 0)))
                    pushIntoWorklist(srcP, dstTgt, Label(a, 0));
                for (NodeID dstP : cflData()->getPreds(dstTgt, Label(a, 0)))
                    pushIntoWorklist(dstP, srcTgt, Label(a, 0));
            }
        }
    }
}


/*!
 * Matching parentheses fbar V f
 */
void PocrAA::checkfEdges(NodeID src, NodeID dst)
{
    for (auto& srcIt : cflData()->getSuccs(src))
    {
        if (srcIt.first.first == f)
            for (auto& dstIt : cflData()->getSuccs(dst))
            {
                if (dstIt.first.first == f && srcIt.first.second == dstIt.first.second)
                    for (NodeID srcTgt : srcIt.second)
                        for (NodeID dstTgt : dstIt.second)
                        {
                            stat->checks++;
                            pushIntoWorklist(srcTgt, dstTgt, Label(V, 0));
                        }
            }
    }
}


void PocrAA::countSumEdges()
{
    /// calculate checks
    stat->checks += hybridData.checks;

    /// calculate summary edges
    for (auto& it1 : cflData()->getSuccMap())
    {
        for (auto& it2 : it1.second)
        {
            /// DV
            if (it2.first.first == d)
                for (NodeID dTgt : it2.second)
                {
                    setM(dTgt, dTgt);
                    for (NodeID dst : cflData()->getSuccs(it1.first, Label(V, 0)))
                        checkAndAddEdge(dTgt, dst, Label(DV, 0));
                }
            /// FV
            if (it2.first.first == f)
                for (NodeID fTgt : it2.second)
                {
                    for (NodeID dst : cflData()->getSuccs(it1.first, Label(V, 0)))
                        checkAndAddEdge(fTgt, dst, Label(FV, it2.first.second));
                }
        }
    }

    AliasAnalysis::countSumEdges();

    for (auto& iter : hybridData.indMap)
        stat->numOfSumEdges += iter.second.size() * 2;  // A and Abar
}
