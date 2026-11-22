#include "CFL/Certificates/CFGCertificates.h"
#include <gtest/gtest.h>
#include <cassert>
#include <iostream>

namespace {
using namespace cflcert::cfg;
static Grammar parenGrammar(){
  Grammar g;g.nonterminals=4;g.terminals=2;g.start=0;
  g.productions={{Production::Binary,0,1,2,-1},
                 {Production::Epsilon,0,-1,-1,-1},
                 {Production::Terminal,1,-1,-1,0},
                 {Production::Binary,2,0,3,-1},
                 {Production::Terminal,3,-1,-1,1}};
  return g;
}
static int runImportedTest() {auto gr=parenGrammar();
  Graph yes;yes.n=3;yes.source=0;yes.target=2;yes.edges={{0,1,0},{1,2,1}};
  PositiveCertificate p;assert(buildPositive(gr,yes,p));assert(verifyPositive(gr,yes,p));
  NegativeCertificate bad;buildNegative(gr,yes,bad);assert(!verifyNegative(gr,yes,bad));
  Graph no;no.n=2;no.source=0;no.target=1;no.edges={{0,1,0}};
  assert(!buildPositive(gr,no,p));NegativeCertificate c;buildNegative(gr,no,c);assert(verifyNegative(gr,no,c));
  std::cout<<"generic CFG tests passed\n";
    return 0;
}


} // namespace

TEST(CFLCertificates, CFGCertificates) {
    EXPECT_EQ(runImportedTest(), 0);
}
