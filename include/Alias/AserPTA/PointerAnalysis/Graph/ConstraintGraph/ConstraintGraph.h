//
// Created by peiming on 8/26/19.
//
#ifndef ASER_PTA_CONSTRAINTGRAPH_H
#define ASER_PTA_CONSTRAINTGRAPH_H

#include "Alias/AserPTA/PointerAnalysis/Graph/ConstraintGraph/CGObjNode.h"
#include "Alias/AserPTA/PointerAnalysis/Graph/ConstraintGraph/CGPtrNode.h"
#include "Alias/AserPTA/PointerAnalysis/Graph/GraphBase/GraphBase.h"
#include "Alias/AserPTA/PointerAnalysis/Program/Program.h"
#include "Alias/AserPTA/Util/Statistics.h"

#include <llvm/Support/DOTGraphTraits.h>



#define DEBUG_TYPE "pta-cons-graph"

namespace aser {

// IMPORTANT!!
// To guarantee the correctness of the SCC detection algorithm,
// 1st, the constraint graph must append newly added super node at the **END**
// of the node vector.
// 2nd, the constraint graph should **NEVER** remove any node from
// the node vector to ensure the same NodeID always corresponding the same node.

template <typename ctx>
class ConstraintGraph : public GraphBase<CGNodeBase<ctx>, Constraints> {
public:
    using CGNodeTy = CGNodeBase<ctx>;
    struct OnNewConstraintCallBack {
        virtual void onNewConstraint(CGNodeTy *src, CGNodeTy *dst, Constraints constraint) = 0;
    };

    LOCAL_STATISTIC(NumObjNode, "Number of Object Nodes");
    LOCAL_STATISTIC(NumPtrNode, "Number of Pointer Nodes");
  LOCAL_STATISTIC(NumConstraints, "Number of Constrains");
  LOCAL_STATISTIC(NumLoadConstraints, "Number of Load Constrains");
  LOCAL_STATISTIC(NumStoreConstraints, "Number of Store Constrains");
  LOCAL_STATISTIC(NumCopyConstraints, "Number of Assign Constrains");
  LOCAL_STATISTIC(NumOffsetConstraints, "Number of Offset Constrains");
  LOCAL_STATISTIC(NumNodes, "Number of Nodes in Total");

private:
    OnNewConstraintCallBack *callBack;

public:
    inline void registerCallBack(OnNewConstraintCallBack *cb) {
        callBack = cb;
    }

    inline void unregisterCallBack() {
        callBack = nullptr;
    }

    inline CGNodeTy *operator[](NodeID id) const {
        return this->getNode(id);
    }

    inline bool addConstraints(CGNodeTy *src, CGNodeTy *dst, Constraints constraint) {
        // should not add edges to nodes that has super node
        assert(src && dst /*&& !src->hasSuperNode() && !dst->hasSuperNode()*/);
        // self-circle copy edges has no effect
        if (src == dst && constraint == Constraints::copy) {
            return false;
        }

        if (constraint == Constraints::addr_of) {
            // addr_of can only be applied between obj --addr_of-> ptr
            assert(src->getType() == CGNodeKind::ObjNode
                   && "taken addr of non-object node is not allowed");

            // This is a trick, so that the copy edge will be visited
            // during solving phase, so that we do not need to handle addr_of constraint explicitly

            // Convention! obj_id + 1 = anonymous node
            // Or we can use a map to store obj->anon, but it is absolutely much faster.
            auto anonNode = this->getNode(src->getNodeID() + 1);
            // must be an anon Node.
            assert(anonNode->getType() == CGNodeKind::PtrNode);
            assert(llvm::cast<CGPtrNode<ctx>>(anonNode)->getPointer() == nullptr);

            if (anonNode->insertConstraint(dst, Constraints::copy)) {
                if (callBack) {
                    callBack->onNewConstraint(anonNode, dst, Constraints::copy);
                }
                return true;
            }
            return false;
        } else {
            bool newEdge = src->insertConstraint(dst, constraint);
            if (callBack && newEdge) {
              callBack->onNewConstraint(src, dst, constraint);
            }
          if (newEdge) {
            switch (constraint) {
            case Constraints::load:
              NumLoadConstraints++;
              NumConstraints++;
              break;
            case Constraints::store:
              NumStoreConstraints++;
              NumConstraints++;
              break;
            case Constraints::copy:
              NumCopyConstraints++;
              NumConstraints++;
              break;
            case Constraints::offset:
              NumOffsetConstraints++;
              NumConstraints++;
              break;
            default:
              break;
            }
          }
            return newEdge;
        }
    }

