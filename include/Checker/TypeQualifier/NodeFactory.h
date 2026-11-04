//
// Created by ubuntu on 2/1/18.
//

#pragma once

#include "llvm/Analysis/ValueTracking.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Value.h>

// #include "IntGlobal.h"
#include "Checker/TypeQualifier/Helper.h"
#include "Checker/TypeQualifier/IntGlobal.h"
#include "Checker/TypeQualifier/PtsSet.h"
#include "Checker/TypeQualifier/StructAnalyzer.h"
////#include "Checker/TypeQualifier/UBIAnalysis.h"

#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

// List propagation: propagate callee's arg lists to caller's arg lists
// #define ListProp

// AndersNode class - This class is used to represent a node in the constraint
// graph. Due to various optimizations, it is not always the case that there is
// always a mapping from a Node to a Value. (In particular, we add artificial
// Node's that represent the set of pointed-to variables shared for each
// location equivalent Node. Ordinary clients are not allowed to create
// AndersNode objects. To guarantee index consistency, AndersNodes (and its
// subclasses) instances should only be created through AndersNodeFactory.
using NodeIndex = unsigned;
using FuncMap = std::unordered_map<std::string, llvm::Function *>;
using GObjMap = std::unordered_map<std::string, llvm::GlobalVariable *>;

class AndersNode {
public:
  enum AndersNodeType { VALUE_NODE, OBJ_NODE };

private:
  AndersNodeType type;
  int q;
  bool isUnOrArrObj;
  bool isArgNode;
  bool isHeapNode;
  NodeIndex idx, mergeTarget;
  const llvm::Value *value;
  const unsigned offset;
  int storeFlag;
  bool mayNull;
  std::string name;

  std::set<std::string> whiteList;
  std::set<std::string> blackList;
  AndersNode(AndersNodeType t, unsigned i, const llvm::Value *v = NULL,
             const unsigned off = 0, const bool _isUnionObj = false,
             const bool _isArgNode = false, bool _isHeapNode = false,
             int _storeFlag = 0, bool _mayNull = false)
      : type(t), isUnOrArrObj(_isUnionObj), isArgNode(_isArgNode),
        isHeapNode(_isHeapNode), idx(i), mergeTarget(i), value(v), offset(off),
        storeFlag(_storeFlag), mayNull(_mayNull) {}

public:
  NodeIndex getIndex() const { return idx; }
  const llvm::Value *getValue() const { return value; }
  unsigned getOffset() const { return offset; }
  void setUnOrArrNode(bool _isUnOrArrObj) { isUnOrArrObj = _isUnOrArrObj; }
  // const bool isUnOrArrNode(){return isUnOrArrObj;};

  void setArgNode(bool _isArgNode) { isArgNode = _isArgNode; }
  bool isArg() const { return isArgNode; }

  void setHeapNode(bool _isHeapNode) { isHeapNode = _isHeapNode; }
  bool heapNode() const { return isHeapNode; }

  void setStoreFlag(int ch) { storeFlag = ch; }
  int getStoreFlag() const { return storeFlag; }

  bool listEmpty() { return whiteList.empty() && blackList.empty(); }
  void setWhiteList(std::set<std::string> &list) {
    whiteList.insert(list.begin(), list.end());
  }
  void setBlackList(std::set<std::string> &list) {
    blackList.insert(list.begin(), list.end());
  }
  std::set<std::string> getWhiteList() { return whiteList; }
  std::set<std::string> getBlackList() { return blackList; }

  void setName(std::string _name) { name = std::move(_name); }
  std::string getName() { return name; }
  friend class AndersNodeFactory;
};

// This is the factory class of AndersNode
// It use a vectors to hold all Nodes in the program
// Since vectors may invalidate all element pointers/iterators when resizing,
// it is impossible to return AndersNode* in public interfaces (they may get
// invalidated). Therefore, we use plain integers to represent nodes for public
// functions like createXXX and getXXX. This is ugly, but it is efficient.
class AndersNodeFactory {
public:
  using GepMap = llvm::DenseMap<std::pair<NodeIndex, unsigned>, NodeIndex>;

  // The largest unsigned int is reserved for invalid index
  static const unsigned InvalidIndex;

private:
  llvm::Module *module;
  GlobalContext *Ctx;
  // The datalayout info
  const llvm::DataLayout *dataLayout;
  // The struct info
  StructAnalyzer *structAnalyzer;
  // Global Object info
  const GObjMap *gobjMap;
  // Function info
  const FuncMap *funcMap;

  // The set of nodes
  std::vector<AndersNode> nodes;
  std::set<NodeIndex> taintedNodes;

