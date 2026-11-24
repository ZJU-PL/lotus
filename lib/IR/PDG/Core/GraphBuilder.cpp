#include "IR/PDG/Core/GraphBuilder.h"

#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include "IR/PDG/Support/DebugInfoUtils.h"
#include "IR/PDG/Support/PDGUtils.h"

using namespace llvm;

void pdg::GraphBuilder::build(Module &M) {
  graph_.reset();
  graph_._built_module = &M;
  // build node for global variables
  for (auto &global_var : M.getGlobalList()) {
    DIType *global_var_di_type = dbgutils::getGlobalVarDIType(global_var);
    if (global_var_di_type == nullptr)
      continue;
    GraphNodeType node_type = GraphNodeType::VAR_STATICALLOCGLOBALSCOPE;
    if (pdgutils::isStaticFuncVar(global_var, M))
      node_type = GraphNodeType::VAR_STATICALLOCFUNCTIONSCOPE;
    else if (pdgutils::isStaticGlobalVar(global_var))
      node_type = GraphNodeType::VAR_STATICALLOCMODULESCOPE;

    Node *n = new Node(global_var, node_type);
    graph_._val_node_map.insert(std::pair<Value *, Node *>(&global_var, n));
    graph_.addNode(*n);
  }

  for (auto &F : M) {
    if (F.isDeclaration() || F.empty())
      continue;
    // Create nodes for instructions in each function.
    FunctionWrapper *func_w = new FunctionWrapper(&F);
    for (auto inst_iter = inst_begin(F); inst_iter != inst_end(F);
         inst_iter++) {
      try {
        GraphNodeType node_type = GraphNodeType::INST_OTHER;
        if (isAnnotationCallInst(*inst_iter))
          node_type = GraphNodeType::ANNO_VAR;
        if (isa<ReturnInst>(&*inst_iter))
          node_type = GraphNodeType::INST_RET;
        if (isa<CallBase>(&*inst_iter))
          node_type = GraphNodeType::INST_FUNCALL;
        if (isa<BranchInst>(&*inst_iter))
          node_type = GraphNodeType::INST_BR;
        // if (auto invokeInst = dyn_cast<InvokeInst>(&*inst_iter))
        //   errs() << invokeInst->getCalledFunction()->getName() << "\n";
        Node *n = new Node(*inst_iter, node_type);
        graph_._val_node_map.insert(std::pair<Value *, Node *>(&*inst_iter, n));
        func_w->addInst(*inst_iter);
        graph_.addNode(*n);
      } catch (...) {
        // Skip invalid instructions
        errs() << "Warning: Skipping invalid instruction in function "
               << F.getName() << "\n";
        continue;
      }
    }
    func_w->buildFormalTreeForArgs();
    func_w->buildFormalTreesForRetVal();
    graph_.addFormalTreeNodesToGraph(*func_w);
    graph_.addNode(*func_w->getEntryNode());
    graph_._func_wrapper_map.insert(std::make_pair(&F, func_w));
    graph_._val_node_map.insert(
        std::pair<Value *, Node *>(&F, func_w->getEntryNode()));
  }

  buildGlobalAnnotationNodes(M);

  // handle call sites
  for (auto &F : M) {
    if (F.isDeclaration() || F.empty())
      continue;
    if (!graph_.hasFuncWrapper(F))
      continue;

    FunctionWrapper *func_w = graph_.getFuncWrapper(F);
    auto call_insts = func_w->getCallInsts();
    for (auto *ci : call_insts) {
      auto *called_func = pdgutils::getCalledFunc(*ci);
      // indirect call, insert a call wrapper first, build parameter tree lazyly
      // when actually connectting indirect call site and called function during
      // PDG construction
      if (called_func == nullptr) {
        CallWrapper *cw = new CallWrapper(*ci);
        graph_._call_wrapper_map.insert(std::make_pair(ci, cw));
        continue;
      }
      if (!graph_.hasFuncWrapper(*called_func))
        continue;
      CallWrapper *cw = new CallWrapper(*ci);
      FunctionWrapper *callee_fw = graph_.getFuncWrapper(*called_func);
      cw->buildActualTreeForArgs(*callee_fw);
      cw->buildActualTreesForRetVal(*callee_fw);
      cw->setHasParamTrees();
      graph_._call_wrapper_map.insert(std::make_pair(ci, cw));
    }
  }
  graph_._is_build = true;
}

