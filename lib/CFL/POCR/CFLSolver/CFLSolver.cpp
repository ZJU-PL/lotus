/* -------------------- CFLSolver.cpp ------------------ */
//
// Created by kisslune on 7/5/22.
//

#include "CFL/POCR/CFLSolver/CFLSolver.h"

using namespace SVF;

void StdCFL::initialize()
{
    _grammar = new CFG();
    _grammar->parseGrammar(grammarName);

    _graph = new CFLGraph(_grammar);
    _graph->readGraph(graphName);

    stat = new CFLStat(this);
    stat->setMemUsageBefore();

    initSolver();
}


void StdCFL::finalize()
{
    stat->setMemUsageAfter();

    dumpStat();
    if (!CFLOpt::outGraphFName().empty())
        graph()->writeGraph(CFLOpt::outGraphFName());
}


void StdCFL::analyze()
{
    initialize();

    std::thread th(StdCFL::timer);      // timer thread

    /// start solving
    double propStart = stat->getClk();

    do
    {
        stat->numOfIteration++;
        reanalyze = false;
        if (CFLOpt::solveCFL())
            solve();
    } while (reanalyze);

    double propEnd = stat->getClk();
    stat->timeOfSolving += (propEnd - propStart) / TIMEINTERVAL;

    finalize();

    pthread_cancel(th.native_handle());     // kill timer
    th.join();
}


Set<Label> StdCFL::unarySumm(Label lty)
{
    Set<Label> retVal;
    auto& lhsSet = grammar()->getLhs(lty.first);

    for (auto lhs : lhsSet)
    {
        if (!lhs)
            continue;

        if (grammar()->isaVariantSymbol(lhs) && grammar()->isaVariantSymbol(lty.first))
            retVal.insert(Label(lhs, lty.second));
        else
            retVal.insert(Label(lhs, 0));
    }

    return retVal;
}


Set<Label> StdCFL::binarySumm(Label lty, Label rty)
{
    Set<Label> retVal;
    auto lhsSet = grammar()->getLhs(std::make_pair(lty.first, rty.first));

    for (auto lhs : lhsSet)
    {
        if (!lhs)       // a fault label
            continue;

        if (grammar()->isaVariantSymbol(lty.first))
        {
            if ((grammar()->isaVariantSymbol(rty.first) && lty.second == rty.second)
                || !grammar()->isaVariantSymbol(rty.first))
            {
                if (grammar()->isaVariantSymbol(lhs))
                    retVal.insert(Label(lhs, lty.second));
                else
                    retVal.insert(Label(lhs, 0));
            }
        }
        else if (grammar()->isaVariantSymbol(rty.first))
        {
            if (grammar()->isaVariantSymbol(lhs))
                retVal.insert(Label(lhs, rty.second));
            else
                retVal.insert(Label(lhs, 0));
        }
        else
            retVal.insert(Label(lhs, 0));
    }

    return retVal;
}


void StdCFL::initSolver()
{
    /// add all edges into adjacency list and worklist
    for (auto edge : graph()->getCFLEdges())
    {
        cflData()->addEdge(edge->getSrcID(), edge->getDstID(), std::make_pair(edge->getEdgeKind(), edge->getEdgeIdx()));
        pushIntoWorklist(edge->getSrcID(), edge->getDstID(), std::make_pair(edge->getEdgeKind(), edge->getEdgeIdx()));
    }

    /// processing empty rules, i.e., X ::= epsilon
    for (auto nIter = graph()->begin(); nIter != graph()->end(); ++nIter)
    {
        NodeID nodeId = nIter->first;
        for (auto lhs : grammar()->getEmptyRules())
        {
            cflData()->addEdge(nodeId, nodeId, std::make_pair(lhs, 0));
            pushIntoWorklist(nodeId, nodeId, std::make_pair(lhs, 0));
        }
    }
}


void StdCFL::dumpStat()
{
    if (CFLOpt::PStat() && stat)
        stat->performStat();
}


void StdCFL::countSumEdges()
{
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

    for (auto& it : stat->sEdgeSet)
        it.second.reset(it.first);

    stat->numOfCountEdges = 0;
    for (auto& it1 : stat->sEdgeSet)
        stat->numOfCountEdges += it1.second.count();
}


/// ---------------- CFL data methods with UCFL options ----------------------------

void StdCFL::addEdge(NodeID src, NodeID dst, Label lbl)
{
    if (!lbl.first || (CFLOpt::ucfl() && !grammar()->isInsertSymbol(lbl.first)))
    {
//        if (grammar()->isCountSymbol(lbl.first))
//            countData.addEdge(src, dst, lbl);
        return;
    }
    cflData()->addEdge(src, dst, lbl);
}


bool StdCFL::checkAndAddEdge(NodeID src, NodeID dst, Label lbl)
{
    if (!lbl.first)
        return false;

    // TODO: need to count the number of checks when lbl is a follow label??
    if (CFLOpt::ucfl() && !grammar()->isInsertSymbol(lbl.first))
    {
//        if (grammar()->isCountSymbol(lbl.first))
//            countData.addEdge(src, dst, lbl);
        return true;
    }
    stat->checks++;
    return cflData()->checkAndAddEdge(src, dst, lbl);
}


NodeBS StdCFL::checkAndAddEdges(NodeID src, const NodeBS& dstSet, Label lbl)
{
    if (!lbl.first)
        return emptyBS;

    // TODO: need to count the number of checks when lbl is a follow label??
    if (CFLOpt::ucfl() && !grammar()->isInsertSymbol(lbl.first))
    {
//        if (grammar()->isCountSymbol(lbl.first))
//            countData.checkAndAddEdges(src, dstSet, lbl);
        return dstSet;
    }
    stat->checks += dstSet.count();
    return cflData()->checkAndAddEdges(src, dstSet, lbl);
}


NodeBS StdCFL::checkAndAddEdges(const NodeBS& srcSet, NodeID dst, Label lbl)
{
    if (!lbl.first)
        return emptyBS;

    // TODO: need to count the number of checks when lbl is a follow label??
    if (CFLOpt::ucfl() && !grammar()->isInsertSymbol(lbl.first))
    {
//        if (grammar()->isCountSymbol(lbl.first))
//            countData.checkAndAddEdges(srcSet, dst, lbl);
        return srcSet;
    }
    stat->checks += srcSet.count();
    return cflData()->checkAndAddEdges(srcSet, dst, lbl);
}
