//===- IVFAnalysis.cpp -- Program Expression Graph-----------------------------//

/*
 * IVFAnalysis.cpp
 *
 *  Created on: Aug 1, 2020
 *      Author: Yuxiang Lei
 */

#include "CFL/POCR/VFA/VFAnalysis.h"

using namespace SVF;
using namespace SVFUtil;


/// ------------------- VFA Base Methods -----------------------

void VFAnalysis::initialize()
{
    setGraph(new IVFG());
    graph()->readGraph(graphName);

    stat = new VFAStat(this);
    stat->setMemUsageBefore();

    /// Graph simplification
    simplifyGraph();
    /// initialize online solver
    initSolver();
}


void VFAnalysis::analyze()
{
    initialize();

    std::thread th(VFAnalysis::timer);     // timer thread

    /// Start solving
    double propHorStart = stat->getClk();

    do
    {
        stat->numOfIteration++;
        reanalyze = false;
        if (CFLOpt::solveCFL())
            solve();
    } while (reanalyze);

    double propHorEnd = stat->getClk();
    stat->timeOfSolving += (propHorEnd - propHorStart) / TIMEINTERVAL;

    /// Finalize the analysis
    finalize();

    pthread_cancel(th.native_handle());     // kill timer
    th.join();
}


void VFAnalysis::finalize()
{
    stat->setMemUsageAfter();

    dumpStat();
    if (!CFLOpt::outGraphFName().empty())
        graph()->writeGraph(CFLOpt::outGraphFName());
}


bool VFAnalysis::pushIntoWorklist(NodeID src, NodeID dst, Label ty)
{
    if (ty.first == fault)
        return false;

    return CFLBase::pushIntoWorklist(src, dst, ty);
}


bool VFAnalysis::checkAndAddEdge(NodeID src, NodeID dst, Label lbl)
{
    if (!lbl.first)
        return false;

    stat->checks++;
    return cflData()->checkAndAddEdge(src, dst, lbl);
}


NodeBS VFAnalysis::checkAndAddEdges(NodeID src, const NodeBS& dstSet, Label lbl)
{
    if (!lbl.first)
        return emptyBS;

    stat->checks += dstSet.count();
    return cflData()->checkAndAddEdges(src, dstSet, lbl);
}


NodeBS VFAnalysis::checkAndAddEdges(const NodeBS& srcSet, NodeID dst, Label lbl)
{
    if (!lbl.first)
        return emptyBS;

    stat->checks += srcSet.count();
    return cflData()->checkAndAddEdges(srcSet, dst, lbl);
}


void VFAnalysis::countSumEdges()
{
    stat->numOfSumEdges = 0;

    for (auto iter1 = cflData()->begin(); iter1 != cflData()->end(); ++iter1)
        for (auto& iter2 : iter1->second)
        {
            stat->numOfSumEdges += iter2.second.count();
            if (iter2.first.first == A)
                stat->numOfSEdges += iter2.second.count();
        }
}


/// ------------------- Std VFA Methods ----------------------

Set<Label> StdVFA::binarySumm(Label lty, Label rty)
{
    u32_t lWord = lty.first;
    u32_t rWord = rty.first;

    u32_t lInd = lty.second;
    u32_t rInd = rty.second;

    if (lWord == A && rWord == A)
        return {std::make_pair(A, 0)};

    if (lWord == call && rWord == A)
        return {std::make_pair(Cl, lInd)};

    if (lWord == Cl && rWord == ret && lInd == rInd)
        return {std::make_pair(A, 0)};

    return {std::make_pair(fault, 0)};
}


Set<Label> StdVFA::unarySumm(Label lty)
{
    u32_t lWord = lty.first;
    if (lWord == a)
        return {std::make_pair(A, 0)};

    return {std::make_pair(fault, 0)};
}


void StdVFA::initSolver()
{
    for (CFLEdge* edge : graph()->getIVFGEdges())
    {
        NodeID srcId = edge->getSrcID();
        NodeID dstId = edge->getDstID();

        if (edge->getEdgeKind() == IVFG::DirectVF)
        {
            checkAndAddEdge(srcId, dstId, std::make_pair(a, 0));
            pushIntoWorklist(srcId, dstId, std::make_pair(a, 0));
        }
        if (edge->getEdgeKind() == IVFG::CallVF)
        {
            checkAndAddEdge(srcId, dstId, std::make_pair(call, edge->getEdgeIdx()));
            pushIntoWorklist(srcId, dstId, std::make_pair(call, edge->getEdgeIdx()));
        }
        if (edge->getEdgeKind() == IVFG::RetVF)
        {
            checkAndAddEdge(srcId, dstId, std::make_pair(ret, edge->getEdgeIdx()));
            pushIntoWorklist(srcId, dstId, std::make_pair(ret, edge->getEdgeIdx()));
        }
    }

    /// A ::= epsilon
    for (auto nIter = graph()->begin(); nIter != graph()->end(); ++nIter)
    {
        NodeID nodeId = nIter->first;
        checkAndAddEdge(nodeId, nodeId, std::make_pair(A, 0));
        pushIntoWorklist(nodeId, nodeId, std::make_pair(A, 0));
    }
}
