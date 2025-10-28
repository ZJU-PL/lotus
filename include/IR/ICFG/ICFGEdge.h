/// @file ICFGEdge.h
/// @brief ICFG edge types for control flow connections.

#pragma once
 
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Analysis/CFG.h>
 
//#include <iostream>
 
#include "LLVMUtils/GenericGraph.h"
 

 
class ICFGNode;

/// @brief Base class for interprocedural control-flow edges.
///
/// Represents control flow connections between ICFG nodes, including
/// intraprocedural (within functions), call, and return edges.
 typedef GenericEdge<ICFGNode> GenericICFGEdgeTy;
 class ICFGEdge : public GenericICFGEdgeTy
 {
 
public:
    /// @brief Edge kinds for different control flow types.
    enum ICFGEdgeK
    {
        IntraCF,  ///< Intraprocedural control flow
        CallCF,   ///< Call edge (caller -> callee entry)
        RetCF,    ///< Return edge (callee exit -> caller)
    };

public:
    /// @brief Constructs an ICFG edge.
    /// @param s Source node.
    /// @param d Destination node.
    /// @param k Edge kind.
    ICFGEdge(ICFGNode* s, ICFGNode* d, ICFGEdgeK k) : GenericICFGEdgeTy(s, d, k)
    {
    }
    
    /// @brief Destructor.
    ~ICFGEdge()
    {
    }

    /// @brief Checks if this is a control flow edge.
    /// @return True for all ICFG edges.
    inline bool isCFGEdge() const
    {
        return getEdgeKind() == IntraCF || getEdgeKind() == CallCF || getEdgeKind() == RetCF;
    }
    
    /// @brief Checks if this is a call edge.
    /// @return True if this edge represents a function call.
    inline bool isCallCFGEdge() const
    {
        return getEdgeKind() == CallCF;
    }
    
    /// @brief Checks if this is a return edge.
    /// @return True if this edge represents a function return.
    inline bool isRetCFGEdge() const
    {
        return getEdgeKind() == RetCF;
    }
    
    /// @brief Checks if this is an intraprocedural edge.
    /// @return True if this edge is within a single function.
    inline bool isIntraCFGEdge() const
    {
        return getEdgeKind() == IntraCF;
    }

    /// @brief Stream operator for printing edge information.
    friend llvm::raw_ostream& operator<< (llvm::raw_ostream &o, const ICFGEdge &edge)
    {
        o << edge.toString();
        return o;
    }

    /// @brief Returns a string representation of this edge.
    /// @return String description.
    virtual std::string toString() const;
 };
 
/// @brief Intraprocedural control flow edge within a function.
///
/// Connects basic blocks within the same function.
class IntraCFGEdge : public ICFGEdge
{

public:
    /// @brief Constructs an intraprocedural edge.
    /// @param s Source node.
    /// @param d Destination node.
    IntraCFGEdge(ICFGNode* s, ICFGNode* d): ICFGEdge(s,d,IntraCF)
    {
    }
    
    /// @brief Type inquiry support for LLVM-style RTTI.
    static inline bool classof(const IntraCFGEdge *)
    {
        return true;
    }
    static inline bool classof(const ICFGEdge *edge)
    {
        return edge->getEdgeKind() == IntraCF;
    }
    static inline bool classof(const GenericICFGEdgeTy *edge)
    {
        return edge->getEdgeKind() == IntraCF;
    }

    /// @brief Returns a string representation of this intra edge.
    /// @return String description.
    virtual std::string toString() const;
};
 
/// @brief Call edge from caller to callee entry.
///
/// Represents a function call, connecting the call site to the entry of the callee.
class CallCFGEdge : public ICFGEdge
{

private:
    const llvm::Instruction*  cs;  ///< Call instruction.
public:
    /// @brief Constructs a call edge.
    /// @param s Source node (caller).
    /// @param d Destination node (callee entry).
    /// @param c Call instruction.
    CallCFGEdge(ICFGNode* s, ICFGNode* d, const llvm::Instruction*  c):
            ICFGEdge(s,d,CallCF),cs(c)
    {
    }
    
    /// @brief Returns the call instruction associated with this edge.
    /// @return Pointer to the call instruction.
    inline const llvm::Instruction*  getCallSite() const
    {
        return cs;
    }
    
    /// @brief Type inquiry support for LLVM-style RTTI.
    static inline bool classof(const CallCFGEdge *)
    {
        return true;
    }
    static inline bool classof(const ICFGEdge *edge)
    {
        return edge->getEdgeKind() == CallCF;
    }
    static inline bool classof(const GenericICFGEdgeTy *edge)
    {
        return edge->getEdgeKind() == CallCF;
    }
    
    /// @brief Returns a string representation of this call edge.
    /// @return String description.
    virtual std::string toString() const;
};
 
/// @brief Return edge from callee exit to caller.
///
/// Represents a function return, connecting the exit of the callee back to the call site.
class RetCFGEdge : public ICFGEdge
{

private:
    const llvm::Instruction*  cs;  ///< Call instruction that this return corresponds to.
public:
    /// @brief Constructs a return edge.
    /// @param s Source node (callee exit).
    /// @param d Destination node (call site).
    /// @param c Call instruction.
    RetCFGEdge(ICFGNode* s, ICFGNode* d, const llvm::Instruction*  c):
            ICFGEdge(s,d,RetCF),cs(c)
    {
    }
    
    /// @brief Returns the call instruction associated with this return.
    /// @return Pointer to the call instruction.
    inline const llvm::Instruction*  getCallSite() const
    {
        return cs;
    }
    
    /// @brief Type inquiry support for LLVM-style RTTI.
    static inline bool classof(const RetCFGEdge *)
    {
        return true;
    }
    static inline bool classof(const ICFGEdge *edge)
    {
        return edge->getEdgeKind() == RetCF;
    }
    static inline bool classof(const GenericICFGEdgeTy *edge)
    {
        return edge->getEdgeKind() == RetCF;
    }
    
    /// @brief Returns a string representation of this return edge.
    /// @return String description.
    virtual std::string toString() const;
};
 

