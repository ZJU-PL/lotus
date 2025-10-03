/**
 * @file PDGUtils.cpp
 * @brief Implementation of utility functions for the Program Dependency Graph
 *
 * This file provides various utility functions used throughout the PDG system.
 * These utilities handle common operations related to LLVM IR analysis, type handling,
 * debug information extraction, and other helper functionality.
 *
 * Key functionality:
 * - Conversion functions between different PDG and LLVM types
 * - String manipulation and formatting for PDG output
 * - LLVM IR traversal and analysis helpers
 * - Debug information extraction and processing
 * - Type analysis and handling for field-sensitive operations
 * - Call site and function analysis helpers
 *
 * These utilities simplify the implementation of the core PDG functionality
 * by providing common operations used across multiple components.
 */

#include "IR/PDG/PDGUtils.h"

using namespace llvm;

/**
 * @brief Extract the struct type from a GetElementPtr instruction
 * 
 * This function analyzes a GEP instruction to determine what struct type
 * it is accessing. It looks at the base pointer operand and extracts the
 * struct type if the base pointer points to a struct.
 * 
 * @param gep The GetElementPtr instruction to analyze
 * @return Pointer to the struct type, or nullptr if not a struct access
 */
StructType *pdg::pdgutils::getStructTypeFromGEP(GetElementPtrInst &gep)
{
  Value *baseAddr = gep.getPointerOperand();
  if (baseAddr->getType()->isPointerTy())
  {
    if (StructType *struct_type = dyn_cast<StructType>(baseAddr->getType()->getPointerElementType()))
      return struct_type;
  }
  return nullptr;
}

/**
 * @brief Calculate the bit offset of a struct field accessed by a GEP instruction
 * 
 * This function computes the bit offset of a struct field that is being accessed
 * by a GetElementPtr instruction. It uses the module's data layout to determine
 * the actual memory layout of the struct and calculates the offset in bits.
 * 
 * @param M The LLVM module containing the GEP instruction
 * @param struct_type The struct type being accessed
 * @param gep The GetElementPtr instruction accessing the struct field
 * @return The bit offset of the accessed field, or INT_MIN on error
 */
uint64_t pdg::pdgutils::getGEPOffsetInBits(Module& M, StructType &struct_type, GetElementPtrInst &gep)
{
  // Get the accessed struct member offset from the gep instruction
  int gep_offset = getGEPAccessFieldOffset(gep);
  if (gep_offset == INT_MIN)
    return INT_MIN;
  
  // Use the struct layout to figure out the offset in bits
  auto const &data_layout = M.getDataLayout();
  auto const struct_layout = data_layout.getStructLayout(&struct_type);
  
  // Check for out-of-bounds access
  if (gep_offset >= struct_type.getNumElements())
  {
    errs() << "dubious gep access outof bound: " << gep << " in func " << gep.getFunction()->getName() << "\n";
    return INT_MIN;
  }
  
  uint64_t field_bit_offset = struct_layout->getElementOffsetInBits(gep_offset);
  // check if the gep may be used for accessing bit fields
  // if (isGEPforBitField(gep))
  // {
  //   // compute the accessed bit offset here
  //   if (auto LShrInst = dyn_cast<LShrOperator>(getLShrOnGep(gep)))
  //   {
  //     auto LShrOffsetOp = LShrInst->getOperand(1);
  //     if (ConstantInt *constInst = dyn_cast<ConstantInt>(LShrOffsetOp))
  //     {
  //       fieldOffsetInBits += constInst->getSExtValue();
  //     }
  //   }
  // }
  return field_bit_offset;
}

/**
 * @brief Extract the field offset from a GetElementPtr instruction
 * 
 * This function analyzes the operands of a GEP instruction to determine
 * which field of a struct is being accessed. It looks at the constant
 * indices in the GEP operands to compute the field offset.
 * 
 * @param gep The GetElementPtr instruction to analyze
 * @return The field offset (0-based index), or INT_MIN on error
 */
