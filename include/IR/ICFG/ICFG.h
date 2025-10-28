/// @file ICFG.h
/// @brief Interprocedural Control-Flow Graph (ICFG) representation.
///
/// This file defines the ICFG class which extends LLVM's basic CFG to support
/// interprocedural analysis by connecting call sites to callee entry/exit points.

#pragma once
 
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Analysis/CFG.h>

//#include <iostream>

#include "IR/ICFG/ICFGEdge.h"
#include "IR/ICFG/ICFGNode.h"
#include "LLVMUtils/GenericGraph.h"

/// @brief Interprocedural Control-Flow Graph (ICFG).
///
/// Extends basic CFG with interprocedural edges (call/return) to enable
/// whole-program control flow analysis.
 typedef GenericGraph<ICFGNode,ICFGEdge> GenericICFGTy;
 class ICFG : public GenericICFGTy
 {
 
 public:
 
     typedef std::unordered_map<NodeID, ICFGNode*> ICFGNodeIDToNodeMapTy;
     typedef ICFGNodeIDToNodeMapTy::iterator iterator;
     typedef ICFGNodeIDToNodeMapTy::const_iterator const_iterator;
 
     typedef std::unordered_map<const llvm::BasicBlock*, IntraBlockNode*> blockToIntraNodeMapTy;
     typedef std::unordered_map<const llvm::Function*, IntraBlockNode*> functionToEntryIntraNodeMapTy;
 //    typedef std::unordered_map<const llvm::BasicBlock*, FunEntryBlockNode*> blockToEntryNodeMapTy;
 //    typedef std::unordered_map<const llvm::BasicBlock*, RetBlockNode*> blockToRetNodeMapTy;
 
     NodeID totalICFGNode;
 
 private:
     blockToIntraNodeMapTy blockToIntraNodeMap;
     functionToEntryIntraNodeMapTy functionToEntryIntraNodeMap;
 //    blockToEntryNodeMapTy blockToEntryNodeMap;
 //    blockToRetNodeMapTy blockToRetNodeMap;
 
public:
    /// @brief Constructs an empty ICFG.
    ICFG();

    /// @brief Destructor.
    virtual ~ICFG()
    {
    }

    /// @brief Retrieves an ICFG node by its ID.
    /// @param id Node identifier.
    /// @return Pointer to the ICFG node.
    inline ICFGNode* getICFGNode(NodeID id) const
    {
        return getGNode(id);
    }

    /// @brief Checks if an ICFG node with the given ID exists.
    /// @param id Node identifier.
    /// @return True if the node exists.
    inline bool hasICFGNode(NodeID id) const
    {
        return hasGNode(id);
    }

    /// @brief Checks if an intraprocedural edge exists between two nodes.
    /// @param src Source node.
    /// @param dst Destination node.
    /// @param kind Edge kind.
    /// @return Pointer to the edge if it exists, nullptr otherwise.
    ICFGEdge* hasIntraICFGEdge(ICFGNode* src, ICFGNode* dst, ICFGEdge::ICFGEdgeK kind);
    
    /// @brief Checks if an interprocedural edge exists between two nodes.
    /// @param src Source node.
    /// @param dst Destination node.
    /// @param kind Edge kind.
    /// @return Pointer to the edge if it exists, nullptr otherwise.
    ICFGEdge* hasInterICFGEdge(ICFGNode* src, ICFGNode* dst, ICFGEdge::ICFGEdgeK kind);

    /// @brief Retrieves an edge between two nodes.
    /// @param src Source node.
    /// @param dst Destination node.
    /// @param kind Edge kind.
    /// @return Pointer to the edge, or nullptr if not found.
    ICFGEdge* getICFGEdge(const ICFGNode* src, const ICFGNode* dst, ICFGEdge::ICFGEdgeK kind);

    /// @brief Gets the mapping from functions to their entry nodes.
    /// @return Map of function to entry node.
    inline functionToEntryIntraNodeMapTy getFunctionEntryMap()
    {
        return functionToEntryIntraNodeMap;
    }
 
public:
    /// @brief Removes an ICFG edge from the graph.
    /// @param edge Edge to remove.
    inline void removeICFGEdge(ICFGEdge* edge)
    {
        edge->getDstNode()->removeIncomingEdge(edge);
        edge->getSrcNode()->removeOutgoingEdge(edge);
        delete edge;
    }
    
    /// @brief Removes an ICFG node from the graph.
    /// @param node Node to remove.
    inline void removeICFGNode(ICFGNode* node)
    {
        removeGNode(node);
    }

    /// @brief Adds an intraprocedural edge between two nodes.
    /// @param srcNode Source node.
    /// @param dstNode Destination node.
    /// @return Pointer to the created edge, or nullptr if already exists.
    ICFGEdge* addIntraEdge(ICFGNode* srcNode, ICFGNode* dstNode);
    
    /// @brief Adds a call edge from caller to callee entry.
    /// @param srcNode Caller node.
    /// @param dstNode Callee entry node.
    /// @param cs Call instruction.
    /// @return Pointer to the created edge, or nullptr if already exists.
    ICFGEdge* addCallEdge(ICFGNode* srcNode, ICFGNode* dstNode, const llvm::Instruction* cs);
    
    /// @brief Adds a return edge from callee exit to caller.
    /// @param srcNode Callee exit node.
    /// @param dstNode Caller node.
    /// @param cs Call instruction.
    /// @return Pointer to the created edge, or nullptr if already exists.
    ICFGEdge* addRetEdge(ICFGNode* srcNode, ICFGNode* dstNode, const llvm::Instruction* cs);

    /// @brief Verifies that both nodes of an intra edge belong to the same function.
    /// @param srcNode Source node.
    /// @param dstNode Destination node.
    inline void checkIntraEdgeParents(const ICFGNode *srcNode, const ICFGNode *dstNode)
    {
        auto* srcfun = srcNode->getFunction();
        auto* dstfun = dstNode->getFunction();
        if(srcfun != nullptr && dstfun != nullptr)
        {
            assert((srcfun == dstfun) && "src and dst nodes of an intra edge should in the same function!" );
        }
    }

    /// @brief Adds an ICFG edge to the graph.
    /// @param edge Edge to add.
    /// @return True if successfully added.
    inline bool addICFGEdge(ICFGEdge* edge)
    {
        bool added1 = edge->getDstNode()->addIncomingEdge(edge);
        bool added2 = edge->getSrcNode()->addOutgoingEdge(edge);
        assert(added1 && added2 && "edge not added??");
        return true;
    }

    /// @brief Adds an ICFG node to the graph.
    /// @param node Node to add.
    virtual inline void addICFGNode(ICFGNode* node)
    {
        addGNode(node->getId(), node);
    }

    /// @brief Checks if an intra-block node exists for a basic block.
    /// @param bb Basic block.
    /// @return True if the node exists.
    bool hasIntraBlockNode(const llvm::BasicBlock* bb);
    
    /// @brief Gets or creates an intra-block node for a basic block.
    /// @param bb Basic block.
    /// @return Pointer to the ICFG node.
    IntraBlockNode* getIntraBlockNode(const llvm::BasicBlock* bb);
 
 private:
     /// Get/Add IntraBlock ICFGNode
     inline IntraBlockNode* getIntraBlockICFGNode(const llvm::BasicBlock* bb)
     {
         blockToIntraNodeMapTy::const_iterator it = blockToIntraNodeMap.find(bb);
         if (it == blockToIntraNodeMap.end())
             return nullptr;
         return it->second;
     }
     inline IntraBlockNode* addIntraBlockICFGNode(const llvm::BasicBlock* bb)
     {
         IntraBlockNode* sNode = new IntraBlockNode(totalICFGNode++, bb);
         addICFGNode(sNode);
         blockToIntraNodeMap[bb] = sNode;
 
         if (bb == &bb->getParent()->front()) {
 
             functionToEntryIntraNodeMap[bb->getParent()] = sNode;
         }
 
         return sNode;
     }
 
     /// Get/Add a function entry node
 //    inline FunEntryBlockNode* getFunEntryICFGNode(const llvm::BasicBlock* bb)
 //    {
 //        blockToEntryNodeMapTy::const_iterator it = blockToEntryNodeMap.find(bb);
 //        if (it == blockToEntryNodeMap.end())
 //            return nullptr;
 //        return it->second;
 //    }
 //    inline FunEntryBlockNode* addFunEntryICFGNode(const llvm::BasicBlock* bb)
 //    {
 //        FunEntryBlockNode* sNode = new FunEntryBlockNode(totalICFGNode++, bb);
 //        addICFGNode(sNode);
 //        blockToEntryNodeMap[bb] = sNode;
 //        return sNode;
 //    }
 //
 //    /// Get/Add a return node
 //    inline RetBlockNode* getRetICFGNode(const llvm::BasicBlock* bb)
 //    {
 //        blockToRetNodeMapTy::const_iterator it = blockToRetNodeMap.find(bb);
 //        if (it == blockToRetNodeMap.end())
 //            return nullptr;
 //        return it->second;
 //    }
 //    inline RetBlockNode* addRetICFGNode(const llvm::BasicBlock* bb)
 //    {
 //        RetBlockNode* sNode = new RetBlockNode(totalICFGNode++, bb);
 //        addICFGNode(sNode);
 //        blockToRetNodeMap[bb] = sNode;
 //        return sNode;
 //    }
 
 };
