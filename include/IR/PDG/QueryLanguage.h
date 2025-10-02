#pragma once

#include "IR/PDG/LLVMEssentials.h"
#include "IR/PDG/Graph.h"
#include "IR/PDG/PDGEnums.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>

namespace pdg {

// Forward declarations
class QueryExecutor;
class QueryResult;

/**
 * @brief Abstract syntax tree nodes for the query language
 */
class QueryAST {
public:
    enum class Type {
        EXPRESSION,
        POLICY,
        FUNCTION_DEF,
        LET_BINDING,
        BINARY_OP,
        UNARY_OP,
        IDENTIFIER,
        LITERAL,
        FUNCTION_CALL,
        PRIMITIVE_EXPR
    };

    virtual ~QueryAST() = default;
    virtual Type getType() const = 0;
    virtual std::string toString() const = 0;
};

class ExpressionAST : public QueryAST {
public:
    Type getType() const override { return Type::EXPRESSION; }
    virtual std::unique_ptr<QueryResult> evaluate(QueryExecutor& executor) const = 0;
};

class PolicyAST : public QueryAST {
public:
    Type getType() const override { return Type::POLICY; }
    virtual bool evaluate(QueryExecutor& executor) const = 0;
};

class BinaryOpAST : public ExpressionAST {
public:
    enum class OpType {
        UNION,      // ∪
        INTERSECTION, // ∩
        DIFFERENCE,   // -
        EQUALS,       // ==
        NOT_EQUALS    // !=
    };

    BinaryOpAST(std::unique_ptr<ExpressionAST> left, OpType op, std::unique_ptr<ExpressionAST> right)
        : left_(std::move(left)), op_(op), right_(std::move(right)) {}

    std::string toString() const override;
    std::unique_ptr<QueryResult> evaluate(QueryExecutor& executor) const override;

private:
    std::unique_ptr<ExpressionAST> left_;
    OpType op_;
    std::unique_ptr<ExpressionAST> right_;
};

class UnaryOpAST : public ExpressionAST {
public:
    enum class OpType {
        NOT,        // !
        NEGATE      // -
    };

    UnaryOpAST(OpType op, std::unique_ptr<ExpressionAST> operand)
        : op_(op), operand_(std::move(operand)) {}

    std::string toString() const override;
    std::unique_ptr<QueryResult> evaluate(QueryExecutor& executor) const override;

private:
    OpType op_;
    std::unique_ptr<ExpressionAST> operand_;
};

class IdentifierAST : public ExpressionAST {
public:
    IdentifierAST(const std::string& name) : name_(name) {}

    std::string toString() const override;
    std::unique_ptr<QueryResult> evaluate(QueryExecutor& executor) const override;
    const std::string& getName() const { return name_; }

private:
    std::string name_;
};

class LiteralAST : public ExpressionAST {
public:
    enum class LiteralType {
        STRING,
        INTEGER,
        BOOLEAN,
        NODE_TYPE,
        EDGE_TYPE
    };

    LiteralAST(const std::string& value, LiteralType type) 
        : value_(value), type_(type) {}

    std::string toString() const override;
    std::unique_ptr<QueryResult> evaluate(QueryExecutor& executor) const override;
    const std::string& getValue() const { return value_; }
    LiteralType getLiteralType() const { return type_; }

private:
    std::string value_;
    LiteralType type_;
};

class FunctionCallAST : public ExpressionAST {
public:
    FunctionCallAST(const std::string& name, std::vector<std::unique_ptr<ExpressionAST>> args)
        : name_(name), args_(std::move(args)) {}

    std::string toString() const override;
    std::unique_ptr<QueryResult> evaluate(QueryExecutor& executor) const override;
    const std::string& getName() const { return name_; }
    const std::vector<std::unique_ptr<ExpressionAST>>& getArgs() const { return args_; }

private:
    std::string name_;
    std::vector<std::unique_ptr<ExpressionAST>> args_;
};

class LetBindingAST : public ExpressionAST {
public:
    LetBindingAST(const std::string& varName, std::unique_ptr<ExpressionAST> value, 
                  std::unique_ptr<ExpressionAST> body)
        : varName_(varName), value_(std::move(value)), body_(std::move(body)) {}

