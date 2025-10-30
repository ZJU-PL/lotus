/* -------------------- PEGInterDyck.cpp ------------------ */
//
// Created by kisslune on 2/22/23.
//

#include "CFL/POCR/AA/PEGInterDyck.h"

using namespace SVF;

/*!
 * build subgraph
 */
void PEGInterDyck::buildSubGraph()
{
    subGraph = new PEG(*peg);

    /// search all asgn edges
    FIFOWorkList<std::pair<NodeID, NodeID>> pairsToMerge;
    for (auto edge: subGraph->getPEGEdges())
    {
        if (edge->getEdgeKind() != PEG::Asgn)
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
}


/*!
 * collapse subgraph by FastDyck
 */
void PEGInterDyck::fastDyck()
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
 * For those who have \<-d- v -d-> or \<-f- v -f->, put into worklist
 */
void PEGInterDyck::toWorklist(NodeID nId)
{
    std::map<Lbl, NodeBS> lblMap;
    for (auto edge: subGraph->getPEGNode(nId)->getOutEdges())
    {
        lblMap[std::make_pair(edge->getEdgeKind(), edge->getEdgeIdx())].set(edge->getDstID());
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
void PEGInterDyck::pruneEdges()
{
    /// mark contributing edges
    for (auto& nIt: anchorMap)
    {
        NodeID rep = subGraph->repNodeID(nIt.first);
        NodeBS& subNodes = subGraph->subNodeIds(rep);
        for (NodeID sub: subNodes)
        {
            /// mark in original PEG
            for (auto edge: peg->getPEGNode(sub)->getOutEdges())
            {
                Lbl lbl = std::make_pair(edge->getEdgeKind(), edge->getEdgeIdx());
                if (nIt.second.find(lbl) != nIt.second.end())
                    edge->setDyckContributing();
            }
        }
    }

    /// remove non-contributing edges
    FIFOWorkList<CFLEdge*> edgesToRemove;
    for (auto edge: peg->getPEGEdges())
    {
        /// omit a-edges
        if (edge->getEdgeKind() == PEG::Asgn)
            continue;

        if (!edge->isDyckContributing())
            edgesToRemove.push(edge);
    }

    while (!edgesToRemove.empty())
    {
        CFLEdge* edge = edgesToRemove.pop();
        peg->removePEGEdge(edge);
    }
}


/*!
 *
 */
void PEGInterDyck::printSubGraph(std::string fname)
{
    std::ofstream outFile(fname + ".txt", std::ios::out);
    if (!outFile)
    {
        std::cout << "error opening file in consg.cpp!";
        return;
    }

    for (auto edge: subGraph->getPEGEdges())
    {
        if (edge->getEdgeKind() == PEG::Asgn)
            outFile << edge->getSrcID() << "\t" << edge->getDstID() << "\ta" << std::endl;
        if (edge->getEdgeKind() == PEG::Deref)
            outFile << edge->getSrcID() << "\t" << edge->getDstID() << "\td" << std::endl;
        if (edge->getEdgeKind() == PEG::Gep)
            outFile << edge->getSrcID() << "\t" << edge->getDstID() << "\tf\t" << edge->getEdgeIdx() << std::endl;
    }
}
