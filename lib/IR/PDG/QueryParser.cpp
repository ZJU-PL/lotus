#include "IR/PDG/QueryParser.h"
#include "IR/PDG/QueryLanguage.h"
#include <iostream>

namespace pdg {

QueryParser::QueryParser() : pdg_(ProgramGraph::getInstance()), executor_(pdg_) {
}

int QueryParser::evaluate(const std::string& query) {
    // For now, just print the query - full parser implementation would go here
    std::cout << "Query: " << query << "\n";
    return 0;
}

int QueryParser::evaluatePolicy(const std::string& policy) {
    // For now, just print the policy - full parser implementation would go here
    std::cout << "Policy: " << policy << "\n";
    return 0;
}

} // namespace pdg