int pdg::pdgutils::getGEPAccessFieldOffset(GetElementPtrInst &gep)
{
  int operand_num = gep.getNumOperands();
  Value *last_idx = gep.getOperand(operand_num - 1);
  // cast the last_idx to int type
  if (ConstantInt *constInt = dyn_cast<ConstantInt>(last_idx))
  {
    auto access_idx = constInt->getSExtValue();
    if (access_idx < 0)
      return INT_MIN;
    return access_idx;
  }
  return INT_MIN;
}

bool pdg::pdgutils::isGEPOffsetMatchDIOffset(DIType &dt, GetElementPtrInst &gep)
{
  StructType *struct_ty = getStructTypeFromGEP(gep);
  if (!struct_ty)
    return false;
  Module &module = *(gep.getFunction()->getParent());
  uint64_t gep_bit_offset = getGEPOffsetInBits(module, *struct_ty, gep);
  if (gep_bit_offset < 0)
    return false;

  // TODO:  
  // Value* lshr_op_inst = getLShrOnGep(gep);
  // if (lshr_op_inst != nullptr)
  // {
  //   if (auto lshr = dyn_cast<UnaryOperator>(lshr_op_inst))
  //   {
  //     auto shift_bits = lshr->getOperand(1);        // constant int in llvm
  //     if (ConstantInt *ci = dyn_cast<ConstantInt>(shift_bits))
  //     {
  //       gep_bit_offset += ci->getZExtValue(); // add the value as an unsigned integer
  //     }
  //   }
  // }

  uint64_t di_type_bit_offset = dt.getOffsetInBits();
  if (gep_bit_offset == di_type_bit_offset)
    return true;
  return false;
}

bool pdg::pdgutils::isNodeBitOffsetMatchGEPBitOffset(Node &n, GetElementPtrInst &gep)
{
  StructType *struct_ty = getStructTypeFromGEP(gep);
  if (struct_ty == nullptr)
    return false;
  Module &module = *(gep.getFunction()->getParent());
  uint64_t gep_bit_offset = pdgutils::getGEPOffsetInBits(module, *struct_ty, gep);
  DIType* node_di_type = n.getDIType();
  if (node_di_type == nullptr || gep_bit_offset == INT_MIN)
    return false;
  uint64_t node_bit_offset = node_di_type->getOffsetInBits();
  if (gep_bit_offset == node_bit_offset)
    return true;
  return false;
}

// a wrapper func that strip pointer casts
Function *pdg::pdgutils::getCalledFunc(CallInst &call_inst)
{
  auto called_val = call_inst.getCalledOperand();
  if (!called_val)
    return nullptr;
  if (Function *func = dyn_cast<Function>(called_val->stripPointerCasts()))
    return func;
  return nullptr;
}

// check access type
bool pdg::pdgutils::hasReadAccess(Value &v)
{
  for (auto user : v.users())
  {
    if (isa<LoadInst>(user))
      return true;
    if (auto gep = dyn_cast<GetElementPtrInst>(user))
    {
      if (gep->getPointerOperand() == &v)
        return true;
    }
  }
  return false;
}

bool pdg::pdgutils::hasWriteAccess(Value &v)
{
  for (auto user : v.users())
  {
    if (auto si = dyn_cast<StoreInst>(user))
    {
      if (!isa<Argument>(si->getValueOperand()) && si->getPointerOperand() == &v)
        return true;
    }
  }
  return false;
}

bool pdg::pdgutils::isStaticFuncVar(GlobalVariable &gv, Module &M)
{
  auto gv_name = gv.getName().str();
  if (gv_name.empty())
    return false;

  size_t pos = 0;
  pos = gv_name.find(".", pos + 1);
  if (pos != std::string::npos)
  {
    std::string func_name = gv_name.substr(0, pos);
    if (M.getFunction(func_name) != nullptr)
      return true;
  }
  return false;
}

