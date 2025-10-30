//===- PEG.cpp -- Program Expression Graph-----------------------------//

/*
 * PEG.cpp
 *
 *  Created on: Nov 22, 2019
 *      Author: Yuxiang Lei
 */

#include "CFL/POCR/CFLSolver/CFLBase.h"
#include "Util/SVFUtil.h" // need SVF?
#include "CFL/POCR/CFLData/PEG.h"
#include <iostream>

using namespace SVF;
using namespace SVFUtil;


/*!
 * Constructor
 */
PEG::PEG() : maxNodeID(0)
{
    /// Specify direct edge kinds for construction
    CFLNode::directEdgeKinds.clear();
    CFLNode::directEdgeKinds.insert(Asgn);
}


/*!
 * Construct a `uni-directed` PEG by reading from graph.
 */
void PEG::readGraph(std::string fname)
{
    std::ifstream gFile;
    gFile.open(fname, std::ios::in);
    if (!gFile.is_open())
    {
        std::cout << "error opening " << fname << std::endl;
        exit(0);
    }

    NodeSet nodeSet;
    std::string line;
    while (getline(gFile, line))
    {
        std::vector<std::string> vec = split(line, '\t');
        if (vec.empty())
            continue;

        NodeID src = stoi(vec[0]);
        NodeID dst = stoi(vec[1]);
        std::string lbl = vec[2];
        addPEGNode(src);
        addPEGNode(dst);

        if (vec[2] == "a")
        {
            addEdge(src, dst, Asgn);
        }
        if (vec[2] == "d")
        {
            addEdge(src, dst, Deref);
        }
        if (vec[2] == "f_i")
        {
            addEdge(src, dst, Gep, std::stoi(vec[3]));
        }
    }

    gFile.close();
}


void PEG::copyBuild(const PEG& rhs)
{
    /// initialize nodes
    for (auto it = rhs.begin(), eit = rhs.end(); it != eit; ++it)
    {
        if (!it->second->hasIncomingEdge() && !it->second->hasOutgoingEdge())
            continue;
        addPEGNode(it->first);
    }

    /// initialize edges
    for (auto edge: rhs.getPEGEdges())
    {
        addEdge(edge->getSrcID(), edge->getDstID(), edge->getEdgeKind(), edge->getEdgeIdx());
    }
}


//@{
bool PEG::addEdge(NodeID srcId, NodeID dstId, CFLEdge::GEdgeKind k, u32_t idx)
{
    CFLNode* src = getPEGNode(srcId);
    CFLNode* dst = getPEGNode(dstId);
    return addEdge(src, dst, k, idx);
}

bool PEG::addEdge(CFLNode* src, CFLNode* dst, CFLEdge::GEdgeKind k, u32_t idx)
{
    if (hasEdge(src, dst, k, idx))
        return false;

    if (k == Asgn && src == dst)
        return false;

    CFLEdge* edge = new CFLEdge(src, dst, k, idx);
    bool added = pegEdgeSet.insert(edge).second;
    src->addOutEdgeWithKind(edge, k);
    dst->addInEdgeWithKind(edge, k);
    return added;
}
//@}


/*!
 * Remove edge from their src and dst edge sets
 */
void PEG::removePEGEdge(CFLEdge* edge)
{
    getPEGNode(edge->getSrcID())->removeCFLOutEdge(edge);
    getPEGNode(edge->getDstID())->removeCFLInEdge(edge);
    bool num1 = pegEdgeSet.erase(edge);
    delete edge;

    assert(num1 && "edge not in the set, can not remove!!!");
}


void PEG::reTargetDstOfEdge(CFLEdge* edge, CFLNode* newDstNode)
{
    NodeID newDstNodeID = newDstNode->getId();
    NodeID srcId = edge->getSrcID();

    addEdge(srcId, newDstNodeID, edge->getEdgeKind(), edge->getEdgeIdx());
    removePEGEdge(edge);
}


void PEG::reTargetSrcOfEdge(CFLEdge* edge, CFLNode* newSrcNode)
{
    NodeID newSrcNodeID = newSrcNode->getId();
    NodeID dstId = edge->getDstID();

    addEdge(newSrcNodeID, dstId, edge->getEdgeKind(), edge->getEdgeIdx());
    removePEGEdge(edge);
}