  // Some special indices
  static const NodeIndex UniversalPtrIndex = 0;
  static const NodeIndex UniversalObjIndex = 1;
  static const NodeIndex NullPtrIndex = 2;
  static const NodeIndex NullObjectIndex = 3;
  static const NodeIndex ConstantIntIndex = 4;

  // valueNodeMap - This map indicates the AndersNode* that a particular Value*
  // corresponds to
  llvm::DenseMap<const llvm::Value *, NodeIndex> valueNodeMap;

  // ObjectNodes - This map contains entries for each memory object in the
  // program: globals, alloca's and mallocs. We are able to represent them as
  // llvm::Value* because we're modeling the heap with the simplest
  // allocation-site approach
  llvm::DenseMap<const llvm::Value *, NodeIndex> objNodeMap;

  // returnMap - This map contains an entry for each function in the
  // program that returns a ptr.
  llvm::DenseMap<const llvm::Function *, NodeIndex> returnMap;

  // varargMap - This map contains the entry used to represent all pointers
  // passed through the varargs portion of a function call for a particular
  // function.  An entry is not present in this map for functions that do not
  // take variable arguments.
  llvm::DenseMap<const llvm::Function *, NodeIndex> varargMap;

  // gepMap - This map maintains the gep-relations across value nodes.
  // The mappings are of the form <base-ptr, offset> -> gep-ptr,
  // where base-ptr is the ValueNodeIndex for nodes that created out of llvm SSA
  // variables while gep-ptr is the ValueNodeIndex for nodes that are created
  // out of GEP instructions
  GepMap gepMap;
  llvm::DenseMap<NodeIndex, std::pair<NodeIndex, unsigned>> gepNodeMap;

  // Helper functions to do GEP translation
  unsigned constGEPtoFieldNum(const llvm::ConstantExpr *expr) const;

public:
  AndersNodeFactory();

  void setModule(llvm::Module *m) { module = m; }
  void setGlobalContext(GlobalContext *_Ctx) { Ctx = _Ctx; }
  void setDataLayout(const llvm::DataLayout *d) { dataLayout = d; }
  void setStructAnalyzer(StructAnalyzer *s) { structAnalyzer = s; }
  void setGobjMap(const GObjMap *m) { gobjMap = m; }
  void setFuncMap(const FuncMap *m) { funcMap = m; }

  // Factory methods
  NodeIndex createValueNode(const llvm::Value *val = NULL);
  NodeIndex createObjectNode(const llvm::Value *val = NULL);
  NodeIndex createObjectNode(const NodeIndex base, const unsigned offset);
  NodeIndex createReturnNode(const llvm::Function *f);
  NodeIndex createVarargNode(const llvm::Function *f);

  // Map lookup interfaces (return NULL if value not found)
  NodeIndex getValueNodeFor(const llvm::Value *val);
  NodeIndex getValueNodeForConstant(const llvm::Constant *c);
  NodeIndex getObjectNodeFor(const llvm::Value *val);
  NodeIndex getObjectNodeForConstant(const llvm::Constant *c);
  NodeIndex getReturnNodeFor(const llvm::Function *f);
  NodeIndex getVarargNodeFor(const llvm::Function *f);

  // Node merge interfaces
  void mergeNode(NodeIndex n0, NodeIndex n1); // Merge n1 into n0
  NodeIndex getMergeTarget(NodeIndex n);
  NodeIndex getMergeTarget(NodeIndex n) const;

  // Pointer arithmetic
  bool isObjectNode(NodeIndex i) const {
    return (nodes.at(i).type == AndersNode::OBJ_NODE);
  }
  bool isValueNode(NodeIndex i) const {
    return (nodes.at(i).type == AndersNode::VALUE_NODE);
  }
  bool isUnOrArrObjNode(NodeIndex i) const { return nodes.at(i).isUnOrArrObj; }
  void setUnOrArrObjNode(NodeIndex i) { nodes.at(i).setUnOrArrNode(true); }

  void setNodeName(NodeIndex i, std::string name) {
    nodes.at(i).setName(std::move(name));
  }
  std::string getNodeName(NodeIndex i) { return nodes.at(i).getName(); }

  bool isArgNode(NodeIndex i) const { return nodes.at(i).isArgNode; }
  void setArgNode(NodeIndex i) { nodes.at(i).setArgNode(true); }

  bool isHeapNode(NodeIndex i) const { return nodes.at(i).isHeapNode; }
  void setHeapNode(NodeIndex i) { nodes.at(i).setHeapNode(true); }