bool pdg::pdgutils::isStaticGlobalVar(llvm::GlobalVariable &gv)
{
  return gv.hasInternalLinkage();
}

// ==== inst iterator related funcs =====

inst_iterator pdg::pdgutils::getInstIter(Instruction &i)
{
  Function* f = i.getFunction();
  for (auto inst_iter = inst_begin(f); inst_iter != inst_end(f); inst_iter++)
  {
    if (&*inst_iter == &i)
      return inst_iter;
  }
  return inst_end(f);
}

std::set<Instruction *> pdg::pdgutils::getInstructionBeforeInst(Instruction &i)
{
  Function* f = i.getFunction();
  auto stop = getInstIter(i);
  std::set<Instruction*> insts_before;
  for (auto inst_iter = inst_begin(f); inst_iter != inst_end(f); inst_iter++)
  {
    if (inst_iter == stop)
      return insts_before;
    insts_before.insert(&*inst_iter);
  }
  return insts_before;
}

std::set<Instruction *> pdg::pdgutils::getInstructionAfterInst(Instruction &i)
{
  Function* f = i.getFunction();
  std::set<Instruction*> insts_after;
  auto start = getInstIter(i);
  if (start == inst_end(f))
    return  insts_after;
  start++;
  for (auto inst_iter = start; inst_iter != inst_end(f); inst_iter++)
  {
    insts_after.insert(&*inst_iter);
  }
  return insts_after;
}

std::set<Value *> pdg::pdgutils::computeAddrTakenVarsFromAlloc(AllocaInst &ai)
{
  std::set<Value *> addr_taken_vars;
  for (auto user : ai.users())
  {
    if (isa<LoadInst>(user))
      addr_taken_vars.insert(user);
  }
  return addr_taken_vars;
}

void pdg::pdgutils::printTreeNodesLabel(Node *node, raw_string_ostream &OS, std::string tree_node_type_str)
{
  TreeNode *n = static_cast<TreeNode *>(node);
  int tree_node_depth = n->getDepth();
  DIType *node_di_type = n->getDIType();
  if (node_di_type == nullptr)
    return;
  std::string field_type_name = dbgutils::getSourceLevelTypeName(*node_di_type);
  OS << tree_node_type_str << " | " << tree_node_depth << " | " << field_type_name;
}

std::string pdg::pdgutils::stripFuncNameVersionNumber(std::string func_name)
{
  auto deli_pos = func_name.find('.');
  if (deli_pos == std::string::npos)
    return func_name;
  return func_name.substr(0, deli_pos);
}

std::string pdg::pdgutils::computeTreeNodeID(TreeNode &tree_node)
{
  std::string parent_type_name = "";
  std::string node_field_name = "";
  TreeNode* parent_node = tree_node.getParentNode();
  if (parent_node != nullptr)
  {
    auto parent_di_type = dbgutils::stripMemberTag(*parent_node->getDIType());
    if (parent_di_type != nullptr)
      parent_type_name = dbgutils::getSourceLevelTypeName(*parent_di_type);
  }

  if (!tree_node.getDIType())
    return parent_type_name;
  DIType* node_di_type = dbgutils::stripAttributes(*tree_node.getDIType());
  node_field_name = dbgutils::getSourceLevelVariableName(*node_di_type);
  
  return (parent_type_name + node_field_name);
}

std::string pdg::pdgutils::stripVersionTag(std::string str)
{
  size_t pos = 0;
  size_t nth = 2;
  while (nth > 0)
  {
    pos = str.find('.', pos + 1);
    if (pos == std::string::npos)
      return str;
    nth--;
  }

  if (pos != std::string::npos)
    return str.substr(0, pos);
  return str;
}


Value *pdg::pdgutils::getLShrOnGep(GetElementPtrInst &gep)
{
  for (auto u : gep.users())
  {
    if (LoadInst *li = dyn_cast<LoadInst>(u))
    {
      for (auto user : li->users())
      {
        if (isa<UnaryOperator>(user))
          return user;
      }
    }
  }
  return nullptr;
}

