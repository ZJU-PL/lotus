#include "CFL/Certificates/CFGCertificates.h"
#include <gtest/gtest.h>
#include <cassert>
#include <iostream>
#include <unordered_set>

namespace {
using namespace cflcert::cfg;

static bool acceptsNormalized(const Grammar& gr,const std::vector<int>& word) {
  Graph g; g.n=(int)word.size()+1; g.source=0; g.target=(int)word.size();
  for(int i=0;i<(int)word.size();++i) g.edges.push_back({i,i+1,word[i]});
  PositiveCertificate c; return buildPositive(gr,g,c) && verifyPositive(gr,g,c);
}
static bool acceptsOriginalBounded(const GeneralGrammar& gr,const std::vector<int>& word) {
  struct S { bool terminal; int id; };
  auto key=[](const std::vector<S>& v){std::string k;for(auto x:v){k+=x.terminal?'t':'n';k+=std::to_string(x.id);k+=',';}return k;};
  std::vector<S> start{{false,gr.start}}; std::vector<std::vector<S>> q{start}; std::unordered_set<std::string> seen{key(start)};
  for(size_t at=0;at<q.size();++at){const auto cur=q[at];
    bool all=true;std::vector<int> w;for(auto x:cur){if(!x.terminal){all=false;break;}w.push_back(x.id);}if(all&&w==word)return true;
    for(size_t i=0;i<cur.size();++i)if(!cur[i].terminal){
      for(const auto&p:gr.productions)if(p.lhs==cur[i].id){std::vector<S> nxt; nxt.insert(nxt.end(),cur.begin(),cur.begin()+i);for(auto x:p.rhs)nxt.push_back({x.terminal,x.id});nxt.insert(nxt.end(),cur.begin()+i+1,cur.end());
        size_t terminals=0;for(auto x:nxt)terminals+=x.terminal;if(terminals>word.size()||nxt.size()>word.size()+5)continue;
        auto k=key(nxt);if(seen.insert(k).second)q.push_back(std::move(nxt));
      }
    }
  }return false;
}
static int runImportedTest() {
  GeneralGrammar g;g.nonterminals=4;g.terminals=2;g.start=0;
  g.productions={
    {0,{Symbol::N(1),Symbol::N(2),Symbol::T(0)}}, {0,{}},
    {1,{}}, {1,{Symbol::T(0)}}, {1,{Symbol::N(3)}},
    {2,{Symbol::T(1)}}, {3,{Symbol::T(0)}}
  };
  Grammar normalized=normalize(g);normalized.validate();
  for(int len=0;len<=3;++len){int count=1<<len;for(int mask=0;mask<count;++mask){std::vector<int>w(len);for(int i=0;i<len;++i)w[i]=(mask>>i)&1;
    bool a=acceptsOriginalBounded(g,w),b=acceptsNormalized(normalized,w);assert(a==b);
    Graph path;path.n=len+1;path.source=0;path.target=len;for(int i=0;i<len;++i)path.edges.push_back({i,i+1,w[i]});
    NegativeCertificate neg;buildNegative(normalized,path,neg);assert(verifyNegative(normalized,path,neg)!=b);
    if(!b && !neg.productByProduction.empty() && !neg.productByProduction[0].empty()) {
      auto corrupted=neg;corrupted.productByProduction[0][0]^=1;assert(!verifyNegative(normalized,path,corrupted));
    }
  }}
  // Long RHS with many nullable occurrences must not cause an exponential
  // nullable-expansion blow-up: binarization happens before epsilon removal.
  GeneralGrammar longG;longG.nonterminals=2;longG.terminals=2;longG.start=0;
  GeneralProduction longRule;longRule.lhs=0;for(int i=0;i<10;++i)longRule.rhs.push_back(Symbol::N(1));longRule.rhs.push_back(Symbol::T(0));
  longG.productions={longRule,{1,{}},{1,{Symbol::T(1)}}};
  Grammar longN=normalize(longG);assert(longN.nonterminals<30);
  for(int k=0;k<=10;++k){std::vector<int>w(k,1);w.push_back(0);assert(acceptsNormalized(longN,w));}
  assert(!acceptsNormalized(longN,{0,1}));

  // A positive certificate with a production whose lhs is not its variable
  // must be rejected (regression for a verifier omission).
  Grammar gr;gr.nonterminals=2;gr.terminals=1;gr.start=0;
  gr.productions={{Production::Terminal,0,-1,-1,0},{Production::Terminal,1,-1,-1,0}};
  Graph path;path.n=2;path.source=0;path.target=1;path.edges={{0,1,0}};
  PositiveCertificate cert;assert(buildPositive(gr,path,cert));
  size_t root=id(0,0,1,path.n);int ri=cert.ruleOf[root];cert.rules[ri].production=1;assert(!verifyPositive(gr,path,cert));
  std::cout<<"CFG normalization and generic certificate tests passed\n";
    return 0;
}

} // namespace

TEST(CFLCertificates, CFGNormalization) {
    EXPECT_EQ(runImportedTest(), 0);
}
