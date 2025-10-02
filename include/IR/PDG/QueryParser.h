#pragma once

#include "IR/PDG/QueryLanguage.h"
#include <string>
#include <memory>

namespace pdg {

/**
 * @brief The query language parser
 * 
 * Implementation of the PIDGINQL-inspired query language. It supports:
 * - Method chaining with dot notation (pgm.method())
 * - Set operations (union U, intersection ∩)
 * - Primitive operations (forwardSlice, selectNodes, etc.)
 * - Policy definitions and checks
 * - Function definitions
 */
class QueryParser {
public:
    QueryParser();

    int evaluate(const std::string& query);
    int evaluatePolicy(const std::string& policy);
    
    // Access to executor for direct operations
    QueryExecutor& getExecutor() { return executor_; }

private:
    ProgramGraph& pdg_;
    QueryExecutor executor_;
    std::unique_ptr<QueryAST> ast_;
    std::unique_ptr<QueryResult> result_;
    std::unique_ptr<PolicyCheckAST> policyCheck_;
    std::unique_ptr<FunctionDefAST> functionDef_;
    std::unique_ptr<LetBindingAST> letBinding_;
    std::unique_ptr<FunctionCallAST> functionCall_;
    std::unique_ptr<LiteralAST> literal_;
    std::unique_ptr<IdentifierAST> identifier_;
    std::unique_ptr<UnaryOpAST> unaryOp_;
    std::unique_ptr<BinaryOpAST> binaryOp_;

};
} // namespace pdg
