/* -------------------- IVFGFold.cpp ------------------ */
//
// Created by kisslune on 2/23/23.
//

#include "CFL/POCR/VFA/IVFGFold.h"

using namespace SVF;


void IVFGFold::foldGraph()
{
    /// detect foldable pairs
    for (auto edge: lg->getIVFGEdges())
    {
        if (edge->getEdgeKind() != IVFG::DirectVF)
            continue;

        auto srcOutEdges = edge->getSrcNode()->getOutEdges();
        auto dstInEdges = edge->getDstNode()->getInEdges();
        if (dstInEdges.size() <= 1 && !edge->getDstNode()->isSrc())
            foldablePairs.push(std::make_pair(edge->getSrcID(), edge->getDstID()));
    }

    /// merge foldable pairs
    while (!foldablePairs.empty())
    {
        NodePair pair = foldablePairs.top();
        foldablePairs.pop();
        NodeID src = lg->repNodeID(pair.first);
        NodeID dst = lg->repNodeID(pair.second);
        if (src == dst)
            continue;

        lg->mergeNodeToRep(dst, src);
    }
}
