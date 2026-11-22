#include "CFL/Certificates/PDACertificates.h"
#include <gtest/gtest.h>
#include <cassert>
#include <iostream>
#include <random>
#include <set>

namespace {
using namespace cflcert::pda;

static bool targetAccepts(const PDAInstance& p,int q,const std::vector<int>& stack) {
    std::vector<int> current{q};
    for(auto it=stack.rbegin();it!=stack.rend();++it){int sym=*it;std::vector<int> next;std::vector<char> seen(p.target.states,0);
        for(int u:current)for(auto [a,v]:p.target.edges[u])if(a==sym&&!seen[v])seen[v]=1,next.push_back(v);
        current.swap(next);if(current.empty())return false;
    }
    for(int u:current)if(std::find(p.target.finals.begin(),p.target.finals.end(),u)!=p.target.finals.end())return true;
    return false;
}
static bool bruteReach(const PDAInstance& p) {
    struct C{int q;std::vector<int> stack;};std::queue<C> todo;todo.push({p.initialState,{p.initialStackSymbol}});
    std::set<std::string> seen;
    auto key=[](const C&c){std::string x=std::to_string(c.q)+":";for(int a:c.stack)x+=std::to_string(a)+",";return x;};seen.insert(key(todo.front()));
    while(!todo.empty()){C c=std::move(todo.front());todo.pop();if(targetAccepts(p,c.q,c.stack))return true;if(c.stack.empty())continue;
        for(const auto&t:p.transitions)if(t.fromState==c.q&&t.popSymbol==c.stack.back()){
            C d{t.toState,c.stack};d.stack.pop_back();d.stack.insert(d.stack.end(),t.pushWord.rbegin(),t.pushWord.rend());
            auto k=key(d);if(seen.insert(k).second)todo.push(std::move(d));
        }
    }return false;
}
static int runImportedTest() {
    std::mt19937 rng(2026);
    for(int z=0;z<120;++z){PDAInstance p;p.controlStates=3;p.stackAlphabet=2;p.initialState=0;p.initialStackSymbol=rng()%2;
        p.target.states=4;p.target.finals={3};if(rng()%4==0)p.target.finals.push_back(rng()%3);p.target.edges.resize(4);
        for(int u=0;u<4;++u)for(int a=0;a<2;++a)if(rng()%3==0)p.target.edges[u].push_back({a,(int)(rng()%4)});
        // Control-state DAG guarantees the direct configuration search is finite.
        for(int q=0;q<2;++q)for(int a=0;a<2;++a){int len=rng()%3;std::vector<int>w;for(int j=0;j<len;++j)w.push_back(rng()%2);p.transitions.push_back({q,a,q+1,w});
            // Add stack-preserving cycles; the direct oracle remains finite
            // because it deduplicates configurations.
            if(rng()%2)p.transitions.push_back({q,a,q,{a}});
        }
        // Normalization must preserve the target automaton language from
        // every original control state for all short stack words.
        PDAInstance normalized=normalizePAutomaton(p);
        assert(normalizePAutomaton(normalized).targetIsNormalized);
        for(int q=0;q<p.controlStates;++q) for(int len=0;len<=3;++len) {
            int words=1<<len; for(int mask=0;mask<words;++mask) {
                std::vector<int> stack(len);for(int j=0;j<len;++j)stack[j]=(mask>>j)&1;
                assert(targetAccepts(p,q,stack)==targetAccepts(normalized,q,stack));
            }
        }
        bool expected=bruteReach(p);
        PositiveCertificate pos;bool got=buildPositiveCertificate(p,pos);if(got!=expected){std::cerr<<"Mismatch z="<<z<<" expected="<<expected<<" got="<<got<<" initial="<<p.initialStackSymbol<<"\n";for(auto&t:p.transitions){std::cerr<<t.fromState<<" pop "<<t.popSymbol<<" -> "<<t.toState<<" push";for(int x:t.pushWord)std::cerr<<" "<<x;std::cerr<<"\n";}for(int u=0;u<p.target.states;++u)for(auto [a,v]:p.target.edges[u])std::cerr<<"edge "<<u<<" -"<<a<<"-> "<<v<<"\n";return 2;}assert(verifyPositive(p,pos)==expected);
        NegativeCertificate neg=buildNegativeCertificate(p);bool negok=verifyNegativeDeterministic(p,neg);if(negok==expected){std::cerr<<"negative mismatch z="<<z<<" expected="<<expected<<" negok="<<negok<<"\n";for(auto&t:p.transitions){std::cerr<<t.fromState<<" pop "<<t.popSymbol<<" -> "<<t.toState<<" push";for(int x:t.pushWord)std::cerr<<" "<<x;std::cerr<<"\n";}for(int u=0;u<p.target.states;++u)for(auto [a,v]:p.target.edges[u])std::cerr<<"edge "<<u<<" -"<<a<<"-> "<<v<<"\n";return 3;}bool nr=verifyNegativeRandomized(p,neg,static_cast<uint64_t>(z+1));if(nr!=!expected){std::cerr<<"random mismatch z="<<z<<" expected="<<expected<<" det="<<negok<<" random="<<nr<<"\n";return 4;}
    }
    std::cout<<"PDA certificate differential tests passed on 120 acyclic pushdown systems\n";
    return 0;
}

} // namespace

TEST(CFLCertificates, PDAExhaustive) {
    EXPECT_EQ(runImportedTest(), 0);
}