bool PEG::moveInEdgesToRepNode(CFLNode* node, CFLNode* rep)
{
    std::vector<CFLEdge*> sccEdges;
    std::vector<CFLEdge*> nonSccEdges;
    for (CFLNode::const_iterator it = node->InEdgeBegin(), eit = node->InEdgeEnd(); it != eit; ++it)
    {
        CFLEdge* subInEdge = *it;
        if (repNodeID(subInEdge->getSrcID()) != rep->getId())
            nonSccEdges.push_back(subInEdge);
        else
            sccEdges.push_back(subInEdge);
    }
    /// if this edge is outside scc, then re-target edge dst to rep
    while (!nonSccEdges.empty())
    {
        CFLEdge* edge = nonSccEdges.back();
        nonSccEdges.pop_back();
        reTargetDstOfEdge(edge, rep);
    }

    bool selfCycle = !sccEdges.empty();
    /// if this edge is inside scc, then remove this edge and two end nodes
    while (!sccEdges.empty())
    {
        CFLEdge* edge = sccEdges.back();
        sccEdges.pop_back();
        reTargetDstOfEdge(edge, rep);
    }
    return selfCycle;
}


bool PEG::moveOutEdgesToRepNode(CFLNode* node, CFLNode* rep)
{

    std::vector<CFLEdge*> sccEdges;
    std::vector<CFLEdge*> nonSccEdges;

    for (CFLNode::const_iterator it = node->OutEdgeBegin(), eit = node->OutEdgeEnd(); it != eit;
         ++it)
    {
        CFLEdge* subOutEdge = *it;
        if (repNodeID(subOutEdge->getDstID()) != rep->getId())
            nonSccEdges.push_back(subOutEdge);
        else
            sccEdges.push_back(subOutEdge);
    }
    /// if this edge is outside scc, then re-target edge src to rep
    while (!nonSccEdges.empty())
    {
        CFLEdge* edge = nonSccEdges.back();
        nonSccEdges.pop_back();
        reTargetSrcOfEdge(edge, rep);
    }
    bool selfCycle = !sccEdges.empty();
    /// if this edge is inside scc, then remove this edge and two end nodes
    while (!sccEdges.empty())
    {
        CFLEdge* edge = sccEdges.back();
        sccEdges.pop_back();
        reTargetSrcOfEdge(edge, rep);
    }
    return selfCycle;
}


void PEG::mergeNodeToRep(NodeID nodeId, NodeID newRepId)
{
    if (nodeId == newRepId || repNodeID(nodeId) != nodeId)
        return;

    CFLNode* node = getPEGNode(nodeId);
    /// move the edges from node to rep, and remove the node
    moveEdgesToRepNode(node, getPEGNode(newRepId));
    /// set rep and sub relations
    updateNodeRepAndSubs(node->getId(), newRepId);
    removePEGNode(node);
}


void PEG::updateNodeRepAndSubs(NodeID nodeId, NodeID newRepId)
{
    setRep(nodeId, newRepId);
    NodeBS repSubs;
    repSubs.set(nodeId);
    /// update nodeToRepMap, for each subs of current node updates its rep to newRepId
    ///  update nodeToSubsMap, union its subs with its rep Subs
    NodeBS& nodeSubs = subNodeIds(nodeId);
    for (NodeID subId: nodeSubs)
        setRep(subId, newRepId);
    repSubs |= nodeSubs;
    setSubs(newRepId, repSubs);
    resetSubs(nodeId);
}


/*!
 * Write graph to file
 */
void PEG::writeGraph(std::string name)
{
    std::ofstream outFile(name, std::ios::out);
    if (!outFile)
    {
        std::cout << "error opening file!";
        return;
    }

    std::set<NodeID> srcs;

    for (auto nIt = begin(); nIt != end(); ++nIt)
    {
        srcs.insert(nIt->first);
    }

    for (auto src: srcs)
    {
        auto node = getPEGNode(src);
        for (auto edge: node->getOutEdgeWithTy(Asgn))
        {
            NodeID dst = edge->getDstID();
            outFile << src << "\t" << dst << "\ta" << std::endl;
            outFile << dst << "\t" << src << "\tabar" << std::endl;
        }
        for (auto edge: node->getOutEdgeWithTy(Deref))
        {
            NodeID dst = edge->getDstID();
            outFile << src << "\t" << dst << "\td" << std::endl;
            outFile << dst << "\t" << src << "\tdbar" << std::endl;
        }
        for (auto edge: node->getOutEdgeWithTy(Gep))
        {
            NodeID dst = edge->getDstID();
            outFile << src << "\t" << dst << "\tf_i" << "\t" << edge->getEdgeIdx() << std::endl;
            outFile << dst << "\t" << src << "\tfbar_i" << "\t" << edge->getEdgeIdx() << std::endl;
        }
    }

    outFile.close();
}
