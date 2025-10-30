//
// Created by kisslune on 7/5/22.
//



#include <iomanip>
#include "CFL/POCR/CFLSolver/CFLStat.h"
#include "CFL/POCR/CFLSolver/CFLSolver.h"

using namespace SVF;

void CFLStat::printStat(std::string statname)
{
    std::cout.flags(std::ios::left);
    unsigned field_width = 20;
    for (NUMStatMap::iterator it = generalNumMap.begin(), eit = generalNumMap.end(); it != eit; ++it)
    {
        // format out put with width 20 space
        std::cout << std::setw(field_width) << it->first << it->second << "\n";
    }
    for (TIMEStatMap::iterator it = timeStatMap.begin(), eit = timeStatMap.end(); it != eit; ++it)
    {
        // format out put with width 20 space
        std::cout << std::setw(field_width) << it->first << it->second << "\n";
    }
    for (NUMStatMap::iterator it = PTNumStatMap.begin(), eit = PTNumStatMap.end(); it != eit; ++it)
    {
        // format out put with width 20 space
        std::cout << std::setw(field_width) << it->first << it->second << "\n";
    }

    std::cout.flush();
    generalNumMap.clear();
    PTNumStatMap.clear();
    timeStatMap.clear();
}


void CFLStat::graphStat()
{
    CFLGraph* g = cfl->graph();

    for (auto nodeIt = g->begin(); nodeIt != g->end(); nodeIt++)
        numOfNodes++;

    for (auto it : g->getCFLEdges())
        numOfEdges++;

    PTNumStatMap["#Nodes"] = numOfNodes;
    PTNumStatMap["#Edges"] = numOfEdges;

    CFLStat::printStat("CFLGraph Stats");
}


void CFLStat::performStat()
{
    endClk();

    graphStat();
    cfl->countSumEdges();

    timeStatMap["AnalysisTime"] = timeOfSolving;
    timeStatMap["VmrssInGB"] = (_vmrssUsageAfter - _vmrssUsageBefore) / 1024.0 / 1024.0;
    PTNumStatMap["#Checks"] = checks;
    PTNumStatMap["#SumEdges"] = numOfSumEdges - numOfEdges;
    PTNumStatMap["#CountEdges"] = numOfCountEdges;

    CFLStat::printStat("CFL-reachability analysis Stats");

    if (!CFLOpt::sPairsFName().empty())
        writeSPairsIntoFile(CFLOpt::sPairsFName());
}


void CFLStat::setMemUsageBefore()
{
    u32_t vmrss, vmsize;
    SVFUtil::getMemoryUsageKB(&vmrss, &vmsize);
    _vmrssUsageBefore = vmrss;
    _vmsizeUsageBefore = vmsize;
}


void CFLStat::setMemUsageAfter()
{
    u32_t vmrss, vmsize;
    SVFUtil::getMemoryUsageKB(&vmrss, &vmsize);
    _vmrssUsageAfter = vmrss;
    _vmsizeUsageAfter = vmsize;
}


void CFLStat::writeSPairsIntoFile(std::string fName)
{
    std::ofstream outFile(fName, std::ios::out);
    if (!outFile)
    {
        std::cout << "error opening file!";
        return;
    }

    for (auto& it1 : sEdgeSet)
    {
        for (auto it2 : it1.second)
            outFile << it1.first << '\t' << it2 << std::endl;
    }

    outFile.close();
}