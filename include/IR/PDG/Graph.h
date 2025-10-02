#pragma once
#include "IR/PDG/PDGNode.h"
#include "IR/PDG/PDGEdge.h"
#include "IR/PDG/Tree.h"
#include "IR/PDG/CallWrapper.h"
#include "IR/PDG/FunctionWrapper.h"
#include "IR/PDG/PDGEnums.h"
#include "IR/PDG/PDGCommandLineOptions.h"


#include <unordered_map>
//#include <map>
#include <set>

namespace pdg
{
  class Node;
  class Edge;

  class GenericGraph
  {
  public:
    using ValueNodeMap = std::unordered_map<llvm::Value *, Node *>;
    using EdgeSet = std::set<Edge *>;
    using NodeSet = std::set<Node *>;
    ValueNodeMap::iterator val_node_map_begin() { return _val_node_map.begin(); }
    ValueNodeMap::iterator val_node_map_end() { return _val_node_map.end(); }
    GenericGraph() = default;
    NodeSet::iterator begin() { return _node_set.begin(); }
    NodeSet::iterator end() { return _node_set.end(); }
    NodeSet::iterator begin() const { return _node_set.begin(); }
    NodeSet::iterator end() const { return _node_set.end(); }
    virtual void build(llvm::Module &M) = 0;
    void addEdge(Edge &e) { _edge_set.insert(&e); }
    void addNode(Node &n) { _node_set.insert(&n); }
    Node *getNode(llvm::Value &v);
    bool hasNode(llvm::Value &v);
    int numEdge() { return _edge_set.size(); }
    int numNode() { return _val_node_map.size(); }
    void setIsBuild() { _is_build = true; }
    bool isBuild() { return _is_build; }
    void reset() { 
      _is_build = false; 
      _val_node_map.clear();
      _edge_set.clear();
      _node_set.clear();
    }
    bool canReach(pdg::Node &src, pdg::Node &dst);
    bool canReach(pdg::Node &src, pdg::Node &dst, std::set<EdgeType> exclude_edge_types);
    ValueNodeMap &getValueNodeMap() { return _val_node_map; }
    void dumpGraph();

  protected:
    ValueNodeMap _val_node_map;
    EdgeSet _edge_set;
    NodeSet _node_set;
    bool _is_build = false;
  };

  class ProgramGraph : public GenericGraph
  {
  public:
    using FuncWrapperMap = std::unordered_map<llvm::Function *, FunctionWrapper *>;
    using CallWrapperMap = std::unordered_map<llvm::CallInst *, CallWrapper *>;
    using ClassNodeMap = std::unordered_map<std::string, Node *>;
    using NodeDIMap = std::unordered_map<Node *, llvm::DIType *>;

    ProgramGraph() = default;
    ProgramGraph(const ProgramGraph &) = delete;
    ProgramGraph(ProgramGraph &&) = delete;
    ProgramGraph &operator=(const ProgramGraph &) = delete;
    ProgramGraph &operator=(ProgramGraph &&) = delete;
    static ProgramGraph &getInstance()
    {
      static ProgramGraph g{};
      return g;
    }

    FuncWrapperMap &getFuncWrapperMap() { return _func_wrapper_map; }
    CallWrapperMap &getCallWrapperMap() { return _call_wrapper_map; }
    NodeDIMap &getNodeDIMap() { return _node_di_type_map; }
    void build(llvm::Module &M) override;
    bool hasFuncWrapper(llvm::Function &F) { return _func_wrapper_map.find(&F) != _func_wrapper_map.end(); }
    bool hasCallWrapper(llvm::CallInst &ci) { return _call_wrapper_map.find(&ci) != _call_wrapper_map.end(); }
    FunctionWrapper *getFuncWrapper(llvm::Function &F) { return _func_wrapper_map[&F]; }
    CallWrapper *getCallWrapper(llvm::CallInst &ci) { return _call_wrapper_map[&ci]; }
    void bindDITypeToNodes(llvm::Module &M);
    llvm::DIType *computeNodeDIType(Node &n);
    void addTreeNodesToGraph(Tree &tree);
    void addFormalTreeNodesToGraph(FunctionWrapper &func_w);
    bool isAnnotationCallInst(llvm::Instruction &inst);
    void buildGlobalAnnotationNodes(llvm::Module &M);
    ClassNodeMap &getClassNodeMap() { return _class_node_map; }
    Node* getClassNodeByName(std::string cls_name);
    void reset() {
      GenericGraph::reset();
      _func_wrapper_map.clear();
      _call_wrapper_map.clear();
      _node_di_type_map.clear();
      _class_node_map.clear();
      _class_name_set.clear();
    }

  private:
    FuncWrapperMap _func_wrapper_map;
    CallWrapperMap _call_wrapper_map;
    NodeDIMap _node_di_type_map;
    ClassNodeMap _class_node_map;
    std::set<std::string> _class_name_set;
  };
} // namespace pdg

