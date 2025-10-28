/// @file ICFGNode.h
/// @brief ICFG node representations for basic blocks.

#pragma once

 #include <llvm/IR/Module.h>
 #include <llvm/IR/Function.h>
 #include <llvm/Support/raw_ostream.h>
 #include <llvm/IR/IRBuilder.h>
 #include <llvm/Analysis/CFG.h>
 
 #include <iostream>
 
 #include "IR/ICFG/ICFGEdge.h"
 #include "LLVMUtils/GenericGraph.h"
 

 
class ICFGNode;

/// @brief Base class for interprocedural control-flow graph nodes.
///
/// Each node represents a program point in the ICFG (typically a basic block).
 typedef GenericNode<ICFGNode, ICFGEdge> GenericICFGNodeTy;
 
 class ICFGNode : public GenericICFGNodeTy
 {
 
 public:
     /// kinds of ICFG node
     enum ICFGNodeK
     {
         IntraBlock, FunEntryBlock, FunRetBlock
     };
 
public:
    /// @brief Constructs an ICFG node.
    /// @param i Node ID.
    /// @param k Node kind.
    ICFGNode(NodeID i, ICFGNodeK k) : GenericICFGNodeTy(i, k), _function(nullptr), _basic_block(nullptr)
    {

    }

    /// @brief Returns the function containing this node.
    /// @return Pointer to the parent function.
    virtual const llvm::Function* getFunction() const
    {
        return _function;
    }

    /// @brief Returns the basic block represented by this node.
    /// @return Pointer to the basic block.
    virtual const llvm::BasicBlock* getBasicBlock() const
    {
        return _basic_block;
    }

    /// @brief Stream operator for printing node information.
    friend llvm::raw_ostream &operator<<(llvm::raw_ostream &o, const ICFGNode &node)
    {
        o << node.toString();
        return o;
    }

    /// @brief Returns a string representation of this node.
    /// @return String description.
    virtual std::string toString() const;

    /// @brief Dumps node information to standard output.
    void dump() const;
 
 protected:
     const llvm::Function* _function;
     const llvm::BasicBlock* _basic_block;
 };
 
/// @brief ICFG node representing a basic block within a function.
///
/// This is the primary node type for intraprocedural control flow.
class IntraBlockNode : public ICFGNode
{

public:
    /// @brief Constructs an intra-block node.
    /// @param id Node ID.
    /// @param bb Basic block this node represents.
    IntraBlockNode(NodeID id, const llvm::BasicBlock* bb) : ICFGNode(id, IntraBlock)
    {
        _basic_block = bb;
        _function = bb->getParent();
    }

    /// @brief Type inquiry support for LLVM-style RTTI.
    static inline bool classof(const IntraBlockNode*)
    {
        return true;
    }

    static inline bool classof(const ICFGNode *node)
    {
        return node->getNodeKind() == IntraBlock;
    }

    static inline bool classof(const GenericICFGNodeTy *node)
    {
        return node->getNodeKind() == IntraBlock;
    }

    /// @brief Returns a string representation of this intra-block node.
    /// @return String description including block name.
    std::string toString() const;
};
 
 ///*!
 // * Function entry ICFGNode containing a set of FormalParmVFGNodes of a function
 // */
 //class FunEntryBlockNode : public ICFGNode
 //{
 //public:
 //    FunEntryBlockNode(NodeID id, const llvm::BasicBlock* bb) : ICFGNode(id, FunEntryBlock)
 //    {
 //        _basic_block = bb;
 //        _function = bb->getParent();
 //    }
 //
 //    ///Methods for support type inquiry through isa, cast, and dyn_cast:
 //    //@{
 //    static inline bool classof(const FunEntryBlockNode *)
 //    {
 //        return true;
 //    }
 //
 //    static inline bool classof(const ICFGNode *node)
 //    {
 //        return node->getNodeKind() == FunEntryBlock;
 //    }
 //
 //    static inline bool classof(const GenericICFGNodeTy *node)
 //    {
 //        return node->getNodeKind() == FunEntryBlock;
 //    }
 //    //@}
 //
 //    virtual std::string toString() const;
 //};
 //
 ///*!
 // * Function return ICFGNode containing (at most one) FormalRetVFGNodes of a function
 // */
 //class RetBlockNode : public ICFGNode
 //{
 //public:
 //    RetBlockNode(NodeID id, const llvm::BasicBlock* bb) : ICFGNode(id, FunRetBlock)
 //    {
 //        _basic_block = bb;
 //        _function = bb->getParent();
 //    }
 //
 //    ///Methods for support type inquiry through isa, cast, and dyn_cast:
 //    //@{
 //    static inline bool classof(const RetBlockNode *)
 //    {
 //        return true;
 //    }
 //
 //    static inline bool classof(const ICFGNode *node)
 //    {
 //        return node->getNodeKind() == FunRetBlock;
 //    }
 //
 //    static inline bool classof(const GenericICFGNodeTy *node)
 //    {
 //        return node->getNodeKind() == FunRetBlock;
 //    }
 //    //@}
 //
 //    virtual std::string toString() const;
 //};
 
