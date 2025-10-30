//
// Created by kisslune on 2/22/23.
//

#include "CFL/POCR/AA/AliasAnalysis.h"

using namespace SVF;

void AliasAnalysis::simplifyGraph()
{
    double startClk = stat->getClk();

    if (CFLOpt::scc() || CFLOpt::graphSimp())
    {
        SCCElimination();
    }
    if (CFLOpt::gf() || CFLOpt::graphSimp())
    {
        graphFolding();
    }
    if (CFLOpt::interDyck())
    {
        interDyckGS();
    }

    double endClk = stat->getClk();
    stat->gsTime = (endClk - startClk) / TIMEINTERVAL;
}


void AliasAnalysis::graphFolding()
{

    double startClk = stat->getClk();

    if (!pegFold)
        pegFold = new PEGFold(_graph);

    pegFold->foldGraph();
    pegFold->mergeDeref();

    double endClk = stat->getClk();
    stat->gfTime = (endClk - startClk) / TIMEINTERVAL;
}


void AliasAnalysis::interDyckGS()
{
    double startClk = stat->getClk();

    if (!interDyck)
        interDyck = new PEGInterDyck(_graph);

    interDyck->buildSubGraph();
    interDyck->fastDyck();
    interDyck->pruneEdges();

    delete interDyck;
    interDyck = nullptr;

    double endClk = stat->getClk();
    stat->interDyckTime = (endClk - startClk) / TIMEINTERVAL;
}


void AliasAnalysis::SCCElimination()
{
    double startClk = stat->getClk();

    if (!scc)
        scc = new SCC(_graph);
    SCCDetect();

    double endClk = stat->getClk();
    stat->sccTime = (endClk - startClk) / TIMEINTERVAL;
}


void AliasAnalysis::SCCDetect()
{
    scc->find();
    mergeSCCCycle();
}


void AliasAnalysis::mergeSCCCycle()
{
    NodeStack revTopoOrder;
    NodeBS repNodes;
    NodeStack& topoOrder = scc->topoNodeStack();
    while (!topoOrder.empty())
    {
        NodeID repNodeId = topoOrder.top();
        topoOrder.pop();
        revTopoOrder.push(repNodeId);
        repNodes.set(repNodeId);

        const NodeBS& subNodes = scc->subNodes(repNodeId);
        // merge sub nodes to rep node
        mergeSCCNodes(repNodeId, subNodes);
    }

    // restore the topological order for later solving.
    while (!revTopoOrder.empty())
    {
        NodeID nodeId = revTopoOrder.top();
        revTopoOrder.pop();
        topoOrder.push(nodeId);
    }
}


void AliasAnalysis::mergeSCCNodes(NodeID repNodeId, const NodeBS& subNodes)
{
    for (NodeBS::iterator nodeIt = subNodes.begin(); nodeIt != subNodes.end(); nodeIt++)
    {
        NodeID subNodeId = *nodeIt;
        if (subNodeId != repNodeId)
        {
            graph()->mergeNodeToRep(subNodeId, repNodeId);
        }
    }
}