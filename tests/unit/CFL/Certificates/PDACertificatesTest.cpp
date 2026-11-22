#include "CFL/Certificates/PDACertificates.h"
#include <gtest/gtest.h>
#include <cassert>
#include <iostream>

namespace {
using namespace cflcert::pda;

static int runImportedTest() {
  // Empty target language: one control state, one stack symbol A, no
  // automaton transitions to final state. PDA can pop A, but target set empty.
  PDAInstance p;
  p.controlStates=1; p.stackAlphabet=1;
  p.initialState=0; p.initialStackSymbol=0;
  p.target.states=2; p.target.finals={1}; p.target.edges.resize(2);
  p.transitions={{0,0,0,{}}};
  auto c=buildNegativeCertificate(p);
  assert(verifyNegativeDeterministic(p,c));
  assert(verifyNegativeRandomized(p,c,12345));

  // Positive reachability: target automaton accepts A from q0 to final.
  p.target.edges[0].push_back({0,1});
  auto bad=buildNegativeCertificate(p);
  assert(!verifyNegativeDeterministic(p,bad));

  // Regression: the PDA can pop its initial symbol and empty the stack
  // exactly at a control state that is already a final P-automaton state.
  // The positive triplet certificate must accept the original control-state
  // endpoint, not only the phase-copied automaton endpoint.
  PDAInstance emptyStackFinal;
  emptyStackFinal.controlStates=2; emptyStackFinal.stackAlphabet=1;
  emptyStackFinal.initialState=0; emptyStackFinal.initialStackSymbol=0;
  emptyStackFinal.target.states=2; emptyStackFinal.target.finals={1};
  emptyStackFinal.target.edges.resize(2);
  emptyStackFinal.transitions={{0,0,1,{}}};
  PositiveCertificate emptyStackCertificate;
  assert(buildPositiveCertificate(emptyStackFinal,emptyStackCertificate));
  assert(verifyPositive(emptyStackFinal,emptyStackCertificate));

  // Push-two rule exercises M_{A,B,C}^1 and ^2.
  PDAInstance q;
  q.controlStates=2; q.stackAlphabet=2; q.initialState=0; q.initialStackSymbol=0;
  q.target.states=3; q.target.finals={2}; q.target.edges.resize(3);
  q.transitions={{0,0,1,{0,1}},{1,0,1,{}},{1,1,1,{}}};
  auto cq=buildNegativeCertificate(q);
  assert(verifyNegativeDeterministic(q,cq));
  assert(verifyNegativeRandomized(q,cq,99));
  auto mutated=cq;
  mutated.M3_2[0][0] ^= 1;
  assert(!verifyNegativeDeterministic(q,mutated));
  assert(!verifyNegativeRandomized(q,mutated,1234));

  // Positive triplet-CFG certificate for a two-symbol push. The target
  // P-automaton has the path q1 -B-> q2 -C-> q3(final).
  PDAInstance pos;
  pos.controlStates=2; pos.stackAlphabet=3;
  pos.initialState=0; pos.initialStackSymbol=0;
  pos.target.states=4; pos.target.finals={3}; pos.target.edges.resize(4);
  pos.target.edges[1].push_back({1,2}); // B
  pos.target.edges[2].push_back({2,3}); // C
  pos.transitions={{0,0,1,{1,2}}};
  PositiveCertificate positive;
  assert(buildPositiveCertificate(pos,positive));
  assert(verifyPositive(pos,positive));
  // Mutation: corrupt the root production's transition reference. The
  // independent verifier must reject the altered straight-line certificate.
  auto corrupted=positive;
  auto normalizedPos=normalizePAutomaton(pos);
  size_t root=tripletId(pos.initialStackSymbol,pos.initialState,positive.acceptingFinal,pos.stackAlphabet,pos.controlStates+normalizedPos.target.states);
  corrupted.rules[corrupted.ruleOf[root]].transition=999999;
  assert(!verifyPositive(pos,corrupted));

  // Differential randomized tiny instances: positive triplet derivability and
  // the negative invariant are complementary.
  unsigned seed=17;
  for(int z=0;z<120;++z){
    PDAInstance x; x.controlStates=2; x.stackAlphabet=2; x.initialState=0; x.initialStackSymbol=0;
    x.target.states=3; x.target.finals={2}; x.target.edges.resize(3);
    for(int u=0;u<3;++u)for(int a=0;a<2;++a)for(int v=0;v<3;++v){seed=seed*1664525u+1013904223u;if(seed>>31)x.target.edges[u].push_back({a,v});}
    for(int p0=0;p0<2;++p0)for(int a=0;a<2;++a)for(int zt=0;zt<2;++zt){seed=seed*1664525u+1013904223u;if((seed>>29)%3==0){std::vector<int>w;int len=(seed>>25)%3;for(int j=0;j<len;++j)w.push_back((seed>>(j+4))&1);x.transitions.push_back({p0,a,zt,w});}}
    PositiveCertificate yes;bool reachable=buildPositiveCertificate(x,yes);
    auto no=buildNegativeCertificate(x);bool certifiedNo=verifyNegativeDeterministic(x,no);
    assert(reachable!=certifiedNo);if(reachable)assert(verifyPositive(x,yes));
  }
  std::cout << "PDA certificate tests passed\n";
    return 0;
}

} // namespace

TEST(CFLCertificates, PDACertificates) {
    EXPECT_EQ(runImportedTest(), 0);
}