  int getStored(NodeIndex i) const { return nodes.at(i).storeFlag; }
  void setStored(NodeIndex i, int ch) { nodes.at(i).setStoreFlag(ch); }

  void setWL(NodeIndex i, std::set<std::string> list) {
    nodes.at(i).setWhiteList(list);
  }
  void setBL(NodeIndex i, std::set<std::string> list) {
    nodes.at(i).setBlackList(list);
  }
  std::set<std::string> getWL(NodeIndex i) {
    return nodes.at(i).getWhiteList();
  }
  std::set<std::string> getBL(NodeIndex i) {
    return nodes.at(i).getBlackList();
  }
  bool listEmpty(NodeIndex i) { return nodes.at(i).listEmpty(); }

  NodeIndex getOffsetObjectNode(NodeIndex n, int offset) const {
    // llvm::errs()<<"inside getOffsetObjectNode:\n";
    if (!isObjectNode(n)) {
      llvm::errs() << "n :" << n << "\n";
      llvm::errs() << "offset :" << offset << "\n";
      for (unsigned int i = n; i <= n + offset; i++)
        dumpNode(i);
    }
    // llvm::errs()<<"n = "<<n<<", offset = "<<offset<<"\n";
    if (nodes.at(n).isUnOrArrObj)
      return n;
    assert(isObjectNode(n + offset) && ((nodes.at(n).getOffset() + offset) ==
                                        nodes.at(n + offset).getOffset()));
    // llvm::errs()<<"n+offset"<<n+offset<<"\n";
    return n + offset;
  }

  unsigned getObjectSize(NodeIndex i) const {
    assert(isObjectNode(i));
    unsigned offset = nodes.at(i).getOffset();

    while ((i + 1) < nodes.size() &&
           nodes.at(i + 1).type == AndersNode::OBJ_NODE &&
           nodes.at(i + 1).getOffset() == (offset + 1)) {
      ++i;
      ++offset;
    }
    // llvm::errs()<<"offset = "<<offset<<"\n";
    return offset + 1; // offset starts from 0
  }

  unsigned getObjectOffset(NodeIndex i) const {
    assert(isObjectNode(i));
    return nodes.at(i).getOffset();
  }

  // Special node getters
  NodeIndex getUniversalPtrNode() const { return UniversalPtrIndex; }
  NodeIndex getUniversalObjNode() const { return UniversalObjIndex; }
  NodeIndex getNullPtrNode() const { return NullPtrIndex; }
  NodeIndex getNullObjectNode() const { return NullObjectIndex; }
  NodeIndex getConstantIntNode() const { return ConstantIntIndex; }

  // Value getters
  const llvm::Value *getValueForNode(NodeIndex i) {
    const AndersNode &n = nodes.at(i);
    if (n.getValue() != nullptr)
      return n.getValue();
    else
      return nodes.at(i - n.getOffset()).getValue();
  }

  // Value remover
  void removeNodeForValue(const llvm::Value *val) { valueNodeMap.erase(val); }

  void removeNodeForObject(const llvm::Value *val) { objNodeMap.erase(val); }

  void updateNodeForObject(const llvm::Value *val, NodeIndex idx) {
    assert(isObjectNode(idx));
    objNodeMap[val] = idx;
  }

  // Size getters
  unsigned getNumNodes() const { return nodes.size(); }

  // GepMap interfaces
  GepMap::const_iterator gepmap_begin() const { return gepMap.begin(); }
  GepMap::const_iterator gepmap_end() const { return gepMap.end(); }
  void clearGepMap() { gepMap.clear(); }

  NodeIndex getObjNodeForGEPExpr(NodeIndex i) const {
    auto itr = gepNodeMap.find(i);
    if (itr == gepNodeMap.end()) {
      return InvalidIndex;
    } else {
      // base + offset
      return itr->second.first + itr->second.second;
    }
  }
  NodeIndex getObjectBound(NodeIndex i) {
    NodeIndex offset = getObjectOffset(i);
    NodeIndex objSize = getObjectSize(i);
    return i - offset + objSize;
  }
  // Taint setter
  void setNodeAsTainted(NodeIndex i);

  // For debugging purpose
  void dumpNode(NodeIndex) const;
  void dumpNode(NodeIndex, std::map<NodeIndex, AndersPtsSet> &,
                std::set<NodeIndex> &, bool) const;
  void dumpNodeInfo(std::map<NodeIndex, AndersPtsSet> &,
                    std::set<const llvm::Value *> * = nullptr) const;
  void dumpRepInfo() const;
  void dumpNodePtrSetInfo(NodeIndex index, std::map<NodeIndex, AndersPtsSet> &,
                          std::set<NodeIndex> &, bool) const;
};
