/// @file PointsToGraph.cpp
/// @brief Points-to graph data structure and core operations
///
/// This file implements `PTGraph` and `PTResult`, the foundational data
/// structures for representing and querying **points-to relationships** in
/// LotusAA.
///
/// **Core Data Structures:**
///
/// 1. **PTResult**: Points-to set for a single pointer value
///    - Direct targets: `{(obj1, offset1), (obj2, offset2), ...}`
///    - Derived targets: `{ptr → offset}` (indirection through another pointer)
///    - Cached and memoized for performance
///
/// 2. **PTResultIterator**: Efficient traversal of points-to sets
///    - Recursively expands derived targets
///    - Handles cycles gracefully
///    - Caches results for repeated queries
///
/// 3. **PTGraph**: Per-function points-to graph
///    - Maps `Value* → PTResult*` (points-to sets)
///    - Owns all `MemObject`s for the function
///    - Provides utilities for memory operations and value tracking
///
/// **Key Operations:**
///
/// - `addPointsTo(ptr, obj, offset)`: Create direct points-to edge
/// - `derivePtsFrom(ptr, other_pts, offset)`: Create derived edge
/// - `loadPtrAt(ptr, inst, result)`: Load values from memory locations
/// - `trackPtrRightValue(val, result)`: Track value through PHI/Select/Load
///
/// **Memory Model Integration:**
/// ```
/// PTGraph
///   ├── pt_results: Value → PTResult
///   ├── mem_objs: MemObject set
///   └── Each MemObject contains ObjectLocators
///       └── Each ObjectLocator tracks stored values
/// ```
///
/// **Optimization Techniques:**
///
/// 1. **Memoization**: PTResults cached by Value*
/// 2. **Iterator Caching**: PTResultIterator results cached in PTResult
/// 3. **Load Categories**: Group equivalent loads for load-load matching
/// 4. **Access Path Depth Limiting**: Prune deep field accesses for scalability
///
/// **Configuration:**
/// - `lotus_restrict_pts_count`: Max points-to set size (default: 100)
/// - `lotus_restrict_obj_ap_depth`: Max access path depth for objects (default:
/// 5)
///
/// **Special Values:**
/// - `NullPTS`: Singleton for null pointer
/// - `DEFAULT_NON_POINTER_TYPE`: Int64 (placeholder for non-pointers)
/// - `DEFAULT_POINTER_TYPE`: Int8* (generic pointer type)
///
/// @see PTResult for points-to set representation
/// @see PTResultIterator for efficient set traversal
/// @see MemObject for memory object abstraction
/// @see ObjectLocator for field-level tracking

#include "Alias/InclusionBased/LotusAA/MemoryModel/PointsToGraph.h"

#include "Alias/InclusionBased/LotusAA/Engine/InterProceduralPass.h"
#include "Alias/InclusionBased/LotusAA/Engine/IntraProceduralAnalysis.h"
#include "Alias/InclusionBased/LotusAA/Support/Config.h"

#include <llvm/Support/ErrorHandling.h>

#include <functional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include <z3++.h>

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/DominanceFrontier.h>
#include <llvm/Analysis/IteratedDominanceFrontier.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/CommandLine.h>

using namespace llvm;
using namespace std;

static cl::opt<int> lotus_restrict_pts_count(
    "lotus-restrict-pts-count",
    cl::desc("Maximum number of locators a pointer may point to"), cl::init(3),
    cl::Hidden);

static cl::opt<int> lotus_restrict_obj_ap_depth(
    "lotus-restrict-obj-ap-depth",
    cl::desc("Maximum AP-depth of objects considered for callees"), cl::init(5),
    cl::Hidden);

namespace {

template <typename T> static pair<T, T> canonicalCondPair(T lhs, T rhs) {
  return (lhs < rhs) ? make_pair(lhs, rhs) : make_pair(rhs, lhs);
}

static bool canSplitSelectCondition(Value *cond) {
  if (!cond || isa<UndefValue>(cond) || isa<PoisonValue>(cond))
    return false;

  if (isa<Instruction>(cond) || isa<Argument>(cond))
    return true;

  if (auto *CI = dyn_cast<ConstantInt>(cond))
    return CI->getBitWidth() == 1;

  return false;
}

static bool isCompositeCond(path_cond_t cond) {
  if (!cond)
    return false;
  switch (cond->getKind()) {
  case PathCond::Kind::And:
  case PathCond::Kind::Or:
  case PathCond::Kind::Not:
    return true;
  default:
    return false;
  }
}

static Type *getFieldTypeAtOffset(Type *type, int64_t offset,
                                  const DataLayout &DL) {
  if (!type || PTGraph::isUnknownOffset(offset) || offset <= 0)
    return type;

  if (auto *array_ty = dyn_cast<ArrayType>(type)) {
    uint64_t elem_size = DL.getTypeSizeInBits(array_ty->getElementType());
    if (elem_size == 0)
      return type;
    uint64_t idx = static_cast<uint64_t>(offset) / elem_size;
    uint64_t remainder = static_cast<uint64_t>(offset) % elem_size;
    if (idx >= array_ty->getNumElements())
      return type;
    return getFieldTypeAtOffset(array_ty->getElementType(),
                                static_cast<int64_t>(remainder), DL);
  }

  if (auto *struct_ty = dyn_cast<StructType>(type)) {
    int n_elem = struct_ty->getNumContainedTypes();
    int cur_size = 0;
    int last_size = 0;
    int idx;
    for (idx = 0; idx < n_elem; idx++) {
      if ((uint64_t)cur_size >= (uint64_t)offset)
        break;

      Type *t = struct_ty->getContainedType(idx);
      last_size = cur_size;
      cur_size += (int)DL.getTypeSizeInBits(t);
    }

    if ((uint64_t)cur_size == (uint64_t)offset && idx < n_elem)
      return getFieldTypeAtOffset(struct_ty->getContainedType(idx), 0, DL);
    if ((uint64_t)cur_size > (uint64_t)offset &&
        (uint64_t)last_size < (uint64_t)offset) {
      return getFieldTypeAtOffset(struct_ty->getContainedType(idx - 1),
                                  offset - last_size, DL);
    }
    return type;
  }

  if (auto *vec_ty = dyn_cast<VectorType>(type)) {
    Type *elem_type = vec_ty->getElementType();
    uint64_t elem_size = DL.getTypeSizeInBits(elem_type);
    if (elem_size == 0)
      return type;
    uint64_t idx = static_cast<uint64_t>(offset) / elem_size;
    uint64_t remainder = static_cast<uint64_t>(offset) % elem_size;
    if (idx >= vec_ty->getElementCount().getKnownMinValue())
      return type;
    return getFieldTypeAtOffset(elem_type, static_cast<int64_t>(remainder), DL);
  }

  return type;
}

} // namespace

// Static members
Type *PTGraph::DEFAULT_NON_POINTER_TYPE = nullptr;
Type *PTGraph::DEFAULT_POINTER_TYPE = nullptr;
const int PTGraph::VALUE_SEQ_UNDEF = -1;
const int PTGraph::VALUE_SEQ_INFINITE = -2;
const int PTGraph::FUNC_OBJ_UNREACHABLE = -1;

// PTResultIterator
PTResultIterator::PTResultIterator(PTResult *target, PTGraph *parent_graph)
    : parent_graph(parent_graph) {
  set<PTResult *> visited;
  std::map<std::pair<PTResult *, int64_t>, path_cond_t> seen_cond;
  visit(target, 0, parent_graph->getEmptyCond(), visited, seen_cond);

  // Optimize: cache results in target
  if (!target->is_optimized) {
    target->derived_list.clear();
    target->pt_list.clear();
    int count = 0;
    for (auto &item : res) {
      count++;
      if (lotus_restrict_pts_count != -1 && count > lotus_restrict_pts_count)
        break;
      target->pt_list.push_back(PTResult::PtItem(item.second, item.first));
    }
    target->is_optimized = true;
  }
}

void PTResultIterator::visit(
    PTResult *target, int64_t off, path_cond_t cond, set<PTResult *> &visited,
    std::map<std::pair<PTResult *, int64_t>, path_cond_t> &seen_cond) {
  // In fuzzing / partially-built summaries we can see null derived targets.
  // Don't crash in release builds; just treat them as empty.
  if (!target)
    return;

  // Check for cycles - if already visited, skip
  if (visited.count(target))
    return;

  // Exact DAG subsumption prune: if (target, off) has already been expanded
  // under a condition that subsumes `cond`, re-expanding cannot strengthen any
  // target condition in `res`.
  auto seen_key = std::make_pair(target, off);
  auto seen_it = seen_cond.find(seen_key);
  if (seen_it != seen_cond.end()) {
    path_cond_t merged =
        parent_graph->findOrCreateOrRegion(seen_it->second, cond);
    if (merged == seen_it->second)
      return;
    seen_it->second = merged;
  } else {
    seen_cond.emplace(seen_key, cond);
  }

  visited.insert(target);

  // Direct targets
  for (PTResult::PtItem &item : target->pt_list) {
    if (!item.locator)
      continue;
    ObjectLocator *locator = item.locator->offsetBy(off);
    if (!locator)
      continue;
    path_cond_t new_cond = parent_graph->findOrCreateAndRegion(cond, item.cond);
    auto it = res.find(locator);
    if (it == res.end()) {
      res.insert(make_pair(locator, new_cond));
    } else {
      it->second = parent_graph->findOrCreateOrRegion(it->second, new_cond);
    }
  }

  // Derived targets
  for (PTResult::DerivedPtItem &item : target->derived_list) {
    if (!item.src_pts)
      continue;
    path_cond_t new_cond = parent_graph->findOrCreateAndRegion(cond, item.cond);
    visit(item.src_pts, off + item.offset, new_cond, visited, seen_cond);
  }

  visited.erase(target);
}

namespace llvm {

raw_ostream &operator<<(raw_ostream &out, PTResultIterator &pt_it) {
  for (auto it = pt_it.begin(); it != pt_it.end(); ++it) {
    out << "  " << *it->first << "\n";
  }
  return out;
}

} // namespace llvm

// PTGraph
PTGraph::PTGraph(Function *F, LotusAA *lotus_aa)
    : analyzed_func(F), lotus_aa(lotus_aa), pt_index(0), obj_index(0),
      load_load_match_performed(false), control_dep_ready_(false) {
  // Get dominance information
  dom_tree = lotus_aa->getDomTree(F);
  post_dom_tree = new PostDominatorTree(*F);

  true_cond_ = nullptr;
  false_cond_ = nullptr;

  // Create NULL points-to result
  NullPTS = addPointsTo(nullptr, MemObject::NullObj, 0, getEmptyCond());
}

PTGraph::~PTGraph() {
  delete NullPTS;

  for (auto &it : pt_results) {
    if (it.second != NullPTS)
      delete it.second;
  }

  for (auto &obj : mem_objs) {
    delete obj.first;
  }

  for (auto &category : load_category_collection) {
    delete category;
  }

  delete post_dom_tree;
}

PTResult *PTGraph::findPTResult(Value *ptr, bool is_create) {
  auto it = pt_results.find(ptr);
  if (it != pt_results.end())
    return it->second;

  if (is_create) {
    PTResult *pts = new PTResult(ptr);
    pt_results[ptr] = pts;
    return pts;
  }

  return nullptr;
}

MemObject *PTGraph::newObject(Value *alloc_site, MemObject::ObjKind obj_type) {
  MemObject *obj = (obj_type == MemObject::CONCRETE)
                       ? new MemObject(alloc_site, this, obj_type)
                       : new SymbolicMemObject(alloc_site, this);

  if (isa_and_nonnull<GlobalValue>(alloc_site)) {
    global_objects.insert(obj);
  }

  mem_objs[obj] = obj_index++;
  return obj;
}

