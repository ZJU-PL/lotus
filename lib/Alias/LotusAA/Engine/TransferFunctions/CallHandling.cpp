/*
 * LotusAA - Call Instruction Transfer Functions
 * 
 * Transfer functions for call instructions: summary application, 
 * input/output linking, escaped object handling.
 */

#include "Alias/LotusAA/Engine/IntraProceduralAnalysis.h"

#include <unordered_map>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace std;

void IntraLotusAA::processUnknownLibraryCall(CallBase *call) {
  // Mark all pointer arguments as potentially modified
  // TODO: this operation may lead to imprecision in the analysis;
  // Another choicse is to treat UnknownLibraryCall a "noop" (does nothing?)
  for (unsigned i = 0; i < call->arg_size(); i++) {
    Value *arg = call->getArgOperand(i);
    if (!arg->getType()->isPointerTy())
      continue;

    processBasePointer(arg);
    
    PTResult *pt_result = findPTResult(arg, false);
    if (!pt_result)
      continue;

    PTResultIterator iter(pt_result, this);
    for (auto loc : iter) {
      loc->storeValue(LocValue::NO_VALUE, call, 0);
    }
  }
}

void IntraLotusAA::processCall(CallBase *call) {
  if (IntraLotusAAConfig::lotus_restrict_inline_depth == 0) {
    if (call->getType()->isPointerTy()) {
      addPointsTo(call, newObject(call, MemObject::CONCRETE), 0);
    }
    return;
  }

  Function *base_func = call->getParent()->getParent();
  auto callees = lotus_aa->getCallees(base_func, call);

  if (!callees) {
    processUnknownLibraryCall(call);
    return;
  }

  // Process each possible callee
  int callee_idx = 0;
  for (Function *callee : *callees) {
    if (callee_idx >= IntraLotusAAConfig::lotus_restrict_cg_size)
      break;

    if (!callee || lotus_aa->isBackEdge(base_func, callee)) {
      if (call->getType()->isPointerTy() && (size_t)callee_idx == callees->size() - 1) {
        if (!pt_results.count(call))
          addPointsTo(call, newObject(call, MemObject::CONCRETE), 0);
      }
      callee_idx++;
      continue;
    }

    IntraLotusAA *callee_result = lotus_aa->getPtGraph(callee);

    if (!callee_result || callee_result->is_considered_as_library) {
      if (call->getType()->isPointerTy() && (size_t)callee_idx == callees->size() - 1) {
        if (!pt_results.count(call))
          addPointsTo(call, newObject(call, MemObject::CONCRETE), 0);
      }
      processUnknownLibraryCall(call);
      callee_idx++;
      continue;
    }

    // Process callee summary: inputs, outputs, and escaped objects
    auto &callee_inputs = callee_result->getInputs();
    auto &callee_outputs = callee_result->getOutputs();
    auto &callee_escape = callee_result->getEscapeObjs();

    func_arg_t &arg_result = func_arg[call][callee];

    std::vector<Value *> formal_args, real_args;
    for (Argument &arg : callee->args()) {
      formal_args.push_back(&arg);
    }
    for (unsigned i = 0; i < call->arg_size(); i++) {
      real_args.push_back(call->getArgOperand(i));
    }

    processCalleeInput(callee_inputs, callee_result->inputs_func_level,
                        real_args, formal_args, call, arg_result);
    processCalleeOutput(callee_outputs, callee_escape, call, callee);

    callee_idx++;
  }
}