    // NOTE: THIS FUNCTION DOES NOT TAKE CARE OF POINT-TO SET
    // collapse the scc to the given super node
    void collapseSCCTo(const std::vector<CGNodeTy *> &scc, CGNodeTy *superNode) {
        // collapse the node in the scc
        using edgeTy = std::pair<Constraints , CGNodeTy *>;

        for (CGNodeTy *node : scc) {
            assert(!node->hasSuperNode());
        }

        for (CGNodeTy *node : scc) {
            if (node == superNode) {
                continue;
            }

            auto pred_it = node->pred_edge_begin();
            auto pred_ie = node->pred_edge_end();
            for (; pred_ie != pred_it; pred_it++) {
                edgeTy edge = *pred_it;

                // merge all the incoming edge
                edge.second->insertConstraint(superNode, edge.first);
            }

            auto succ_it = node->succ_edge_begin();
            auto succ_ie = node->succ_edge_end();
            for (; succ_ie != succ_it; succ_it++) {
                edgeTy edge = *succ_it;

                // merge all the outgoing edge
                superNode->insertConstraint(edge.second, edge.first);
            }

            superNode->childNodes |= node->childNodes;
            superNode->indirectNodes.insert(node->indirectNodes.begin(), node->indirectNodes.end());

            node->indirectNodes.clear();
            node->childNodes.clear(); // release the memory
            node->clearConstraints();

            node->setSuperNode(superNode); // set the supernode
        }
    }

    // IMPORTANT: does not guaranttee that the same node will not be added twice
    // caller should enforce it
    template <typename Node, typename PT, typename... Args>
    inline Node *addCGNode(Args &&... args) {
        auto node = this->template addNewNode<Node>(std::forward<Args>(args)...);
        PT::onNewNodeCreation(node->getNodeID());

        if (node->getType() == CGNodeKind::ObjNode) {
            NumObjNode++;
            // create an anon node which take the address of the node
            // Convention! objnode_id + 1 = anonomyous node
            CGPtrNode<ctx> *anonNode = addCGNode<CGPtrNode<ctx>, PT>();
            node->insertConstraint(anonNode, Constraints::addr_of);
            // Anonmyous Node can points to the object
            PT::insert(anonNode->getNodeID(), node->getNodeID());
        } else {
            NumPtrNode++;
        }
        NumNodes ++;
        return node;
    }

    ConstraintGraph() : GraphBase<CGNodeBase<ctx>, Constraints>(),
                        callBack(nullptr) {};
};

}  // namespace aser

namespace llvm {

template <typename ctx>
struct GraphTraits<const aser::ConstraintGraph<ctx>>
    : public GraphTraits<const aser::GraphBase<aser::CGNodeBase<ctx>, aser::Constraints>> {};

template <typename ctx>
struct GraphTraits<aser::ConstraintGraph<ctx>>
    : public GraphTraits<aser::GraphBase<aser::CGNodeBase<ctx>, aser::Constraints>> {};

// for callgraph visualization
template <typename ctx>
struct DOTGraphTraits<const aser::ConstraintGraph<ctx>> : public DefaultDOTGraphTraits {
    using GraphTy = aser::ConstraintGraph<ctx>;
    using NodeTy = aser::CGNodeBase<ctx>;

    explicit DOTGraphTraits(bool simple = false) : DefaultDOTGraphTraits(simple) {}

    static std::string getGraphName(const GraphTy &) { return "constraint_graph"; }

    /// Return function name;
    static std::string getNodeLabel(const NodeTy *node, const GraphTy &graph) {
        std::string str;
        raw_string_ostream os(str);

        os << node->toString() << "\n";
        return os.str();
    }

    static std::string getNodeAttributes(const NodeTy *node, const GraphTy &graph) {
        //        if (isa<CGPtrNode<ctx>>(node)) {
        //            return "";
        //        } else if (isa<CGSuperNode<ctx, PtsTy>>(node)) {
        //            return "color=red";
        //        } else {
        //            return "color=green";
        //        }
        return "";
    }

    static bool isNodeHidden(const NodeTy *node, const GraphTy &graph) {
        return false;  // Show all nodes by default
    }

    template <typename EdgeIter>
    static std::string getEdgeAttributes(const NodeTy *node, EdgeIter EI, const GraphTy &graph) {
        aser::Constraints edgeTy = (*EI).first;
        switch (edgeTy) {
            case aser::Constraints::load:
                return "color=red";
            case aser::Constraints::store:
                return "color=blue";
            case aser::Constraints::copy:
                break;
            case aser::Constraints::addr_of:
                return "color=green";
            case aser::Constraints::offset:
                return "color=purple";
        }
        return "";
    }
};

}  // namespace llvm

#undef DEBUG_TYPE

#endif