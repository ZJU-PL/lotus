#include "CFL/Certificates/TwoNPDA.h"
#include <gtest/gtest.h>
#include <cassert>
#include <iostream>

namespace {
using namespace cflcert;
static int runImportedTest() {
  // A two-way machine accepting every nonempty word over symbol 1.
  twopda::Machine A;A.states=2;A.alphabet=3;A.stackAlphabet=2;A.initial=0;A.bottom=0;A.leftMarker=0;A.rightMarker=2;A.finals={1};
  A.transitions={{0,0,0,0,{0},1},{0,1,0,0,{0},1},{0,2,0,1,{0},0}};
  twopda::Theorem21Homomorphism h(A);
  for(int n=1;n<=4;++n){std::vector<int>w(n,1);auto P=twopda::compileInputToPDAEmptiness(A,w);auto G=pda::reducePDAEmptinessToDyck2(P);bool expected=decideCubic(G);auto encoded=h.apply(w);assert(twopda::recognizeHardestLanguage(encoded)==(expected?twopda::Decision::Accept:twopda::Decision::Reject));assert(twopda::simulateHardest2NPDA(encoded,100000)==(expected?twopda::Simulation::Accept:twopda::Simulation::Reject));}
  // Machine with no final states: every homomorphic image is rejected.
  twopda::Machine R;R.states=1;R.alphabet=3;R.stackAlphabet=1;R.initial=0;R.bottom=0;R.leftMarker=0;R.rightMarker=2;
  twopda::Theorem21Homomorphism hr(R);auto encoded=hr.apply({1,1});assert(twopda::recognizeHardestLanguage(encoded)==twopda::Decision::Reject);
  // Lemma 27 with a multi-bit stack code and a two-symbol push.
  pda::EmptinessPDA stackP;stackP.states=5;stackP.stackAlphabet=3;stackP.initialState=0;stackP.initialSymbol=0;stackP.acceptingStates={4};
  stackP.transitions={{0,0,1,{1,2,0}},{1,1,2,{}},{2,2,3,{}},{3,0,4,{0}}};
  auto stackG=pda::reducePDAEmptinessToDyck2(stackP);assert(decideCubic(stackG));
  // Lemma 26 compilation agrees with PDA->D2 on a hand-built accepting run.
  twopda::Machine B;B.states=2;B.alphabet=3;B.stackAlphabet=1;B.initial=0;B.bottom=0;B.leftMarker=0;B.rightMarker=2;B.finals={1};
  B.transitions={{0,0,0,0,{0},1},{0,1,0,1,{0},1},{1,2,0,1,{0},0}};
  auto P=twopda::compileInputToPDAEmptiness(B,{1});auto G=pda::reducePDAEmptinessToDyck2(P);assert(decideCubic(G));

  // Lemma 28 arbitrary-graph encoding/recognition (binary vertex IDs).
  Instance arbitrary;arbitrary.n=5;arbitrary.source=0;arbitrary.target=4;
  arbitrary.edges={{0,1,Label::Open1},{1,2,Label::Open2},{2,3,Label::Close2},{3,4,Label::Close1}};
  auto encodedGeneral=twopda::encodeGeneralD2Instance(arbitrary);
  assert(twopda::recognizeGeneralD2Language(encodedGeneral)==twopda::Decision::Accept);
  Instance decoded;assert(twopda::parseGeneralD2Instance(encodedGeneral,decoded));assert(decideCubic(decoded));
  // Non-contiguous binary IDs are compressed without changing reachability.
  std::string sparseIds="1010;1111101000;1010,1011,(;1011,1111101000,);";
  assert(twopda::recognizeGeneralD2Language(sparseIds)==twopda::Decision::Accept);
  assert(twopda::recognizeGeneralD2Language("01;1;")==twopda::Decision::Invalid);

  // Restricted hardest-language parser and its bounded macro-simulator.
  twopda::EncodedGraph restricted={{{'(',1}},{{')',0}}};
  std::string restrictedWord=twopda::encodeGraph(restricted);
  assert(twopda::recognizeHardestLanguage(restrictedWord)==twopda::Decision::Accept);
  assert(twopda::simulateHardest2NPDA(restrictedWord)==twopda::Simulation::Accept);
  assert(twopda::recognizeHardestLanguage("#x") == twopda::Decision::Invalid);

  // The arithmetic reflection helper must agree with literal one-cell
  // movement, including exact hits on either virtual endmarker.
  for(int n=1;n<=9;++n)for(int src=1;src<=n;++src)for(int off=-40;off<=40;++off){
    int pos=src,dir=off<0?-1:1;for(int k=0;k<std::abs(off);++k){pos+=dir;if(pos==0||pos==n+1)dir=-dir;}
    assert(twopda::reflectedDestination(src,off,n)==pos);
  }
  // Independent bounded operational semantics for a 2NPDA. Stack words are
  // stored bottom-to-top, with the rightmost symbol at the top; transition
  // replacement words themselves are top-first.
  auto direct2npda = [](const twopda::Machine& M,const std::vector<int>& input)->int {
    struct C { int q,pos; std::vector<int> st; };
    std::vector<int> tape{M.leftMarker};tape.insert(tape.end(),input.begin(),input.end());tape.push_back(M.rightMarker);
    std::queue<C> todo;todo.push({M.initial,0,{M.bottom}});std::set<std::string> seen;
    auto key=[](const C&c){std::string z=std::to_string(c.q)+":"+std::to_string(c.pos)+":";for(int a:c.st)z+=std::to_string(a)+",";return z;};
    size_t steps=0;
    while(!todo.empty()&&++steps<100000){C c=std::move(todo.front());todo.pop();if(std::find(M.finals.begin(),M.finals.end(),c.q)!=M.finals.end()&&c.pos==(int)tape.size()-1&&c.st==std::vector<int>{M.bottom})return 1;
      for(const auto&t:M.transitions)if(t.from==c.q&&t.read==tape[c.pos]&&!c.st.empty()&&t.pop==c.st.back()){int np=c.pos+t.move;if(np<0||np>=(int)tape.size())continue;C d{t.to,np,c.st};d.st.pop_back();d.st.insert(d.st.end(),t.push.rbegin(),t.push.rend());std::string k=key(d);if(seen.insert(k).second)todo.push(std::move(d));}
    }return todo.empty()?0:-1;
  };
  twopda::Machine PushPop;PushPop.states=4;PushPop.alphabet=3;PushPop.stackAlphabet=2;PushPop.initial=0;PushPop.bottom=0;PushPop.leftMarker=0;PushPop.rightMarker=2;PushPop.finals={3};
  PushPop.transitions={{0,0,0,1,{0},1},{1,1,0,2,{1,0},1},{2,2,1,3,{},0}};
  auto direct=direct2npda(PushPop,{1});assert(direct==1);
  auto product=twopda::compileInputToPDAEmptiness(PushPop,{1});auto reduced=pda::reducePDAEmptinessToDyck2(product);assert(decideCubic(reduced));
  twopda::Theorem21Homomorphism hp(PushPop);assert(twopda::recognizeHardestLanguage(hp.apply({1}))==twopda::Decision::Accept);
  assert(direct2npda(PushPop,{})==0);
  auto emptyProduct=twopda::compileInputToPDAEmptiness(PushPop,{});
  assert(!decideCubic(pda::reducePDAEmptinessToDyck2(emptyProduct)));

  // Exercise a genuine left move and a stationary move in the input-position
  // product. This catches the common off-by-one errors at both endmarkers.
  twopda::Machine TwoWay;TwoWay.states=7;TwoWay.alphabet=3;TwoWay.stackAlphabet=1;TwoWay.initial=0;TwoWay.bottom=0;TwoWay.leftMarker=0;TwoWay.rightMarker=2;TwoWay.finals={6};
  TwoWay.transitions={{0,0,0,1,{0},1},{1,1,0,2,{0},1},{2,1,0,3,{0},-1},{3,1,0,4,{0},1},{4,1,0,5,{0},1},{5,2,0,6,{0},0}};
  assert(direct2npda(TwoWay,{1,1})==1);
  auto twoWayP=twopda::compileInputToPDAEmptiness(TwoWay,{1,1});
  assert(decideCubic(pda::reducePDAEmptinessToDyck2(twoWayP)));
  twopda::Theorem21Homomorphism twoWayH(TwoWay);
  assert(twopda::recognizeHardestLanguage(twoWayH.apply({1,1}))==twopda::Decision::Accept);

  // A non-palindromic two-symbol push catches accidental reversal errors in
  // the PDA->Dyck encoding. The replacement is bottom-to-top {0,1,2} (top 2).
  pda::EmptinessPDA stackOrder;stackOrder.states=5;stackOrder.stackAlphabet=3;stackOrder.initialState=0;stackOrder.initialSymbol=0;stackOrder.acceptingStates={4};
  stackOrder.transitions={{0,0,1,{1,2,0}},{1,1,2,{}},{2,2,3,{}},{3,0,4,{0}}};
  auto topGraph=pda::reducePDAEmptinessToDyck2(stackOrder);assert(decideCubic(topGraph));

  auto directPDA = [](const pda::EmptinessPDA& P)->bool {
    struct C { int q; std::vector<int> st; };
    std::queue<C> todo;todo.push({P.initialState,{P.initialSymbol}});std::set<std::string> seen;
    auto key=[](const C&c){std::string z=std::to_string(c.q)+":";for(int a:c.st)z+=std::to_string(a)+",";return z;};
    size_t steps=0;
    while(!todo.empty()&&++steps<100000){C c=std::move(todo.front());todo.pop();
      if(std::find(P.acceptingStates.begin(),P.acceptingStates.end(),c.q)!=P.acceptingStates.end()&&c.st==std::vector<int>{P.initialSymbol})return true;
      if(c.st.empty())continue;
      for(const auto&t:P.transitions)if(t.fromState==c.q&&t.popSymbol==c.st.back()){C d{t.toState,c.st};d.st.pop_back();d.st.insert(d.st.end(),t.pushWord.rbegin(),t.pushWord.rend());auto k=key(d);if(seen.insert(k).second)todo.push(std::move(d));}
    }return false;
  };
  assert(directPDA(stackOrder));assert(decideCubic(topGraph));
  auto wrongOrder=stackOrder;wrongOrder.transitions[1].popSymbol=2;wrongOrder.transitions[2].popSymbol=1;
  assert(!directPDA(wrongOrder));assert(!decideCubic(pda::reducePDAEmptinessToDyck2(wrongOrder)));

  std::cout<<"2NPDA and Theorem 21 tests passed\n";
    return 0;
}

} // namespace

TEST(CFLCertificates, TwoNPDA) {
    EXPECT_EQ(runImportedTest(), 0);
}