std::string pdg::pdgutils::getNodeTypeStr(GraphNodeType node_type)
{
  switch (node_type)
  {
  case GraphNodeType::INST_FUNCALL:
    return "INST_FUNCALL";
  case GraphNodeType::INST_RET:
    return "INST_RET";
  case GraphNodeType::INST_BR:
    return "INST_BR";
  case GraphNodeType::INST_OTHER:
    return "INST_OTHER";
  case GraphNodeType::FUNC_ENTRY:
    return "FUNC_ENTRY";
  case GraphNodeType::PARAM_FORMALIN:
    return "PARAM_FORMALIN";
  case GraphNodeType::PARAM_FORMALOUT:
    return "PARAM_FORMALOUT";
  case GraphNodeType::PARAM_ACTUALIN:
    return "PARAM_ACTUALIN";
  case GraphNodeType::PARAM_ACTUALOUT:
    return "PARAM_ACTUALOUT";
  case GraphNodeType::VAR_STATICALLOCGLOBALSCOPE:
    return "VAR_STATICALLOCGLOBALSCOPE";
  case GraphNodeType::VAR_STATICALLOCMODULESCOPE:
    return "VAR_STATICALLOCMODULESCOPE";
  case GraphNodeType::VAR_STATICALLOCFUNCTIONSCOPE:
    return "VAR_STATICALLOCFUNCTIONSCOPE";
  case GraphNodeType::VAR_OTHER:
    return "VAR_OTHER";
  case GraphNodeType::FUNC:
    return "FUNC";
  case GraphNodeType::ANNO_VAR:
    return "ANNO_VAR";
  case GraphNodeType::ANNO_GLOBAL:
    return "ANNO_GLOBAL";
  case GraphNodeType::ANNO_OTHER:
    return "ANNO_OTHER";
  default:
    break;
  }
  return "";
}

std::string pdg::pdgutils::getEdgeTypeStr(EdgeType edge_type)
{
  switch (edge_type)
  {
  case EdgeType::IND_CALL:
    return "IND_CALL";
  case EdgeType::CONTROLDEP_CALLINV:
    return "CONTROLDEP_CALLINV";
  case EdgeType::CONTROLDEP_ENTRY:
    return "CONTROLDEP_ENTRY";
  case EdgeType::CONTROLDEP_BR:
    return "CONTROLDEP_BR";
  case EdgeType::CONTROLDEP_IND_BR:
    return "CONTROLDEP_IND_BR";
  case EdgeType::DATA_DEF_USE:
    return "DATA_DEF_USE";
  case EdgeType::DATA_RAW:
    return "DATA_RAW";
  case EdgeType::DATA_READ:
    return "DATA_READ";
  case EdgeType::DATA_ALIAS:
    return "DATA_ALIAS";
  case EdgeType::DATA_RET:
    return "DATA_RET";
  case EdgeType::PARAMETER_IN:
    return "PARAMETER_IN";
  case EdgeType::PARAMETER_OUT:
    return "PARAMETER_OUT";
  case EdgeType::PARAMETER_FIELD:
    return "PARAMETER_FIELD";
  case EdgeType::GLOBAL_DEP:
    return "GLOBAL_DEP";
  case EdgeType::VAL_DEP:
    return "VAL_DEP";
  case EdgeType::ANNO_VAR:
    return "ANNO_VAR";
  case EdgeType::ANNO_GLOBAL:
    return "ANNO_GLOBAL";
  case EdgeType::ANNO_OTHER:
    return "ANNO_OTHER";
  case EdgeType::TYPE_OTHEREDGE:
    return "TYPE_OTHEREDGE";
  default:
    break;
  }
  return "";
}

std::string& pdg::pdgutils::rtrim(std::string& s, const char* t)
{
    s.erase(s.find_last_not_of(t) + 1);
    return s;
}