PTResult *PTGraph::addPointsTo(Value *ptr, MemObject *obj, int64_t offset,
                               path_cond_t cond) {
  if (pt_results.find(ptr) != pt_results.end())
    llvm::report_fatal_error("Re-assigning value -- violating SSA");
  PTResult *pts = findPTResult(ptr, true);
  pts->add_target(cond, obj, offset);
  return pts;
}

PTResult *PTGraph::derivePtsFrom(Value *ptr, PTResult *other_pts,
                                 int64_t offset, path_cond_t cond) {
  if (pt_results.find(ptr) != pt_results.end())
    llvm::report_fatal_error("Re-assigning value -- violating SSA");
  PTResult *pts = findPTResult(ptr, true);
  pts->add_derived_target(cond, other_pts, offset);
  return pts;
}

PTResult *PTGraph::assignPts(Value *ptr, PTResult *pts) {
  pt_results[ptr] = pts;
  return pts;
}

Type *PTGraph::normalizeType(Type *type) {
  assert(type && "Normalizing NULL type");
  return type; // Use original type
}

void PTGraph::refineResult(mem_value_t &to_refine) {
  map<Value *, map<Instruction *, pair<path_cond_t, float>, llvm_cmp>, llvm_cmp>
      tmp_to_merge_values;
  for (auto &val_struct : to_refine) {
    path_cond_t cond = val_struct.cond;
    Value *val = val_struct.val;
    Instruction *pos = val_struct.pos;
    float confidence = val_struct.confidence;
    if (tmp_to_merge_values.count(val) == 0 ||
        tmp_to_merge_values[val].count(pos) == 0) {
      tmp_to_merge_values[val][pos] = make_pair(cond, confidence);
    } else {
      path_cond_t pre_cond = tmp_to_merge_values[val][pos].first;
      float pre_confidence = tmp_to_merge_values[val][pos].second;
      tmp_to_merge_values[val][pos].first =
          findOrCreateOrRegion(pre_cond, cond);
      tmp_to_merge_values[val][pos].second =
          mem_value_item_t::compute_or_confidence(pre_confidence, confidence);
    }
  }

  to_refine.clear();
  for (auto &val_pair : tmp_to_merge_values) {
    for (auto &pos_cond_pair : val_pair.second) {
      to_refine.push_back(mem_value_item_t(pos_cond_pair.second.first,
                                           pos_cond_pair.first, val_pair.first,
                                           pos_cond_pair.second.second));
    }
  }
}

void PTGraph::trackPtrRightValue(Value *ptr, mem_value_t &res) {
  trackPtrRightValueUnderCondition(ptr, res, getEmptyCond(), 1.0f);
  refineResult(res);
}

void PTGraph::trackPtrRightValueUnderCondition(Value *ptr, mem_value_t &res,
                                               path_cond_t base_cond,
                                               float base_confidence) {
  if (IntraLotusAAConfig::lotus_restrict_right_value_count != -1 &&
      static_cast<int>(res.size()) >=
          IntraLotusAAConfig::lotus_restrict_right_value_count)
    return;

  if (Argument *arg = dyn_cast<Argument>(ptr)) {
    res.push_back(mem_value_item_t(base_cond, nullptr, arg, base_confidence));
  } else if (LoadInst *load = dyn_cast<LoadInst>(ptr)) {
    mem_value_t load_result;
    getLoadValues(load->getPointerOperand(), load, load_result);
    for (auto &item : load_result) {
      path_cond_t final_cond = findOrCreateAndRegion(base_cond, item.cond);
      float final_confidence = mem_value_item_t::compute_and_confidence(
          base_confidence, item.confidence);
      trackPtrRightValueUnderCondition(item.val, res, final_cond,
                                       final_confidence);
    }
  } else if (PHINode *phi = dyn_cast<PHINode>(ptr)) {
    for (unsigned i = 0; i < phi->getNumIncomingValues(); i++) {
      Value *incoming_val = phi->getIncomingValue(i);
      BasicBlock *incoming_bb = phi->getIncomingBlock(i);
      path_cond_t phi_cond =
          findOrCreateUnitPhiRegion(phi->getParent(), incoming_bb);
      trackPtrRightValueUnderCondition(
          incoming_val, res, findOrCreateAndRegion(base_cond, phi_cond),
          base_confidence);
    }
  } else if (SelectInst *sel = dyn_cast<SelectInst>(ptr)) {
    if (!canSplitSelectCondition(sel->getCondition())) {
      // Falcon keeps an unmodeled select opaque in right-value propagation
      // instead of inventing a fresh path split.
      res.push_back(mem_value_item_t(base_cond, sel, ptr, base_confidence));
    } else {
      path_cond_t true_cond = getValueCond(sel->getCondition(), true);
      path_cond_t false_cond = getValueCond(sel->getCondition(), false);
      trackPtrRightValueUnderCondition(sel->getTrueValue(), res,
                                       findOrCreateAndRegion(base_cond,
                                                             true_cond),
                                       base_confidence);
      trackPtrRightValueUnderCondition(sel->getFalseValue(), res,
                                       findOrCreateAndRegion(base_cond,
                                                             false_cond),
                                       base_confidence);
    }
  } else if (CastInst *cast = dyn_cast<CastInst>(ptr)) {
    trackPtrRightValueUnderCondition(cast->getOperand(0), res, base_cond,
                                     base_confidence);
  } else {
    res.push_back(mem_value_item_t(base_cond, dyn_cast<Instruction>(ptr), ptr,
                                   base_confidence));
  }
}

void PTGraph::getLoadValues(Value *ptr, Instruction *from_loc, mem_value_t &res,
                            int64_t offset) {
  loadPtrAt(ptr, from_loc, res, false, offset);
}

void PTGraph::loadPtrAt(Value *ptr, Instruction *from_loc, mem_value_t &result,
                        bool create_symbol, int64_t query_offset,
                        int func_level, ObjectLocator *func_call_cache,
                        bool is_include_func_summary,
                        bool is_maintain_load_map,
                        const std::set<Instruction *, llvm_cmp>
                            *surviving_store_positions) {
  // Use visited set to prevent infinite recursion
  std::set<std::tuple<Value *, Instruction *, int64_t>> visited;
  loadPtrAtImpl(ptr, from_loc, result, create_symbol, query_offset, func_level,
                func_call_cache, is_include_func_summary, is_maintain_load_map,
                visited, surviving_store_positions);
}

void PTGraph::loadPtrAtImpl(
    Value *ptr, Instruction *from_loc, mem_value_t &result, bool create_symbol,
    int64_t query_offset, int func_level, ObjectLocator *func_call_cache,
    bool is_include_func_summary, bool is_maintain_load_map,
    std::set<std::tuple<Value *, Instruction *, int64_t>> &visited,
    const std::set<Instruction *, llvm_cmp> *surviving_store_positions) {
  // Defensive: callers should pass real IR values/locations, but fuzzing and
  // summary edges can route nullptrs here. Avoid null-deref inside type queries
  // and ObjectLocator::getValues() (which requires a non-null Instruction*).
  if (!ptr)
    return;

  // Cycle detection - prevent infinite recursion
  std::tuple<Value *, Instruction *, int64_t> key(ptr, from_loc, query_offset);
  if (visited.count(key))
    return;
  visited.insert(key);

  PTResult *ptr_pts = findPTResult(ptr);
  if (!ptr_pts) {
    // Return early if no points-to result (instead of asserting)
    return;
  }

  Type *value_type = nullptr;
  if (create_symbol) {
    PointerType *ptr_type = dyn_cast<PointerType>(ptr->getType());
    if (ptr_type) {
      value_type = getPointerElementTypeCompat(ptr_type, &getDL());
      value_type = getFieldTypeAtOffset(value_type, query_offset, getDL());
    } else {
      value_type = DEFAULT_NON_POINTER_TYPE;
    }
  }

  // Collect all points-to targets
  PTResultIterator iter(ptr_pts, this);
  if (lotus_restrict_pts_count != -1 && iter.size() > lotus_restrict_pts_count)
    return;

  for (auto &point_to_item : iter) {
    ObjectLocator *loc = point_to_item.first;
    int64_t offset = loc->getOffset();
    MemObject *obj = loc->getObj();

    // Skip null and unknown objects
    if (obj->isNull() || obj->isUnknown())
      continue;

    // Adjust offset if query_offset provided
    if (query_offset != 0) {
      loc =
          obj->findLocator(PTGraph::composeOffset(offset, query_offset), true);
    }

    // Get values from this locator
    mem_value_t tmp_result;
    if (from_loc) {
      loc->getValues(from_loc, point_to_item.second, tmp_result, value_type,
                     func_level, true, func_call_cache,
                     is_include_func_summary, surviving_store_positions);
    } else {
      // No program point: fall back to the coarse per-object stored-value cache
      // at this offset. This is conservative and avoids requiring dominance
      // info.
      const int64_t off_key = PTGraph::composeOffset(offset, query_offset);
      auto &stored = obj->getStoredValues();
      auto it = stored.find(off_key);
      if (it != stored.end()) {
        for (Value *v : it->second) {
          tmp_result.push_back(
              mem_value_item_t(point_to_item.second, nullptr, v));
        }
      }

      // If nothing known was stored, mirror ObjectLocator::getValues() default.
      if (tmp_result.empty()) {
        tmp_result.push_back(mem_value_item_t(point_to_item.second, nullptr,
                                              obj->isReallyAllocated()
                                                  ? LocValue::UNDEF_VALUE
                                                  : LocValue::FREE_VARIABLE));
      }
    }

    result.insert(result.end(), tmp_result.begin(), tmp_result.end());

    // Track loaded values for load instructions
    if (is_maintain_load_map && from_loc &&
        (isa<LoadInst>(from_loc) || isa<CallBase>(from_loc))) {
      Value *val = from_loc;
      if (isa<LoadInst>(from_loc))
        val = from_loc;
      obj->getLoadedValues()[PTGraph::composeOffset(offset, query_offset)]
          .insert(val);
    }
  }
}

bool PTGraph::cacheLoadCategory(LoadInst *load_inst) {
  for (unsigned idx = 0; idx < load_category_collection.size(); idx++) {
    assert(!load_category_collection[idx]->empty());
    LoadInst *rep = *load_category_collection[idx]->begin();
    if (isSameValue(load_inst, rep)) {
      load_category_collection[idx]->insert(load_inst);
      load_category[load_inst] = idx;
      return false;
    }
  }

  // New category
  load_category[load_inst] = load_category_collection.size();
  auto *new_category = new set<LoadInst *, llvm_cmp>;
  new_category->insert(load_inst);
  load_category_collection.push_back(new_category);
  return true;
}

void PTGraph::performLoadLoadMatch() {
  if (load_load_match_performed)
    return;

  for (BasicBlock &B : *analyzed_func) {
    for (Instruction &I : B) {
      if (LoadInst *load = dyn_cast<LoadInst>(&I)) {
        cacheLoadCategory(load);
      }
    }
  }

  load_load_match_performed = true;
}

const set<LoadInst *, llvm_cmp> &
PTGraph::getAllLoadWithSameValue(LoadInst *load_inst) {
  if (!load_category.count(load_inst))
    performLoadLoadMatch();
  assert(load_category.count(load_inst));
  int idx = load_category[load_inst];
  return *load_category_collection[idx];
}

bool PTGraph::isSameValue(LoadInst *l1, LoadInst *l2) {
  if (!load_category.empty() && load_category.count(l1) &&
      load_category.count(l2)) {
    return load_category[l1] == load_category[l2];
  }
  return isSameValue(l1->getPointerOperand(), l1, l2->getPointerOperand(), l2);
}