void IntraLotusAA::processCalleeInput(
    map<Value *, AccessPath, llvm_cmp> &callee_input,
    map<Value *, int, llvm_cmp> &/*callee_input_func_level*/,
    std::vector<Value *> &real_args, std::vector<Value *> &formal_args,
    CallBase *callsite, func_arg_t &result) {

  // (1) Collect the real arguments and link the values to pseudo-arguments
  int real_size = real_args.size();
  int formal_size = formal_args.size();
  for (int idx = 0; idx < real_size && idx < formal_size; idx++) {
    Value *formal_arg = formal_args[idx];
    Value *real_arg = real_args[idx];
    
    mem_value_item_t mem_val_item(nullptr, real_arg);
    result[formal_arg].push_back(mem_val_item);

    if (real_arg->getType()->isPointerTy()) {
      processBasePointer(real_arg);
    }
  }

  // (2) Process the side-effect inputs
  // For each input ptr->idx1->idx2->idx3
  // We first check if it is already processed
  // If it is not processed, we use the value of ptr->idx1->idx2 and compute the
  // value of ptr->idx1->idx2->idx3 If ptr->idx1->idx2 also does not exist, we
  // first use ptr->idx1 to compute ptr->idx1->idx2 We keep doing these steps
  // until the required value is computed or is a global or argument
  set<Value *, llvm_cmp> processed;
  for (auto &iter : callee_input) {
    Value *pseudo_arg = iter.first;
    if (processed.count(pseudo_arg)) {
      continue;
    }

    std::vector<Value *> parents;
    Value *parent_iter = pseudo_arg;
    while ((!processed.count(parent_iter)) &&
           (callee_input.count(parent_iter))) {
      parents.push_back(parent_iter);
      AccessPath &parent_info = callee_input[parent_iter];
      Value *parent_arg = parent_info.getParentPtr();
      parent_iter = parent_arg;
    }

    for (int i = parents.size() - 1; i >= 0; i--) {
      Value *curr_arg_val = parents[i];
      processed.insert(curr_arg_val);
      assert(callee_input.count(curr_arg_val) && "Invalid Value found");
      AccessPath &arg_info = callee_input[curr_arg_val];

      Value *parent_arg = arg_info.getParentPtr();
      int64_t offset = arg_info.getOffset();

      mem_value_t &parent_arg_values = result[parent_arg];

      if (!isPseudoInput(parent_arg)) {
        // The parent arg is a real Argument or a Global Value
        processBasePointer(parent_arg);
        if (isa<GlobalValue>(parent_arg)) {
          // Process the global values on demand
          mem_value_item_t mem_value_item(nullptr, parent_arg);
          parent_arg_values.push_back(mem_value_item);
        } else if (isa<Argument>(parent_arg)) {
          // Arguments are processed before
        } else {
          // Default
        }
      }

      refineResult(parent_arg_values);

      mem_value_t &arg_values = result[curr_arg_val];
      for (auto &parent_value_pair : parent_arg_values) {
        Value *parent_value = parent_value_pair.val;
        if (parent_value == LocValue::FREE_VARIABLE ||
            parent_value == LocValue::UNDEF_VALUE ||
            parent_value == LocValue::SUMMARY_VALUE) {
          continue;
      }

      mem_value_t tmp_values;

        if (findPTResult(parent_value) == nullptr) {
          if (isa<Argument>(parent_value)) {
            // Only when the parent value is an argument (Real Argument/ Side
            // effect input/ Output from callee), we create a new object
            Argument *parent_value_to_arg = dyn_cast<Argument>(parent_value);
            processArg(parent_value_to_arg);
          } else {
            continue;
          }
        }
        loadPtrAt(parent_value, callsite, tmp_values, true, offset);

        for (auto &tmp_val : tmp_values) {
          mem_value_item_t mem_value_item(nullptr, tmp_val.val);
          arg_values.push_back(mem_value_item);
        }
      }
    refineResult(arg_values);
    }
  }
}

std::vector<Value *> &IntraLotusAA::createPseudoOutputNodes(
    std::vector<OutputItem *> &callee_output,
    Instruction *callsite, Function *callee) {
  
  assert(!func_ret[callsite].count(callee) && "callsite already processed!!!");

  std::vector<Value *> &out_values = func_ret[callsite][callee];
  out_values.push_back(callsite);
  
  for (size_t idx = 1; idx < callee_output.size(); idx++) {
    OutputItem *output = callee_output[idx];
    Type *output_type = output->getType();
    
    // LLVM Arguments must have first-class types or be void
    Type *actual_type = output_type;
    if (!output_type->isFirstClassType() && !output_type->isVoidTy()) {
      actual_type = output_type->getPointerTo();
    }

    // LLVM doesn't allow naming void-typed values, so use empty name for void types
    string name_str;
    if (!actual_type->isVoidTy()) {
      raw_string_ostream ss(name_str);
      ss << "LPseudoCallSiteOutput_" << callsite << "_" << callee << "_#" << idx;
      ss.flush();
    }

    Argument *new_arg = new Argument(actual_type, name_str);
    out_values.push_back(new_arg);
    func_pseudo_ret_cache[new_arg] = make_pair(callsite, idx);
  }

  assert(out_values.size() == callee_output.size() &&
         "Incorrect collection of outputs");
  
  return out_values;
}

