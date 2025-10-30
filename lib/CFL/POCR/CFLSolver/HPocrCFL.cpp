/* -------------------- HPocrCFL.cpp ------------------ */
//
// Created by kisslune on 3/30/23.
//

#include "CFL/POCR/CFLSolver/CFLSolver.h"

using namespace SVF;


void HPocrCFL::solve()
{
    while (!primaryList.empty())
    {
        CFLItem item = primaryList.pop();
        procPrimaryItem(item);
    }

    while (!isWorklistEmpty())
    {
        CFLItem item = popFromWorklist();
        processCFLItem(item);
    }
}


bool HPocrCFL::pushIntoWorklist(NodeID src, NodeID dst, Label ty, bool isPrimary)
{
    CFLItem item = CFLItem(src, dst, ty, isPrimary);
    if (isPrimary && grammar()->isTransitive(ty.first))
    {
        primaryList.push(item);
        reanalyze = true;
    }

    return CFLBase::pushIntoWorklist(item);
}
