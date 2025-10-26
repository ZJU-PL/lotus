 #include <iostream>

 #include "IR/ICFG/ICFG.h"
 
 using namespace llvm;
 
 //
 //=============================================================================
 // ICFG Node
 //=============================================================================
 //
 
 std::string ICFGNode::toString() const {
     std::string str;
     raw_string_ostream rawstr(str);
     rawstr << "ICFGNode ID: " << getId();
     return rawstr.str();
 }
 
 void ICFGNode::dump() const {
 
     std::cout << this->toString() << "\n";
     std::cout << "OutEdges:\n";
     for (auto edge : getOutEdges()) {
 
         std::cout << "\t" << edge->toString() << "\n";
     }
     std::cout << "InEdges:\n";
     for (auto edge : getInEdges()) {
 
         std::cout << "\t" << edge->toString() << "\n";
     }
 }
 
 std::string IntraBlockNode::toString() const {
 
     std::string str;
     raw_string_ostream rawstr(str);
     rawstr << "IntraBlockNode ID: " << getId();
     if (getBasicBlock()->hasName()) {
 
         rawstr << ", Name: " << getBasicBlock()->getName().str();
     }
 
     return rawstr.str();
 }
 
 //
 //=============================================================================
 // ICFG Edge
 //=============================================================================
 //
 
 std::string ICFGEdge::toString() const {
     std::string str;
     raw_string_ostream rawstr(str);
     rawstr << "ICFGEdge: [" << getDstID() << "<--" << getSrcID() << "]\t";
     return rawstr.str();
 }
 
 std::string IntraCFGEdge::toString() const {
     std::string str;
     raw_string_ostream rawstr(str);
     rawstr << "IntraCFGEdge: [" << getDstID() << "<--" << getSrcID() << "]\t";
 
     return rawstr.str();
 }
 
 std::string CallCFGEdge::toString() const {
     std::string str;
     raw_string_ostream rawstr(str);
     rawstr << "CallCFGEdge " << " [";
     rawstr << getDstID() << "<--" << getSrcID() << "]\t CallSite: " << *cs << "\t";
     return rawstr.str();
 }
 
 std::string RetCFGEdge::toString() const {
     std::string str;
     raw_string_ostream rawstr(str);
     rawstr << "RetCFGEdge " << " [";
     rawstr << getDstID() << "<--" << getSrcID() << "]\t CallSite: " << *cs << "\t";
     return rawstr.str();
 }
 
 //
 //=============================================================================
 // ICFG
 //=============================================================================
 //
 
 /*!
  * Constructor
  *  * Build ICFG
  * 1) build ICFG nodes
  *    statements for top level pointers 
  * 2) connect ICFG edges
  *    between two statements 
  */
 ICFG::ICFG(): totalICFGNode(0) {}
 
 /*!
  * Whether we has an intra ICFG edge
  */
 ICFGEdge* ICFG::hasIntraICFGEdge(ICFGNode* src, ICFGNode* dst, ICFGEdge::ICFGEdgeK kind)
 {
     ICFGEdge edge(src, dst, kind);
     ICFGEdge* outEdge = src->hasOutgoingEdge(&edge);
     ICFGEdge* inEdge = dst->hasIncomingEdge(&edge);
     if (outEdge && inEdge)
     {
         assert(outEdge == inEdge && "edges not match");
         return outEdge;
     }
     else
         return nullptr;
 }
 
 /*!
  * Whether we has an inter ICFG edge
  */
 ICFGEdge* ICFG::hasInterICFGEdge(ICFGNode* src, ICFGNode* dst, ICFGEdge::ICFGEdgeK kind)
 {
     ICFGEdge edge(src, dst, kind);
     ICFGEdge* outEdge = src->hasOutgoingEdge(&edge);
     ICFGEdge* inEdge = dst->hasIncomingEdge(&edge);
     if (outEdge && inEdge)
     {
         assert(outEdge == inEdge && "edges not match");
         return outEdge;
     }
     else
         return nullptr;
 }
 
 /*!
  * Return the corresponding ICFGEdge
  */
 ICFGEdge* ICFG::getICFGEdge(const ICFGNode* src, const ICFGNode* dst, ICFGEdge::ICFGEdgeK kind)
 {
     ICFGEdge * edge = nullptr;
     size_t counter = 0;
     for (auto iter = src->OutEdgeBegin(); iter != src->OutEdgeEnd(); ++iter)
     {
         if ((*iter)->getDstID() == dst->getId() && (*iter)->getEdgeKind() == kind)
         {
             counter++;
             edge = (*iter);
         }
     }
     assert(counter <= 1 && "there's more than one edge between two ICFG nodes");
     return edge;
 }
 
 /*!
  * Add intraprocedural edges between two nodes
  */
 ICFGEdge* ICFG::addIntraEdge(ICFGNode* srcNode, ICFGNode* dstNode)
 {
     checkIntraEdgeParents(srcNode, dstNode);
     if(ICFGEdge* edge = hasIntraICFGEdge(srcNode, dstNode, ICFGEdge::IntraCF))
     {
         assert(edge->isIntraCFGEdge() && "this should be an intra CFG edge!");
         return nullptr;
     }
     else
     {
         IntraCFGEdge* intraEdge = new IntraCFGEdge(srcNode,dstNode);
         return (addICFGEdge(intraEdge) ? intraEdge : nullptr);
     }
 }
 
 /*!
  * Add interprocedural call edges between two nodes
  */
 ICFGEdge* ICFG::addCallEdge(ICFGNode* srcNode, ICFGNode* dstNode, const llvm::Instruction*  cs)
 {
     if(ICFGEdge* edge = hasInterICFGEdge(srcNode,dstNode, ICFGEdge::CallCF))
     {
         assert(edge->isCallCFGEdge() && "this should be a call CFG edge!");
         return nullptr;
     }
     else
     {
         CallCFGEdge* callEdge = new CallCFGEdge(srcNode,dstNode,cs);
         return (addICFGEdge(callEdge) ? callEdge : nullptr);
     }
 }
 
 /*!
  * Add interprocedural return edges between two nodes
  */
 ICFGEdge* ICFG::addRetEdge(ICFGNode* srcNode, ICFGNode* dstNode, const llvm::Instruction*  cs)
 {
     if(ICFGEdge* edge = hasInterICFGEdge(srcNode,dstNode, ICFGEdge::RetCF))
     {
         assert(edge->isRetCFGEdge() && "this should be a return CFG edge!");
         return nullptr;
     }
     else
     {
         RetCFGEdge* retEdge = new RetCFGEdge(srcNode,dstNode,cs);
         return (addICFGEdge(retEdge) ? retEdge : nullptr);
     }
 }
 
 bool ICFG::hasIntraBlockNode(const llvm::BasicBlock* bb) {
 
     IntraBlockNode* node = getIntraBlockICFGNode(bb);
     return node != nullptr;
 }
 
 IntraBlockNode* ICFG::getIntraBlockNode(const llvm::BasicBlock* bb) {
 
     IntraBlockNode* node = getIntraBlockICFGNode(bb);
     if(node == nullptr)
         node = addIntraBlockICFGNode(bb);
     return node;
 }
 