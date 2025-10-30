/* -------------------- PEGFold.cpp ------------------ */
//
// Created by kisslune on 2/22/23.
//

#include "CFL/POCR/AA/PEGFold.h"

using namespace SVF;


void PEGFold::foldGraph()
{
    /// detect foldable pairs
    for (auto edge: peg->getPEGEdges())
    {
        if (edge->getEdgeKind() != PEG::Asgn)
            continue;

        auto dstInAEdges = edge->getDstNode()->getInEdgeWithTy(PEG::Asgn);
        auto dstInEdges = edge->getDstNode()->getInEdges();
        if (dstInAEdges.size() <= 1 && dstInEdges.size() == dstInAEdges.size())
            foldablePairs.push(std::make_pair(edge->getSrcID(), edge->getDstID()));
    }

    /// merge foldable pairs
    while (!foldablePairs.empty())
    {
        NodePair pair = foldablePairs.top();
        foldablePairs.pop();
        NodeID src = peg->repNodeID(pair.first);
        NodeID dst = peg->repNodeID(pair.second);
        if (src == dst)
            continue;

        peg->mergeNodeToRep(dst, src);
    }
}


/*!
 * Merge common source derefs
 */
void PEGFold::mergeDeref()
{
    FIFOWorkList<NodeID> checkNodes;
    for (auto it = peg->begin(); it != peg->end(); ++it)
        checkNodes.push(it->first);

    while  (!checkNodes.empty())
    {
        CFLNode* n = peg->getPEGNode(checkNodes.pop());
        Set<NodeID> dChildren;
        for (auto edge : n->getOutEdgeWithTy(PEG::Deref))
            dChildren.insert(edge->getDstID());

        if (dChildren.size() > 1)
        {
            NodeID dRep = *dChildren.begin();
            for (auto dChild : dChildren)
                peg->mergeNodeToRep(dChild, dRep);
            checkNodes.push(dRep);
        }
    }
}