bool PTGraph::isSameValue(Value *ptr1, Instruction *pos1, Value *ptr2,
                          Instruction *pos2, int64_t offset1, int64_t offset2) {
  PTResult *ptr1_pts = findPTResult(ptr1);
  PTResult *ptr2_pts = findPTResult(ptr2);

  if (!ptr1_pts || !ptr2_pts)
    return false;

  PTResultIterator iter1(ptr1_pts, this);
  PTResultIterator iter2(ptr2_pts, this);

  if (iter1.size() != iter2.size() || iter1.size() == 0)
    return false;

  std::map<ObjectLocator *, path_cond_t, obj_loc_cmp> adjusted1, adjusted2;
  for (auto &item : iter1)
    adjusted1[item.first->offsetBy(offset1)] = item.second;
  for (auto &item : iter2)
    adjusted2[item.first->offsetBy(offset2)] = item.second;

  if (adjusted1.size() != adjusted2.size())
    return false;

  for (const auto &item : adjusted1) {
    ObjectLocator *loc = item.first;
    auto other = adjusted2.find(loc);
    if (other == adjusted2.end())
      return false;

    // Load-load matching is path-sensitive in Falcon: the reaching-condition
    // for each matched locator must also agree, not just the locator/version.
    if (item.second != other->second)
      return false;

    MemObject *obj = loc->getObj();
    if (obj->isNull() || obj->isUnknown())
      continue;

    if (loc->getVersion(pos1) != loc->getVersion(pos2))
      return false;
  }

  return true;
}

const PTGraph::GuardedPointsToSignature &
PTGraph::getGuardedPointsToSignature(PTResult *points_to, int64_t offset) {
  auto key = std::make_pair(points_to, offset);
  auto cached = guarded_points_to_signature_cache_.find(key);
  if (cached != guarded_points_to_signature_cache_.end() &&
      cached->second.revision == points_to->getRevision()) {
    return cached->second;
  }

  GuardedPointsToSignature signature;
  signature.revision = points_to->getRevision();
  signature.must_alias_eligible = true;
  std::map<ObjectLocator *, path_cond_t, obj_loc_cmp> adjusted;
  PTResultIterator iterator(points_to, this);
  for (const auto &item : iterator) {
    ObjectLocator *locator = item.first ? item.first->offsetBy(offset) : nullptr;
    if (!locator || !locator->getObj()->isValid() ||
        isUnknownOffset(locator->getOffset())) {
      signature.must_alias_eligible = false;
      continue;
    }
    auto inserted = adjusted.emplace(locator, item.second);
    if (!inserted.second) {
      inserted.first->second =
          findOrCreateOrRegion(inserted.first->second, item.second);
    }
  }
  if (adjusted.empty())
    signature.must_alias_eligible = false;

  size_t fingerprint = adjusted.size();
  for (const auto &item : adjusted) {
    signature.entries.push_back(item);
    const size_t locator_hash = std::hash<const void *>()(item.first);
    const size_t condition_hash = std::hash<const void *>()(item.second);
    fingerprint ^=
        locator_hash + 0x9e3779b9U + (fingerprint << 6) + (fingerprint >> 2);
    fingerprint ^=
        condition_hash + 0x9e3779b9U + (fingerprint << 6) + (fingerprint >> 2);
  }
  signature.fingerprint = fingerprint;

  if (cached == guarded_points_to_signature_cache_.end()) {
    return guarded_points_to_signature_cache_
        .emplace(key, std::move(signature))
        .first->second;
  }
  cached->second = std::move(signature);
  return cached->second;
}

path_cond_t PTGraph::getAliasCondition(Value *ptr1, Value *ptr2,
                                       int64_t offset1, int64_t offset2) {
  PTResult *ptr1_pts = findPTResult(ptr1, false);
  PTResult *ptr2_pts = findPTResult(ptr2, false);
  if (!ptr1_pts || !ptr2_pts)
    return getFalseCond();

  const auto &lhs = getGuardedPointsToSignature(ptr1_pts, offset1);
  const auto &rhs = getGuardedPointsToSignature(ptr2_pts, offset2);
  auto lhs_item = lhs.entries.begin();
  auto rhs_item = rhs.entries.begin();
  obj_loc_cmp compare;
  path_cond_t result = getFalseCond();
  while (lhs_item != lhs.entries.end() && rhs_item != rhs.entries.end()) {
    if (compare(lhs_item->first, rhs_item->first)) {
      ++lhs_item;
      continue;
    }
    if (compare(rhs_item->first, lhs_item->first)) {
      ++rhs_item;
      continue;
    }

    path_cond_t overlap =
        findOrCreateAndRegion(lhs_item->second, rhs_item->second);
    if (isSatisfiable(overlap)) {
      result = findOrCreateOrRegion(result, overlap);
      if (isKnownTrue(result))
        break;
    }
    ++lhs_item;
    ++rhs_item;
  }
  return result;
}

bool PTGraph::mayAlias(Value *ptr1, Value *ptr2, int64_t offset1,
                       int64_t offset2) {
  PTResult *ptr1_pts = findPTResult(ptr1, false);
  PTResult *ptr2_pts = findPTResult(ptr2, false);
  if (!ptr1_pts || !ptr2_pts)
    return false;

  if (ptr1_pts > ptr2_pts ||
      (ptr1_pts == ptr2_pts && offset1 > offset2)) {
    std::swap(ptr1_pts, ptr2_pts);
    std::swap(offset1, offset2);
  }

  auto cache_key = std::make_tuple(ptr1_pts, offset1, ptr2_pts, offset2);
  auto cached = may_alias_cache_.find(cache_key);
  if (cached != may_alias_cache_.end() &&
      cached->second.rev1 == ptr1_pts->getRevision() &&
      cached->second.rev2 == ptr2_pts->getRevision()) {
    return cached->second.result;
  }

  const auto &lhs = getGuardedPointsToSignature(ptr1_pts, offset1);
  const auto &rhs = getGuardedPointsToSignature(ptr2_pts, offset2);
  auto lhs_item = lhs.entries.begin();
  auto rhs_item = rhs.entries.begin();
  obj_loc_cmp compare;
  bool result = false;
  while (lhs_item != lhs.entries.end() && rhs_item != rhs.entries.end()) {
    if (compare(lhs_item->first, rhs_item->first)) {
      ++lhs_item;
      continue;
    }
    if (compare(rhs_item->first, lhs_item->first)) {
      ++rhs_item;
      continue;
    }

    if (isKnownTrue(lhs_item->second) && isKnownTrue(rhs_item->second)) {
      result = true;
      break;
    }
    path_cond_t overlap =
        findOrCreateAndRegion(lhs_item->second, rhs_item->second);
    if (isSatisfiable(overlap)) {
      result = true;
      break;
    }
    ++lhs_item;
    ++rhs_item;
  }

  may_alias_cache_[cache_key] = MayAliasCacheEntry{
      ptr1_pts->getRevision(), ptr2_pts->getRevision(), result};
  return result;
}

bool PTGraph::areMustAliases(Value *ptr1, Value *ptr2, int64_t offset1,
                             int64_t offset2) {
  PTResult *ptr1_pts = findPTResult(ptr1, false);
  PTResult *ptr2_pts = findPTResult(ptr2, false);
  if (!ptr1_pts || !ptr2_pts)
    return false;

  const auto &lhs = getGuardedPointsToSignature(ptr1_pts, offset1);
  const auto &rhs = getGuardedPointsToSignature(ptr2_pts, offset2);
  if (!lhs.must_alias_eligible || !rhs.must_alias_eligible ||
      lhs.fingerprint != rhs.fingerprint ||
      lhs.entries.size() != rhs.entries.size()) {
    return false;
  }

  // Confirm a hash match structurally so collisions cannot manufacture a
  // must-alias proof.
  for (size_t index = 0; index < lhs.entries.size(); ++index) {
    if (lhs.entries[index] != rhs.entries[index])
      return false;
  }
  return true;
}

void PTGraph::dumpMemObjs() {
  for (auto &pair : mem_objs) {
    outs() << "ID:" << pair.second << "\n";
    pair.first->dump();
  }
}

int PTGraph::getObjectToCallApDepth(MemObject *obj, CallInst *call) {
  if (!obj || !call)
    return FUNC_OBJ_UNREACHABLE;

  // Get or compute frontier
  set<MemObject *, mem_obj_cmp> &frontier = object_call_ap_depth_frontier[call];
  map<MemObject *, int, mem_obj_cmp> &cache =
      object_call_arg_ap_depth_cache[call];

  if (frontier.empty()) {
    // Initialize: add global objects
    for (MemObject *global_obj : global_objects) {
      frontier.insert(global_obj);
      cache[global_obj] = 1;
    }

    // Add objects reachable from call arguments
    for (unsigned i = 0; i < call->arg_size(); i++) {
      Value *arg = call->getArgOperand(i);
      PTResult *pts_result = findPTResult(arg, false);
      if (pts_result) {
        PTResultIterator result_iter(pts_result, this);
        for (auto &pt_item : result_iter) {
          MemObject *pt_obj = pt_item.first->getObj();
          if (!cache.count(pt_obj)) {
            cache[pt_obj] = 1;
            frontier.insert(pt_obj);
          }
        }
      }
    }
  }

  // Check cache
  auto cache_find = cache.find(obj);
  if (cache_find != cache.end())
    return cache_find->second;

  // Not in cache - compute on demand
  if (frontier.empty()) {
    cache[obj] = FUNC_OBJ_UNREACHABLE;
    return FUNC_OBJ_UNREACHABLE;
  }

  // Get frontier depth
  MemObject *frontier_sample = *frontier.begin();
  int frontier_depth = cache[frontier_sample];

  // Expand frontier until target found or max depth reached
  set<MemObject *, mem_obj_cmp> new_frontier;
  while (frontier_depth < lotus_restrict_obj_ap_depth) {
    for (MemObject *frontier_obj : frontier) {
      map<int64_t, Type *> &updated_offsets = frontier_obj->getUpdatedOffset();

      for (auto &offset_pair : updated_offsets) {
        int64_t offset = offset_pair.first;
        ObjectLocator *locator = frontier_obj->findLocator(offset, false);
        if (locator) {
          mem_value_t pt_values;
          locator->getValues(call, getEmptyCond(), pt_values);

          for (mem_value_item_t &value_item : pt_values) {
            Value *val = value_item.val;
            PTResult *pts_result = findPTResult(val, false);
            if (pts_result) {
              PTResultIterator result_iter(pts_result, this);
              for (auto &pt_item : result_iter) {
                MemObject *pt_obj = pt_item.first->getObj();
                if (!cache.count(pt_obj)) {
                  cache[pt_obj] = frontier_depth + 1;
                  new_frontier.insert(pt_obj);
                }
              }
            }
          }
        }
      }
    }

    frontier.clear();
    for (MemObject *mem_obj : new_frontier) {
      frontier.insert(mem_obj);
    }
    new_frontier.clear();

    // Check if target found
    cache_find = cache.find(obj);
    if (cache_find != cache.end())
      return cache_find->second;

    frontier_depth++;
  }

  cache[obj] = FUNC_OBJ_UNREACHABLE;
  return FUNC_OBJ_UNREACHABLE;
}

const DataLayout &PTGraph::getDL() { return lotus_aa->getDataLayout(); }

//===----------------------------------------------------------------------===//
// PathCondBddManager - Hybrid Complement-Edge ROBDD + AIG + Tseitin SAT Engine
//===----------------------------------------------------------------------===//

struct PathCondBddManager::SatState {
  z3::context ctx;
  z3::solver solver;
  z3::sort bool_sort;
  std::unordered_map<uint32_t, z3::expr> node_vars;
  std::unordered_set<uint32_t> encoded;
  unsigned queries = 0;