    std::string toString() const override;
    std::unique_ptr<QueryResult> evaluate(QueryExecutor& executor) const override;

private:
    std::string varName_;
    std::unique_ptr<ExpressionAST> value_;
    std::unique_ptr<ExpressionAST> body_;
};

class PrimitiveExprAST : public ExpressionAST {
public:
    enum class PrimitiveType {
        PGM,                    // pgm - the program graph
        FORWARD_SLICE,          // forwardSlice(expr)
        BACKWARD_SLICE,         // backwardSlice(expr)
        SHORTEST_PATH,          // shortestPath(expr1, expr2)
        REMOVE_NODES,           // removeNodes(expr)
        REMOVE_EDGES,           // removeEdges(expr)
        SELECT_EDGES,           // selectEdges(edgeType)
        SELECT_NODES,           // selectNodes(nodeType)
        FOR_EXPRESSION,         // forExpression(javaExpr)
        FOR_PROCEDURE,          // forProcedure(procName)
        FIND_PC_NODES,          // findPCNodes(expr, edgeType)
        REMOVE_CONTROL_DEPS,    // removeControlDeps(expr)
        RETURNS_OF,             // returnsOf(procName)
        FORMALS_OF,             // formalsOf(procName)
        ENTRIES_OF,             // entriesOf(procName)
        BETWEEN,                // between(expr1, expr2)
        DECLASSIFIES,           // declassifies(declassifiers, sources, sinks)
        NO_EXPLICIT_FLOWS,      // noExplicitFlows(sources, sinks)
        FLOW_ACCESS_CONTROLLED, // flowAccessControlled(checks, sources, sinks)
        ACCESS_CONTROLLED       // accessControlled(checks, sensitiveOps)
    };

    PrimitiveExprAST(PrimitiveType type, std::vector<std::unique_ptr<ExpressionAST>> args)
        : type_(type), args_(std::move(args)) {}

    std::string toString() const override;
    std::unique_ptr<QueryResult> evaluate(QueryExecutor& executor) const override;
    PrimitiveType getPrimitiveType() const { return type_; }
    const std::vector<std::unique_ptr<ExpressionAST>>& getArgs() const { return args_; }

private:
    PrimitiveType type_;
    std::vector<std::unique_ptr<ExpressionAST>> args_;
};

class PolicyCheckAST : public PolicyAST {
public:
    PolicyCheckAST(std::unique_ptr<ExpressionAST> expr, bool shouldBeEmpty)
        : expr_(std::move(expr)), shouldBeEmpty_(shouldBeEmpty) {}

    std::string toString() const override;
    bool evaluate(QueryExecutor& executor) const override;

private:
    std::unique_ptr<ExpressionAST> expr_;
    bool shouldBeEmpty_; // true for "is empty", false for "is not empty"
};

class FunctionDefAST : public QueryAST {
public:
    FunctionDefAST(const std::string& name, std::vector<std::string> params, 
                   std::unique_ptr<ExpressionAST> body, bool isPolicy = false)
        : name_(name), params_(std::move(params)), body_(std::move(body)), isPolicy_(isPolicy) {}

    Type getType() const override { return Type::FUNCTION_DEF; }
    std::string toString() const override;
    const std::string& getName() const { return name_; }
    const std::vector<std::string>& getParams() const { return params_; }
    const ExpressionAST* getBody() const { return body_.get(); }
    bool isPolicy() const { return isPolicy_; }

private:
    std::string name_;
    std::vector<std::string> params_;
    std::unique_ptr<ExpressionAST> body_;
    bool isPolicy_;
};

/**
 * @brief Query result representing a subgraph or set of nodes/edges
 */
class QueryResult {
public:
    enum class Type {
        NODE_SET,
        EDGE_SET,
        SUBGRAPH,
        BOOLEAN,
        STRING,
        INTEGER
    };

    QueryResult(Type type) : type_(type) {}
    virtual ~QueryResult() = default;

    Type getType() const { return type_; }
    virtual std::string toString() const = 0;
    virtual bool isEmpty() const = 0;

    // Node set operations
    virtual const std::unordered_set<Node*>& getNodes() const;
    virtual void addNode(Node* node);
    virtual void removeNode(Node* node);
    virtual bool containsNode(Node* node) const;

    // Edge set operations  
    virtual const std::unordered_set<Edge*>& getEdges() const;
    virtual void addEdge(Edge* edge);
    virtual void removeEdge(Edge* edge);
    virtual bool containsEdge(Edge* edge) const;

    // Set operations
    virtual std::unique_ptr<QueryResult> unionWith(const QueryResult& other) const;
    virtual std::unique_ptr<QueryResult> intersectWith(const QueryResult& other) const;
    virtual std::unique_ptr<QueryResult> differenceWith(const QueryResult& other) const;

protected:
    Type type_;
};

class NodeSetResult : public QueryResult {
public:
    NodeSetResult() : QueryResult(Type::NODE_SET) {}
    NodeSetResult(const std::unordered_set<Node*>& nodes) 
        : QueryResult(Type::NODE_SET), nodes_(nodes) {}

    std::string toString() const override;
    bool isEmpty() const override;
    const std::unordered_set<Node*>& getNodes() const override { return nodes_; }
    void addNode(Node* node) override;
    void removeNode(Node* node) override;
    bool containsNode(Node* node) const override;
    std::unique_ptr<QueryResult> unionWith(const QueryResult& other) const override;
    std::unique_ptr<QueryResult> intersectWith(const QueryResult& other) const override;
    std::unique_ptr<QueryResult> differenceWith(const QueryResult& other) const override;

private:
    std::unordered_set<Node*> nodes_;
};

class EdgeSetResult : public QueryResult {
public:
    EdgeSetResult() : QueryResult(Type::EDGE_SET) {}
    EdgeSetResult(const std::unordered_set<Edge*>& edges) 
        : QueryResult(Type::EDGE_SET), edges_(edges) {}