void pdg::GraphBuilder::bindDebugInfo(Module &M) {
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    FunctionWrapper *fw = graph_.getFuncWrapper(F);
    if (!fw)
      continue;
    auto dbg_declare_insts = fw->getDbgDeclareInsts();
    // bind ditype to the top-level pointer (alloca)
    for (auto *dbg_declare_inst : dbg_declare_insts) {
      // For LLVM 14.0.0, we need to handle Metadata conversion correctly
      Metadata *MD = dbg_declare_inst->getRawLocation();
      if (!MD)
        continue;

      Value *addr = nullptr;
      if (auto *VMD = dyn_cast<ValueAsMetadata>(MD)) {
        addr = VMD->getValue();
      } else {
        continue;
      }

      Node *addr_node = graph_.getNode(*addr);
      if (!addr_node)
        continue;
      auto *DLV = dbg_declare_inst->getVariable(); // di local variable instance
      assert(DLV != nullptr &&
             "cannot find DILocalVariable Node for computing DIType");
      DIType *var_di_type = DLV->getType();
      std::string var_name = DLV->getName().str();
      assert(var_di_type != nullptr && "cannot bind nullptr ditype to node!");
      addr_node->setDIType(*var_di_type);
      graph_._node_di_type_map.insert(std::make_pair(addr_node, var_di_type));
      // if dbg contains class information, create a separate class node
      if (dbgutils::isClassPointerType(*var_di_type)) {
        auto class_name = dbgutils::getSourceLevelTypeName(*var_di_type);
        if (graph_._class_name_set.find(class_name) !=
            graph_._class_name_set.end()) {
          // if the variable name is this, the current function is a class
          // method
          if (var_name == "this" && fw->getClassName().empty())
            fw->setClassName(class_name);
          continue;
        }
        graph_._class_name_set.insert(class_name);
        Node *class_node = new Node(GraphNodeType::CLASS);
        class_node->setDIType(*var_di_type);
        graph_.addNode(*class_node);
        graph_._class_node_map.insert(std::make_pair(class_name, class_node));
      }
    }

    for (auto inst_iter = inst_begin(F); inst_iter != inst_end(F);
         inst_iter++) {
      Instruction &i = *inst_iter;
      Node *n = graph_.getNode(i);
      assert(n != nullptr && "cannot compute node di type for null node!\n");
      DIType *node_di_type = computeNodeDIType(*n);
      if (node_di_type != nullptr)
        n->setDIType(*node_di_type);
    }
  }

  for (auto &global_var : M.getGlobalList()) {
    Node *global_node = graph_.getNode(global_var);
    if (global_node != nullptr) {
      auto *dt = dbgutils::getGlobalVarDIType(global_var);
      if (dt != nullptr)
        global_node->setDIType(*dt);
    }
  }
}

/**
 * @brief Computes the DIType for a given node.
 *
 * Infers the DIType based on the node's underlying value (Alloca, Load, GEP,
 * Cast, etc.) and its relationships to other nodes (e.g., base address of a
 * GEP).
 *
 * @param n The node to compute DIType for.
 * @return The computed DIType, or nullptr if not determinable.
 */