  // Use QF_FD so Z3 dispatches pure propositional Tseitin CNF directly to its
  // native incremental CDCL SAT solver rather than smt::context.
  SatState() : solver(ctx, "QF_FD"), bool_sort(ctx.bool_sort()) {}
};

static uint64_t splitMix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

PathCondBddManager::PathCondBddManager() {
  // Index 0 is the terminal sentinel: FALSE_ID = 0, TRUE_ID = 1.
  nodes_.push_back(Node{std::numeric_limits<uint32_t>::max(), 0, 0});
  aig_nodes_.push_back(AigNode{std::numeric_limits<uint32_t>::max(),
                               std::numeric_limits<uint32_t>::max(),
                               {0ULL, 0ULL, 0ULL, 0ULL}});
}

PathCondBddManager::~PathCondBddManager() = default;

uint32_t PathCondBddManager::allocVar() {
  uint32_t var = next_var_++;
  uint32_t aig_idx = static_cast<uint32_t>(aig_nodes_.size());
  AigNode node{var, var, {0ULL, 0ULL, 0ULL, 0ULL}};
  uint64_t seed = (static_cast<uint64_t>(var) + 1ULL) * 0x9e3779b97f4a7c15ULL;
  for (unsigned w = 0; w < 4; ++w) {
    seed = splitMix64(seed + w);
    node.sim[w] = seed;
  }
  // Pin column 0 to all-0 and column 1 to all-1 for deterministic coverage of
  // monotone all-false / all-true witnesses.
  node.sim[0] = (node.sim[0] & ~uint64_t(3)) | uint64_t(2);
  aig_nodes_.push_back(node);
  var_to_aig_.push_back(aig_idx << 1);
  return var;
}

uint32_t PathCondBddManager::mkBounded(uint32_t var, uint32_t low,
                                       uint32_t high) {
  if (low == high)
    return low;

  bool flip = (low & 1U) != 0;
  if (flip) {
    low ^= 1U;
    high ^= 1U;
  }

  NodeKey key{var, low, high};
  auto it = unique_table_.find(key);
  uint32_t id;
  if (it != unique_table_.end()) {
    id = it->second;
  } else {
    if (nodes_.size() >= MAX_BDD_NODES)
      return INVALID_ID;
    uint32_t idx = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back(Node{var, low, high});
    id = idx << 1;
    unique_table_.try_emplace(key, id);
  }
  return flip ? (id ^ 1U) : id;
}

uint32_t PathCondBddManager::bddAndBounded(uint32_t u, uint32_t v,
                                           unsigned &steps) {
  if (u == FALSE_ID || v == FALSE_ID)
    return FALSE_ID;
  if (u == TRUE_ID)
    return v;
  if (v == TRUE_ID)
    return u;
  if (u == INVALID_ID || v == INVALID_ID)
    return INVALID_ID;
  if (u == v)
    return u;
  if (u == (v ^ 1U))
    return FALSE_ID;

  if (u > v)
    std::swap(u, v);

  uint64_t key = (static_cast<uint64_t>(u) << 32) | static_cast<uint64_t>(v);
  auto it = and_cache_.find(key);
  if (it != and_cache_.end())
    return it->second;

  if (++steps > MAX_BDD_STEPS || nodes_.size() >= MAX_BDD_NODES)
    return INVALID_ID;

  // Copy node fields before recursing because mkBounded() may reallocate nodes_.
  const Node nu = nodes_[u >> 1];
  const Node nv = nodes_[v >> 1];
  const uint32_t top_var = std::min(nu.var, nv.var);
  const uint32_t u_comp = u & 1U;
  const uint32_t v_comp = v & 1U;

  uint32_t u_lo = (nu.var == top_var) ? (nu.low ^ u_comp) : u;
  uint32_t u_hi = (nu.var == top_var) ? (nu.high ^ u_comp) : u;
  uint32_t v_lo = (nv.var == top_var) ? (nv.low ^ v_comp) : v;
  uint32_t v_hi = (nv.var == top_var) ? (nv.high ^ v_comp) : v;

  uint32_t res_lo = bddAndBounded(u_lo, v_lo, steps);
  if (res_lo == INVALID_ID)
    return INVALID_ID;
  uint32_t res_hi = bddAndBounded(u_hi, v_hi, steps);
  if (res_hi == INVALID_ID)
    return INVALID_ID;
  uint32_t res = mkBounded(top_var, res_lo, res_hi);
  if (res == INVALID_ID)
    return INVALID_ID;
  and_cache_[key] = res;
  return res;
}

uint32_t PathCondBddManager::tryBddAnd(uint32_t u, uint32_t v) {
  if (u == FALSE_ID || v == FALSE_ID)
    return FALSE_ID;
  if (u == TRUE_ID)
    return v;
  if (v == TRUE_ID)
    return u;
  if (u == INVALID_ID || v == INVALID_ID)
    return INVALID_ID;
  if (u == v)
    return u;
  if (u == (v ^ 1U))
    return FALSE_ID;
  if (u > v)
    std::swap(u, v);
  uint64_t key = (static_cast<uint64_t>(u) << 32) | static_cast<uint64_t>(v);
  auto it = and_cache_.find(key);
  if (it != and_cache_.end())
    return it->second;
  if (nodes_.size() >= MAX_BDD_NODES)
    return INVALID_ID;
  unsigned steps = 0;
  uint32_t res = bddAndBounded(u, v, steps);
  if (res == INVALID_ID)
    and_cache_[key] = INVALID_ID;
  return res;
}

uint32_t PathCondBddManager::aigAnd(uint32_t a, uint32_t b) {
  if (a == FALSE_ID || b == FALSE_ID)
    return FALSE_ID;
  if (a == TRUE_ID)
    return b;
  if (b == TRUE_ID)
    return a;
  if (a == b)
    return a;
  if (a == (b ^ 1U))
    return FALSE_ID;

  if (a > b)
    std::swap(a, b);

  // Two-level exact structural rewriting catches local absorption and
  // contradictions in O(1) without touching SAT.
  if (isAigGate(a)) {
    const AigNode &na = aig_nodes_[a >> 1];
    if ((a & 1U) == 0) {
      if (b == na.lhs || b == na.rhs)
        return a;
      if (b == (na.lhs ^ 1U) || b == (na.rhs ^ 1U))
        return FALSE_ID;
    } else {
      if (b == (na.lhs ^ 1U) || b == (na.rhs ^ 1U))
        return b;
    }
  }
  if (isAigGate(b)) {
    const AigNode &nb = aig_nodes_[b >> 1];
    if ((b & 1U) == 0) {
      if (a == nb.lhs || a == nb.rhs)
        return b;
      if (a == (nb.lhs ^ 1U) || a == (nb.rhs ^ 1U))
        return FALSE_ID;
    } else {
      if (a == (nb.lhs ^ 1U) || a == (nb.rhs ^ 1U))
        return a;
    }
  }

  uint64_t key = (static_cast<uint64_t>(a) << 32) | static_cast<uint64_t>(b);
  auto it = aig_unique_.find(key);
  if (it != aig_unique_.end())
    return it->second;

  uint32_t idx = static_cast<uint32_t>(aig_nodes_.size());
  AigNode gate{a, b, {0ULL, 0ULL, 0ULL, 0ULL}};
  const AigNode &na = aig_nodes_[a >> 1];
  const AigNode &nb = aig_nodes_[b >> 1];
  const uint64_t ma = (a & 1U) ? ~uint64_t(0) : uint64_t(0);
  const uint64_t mb = (b & 1U) ? ~uint64_t(0) : uint64_t(0);
  for (unsigned w = 0; w < 4; ++w)
    gate.sim[w] = (na.sim[w] ^ ma) & (nb.sim[w] ^ mb);
  aig_nodes_.push_back(gate);
  uint32_t id = idx << 1;
  aig_unique_.try_emplace(key, id);
  return id;
}

PathCondBddManager::FormulaIds
PathCondBddManager::makeVarIds(uint32_t var) {
  return FormulaIds{mkBounded(var, FALSE_ID, TRUE_ID), aigVar(var)};
}

PathCondBddManager::FormulaIds
PathCondBddManager::combineAnd(FormulaIds lhs, FormulaIds rhs) {
  uint32_t aig = aigAnd(lhs.aig, rhs.aig);
  if (aig == FALSE_ID)
    return FormulaIds{FALSE_ID, FALSE_ID};
  if (aig == TRUE_ID)
    return FormulaIds{TRUE_ID, TRUE_ID};

  uint32_t bdd = tryBddAnd(lhs.bdd, rhs.bdd);
  if (bdd == FALSE_ID)
    return FormulaIds{FALSE_ID, FALSE_ID};
  if (bdd == TRUE_ID)
    return FormulaIds{TRUE_ID, TRUE_ID};
  return FormulaIds{bdd, aig};
}

PathCondBddManager::FormulaIds
PathCondBddManager::encodeMutexMember(MutexGroup &group,
                                      const void *member_key) {
  auto it = group.member_ids.find(member_key);
  if (it != group.member_ids.end())
    return it->second;

  FormulaIds var_ids = makeVarIds(allocVar());
  FormulaIds member_ids = combineAnd(var_ids, group.none_yet);
  group.none_yet = combineAnd(group.none_yet, combineNot(var_ids));
  group.member_ids.emplace(member_key, member_ids);
  return member_ids;
}

uint32_t PathCondBddManager::getValueVar(const Value *value) {
  if (!value) {
    if (null_value_var_ == std::numeric_limits<uint32_t>::max())
      null_value_var_ = allocVar();
    return null_value_var_;
  }
  auto it = value_vars_.find(value);
  if (it != value_vars_.end())
    return it->second;
  uint32_t var = allocVar();
  value_vars_.try_emplace(value, var);
  return var;
}

PathCondBddManager::FormulaIds
PathCondBddManager::encodeValueAtom(const Value *value, bool sense) {
  FormulaIds ids = makeVarIds(getValueVar(value));
  return sense ? ids : combineNot(ids);
}

PathCondBddManager::FormulaIds
PathCondBddManager::encodeBranchAtom(const BasicBlock *block,
                                     const BasicBlock *successor,
                                     const Value *value, bool sense) {
  if (value)
    return encodeValueAtom(value, sense);

  auto *br = block ? dyn_cast_or_null<BranchInst>(block->getTerminator())
                   : nullptr;
  if (br && br->isConditional() && br->getNumSuccessors() == 2 &&
      br->getSuccessor(0) != br->getSuccessor(1)) {
    auto &entry = branch_noval_vars_[block];
    if (entry.var == std::numeric_limits<uint32_t>::max()) {
      entry.var = allocVar();
      entry.first_succ = br->getSuccessor(0);
    }
    FormulaIds var_ids = makeVarIds(entry.var);
    if (successor == entry.first_succ)
      return var_ids;
    if (successor == br->getSuccessor(1))
      return combineNot(var_ids);
  }

  return encodeMutexMember(fallback_branch_groups_[block], successor);
}

