#include "CFL/Certificates/HardestTwoNPDA.h"
#include <gtest/gtest.h>
#include <cassert>
#include <iostream>
#include <random>

namespace {
using namespace cflcert;
static int runImportedTest() {
 std::mt19937 rng(19);
 for(int z=0;z<12;++z){
  twopda::Machine A;A.states=2+(z%2);A.alphabet=4;A.stackAlphabet=2+(z%3);A.initial=0;A.bottom=A.stackAlphabet-1;A.leftMarker=0;A.rightMarker=3;A.finals={A.states-1};
  // Bottom-preserving transitions may move the input head. Other stack
  // symbols and their multi-symbol replacements are included in the machine
  // description to exercise the binary code templates, though unreachable
  // from the designated bottom-only initial configuration unless pushed.
  for(int q=0;q<A.states;++q)for(int a=0;a<A.alphabet;++a)for(int top=0;top<A.stackAlphabet;++top)
   if(rng()%9==0){
    int d=(int)(rng()%3)-1;if(a==A.leftMarker&&d<0)continue;if(a==A.rightMarker&&d>0)continue;
    std::vector<int> push;
    if(top==A.bottom) {int len=rng()%3;for(int j=0;j<len;++j)push.push_back(rng()%(A.stackAlphabet-1));push.push_back(A.bottom);}
    else {int len=rng()%3;for(int j=0;j<len;++j){int x=rng()%(A.stackAlphabet-1);push.push_back(x);}}
    A.transitions.push_back({q,a,top,(int)(rng()%A.states),push,d});
   }
  twopda::Theorem21Homomorphism h(A);
  for(int len=1;len<=2;++len){
   std::vector<int>w(len);for(auto&x:w)x=1+(rng()%2);
   auto P=twopda::compileInputToPDAEmptiness(A,w);
   auto G=pda::reducePDAEmptinessToDyck2(P);
   bool ex=decideCubic(G);
   auto enc=h.apply(w);
   bool got=twopda::recognizeHardestLanguage(enc)==twopda::Decision::Accept;
   if(z<4 && len==1) {
     auto audit=hardest::auditTheorem21(A,w);
     assert(audit.syntaxValid && audit.allEncodedEdgesHaveRealEndpoints);
     assert(audit.concatenationHomomorphism && audit.composedReachabilityAgrees);
   }
   if(ex!=got){std::cerr<<"Mismatch z="<<z<<" len="<<len<<" gamma="<<A.stackAlphabet<<"\n";return 1;}
   // A symbol-wise homomorphism must preserve concatenation exactly.
   if(len==2){auto one=h.apply({w[0]});auto two=h.apply({w[1]});assert(enc==one+two);}
  }
 }
 std::cout<<"random Theorem 21 homomorphism tests passed\n";
    return 0;
}

} // namespace

TEST(CFLCertificates, Theorem21Random) {
    EXPECT_EQ(runImportedTest(), 0);
}