DIType *pdg::GraphBuilder::computeNodeDIType(Node &n) {
  // local variable
  Function *func = n.getFunc();
  if (!func)
    return nullptr;
  Value *val = n.getValue();
  if (!val)
    return nullptr;

  // alloc inst
  if (isa<AllocaInst>(val))
    return n.getDIType();
  // load inst
  if (LoadInst *li = dyn_cast<LoadInst>(val)) {
    if (Instruction *load_addr =
            dyn_cast<Instruction>(li->getPointerOperand())) {
      Node *load_addr_node = graph_.getNode(*load_addr);
      if (!load_addr_node)
        return nullptr;
      DIType *load_addr_di_type = load_addr_node->getDIType();
      if (!load_addr_di_type)
        return nullptr;
      // DIType* retDIType = DIUtils::stripAttributes(sourceInstDIType);
      DIType *loaded_val_di_type = dbgutils::getBaseDIType(*load_addr_di_type);
      if (loaded_val_di_type != nullptr)
        return dbgutils::stripAttributes(*loaded_val_di_type);
      return loaded_val_di_type;
    }

    if (GlobalVariable *gv =
            dyn_cast<GlobalVariable>(li->getPointerOperand())) {
      DIType *global_var_di_type = dbgutils::getGlobalVarDIType(*gv);
      if (!global_var_di_type)
        return nullptr;
      return dbgutils::getBaseDIType(*global_var_di_type);
    }
  }
  // gep inst
  if (GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(val)) {
    Value *base_addr = gep->getPointerOperand();
    Node *base_addr_node = graph_.getNode(*base_addr);
    if (!base_addr_node)
      return nullptr;
    DIType *base_addr_di_type = base_addr_node->getDIType();
    if (!base_addr_di_type)
      return nullptr;
    DIType *base_addr_lowest_di_type =
        dbgutils::getLowestDIType(*base_addr_di_type);
    if (!base_addr_lowest_di_type)
      return nullptr;
    if (!dbgutils::isStructType(*base_addr_lowest_di_type))
      return nullptr;
    if (auto *dict = dyn_cast<DICompositeType>(base_addr_lowest_di_type)) {
      auto di_node_arr = dict->getElements();
      for (unsigned i = 0; i < di_node_arr.size(); ++i) {
        DIType *field_di_type = dyn_cast<DIType>(di_node_arr[i]);
        if (field_di_type == nullptr)
          continue;
        // assert(field_di_type != nullptr && "fail to retrive field di type
        // (computeNodeDIType)");
        if (pdgutils::isGEPOffsetMatchDIOffset(*field_di_type, *gep))
          return field_di_type;
      }
    }
  }
  // cast inst
  if (CastInst *cast_inst = dyn_cast<CastInst>(val)) {
    Value *casted_val = cast_inst->getOperand(0);
    Node *casted_val_node = graph_.getNode(*casted_val);
    if (!casted_val_node)
      return nullptr;
    return casted_val_node->getDIType();
  }

  // default
  return nullptr;
}

bool pdg::GraphBuilder::isAnnotationCallInst(Instruction &inst) {
  if (CallBase *ci = dyn_cast<CallBase>(&inst)) {
    Function *f = pdgutils::getCalledFunc(*ci);
    if (f == nullptr)
      return false;
    std::string called_func_name = f->getName().str();
    if (called_func_name == "llvm.var.annotation")
      return true;
  }
  return false;
}

void pdg::GraphBuilder::buildGlobalAnnotationNodes(Module &M) {
  auto *global_annos = M.getNamedGlobal("llvm.global.annotations");
  if (global_annos) {
    // build a node for the annotation
    Node *global_anno_node =
        new Node(*global_annos, GraphNodeType::ANNO_GLOBAL);
    graph_._val_node_map.insert(
        std::pair<Value *, Node *>(global_annos, global_anno_node));
    graph_.addNode(*global_anno_node);
    auto *casted_array = cast<ConstantArray>(global_annos->getOperand(0));
    for (unsigned i = 0; i < casted_array->getNumOperands(); i++) {
      auto *casted_struct = cast<ConstantStruct>(casted_array->getOperand(i));
      if (auto *annotated_gv = dyn_cast<GlobalValue>(
              casted_struct->getOperand(0)->getOperand(0))) {
        auto *globalSenStr =
            cast<GlobalVariable>(casted_struct->getOperand(1)->getOperand(0));
        auto anno = cast<ConstantDataArray>(globalSenStr->getOperand(0))
                        ->getAsCString();
        Node *n = graph_.getNode(*annotated_gv);
        if (n == nullptr) {
          n = new Node(*annotated_gv,
                       GraphNodeType::VAR_STATICALLOCGLOBALSCOPE);
          graph_._val_node_map.insert(
              std::pair<Value *, Node *>(annotated_gv, n));
          graph_.addNode(*n);
        }
        n->addNeighbor(*global_anno_node, EdgeType::ANNO_GLOBAL);
      }
    }
  }
}