PathCondBddManager::FormulaIds
PathCondBddManager::encodeSwitchAtom(const BasicBlock *block,
                                     const Value *value,
                                     const ConstantInt *case_value,
                                     bool is_default) {
  auto key = std::make_pair(block, value);
  auto it = switch_encodings_.find(key);
  if (it == switch_encodings_.end()) {
    auto *sw = block ? dyn_cast_or_null<SwitchInst>(block->getTerminator())
                     : nullptr;
    if (sw && sw->getCondition() == value && sw->getNumCases() > 0) {
      SmallVector<const ConstantInt *, 16> unique_cases;
      SmallPtrSet<const ConstantInt *, 16> seen_cases;
      for (const auto &case_it : sw->cases()) {
        const ConstantInt *cv = case_it.getCaseValue();
        if (seen_cases.insert(cv).second)
          unique_cases.push_back(cv);
      }

      uint32_t k = static_cast<uint32_t>(unique_cases.size());
      uint32_t num_bits = 1;
      while ((1ULL << num_bits) < static_cast<uint64_t>(k) + 1ULL)
        ++num_bits;

      uint32_t base_var = next_var_;
      for (uint32_t b = 0; b < num_bits; ++b)
        (void)allocVar();

      SwitchEncoding enc;
      enc.case_ids.reserve(k);
      for (uint32_t i = 0; i < k; ++i) {
        uint32_t bdd_node = TRUE_ID;
        uint32_t aig_node = TRUE_ID;
        for (int j = static_cast<int>(num_bits) - 1; j >= 0; --j) {
          uint32_t var = base_var + static_cast<uint32_t>(j);
          uint32_t shift = num_bits - 1U - static_cast<uint32_t>(j);
          bool bit = ((i >> shift) & 1U) != 0;
          if (bdd_node != INVALID_ID) {
            bdd_node = bit ? mkBounded(var, FALSE_ID, bdd_node)
                           : mkBounded(var, bdd_node, FALSE_ID);
          }
          uint32_t v_aig = aigVar(var);
          aig_node = aigAnd(bit ? v_aig : aigNot(v_aig), aig_node);
        }
        enc.case_ids.try_emplace(unique_cases[i],
                                 FormulaIds{bdd_node, aig_node});
      }

      // Encode default as unsigned_value >= k so all k cases + default form
      // an exact mutually-exclusive, exhaustive partition in O(log k) nodes.
      uint32_t def_bdd = TRUE_ID;
      uint32_t def_aig = TRUE_ID;
      for (int j = static_cast<int>(num_bits) - 1; j >= 0; --j) {
        uint32_t var = base_var + static_cast<uint32_t>(j);
        uint32_t shift = num_bits - 1U - static_cast<uint32_t>(j);
        bool bit = ((k >> shift) & 1U) != 0;
        if (def_bdd != INVALID_ID) {
          def_bdd = bit ? mkBounded(var, FALSE_ID, def_bdd)
                        : mkBounded(var, def_bdd, TRUE_ID);
        }
        uint32_t v_aig = aigVar(var);
        def_aig = bit ? aigAnd(v_aig, def_aig) : aigOr(v_aig, def_aig);
      }
      enc.default_ids = FormulaIds{def_bdd, def_aig};

      it = switch_encodings_.emplace(key, std::move(enc)).first;
    }
  }

  if (it != switch_encodings_.end()) {
    if (is_default)
      return it->second.default_ids;
    auto cit = it->second.case_ids.find(case_value);
    if (cit != it->second.case_ids.end())
      return cit->second;
  }

  const void *member_key =
      is_default ? nullptr : static_cast<const void *>(case_value);
  return encodeMutexMember(fallback_switch_groups_[key], member_key);
}

PathCondBddManager::FormulaIds
PathCondBddManager::encodeInvokeAtom(const BasicBlock *block, bool is_normal) {
  auto it = invoke_vars_.find(block);
  uint32_t var;
  if (it != invoke_vars_.end()) {
    var = it->second;
  } else {
    var = allocVar();
    invoke_vars_.try_emplace(block, var);
  }
  FormulaIds ids = makeVarIds(var);
  return is_normal ? ids : combineNot(ids);
}

PathCondBddManager::FormulaIds
PathCondBddManager::encodeBlockAtom(const BasicBlock *block) {
  auto it = block_vars_.find(block);
  if (it != block_vars_.end())
    return makeVarIds(it->second);
  uint32_t var = allocVar();
  block_vars_.try_emplace(block, var);
  return makeVarIds(var);
}

PathCondBddManager::FormulaIds
PathCondBddManager::encodeCallTargetAtom(const Value *value,
                                         const Function *callee) {
  MutexGroup &group =
      value ? call_target_groups_[value] : null_call_target_group_;
  return encodeMutexMember(group, callee);
}

PathCondBddManager::FormulaIds
PathCondBddManager::encodeImportedAtom(const PathCond *imported) {
  auto it = imported_vars_.find(imported);
  if (it != imported_vars_.end())
    return makeVarIds(it->second);
  uint32_t var = allocVar();
  imported_vars_.try_emplace(imported, var);
  return makeVarIds(var);
}

int PathCondBddManager::getCachedAigSat(uint32_t aig_id) const {
  if (aig_id == FALSE_ID)
    return 0;
  if (aig_id == TRUE_ID)
    return 1;
  if ((aig_id >> 1) < aig_nodes_.size()) {
    const AigNode &node = aig_nodes_[aig_id >> 1];
    const uint64_t mask = (aig_id & 1U) ? ~uint64_t(0) : uint64_t(0);
    if (((node.sim[0] ^ mask) | (node.sim[1] ^ mask) | (node.sim[2] ^ mask) |
         (node.sim[3] ^ mask)) != 0)
      return 1;
  }
  std::lock_guard<std::mutex> lock(sat_mutex_);
  auto it = aig_sat_cache_.find(aig_id);
  return it == aig_sat_cache_.end() ? -1 : static_cast<int>(it->second);
}

bool PathCondBddManager::isAigSatisfiable(uint32_t aig_id) const {
  if (aig_id == FALSE_ID)
    return false;
  if (aig_id == TRUE_ID)
    return true;
  if (aig_id == INVALID_ID)
    return true;
  if ((aig_id >> 1) < aig_nodes_.size()) {
    const AigNode &node = aig_nodes_[aig_id >> 1];
    const uint64_t mask = (aig_id & 1U) ? ~uint64_t(0) : uint64_t(0);
    if (((node.sim[0] ^ mask) | (node.sim[1] ^ mask) | (node.sim[2] ^ mask) |
         (node.sim[3] ^ mask)) != 0)
      return true;
  }

  std::lock_guard<std::mutex> lock(sat_mutex_);
  auto cached = aig_sat_cache_.find(aig_id);
  if (cached != aig_sat_cache_.end())
    return cached->second != 0;

  // Exact Boolean Constraint Propagation (BCP) over top-level AND/NAND chains.
  // Catches contradictions (e.g., mutually exclusive switch cases) and pure
  // satisfiable conjunctions in O(cone_backbone) with zero Z3 calls.
  SmallDenseMap<uint32_t, uint8_t, 64> assigned;
  SmallVector<uint32_t, 64> unit_wl;
  SmallVector<uint32_t, 32> deferred_nands;

  auto evalLit = [&](uint32_t lit) -> int {
    if (lit == FALSE_ID)
      return 0;
    if (lit == TRUE_ID)
      return 1;
    auto it = assigned.find(lit >> 1);
    if (it == assigned.end())
      return -1;
    int v = static_cast<int>(it->second);
    return (lit & 1U) ? (1 - v) : v;
  };

  auto assertLitTrue = [&](uint32_t lit) -> bool {
    if (lit == FALSE_ID)
      return false;
    if (lit == TRUE_ID)
      return true;
    uint32_t idx = lit >> 1;
    uint8_t need = (lit & 1U) ? 0 : 1;
    auto ins = assigned.try_emplace(idx, need);
    if (!ins.second)
      return ins.first->second == need;
    unit_wl.push_back(lit);
    return true;
  };

  bool conflict = !assertLitTrue(aig_id);
  while (!conflict) {
    while (!unit_wl.empty()) {
      uint32_t lit = unit_wl.pop_back_val();
      uint32_t idx = lit >> 1;
      uint8_t need = (lit & 1U) ? 0 : 1;
      if (idx == 0 || idx >= aig_nodes_.size())
        continue;
      const AigNode &node = aig_nodes_[idx];
      if (node.lhs == node.rhs)
        continue;
      if (need == 1) {
        if (!assertLitTrue(node.lhs) || !assertLitTrue(node.rhs)) {
          conflict = true;
          break;
        }
      } else {
        deferred_nands.push_back(idx);
      }
    }
    if (conflict)
      break;

    bool progress = false;
    size_t write_idx = 0;
    for (size_t i = 0; i < deferred_nands.size(); ++i) {
      uint32_t gidx = deferred_nands[i];
      const AigNode &node = aig_nodes_[gidx];
      int lv = evalLit(node.lhs);
      int rv = evalLit(node.rhs);
      if (lv == 0 || rv == 0)
        continue;
      if (lv == 1 && rv == 1) {
        conflict = true;
        break;
      }
      if (lv == 1) {
        if (!assertLitTrue(node.rhs ^ 1U)) {
          conflict = true;
          break;
        }
        progress = true;
        continue;
      }
      if (rv == 1) {
        if (!assertLitTrue(node.lhs ^ 1U)) {
          conflict = true;
          break;
        }
        progress = true;
        continue;
      }
      deferred_nands[write_idx++] = gidx;
    }
    deferred_nands.resize(write_idx);
    if (!progress)
      break;
  }

  if (conflict) {
    aig_sat_cache_.try_emplace(aig_id, 0);
    aig_sat_cache_.try_emplace(aig_id ^ 1U, 1);
    return false;
  }
  if (deferred_nands.empty()) {
    aig_sat_cache_.try_emplace(aig_id, 1);
    return true;
  }

  // When SMT fallback is disabled in LotusAA, soundly over-approximate
  // inconclusive disjunctive AIG cones as satisfiable (and do not cache the
  // unverified result) so exact path conditions preserved in GVFG are solved
  // on demand by downstream checkers.
  if (!IntraLotusAAConfig::lotus_enable_pathcond_smt)
    return true;

  // Keep the incremental SAT solver compact so sat::solver::next_var() does
  // not traverse hundreds of thousands of unrelated gates from earlier queries,
  // while avoiding thrashing on a single large cone.
  if (!sat_state_ ||
      (sat_state_->encoded.size() > 65536 && sat_state_->queries >= 128)) {
    sat_state_ = std::make_unique<SatState>();
  }
  ++sat_state_->queries;

  z3::context &ctx = sat_state_->ctx;
  auto getLit = [&](uint32_t id) -> z3::expr {
    if (id == FALSE_ID)
      return ctx.bool_val(false);
    if (id == TRUE_ID)
      return ctx.bool_val(true);
    const z3::expr &v = sat_state_->node_vars.at(id >> 1);
    return (id & 1U) ? !v : v;
  };

  auto ensureEncoded = [&](uint32_t root_idx) {
    SmallVector<std::pair<uint32_t, bool>, 64> stack;
    stack.emplace_back(root_idx, false);
    while (!stack.empty()) {
      auto entry = stack.pop_back_val();
      uint32_t idx = entry.first;
      bool ready = entry.second;
      if (idx == 0 || idx >= aig_nodes_.size() || sat_state_->encoded.count(idx))
        continue;

      const AigNode &node = aig_nodes_[idx];
      bool is_var = (node.lhs == node.rhs);
      if (!is_var && !ready) {
        stack.emplace_back(idx, true);
        if (node.lhs > 1U && !sat_state_->encoded.count(node.lhs >> 1))
          stack.emplace_back(node.lhs >> 1, false);
        if (node.rhs > 1U && !sat_state_->encoded.count(node.rhs >> 1))
          stack.emplace_back(node.rhs >> 1, false);
        continue;
      }

      z3::expr g(ctx,
                 Z3_mk_const(ctx, Z3_mk_int_symbol(ctx, static_cast<int>(idx)),
                             sat_state_->bool_sort));
      sat_state_->node_vars.emplace(idx, g);
      sat_state_->encoded.insert(idx);

      if (!is_var) {
        z3::expr la = getLit(node.lhs);
        z3::expr lb = getLit(node.rhs);
        sat_state_->solver.add(!g || la);
        sat_state_->solver.add(!g || lb);
        sat_state_->solver.add(g || !la || !lb);
      }
    }
  };

  // Encode only the residual unresolved NAND gates and constrained primary
  // input literals; all resolved AND-backbone gates are already enforced.
  z3::expr_vector assumps(ctx);
  for (uint32_t gidx : deferred_nands) {
    ensureEncoded(gidx);
    assumps.push_back(getLit((gidx << 1) | 1U));
  }
  for (const auto &kv : assigned) {
    uint32_t idx = kv.first;
    if (idx == 0 || idx >= aig_nodes_.size())
      continue;
    const AigNode &node = aig_nodes_[idx];
    if (node.lhs != node.rhs)
      continue; // Only primary input variables need direct unit assumptions.
    if (!sat_state_->encoded.count(idx))
      continue; // Primary input does not appear in any unresolved NAND cone.
    assumps.push_back(getLit((idx << 1) | (kv.second ? 0U : 1U)));
  }

  z3::check_result res = sat_state_->solver.check(assumps);
  bool is_sat = (res != z3::unsat);
  aig_sat_cache_.try_emplace(aig_id, is_sat ? 1 : 0);
  if (!is_sat)
    aig_sat_cache_.try_emplace(aig_id ^ 1U, 1);
  return is_sat;
}

