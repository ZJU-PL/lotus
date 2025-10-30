/*
 // Valueflow analysis
 //
 // Author: Kisslune
 */

#include "SVF-LLVM/LLVMUtil.h"
#include "CFL/POCR/VFA/VFAnalysis.h"

using namespace SVF;

static Option<bool> Default_VFA("std", "Standard valueflow analysis", false);
static Option<bool> Pocr_VFA("pocr", "POCR valueflow analysis", false);
static Option<bool> Gspan_VFA("gspan", "Graspan valueflow analysis", false);
static Option<bool> Gr_VFA("gr", "Grammar rewritting valueflow analysis", false);
static Option<bool> GrGspan_VFA("grgspan", "Grammar rewritting Graspan valueflow analysis", false);
static Option<bool> Focr_VFA("focr", "Transitive-reduction valueflow analysis", false);


int main(int argc, char** argv)
{
    int arg_num = 0;
    char** arg_vec = new char* [argc];
    std::vector<std::string> moduleNameVec;
    std::vector<std::string> inFileVec;
    processArgs(argc, argv, arg_num, arg_vec, inFileVec);
    OptionBase::parseOptions(arg_num, arg_vec, "Valueflow analysis\n", "[options] <input>");

    VFAnalysis* vfa;
    if (Default_VFA())
    {
        vfa = new StdVFA(inFileVec[0]);
        vfa->analyze();
    }
    else if (Pocr_VFA())
    {
        vfa = new PocrVFA(inFileVec[0]);
        vfa->analyze();
    }
    else if (Gspan_VFA())
    {
        vfa = new GspanVFA(inFileVec[0]);
        vfa->analyze();
    }
    else if (Gr_VFA())
    {
        vfa = new GRVFA(inFileVec[0]);
        vfa->analyze();
    }
    else if (GrGspan_VFA())
    {
        vfa = new GRGspanVFA(inFileVec[0]);
        vfa->analyze();
    }
    else if (Focr_VFA())
    {
        vfa = new FocrVFA(inFileVec[0]);
        vfa->analyze();
    }
    else    // pocr is the default solver
    {
        vfa = new PocrVFA(inFileVec[0]);
        vfa->analyze();
    }

    return 0;
}