void IntraLotusAA::createEscapedObjects(
    set<MemObject *, mem_obj_cmp> &callee_escape,
    Instruction *callsite, Function *callee,
    map<Value *, MemObject *, llvm_cmp> &escape_object_map) {
  
  int escape_obj_idx = 0;

  for (MemObject *callee_escape_obj : callee_escape) {
    if (callee_escape_obj == nullptr) {
      continue;
    }

    Value *alloca_site = callee_escape_obj->getAllocSite();
    if (alloca_site == nullptr) {
      // Null Objects and Unknown Objects are not processed
      continue;
    }
    Type *obj_ptr_type = alloca_site->getType();
    
    // LLVM Arguments must have first-class types or be void
    Type *actual_type = obj_ptr_type;
    if (!obj_ptr_type->isFirstClassType() && !obj_ptr_type->isVoidTy()) {
      actual_type = obj_ptr_type->getPointerTo();
    }

    // LLVM doesn't allow naming void-typed values, so use empty name for void types
    string name_str;
    if (!actual_type->isVoidTy()) {
      raw_string_ostream ss(name_str);
      ss << "LCallSiteEscapedObject_" << callsite << "_#" << escape_obj_idx++;
      ss.flush();
    }

    Argument *new_arg = new Argument(actual_type, name_str);
    func_pseudo_ret_cache[new_arg] = make_pair(callsite, PTR_TO_ESC_OBJ);
    MemObject::ObjKind obj_kind = MemObject::CONCRETE;
    MemObject *escaped_obj_to = newObject(new_arg, obj_kind);
    addPointsTo(new_arg, escaped_obj_to, 0);
    escape_object_map[alloca_site] = escaped_obj_to;
    
    // Cache the escape mapping
    func_escape[callsite][callee][callee_escape_obj] = escaped_obj_to;
  }
}

void IntraLotusAA::linkOutputPointsToResults(
    OutputItem *output, Value *curr_output,
    map<Value *, MemObject *, llvm_cmp> &escape_object_map,
    func_arg_t &callee_func_arg,
    std::set<PTResult *> &visited) {
  
  auto &callee_point_to = output->getPseudoPointTo();
  PTResult *curr_output_pts = nullptr;
  int func_level = output->getFuncLevel();

  if (func_level == ObjectLocator::FUNC_LEVEL_UNDEFINED) {
    func_level = 0;
    output->func_level = 0;
  }

  // Link the pointer-result and the values
  for (auto &callee_point_to_item_info : callee_point_to) {
    Value *callee_point_to_item_parent_ptr = callee_point_to_item_info.getParentPtr();
    int64_t callee_point_to_item_offset = callee_point_to_item_info.getOffset();
    
    if (callee_point_to_item_parent_ptr == nullptr) {
      // Pointer pointing to null or unknown object
      curr_output_pts = curr_output_pts ? curr_output_pts : findPTResult(curr_output, true);
      curr_output_pts->add_target(MemObject::UnknownObj, callee_point_to_item_offset);
    } else if (isa<GlobalValue>(callee_point_to_item_parent_ptr)) {
      PTResult *linked_pts = processBasePointer(callee_point_to_item_parent_ptr);
      curr_output_pts = curr_output_pts ? curr_output_pts : findPTResult(curr_output, true);
      curr_output_pts->add_derived_target(linked_pts, callee_point_to_item_offset);
    } else if (escape_object_map.count(callee_point_to_item_parent_ptr)) {
      // Escaped_obj from callee
      MemObject *curr_obj = escape_object_map[callee_point_to_item_parent_ptr];
      curr_output_pts = curr_output_pts ? curr_output_pts : findPTResult(curr_output, true);
      curr_output_pts->add_target(curr_obj, callee_point_to_item_offset);
    } else {
      // The point-to object is from the analyzed function (caller function)
      if (!callee_func_arg.count(callee_point_to_item_parent_ptr))
        continue;
        
      auto &callee_arg_vals = callee_func_arg[callee_point_to_item_parent_ptr];

      if (!callee_arg_vals.empty()) {
        curr_output_pts = curr_output_pts ? curr_output_pts : findPTResult(curr_output, true);
        visited.emplace(curr_output_pts);
      }
      for (auto &arg_point_to : callee_arg_vals) {
        Value *pointer = arg_point_to.val;

        PTResult *linked_pts = processBasePointer(pointer);
        curr_output_pts->add_derived_target(linked_pts, callee_point_to_item_offset);
      }
    }
  }
}

