#include <gtest/gtest.h>
#include "CFL/Certificates/HardestTwoNPDA.h"
#include <cassert>
#include <iostream>
#include <random>
#include <sstream>
#include <cstring>

namespace {
using namespace cflcert;
static int runImportedTest() {
  auto T=hardest::makeTransitionTable(); T.validate();
  assert(T.rules.size()>900);
  for(int i=0;i<static_cast<int>(hardest::State::Count);++i)
    assert(std::strcmp(hardest::stateName(static_cast<hardest::State>(i)),"<invalid-state>")!=0);
  std::ostringstream tableDump; hardest::writeTransitionTable(T,tableDump);
  assert(!tableDump.str().empty());
  auto checkPositive=[&](const std::string& w){
    assert(hardest::recognize(w)==twopda::Decision::Accept);
    assert(twopda::recognizeHardestLanguage(w)==twopda::Decision::Accept);
    assert(hardest::run(T,w,200000)==hardest::RunResult::Accept);
  };
  // Check the triangular-wave offset formula against literal reflected steps.
  for(int n=1;n<=8;++n) for(int u=1;u<=n;++u) for(int d=-50;d<=50;++d){
    int pos=u, dir=(d<0?-1:1);
    for(int k=0;k<std::abs(d);++k){
      pos+=dir;
      if(pos==0) dir=+1;
      else if(pos==n+1) dir=-1;
    }
    auto formula=hardest::reflectedRealVertex(u,d,n);
    if(pos==0||pos==n+1) assert(!formula);
    else assert(formula && *formula==pos);
  }
  checkPositive("#");                         // empty path, one vertex
  checkPositive("#(1#)");                    // positive offset + matched pair
  checkPositive("#[1#]");                    // second bracket type
  // Negative offset and reflection at the right endmarker:
  // 1 --( +4--> 3 --) -4--> 1 --( +4--> 3 --) 0--> 3.
  twopda::EncodedGraph reflected(3);
  reflected[0].push_back({'(',4});
  reflected[0].push_back({'(',4});
  reflected[2].push_back({')',-4});
  reflected[2].push_back({')',0});
  checkPositive(twopda::encodeGraph(reflected));
  // An offset ending exactly at a virtual endmarker is not a graph edge.
  assert(hardest::recognize("#(1") == twopda::Decision::Accept); // empty path at sole vertex
  assert(hardest::run(T,"#(1")==hardest::RunResult::Accept);
  // Invalid alphabet and malformed signed unary offsets are rejected.
  assert(hardest::run(T,"#x")==hardest::RunResult::InvalidInput);
  assert(hardest::run(T,"#(-#")==hardest::RunResult::Reject);
  assert(hardest::run(T,"#(*#")==hardest::RunResult::Reject);
  assert(hardest::run(T,"#(1**)#")==hardest::RunResult::Reject);
  // No edge between two empty vertices; exact virtual endpoints are not graph vertices.
  assert(hardest::recognize("##")==twopda::Decision::Reject);
  assert(hardest::run(T,"##")==hardest::RunResult::Reject);
  assert(hardest::recognize("#(-1#")==twopda::Decision::Reject);
  assert(hardest::run(T,"#(-1#")==hardest::RunResult::Reject);
  assert(hardest::recognize("##(1")==twopda::Decision::Reject);
  assert(hardest::run(T,"##(1")==hardest::RunResult::Reject);
  // A no-instance can have an infinite opening-loop branch. Bounded 2NPDA
  // simulation correctly reports Inconclusive rather than claiming rejection.
  assert(hardest::recognize("#(##")==twopda::Decision::Reject);
  assert(hardest::run(T,"#(##",1000)==hardest::RunResult::Inconclusive);

  // The explicit transition table and semantic graph interpretation agree
  // on generated positive paths, including both bracket types and reflected
  // movement. Each instance has a short accepting branch, avoiding the
  // inherently unbounded negative-run issue of nondeterministic PDAs.
  std::mt19937 rng(7);
  for(int it=0;it<40;++it){
    twopda::EncodedGraph x(3);
    char op=(rng()%2)?'(': '['; char cl=(op=='('?')':']');
    int jump=(it%2)?2:4; // +2 or +4 from vertex 1 reaches vertex 3 for n=3
    x[0].push_back({op,jump});
    x[2].push_back({cl,0});
    std::string z=twopda::encodeGraph(x);
    assert(hardest::recognize(z)==twopda::Decision::Accept);
    assert(twopda::recognizeHardestLanguage(z)==twopda::Decision::Accept);
    assert(hardest::run(T,z,50000)==hardest::RunResult::Accept);
  }

  // Audit the actual Theorem 21 homomorphism against the independent chain
  // Lemma 26 -> Lemma 27 -> restricted Dyck-2 reachability.
  twopda::Machine A; A.states=2; A.alphabet=3; A.stackAlphabet=2;
  A.initial=0; A.bottom=0; A.leftMarker=0; A.rightMarker=2; A.finals={1};
  A.transitions={{0,0,0,0,{0},1},{0,1,0,0,{0},1},{0,2,0,1,{0},0}};
  auto audit=hardest::auditTheorem21(A,{1,1});
  assert(audit.syntaxValid && audit.allEncodedEdgesHaveRealEndpoints);
  assert(audit.concatenationHomomorphism && audit.composedReachabilityAgrees);
  assert(audit.hardestMachineAccepts);
  twopda::Theorem21Homomorphism hom(A);
  assert(hardest::run(hom.apply({1,1}),1000000)==hardest::RunResult::Accept);

  std::cout << "explicit hardest 2NPDA table tests passed; states="
            << static_cast<int>(hardest::State::Count)
            << " rules=" << T.rules.size() << "\n";
    return 0;
}

} // namespace

TEST(CFLCertificates, HardestTwoNPDA) {
    EXPECT_EQ(runImportedTest(), 0);
}