//===----------------------------------------------------------------------===//
// PTGraph PathCond canonicalization & region construction
//===----------------------------------------------------------------------===//

const SmallVector<BasicBlock *, 8> &
PTGraph::getIteratedDominanceFrontier(BasicBlock *bb) {
  auto it = idf_cache_.find(bb);
  if (it != idf_cache_.end())
    return it->second;

  SmallVector<BasicBlock *, 8> &frontier = idf_cache_[bb];
  if (dom_tree && bb) {
    SmallVector<BasicBlock *, 32> raw_frontier;
    SmallPtrSet<BasicBlock *, 4> def_blocks;
    def_blocks.insert(bb);
    ForwardIDFCalculator idf(*dom_tree);
    idf.setDefiningBlocks(def_blocks);
    idf.calculate(raw_frontier);
    frontier.assign(raw_frontier.begin(), raw_frontier.end());
  }
  return frontier;
}

PathCondBddManager::FormulaIds PTGraph::getFormulaIds(path_cond_t cond) {
  if (!cond)
    return {PathCond::TRUE_BDD_ID, PathCond::TRUE_BDD_ID};
  uint32_t existing_bdd = cond->getBddId();
  uint32_t existing_aig = cond->getAigId();
  if (existing_bdd == PathCond::TRUE_BDD_ID ||
      existing_aig == PathCond::TRUE_BDD_ID) {
    return {PathCond::TRUE_BDD_ID, PathCond::TRUE_BDD_ID};
  }
  if (existing_bdd == PathCond::FALSE_BDD_ID ||
      existing_aig == PathCond::FALSE_BDD_ID) {
    return {PathCond::FALSE_BDD_ID, PathCond::FALSE_BDD_ID};
  }
  if (cond->getBddOwner() == &bdd_mgr_ &&
      existing_aig != PathCond::INVALID_BDD_ID) {
    return {existing_bdd, existing_aig};
  }
  auto it = foreign_formula_cache_.find(cond);
  if (it != foreign_formula_cache_.end())
    return it->second;
  auto ids = computeFormulaIds(cond);
  foreign_formula_cache_.try_emplace(cond, ids);
  return ids;
}

PathCondBddManager::FormulaIds
PTGraph::computeFormulaIds(const PathCond *cond) {
  if (!cond)
    return {PathCond::TRUE_BDD_ID, PathCond::TRUE_BDD_ID};
  switch (cond->getKind()) {
  case PathCond::Kind::True:
    return {PathCond::TRUE_BDD_ID, PathCond::TRUE_BDD_ID};
  case PathCond::Kind::False:
    return {PathCond::FALSE_BDD_ID, PathCond::FALSE_BDD_ID};
  case PathCond::Kind::ValueAtom:
    return bdd_mgr_.encodeValueAtom(cond->getValue(), cond->getSense());
  case PathCond::Kind::BranchAtom:
    return bdd_mgr_.encodeBranchAtom(cond->getBlock(), cond->getSuccessor(),
                                     cond->getValue(), cond->getSense());
  case PathCond::Kind::SwitchCaseAtom:
    return bdd_mgr_.encodeSwitchAtom(cond->getBlock(), cond->getValue(),
                                     cond->getCaseValue(), false);
  case PathCond::Kind::SwitchDefaultAtom:
    return bdd_mgr_.encodeSwitchAtom(cond->getBlock(), cond->getValue(),
                                     nullptr, true);
  case PathCond::Kind::InvokeNormalAtom:
    return bdd_mgr_.encodeInvokeAtom(cond->getBlock(), true);
  case PathCond::Kind::InvokeUnwindAtom:
    return bdd_mgr_.encodeInvokeAtom(cond->getBlock(), false);
  case PathCond::Kind::BlockAtom:
    return bdd_mgr_.encodeBlockAtom(cond->getBlock());
  case PathCond::Kind::CallTargetAtom:
    return bdd_mgr_.encodeCallTargetAtom(cond->getValue(), cond->getCallee());
  case PathCond::Kind::ImportedAtom:
    return bdd_mgr_.encodeImportedAtom(cond->getImportedSource());
  case PathCond::Kind::Not:
    return cond->getLhs() ? bdd_mgr_.combineNot(getFormulaIds(cond->getLhs()))
                          : PathCondBddManager::FormulaIds{
                                PathCond::FALSE_BDD_ID, PathCond::FALSE_BDD_ID};
  case PathCond::Kind::And:
    return (cond->getLhs() && cond->getRhs())
               ? bdd_mgr_.combineAnd(getFormulaIds(cond->getLhs()),
                                     getFormulaIds(cond->getRhs()))
               : PathCondBddManager::FormulaIds{PathCond::FALSE_BDD_ID,
                                                PathCond::FALSE_BDD_ID};
  case PathCond::Kind::Or:
    return (cond->getLhs() && cond->getRhs())
               ? bdd_mgr_.combineOr(getFormulaIds(cond->getLhs()),
                                    getFormulaIds(cond->getRhs()))
               : PathCondBddManager::FormulaIds{PathCond::FALSE_BDD_ID,
                                                PathCond::FALSE_BDD_ID};
  }
  return {PathCond::FALSE_BDD_ID, PathCond::FALSE_BDD_ID};
}

path_cond_t
PTGraph::lookupCachedCond(PathCondBddManager::FormulaIds ids) const {
  if (ids.bdd != PathCond::INVALID_BDD_ID) {
    auto it = formula_cond_cache_.find(ids.bdd);
    if (it != formula_cond_cache_.end())
      return it->second;
  }
  if (ids.aig != PathCond::INVALID_BDD_ID) {
    auto it = aig_cond_cache_.find(ids.aig);
    if (it != aig_cond_cache_.end())
      return it->second;
  }
  return nullptr;
}

path_cond_t PTGraph::internCondWithIds(PathCondBddManager::FormulaIds ids,
                                       std::unique_ptr<PathCond> cond) {
  if (path_cond_t existing = lookupCachedCond(ids))
    return existing;

  cond->setIds(ids.bdd, ids.aig, &bdd_mgr_);
  path_cond_t raw = cond.get();
  if (ids.bdd != PathCond::INVALID_BDD_ID)
    formula_cond_cache_.try_emplace(ids.bdd, raw);
  if (ids.aig != PathCond::INVALID_BDD_ID)
    aig_cond_cache_.try_emplace(ids.aig, raw);
  cond_nodes_.push_back(std::move(cond));
  return raw;
}

path_cond_t PTGraph::internCond(std::unique_ptr<PathCond> cond) {
  auto ids = computeFormulaIds(cond.get());
  return internCondWithIds(ids, std::move(cond));
}

path_cond_t PTGraph::getEmptyCond() {
  if (!true_cond_) {
    true_cond_ = internCondWithIds(
        {PathCond::TRUE_BDD_ID, PathCond::TRUE_BDD_ID},
        std::unique_ptr<PathCond>(PathCond::createTrue()));
  }
  return true_cond_;
}

path_cond_t PTGraph::getFalseCond() {
  if (!false_cond_) {
    false_cond_ = internCondWithIds(
        {PathCond::FALSE_BDD_ID, PathCond::FALSE_BDD_ID},
        std::unique_ptr<PathCond>(PathCond::createFalse()));
  }
  return false_cond_;
}

bool PTGraph::isKnownTrue(path_cond_t cond) const {
  if (!cond)
    return true;
  uint32_t bdd_id = cond->getBddId();
  if (bdd_id != PathCond::INVALID_BDD_ID)
    return bdd_id == PathCond::TRUE_BDD_ID;
  uint32_t aig_id = cond->getAigId();
  if (aig_id == PathCond::TRUE_BDD_ID)
    return true;
  if (aig_id == PathCond::FALSE_BDD_ID)
    return false;
  if (aig_id != PathCond::INVALID_BDD_ID && cond->getBddOwner()) {
    return static_cast<const PathCondBddManager *>(cond->getBddOwner())
               ->getCachedAigSat(PathCondBddManager::aigNot(aig_id)) == 0;
  }
  return false;
}

bool PTGraph::isKnownFalse(path_cond_t cond) const {
  if (!cond)
    return false;
  uint32_t bdd_id = cond->getBddId();
  if (bdd_id != PathCond::INVALID_BDD_ID)
    return bdd_id == PathCond::FALSE_BDD_ID;
  uint32_t aig_id = cond->getAigId();
  if (aig_id == PathCond::FALSE_BDD_ID)
    return true;
  if (aig_id == PathCond::TRUE_BDD_ID)
    return false;
  if (aig_id != PathCond::INVALID_BDD_ID && cond->getBddOwner()) {
    return static_cast<const PathCondBddManager *>(cond->getBddOwner())
               ->getCachedAigSat(aig_id) == 0;
  }
  return false;
}

bool PTGraph::isAlwaysSatisfied(path_cond_t cond) const {
  if (!cond)
    return true;
  uint32_t bdd_id = cond->getBddId();
  if (bdd_id != PathCond::INVALID_BDD_ID)
    return bdd_id == PathCond::TRUE_BDD_ID;
  uint32_t aig_id = cond->getAigId();
  if (aig_id != PathCond::INVALID_BDD_ID && cond->getBddOwner()) {
    return !static_cast<const PathCondBddManager *>(cond->getBddOwner())
                ->isAigSatisfiable(PathCondBddManager::aigNot(aig_id));
  }
  return cond->getConstraintSummary().always_true;
}

bool PTGraph::isSatisfiable(path_cond_t cond) const {
  if (!cond)
    return true;
  uint32_t bdd_id = cond->getBddId();
  if (bdd_id != PathCond::INVALID_BDD_ID)
    return bdd_id != PathCond::FALSE_BDD_ID;
  uint32_t aig_id = cond->getAigId();
  if (aig_id != PathCond::INVALID_BDD_ID && cond->getBddOwner()) {
    return static_cast<const PathCondBddManager *>(cond->getBddOwner())
        ->isAigSatisfiable(aig_id);
  }
  return !cond->getConstraintSummary().always_false;
}

bool PTGraph::isNoEffectFunction(Function *F) const {
  return lotus_aa && F && lotus_aa->getSpecManager().isNoEffect(F);
}

path_cond_t PTGraph::getValueCond(Value *value, bool sense) {
  if (!value)
    return sense ? getEmptyCond() : getFalseCond();

  if (auto *CI = dyn_cast<ConstantInt>(value)) {
    bool is_true = CI->isOne();
    return (is_true == sense) ? getEmptyCond() : getFalseCond();
  }

  auto key = make_pair(value, sense ? 1 : 0);
  auto it = value_cond_cache_.find(key);
  if (it != value_cond_cache_.end())
    return it->second;

  auto ids = bdd_mgr_.encodeValueAtom(value, sense);
  path_cond_t cond = lookupCachedCond(ids);
  if (!cond) {
    cond = internCondWithIds(
        ids, std::unique_ptr<PathCond>(PathCond::createValueAtom(value, sense)));
  }
  value_cond_cache_[key] = cond;
  return cond;
}