void IntraLotusAA::linkOutputValues(
    OutputItem *output, Value *curr_output, size_t idx,
    map<Value *, MemObject *, llvm_cmp> &escape_object_map,
    func_arg_t &callee_func_arg,
    Instruction *callsite,
    std::unordered_map<PTResult *, PTResultIterator> &pt_result_cache) {
  
  if (idx == 0) {
    // idx=0 means that the real return value, which do not need special linkage
    return;
  }
  
  AccessPath output_info = output->getSymbolicInfo();
  Value *output_parent = output_info.getParentPtr();
  int64_t output_offset = output_info.getOffset();
  
  if (escape_object_map.count(output_parent)) {
    // Escaped_obj from callee
    MemObject *curr_obj = escape_object_map[output_parent];
    ObjectLocator *locator = curr_obj->findLocator(output_offset, true);
    locator->storeValue(curr_output, callsite, 0);
  } else {
    if (!callee_func_arg.count(output_parent))
      return;

    auto &callee_arg_vals = callee_func_arg[output_parent];

    if (callee_arg_vals.empty() && isa<GlobalValue>(output_parent)) {
      mem_value_item_t global_value(nullptr, output_parent);
      callee_arg_vals.push_back(global_value);
    }

    for (auto &arg_point_to : callee_arg_vals) {
      Value *pointer = arg_point_to.val;
      if (pointer == LocValue::FREE_VARIABLE) {
        continue;
      }

      PTResult *pt_res = findPTResult(pointer);
      if (pt_res == nullptr) {
        if (isa<Argument>(pointer)) {
          Argument *parent_value_to_arg = dyn_cast<Argument>(pointer);
          pt_res = processArg(parent_value_to_arg);
        } else if (isa<GlobalValue>(pointer)) {
          GlobalValue *global = dyn_cast<GlobalValue>(pointer);
          pt_res = processGlobal(global);
        } else {
          continue;
        }
      }

      if (!pt_result_cache.count(pt_res)) {
        PTResultIterator pt_iter(pt_res, this);
        pt_result_cache.emplace(pt_res, std::move(pt_iter));
      }

      for (auto loc : pt_result_cache.at(pt_res)) {
        ObjectLocator *revised_locator = loc->offsetBy(output_offset);
        revised_locator->storeValue(curr_output, callsite, 0);
      }
    }
  }
}

void IntraLotusAA::processCalleeOutput(
    std::vector<OutputItem *> &callee_output,
    set<MemObject *, mem_obj_cmp> &callee_escape,
    Instruction *callsite, Function *callee) {

  auto &func_arg_all = func_arg[callsite];

  if (!func_arg_all.count(callee)) {
    // Inputs for callee function is not processed
    return;
  }

  func_arg_t &callee_func_arg = func_arg_all[callee];

  // (1) Create pseudo-nodes for return value and the side-effect outputs
  std::vector<Value *> &out_values = createPseudoOutputNodes(callee_output, callsite, callee);

  // (2) Create the objects that escape to this caller function
  map<Value *, MemObject *, llvm_cmp> escape_object_map;
  createEscapedObjects(callee_escape, callsite, callee, escape_object_map);

  // (3) Link the point-to results and values for each output
  std::set<PTResult *> visited;
  std::unordered_map<PTResult *, PTResultIterator> pt_result_cache;
  
  for (size_t idx = 0; idx < callee_output.size(); idx++) {
    OutputItem *output = callee_output[idx];
    Value *curr_output = out_values[idx];
    
    // Link the point-to results for pseudo outputs
    linkOutputPointsToResults(output, curr_output, escape_object_map, 
                               callee_func_arg, visited);
    
    // Cache PT result iterators
    for (PTResult *visited_item : visited) {
      if (!pt_result_cache.count(visited_item)) {
        PTResultIterator iter(visited_item, this);
        pt_result_cache.emplace(visited_item, std::move(iter));
      }
    }

    // Link the value
    linkOutputValues(output, curr_output, idx, escape_object_map, 
                     callee_func_arg, callsite, pt_result_cache);
  }
}

void IntraLotusAA::cacheFunctionCallInfo() {
  if (func_obj)
    return;

  func_obj = newObject(nullptr);
  ObjectLocator *loc = func_obj->findLocator(0, true);
  
  for (BasicBlock *bb : topBBs) {
    for (Instruction &inst : *bb) {
      if (CallBase *call = dyn_cast<CallBase>(&inst)) {
        if (Function *called = call->getCalledFunction()) {
          if (called->isIntrinsic())
            continue;
        }
        loc->storeValue(call, call, 0);
      }
    }
  }
}