    std::string toString() const override;
    bool isEmpty() const override;
    const std::unordered_set<Edge*>& getEdges() const override { return edges_; }
    void addEdge(Edge* edge) override;
    void removeEdge(Edge* edge) override;
    bool containsEdge(Edge* edge) const override;
    std::unique_ptr<QueryResult> unionWith(const QueryResult& other) const override;
    std::unique_ptr<QueryResult> intersectWith(const QueryResult& other) const override;
    std::unique_ptr<QueryResult> differenceWith(const QueryResult& other) const override;

private:
    std::unordered_set<Edge*> edges_;
};

class BooleanResult : public QueryResult {
public:
    BooleanResult(bool value) : QueryResult(Type::BOOLEAN), value_(value) {}

    std::string toString() const override;
    bool isEmpty() const override;
    bool getValue() const { return value_; }

private:
    bool value_;
};

class StringResult : public QueryResult {
public:
    StringResult(const std::string& value) : QueryResult(Type::STRING), value_(value) {}

    std::string toString() const override;
    bool isEmpty() const override;
    const std::string& getValue() const { return value_; }

private:
    std::string value_;
};

class IntegerResult : public QueryResult {
public:
    IntegerResult(int64_t value) : QueryResult(Type::INTEGER), value_(value) {}

    std::string toString() const override;
    bool isEmpty() const override;
    int64_t getValue() const { return value_; }

private:
    int64_t value_;
};

/**
 * @brief Query executor that evaluates queries against a PDG
 */
class QueryExecutor {
public:
    QueryExecutor(ProgramGraph& pdg) : pdg_(pdg) {}

    // Variable binding
    void bindVariable(const std::string& name, std::unique_ptr<QueryResult> value);
    std::unique_ptr<QueryResult> getVariable(const std::string& name);
    bool hasVariable(const std::string& name) const;

    // Function definitions
    void defineFunction(const std::string& name, std::unique_ptr<FunctionDefAST> func);
    std::unique_ptr<FunctionDefAST> getFunction(const std::string& name);
    bool hasFunction(const std::string& name) const;

    // Primitive operations
    std::unique_ptr<QueryResult> forwardSlice(const std::unordered_set<Node*>& startNodes, int depth = -1);
    std::unique_ptr<QueryResult> backwardSlice(const std::unordered_set<Node*>& startNodes, int depth = -1);
    std::unique_ptr<QueryResult> shortestPath(const std::unordered_set<Node*>& from, const std::unordered_set<Node*>& to);
    std::unique_ptr<QueryResult> selectEdges(EdgeType edgeType);
    std::unique_ptr<QueryResult> selectNodes(GraphNodeType nodeType);
    std::unique_ptr<QueryResult> findPCNodes(const std::unordered_set<Node*>& exprNodes, EdgeType edgeType);
    std::unique_ptr<QueryResult> removeControlDeps(const std::unordered_set<Node*>& nodes);
    std::unique_ptr<QueryResult> returnsOf(const std::string& procName);
    std::unique_ptr<QueryResult> formalsOf(const std::string& procName);
    std::unique_ptr<QueryResult> entriesOf(const std::string& procName);
    std::unique_ptr<QueryResult> between(const std::unordered_set<Node*>& from, const std::unordered_set<Node*>& to);
    std::unique_ptr<QueryResult> declassifies(const std::unordered_set<Node*>& declassifiers, 
                                             const std::unordered_set<Node*>& sources, 
                                             const std::unordered_set<Node*>& sinks);
    std::unique_ptr<QueryResult> noExplicitFlows(const std::unordered_set<Node*>& sources, 
                                                 const std::unordered_set<Node*>& sinks);
    std::unique_ptr<QueryResult> flowAccessControlled(const std::unordered_set<Node*>& checks,
                                                      const std::unordered_set<Node*>& sources,
                                                      const std::unordered_set<Node*>& sinks);
    std::unique_ptr<QueryResult> accessControlled(const std::unordered_set<Node*>& checks,
                                                  const std::unordered_set<Node*>& sensitiveOps);

    // Utility functions
    ProgramGraph& getPDG() { return pdg_; }
    const ProgramGraph& getPDG() const { return pdg_; }

    // Clear all bindings (useful for fresh evaluation)
    void clearBindings();

private:
    ProgramGraph& pdg_;
    std::unordered_map<std::string, std::unique_ptr<QueryResult>> variables_;
    std::unordered_map<std::string, std::unique_ptr<FunctionDefAST>> functions_;
};

} // namespace pdg
