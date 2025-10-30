/* -------------------- IVFGInterDyck.cpp ------------------ */
//
// Created by kisslune on 2/23/23.
//

#include "CFL/POCR/VFA/IVFGInterDyck.h"

using namespace SVF;

/*!
 * build subgraph
 */
void IVFGInterDyck::buildSubGraph()
{
    subGraph = new IVFG(*ivfg);

    /// search all asgn edges
    FIFOWorkList<std::pair<NodeID, NodeID>> pairsToMerge;
    for (auto edge: subGraph->getIVFGEdges())
    {
        if (edge->getEdgeKind() != IVFG::DirectVF)
            continue;

        NodeID srcId = edge->getSrcID();
        NodeID dstId = edge->getDstID();
        if (srcId == dstId)
            continue;

        pairsToMerge.push(std::make_pair(srcId, dstId));
    }

    /// merge
    while (!pairsToMerge.empty())
    {
        auto mergePair = pairsToMerge.pop();
        auto srcRep = subGraph->repNodeID(mergePair.first);
        auto dstRep = subGraph->repNodeID(mergePair.second);
        subGraph->mergeNodeToRep(dstRep, srcRep);
    }

    /// change call into ret
    FIFOWorkList<CFLEdge*> edgesToRemove;
    for (auto edge: subGraph->getIVFGEdges())
    {
        if (edge->getEdgeKind() == IVFG::CallVF)
            edgesToRemove.push(edge);
    }

    while (!edgesToRemove.empty())
    {
        auto edge = edgesToRemove.pop();
        subGraph->addEdge(edge->getDstNode(), edge->getSrcNode(), IVFG::RetVF, edge->getEdgeIdx());
        subGraph->removeIVFGEdge(edge);
    }
}


/*!
 * collapse subgraph by FastDyck
 */
void IVFGInterDyck::fastDyck()
{
    /// init worklist
    for (auto nIt = subGraph->begin(); nIt != subGraph->end(); ++nIt)
    {
        toWorklist(nIt->first);
    }

    /// process worklist
    while (!workList.empty())
    {
        auto rep = workList.pop();
        auto subs = fastDyckRepSubMap[rep];

        for (auto sub: subs)
        {
            /// merge the real nodes
            subGraph->mergeNodeToRep(subGraph->repNodeID(sub), subGraph->repNodeID(rep));
        }

        toWorklist(subGraph->repNodeID(rep));
    }
}


/*!
 * For those who have \<-ret- v -ret->, put into worklist
 */
void IVFGInterDyck::toWorklist(NodeID nId)
{
    std::map<Lbl, NodeBS> lblMap;
    for (auto edge: subGraph->getIVFGNode(nId)->getOutEdges())
    {
        lblMap[edge->getEdgeIdx()].set(edge->getDstID());
    }
    for (auto& indIt: lblMap)
    {
        if (indIt.second.count() > 1)
        {
            NodeID rep = indIt.second.find_first();
            workList.push(rep);
            fastDyckRepSubMap[rep] = indIt.second;
            anchorMap[nId].insert(indIt.first);
        }
    }
}


/*!
 * prune non-contributing edges
 */
void IVFGInterDyck::pruneEdges()
{
    /// mark contributing edges
    for (auto& nIt: anchorMap)
    {
        NodeID rep = subGraph->repNodeID(nIt.first);
        NodeBS& subNodes = subGraph->subNodeIds(rep);
        for (NodeID sub: subNodes)
        {
            /// mark in call edges
            for (auto edge: ivfg->getIVFGNode(sub)->getInEdgeWithTy(IVFG::CallVF))
            {
                Lbl lbl = edge->getEdgeIdx();
                if (nIt.second.find(lbl) != nIt.second.end())
                    edge->setDyckContributing();
            }

            /// mark out ret edges
            for (auto edge: ivfg->getIVFGNode(sub)->getOutEdgeWithTy(IVFG::RetVF))
            {
                Lbl lbl = edge->getEdgeIdx();
                if (nIt.second.find(lbl) != nIt.second.end())
                    edge->setDyckContributing();
            }
        }
    }

    /// remove non-contributing edges
    FIFOWorkList<CFLEdge*> edgesToRemove;
    for (auto edge: ivfg->getIVFGEdges())
    {
        /// omit a-edges
        if (edge->getEdgeKind() == IVFG::DirectVF)
            continue;

        if (!edge->isDyckContributing())
            edgesToRemove.push(edge);
    }

    while (!edgesToRemove.empty())
    {
        CFLEdge* edge = edgesToRemove.pop();
        ivfg->removeIVFGEdge(edge);
    }
}


/*!
 *
 */
void IVFGInterDyck::printSubGraph(std::string fname)
{
    std::ofstream outFile(fname + ".txt", std::ios::out);
    if (!outFile)
    {
        std::cout << "error opening file in consg.cpp!";
        return;
    }

    for (auto edge: subGraph->getIVFGEdges())
    {
        if (edge->getEdgeKind() == IVFG::DirectVF)
            outFile << edge->getSrcID() << "\t" << edge->getDstID() << "\ta" << std::endl;
        if (edge->getEdgeKind() == IVFG::CallVF)
            outFile << edge->getSrcID() << "\t" << edge->getDstID() << "\tcall\t" << edge->getEdgeIdx() << std::endl;
        if (edge->getEdgeKind() == IVFG::RetVF)
            outFile << edge->getSrcID() << "\t" << edge->getDstID() << "\tret\t" << edge->getEdgeIdx() << std::endl;
    }
}
