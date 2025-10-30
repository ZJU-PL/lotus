//===- AAStat.cpp -- Program Expression Graph-----------------------------//

/*
 * AAStat.cpp
 *
 *  Created on: Aug 1, 2020
 *      Author: Yuxiang Lei
 */

#include <iomanip>
#include "CFL/POCR/AA/AAStat.h"
#include "CFL/POCR/AA/AliasAnalysis.h"
"

using namespace SVF;


void AAStat::printStat(std::string statname)
{
    std::cout.flags(std::ios::left);
    unsigned field_width = 20;
    for (TIMEStatMap::iterator it = timeStatMap.begin(), eit = timeStatMap.end(); it != eit; ++it)
    {
        // format out put with width 20 space
        std::cout << it->first << "\t" << it->second << "\n";
    }
    for (NUMStatMap::iterator it = PTNumStatMap.begin(), eit = PTNumStatMap.end(); it != eit; ++it)
    {
        // format out put with width 20 space
        std::cout << it->first << "\t" << it->second << "\n";
    }

    std::cout.flush();
    PTNumStatMap.clear();
    timeStatMap.clear();
}


void AAStat::pegStat()
{
    PEG* peg = aa->graph();

    for (auto nodeIt = peg->begin(); nodeIt != peg->end(); nodeIt++)
        numOfNodes++;

    for (auto it: peg->getPEGEdges())
        numOfEdges += 2;

    PTNumStatMap["#Nodes"] = numOfNodes;
    PTNumStatMap["#Edges"] = numOfEdges;
    timeStatMap["GraphSimpTime"] = gsTime;

    AAStat::printStat("PEG Stats");
}


void AAStat::performStat()
{
    endClk();

    if (CFLOpt::graphStat())
        pegStat();

    if (!CFLOpt::PStat())
        return;

    aa->countSumEdges();

    timeStatMap["AnalysisTime"] = timeOfSolving;
    timeStatMap["VmrssInGB"] = (_vmrssUsageAfter - _vmrssUsageBefore) / 1024.0 / 1024.0;
    PTNumStatMap["#Checks"] = checks;
    PTNumStatMap["#SumEdges"] = numOfSumEdges - numOfEdges;
    PTNumStatMap["#SEdges"] = numOfSEdges;

    printStat("CFL-reachability analysis Stats");
}


void AAStat::setMemUsageBefore()
{
    u32_t vmrss, vmsize;
    SVFUtil::getMemoryUsageKB(&vmrss, &vmsize);
    _vmrssUsageBefore = vmrss;
    _vmsizeUsageBefore = vmsize;
}


void AAStat::setMemUsageAfter()
{
    u32_t vmrss, vmsize;
    SVFUtil::getMemoryUsageKB(&vmrss, &vmsize);
    _vmrssUsageAfter = vmrss;
    _vmsizeUsageAfter = vmsize;
}