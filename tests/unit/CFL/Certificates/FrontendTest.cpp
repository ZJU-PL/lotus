#include "CFL/Certificates/Frontend.h"
#include <gtest/gtest.h>
#include <cassert>
#include <iostream>
#include <queue>
#include <random>
#include <set>

namespace {

using namespace cflcert;

static twopda::Machine makeLongPushMachine(bool accepting) {
    twopda::Machine m;
    m.states=7; m.alphabet=3; m.stackAlphabet=3;
    m.initial=0; m.bottom=0; m.leftMarker=0; m.rightMarker=2;
    if (accepting) m.finals={6};
    m.transitions={
        {0,0,0,1,{0},+1},             // enter the input
        {1,1,0,2,{1,2,1,0},+1},       // arbitrary four-symbol push
        {2,2,1,3,{},0},               // pop top 1
        {3,2,2,4,{},0},               // pop 2
        {4,2,1,5,{},0},               // pop 1
        {5,2,0,6,{0},0}               // restore bottom and enter final
    };
    m.validate();
    return m;
}

static bool directFinite2NPDA(const twopda::Machine& m,
                              const std::vector<int>& word,
                              frontend::AcceptanceMode mode) {
    struct C { int q, pos; };
    std::vector<int> tape{m.leftMarker};
    tape.insert(tape.end(),word.begin(),word.end());
    tape.push_back(m.rightMarker);
    std::queue<C> todo; todo.push({m.initial,0});
    std::set<std::pair<int,int>> seen; seen.insert({m.initial,0});
    while (!todo.empty()) {
        C c=todo.front(); todo.pop();
        const bool final=std::find(m.finals.begin(),m.finals.end(),c.q)!=m.finals.end();
        if (final && (mode==frontend::AcceptanceMode::AnyPosition || c.pos==(int)tape.size()-1))
            return true;
        for (const auto& t:m.transitions) {
            if (t.from!=c.q || t.read!=tape[c.pos] || t.pop!=m.bottom ||
                t.push!=std::vector<int>{m.bottom}) continue;
            int np=c.pos+t.move;
            if (np<0 || np>=(int)tape.size()) continue;
            auto key=std::make_pair(t.to,np);
            if (seen.insert(key).second) todo.push({t.to,np});
        }
    }
    return false;
}

static int runImportedTest() {
    // General CFG: S -> A B C, A -> epsilon | a, B -> b, C -> c.
    cfg::GeneralGrammar G;
    G.nonterminals=4; G.terminals=3; G.start=0;
    G.productions={
        {0,{cfg::Symbol::N(1),cfg::Symbol::N(2),cfg::Symbol::N(3)}},
        {1,{}}, {1,{cfg::Symbol::T(0)}},
        {2,{cfg::Symbol::T(1)}},
        {3,{cfg::Symbol::T(2)}}
    };
    cfg::Graph graph; graph.n=3; graph.source=0; graph.target=2;
    graph.edges={{0,1,1},{1,2,2}}; // "bc" accepted via A => epsilon
    auto yes=frontend::solveCFG(G,graph);
    assert(yes.status==frontend::ReachabilityStatus::Reachable);
    assert(frontend::verifyCFGResult(G,graph,yes));
    cfg::Graph no=graph; no.edges[1].label=1; // "bb" not accepted
    auto noResult=frontend::solveCFG(G,no);
    assert(noResult.status==frontend::ReachabilityStatus::Unreachable);
    assert(frontend::verifyCFGResult(G,no,noResult));
    cfg::Graph empty; empty.n=1; empty.source=0; empty.target=0;
    cfg::GeneralGrammar eps; eps.nonterminals=1; eps.terminals=0; eps.start=0;
    eps.productions={{0,{}}};
    auto epsResult=frontend::solveCFG(eps,empty);
    assert(epsResult.status==frontend::ReachabilityStatus::Reachable);
    assert(frontend::verifyCFGResult(eps,empty,epsResult));

    // Arbitrary 2NPDA replacement words are reduced to length <= 2.
    auto M=makeLongPushMachine(true);
    auto compiled=frontend::compile2NPDA(M,{1});
    for (const auto& t:compiled.emptiness.transitions) assert(t.pushWord.size()<=2);
    assert(frontend::decide2NPDA(M,{1}));
    pda::PositiveCertificate pc;
    assert(frontend::buildPositive2NPDACertificate(M,{1},pc));
    assert(frontend::verifyPositive2NPDACertificate(M,{1},pc));

    // A non-accepting variant has a valid negative certificate.
    auto R=makeLongPushMachine(false);
    assert(!frontend::decide2NPDA(R,{1}));
    pda::NegativeCertificate nc;
    frontend::buildNegative2NPDACertificate(R,{1},nc);
    assert(frontend::verifyNegative2NPDACertificate(R,{1},nc));

    // Any-position final-state acceptance is separately supported and cleaned
    // to the permanent bottom symbol before invoking PDA emptiness.
    twopda::Machine any;
    any.states=3; any.alphabet=3; any.stackAlphabet=2;
    any.initial=0; any.bottom=0; any.leftMarker=0; any.rightMarker=2;
    any.finals={2};
    any.transitions={{0,0,0,1,{0},+1},{1,1,0,2,{1,0},0}};
    assert(!frontend::decide2NPDA(any,{1},frontend::AcceptanceMode::RightEndmarker));
    assert(frontend::decide2NPDA(any,{1},frontend::AcceptanceMode::AnyPosition));
    auto anyCompiled=frontend::compile2NPDA(any,{1},frontend::AcceptanceMode::AnyPosition);
    for (const auto& t:anyCompiled.emptiness.transitions) assert(t.pushWord.size()<=2);
    pda::PositiveCertificate anyCert;
    assert(pda::buildPositiveCertificate(anyCompiled.certificateInstance,anyCert));
    assert(pda::verifyPositive(anyCompiled.certificateInstance,anyCert));

    // Differential testing on generated finite-configuration 2NPDAs. The
    // one-symbol stack alphabet keeps the stack fixed at bottom, while the
    // input head, control state, and nondeterministic transition choices vary.
    std::mt19937 rng(0xCF12026u);
    for (int trial=0;trial<100;++trial) {
        twopda::Machine random;
        random.states=1+(rng()%4); random.alphabet=4; random.stackAlphabet=1;
        random.initial=static_cast<int>(rng()%random.states);
        random.bottom=0; random.leftMarker=0; random.rightMarker=3;
        for (int q=0;q<random.states;++q) if (rng()%3==0) random.finals.push_back(q);
        const int count=static_cast<int>(rng()%16);
        for (int j=0;j<count;++j)
            random.transitions.push_back({static_cast<int>(rng()%random.states),
                static_cast<int>(rng()%4),0,static_cast<int>(rng()%random.states),
                {0},static_cast<int>(rng()%3)-1});
        random.validate();
        std::vector<int> input(static_cast<size_t>(rng()%4));
        for (int& a:input) a=1+static_cast<int>(rng()%2);
        for (auto mode:{frontend::AcceptanceMode::RightEndmarker,
                        frontend::AcceptanceMode::AnyPosition}) {
            const bool reference=directFinite2NPDA(random,input,mode);
            const bool reduced=frontend::decide2NPDA(random,input,mode);
            assert(reference==reduced);
        }
    }

    std::cout << "General CFG and arbitrary 2NPDA front-end tests passed\n";
    return 0;
}

} // namespace

TEST(CFLCertificates, Frontend) {
    EXPECT_EQ(runImportedTest(), 0);
}