path_cond_t PTGraph::getBlockCond(BasicBlock *BB) { return getUnitRegion(BB); }

void PTGraph::buildControlDependenceInfo() {
  if (control_dep_ready_)
    return;

  control_dep_cache_.clear();
  unit_region_cache_.clear();
  visited.clear();
  bb_rpo_order_.clear();

  // Compute CFG Reverse Post-Order and pre-allocate BDD variables for branch,
  // switch, and invoke terminators in RPO so outer/earlier guards always
  // precede inner/later guards in the ROBDD variable order.
  if (analyzed_func && !analyzed_func->empty()) {
    SmallVector<BasicBlock *, 64> rpo_blocks;
    rpo_blocks.reserve(analyzed_func->size());
    ReversePostOrderTraversal<Function *> rpot(analyzed_func);
    for (BasicBlock *BB : rpot) {
      if (bb_rpo_order_.try_emplace(BB, static_cast<unsigned>(rpo_blocks.size()))
              .second) {
        rpo_blocks.push_back(BB);
      }
    }
    for (BasicBlock &BB : *analyzed_func) {
      if (bb_rpo_order_
              .try_emplace(&BB, static_cast<unsigned>(rpo_blocks.size()))
              .second) {
        rpo_blocks.push_back(&BB);
      }
    }

    for (BasicBlock *BB : rpo_blocks) {
      Instruction *term = BB->getTerminator();
      if (auto *br = dyn_cast_or_null<BranchInst>(term)) {
        if (br->isConditional() && br->getSuccessor(0) != br->getSuccessor(1)) {
          (void)bdd_mgr_.encodeBranchAtom(BB, br->getSuccessor(0),
                                          br->getCondition(), true);
        }
      } else if (auto *sw = dyn_cast_or_null<SwitchInst>(term)) {
        (void)bdd_mgr_.encodeSwitchAtom(BB, sw->getCondition(), nullptr, true);
      } else if (isa_and_nonnull<InvokeInst>(term)) {
        (void)bdd_mgr_.encodeInvokeAtom(BB, true);
      }
    }
  }

  auto get_rpo = [this](BasicBlock *BB) -> unsigned {
    auto it = bb_rpo_order_.find(BB);
    return it != bb_rpo_order_.end() ? it->second
                                     : std::numeric_limits<unsigned>::max();
  };

  if (lotus_aa) {
    if (gsa::ControlDependenceAnalysis *cda =
            lotus_aa->getControlDependenceAnalysis(analyzed_func)) {
      DenseMap<BasicBlock *, SmallPtrSet<BasicBlock *, 8>> pdt_controlled_by;
      SmallVector<std::pair<BasicBlock *, BasicBlock *>, 16> fallback_pairs;

      for (BasicBlock &BB : *analyzed_func) {
        if (!cda->isTracked(BB))
          continue;

        auto *target_node =
            post_dom_tree ? post_dom_tree->getNode(&BB) : nullptr;
        for (BasicBlock *controller : cda->getCDBlocks(&BB)) {
          auto *controller_node =
              post_dom_tree ? post_dom_tree->getNode(controller) : nullptr;
          if (target_node && controller_node) {
            pdt_controlled_by[controller].insert(&BB);
          } else {
            fallback_pairs.emplace_back(&BB, controller);
          }
        }
      }

      // Process controllers in deterministic CFG RPO order and walk the
      // post-dominator chain once per unique (controller, succ) edge.
      SmallVector<BasicBlock *, 32> ordered_controllers;
      ordered_controllers.reserve(pdt_controlled_by.size());
      for (auto &ctrl_entry : pdt_controlled_by)
        ordered_controllers.push_back(ctrl_entry.first);
      std::sort(ordered_controllers.begin(), ordered_controllers.end(),
                [&](BasicBlock *lhs, BasicBlock *rhs) {
                  return get_rpo(lhs) < get_rpo(rhs);
                });

      for (BasicBlock *controller : ordered_controllers) {
        const auto &controlled_set = pdt_controlled_by[controller];
        auto *controller_node = post_dom_tree->getNode(controller);
        SmallPtrSet<BasicBlock *, 8> seen_succs;

        for (BasicBlock *succ : successors(controller)) {
          if (!succ || !seen_succs.insert(succ).second)
            continue;
          if (post_dom_tree->dominates(controller, succ))
            continue;
          auto *succ_node = post_dom_tree->getNode(succ);
          if (!succ_node)
            continue;

          path_cond_t edge_cond = getCFGEdgeCond(controller, succ);
          if (!isSatisfiable(edge_cond))
            continue;

          for (; succ_node && succ_node != controller_node;
               succ_node = succ_node->getIDom()) {
            BasicBlock *curr = succ_node->getBlock();
            if (!controlled_set.count(curr))
              continue;
            path_cond_t &curr_cond = control_dep_cache_[curr][controller];
            curr_cond = curr_cond ? findOrCreateOrRegion(curr_cond, edge_cond)
                                  : edge_cond;
          }
        }
      }

      for (const auto &pair : fallback_pairs) {
        BasicBlock *bb = pair.first;
        BasicBlock *controller = pair.second;
        path_cond_t combined_cond = nullptr;
        SmallPtrSet<BasicBlock *, 8> seen_succs;
        for (BasicBlock *succ : successors(controller)) {
          if (!succ || !seen_succs.insert(succ).second)
            continue;
          path_cond_t edge_cond = getCFGEdgeCond(controller, succ);
          if (!isSatisfiable(edge_cond))
            continue;
          if (succ == bb || cda->isReachable(succ, bb)) {
            combined_cond = combined_cond
                                ? findOrCreateOrRegion(combined_cond, edge_cond)
                                : edge_cond;
          }
        }
        if (combined_cond)
          control_dep_cache_[bb][controller] = combined_cond;
      }

      control_dep_ready_ = true;
      return;
    }
  }

  if (!post_dom_tree) {
    control_dep_ready_ = true;
    return;
  }

  for (BasicBlock &BB : *analyzed_func) {
    BasicBlock *controller = &BB;
    auto *controller_node = post_dom_tree->getNode(controller);
    if (!controller_node)
      continue;

    SmallPtrSet<BasicBlock *, 8> seen_succs;
    for (BasicBlock *succ : successors(controller)) {
      if (!succ || !seen_succs.insert(succ).second)
        continue;
      auto *succ_node = post_dom_tree->getNode(succ);
      if (!succ_node)
        continue;
      if (post_dom_tree->dominates(controller, succ))
        continue;

      path_cond_t edge_cond = getCFGEdgeCond(controller, succ);
      if (!isSatisfiable(edge_cond))
        continue;

      while (succ_node && succ_node != controller_node) {
        BasicBlock *curr = succ_node->getBlock();
        path_cond_t &curr_cond = control_dep_cache_[curr][controller];
        curr_cond =
            curr_cond ? findOrCreateOrRegion(curr_cond, edge_cond) : edge_cond;
        succ_node = succ_node->getIDom();
      }
    }
  }

  control_dep_ready_ = true;
}

path_cond_t PTGraph::getCFGEdgeCond(BasicBlock *src_bb, BasicBlock *succ_bb) {
  if (!src_bb || !succ_bb)
    return getFalseCond();

  auto key = make_pair(src_bb, succ_bb);
  auto it = edge_cond_cache_.find(key);
  if (it != edge_cond_cache_.end())
    return it->second;

  path_cond_t cond = getEmptyCond();
  Instruction *term = src_bb->getTerminator();
  if (auto *br = dyn_cast<BranchInst>(term)) {
    if (br->isConditional()) {
      if (br->getSuccessor(0) == br->getSuccessor(1)) {
        cond = (succ_bb == br->getSuccessor(0)) ? getEmptyCond()
                                                : getFalseCond();
      } else {
        bool sense = succ_bb == br->getSuccessor(0);
        auto ids = bdd_mgr_.encodeBranchAtom(
            src_bb, succ_bb, br->getCondition(), sense);
        cond = lookupCachedCond(ids);
        if (!cond) {
          cond = internCondWithIds(
              ids, std::unique_ptr<PathCond>(PathCond::createBranchAtom(
                       src_bb, succ_bb, br->getCondition(), sense)));
        }
      }
    }
  } else if (auto *sw = dyn_cast<SwitchInst>(term)) {
    // Populate all successor edge conditions of this switch in one linear pass
    // over cases() to avoid O(cases * successors) re-scanning.
    DenseMap<BasicBlock *, path_cond_t> succ_conds;
    for (const auto &case_it : sw->cases()) {
      BasicBlock *case_succ = case_it.getCaseSuccessor();
      auto ids = bdd_mgr_.encodeSwitchAtom(src_bb, sw->getCondition(),
                                           case_it.getCaseValue(), false);
      path_cond_t case_cond = lookupCachedCond(ids);
      if (!case_cond) {
        case_cond = internCondWithIds(
            ids, std::unique_ptr<PathCond>(PathCond::createSwitchCaseAtom(
                     src_bb, case_succ, sw->getCondition(),
                     case_it.getCaseValue())));
      }
      path_cond_t &slot = succ_conds[case_succ];
      slot = slot ? findOrCreateOrRegion(slot, case_cond) : case_cond;
    }

    BasicBlock *def_succ = sw->getDefaultDest();
    auto def_ids =
        bdd_mgr_.encodeSwitchAtom(src_bb, sw->getCondition(), nullptr, true);
    path_cond_t default_cond = lookupCachedCond(def_ids);
    if (!default_cond) {
      default_cond = internCondWithIds(
          def_ids, std::unique_ptr<PathCond>(PathCond::createSwitchDefaultAtom(
                       src_bb, def_succ, sw->getCondition())));
    }
    path_cond_t &def_slot = succ_conds[def_succ];
    def_slot =
        def_slot ? findOrCreateOrRegion(def_slot, default_cond) : default_cond;

    for (const auto &entry : succ_conds)
      edge_cond_cache_[make_pair(src_bb, entry.first)] = entry.second;

    auto found = succ_conds.find(succ_bb);
    cond = (found != succ_conds.end()) ? found->second : getFalseCond();
  } else if (auto *invoke = dyn_cast<InvokeInst>(term)) {
    if (invoke->getNormalDest() == succ_bb) {
      cond = internCond(std::unique_ptr<PathCond>(
          PathCond::createInvokeNormalAtom(src_bb, succ_bb)));
    } else if (invoke->getUnwindDest() == succ_bb) {
      cond = internCond(std::unique_ptr<PathCond>(
          PathCond::createInvokeUnwindAtom(src_bb, succ_bb)));
    } else {
      cond = getFalseCond();
    }
  }

  edge_cond_cache_[key] = cond;
  return cond;
}

path_cond_t PTGraph::localizePathCond(path_cond_t cond) {
  if (!cond)
    return getEmptyCond();
  if (cond->getOwnerFunc() == nullptr || cond->getOwnerFunc() == analyzed_func)
    return cond;
  return importPathCond(cond, nullptr, nullptr);
}

path_cond_t PTGraph::getComplementaryBranchCond(path_cond_t cond) {
  if (!cond || cond->getKind() != PathCond::Kind::BranchAtom)
    return nullptr;

  BasicBlock *block = cond->getBlock();
  BasicBlock *succ = cond->getSuccessor();
  auto *br =
      block ? dyn_cast_or_null<BranchInst>(block->getTerminator()) : nullptr;
  if (!br || !br->isConditional() || br->getNumSuccessors() != 2)
    return nullptr;

  BasicBlock *other_succ =
      (succ == br->getSuccessor(0)) ? br->getSuccessor(1) : br->getSuccessor(0);
  if (!other_succ || other_succ == succ)
    return nullptr;
  return getCFGEdgeCond(block, other_succ);
}

