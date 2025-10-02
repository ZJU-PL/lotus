/**
*  FIXME: implemeent the full lang in Exploring and Enforcing Security Guarantees via Program Dependence Graphs, PLDI'25
 * @file QueryLanguage.cpp
 * @brief Implementation of the query language and executor for PDG analysis
 *
 * This file implements the core functionality for querying Program Dependence Graphs
 * using a domain-specific language inspired by PIDGINQL. The query language supports:
 * - Graph traversal operations (forward/backward slicing)
 * - Node and edge selection based on types
 * - Set operations (union, intersection, difference)
 * - Policy definitions and evaluation
 * - Function definitions and calls
 * - Primitive operations for security analysis
 */

#include "IR/PDG/QueryLanguage.h"
#include <sstream>

using namespace llvm;

namespace pdg {

// ============================================================================
// QueryAST implementations
// ============================================================================

std::string BinaryOpAST::toString() const {
    std::string opStr;
    switch (op_) {
        case OpType::UNION: opStr = "∪"; break;
        case OpType::INTERSECTION: opStr = "∩"; break;
        case OpType::DIFFERENCE: opStr = "-"; break;
        case OpType::EQUALS: opStr = "=="; break;
        case OpType::NOT_EQUALS: opStr = "!="; break;
    }
    return "(" + left_->toString() + " " + opStr + " " + right_->toString() + ")";
}

std::unique_ptr<QueryResult> BinaryOpAST::evaluate(QueryExecutor& executor) const {
    auto leftResult = left_->evaluate(executor);
    auto rightResult = right_->evaluate(executor);
    
    switch (op_) {
        case OpType::UNION:
            return leftResult->unionWith(*rightResult);
        case OpType::INTERSECTION:
            return leftResult->intersectWith(*rightResult);
        case OpType::DIFFERENCE:
            return leftResult->differenceWith(*rightResult);
        case OpType::EQUALS:
            return std::make_unique<BooleanResult>(leftResult->toString() == rightResult->toString());
        case OpType::NOT_EQUALS:
            return std::make_unique<BooleanResult>(leftResult->toString() != rightResult->toString());
    }
    return std::make_unique<BooleanResult>(false);
}

std::string UnaryOpAST::toString() const {
    std::string opStr;
    switch (op_) {
        case OpType::NOT: opStr = "!"; break;
        case OpType::NEGATE: opStr = "-"; break;
    }
    return opStr + operand_->toString();
}

std::unique_ptr<QueryResult> UnaryOpAST::evaluate(QueryExecutor& executor) const {
    auto result = operand_->evaluate(executor);
    
    switch (op_) {
        case OpType::NOT:
            return std::make_unique<BooleanResult>(!result->isEmpty());
        case OpType::NEGATE:
            // For now, just return the operand result
            return result;
    }
    return result;
}

std::string IdentifierAST::toString() const {
    return name_;
}

std::unique_ptr<QueryResult> IdentifierAST::evaluate(QueryExecutor& executor) const {
    auto result = executor.getVariable(name_);
    if (!result) {
        // Return empty result if variable not found
        return std::make_unique<NodeSetResult>();
    }
    return result;
}

std::string LiteralAST::toString() const {
    return value_;
}

std::unique_ptr<QueryResult> LiteralAST::evaluate(QueryExecutor& /*executor*/) const {
    switch (type_) {
        case LiteralType::STRING:
            return std::make_unique<StringResult>(value_);
        case LiteralType::INTEGER:
            return std::make_unique<IntegerResult>(std::stoll(value_));
        case LiteralType::BOOLEAN:
            return std::make_unique<BooleanResult>(value_ == "true");
        case LiteralType::NODE_TYPE:
        case LiteralType::EDGE_TYPE:
            // These are used in primitive expressions, return as string for now
            return std::make_unique<StringResult>(value_);
        default:
            return std::make_unique<StringResult>(value_);
    }
}

std::string FunctionCallAST::toString() const {
    std::string result = name_ + "(";
    for (size_t i = 0; i < args_.size(); ++i) {
        if (i > 0) result += ", ";
        result += args_[i]->toString();
    }
    result += ")";
    return result;
}

std::unique_ptr<QueryResult> FunctionCallAST::evaluate(QueryExecutor& executor) const {
    // Check if it's a built-in function
    if (name_ == "forwardSlice") {
        if (args_.size() != 1) {
            return std::make_unique<NodeSetResult>();
        }
        auto argResult = args_[0]->evaluate(executor);
        auto nodes = argResult->getNodes();
        return executor.forwardSlice(nodes);
    }
    else if (name_ == "backwardSlice") {
        if (args_.size() != 1) {
            return std::make_unique<NodeSetResult>();
        }
        auto argResult = args_[0]->evaluate(executor);
        auto nodes = argResult->getNodes();
        return executor.backwardSlice(nodes);
    }
    else if (name_ == "shortestPath") {
        if (args_.size() != 2) {
            return std::make_unique<NodeSetResult>();
        }
        auto fromResult = args_[0]->evaluate(executor);
        auto toResult = args_[1]->evaluate(executor);
        return executor.shortestPath(fromResult->getNodes(), toResult->getNodes());
    }
    
    // Check for user-defined function
    auto funcDef = executor.getFunction(name_);
    if (funcDef) {
        // Bind arguments to parameters
        auto params = funcDef->getParams();
        for (size_t i = 0; i < args_.size() && i < params.size(); ++i) {
            auto argResult = args_[i]->evaluate(executor);
            executor.bindVariable(params[i], std::move(argResult));
        }
        
        // Evaluate function body
        auto result = funcDef->getBody()->evaluate(executor);
        
        // Clean up bound variables
        for (const auto& param : params) {
            (void)param; // Suppress unused parameter warning
            executor.clearBindings(); // Simplified cleanup
        }
        
        return result;
    }
    
    return std::make_unique<NodeSetResult>();
}

std::string LetBindingAST::toString() const {
    return "let " + varName_ + " = " + value_->toString() + " in " + body_->toString();
}

std::unique_ptr<QueryResult> LetBindingAST::evaluate(QueryExecutor& executor) const {
    auto valueResult = value_->evaluate(executor);
    executor.bindVariable(varName_, std::move(valueResult));
    auto bodyResult = body_->evaluate(executor);
    return bodyResult;
}

std::string PrimitiveExprAST::toString() const {
    std::string result;
    switch (type_) {
        case PrimitiveType::PGM: result = "pgm"; break;
        case PrimitiveType::FORWARD_SLICE: result = "forwardSlice"; break;
        case PrimitiveType::BACKWARD_SLICE: result = "backwardSlice"; break;
        case PrimitiveType::SHORTEST_PATH: result = "shortestPath"; break;
        case PrimitiveType::REMOVE_NODES: result = "removeNodes"; break;
        case PrimitiveType::REMOVE_EDGES: result = "removeEdges"; break;
        case PrimitiveType::SELECT_EDGES: result = "selectEdges"; break;
        case PrimitiveType::SELECT_NODES: result = "selectNodes"; break;
        case PrimitiveType::FOR_EXPRESSION: result = "forExpression"; break;
        case PrimitiveType::FOR_PROCEDURE: result = "forProcedure"; break;
        case PrimitiveType::FIND_PC_NODES: result = "findPCNodes"; break;
        case PrimitiveType::REMOVE_CONTROL_DEPS: result = "removeControlDeps"; break;
        case PrimitiveType::RETURNS_OF: result = "returnsOf"; break;
        case PrimitiveType::FORMALS_OF: result = "formalsOf"; break;
        case PrimitiveType::ENTRIES_OF: result = "entriesOf"; break;
        case PrimitiveType::BETWEEN: result = "between"; break;
        case PrimitiveType::DECLASSIFIES: result = "declassifies"; break;
        case PrimitiveType::NO_EXPLICIT_FLOWS: result = "noExplicitFlows"; break;
        case PrimitiveType::FLOW_ACCESS_CONTROLLED: result = "flowAccessControlled"; break;
        case PrimitiveType::ACCESS_CONTROLLED: result = "accessControlled"; break;
    }
    
    if (!args_.empty()) {
        result += "(";
        for (size_t i = 0; i < args_.size(); ++i) {
            if (i > 0) result += ", ";
            result += args_[i]->toString();
        }
        result += ")";
    }
    
    return result;
}

std::unique_ptr<QueryResult> PrimitiveExprAST::evaluate(QueryExecutor& executor) const {
    switch (type_) {
        case PrimitiveType::PGM: {
            // Return all nodes in the PDG
            auto result = std::make_unique<NodeSetResult>();
            auto& pdg = executor.getPDG();
            for (auto it = pdg.begin(); it != pdg.end(); ++it) {
                result->addNode(*it);
            }
            return result;
        }
        
        case PrimitiveType::FORWARD_SLICE: {
            // For method calls like pgm.forwardSlice(arg), args_[0] is pgm, args_[1] is the actual argument
            if (args_.size() < 2) {
                // Fallback for standalone calls: forwardSlice(arg)
                if (args_.size() == 1) {
                    auto argResult = args_[0]->evaluate(executor);
                    return executor.forwardSlice(argResult->getNodes());
                }
                return std::make_unique<NodeSetResult>();
            }
            auto argResult = args_[1]->evaluate(executor);
            return executor.forwardSlice(argResult->getNodes());
        }
        
        case PrimitiveType::BACKWARD_SLICE: {
            // For method calls like pgm.backwardSlice(arg), args_[0] is pgm, args_[1] is the actual argument
            if (args_.size() < 2) {
                // Fallback for standalone calls: backwardSlice(arg)
                if (args_.size() == 1) {
                    auto argResult = args_[0]->evaluate(executor);
                    return executor.backwardSlice(argResult->getNodes());
                }
                return std::make_unique<NodeSetResult>();
            }
            auto argResult = args_[1]->evaluate(executor);
            return executor.backwardSlice(argResult->getNodes());
        }
        
        case PrimitiveType::SHORTEST_PATH: {
            // For method calls like pgm.shortestPath(from, to), args_[0] is pgm, args_[1] is from, args_[2] is to
            if (args_.size() < 3) {
                // Fallback for standalone calls: shortestPath(from, to)
                if (args_.size() == 2) {
                    auto fromResult = args_[0]->evaluate(executor);
                    auto toResult = args_[1]->evaluate(executor);
                    return executor.shortestPath(fromResult->getNodes(), toResult->getNodes());
                }
                return std::make_unique<NodeSetResult>();
            }
            auto fromResult = args_[1]->evaluate(executor);
            auto toResult = args_[2]->evaluate(executor);
            return executor.shortestPath(fromResult->getNodes(), toResult->getNodes());
        }
        
        case PrimitiveType::SELECT_EDGES: {
            // For method calls like pgm.selectEdges(edgeType), args_[0] is pgm, args_[1] is edgeType
            if (args_.size() < 2) {
                // Fallback for standalone calls: selectEdges(edgeType)
                if (args_.size() == 1) {
                    auto argResult = args_[0]->evaluate(executor);
                    // Parse edge type from string
                    if (argResult->getType() == QueryResult::Type::STRING) {
                        auto stringResult = static_cast<const StringResult*>(argResult.get());
                        std::string edgeTypeStr = stringResult->getValue();
                        // Map string to EdgeType enum
                        EdgeType edgeType = EdgeType::DATA_DEF_USE; // Default
                        if (edgeTypeStr == "CD") edgeType = EdgeType::CONTROLDEP_BR;
                        else if (edgeTypeStr == "EXP") edgeType = EdgeType::DATA_DEF_USE;
                        else if (edgeTypeStr == "TRUE") edgeType = EdgeType::CONTROLDEP_BR;
                        else if (edgeTypeStr == "FALSE") edgeType = EdgeType::CONTROLDEP_BR;
                        return executor.selectEdges(edgeType);
                    }
                    return executor.selectEdges(EdgeType::DATA_DEF_USE);
                }
                return std::make_unique<EdgeSetResult>();
            }
            auto argResult = args_[1]->evaluate(executor);
            // Parse edge type from string
            if (argResult->getType() == QueryResult::Type::STRING) {
                auto stringResult = static_cast<const StringResult*>(argResult.get());
                std::string edgeTypeStr = stringResult->getValue();
                // Map string to EdgeType enum
                EdgeType edgeType = EdgeType::DATA_DEF_USE; // Default
                if (edgeTypeStr == "CD") edgeType = EdgeType::CONTROLDEP_BR;
                else if (edgeTypeStr == "EXP") edgeType = EdgeType::DATA_DEF_USE;
                else if (edgeTypeStr == "TRUE") edgeType = EdgeType::CONTROLDEP_BR;
                else if (edgeTypeStr == "FALSE") edgeType = EdgeType::CONTROLDEP_BR;
                return executor.selectEdges(edgeType);
            }
            return executor.selectEdges(EdgeType::DATA_DEF_USE);
        }
        
        case PrimitiveType::SELECT_NODES: {
            // For method calls like pgm.selectNodes(nodeType), args_[0] is pgm, args_[1] is nodeType
            if (args_.size() < 2) {
                // Fallback for standalone calls: selectNodes(nodeType)
                if (args_.size() == 1) {
                    auto argResult = args_[0]->evaluate(executor);
                    // Parse node type from string
                    if (argResult->getType() == QueryResult::Type::STRING) {
                        auto stringResult = static_cast<const StringResult*>(argResult.get());
                        std::string nodeTypeStr = stringResult->getValue();
                        // Map string to GraphNodeType enum
                        GraphNodeType nodeType = GraphNodeType::INST_OTHER; // Default
                        if (nodeTypeStr == "PC") nodeType = GraphNodeType::INST_BR;
                        else if (nodeTypeStr == "ENTRYPC") nodeType = GraphNodeType::FUNC_ENTRY;
                        else if (nodeTypeStr == "FORMAL") nodeType = GraphNodeType::PARAM_FORMALIN;
                        else if (nodeTypeStr == "ACTUAL") nodeType = GraphNodeType::PARAM_ACTUALIN;
                        else if (nodeTypeStr == "INST_FUNCALL") nodeType = GraphNodeType::INST_FUNCALL;
                        else if (nodeTypeStr == "INST_RET") nodeType = GraphNodeType::INST_RET;
                        else if (nodeTypeStr == "INST_BR") nodeType = GraphNodeType::INST_BR;
                        else if (nodeTypeStr == "INST_OTHER") nodeType = GraphNodeType::INST_OTHER;
                        return executor.selectNodes(nodeType);
                    }
                    return executor.selectNodes(GraphNodeType::INST_OTHER);
                }
                return std::make_unique<NodeSetResult>();
            }
            auto argResult = args_[1]->evaluate(executor);
            // Parse node type from string
            if (argResult->getType() == QueryResult::Type::STRING) {
                auto stringResult = static_cast<const StringResult*>(argResult.get());
                std::string nodeTypeStr = stringResult->getValue();
                // Map string to GraphNodeType enum
                GraphNodeType nodeType = GraphNodeType::INST_OTHER; // Default
                if (nodeTypeStr == "PC") nodeType = GraphNodeType::INST_BR;
                else if (nodeTypeStr == "ENTRYPC") nodeType = GraphNodeType::FUNC_ENTRY;
                else if (nodeTypeStr == "FORMAL") nodeType = GraphNodeType::PARAM_FORMALIN;
                else if (nodeTypeStr == "ACTUAL") nodeType = GraphNodeType::PARAM_ACTUALIN;
                else if (nodeTypeStr == "INST_FUNCALL") nodeType = GraphNodeType::INST_FUNCALL;
                else if (nodeTypeStr == "INST_RET") nodeType = GraphNodeType::INST_RET;
                else if (nodeTypeStr == "INST_BR") nodeType = GraphNodeType::INST_BR;
                else if (nodeTypeStr == "INST_OTHER") nodeType = GraphNodeType::INST_OTHER;
                return executor.selectNodes(nodeType);
            }
            return executor.selectNodes(GraphNodeType::INST_OTHER);
        }
        
        case PrimitiveType::RETURNS_OF: {
            // For method calls like pgm.returnsOf(procName), args_[0] is pgm, args_[1] is procName
            if (args_.size() < 2) {
                // Fallback for standalone calls: returnsOf(procName)
                if (args_.size() == 1) {
                    auto argResult = args_[0]->evaluate(executor);
                    if (argResult->getType() == QueryResult::Type::STRING) {
                        auto stringResult = static_cast<const StringResult*>(argResult.get());
                        return executor.returnsOf(stringResult->getValue());
                    }
                    return std::make_unique<NodeSetResult>();
                }
                return std::make_unique<NodeSetResult>();
            }
            auto argResult = args_[1]->evaluate(executor);
            if (argResult->getType() == QueryResult::Type::STRING) {
                auto stringResult = static_cast<const StringResult*>(argResult.get());
                return executor.returnsOf(stringResult->getValue());
            }
            return std::make_unique<NodeSetResult>();
        }
        
        case PrimitiveType::FORMALS_OF: {
            // For method calls like pgm.formalsOf(procName), args_[0] is pgm, args_[1] is procName
            if (args_.size() < 2) {
                // Fallback for standalone calls: formalsOf(procName)
                if (args_.size() == 1) {
                    auto argResult = args_[0]->evaluate(executor);
                    if (argResult->getType() == QueryResult::Type::STRING) {
                        auto stringResult = static_cast<const StringResult*>(argResult.get());
                        return executor.formalsOf(stringResult->getValue());
                    }
                    return std::make_unique<NodeSetResult>();
                }
                return std::make_unique<NodeSetResult>();
            }
            auto argResult = args_[1]->evaluate(executor);
            if (argResult->getType() == QueryResult::Type::STRING) {
                auto stringResult = static_cast<const StringResult*>(argResult.get());
                return executor.formalsOf(stringResult->getValue());
            }
            return std::make_unique<NodeSetResult>();
        }
        
        case PrimitiveType::BETWEEN: {
            // For method calls like pgm.between(from, to), args_[0] is pgm, args_[1] is from, args_[2] is to
            if (args_.size() < 3) {
                // Fallback for standalone calls: between(from, to)
                if (args_.size() == 2) {
                    auto fromResult = args_[0]->evaluate(executor);
                    auto toResult = args_[1]->evaluate(executor);
                    return executor.between(fromResult->getNodes(), toResult->getNodes());
                }
                return std::make_unique<NodeSetResult>();
            }
            auto fromResult = args_[1]->evaluate(executor);
            auto toResult = args_[2]->evaluate(executor);
            return executor.between(fromResult->getNodes(), toResult->getNodes());
        }
        
        default:
            return std::make_unique<NodeSetResult>();
    }
}

std::string PolicyCheckAST::toString() const {
    return expr_->toString() + (shouldBeEmpty_ ? " is empty" : " is not empty");
}

bool PolicyCheckAST::evaluate(QueryExecutor& executor) const {
    auto result = expr_->evaluate(executor);
    bool isEmpty = result->isEmpty();
    return shouldBeEmpty_ ? isEmpty : !isEmpty;
}

std::string FunctionDefAST::toString() const {
    std::string result = "let " + name_ + "(";
    for (size_t i = 0; i < params_.size(); ++i) {
        if (i > 0) result += ", ";
        result += params_[i];
    }
    result += ") = " + body_->toString();
    if (isPolicy_) {
        result += " is empty";
    }
    result += ";";
    return result;
}

// ============================================================================
// QueryResult implementations
// ============================================================================

const std::unordered_set<Node*>& QueryResult::getNodes() const {
    static const std::unordered_set<Node*> empty;
    return empty;
}

void QueryResult::addNode(Node* /*node*/) {
    // Default implementation does nothing
}

void QueryResult::removeNode(Node* /*node*/) {
    // Default implementation does nothing
}

bool QueryResult::containsNode(Node* /*node*/) const {
    return false;
}

const std::unordered_set<Edge*>& QueryResult::getEdges() const {
    static const std::unordered_set<Edge*> empty;
    return empty;
}

void QueryResult::addEdge(Edge* /*edge*/) {
    // Default implementation does nothing
}

void QueryResult::removeEdge(Edge* /*edge*/) {
    // Default implementation does nothing
}

bool QueryResult::containsEdge(Edge* /*edge*/) const {
    return false;
}

std::unique_ptr<QueryResult> QueryResult::unionWith(const QueryResult& /*other*/) const {
    // Default implementation returns empty result
    return std::make_unique<NodeSetResult>();
}

std::unique_ptr<QueryResult> QueryResult::intersectWith(const QueryResult& /*other*/) const {
    // Default implementation returns empty result
    return std::make_unique<NodeSetResult>();
}

std::unique_ptr<QueryResult> QueryResult::differenceWith(const QueryResult& /*other*/) const {
    // Default implementation returns empty result
    return std::make_unique<NodeSetResult>();
}

std::string NodeSetResult::toString() const {
    std::ostringstream oss;
    oss << "NodeSet(" << nodes_.size() << " nodes)";
    return oss.str();
}

bool NodeSetResult::isEmpty() const {
    return nodes_.empty();
}

void NodeSetResult::addNode(Node* node) {
    if (node) {
        nodes_.insert(node);
    }
}

void NodeSetResult::removeNode(Node* node) {
    nodes_.erase(node);
}

bool NodeSetResult::containsNode(Node* node) const {
    return nodes_.find(node) != nodes_.end();
}

std::unique_ptr<QueryResult> NodeSetResult::unionWith(const QueryResult& other) const {
    auto result = std::make_unique<NodeSetResult>(nodes_);
    if (other.getType() == Type::NODE_SET) {
        const auto& otherNodes = other.getNodes();
        for (auto node : otherNodes) {
            result->addNode(node);
        }
    }
    return result;
}

std::unique_ptr<QueryResult> NodeSetResult::intersectWith(const QueryResult& other) const {
    auto result = std::make_unique<NodeSetResult>();
    if (other.getType() == Type::NODE_SET) {
        const auto& otherNodes = other.getNodes();
        for (auto node : nodes_) {
            if (otherNodes.find(node) != otherNodes.end()) {
                result->addNode(node);
            }
        }
    }
    return result;
}

std::unique_ptr<QueryResult> NodeSetResult::differenceWith(const QueryResult& other) const {
    auto result = std::make_unique<NodeSetResult>(nodes_);
    if (other.getType() == Type::NODE_SET) {
        const auto& otherNodes = other.getNodes();
        for (auto node : otherNodes) {
            result->removeNode(node);
        }
    }
    return result;
}

std::string EdgeSetResult::toString() const {
    std::ostringstream oss;
    oss << "EdgeSet(" << edges_.size() << " edges)";
    return oss.str();
}

bool EdgeSetResult::isEmpty() const {
    return edges_.empty();
}

void EdgeSetResult::addEdge(Edge* edge) {
    if (edge) {
        edges_.insert(edge);
    }
}

void EdgeSetResult::removeEdge(Edge* edge) {
    edges_.erase(edge);
}

bool EdgeSetResult::containsEdge(Edge* edge) const {
    return edges_.find(edge) != edges_.end();
}

std::unique_ptr<QueryResult> EdgeSetResult::unionWith(const QueryResult& other) const {
    auto result = std::make_unique<EdgeSetResult>(edges_);
    if (other.getType() == Type::EDGE_SET) {
        const auto& otherEdges = other.getEdges();
        for (auto edge : otherEdges) {
            result->addEdge(edge);
        }
    }
    return result;
}

std::unique_ptr<QueryResult> EdgeSetResult::intersectWith(const QueryResult& other) const {
    auto result = std::make_unique<EdgeSetResult>();
    if (other.getType() == Type::EDGE_SET) {
        const auto& otherEdges = other.getEdges();
        for (auto edge : edges_) {
            if (otherEdges.find(edge) != otherEdges.end()) {
                result->addEdge(edge);
            }
        }
    }
    return result;
}

std::unique_ptr<QueryResult> EdgeSetResult::differenceWith(const QueryResult& other) const {
    auto result = std::make_unique<EdgeSetResult>(edges_);
    if (other.getType() == Type::EDGE_SET) {
        const auto& otherEdges = other.getEdges();
        for (auto edge : otherEdges) {
            result->removeEdge(edge);
        }
    }
    return result;
}

std::string BooleanResult::toString() const {
    return value_ ? "true" : "false";
}

bool BooleanResult::isEmpty() const {
    return !value_;
}

std::string StringResult::toString() const {
    return "\"" + value_ + "\"";
}

bool StringResult::isEmpty() const {
    return value_.empty();
}

std::string IntegerResult::toString() const {
    return std::to_string(value_);
}

bool IntegerResult::isEmpty() const {
    return value_ == 0;
}

// ============================================================================
// QueryExecutor implementations
// ============================================================================

void QueryExecutor::bindVariable(const std::string& name, std::unique_ptr<QueryResult> value) {
    variables_[name] = std::move(value);
}

std::unique_ptr<QueryResult> QueryExecutor::getVariable(const std::string& name) {
    auto it = variables_.find(name);
    if (it != variables_.end()) {
        // Return a copy of the result
        return std::make_unique<NodeSetResult>(it->second->getNodes());
    }
    return nullptr;
}

bool QueryExecutor::hasVariable(const std::string& name) const {
    return variables_.find(name) != variables_.end();
}

void QueryExecutor::defineFunction(const std::string& name, std::unique_ptr<FunctionDefAST> func) {
    functions_[name] = std::move(func);
}

std::unique_ptr<FunctionDefAST> QueryExecutor::getFunction(const std::string& name) {
    auto it = functions_.find(name);
    if (it != functions_.end()) {
        // Return a copy (simplified)
        return nullptr; // Would need proper copying
    }
    return nullptr;
}

bool QueryExecutor::hasFunction(const std::string& name) const {
    return functions_.find(name) != functions_.end();
}

std::unique_ptr<QueryResult> QueryExecutor::forwardSlice(const std::unordered_set<Node*>& startNodes, int depth) {
    auto result = std::make_unique<NodeSetResult>();
    std::unordered_set<Node*> visited;
    std::vector<Node*> queue(startNodes.begin(), startNodes.end());
    
    int currentDepth = 0;
    while (!queue.empty() && (depth == -1 || currentDepth < depth)) {
        std::vector<Node*> nextLevel;
        
        for (auto node : queue) {
            if (visited.find(node) != visited.end()) continue;
            visited.insert(node);
            result->addNode(node);
            
            // Add all outgoing neighbors
            for (auto edge : node->getOutEdgeSet()) {
                auto neighbor = edge->getDstNode();
                if (visited.find(neighbor) == visited.end()) {
                    nextLevel.push_back(neighbor);
                }
            }
        }
        
        queue = nextLevel;
        currentDepth++;
    }
    
    return result;
}

std::unique_ptr<QueryResult> QueryExecutor::backwardSlice(const std::unordered_set<Node*>& startNodes, int depth) {
    auto result = std::make_unique<NodeSetResult>();
    std::unordered_set<Node*> visited;
    std::vector<Node*> queue(startNodes.begin(), startNodes.end());
    
    int currentDepth = 0;
    while (!queue.empty() && (depth == -1 || currentDepth < depth)) {
        std::vector<Node*> nextLevel;
        
        for (auto node : queue) {
            if (visited.find(node) != visited.end()) continue;
            visited.insert(node);
            result->addNode(node);
            
            // Add all incoming neighbors
            for (auto edge : node->getInEdgeSet()) {
                auto neighbor = edge->getSrcNode();
                if (visited.find(neighbor) == visited.end()) {
                    nextLevel.push_back(neighbor);
                }
            }
        }
        
        queue = nextLevel;
        currentDepth++;
    }
    
    return result;
}

std::unique_ptr<QueryResult> QueryExecutor::shortestPath(const std::unordered_set<Node*>& from, const std::unordered_set<Node*>& to) {
    auto result = std::make_unique<NodeSetResult>();
    
    // BFS to find shortest path
    std::unordered_map<Node*, Node*> parent;
    std::queue<Node*> queue;
    std::unordered_set<Node*> visited;
    
    // Start from all 'from' nodes
    for (auto start : from) {
        queue.push(start);
        visited.insert(start);
    }
    
    Node* target = nullptr;
    while (!queue.empty() && !target) {
        auto current = queue.front();
        queue.pop();
        
        if (to.find(current) != to.end()) {
            target = current;
            break;
        }
        
        for (auto edge : current->getOutEdgeSet()) {
            auto neighbor = edge->getDstNode();
            if (visited.find(neighbor) == visited.end()) {
                visited.insert(neighbor);
                parent[neighbor] = current;
                queue.push(neighbor);
            }
        }
    }
    
    // Reconstruct path
    if (target) {
        std::vector<Node*> path;
        Node* current = target;
        while (current) {
            path.push_back(current);
            auto it = parent.find(current);
            current = (it != parent.end()) ? it->second : nullptr;
        }
        
        for (auto node : path) {
            result->addNode(node);
        }
    }
    
    return result;
}

std::unique_ptr<QueryResult> QueryExecutor::selectEdges(EdgeType edgeType) {
    auto result = std::make_unique<EdgeSetResult>();
    
    for (auto it = pdg_.begin(); it != pdg_.end(); ++it) {
        for (auto edge : (*it)->getOutEdgeSet()) {
            if (edge->getEdgeType() == edgeType) {
                result->addEdge(edge);
            }
        }
    }
    
    return result;
}

std::unique_ptr<QueryResult> QueryExecutor::selectNodes(GraphNodeType nodeType) {
    auto result = std::make_unique<NodeSetResult>();
    
    for (auto it = pdg_.begin(); it != pdg_.end(); ++it) {
        if ((*it)->getNodeType() == nodeType) {
            result->addNode(*it);
        }
    }
    
    return result;
}

std::unique_ptr<QueryResult> QueryExecutor::findPCNodes(const std::unordered_set<Node*>& exprNodes, EdgeType edgeType) {
    auto result = std::make_unique<NodeSetResult>();
    
    for (auto exprNode : exprNodes) {
        for (auto edge : exprNode->getOutEdgeSet()) {
            if (edge->getEdgeType() == edgeType) {
                auto pcNode = edge->getDstNode();
                if (pcNode->getNodeType() == GraphNodeType::INST_BR) {
                    result->addNode(pcNode);
                }
            }
        }
    }
    
    return result;
}

std::unique_ptr<QueryResult> QueryExecutor::removeControlDeps(const std::unordered_set<Node*>& nodes) {
    auto result = std::make_unique<NodeSetResult>();
    
    for (auto node : nodes) {
        // Add node if it's not control dependent on the specified nodes
        bool isControlDependent = false;
        for (auto edge : node->getInEdgeSet()) {
            if (edge->getEdgeType() == EdgeType::CONTROLDEP_BR || 
                edge->getEdgeType() == EdgeType::CONTROLDEP_ENTRY) {
                if (nodes.find(edge->getSrcNode()) != nodes.end()) {
                    isControlDependent = true;
                    break;
                }
            }
        }
        
        if (!isControlDependent) {
            result->addNode(node);
        }
    }
    
    return result;
}

std::unique_ptr<QueryResult> QueryExecutor::returnsOf(const std::string& procName) {
    auto result = std::make_unique<NodeSetResult>();
    
    // Find function by name
    for (auto& F : pdg_.getFuncWrapperMap()) {
        if (F.first->getName() == procName) {
            // Find return nodes for this function
            for (auto it = pdg_.begin(); it != pdg_.end(); ++it) {
                if ((*it)->getNodeType() == GraphNodeType::INST_RET && 
                    (*it)->getFunc() == F.first) {
                    result->addNode(*it);
                }
            }
            break;
        }
    }
    
    return result;
}

std::unique_ptr<QueryResult> QueryExecutor::formalsOf(const std::string& procName) {
    auto result = std::make_unique<NodeSetResult>();
    
    // Find function by name
    for (auto& F : pdg_.getFuncWrapperMap()) {
        if (F.first->getName() == procName) {
            // Find formal parameter nodes for this function
            for (auto it = pdg_.begin(); it != pdg_.end(); ++it) {
                if (((*it)->getNodeType() == GraphNodeType::PARAM_FORMALIN ||
                     (*it)->getNodeType() == GraphNodeType::PARAM_FORMALOUT) && 
                    (*it)->getFunc() == F.first) {
                    result->addNode(*it);
                }
            }
            break;
        }
    }
    
    return result;
}

std::unique_ptr<QueryResult> QueryExecutor::entriesOf(const std::string& procName) {
    auto result = std::make_unique<NodeSetResult>();
    
    // Find function by name
    for (auto& F : pdg_.getFuncWrapperMap()) {
        if (F.first->getName() == procName) {
            // Find entry nodes for this function
            for (auto it = pdg_.begin(); it != pdg_.end(); ++it) {
                if ((*it)->getNodeType() == GraphNodeType::FUNC_ENTRY && 
                    (*it)->getFunc() == F.first) {
                    result->addNode(*it);
                }
            }
            break;
        }
    }
    
    return result;
}

std::unique_ptr<QueryResult> QueryExecutor::between(const std::unordered_set<Node*>& from, const std::unordered_set<Node*>& to) {
    auto result = std::make_unique<NodeSetResult>();
    
    // Find all nodes on paths from 'from' to 'to'
    for (auto start : from) {
        std::unordered_set<Node*> visited;
        std::queue<Node*> queue;
        queue.push(start);
        visited.insert(start);
        
        while (!queue.empty()) {
            auto current = queue.front();
            queue.pop();
            
            if (to.find(current) != to.end()) {
                // Found a path, add all nodes in the path
                result->addNode(current);
                continue;
            }
            
            for (auto edge : current->getOutEdgeSet()) {
                auto neighbor = edge->getDstNode();
                if (visited.find(neighbor) == visited.end()) {
                    visited.insert(neighbor);
                    queue.push(neighbor);
                }
            }
        }
    }
    
    return result;
}

std::unique_ptr<QueryResult> QueryExecutor::declassifies(const std::unordered_set<Node*>& /*declassifiers*/, 
                                                        const std::unordered_set<Node*>& sources, 
                                                        const std::unordered_set<Node*>& sinks) {
    // Remove declassifier nodes and check if there are still paths from sources to sinks
    auto result = std::make_unique<NodeSetResult>();
    
    // This is a simplified implementation
    // In practice, we would remove declassifier nodes and then check for remaining paths
    for (auto source : sources) {
        for (auto sink : sinks) {
            if (pdg_.canReach(*source, *sink)) {
                result->addNode(source);
                result->addNode(sink);
            }
        }
    }
    
    return result;
}

std::unique_ptr<QueryResult> QueryExecutor::noExplicitFlows(const std::unordered_set<Node*>& sources, 
                                                           const std::unordered_set<Node*>& sinks) {
    // Remove control dependencies and check for data flows
    auto result = std::make_unique<NodeSetResult>();
    
    for (auto source : sources) {
        for (auto sink : sinks) {
            // Check for data dependencies only (no control dependencies)
            std::set<EdgeType> excludeEdges = {
                EdgeType::CONTROLDEP_BR,
                EdgeType::CONTROLDEP_ENTRY,
                EdgeType::CONTROLDEP_CALLINV,
                EdgeType::CONTROLDEP_CALLRET,
                EdgeType::CONTROLDEP_IND_BR
            };
            
            if (pdg_.canReach(*source, *sink, excludeEdges)) {
                result->addNode(source);
                result->addNode(sink);
            }
        }
    }
    
    return result;
}

std::unique_ptr<QueryResult> QueryExecutor::flowAccessControlled(const std::unordered_set<Node*>& checks,
                                                                const std::unordered_set<Node*>& sources,
                                                                const std::unordered_set<Node*>& sinks) {
    // Check if flows from sources to sinks are controlled by checks
    auto result = std::make_unique<NodeSetResult>();
    
    // This is a simplified implementation
    // In practice, we would check if all paths from sources to sinks go through checks
    for (auto source : sources) {
        for (auto sink : sinks) {
            bool isControlled = false;
            for (auto check : checks) {
                if (pdg_.canReach(*source, *check) && pdg_.canReach(*check, *sink)) {
                    isControlled = true;
                    break;
                }
            }
            
            if (!isControlled) {
                result->addNode(source);
                result->addNode(sink);
            }
        }
    }
    
    return result;
}

std::unique_ptr<QueryResult> QueryExecutor::accessControlled(const std::unordered_set<Node*>& checks,
                                                            const std::unordered_set<Node*>& sensitiveOps) {
    // Check if sensitive operations are controlled by checks
    auto result = std::make_unique<NodeSetResult>();
    
    for (auto op : sensitiveOps) {
        bool isControlled = false;
        for (auto check : checks) {
            if (pdg_.canReach(*check, *op)) {
                isControlled = true;
                break;
            }
        }
        
        if (!isControlled) {
            result->addNode(op);
        }
    }
    
    return result;
}

void QueryExecutor::clearBindings() {
    variables_.clear();
}

} // namespace pdg
