#include "CFL/Certificates/Frontend.h"
#include <iostream>

int main() {
    using namespace cflcert;
    // S -> A B C; A -> epsilon | a; B -> b; C -> c.
    cfg::GeneralGrammar grammar;
    grammar.nonterminals = 4;
    grammar.terminals = 3; // a=0, b=1, c=2
    grammar.start = 0;
    grammar.productions = {
        {0, {cfg::Symbol::N(1), cfg::Symbol::N(2), cfg::Symbol::N(3)}},
        {1, {}},
        {1, {cfg::Symbol::T(0)}},
        {2, {cfg::Symbol::T(1)}},
        {3, {cfg::Symbol::T(2)}}
    };

    cfg::Graph graph;
    graph.n = 3;
    graph.source = 0;
    graph.target = 2;
    graph.edges = {{0, 1, 1}, {1, 2, 2}}; // path label "bc"

    auto result = frontend::solveCFG(grammar, graph);
    std::cout << (result.status == frontend::ReachabilityStatus::Reachable
                      ? "reachable" : "unreachable")
              << "; certificate valid = "
              << std::boolalpha << frontend::verifyCFGResult(grammar, graph, result)
              << '\n';
}