path_cond_t PTGraph::getUnitRegion(BasicBlock *BB) {
  if (!BB)
    return getEmptyCond();

  buildControlDependenceInfo();

  auto cache_it = unit_region_cache_.find(BB);
  if (cache_it != unit_region_cache_.end())
    return cache_it->second;

  // Control dependence can be cyclic
  if (!visited.insert(BB).second)
    return getEmptyCond();

  path_cond_t result = getEmptyCond();
  auto dep_it = control_dep_cache_.find(BB);
  if (dep_it != control_dep_cache_.end()) {
    SmallVector<std::pair<BasicBlock *, path_cond_t>, 8> deps(
        dep_it->second.begin(), dep_it->second.end());
    if (deps.size() > 1) {
      std::sort(deps.begin(), deps.end(),
                [this](const std::pair<BasicBlock *, path_cond_t> &lhs,
                       const std::pair<BasicBlock *, path_cond_t> &rhs) {
                  auto lit = bb_rpo_order_.find(lhs.first);
                  auto rit = bb_rpo_order_.find(rhs.first);
                  unsigned lo = lit != bb_rpo_order_.end()
                                    ? lit->second
                                    : std::numeric_limits<unsigned>::max();
                  unsigned ro = rit != bb_rpo_order_.end()
                                    ? rit->second
                                    : std::numeric_limits<unsigned>::max();
                  return lo < ro;
                });
    }
    bool has_dep = false;
    for (auto &dep_item : deps) {
      path_cond_t dep_region = getUnitRegion(dep_item.first);
      path_cond_t dep_cond = findOrCreateAndRegion(dep_region, dep_item.second);
      result = has_dep ? findOrCreateOrRegion(result, dep_cond) : dep_cond;
      has_dep = true;
    }
  }

  visited.erase(BB);
  unit_region_cache_[BB] = result;
  return result;
}

path_cond_t PTGraph::findOrCreateBBRegion(BasicBlock *src_bb,
                                          BasicBlock *target_bb) {
  if (!src_bb || !target_bb)
    return getFalseCond();
  if (src_bb == target_bb)
    return getEmptyCond();

  auto cache_key = std::make_pair(src_bb, target_bb);
  auto cached = bb_region_cache.find(cache_key);
  if (cached != bb_region_cache.end())
    return cached->second;

  if (dom_tree && !dom_tree->dominates(src_bb, target_bb)) {
    bb_region_cache[cache_key] = getFalseCond();
    return getFalseCond();
  }

  buildControlDependenceInfo();

  if (!bb_region_active_.insert(cache_key).second)
    return getEmptyCond();

  path_cond_t result = getEmptyCond();
  bool has_dep = false;
  auto dep_it = control_dep_cache_.find(target_bb);
  if (dep_it != control_dep_cache_.end()) {
    SmallVector<std::pair<BasicBlock *, path_cond_t>, 8> deps(
        dep_it->second.begin(), dep_it->second.end());
    if (deps.size() > 1) {
      std::sort(deps.begin(), deps.end(),
                [this](const std::pair<BasicBlock *, path_cond_t> &lhs,
                       const std::pair<BasicBlock *, path_cond_t> &rhs) {
                  auto lit = bb_rpo_order_.find(lhs.first);
                  auto rit = bb_rpo_order_.find(rhs.first);
                  unsigned lo = lit != bb_rpo_order_.end()
                                    ? lit->second
                                    : std::numeric_limits<unsigned>::max();
                  unsigned ro = rit != bb_rpo_order_.end()
                                    ? rit->second
                                    : std::numeric_limits<unsigned>::max();
                  return lo < ro;
                });
    }
    for (auto &dep_item : deps) {
      BasicBlock *dep_bb = dep_item.first;
      if (dep_bb != src_bb && dom_tree && !dom_tree->dominates(src_bb, dep_bb))
        continue;

      path_cond_t dep_region = (dep_bb == src_bb)
                                   ? getEmptyCond()
                                   : findOrCreateBBRegion(src_bb, dep_bb);
      path_cond_t dep_cond = findOrCreateAndRegion(dep_region, dep_item.second);
      result = has_dep ? findOrCreateOrRegion(result, dep_cond) : dep_cond;
      has_dep = true;
    }
  }

  bb_region_active_.erase(cache_key);
  bb_region_cache[cache_key] = result;
  return result;
}

path_cond_t PTGraph::importPathCond(path_cond_t cond, Value *callsite,
                                    Function *callee) {
  if (!cond)
    return getEmptyCond();
  if (cond->getOwnerFunc() == analyzed_func || cond->getOwnerFunc() == nullptr)
    return cond;

  auto it = imported_cond_cache_.find(cond);
  if (it != imported_cond_cache_.end())
    return it->second;

  path_cond_t imported = nullptr;
  if (isAlwaysSatisfied(cond)) {
    imported = getEmptyCond();
  } else if (!isSatisfiable(cond)) {
    imported = getFalseCond();
  } else {
    imported = internCond(std::unique_ptr<PathCond>(
        PathCond::createImportedAtom(analyzed_func, cond)));
  }
  imported_cond_cache_[cond] = imported;
  return imported;
}

path_cond_t PTGraph::getCallTargetCond(Value *called_value, Function *callee) {
  if (!called_value || !callee)
    return getEmptyCond();

  auto key = make_pair(called_value, callee);
  auto it = call_target_cond_cache_.find(key);
  if (it != call_target_cond_cache_.end())
    return it->second;

  path_cond_t cond = internCond(std::unique_ptr<PathCond>(
      PathCond::createCallTargetAtom(called_value, callee)));
  call_target_cond_cache_[key] = cond;
  return cond;
}

path_cond_t PTGraph::findOrCreateAndRegion(path_cond_t lhs, path_cond_t rhs) {
  if (!lhs)
    lhs = getEmptyCond();
  if (!rhs)
    rhs = getEmptyCond();
  lhs = localizePathCond(lhs);
  rhs = localizePathCond(rhs);
  if (isKnownFalse(lhs) || isKnownFalse(rhs))
    return getFalseCond();
  if (isKnownTrue(lhs))
    return rhs;
  if (isKnownTrue(rhs))
    return lhs;
  if (lhs == rhs)
    return lhs;

  auto key = canonicalCondPair(lhs, rhs);
  auto it = and_cond_cache_.find(key);
  if (it != and_cond_cache_.end())
    return it->second;

  auto ids = bdd_mgr_.combineAnd(getFormulaIds(key.first),
                                 getFormulaIds(key.second));
  path_cond_t cond = nullptr;
  if (ids.aig == PathCond::TRUE_BDD_ID || ids.bdd == PathCond::TRUE_BDD_ID) {
    cond = getEmptyCond();
  } else if (ids.aig == PathCond::FALSE_BDD_ID ||
             ids.bdd == PathCond::FALSE_BDD_ID) {
    cond = getFalseCond();
  } else {
    cond = lookupCachedCond(ids);
    if (!cond) {
      cond = internCondWithIds(
          ids, std::unique_ptr<PathCond>(
                   PathCond::createAnd(key.first, key.second)));
    }
  }
  and_cond_cache_[key] = cond;
  return cond;
}

path_cond_t PTGraph::findOrCreateOrRegion(path_cond_t lhs, path_cond_t rhs) {
  if (!lhs)
    lhs = getEmptyCond();
  if (!rhs)
    rhs = getEmptyCond();
  lhs = localizePathCond(lhs);
  rhs = localizePathCond(rhs);
  if (isKnownTrue(lhs) || isKnownTrue(rhs))
    return getEmptyCond();
  if (isKnownFalse(lhs))
    return rhs;
  if (isKnownFalse(rhs))
    return lhs;
  if (lhs == rhs)
    return lhs;

  auto key = canonicalCondPair(lhs, rhs);
  auto it = or_cond_cache_.find(key);
  if (it != or_cond_cache_.end())
    return it->second;

  auto ids =
      bdd_mgr_.combineOr(getFormulaIds(key.first), getFormulaIds(key.second));
  path_cond_t cond = nullptr;
  if (ids.aig == PathCond::TRUE_BDD_ID || ids.bdd == PathCond::TRUE_BDD_ID) {
    cond = getEmptyCond();
  } else if (ids.aig == PathCond::FALSE_BDD_ID ||
             ids.bdd == PathCond::FALSE_BDD_ID) {
    cond = getFalseCond();
  } else {
    cond = lookupCachedCond(ids);
    if (!cond) {
      cond = internCondWithIds(
          ids, std::unique_ptr<PathCond>(
                   PathCond::createOr(key.first, key.second)));
    }
  }
  or_cond_cache_[key] = cond;
  return cond;
}

path_cond_t PTGraph::findOrCreateNotRegion(path_cond_t cond) {
  if (!cond)
    return getFalseCond();
  cond = localizePathCond(cond);
  if (isKnownTrue(cond))
    return getFalseCond();
  if (isKnownFalse(cond))
    return getEmptyCond();
  if (cond->getKind() == PathCond::Kind::ValueAtom)
    return getValueCond(cond->getValue(), !cond->getSense());
  if (cond->getKind() == PathCond::Kind::BranchAtom) {
    if (path_cond_t complement = getComplementaryBranchCond(cond))
      return complement;
  }
  if (cond->getKind() == PathCond::Kind::Not)
    return cond->getLhs() ? cond->getLhs() : getEmptyCond();

  auto it = not_cond_cache_.find(cond);
  if (it != not_cond_cache_.end())
    return it->second;

  auto ids = bdd_mgr_.combineNot(getFormulaIds(cond));
  path_cond_t neg = nullptr;
  if (ids.aig == PathCond::TRUE_BDD_ID || ids.bdd == PathCond::TRUE_BDD_ID) {
    neg = getEmptyCond();
  } else if (ids.aig == PathCond::FALSE_BDD_ID ||
             ids.bdd == PathCond::FALSE_BDD_ID) {
    neg = getFalseCond();
  } else {
    neg = lookupCachedCond(ids);
    if (!neg) {
      neg = internCondWithIds(
          ids, std::unique_ptr<PathCond>(PathCond::createNot(cond)));
    }
  }
  not_cond_cache_[cond] = neg;
  return neg;
}

path_cond_t PTGraph::findOrCreateUnitPhiRegion(BasicBlock *cur_bb,
                                               BasicBlock *incoming_bb) {
  auto cache_key = std::make_pair(cur_bb, incoming_bb);
  auto cache_it = phi_region_cache.find(cache_key);
  if (cache_it != phi_region_cache.end())
    return cache_it->second;

  path_cond_t cond = getEmptyCond();
  if (incoming_bb) {
    if (IntraLotusAAConfig::lotus_use_full_phi_cond) {
      cond = getUnitRegion(incoming_bb);
    } else {
      BasicBlock *dom_bb = nullptr;
      if (dom_tree) {
        if (DomTreeNode *sink_node = dom_tree->getNode(cur_bb)) {
          if (DomTreeNode *idom_node = sink_node->getIDom())
            dom_bb = idom_node->getBlock();
        }
      }

      cond = dom_bb ? findOrCreateBBRegion(dom_bb, incoming_bb)
                    : getUnitRegion(incoming_bb);
    }

    if (auto *br = dyn_cast_or_null<BranchInst>(incoming_bb->getTerminator())) {
      if (br->isConditional())
        cond = findOrCreateAndRegion(cond, getCFGEdgeCond(incoming_bb, cur_bb));
    }
  }
  phi_region_cache[cache_key] = cond;
  return cond;
}
