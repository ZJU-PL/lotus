#include "Concurrency/OpenMP/OpenMPSemantics.h"

#include "Concurrency/API/ThreadAPI.h"
#include "Concurrency/OpenMP/OpenMPModel.h"

#include <deque>
#include <optional>
#include <set>

#include <llvm/Analysis/CFG.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>

using namespace llvm;

namespace OpenMP {

namespace {

DependType decodeDependType(uint64_t flags) {
  switch (flags) {
  case 0x1ULL:
    return DependType::IN;
  case 0x2ULL:
    return DependType::OUT;
  case 0x3ULL:
    return DependType::INOUT;
  case 0x4ULL:
    return DependType::MUTEXINOUTSET;
  case 0x8ULL:
    return DependType::INOUTSET;
  case 0x80ULL:
    return DependType::ALL_MEMORY;
  default:
    return DependType::UNKNOWN;
  }
}

const Value *stripValue(const Value *value) {
  return value ? value->stripPointerCasts() : nullptr;
}

std::pair<DependencySourceKind, DependencyProof>
classifyDependencyAddressEvidence(const Value *value) {
  if (!value) {
    return {DependencySourceKind::RegionSummary, DependencyProof::Unknown};
  }

  value = value->stripPointerCasts();
  if (isa<GlobalValue>(value) || isa<AllocaInst>(value)) {
    return {DependencySourceKind::DirectAddress, DependencyProof::Definite};
  }

  if (const auto *ce = dyn_cast<ConstantExpr>(value)) {
    if (ce->isCast() || ce->getOpcode() == Instruction::GetElementPtr) {
      const Value *base =
          getUnderlyingObject(ce->getOperand(0)->stripPointerCasts());
      if (isa<GlobalValue>(base) || isa<AllocaInst>(base)) {
        return {DependencySourceKind::DirectAddress, DependencyProof::Definite};
      }
    }
  }

  if (const auto *arg = dyn_cast<Argument>(value)) {
    return {DependencySourceKind::Iterator, arg->getType()->isPointerTy()
                                                ? DependencyProof::Possible
                                                : DependencyProof::Unknown};
  }

  return {DependencySourceKind::RegionSummary, DependencyProof::Possible};
}

const Value *canonicalizeDependencyAddress(const Value *value,
                                           const DataLayout &DL,
                                           int64_t &offset,
                                           bool &has_precise_offset) {
  offset = 0;
  has_precise_offset = false;
  if (!value) {
    return nullptr;
  }

  value = stripValue(value);
  if (const auto *load = dyn_cast<LoadInst>(value)) {
    value = load->getPointerOperand()->stripPointerCasts();
  }

  if (const Value *base_with_offset =
          GetPointerBaseWithConstantOffset(value, offset, DL)) {
    const Value *canonical_base = stripValue(base_with_offset);
    if (!(isa<GEPOperator>(value) && canonical_base == stripValue(value))) {
      has_precise_offset = true;
      return canonical_base;
    }
  }

  if (const Value *base = getUnderlyingObject(value)) {
    return stripValue(base);
  }
  return value;
}

bool decodeConstantDependency(const Constant *elt, Dependency &dep) {
  if (!elt) {
    return false;
  }
  const auto *cs = dyn_cast<ConstantStruct>(elt);
  if (!cs || cs->getNumOperands() < 3) {
    return false;
  }

  bool all_memory_sentinel = false;
  if (const auto *address_expr = dyn_cast<ConstantExpr>(cs->getOperand(0))) {
    if (address_expr->getOpcode() == Instruction::IntToPtr) {
      if (const auto *address_int =
              dyn_cast<ConstantInt>(address_expr->getOperand(0))) {
        all_memory_sentinel = address_int->isMinusOne();
      }
    }
  }
  dep.address = all_memory_sentinel ? nullptr : stripValue(cs->getOperand(0));
  dep.size = 0;
  dep.type = DependType::INOUT;

  if (const auto *len = dyn_cast<ConstantInt>(cs->getOperand(1))) {
    dep.size = len->getZExtValue();
  }
  if (const auto *flags = dyn_cast<ConstantInt>(cs->getOperand(2))) {
    dep.type = decodeDependType(flags->getZExtValue());
  }
  if (all_memory_sentinel) {
    dep.type = DependType::ALL_MEMORY;
  }
  return dep.address != nullptr || dep.type == DependType::ALL_MEMORY;
}

const Value *resolveDependencyListValue(const Value *value) {
  if (!value) {
    return nullptr;
  }
  std::deque<const Value *> worklist;
  std::set<const Value *> visited;
  const Value *resolved = nullptr;
  worklist.push_back(value);

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    if (!current || !visited.insert(current).second) {
      continue;
    }

    current = current->stripPointerCasts();
    if (const auto *load = dyn_cast<LoadInst>(current)) {
      worklist.push_back(load->getPointerOperand());
      continue;
    }
    if (const auto *store = dyn_cast<StoreInst>(current)) {
      worklist.push_back(store->getValueOperand());
      continue;
    }
    if (const auto *phi = dyn_cast<PHINode>(current)) {
      for (const Value *incoming : phi->incoming_values()) {
        worklist.push_back(incoming);
      }
      continue;
    }
    if (const auto *select = dyn_cast<SelectInst>(current)) {
      worklist.push_back(select->getTrueValue());
      worklist.push_back(select->getFalseValue());
      continue;
    }
    if (const auto *gep = dyn_cast<GEPOperator>(current)) {
      worklist.push_back(gep->getPointerOperand());
      continue;
    }

    if (isa<AllocaInst>(current) || isa<GlobalVariable>(current) ||
        isa<Argument>(current)) {
      bool saw_stored_pointer = false;
      for (const Use &use : current->uses()) {
        if (const auto *store = dyn_cast<StoreInst>(use.getUser())) {
          if (store->getPointerOperand()->stripPointerCasts() == current) {
            worklist.push_back(store->getValueOperand());
            saw_stored_pointer = true;
          }
        }
      }
      if (saw_stored_pointer) {
        continue;
      }
    }

    const Value *underlying = getUnderlyingObject(current);
    if (underlying) {
      current = underlying->stripPointerCasts();
    }
    if (!resolved) {
      resolved = current;
    } else if (resolved != current) {
      return nullptr;
    }
  }

  return resolved ? resolved : value->stripPointerCasts();
}

bool isBeforeInBlock(const Instruction *lhs, const Instruction *rhs) {
  if (!lhs || !rhs || lhs->getParent() != rhs->getParent()) {
    return false;
  }
  for (const Instruction &inst : *lhs->getParent()) {
    if (&inst == lhs) {
      return true;
    }
    if (&inst == rhs) {
      return false;
    }
  }
  return false;
}

bool mustHappenBefore(const Instruction *lhs, const Instruction *rhs);

// OpenMP task entry points are occasionally kept in a local, function-pointer
// slot before being passed to the runtime as an opaque i8*.  Resolve only the
// simple case where every reaching store is a direct function and all stores
// agree; more complex task objects still require field-sensitive analysis.
const Function *resolveTaskEntryFromLocalSlot(const Value *task_arg,
                                              const Instruction *task_call) {
  if (!task_arg || !task_call || !task_call->getFunction()) {
    return nullptr;
  }

  const Value *storage = getUnderlyingObject(task_arg);
  if (!isa_and_nonnull<AllocaInst>(storage)) {
    return nullptr;
  }

  const Function *entry = nullptr;
  for (const Instruction &inst : instructions(task_call->getFunction())) {
    const auto *store = dyn_cast<StoreInst>(&inst);
    if (!store || getUnderlyingObject(store->getPointerOperand()) != storage) {
      continue;
    }
    if (!mustHappenBefore(store, task_call)) {
      return nullptr;
    }

    const auto *candidate =
        dyn_cast<Function>(store->getValueOperand()->stripPointerCasts());
    if (!candidate || (entry && entry != candidate)) {
      return nullptr;
    }
    entry = candidate;
  }
  return entry;
}

bool mustHappenBefore(const Instruction *lhs, const Instruction *rhs) {
  if (!lhs || !rhs || lhs == rhs || lhs->getFunction() != rhs->getFunction()) {
    return false;
  }
  if (lhs->getParent() == rhs->getParent()) {
    return isBeforeInBlock(lhs, rhs);
  }

  const Function *func = lhs->getFunction();
  if (!func || func->isDeclaration()) {
    return false;
  }

  DominatorTree DT(*const_cast<Function *>(func));
  if (!DT.dominates(lhs, rhs)) {
    return false;
  }

  LoopInfo LI(DT);
  return !isPotentiallyReachable(rhs, lhs, nullptr, &DT, &LI);
}

std::pair<const Task *, const Task *> normalizeTaskPair(const Task *lhs,
                                                        const Task *rhs) {
  return lhs < rhs ? std::make_pair(lhs, rhs) : std::make_pair(rhs, lhs);
}

const Instruction *taskOrderingSite(const Task *task) {
  if (!task) {
    return nullptr;
  }
  return task->generating_context ? task->generating_context
                                  : task->task_create;
}

bool isTaskSiteInLoop(const Instruction *inst) {
  if (!inst || !inst->getFunction() || inst->getFunction()->isDeclaration()) {
    return false;
  }
  DominatorTree DT(*const_cast<Function *>(inst->getFunction()));
  LoopInfo LI(DT);
  return LI.getLoopFor(inst->getParent()) != nullptr;
}

bool canReachFunction(const Function *current, const Function *target,
                      std::set<const Function *> &visited) {
  if (!current || current->isDeclaration() || !visited.insert(current).second) {
    return false;
  }
  for (const Instruction &inst : instructions(current)) {
    const auto *call = dyn_cast<CallBase>(&inst);
    if (!call) {
      continue;
    }
    const Function *callee = call->getCalledFunction();
    if (!callee) {
      callee = dyn_cast<Function>(call->getCalledOperand()->stripPointerCasts());
    }
    if (!callee || callee->isDeclaration()) {
      continue;
    }
    if (callee == target || canReachFunction(callee, target, visited)) {
      return true;
    }
  }
  return false;
}

bool isRecursiveFunction(const Function *func) {
  if (!func || func->isDeclaration()) {
    return false;
  }
  std::set<const Function *> visited;
  visited.insert(func);
  for (const Instruction &inst : instructions(func)) {
    const auto *call = dyn_cast<CallBase>(&inst);
    if (!call) {
      continue;
    }
    const Function *callee = call->getCalledFunction();
    if (!callee) {
      callee = dyn_cast<Function>(call->getCalledOperand()->stripPointerCasts());
    }
    if (!callee || callee->isDeclaration()) {
      continue;
    }
    if (callee == func || canReachFunction(callee, func, visited)) {
      return true;
    }
  }
  return false;
}

bool taskBelongsToGroup(const Task *task, size_t taskgroup_id) {
  return task && taskgroup_id != 0 &&
         task->taskgroup_ids.count(taskgroup_id) != 0;
}

constexpr uint64_t kLibompTaskTiednessMask = 0x1ULL;
constexpr uint64_t kLibompTaskFinalMask = 0x2ULL;
constexpr uint64_t kLibompTaskMergedIf0Mask = 0x4ULL;
constexpr uint64_t kLibompTaskProxyMask = 0x10ULL;
constexpr uint64_t kLibompTaskDetachableMask = 0x40ULL;

struct PartialDependency {
  const Value *address = nullptr;
  uint64_t size = 0;
  uint64_t flags = 0;
  DependencySourceKind source_kind = DependencySourceKind::RegionSummary;
  DependencyProof proof = DependencyProof::Unknown;
  bool has_address = false;
  bool has_size = false;
  bool has_flags = false;
};

std::optional<uint64_t>
resolveDependencyCount(const Value *value, const Instruction *before,
                       std::set<const Value *> &visiting) {
  if (!value) {
    return std::nullopt;
  }

  value = value->stripPointerCasts();
  if (!visiting.insert(value).second) {
    return std::nullopt;
  }

  if (const auto *ci = dyn_cast<ConstantInt>(value)) {
    visiting.erase(value);
    return ci->getZExtValue();
  }

  if (const auto *cast = dyn_cast<CastInst>(value)) {
    auto resolved =
        resolveDependencyCount(cast->getOperand(0), before, visiting);
    visiting.erase(value);
    return resolved;
  }

  if (const auto *ce = dyn_cast<ConstantExpr>(value)) {
    if (ce->isCast()) {
      auto resolved =
          resolveDependencyCount(ce->getOperand(0), before, visiting);
      visiting.erase(value);
      return resolved;
    }
  }

  if (const auto *select = dyn_cast<SelectInst>(value)) {
    auto true_value =
        resolveDependencyCount(select->getTrueValue(), before, visiting);
    auto false_value =
        resolveDependencyCount(select->getFalseValue(), before, visiting);
    if (true_value && false_value && *true_value == *false_value) {
      visiting.erase(value);
      return true_value;
    }
    visiting.erase(value);
    return std::nullopt;
  }

  if (const auto *phi = dyn_cast<PHINode>(value)) {
    std::optional<uint64_t> resolved;
    for (const Value *incoming : phi->incoming_values()) {
      auto incoming_value = resolveDependencyCount(incoming, before, visiting);
      if (!incoming_value) {
        visiting.erase(value);
        return std::nullopt;
      }
      if (!resolved) {
        resolved = incoming_value;
        continue;
      }
      if (*resolved != *incoming_value) {
        visiting.erase(value);
        return std::nullopt;
      }
    }
    visiting.erase(value);
    return resolved;
  }

  if (const auto *load = dyn_cast<LoadInst>(value)) {
    const Value *storage = getUnderlyingObject(load->getPointerOperand());
    if (!storage) {
      storage = load->getPointerOperand()->stripPointerCasts();
    }

    if (const auto *gv = dyn_cast<GlobalVariable>(storage)) {
      if (const auto *init = dyn_cast<ConstantInt>(gv->getInitializer())) {
        visiting.erase(value);
        return init->getZExtValue();
      }
    }

    const Function *parent = before ? before->getFunction() : nullptr;
    if (!parent) {
      visiting.erase(value);
      return std::nullopt;
    }

    const StoreInst *latest_store = nullptr;
    for (const Instruction &inst : instructions(parent)) {
      const auto *store = dyn_cast<StoreInst>(&inst);
      if (!store || !mustHappenBefore(store, before)) {
        continue;
      }
      const Value *store_target =
          getUnderlyingObject(store->getPointerOperand());
      if (!store_target) {
        store_target = store->getPointerOperand()->stripPointerCasts();
      }
      if (store_target != storage) {
        continue;
      }
      if (!latest_store || mustHappenBefore(latest_store, store)) {
        latest_store = store;
      }
    }

    if (latest_store) {
      auto resolved = resolveDependencyCount(latest_store->getValueOperand(),
                                             before, visiting);
      visiting.erase(value);
      return resolved;
    }
  }

  visiting.erase(value);
  return std::nullopt;
}

std::optional<uint64_t> resolveDependencyCount(const Value *value,
                                               const Instruction *before) {
  std::set<const Value *> visiting;
  return resolveDependencyCount(value, before, visiting);
}

uint64_t getDependencyArrayCapacity(const Value *dep_base) {
  dep_base = dep_base ? dep_base->stripPointerCasts() : nullptr;
  if (!dep_base) {
    return 0;
  }

  if (const auto *gv = dyn_cast<GlobalVariable>(dep_base)) {
    if (const auto *array = dyn_cast<ArrayType>(gv->getValueType())) {
      return array->getNumElements();
    }
  }

  if (const auto *alloca = dyn_cast<AllocaInst>(dep_base)) {
    if (const auto *array = dyn_cast<ArrayType>(alloca->getAllocatedType())) {
      return array->getNumElements();
    }
  }

  return 0;
}

uint64_t getDependencyStartIndex(const Value *dep_root, const Value *dep_base) {
  if (!dep_root || !dep_base) {
    return 0;
  }

  dep_root = dep_root->stripPointerCasts();
  dep_base = dep_base->stripPointerCasts();
  if (dep_root == dep_base) {
    return 0;
  }

  const auto *gep = dyn_cast<GEPOperator>(dep_root);
  if (!gep || getUnderlyingObject(gep->getPointerOperand()) != dep_base) {
    return 0;
  }

  SmallVector<uint64_t, 4> indices;
  for (unsigned i = 0; i < gep->getNumIndices(); ++i) {
    const auto *ci = dyn_cast<ConstantInt>(gep->getOperand(i + 1));
    if (!ci) {
      return 0;
    }
    indices.push_back(ci->getZExtValue());
  }

  if (indices.empty()) {
    return 0;
  }
  return indices.back();
}

void seedPartialDependencies(std::map<uint64_t, PartialDependency> &partials,
                             const std::vector<Dependency> &deps,
                             uint64_t start_index) {
  for (size_t i = 0; i < deps.size(); ++i) {
    PartialDependency &partial = partials[start_index + i];
    partial.address = deps[i].address;
    partial.size = deps[i].size;
    partial.flags = deps[i].type == DependType::IN              ? 0x1
                    : deps[i].type == DependType::OUT           ? 0x2
                    : deps[i].type == DependType::MUTEXINOUTSET ? 0x4
                    : deps[i].type == DependType::INOUTSET      ? 0x8
                    : deps[i].type == DependType::ALL_MEMORY    ? 0x80
                    : deps[i].type == DependType::INOUT         ? 0x3
                                                                : 0;
    partial.source_kind = deps[i].source_kind;
    partial.proof = deps[i].proof;
    partial.has_address = deps[i].address != nullptr;
    partial.has_size = true;
    partial.has_flags = true;
  }
}

} // namespace

OpenMPSemantics::OpenMPSemantics(Module &module) : m_module(module) {}

void OpenMPSemantics::analyze() {
  m_tasks.clear();
  m_inst_to_task.clear();
  m_handle_to_task.clear();
  m_wait_boundaries.clear();
  m_wait_boundary_infos.clear();
  m_entities.clear();
  m_events.clear();
  m_task_events.clear();
  m_task_completion_events.clear();
  m_relations.clear();
  m_summary = AnalysisSummary{};
  m_nested_depth = 0;
  m_next_scheduling_context_id = 1;
  m_next_entity_id = 1;
  m_next_taskgroup_id = 1;
  m_region_nesting_depth.clear();
  m_omp_relevant_functions.clear();
  m_deferred_imprecise_conflict_count = 0;
  m_deferred_reason_counts.clear();
  identifySemanticStructure();
  if (m_omp_relevant_functions.empty()) {
    return;
  }
  m_summary.task_count = m_tasks.size();
  m_summary.task_with_dependencies_count = 0;
  m_summary.included_task_count = 0;
  m_summary.final_task_count = 0;
  m_summary.untied_task_count = 0;
  m_summary.detached_task_count = 0;
  m_summary.taskloop_count = 0;
  for (const auto &task : m_tasks) {
    m_summary.task_with_dependencies_count += task->has_dependency_submission;
    m_summary.included_task_count +=
        task->has_included_submission && !task->has_deferred_submission;
    m_summary.final_task_count += task->is_final;
    m_summary.untied_task_count += task->is_untied;
    m_summary.detached_task_count += task->is_detached;
    m_summary.taskloop_count += task->is_taskloop;
  }
  rebuildWaitBoundaryViews();
  attachDataSharingFacts();
  buildTaskRelations();
  m_summary.task_count = m_tasks.size();
  m_summary.wait_boundary_count = 0;
  m_summary.partial_wait_boundary_count = 0;
  for (const WaitBoundaryInfo &info : m_wait_boundary_infos) {
    if (info.kind != WaitBoundaryInfo::Kind::Flush) {
      ++m_summary.wait_boundary_count;
    }
    if (info.is_partial_wait) {
      if (info.kind != WaitBoundaryInfo::Kind::Flush) {
        ++m_summary.partial_wait_boundary_count;
      }
    }
  }
  m_summary.semantic_entity_count = m_entities.size();
  m_summary.semantic_event_count = m_events.size();
}

const Task *OpenMPSemantics::getTaskForCreate(const Instruction *inst) const {
  auto it = m_inst_to_task.find(inst);
  return it != m_inst_to_task.end() ? it->second : nullptr;
}

const Task *OpenMPSemantics::getTaskForHandle(const Value *value) const {
  const Value *handle = canonicalizeTaskHandle(value);
  if (!handle) {
    return nullptr;
  }
  auto it = m_handle_to_task.find(handle);
  return it == m_handle_to_task.end() ? nullptr : it->second;
}

bool OpenMPSemantics::isNestedRegion(size_t region_id) const {
  auto it = m_region_nesting_depth.find(region_id);
  if (it == m_region_nesting_depth.end()) {
    return false;
  }
  return it->second > 1;
}

size_t OpenMPSemantics::getRegionNestingDepth(size_t region_id) const {
  auto it = m_region_nesting_depth.find(region_id);
  if (it == m_region_nesting_depth.end()) {
    return 0;
  }
  return it->second;
}

bool OpenMPSemantics::hasLinearControlFlow(const llvm::Function *func) const {
  if (!func || func->isDeclaration()) {
    return true;
  }

  for (const BasicBlock &bb : *func) {
    if (&bb != &func->getEntryBlock() && !bb.hasNPredecessors(1)) {
      return false;
    }

    const Instruction *term = bb.getTerminator();
    if (!term) {
      continue;
    }
    if (isa<ReturnInst>(term) || isa<ResumeInst>(term) ||
        isa<UnreachableInst>(term)) {
      continue;
    }
    if (term->getNumSuccessors() != 1) {
      return false;
    }
    if (const auto *branch = dyn_cast<BranchInst>(term)) {
      if (!branch->isUnconditional()) {
        return false;
      }
      continue;
    }
    return false;
  }

  return true;
}

const std::vector<DataSharingEntry> &
OpenMPSemantics::getDataSharingEntriesForEntity(size_t entity_id) const {
  static const std::vector<DataSharingEntry> kEmpty;
  for (const SemanticEntity &entity : m_entities) {
    if (entity.id == entity_id) {
      return entity.data_sharing_entries;
    }
  }
  return kEmpty;
}

size_t OpenMPSemantics::addEntity(SemanticEntityKind kind,
                                  const Instruction *anchor_inst,
                                  const Function *func,
                                  size_t scheduling_context_id,
                                  size_t parent_id, size_t region_id,
                                  size_t phase_id, size_t taskgroup_id) {
  SemanticEntity entity;
  entity.id = m_next_entity_id++;
  entity.kind = kind;
  entity.anchor_inst = anchor_inst;
  entity.function = func;
  entity.scheduling_context_id = scheduling_context_id;
  entity.parent_id = parent_id;
  entity.region_id = region_id;
  entity.phase_id = phase_id;
  entity.taskgroup_id = taskgroup_id;
  m_entities.push_back(entity);
  return entity.id;
}

void OpenMPSemantics::addEvent(SemanticEventKind kind, const Instruction *inst,
                               size_t entity_id, size_t scheduling_context_id,
                               size_t sequence_index, size_t event_order,
                               size_t region_id, size_t phase_id,
                               bool is_partial) {
  SemanticEvent event;
  event.entity_id = entity_id;
  event.kind = kind;
  event.inst = inst;
  event.scheduling_context_id = scheduling_context_id;
  event.sequence_index = sequence_index;
  event.event_order = event_order;
  event.region_id = region_id;
  event.phase_id = phase_id;
  event.is_partial = is_partial;
  m_events.push_back(event);
}

void OpenMPSemantics::addTaskEvent(
    OpenMPTaskEvent::Kind kind, const Instruction *inst,
    size_t scheduling_context_id, size_t sequence_index, size_t event_order,
    size_t phase_id, size_t taskgroup_id, size_t region_id,
    size_t semantic_entity_id, const Task *task,
    WaitBoundaryInfo::Kind boundary_kind, bool is_partial_wait,
    bool is_taskgroup_end, std::vector<Dependency> dependencies) {
  OpenMPTaskEvent event;
  event.kind = kind;
  event.inst = inst;
  event.task = task;
  event.scheduling_context_id = scheduling_context_id;
  event.sequence_index = sequence_index;
  event.event_order = event_order;
  event.phase_id = phase_id;
  event.taskgroup_id = taskgroup_id;
  event.region_id = region_id;
  event.semantic_entity_id = semantic_entity_id;
  event.is_partial_wait = is_partial_wait;
  event.is_taskgroup_end = is_taskgroup_end;
  event.boundary_kind = boundary_kind;
  event.dependencies = std::move(dependencies);
  m_task_events.push_back(std::move(event));
}

void OpenMPSemantics::rebuildWaitBoundaryViews() {
  m_wait_boundaries.clear();
  m_wait_boundary_infos.clear();

  for (const OpenMPTaskEvent &event : m_task_events) {
    switch (event.kind) {
    case OpenMPTaskEvent::Kind::Taskwait:
    case OpenMPTaskEvent::Kind::TaskwaitDeps:
    case OpenMPTaskEvent::Kind::TaskgroupEnd:
    case OpenMPTaskEvent::Kind::Barrier:
    case OpenMPTaskEvent::Kind::Flush:
    case OpenMPTaskEvent::Kind::DoacrossWait:
    case OpenMPTaskEvent::Kind::TargetBoundary:
    case OpenMPTaskEvent::Kind::ReductionNowaitBoundary:
      break;
    default:
      continue;
    }

    WaitBoundaryInfo info;
    info.inst = event.inst;
    info.scheduling_context_id = event.scheduling_context_id;
    info.sequence_index = event.sequence_index;
    info.event_order = event.event_order;
    info.phase_id = event.phase_id;
    info.taskgroup_id = event.taskgroup_id;
    info.region_id = event.region_id;
    info.semantic_entity_id = event.semantic_entity_id;
    info.is_taskgroup_end = event.is_taskgroup_end;
    info.is_partial_wait = event.is_partial_wait;
    info.kind = event.boundary_kind;
    m_wait_boundary_infos.push_back(info);

    WaitBoundaryRecord record;
    record.inst = event.inst;
    record.scheduling_context_id = event.scheduling_context_id;
    record.sequence_index = event.sequence_index;
    record.event_order = event.event_order;
    record.sibling_group = event.phase_id;
    record.taskgroup_id = event.taskgroup_id;
    record.semantic_entity_id = event.semantic_entity_id;
    record.is_taskgroup_end = event.is_taskgroup_end;
    record.is_partial_wait = event.is_partial_wait;
    record.kind = event.boundary_kind;
    record.dependencies = event.dependencies;
    m_wait_boundaries[event.scheduling_context_id].push_back(std::move(record));
  }
}

void OpenMPSemantics::identifySemanticStructure() {
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  if (!api->getConfig().enable_openmp()) {
    return;
  }

  std::unordered_map<const Function *, std::vector<const Function *>> callers;
  std::unordered_set<const Function *> direct_omp_funcs;
  std::unordered_set<const Function *> directly_called;

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }
    bool has_direct_omp = false;
    for (const BasicBlock &bb : func) {
      for (const Instruction &inst : bb) {
        const auto *call = dyn_cast<CallBase>(&inst);
        if (!call) {
          continue;
        }
        const Function *callee = api->getCallee(call);
        if (callee && !callee->isDeclaration()) {
          directly_called.insert(callee);
          callers[callee].push_back(&func);
        }
        const ThreadAPI::TD_TYPE type = api->getType(call);
        const ThreadAPI::RuntimeLibrary library = api->getRuntimeLibrary(call);
        if (library == ThreadAPI::RuntimeLibrary::OpenMP ||
            (callee && callee->hasName() &&
             (OpenMPModel::isOpenMP(callee->getName()) ||
              callee->getName().startswith("__kmpc_atomic_"))) ||
            (type >= ThreadAPI::TD_OMP_TASK &&
             type <= ThreadAPI::TD_OMP_DOACROSS_SUBMIT)) {
          has_direct_omp = true;
        }
        if (library == ThreadAPI::RuntimeLibrary::OpenMP &&
            api->isForkLike(call)) {
          if (const auto *fork_target =
                  dyn_cast_or_null<Function>(api->getForkedFun(call))) {
            if (!fork_target->isDeclaration()) {
              directly_called.insert(fork_target);
              callers[fork_target].push_back(&func);
            }
          }
        }
        if (type == ThreadAPI::TD_OMP_TASK_WITH_DEPS ||
            type == ThreadAPI::TD_OMP_TASK ||
            type == ThreadAPI::TD_OMP_TASKLOOP) {
          if (const Function *task_function = extractTaskFunction(call)) {
            if (!task_function->isDeclaration()) {
              directly_called.insert(task_function);
              callers[task_function].push_back(&func);
            }
          }
        }
      }
    }
    if (has_direct_omp) {
      direct_omp_funcs.insert(&func);
    }
  }

  // Fast exit: modules with no OpenMP runtime calls have no OpenMP semantics
  // to construct; avoid walking the entire call graph exponentially.
  if (direct_omp_funcs.empty()) {
    return;
  }

  // Propagate OpenMP relevance backwards along direct call edges so
  // scanSchedulingContext only traverses functions that can reach OpenMP calls.
  std::deque<const Function *> worklist(direct_omp_funcs.begin(),
                                        direct_omp_funcs.end());
  m_omp_relevant_functions.insert(direct_omp_funcs.begin(),
                                  direct_omp_funcs.end());
  while (!worklist.empty()) {
    const Function *cur = worklist.front();
    worklist.pop_front();
    auto it = callers.find(cur);
    if (it == callers.end()) {
      continue;
    }
    for (const Function *caller : it->second) {
      if (m_omp_relevant_functions.insert(caller).second) {
        worklist.push_back(caller);
      }
    }
  }

  std::vector<const Function *> roots;
  for (const Function &func : m_module) {
    if (!func.isDeclaration() && m_omp_relevant_functions.count(&func) &&
        !directly_called.count(&func)) {
      roots.push_back(&func);
    }
  }
  if (roots.empty()) {
    for (const Function &func : m_module) {
      if (!func.isDeclaration() && m_omp_relevant_functions.count(&func)) {
        roots.push_back(&func);
      }
    }
  }

  for (const Function *root : roots) {
    TraversalState state;
    state.scheduling_context_id = m_next_scheduling_context_id++;
    state.scheduling_context_entity_id =
        addEntity(SemanticEntityKind::SchedulingContext, nullptr, root,
                  state.scheduling_context_id, 0, 0, 0, 0);
    state.phase_stack.push_back(0);
    std::set<const Function *> call_stack;
    scanSchedulingContext(root, state, call_stack);
  }
}

void OpenMPSemantics::attachDataSharingFacts() {
  DataSharingAnalysis data_sharing(m_module);
  data_sharing.analyze();

  auto entriesForFunction = [&](const Function *func) {
    return func ? data_sharing.getEntriesForRegion(func)
                : std::vector<DataSharingEntry>{};
  };

  for (auto &entity : m_entities) {
    entity.data_sharing_entries = entriesForFunction(entity.function);
  }

  for (auto &task_uptr : m_tasks) {
    Task *task = task_uptr.get();
    if (!task) {
      continue;
    }
    task->data_sharing_entries = entriesForFunction(task->task_function);
    if (task->task_function &&
        task->data_sharing_entries.size() < task->task_function->arg_size()) {
      ++m_deferred_reason_counts["omp_data_sharing_capture_unresolved"];
    }
    for (SemanticEntity &entity : m_entities) {
      if (entity.id == task->semantic_entity_id &&
          entity.data_sharing_entries.empty()) {
        entity.data_sharing_entries = task->data_sharing_entries;
        break;
      }
    }
  }
}

void OpenMPSemantics::buildTaskRelations() {
  auto recordRelation =
      [&](const Task *lhs, const Task *rhs, concurrency::RelationKind kind,
          concurrency::ProofStrength proof, StringRef reason) {
        if (!lhs || !rhs || lhs == rhs) {
          return;
        }
        auto key = normalizeTaskPair(lhs, rhs);
        concurrency::Relation relation;
        relation.kind = kind;
        relation.proof = proof;
        relation.reason = reason.str();
        concurrency::addRelationEvidence(relation, relation.reason);

        auto it = m_relations.find(key);
        if (it == m_relations.end() ||
            concurrency::relationPriority(kind) >
                concurrency::relationPriority(it->second.kind)) {
          m_relations[key] = std::move(relation);
        } else if (it->second.kind == kind) {
          concurrency::mergeSameKindRelation(it->second, relation);
        }
      };

  auto deferredPartialWaitReason = [&](const Task *lhs,
                                       const Task *rhs) -> StringRef {
    if (!lhs || !rhs ||
        lhs->scheduling_context_id != rhs->scheduling_context_id) {
      return "";
    }
    const size_t earlier = std::min(lhs->event_order, rhs->event_order);
    const size_t later = std::max(lhs->event_order, rhs->event_order);
    for (const OpenMPTaskEvent &event : m_task_events) {
      if (event.scheduling_context_id != lhs->scheduling_context_id ||
          !event.is_partial_wait || !event.dependencies.empty()) {
        continue;
      }
      if (earlier < event.event_order && event.event_order <= later) {
        switch (event.boundary_kind) {
        case WaitBoundaryInfo::Kind::Flush:
          return "omp_flush_witness_required";
        case WaitBoundaryInfo::Kind::DoacrossWait:
          return "omp_doacross_partial";
        case WaitBoundaryInfo::Kind::TargetNowait:
          return "omp_target_nowait_partial";
        case WaitBoundaryInfo::Kind::TargetDataNowait:
          return "omp_target_data_nowait_partial";
        case WaitBoundaryInfo::Kind::ReduceNowait:
          return "omp_reduction_nowait_partial";
        default:
          return "omp_taskwait_deps_partial";
        }
      }
    }
    return "";
  };

  auto hasUniqueMatchingDoacrossSubmit = [&](const OpenMPTaskEvent &boundary) {
    if (boundary.boundary_kind != WaitBoundaryInfo::Kind::DoacrossWait ||
        boundary.dependencies.empty()) {
      return true;
    }

    const OpenMPTaskEvent *match = nullptr;
    for (const OpenMPTaskEvent &event : m_task_events) {
      if (event.kind != OpenMPTaskEvent::Kind::DoacrossSubmit ||
          event.scheduling_context_id != boundary.scheduling_context_id ||
          !event.inst || !boundary.inst ||
          !mustHappenBefore(event.inst, boundary.inst)) {
        continue;
      }

      bool conflicts = false;
      for (const Dependency &submit_dep : event.dependencies) {
        for (const Dependency &wait_dep : boundary.dependencies) {
          if (classifyDependencyConflict(submit_dep, wait_dep) ==
              DependencyConflict::MustConflict) {
            conflicts = true;
            break;
          }
        }
        if (conflicts) {
          break;
        }
      }

      if (!conflicts) {
        continue;
      }
      if (match) {
        ++m_deferred_reason_counts["omp_doacross_submit_ambiguous"];
        return false;
      }
      match = &event;
    }

    if (!match) {
      ++m_deferred_reason_counts["omp_doacross_submit_missing"];
      return false;
    }
    return true;
  };

  for (size_t i = 0; i < m_tasks.size(); ++i) {
    Task *task_i = m_tasks[i].get();
    for (size_t j = i + 1; j < m_tasks.size(); ++j) {
      Task *task_j = m_tasks[j].get();
      if (task_i->scheduling_context_id != task_j->scheduling_context_id) {
        continue;
      }
      bool saw_conflict = false;
      bool saw_mutex_exclusion = false;
      bool saw_unknown_conflict = false;
      for (const Dependency &dep_i : task_i->dependencies) {
        for (const Dependency &dep_j : task_j->dependencies) {
          DependencyConflict conflict =
              classifyDependencyConflict(dep_i, dep_j);
          if (conflict == DependencyConflict::MustConflict) {
            saw_conflict = true;
            saw_mutex_exclusion =
                saw_mutex_exclusion || isMutexLikeExclusion(dep_i, dep_j);
          } else if (conflict == DependencyConflict::MayConflict ||
                     conflict == DependencyConflict::Unknown) {
            saw_unknown_conflict = true;
          }
        }
      }

      if (!saw_conflict) {
        if (saw_unknown_conflict) {
          ++m_deferred_reason_counts["omp_depend_may_conflict"];
          recordRelation(
              task_i, task_j, concurrency::RelationKind::UnknownDueToModelGap,
              concurrency::ProofStrength::May, "omp_depend_may_conflict");
        } else if (StringRef deferred_reason =
                       deferredPartialWaitReason(task_i, task_j);
                   !deferred_reason.empty()) {
          ++m_deferred_reason_counts[deferred_reason.str()];
          recordRelation(task_i, task_j,
                         concurrency::RelationKind::UnknownDueToModelGap,
                         concurrency::ProofStrength::Unknown,
                         deferred_reason);
        }
        continue;
      }

      if (saw_mutex_exclusion) {
        task_i->exclusions.insert(task_j);
        task_j->exclusions.insert(task_i);
        recordRelation(task_i, task_j,
                       concurrency::RelationKind::MutuallyExclusive,
                       concurrency::ProofStrength::Must, "omp_mutexinoutset");
        continue;
      }

      if (mustHappenBefore(taskOrderingSite(task_i),
                           taskOrderingSite(task_j))) {
        task_i->successors.insert(task_j);
        task_j->predecessors.insert(task_i);
        recordRelation(task_i, task_j,
                       concurrency::RelationKind::MustHappenBefore,
                       concurrency::ProofStrength::Must, "omp_depend_ordered");
      } else if (mustHappenBefore(taskOrderingSite(task_j),
                                  taskOrderingSite(task_i))) {
        task_j->successors.insert(task_i);
        task_i->predecessors.insert(task_j);
        recordRelation(task_i, task_j,
                       concurrency::RelationKind::MustHappenBefore,
                       concurrency::ProofStrength::Must, "omp_depend_ordered");
      } else {
        recordRelation(
            task_i, task_j, concurrency::RelationKind::UnknownDueToModelGap,
            concurrency::ProofStrength::Unknown, "omp_nonlexical_task_order");
      }
    }
  }

  auto selectiveReasonForBoundary = [](WaitBoundaryInfo::Kind kind) {
    switch (kind) {
    case WaitBoundaryInfo::Kind::Flush:
      return std::make_pair(StringRef("omp_flush_selective"),
                            StringRef("omp_flush_witness_required"));
    case WaitBoundaryInfo::Kind::DoacrossWait:
      return std::make_pair(StringRef("omp_doacross_selective"),
                            StringRef("omp_doacross_partial"));
    case WaitBoundaryInfo::Kind::TargetNowait:
      return std::make_pair(StringRef("omp_target_nowait_selective"),
                            StringRef("omp_target_nowait_partial"));
    case WaitBoundaryInfo::Kind::TargetDataNowait:
      return std::make_pair(StringRef("omp_target_data_nowait_selective"),
                            StringRef("omp_target_data_nowait_partial"));
    case WaitBoundaryInfo::Kind::ReduceNowait:
      return std::make_pair(StringRef("omp_reduction_nowait_selective"),
                            StringRef("omp_reduction_nowait_partial"));
    default:
      return std::make_pair(StringRef("omp_taskwait_deps_selective"),
                            StringRef("omp_taskwait_deps_partial"));
    }
  };

  for (const OpenMPTaskEvent &boundary : m_task_events) {
    switch (boundary.kind) {
    case OpenMPTaskEvent::Kind::Taskwait:
    case OpenMPTaskEvent::Kind::TaskwaitDeps:
    case OpenMPTaskEvent::Kind::TaskgroupEnd:
    case OpenMPTaskEvent::Kind::Barrier:
    case OpenMPTaskEvent::Kind::Flush:
    case OpenMPTaskEvent::Kind::DoacrossWait:
    case OpenMPTaskEvent::Kind::TargetBoundary:
    case OpenMPTaskEvent::Kind::ReductionNowaitBoundary:
      break;
    default:
      continue;
    }

    for (const auto &lhs : m_tasks) {
      if (boundary.is_taskgroup_end) {
        if (!taskBelongsToGroup(lhs.get(), boundary.taskgroup_id) ||
            !mustHappenBefore(taskOrderingSite(lhs.get()), boundary.inst)) {
          continue;
        }
      } else if (lhs->scheduling_context_id != boundary.scheduling_context_id ||
                 lhs->event_order >= boundary.event_order) {
        continue;
      }
      if (!boundary.is_taskgroup_end && !boundary.is_partial_wait &&
          lhs->phase_id != boundary.phase_id) {
        continue;
      }
      for (const auto &rhs : m_tasks) {
        if (rhs->scheduling_context_id != boundary.scheduling_context_id ||
            rhs.get() == lhs.get() || rhs->event_order < boundary.event_order) {
          continue;
        }
        if (!boundary.is_taskgroup_end && !boundary.is_partial_wait &&
            rhs->phase_id <= boundary.phase_id) {
          continue;
        }
        if (boundary.is_partial_wait) {
          if (boundary.boundary_kind == WaitBoundaryInfo::Kind::DoacrossWait &&
              !hasUniqueMatchingDoacrossSubmit(boundary)) {
            recordRelation(lhs.get(), rhs.get(),
                           concurrency::RelationKind::UnknownDueToModelGap,
                           concurrency::ProofStrength::Unknown,
                           "omp_doacross_submit_missing");
            continue;
          }
          bool lhs_selected = false;
          bool rhs_selected = false;
          for (const Dependency &wait_dep : boundary.dependencies) {
            for (const Dependency &lhs_dep : lhs->dependencies) {
              if (classifyDependencyConflict(lhs_dep, wait_dep) ==
                  DependencyConflict::MustConflict) {
                lhs_selected = true;
                break;
              }
            }
            for (const Dependency &rhs_dep : rhs->dependencies) {
              if (classifyDependencyConflict(rhs_dep, wait_dep) ==
                  DependencyConflict::MustConflict) {
                rhs_selected = true;
                break;
              }
            }
            if (lhs_selected && rhs_selected) {
              break;
            }
          }
          StringRef selective_reason = "omp_taskwait_deps_selective";
          StringRef deferred_reason = "omp_taskwait_deps_partial";
          std::tie(selective_reason, deferred_reason) =
              selectiveReasonForBoundary(boundary.boundary_kind);
          if (!boundary.dependencies.empty() && lhs_selected && rhs_selected &&
              mustHappenBefore(taskOrderingSite(lhs.get()), boundary.inst) &&
              mustHappenBefore(boundary.inst, taskOrderingSite(rhs.get()))) {
            lhs->successors.insert(rhs.get());
            rhs->predecessors.insert(lhs.get());
            recordRelation(lhs.get(), rhs.get(),
                           concurrency::RelationKind::SelectiveHappenBefore,
                           concurrency::ProofStrength::Must, selective_reason);
          } else {
            ++m_deferred_reason_counts[deferred_reason.str()];
            recordRelation(lhs.get(), rhs.get(),
                           concurrency::RelationKind::UnknownDueToModelGap,
                           concurrency::ProofStrength::Unknown,
                           deferred_reason);
          }
          continue;
        }
        const bool lhs_before_boundary =
            boundary.is_taskgroup_end ||
            mustHappenBefore(taskOrderingSite(lhs.get()), boundary.inst);
        if (lhs_before_boundary &&
            mustHappenBefore(boundary.inst, taskOrderingSite(rhs.get()))) {
          lhs->successors.insert(rhs.get());
          rhs->predecessors.insert(lhs.get());
          recordRelation(lhs.get(), rhs.get(),
                         concurrency::RelationKind::MustHappenBefore,
                         concurrency::ProofStrength::Must, "omp_wait_boundary");
        } else {
          recordRelation(lhs.get(), rhs.get(),
                         concurrency::RelationKind::UnknownDueToModelGap,
                         concurrency::ProofStrength::Unknown,
                         "omp_conditional_wait_boundary");
        }
      }
    }
  }

  for (const TaskCompletionEvent &event : m_task_completion_events) {
    if (!event.task || !event.inst) {
      continue;
    }
    Task *completed_task = const_cast<Task *>(event.task);
    if (!completed_task) {
      continue;
    }
    for (const auto &rhs : m_tasks) {
      if (!rhs || rhs.get() == event.task ||
          rhs->scheduling_context_id != event.scheduling_context_id ||
          rhs->event_order < event.event_order) {
        continue;
      }
      if (!mustHappenBefore(event.inst, taskOrderingSite(rhs.get()))) {
        continue;
      }
      completed_task->successors.insert(rhs.get());
      rhs->predecessors.insert(completed_task);
      recordRelation(completed_task, rhs.get(),
                     concurrency::RelationKind::MustHappenBefore,
                     concurrency::ProofStrength::Must,
                     "omp_detached_task_completion");
    }
  }
}

void OpenMPSemantics::scanSchedulingContext(
    const Function *func, TraversalState &state,
    std::set<const Function *> &call_stack) {
  if (!func || func->isDeclaration() || !call_stack.insert(func).second) {
    return;
  }

  ThreadAPI *api = ThreadAPI::getThreadAPI();
  if (!api->getConfig().enable_openmp()) {
    call_stack.erase(func);
    return;
  }
  const bool precise_phase_tracking = hasLinearControlFlow(func);
  bool recorded_cfg_model_gap = false;
  auto currentPhaseToken = [&state]() -> size_t {
    return state.phase_stack.empty() ? 0 : state.phase_stack.back();
  };
  auto currentRegionId = [&state]() -> size_t {
    return state.region_stack.empty() ? 0 : state.region_stack.back().id;
  };
  auto currentRegionEntityId = [&state]() -> size_t {
    return state.region_stack.empty() ? state.scheduling_context_entity_id
                                      : state.region_stack.back().entity_id;
  };
  auto nextEventOrder = [&state]() -> size_t {
    return state.next_event_order++;
  };
  auto advanceCurrentPhase = [&state, precise_phase_tracking]() {
    if (!precise_phase_tracking) {
      return;
    }
    if (state.phase_stack.empty()) {
      state.phase_stack.push_back(state.next_phase_token++);
    } else {
      state.phase_stack.back() = state.next_phase_token++;
    }
  };
  auto pushRegion = [&](WaitBoundaryInfo::Kind kind,
                        SemanticEntityKind entity_kind,
                        const Instruction *anchor = nullptr) {
    TraversalState::RegionFrame frame;
    frame.id = state.next_region_id++;
    frame.kind = kind;
    frame.entity_id = addEntity(
        entity_kind, anchor, func, state.scheduling_context_id,
        currentRegionEntityId(), frame.id, currentPhaseToken(),
        state.taskgroup_stack.empty() ? 0 : state.taskgroup_stack.back());
    if (precise_phase_tracking) {
      state.region_stack.push_back(frame);
    }
    const size_t event_order = nextEventOrder();
    addEvent(SemanticEventKind::RegionBegin, anchor, frame.entity_id,
             state.scheduling_context_id, state.sequence_index, event_order,
             frame.id, currentPhaseToken());
    return frame;
  };
  auto popRegion = [&](WaitBoundaryInfo::Kind kind, const Instruction *anchor) {
    if (!precise_phase_tracking) {
      ++m_deferred_reason_counts["omp_region_end_cfg_deferred"];
      return TraversalState::RegionFrame{};
    }
    if (state.region_stack.empty()) {
      ++m_deferred_reason_counts["omp_region_end_unmatched"];
      return TraversalState::RegionFrame{};
    }
    if (state.region_stack.back().kind == kind) {
      TraversalState::RegionFrame frame = state.region_stack.back();
      state.region_stack.pop_back();
      const size_t event_order = nextEventOrder();
      addEvent(SemanticEventKind::RegionEnd, anchor, frame.entity_id,
               state.scheduling_context_id, state.sequence_index, event_order,
               frame.id, currentPhaseToken());
      return frame;
    }
    for (size_t idx = state.region_stack.size(); idx > 0; --idx) {
      if (state.region_stack[idx - 1].kind != kind) {
        continue;
      }
      ++m_deferred_reason_counts["omp_region_mismatched_end"];
      const size_t stale_frames = state.region_stack.size() - idx;
      if (stale_frames != 0) {
        m_deferred_reason_counts["omp_region_stale_frames_dropped"] +=
            stale_frames;
      }
      for (size_t stale_idx = state.region_stack.size(); stale_idx > idx - 1;
           --stale_idx) {
        const TraversalState::RegionFrame &frame =
            state.region_stack[stale_idx - 1];
        const size_t event_order = nextEventOrder();
        addEvent(SemanticEventKind::RegionEnd, anchor, frame.entity_id,
                 state.scheduling_context_id, state.sequence_index, event_order,
                 frame.id, currentPhaseToken());
      }
      TraversalState::RegionFrame frame = state.region_stack[idx - 1];
      state.region_stack.erase(state.region_stack.begin() + (idx - 1),
                               state.region_stack.end());
      return frame;
    }
    ++m_deferred_reason_counts["omp_region_end_unmatched"];
    return TraversalState::RegionFrame{};
  };
  auto recordBoundary = [&](const CallBase *call, WaitBoundaryInfo::Kind kind,
                            bool partial_wait = false,
                            bool taskgroup_end = false) {
    partial_wait = partial_wait || !precise_phase_tracking;
    const size_t entity_id = addEntity(
        SemanticEntityKind::WaitBoundary, call, func,
        state.scheduling_context_id, currentRegionEntityId(), currentRegionId(),
        currentPhaseToken(),
        state.taskgroup_stack.empty() ? 0 : state.taskgroup_stack.back());
    WaitBoundaryInfo info;
    info.inst = call;
    info.scheduling_context_id = state.scheduling_context_id;
    info.sequence_index = state.sequence_index;
    info.event_order = nextEventOrder();
    info.phase_id = currentPhaseToken();
    info.region_id = currentRegionId();
    info.semantic_entity_id = entity_id;
    info.kind = kind;
    info.is_partial_wait = partial_wait;
    info.is_taskgroup_end = taskgroup_end;
    m_wait_boundary_infos.push_back(info);
    addEvent(
        kind == WaitBoundaryInfo::Kind::Barrier ? SemanticEventKind::Barrier
        : kind == WaitBoundaryInfo::Kind::Flush ? SemanticEventKind::Flush
        : kind == WaitBoundaryInfo::Kind::Target ||
                kind == WaitBoundaryInfo::Kind::TargetNowait ||
                kind == WaitBoundaryInfo::Kind::TargetData ||
                kind == WaitBoundaryInfo::Kind::TargetDataNowait
            ? SemanticEventKind::TargetLaunch
            : SemanticEventKind::Boundary,
        call, entity_id, state.scheduling_context_id, state.sequence_index,
        info.event_order, currentRegionId(), currentPhaseToken(), partial_wait);
    return info;
  };

  for (const BasicBlock &bb : *func) {
    for (const Instruction &inst : bb) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }

      const Function *callee = api->getCallee(call);
      ThreadAPI::TD_TYPE type = api->getType(call);
      ThreadAPI::RuntimeLibrary library = api->getRuntimeLibrary(call);
      if (!precise_phase_tracking && !recorded_cfg_model_gap &&
          type != ThreadAPI::TD_DUMMY &&
          library == ThreadAPI::RuntimeLibrary::OpenMP) {
        ++m_deferred_reason_counts[
            "omp_cfg_path_sensitive_semantics_deferred"];
        recorded_cfg_model_gap = true;
      }
      bool is_nowait_variant = callee && callee->getName().contains("nowait");

      if (type == ThreadAPI::TD_DUMMY && callee && callee->hasName() &&
          callee->getName().startswith("__kmpc_atomic_")) {
        ++m_summary.atomic_region_count;
        ++m_deferred_reason_counts["omp_atomic_runtime_unmodeled"];
        continue;
      }

      if (library == ThreadAPI::RuntimeLibrary::OpenMP) {
        if (type == ThreadAPI::TD_FORK) {
          ++m_summary.parallel_region_count;
          size_t current_depth = state.region_stack.size() + 1;
          m_nested_depth = std::max(m_nested_depth, current_depth);
          m_summary.nested_parallelism_max_depth =
              std::max(m_summary.nested_parallelism_max_depth, current_depth);
          if (current_depth > 1) {
            ++m_summary.nested_parallelism_nested_regions;
          } else {
            ++m_summary.nested_parallelism_flat_regions;
          }
          const bool explicit_parallel_end =
              api->hasTrait(call, "parallel-explicit-end");
          TraversalState::RegionFrame frame =
              pushRegion(WaitBoundaryInfo::Kind::Unknown,
                         SemanticEntityKind::ParallelRegion, call);
          m_region_nesting_depth[frame.id] = current_depth;
          if (const auto *fork_target =
                  dyn_cast_or_null<Function>(api->getForkedFun(call))) {
            TraversalState fork_state;
            fork_state.scheduling_context_id = m_next_scheduling_context_id++;
            fork_state.scheduling_context_entity_id =
                addEntity(SemanticEntityKind::SchedulingContext, call,
                          fork_target, fork_state.scheduling_context_id,
                          frame.entity_id, frame.id, currentPhaseToken(), 0);
            fork_state.phase_stack.push_back(0);
            fork_state.anchor_inst = call;
            std::set<const Function *> nested_call_stack;
            scanSchedulingContext(fork_target, fork_state, nested_call_stack);
          }
          if (!explicit_parallel_end) {
            popRegion(WaitBoundaryInfo::Kind::Unknown, call);
          }
          continue;
        }

        if (type == ThreadAPI::TD_BAR_WAIT) {
          ++m_summary.barrier_count;
          WaitBoundaryInfo boundary =
              recordBoundary(call, WaitBoundaryInfo::Kind::Barrier);
          addTaskEvent(
              OpenMPTaskEvent::Kind::Barrier, call, state.scheduling_context_id,
              state.sequence_index, boundary.event_order, currentPhaseToken(),
              state.taskgroup_stack.empty() ? 0 : state.taskgroup_stack.back(),
              currentRegionId(), boundary.semantic_entity_id, nullptr,
              WaitBoundaryInfo::Kind::Barrier, boundary.is_partial_wait);
          if (api->hasTrait(call, "parallel-end")) {
            popRegion(WaitBoundaryInfo::Kind::Unknown, call);
          }
          advanceCurrentPhase();
          continue;
        }

        if (type == ThreadAPI::TD_OMP_TASKYIELD) {
          ++m_summary.taskyield_count;
          continue;
        }
        if (type == ThreadAPI::TD_ACQUIRE || type == ThreadAPI::TD_RELEASE ||
            type == ThreadAPI::TD_TRY_ACQUIRE) {
          if (api->semanticTagStartsWith(callee, "critical")) {
            if (type == ThreadAPI::TD_ACQUIRE) {
              ++m_summary.critical_region_count;
              pushRegion(WaitBoundaryInfo::Kind::CriticalEnd,
                         SemanticEntityKind::CriticalRegion, call);
            } else if (type == ThreadAPI::TD_RELEASE) {
              popRegion(WaitBoundaryInfo::Kind::CriticalEnd, call);
            }
          } else {
            ++m_summary.lock_api_count;
          }
          continue;
        }
        if (type == ThreadAPI::TD_OMP_CANCEL) {
          if (api->hasSemanticTag(callee, "cancellation-point")) {
            ++m_summary.cancellation_point_count;
            ++m_deferred_reason_counts
                ["omp_cancellation_point_runtime_unmodeled"];
          } else {
            ++m_summary.cancel_count;
            ++m_deferred_reason_counts["omp_cancel_runtime_unmodeled"];
          }
          continue;
        }
      }

      if (type == ThreadAPI::TD_OMP_TASKWAIT_DEPS) {
        const bool is_taskwait_deps_51 =
            callee &&
            callee->getName().equals("__kmpc_omp_taskwait_deps_51");
        const unsigned minimum_arity = is_taskwait_deps_51 ? 7 : 6;
        if (call->arg_size() < minimum_arity) {
          ++m_deferred_reason_counts[
              is_taskwait_deps_51
                  ? "omp_taskwait_deps_51_arity_invalid"
                  : "omp_taskwait_deps_abi_arity_invalid"];
          continue;
        }
        std::vector<Dependency> dependencies =
            extractRuntimeDependencies(call, 2, 3);
        std::vector<Dependency> noalias_dependencies =
            extractRuntimeDependencies(call, 4, 5);
        dependencies.insert(dependencies.end(), noalias_dependencies.begin(),
                            noalias_dependencies.end());
        bool deferred_dependency_task = false;
        bool unresolved_nowait = false;
        if (is_taskwait_deps_51) {
          if (const auto *nowait =
                  dyn_cast<ConstantInt>(call->getArgOperand(6))) {
            deferred_dependency_task = !nowait->isZero();
          } else {
            unresolved_nowait = true;
            ++m_deferred_reason_counts["omp_taskwait_deps_nowait_unknown"];
          }
        }

        if (deferred_dependency_task) {
          auto task = std::make_unique<Task>();
          task->task_create = call;
          task->task_function = nullptr;
          task->has_deferred_submission = true;
          task->has_dependency_submission = true;
          task->parent_context = func;
          task->generating_context = state.anchor_inst ? state.anchor_inst : call;
          task->scheduling_context_id = state.scheduling_context_id;
          task->taskgroup_id = state.taskgroup_stack.empty()
                                   ? 0
                                   : state.taskgroup_stack.back();
          task->taskgroup_ids.insert(state.taskgroup_stack.begin(),
                                     state.taskgroup_stack.end());
          task->phase_id = currentPhaseToken();
          task->sibling_group = currentPhaseToken();
          task->sequence_index = state.sequence_index++;
          task->event_order = nextEventOrder();
          task->region_id = currentRegionId();
          task->dependencies = std::move(dependencies);
          task->semantic_entity_id = addEntity(
              SemanticEntityKind::ExplicitTask, call, func,
              state.scheduling_context_id, currentRegionEntityId(),
              currentRegionId(), currentPhaseToken(), task->taskgroup_id);
          addEvent(SemanticEventKind::TaskCreate, call,
                   task->semantic_entity_id, state.scheduling_context_id,
                   task->sequence_index, task->event_order, currentRegionId(),
                   currentPhaseToken());
          addTaskEvent(OpenMPTaskEvent::Kind::TaskCreate, call,
                       state.scheduling_context_id, task->sequence_index,
                       task->event_order, currentPhaseToken(),
                       task->taskgroup_id, currentRegionId(),
                       task->semantic_entity_id, task.get());
          m_inst_to_task[call] = task.get();
          m_tasks.push_back(std::move(task));
          continue;
        }

        WaitBoundaryInfo boundary = recordBoundary(
            call, WaitBoundaryInfo::Kind::TaskwaitDeps, true);
        addTaskEvent(
            OpenMPTaskEvent::Kind::TaskwaitDeps, call,
            state.scheduling_context_id, state.sequence_index,
            boundary.event_order, currentPhaseToken(),
            state.taskgroup_stack.empty() ? 0 : state.taskgroup_stack.back(),
            currentRegionId(), boundary.semantic_entity_id, nullptr,
            WaitBoundaryInfo::Kind::TaskwaitDeps, true, false,
            std::move(dependencies));
        if (unresolved_nowait) {
          m_task_events.back().is_partial_wait = true;
        }
        continue;
      }

      if (type == ThreadAPI::TD_OMP_TASKWAIT) {
        WaitBoundaryInfo boundary =
            recordBoundary(call, WaitBoundaryInfo::Kind::Taskwait);
        addTaskEvent(
            OpenMPTaskEvent::Kind::Taskwait, call, state.scheduling_context_id,
            state.sequence_index, boundary.event_order, currentPhaseToken(),
            state.taskgroup_stack.empty() ? 0 : state.taskgroup_stack.back(),
            currentRegionId(), boundary.semantic_entity_id, nullptr,
            WaitBoundaryInfo::Kind::Taskwait, boundary.is_partial_wait);
        advanceCurrentPhase();
        continue;
      }

      if (type == ThreadAPI::TD_OMP_TASKGROUP_START) {
        size_t taskgroup_id = 0;
        if (precise_phase_tracking) {
          taskgroup_id = m_next_taskgroup_id++;
          state.taskgroup_stack.push_back(taskgroup_id);
          state.phase_stack.push_back(state.next_phase_token++);
        }
        TraversalState::RegionFrame frame =
            pushRegion(WaitBoundaryInfo::Kind::TaskgroupEnd,
                       SemanticEntityKind::Taskgroup, call);
        const size_t event_order = nextEventOrder();
        addTaskEvent(OpenMPTaskEvent::Kind::TaskgroupBegin, call,
                     state.scheduling_context_id, state.sequence_index,
                     event_order, currentPhaseToken(), taskgroup_id, frame.id,
                     frame.entity_id);
        ++m_summary.taskgroup_region_count;
        continue;
      }

      if (type == ThreadAPI::TD_OMP_TASKGROUP_END) {
        WaitBoundaryInfo info =
            recordBoundary(call, WaitBoundaryInfo::Kind::TaskgroupEnd,
                           !precise_phase_tracking, true);
        size_t taskgroup_id = 0;
        if (precise_phase_tracking && !state.taskgroup_stack.empty()) {
          taskgroup_id = state.taskgroup_stack.back();
          info.taskgroup_id = taskgroup_id;
          state.taskgroup_stack.pop_back();
        }
        TraversalState::RegionFrame frame =
            popRegion(WaitBoundaryInfo::Kind::TaskgroupEnd, call);
        info.region_id = frame.id;
        addTaskEvent(OpenMPTaskEvent::Kind::TaskgroupEnd, call,
                     state.scheduling_context_id, state.sequence_index,
                     info.event_order, currentPhaseToken(), taskgroup_id,
                     frame.id, info.semantic_entity_id, nullptr,
                     WaitBoundaryInfo::Kind::TaskgroupEnd, info.is_partial_wait,
                     true);
        if (precise_phase_tracking && !state.phase_stack.empty()) {
          state.phase_stack.pop_back();
        }
        advanceCurrentPhase();
        continue;
      }

      if (type == ThreadAPI::TD_OMP_SINGLE_START) {
        pushRegion(WaitBoundaryInfo::Kind::SingleEnd,
                   SemanticEntityKind::SingleRegion, call);
        ++m_summary.single_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_MASTER_START) {
        pushRegion(WaitBoundaryInfo::Kind::MasterEnd,
                   SemanticEntityKind::MasterRegion, call);
        ++m_summary.master_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_ORDERED_START) {
        pushRegion(WaitBoundaryInfo::Kind::OrderedEnd,
                   SemanticEntityKind::OrderedRegion, call);
        ++m_summary.ordered_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_FOR_STATIC_INIT ||
          type == ThreadAPI::TD_OMP_FOR_DISPATCH_INIT) {
        pushRegion(type == ThreadAPI::TD_OMP_FOR_STATIC_INIT
                       ? WaitBoundaryInfo::Kind::ForFini
                       : WaitBoundaryInfo::Kind::DispatchFini,
                   SemanticEntityKind::WorksharingLoop, call);
        ++m_summary.worksharing_loop_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_REDUCE_START ||
          type == ThreadAPI::TD_OMP_REDUCE_NOWAIT_START) {
        pushRegion(WaitBoundaryInfo::Kind::Unknown,
                   SemanticEntityKind::ReductionRegion, call);
        ++m_summary.reduction_region_count;
        ++m_deferred_reason_counts["omp_reduction_protocol_unmodeled"];
        continue;
      }
      if (type == ThreadAPI::TD_OMP_SECTIONS_INIT) {
        pushRegion(WaitBoundaryInfo::Kind::SectionsEnd,
                   SemanticEntityKind::SectionsRegion, call);
        ++m_summary.sections_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_ATOMIC_START) {
        ++m_summary.atomic_region_count;
        ++m_deferred_reason_counts["omp_atomic_runtime_unmodeled"];
        continue;
      }
      if (type == ThreadAPI::TD_OMP_TARGET) {
        ++m_summary.target_region_count;
        WaitBoundaryInfo::Kind kind = is_nowait_variant
                                          ? WaitBoundaryInfo::Kind::TargetNowait
                                          : WaitBoundaryInfo::Kind::Target;
        WaitBoundaryInfo boundary = recordBoundary(call, kind, true);
        addTaskEvent(
            OpenMPTaskEvent::Kind::TargetBoundary, call,
            state.scheduling_context_id, state.sequence_index,
            boundary.event_order, currentPhaseToken(),
            state.taskgroup_stack.empty() ? 0 : state.taskgroup_stack.back(),
            currentRegionId(), boundary.semantic_entity_id, nullptr, kind,
            boundary.is_partial_wait);
        if (is_nowait_variant) {
          ++m_summary.target_nowait_boundary_count;
        }
        ++m_deferred_reason_counts["omp_target_task_unmodeled"];
        continue;
      }
      if (type == ThreadAPI::TD_OMP_TARGET_DATA_BEGIN ||
          type == ThreadAPI::TD_OMP_TARGET_DATA_END ||
          type == ThreadAPI::TD_OMP_TARGET_DATA_UPDATE) {
        ++m_summary.target_data_region_count;
        if (type == ThreadAPI::TD_OMP_TARGET_DATA_END) {
          WaitBoundaryInfo::Kind kind =
              is_nowait_variant ? WaitBoundaryInfo::Kind::TargetDataNowait
                                : WaitBoundaryInfo::Kind::TargetData;
          WaitBoundaryInfo boundary =
              recordBoundary(call, kind, is_nowait_variant);
          addTaskEvent(
              OpenMPTaskEvent::Kind::TargetBoundary, call,
              state.scheduling_context_id, state.sequence_index,
              boundary.event_order, currentPhaseToken(),
              state.taskgroup_stack.empty() ? 0 : state.taskgroup_stack.back(),
              currentRegionId(), boundary.semantic_entity_id, nullptr, kind,
              boundary.is_partial_wait);
          if (is_nowait_variant) {
            ++m_summary.target_nowait_boundary_count;
          } else {
            advanceCurrentPhase();
          }
          ++m_deferred_reason_counts["omp_target_data_task_unmodeled"];
        }
        continue;
      }
      if (type == ThreadAPI::TD_OMP_DOACROSS_INIT) {
        ++m_summary.doacross_init_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_DOACROSS_WAIT) {
        ++m_summary.doacross_wait_count;
        std::vector<Dependency> dependencies;
        if (call->arg_size() >= 3) {
          const Value *witness = stripValue(call->getArgOperand(2));
          if (witness && !isa<ConstantPointerNull>(witness)) {
            Dependency dep;
            dep.address = witness;
            dep.type = DependType::INOUT;
            dep.size = 0;
            dep.source_kind = DependencySourceKind::Iterator;
            dep.proof = DependencyProof::Possible;
            int64_t offset = 0;
            bool precise = false;
            dep.canonical_base = canonicalizeDependencyAddress(
                witness, m_module.getDataLayout(), offset, precise);
            dep.offset = offset;
            dep.has_precise_offset = precise;
            dependencies.push_back(dep);
          }
        }
        WaitBoundaryInfo boundary =
            recordBoundary(call, WaitBoundaryInfo::Kind::DoacrossWait, true);
        addTaskEvent(
            OpenMPTaskEvent::Kind::DoacrossWait, call,
            state.scheduling_context_id, state.sequence_index,
            boundary.event_order, currentPhaseToken(),
            state.taskgroup_stack.empty() ? 0 : state.taskgroup_stack.back(),
            currentRegionId(), boundary.semantic_entity_id, nullptr,
            WaitBoundaryInfo::Kind::DoacrossWait, true, false,
            std::move(dependencies));
        continue;
      }
      if (type == ThreadAPI::TD_OMP_DOACROSS_SUBMIT) {
        ++m_summary.doacross_submit_count;
        std::vector<Dependency> dependencies;
        if (call->arg_size() >= 3) {
          const Value *witness = stripValue(call->getArgOperand(2));
          if (witness && !isa<ConstantPointerNull>(witness)) {
            Dependency dep;
            dep.address = witness;
            dep.type = DependType::INOUT;
            dep.size = 0;
            dep.source_kind = DependencySourceKind::Iterator;
            dep.proof = DependencyProof::Possible;
            int64_t offset = 0;
            bool precise = false;
            dep.canonical_base = canonicalizeDependencyAddress(
                witness, m_module.getDataLayout(), offset, precise);
            dep.offset = offset;
            dep.has_precise_offset = precise;
            dependencies.push_back(dep);
          }
        }
        const size_t event_order = nextEventOrder();
        addTaskEvent(
            OpenMPTaskEvent::Kind::DoacrossSubmit, call,
            state.scheduling_context_id, state.sequence_index, event_order,
            currentPhaseToken(),
            state.taskgroup_stack.empty() ? 0 : state.taskgroup_stack.back(),
            currentRegionId(), currentRegionEntityId(), nullptr,
            WaitBoundaryInfo::Kind::Unknown, false, false,
            std::move(dependencies));
        continue;
      }
      if (type == ThreadAPI::TD_OMP_TASK_COMPLETE) {
        const Task *completed_task =
            call->arg_size() >= 3 ? getTaskForHandle(call->getArgOperand(2))
                                  : nullptr;
        if (completed_task && completed_task->is_detached) {
          ++m_deferred_reason_counts[
              "omp_detached_fulfillment_event_unresolved"];
        }
        const size_t event_order = nextEventOrder();
        TaskCompletionEvent completion_event;
        completion_event.inst = call;
        // __kmpc_omp_task_complete_if0 closes the inline task lifecycle.  It
        // does not fulfill a detachable task's allow-completion event.
        completion_event.task = nullptr;
        completion_event.scheduling_context_id = state.scheduling_context_id;
        completion_event.sequence_index = state.sequence_index;
        completion_event.event_order = event_order;
        completion_event.phase_id = currentPhaseToken();
        completion_event.taskgroup_id =
            state.taskgroup_stack.empty() ? 0 : state.taskgroup_stack.back();
        completion_event.region_id = currentRegionId();
        completion_event.semantic_entity_id = currentRegionEntityId();
        m_task_completion_events.push_back(completion_event);
        addTaskEvent(
            OpenMPTaskEvent::Kind::TaskComplete, call,
            state.scheduling_context_id, state.sequence_index, event_order,
            currentPhaseToken(),
            state.taskgroup_stack.empty() ? 0 : state.taskgroup_stack.back(),
            currentRegionId(), currentRegionEntityId(),
            completion_event.task);
        continue;
      }
      if (type == ThreadAPI::TD_OMP_TEAMS ||
          type == ThreadAPI::TD_OMP_TEAMS_HOST ||
          type == ThreadAPI::TD_OMP_TEAMS_DISTRIBUTE) {
        ++m_summary.teams_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_DISTRIBUTE ||
          type == ThreadAPI::TD_OMP_DISTRIBUTE_STATIC ||
          type == ThreadAPI::TD_OMP_DISTRIBUTE_DYNAMIC ||
          type == ThreadAPI::TD_OMP_DISTRIBUTE_GUIDANCE) {
        ++m_summary.distribute_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_LOOP_STATIC_INIT ||
          type == ThreadAPI::TD_OMP_LOOP_DYNAMIC_INIT ||
          type == ThreadAPI::TD_OMP_LOOP_GUIDANCE_INIT) {
        ++m_summary.loop_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_AFFINITY) {
        ++m_summary.affinity_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_SCOPE_START ||
          type == ThreadAPI::TD_OMP_SCOPE_END) {
        ++m_summary.scope_region_count;
        continue;
      }

      if (type == ThreadAPI::TD_OMP_MASTER_END ||
          type == ThreadAPI::TD_OMP_ORDERED_END) {
        popRegion(type == ThreadAPI::TD_OMP_MASTER_END
                      ? WaitBoundaryInfo::Kind::MasterEnd
                      : WaitBoundaryInfo::Kind::OrderedEnd,
                  call);
        continue;
      }

      if (type == ThreadAPI::TD_OMP_SINGLE_END ||
          type == ThreadAPI::TD_OMP_SECTIONS_END ||
          type == ThreadAPI::TD_OMP_FOR_STATIC_FINI ||
          type == ThreadAPI::TD_OMP_FOR_DISPATCH_FINI) {
        WaitBoundaryInfo::Kind kind = WaitBoundaryInfo::Kind::Unknown;
        if (type == ThreadAPI::TD_OMP_SINGLE_END) {
          kind = WaitBoundaryInfo::Kind::SingleEnd;
        } else if (type == ThreadAPI::TD_OMP_SECTIONS_END) {
          kind = WaitBoundaryInfo::Kind::SectionsEnd;
        } else if (type == ThreadAPI::TD_OMP_FOR_STATIC_FINI) {
          kind = WaitBoundaryInfo::Kind::ForFini;
        } else if (type == ThreadAPI::TD_OMP_FOR_DISPATCH_FINI) {
          kind = WaitBoundaryInfo::Kind::DispatchFini;
        }
        popRegion(kind, call);
        continue;
      }

      if (type == ThreadAPI::TD_OMP_REDUCE_END ||
          type == ThreadAPI::TD_OMP_REDUCE_NOWAIT_END ||
          type == ThreadAPI::TD_OMP_FLUSH) {
        if (type == ThreadAPI::TD_OMP_FLUSH) {
          ++m_summary.flush_count;
          WaitBoundaryInfo boundary =
              recordBoundary(call, WaitBoundaryInfo::Kind::Flush, true);
          addTaskEvent(
              OpenMPTaskEvent::Kind::Flush, call, state.scheduling_context_id,
              state.sequence_index, boundary.event_order, currentPhaseToken(),
              state.taskgroup_stack.empty() ? 0 : state.taskgroup_stack.back(),
              currentRegionId(), boundary.semantic_entity_id, nullptr,
              WaitBoundaryInfo::Kind::Flush, true);
        } else {
          popRegion(WaitBoundaryInfo::Kind::Unknown, call);
        }
        continue;
      }

      if (type == ThreadAPI::TD_OMP_TASK_WITH_DEPS ||
          type == ThreadAPI::TD_OMP_TASK ||
          type == ThreadAPI::TD_OMP_TASKLOOP) {
        const unsigned minimum_arity =
            type == ThreadAPI::TD_OMP_TASK_WITH_DEPS ? 7 : 3;
        if (call->arg_size() < minimum_arity) {
          ++m_deferred_reason_counts["omp_task_abi_arity_invalid"];
          continue;
        }

        const bool is_included_submission =
            callee && callee->getName().equals("__kmpc_omp_task_begin_if0");
        const CallBase *task_alloc = findTaskAllocCall(call->getArgOperand(2));
        const Value *task_handle = canonicalizeTaskHandle(call->getArgOperand(2));
        Task *existing_task = nullptr;
        if (task_alloc && task_handle) {
          auto existing_it = m_handle_to_task.find(task_handle);
          if (existing_it != m_handle_to_task.end()) {
            existing_task = existing_it->second;
          }
        }

        if (existing_task) {
          existing_task->has_included_submission |= is_included_submission;
          existing_task->has_deferred_submission |= !is_included_submission;
          if (!is_included_submission) {
            existing_task->task_create = call;
          }
          if (state.executing_final_task) {
            existing_task->is_final = true;
            existing_task->is_untied = false;
            existing_task->has_deferred_submission = false;
            existing_task->execution_mode = TaskExecutionMode::Included;
          } else if (existing_task->has_deferred_submission) {
            existing_task->execution_mode = TaskExecutionMode::Deferred;
          } else if (existing_task->has_included_submission) {
            existing_task->execution_mode = TaskExecutionMode::Included;
          }
          const bool site_may_repeat =
              isTaskSiteInLoop(call) || isRecursiveFunction(func);
          existing_task->is_recurrent = state.executing_recurrent_task ||
                                        (site_may_repeat &&
                                         existing_task->has_deferred_submission);
          if (type == ThreadAPI::TD_OMP_TASK_WITH_DEPS) {
            existing_task->has_dependency_submission = true;
            if (existing_task->dependencies.empty()) {
              existing_task->dependencies = extractDependencies(call);
            }
            for (const Dependency &dep : existing_task->dependencies) {
              if (dep.canonical_base) {
                existing_task->synchronization_objects.insert(
                    dep.canonical_base);
              }
              if (existing_task->is_recurrent &&
                  classifyDependencyConflict(dep, dep) ==
                      DependencyConflict::MustConflict) {
                if (dep.type == DependType::MUTEXINOUTSET) {
                  existing_task->recurrent_instances_excluded = true;
                } else {
                  existing_task->recurrent_instances_serialized = true;
                }
              }
            }
          }
          existing_task->is_taskloop |= type == ThreadAPI::TD_OMP_TASKLOOP;
          m_inst_to_task[call] = existing_task;
          continue;
        }

        auto task = std::make_unique<Task>();
        task->task_create = call;
        task->task_function = extractTaskFunction(call);
        task->task_handle = task_handle;
        task->has_included_submission = is_included_submission;
        task->has_deferred_submission = !is_included_submission;
        task->has_dependency_submission =
            type == ThreadAPI::TD_OMP_TASK_WITH_DEPS;
        task->is_taskloop = type == ThreadAPI::TD_OMP_TASKLOOP;
        applyTaskExecutionHints(*task, call);
        const bool site_may_repeat =
            isTaskSiteInLoop(call) || isRecursiveFunction(func);
        task->is_recurrent = state.executing_recurrent_task ||
                             (site_may_repeat &&
                              task->has_deferred_submission);
        if (state.executing_final_task) {
          task->is_final = true;
          task->is_untied = false;
          task->execution_mode = TaskExecutionMode::Included;
          task->has_included_submission = true;
          task->has_deferred_submission = false;
        }
        task->parent_context = func;
        task->generating_context = state.anchor_inst ? state.anchor_inst : call;
        task->scheduling_context_id = state.scheduling_context_id;
        task->taskgroup_id =
            state.taskgroup_stack.empty() ? 0 : state.taskgroup_stack.back();
        task->taskgroup_ids.insert(state.taskgroup_stack.begin(),
                                   state.taskgroup_stack.end());
        task->phase_id = currentPhaseToken();
        task->sibling_group = currentPhaseToken();
        task->sequence_index = state.sequence_index++;
        task->event_order = nextEventOrder();
        task->region_id = currentRegionId();
        task->semantic_entity_id = addEntity(
            SemanticEntityKind::ExplicitTask, call,
            task->task_function ? task->task_function : func,
            state.scheduling_context_id, currentRegionEntityId(),
            currentRegionId(), currentPhaseToken(), task->taskgroup_id);
        addEvent(SemanticEventKind::TaskCreate, call, task->semantic_entity_id,
                 state.scheduling_context_id, task->sequence_index,
                 task->event_order, currentRegionId(), currentPhaseToken());
        addTaskEvent(OpenMPTaskEvent::Kind::TaskCreate, call,
                     state.scheduling_context_id, task->sequence_index,
                     task->event_order, currentPhaseToken(), task->taskgroup_id,
                     currentRegionId(), task->semantic_entity_id, task.get());

        if (type == ThreadAPI::TD_OMP_TASK_WITH_DEPS) {
          task->dependencies = extractDependencies(call);
          ++m_summary.task_with_dependencies_count;
          for (const Dependency &dep : task->dependencies) {
            if (dep.canonical_base) {
              task->synchronization_objects.insert(dep.canonical_base);
            }
          }
        }
        if (task->is_recurrent) {
          for (const Dependency &dep : task->dependencies) {
            if (classifyDependencyConflict(dep, dep) ==
                DependencyConflict::MustConflict) {
              if (dep.type == DependType::MUTEXINOUTSET) {
                task->recurrent_instances_excluded = true;
              } else {
                task->recurrent_instances_serialized = true;
              }
            }
          }
        }
        if (type == ThreadAPI::TD_OMP_TASKLOOP) {
          ++m_summary.taskloop_count;
        }
        if (task->execution_mode == TaskExecutionMode::Included) {
          ++m_summary.included_task_count;
        }
        if (task->is_final) {
          ++m_summary.final_task_count;
        }
        if (task->is_untied) {
          ++m_summary.untied_task_count;
        }
        if (task->is_detached) {
          ++m_summary.detached_task_count;
        }

        m_inst_to_task[call] = task.get();
        if (task_alloc && task->task_handle) {
          m_handle_to_task[task->task_handle] = task.get();
        }
        Task *tracked_task = task.get();
        m_tasks.push_back(std::move(task));
        if (tracked_task->task_function &&
            !tracked_task->task_function->isDeclaration()) {
          TraversalState task_state;
          task_state.scheduling_context_id = m_next_scheduling_context_id++;
          task_state.scheduling_context_entity_id = addEntity(
              SemanticEntityKind::SchedulingContext, call,
              tracked_task->task_function, task_state.scheduling_context_id,
              tracked_task->semantic_entity_id, currentRegionId(),
              currentPhaseToken(), tracked_task->taskgroup_id);
          task_state.phase_stack.push_back(0);
          task_state.anchor_inst = call;
          task_state.taskgroup_stack = state.taskgroup_stack;
          task_state.executing_final_task = tracked_task->is_final;
          task_state.executing_recurrent_task = tracked_task->is_recurrent;
          std::set<const Function *> nested_call_stack = call_stack;
          scanSchedulingContext(tracked_task->task_function, task_state,
                                nested_call_stack);
        }
        continue;
      }

      if (callee && !callee->isDeclaration() && type == ThreadAPI::TD_DUMMY &&
          !OpenMPModel::isOpenMP(callee->getName()) &&
          m_omp_relevant_functions.count(callee) != 0) {
        const Instruction *saved_anchor = state.anchor_inst;
        state.anchor_inst = call;
        scanSchedulingContext(callee, state, call_stack);
        state.anchor_inst = saved_anchor;
      }
    }
  }

  call_stack.erase(func);
}

std::vector<Dependency>
OpenMPSemantics::extractDependencies(const CallBase *task_call) {
  std::vector<Dependency> deps =
      extractRuntimeDependencies(task_call, 3, 4);
  std::vector<Dependency> noalias_deps =
      extractRuntimeDependencies(task_call, 5, 6);
  deps.insert(deps.end(), noalias_deps.begin(), noalias_deps.end());
  return deps;
}

std::vector<Dependency> OpenMPSemantics::extractRuntimeDependencies(
    const CallBase *task_call, unsigned ndeps_arg_idx, unsigned dep_arg_idx) {
  std::vector<Dependency> deps;
  const DataLayout &DL = m_module.getDataLayout();

  if (!task_call ||
      task_call->arg_size() <= std::max(ndeps_arg_idx, dep_arg_idx)) {
    return deps;
  }

  const Value *ndeps_val = task_call->getArgOperand(ndeps_arg_idx);
  const Value *dep_list = task_call->getArgOperand(dep_arg_idx);
  const Value *dep_root = resolveDependencyListValue(dep_list);
  const Value *dep_base =
      dep_root ? getUnderlyingObject(dep_root->stripPointerCasts()) : nullptr;
  if (!dep_base && dep_root) {
    dep_base = dep_root->stripPointerCasts();
  }

  const std::optional<uint64_t> resolved_ndeps =
      resolveDependencyCount(ndeps_val, task_call);
  uint64_t start_index = getDependencyStartIndex(dep_root, dep_base);
  uint64_t max_entries = resolved_ndeps.value_or(0);
  if (max_entries == 0) {
    uint64_t capacity = getDependencyArrayCapacity(dep_base);
    if (capacity > start_index) {
      max_entries = capacity - start_index;
    }
  }
  if (max_entries == 0) {
    return deps;
  }

  if (const auto *gv = dyn_cast_or_null<GlobalVariable>(dep_base)) {
    if (const auto *init = gv->getInitializer()) {
      if (const auto *array = dyn_cast<ConstantArray>(init)) {
        for (unsigned i = start_index;
             i < array->getNumOperands() && deps.size() < max_entries; ++i) {
          Dependency dep;
          if (decodeConstantDependency(dyn_cast<Constant>(array->getOperand(i)),
                                       dep)) {
            dep.source_kind = gv->isConstant()
                                  ? DependencySourceKind::DirectAddress
                                  : DependencySourceKind::RegionSummary;
            dep.proof = gv->isConstant() ? DependencyProof::Definite
                                         : DependencyProof::Possible;
            dep.canonical_base = canonicalizeDependencyAddress(
                dep.address, DL, dep.offset, dep.has_precise_offset);
            deps.push_back(dep);
          }
        }
      }
    }
  } else if (const auto *alloca = dyn_cast_or_null<AllocaInst>(dep_base)) {
    std::map<uint64_t, PartialDependency> partials;
    const Function *parent = alloca->getFunction();
    if (!parent) {
      return deps;
    }

    const MemTransferInst *latest_transfer = nullptr;
    for (const Instruction &inst : instructions(parent)) {
      const auto *transfer = dyn_cast<MemTransferInst>(&inst);
      if (!transfer || !mustHappenBefore(transfer, task_call)) {
        continue;
      }
      const Value *dest = getUnderlyingObject(transfer->getRawDest());
      if (!dest) {
        dest = transfer->getRawDest()->stripPointerCasts();
      }
      if (dest != alloca) {
        continue;
      }
      if (!latest_transfer || mustHappenBefore(latest_transfer, transfer)) {
        latest_transfer = transfer;
      }
    }

    if (latest_transfer) {
      const Value *source_root =
          resolveDependencyListValue(latest_transfer->getRawSource());
      const Value *source_base =
          source_root ? getUnderlyingObject(source_root->stripPointerCasts())
                      : nullptr;
      if (!source_base && source_root) {
        source_base = source_root->stripPointerCasts();
      }

      std::vector<Dependency> seeded;
      if (const auto *source_gv =
              dyn_cast_or_null<GlobalVariable>(source_base)) {
        if (const auto *init = source_gv->getInitializer()) {
          if (const auto *array = dyn_cast<ConstantArray>(init)) {
            uint64_t source_start =
                getDependencyStartIndex(source_root, source_base);
            for (unsigned i = source_start;
                 i < array->getNumOperands() && seeded.size() < max_entries;
                 ++i) {
              Dependency dep;
              if (!decodeConstantDependency(
                      dyn_cast<Constant>(array->getOperand(i)), dep)) {
                continue;
              }
              dep.source_kind = source_gv->isConstant()
                                    ? DependencySourceKind::DirectAddress
                                    : DependencySourceKind::RegionSummary;
              dep.proof = source_gv->isConstant() ? DependencyProof::Definite
                                                  : DependencyProof::Possible;
              dep.canonical_base = canonicalizeDependencyAddress(
                  dep.address, DL, dep.offset, dep.has_precise_offset);
              seeded.push_back(dep);
            }
          }
        }
      }

      if (!seeded.empty()) {
        seedPartialDependencies(partials, seeded, start_index);
      }
    }

    for (const Instruction &inst : instructions(parent)) {
      const auto *store = dyn_cast<StoreInst>(&inst);
      if (!store) {
        continue;
      }
      if (!mustHappenBefore(store, task_call)) {
        continue;
      }
      const auto *gep = dyn_cast<GEPOperator>(store->getPointerOperand());
      if (!gep || getUnderlyingObject(gep->getPointerOperand()) != alloca) {
        continue;
      }
      if (gep->getNumIndices() < 2) {
        continue;
      }

      SmallVector<unsigned, 4> indices;
      bool all_constant = true;
      for (unsigned i = 0; i < gep->getNumIndices(); ++i) {
        const auto *ci = dyn_cast<ConstantInt>(gep->getOperand(i + 1));
        if (!ci) {
          all_constant = false;
          break;
        }
        indices.push_back(ci->getZExtValue());
      }
      if (!all_constant) {
        continue;
      }

      uint64_t dep_idx = indices[indices.size() - 2];
      unsigned field_idx = indices.back();
      if (dep_idx < start_index || dep_idx >= start_index + max_entries) {
        continue;
      }

      PartialDependency &partial = partials[dep_idx];
      const Value *stored = store->getValueOperand();
      if (field_idx == 0) {
        partial.address = stripValue(stored);
        partial.has_address = partial.address != nullptr;
        auto evidence = classifyDependencyAddressEvidence(stored);
        partial.source_kind = evidence.first;
        partial.proof = evidence.second;
      } else if (field_idx == 1) {
        if (const auto *len = dyn_cast<ConstantInt>(stored)) {
          partial.size = len->getZExtValue();
          partial.has_size = true;
        }
      } else if (field_idx == 2) {
        if (const auto *flags = dyn_cast<ConstantInt>(stored)) {
          partial.flags = flags->getZExtValue();
          partial.has_flags = true;
        }
      }
    }

    for (uint64_t i = start_index; i < start_index + max_entries; ++i) {
      auto it = partials.find(i);
      if (it == partials.end() || !it->second.has_address) {
        continue;
      }
      Dependency dep;
      dep.address = it->second.address;
      dep.size = it->second.has_size ? it->second.size : 0;
      dep.type =
          decodeDependType(it->second.has_flags ? it->second.flags : 0x3);
      dep.source_kind = it->second.source_kind;
      dep.proof =
          it->second.has_address ? it->second.proof : DependencyProof::Unknown;
      dep.canonical_base = canonicalizeDependencyAddress(
          dep.address, DL, dep.offset, dep.has_precise_offset);
      deps.push_back(dep);
    }
  }

  return deps;
}

const CallBase *
OpenMPSemantics::findTaskAllocCall(const Value *task_value) const {
  if (!task_value) {
    return nullptr;
  }

  std::deque<const Value *> worklist;
  std::set<const Value *> visited;
  worklist.push_back(task_value);

  auto isTaskAllocCall = [](const Value *value) -> const CallBase * {
    const auto *cb = dyn_cast<CallBase>(value);
    if (!cb) {
      return nullptr;
    }
    const Function *callee = cb->getCalledFunction();
    return callee && OpenMPModel::isTaskAlloc(callee->getName()) ? cb : nullptr;
  };

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    if (!current || !visited.insert(current).second) {
      continue;
    }

    current = current->stripPointerCasts();
    if (const CallBase *alloc = isTaskAllocCall(current)) {
      return alloc;
    }
    if (const auto *load = dyn_cast<LoadInst>(current)) {
      worklist.push_back(load->getPointerOperand());
      continue;
    }
    if (const auto *phi = dyn_cast<PHINode>(current)) {
      for (const Value *incoming : phi->incoming_values()) {
        worklist.push_back(incoming);
      }
      continue;
    }
    if (const auto *select = dyn_cast<SelectInst>(current)) {
      worklist.push_back(select->getTrueValue());
      worklist.push_back(select->getFalseValue());
      continue;
    }
    if (const auto *gep = dyn_cast<GEPOperator>(current)) {
      worklist.push_back(gep->getPointerOperand());
      continue;
    }

    for (const User *user : current->users()) {
      if (const auto *store = dyn_cast<StoreInst>(user)) {
        if (store->getPointerOperand()->stripPointerCasts() == current) {
          worklist.push_back(store->getValueOperand());
        }
      }
    }
  }

  return nullptr;
}

const Value *
OpenMPSemantics::canonicalizeTaskHandle(const Value *task_value) const {
  if (!task_value) {
    return nullptr;
  }

  task_value = task_value->stripPointerCasts();
  if (const CallBase *task_alloc = findTaskAllocCall(task_value)) {
    return task_alloc;
  }

  if (const Value *underlying = getUnderlyingObject(task_value)) {
    return underlying->stripPointerCasts();
  }
  return task_value;
}

const Function *
OpenMPSemantics::extractTaskFunction(const CallBase *task_call) {
  if (!task_call || task_call->arg_size() < 3) {
    return nullptr;
  }

  if (const Function *callee = task_call->getCalledFunction()) {
    if (callee->hasName() && callee->getName().equals("GOMP_task") &&
        task_call->arg_size() >= 1) {
      if (const auto *direct = dyn_cast<Function>(
              task_call->getArgOperand(0)->stripPointerCasts())) {
        return direct;
      }
    }
  }

  const Value *task_arg = task_call->getArgOperand(2)->stripPointerCasts();
  if (const auto *direct = dyn_cast<Function>(task_arg)) {
    return direct;
  }

  if (const CallBase *task_alloc = findTaskAllocCall(task_arg)) {
    if (task_alloc->arg_size() >= 6) {
      const Value *entry = task_alloc->getArgOperand(5)->stripPointerCasts();
      if (const auto *direct = dyn_cast<Function>(entry)) {
        return direct;
      }
    }
  }

  if (const Function *entry =
          resolveTaskEntryFromLocalSlot(task_arg, task_call)) {
    return entry;
  }

  // More complex task objects require field-sensitive reaching-definition
  // information to identify the entry safely.
  ++m_deferred_reason_counts["omp_task_entry_unresolved"];
  return nullptr;
}

void OpenMPSemantics::applyTaskExecutionHints(Task &task,
                                              const CallBase *task_call) {
  if (!task_call) {
    return;
  }

  const Function *callee = task_call->getCalledFunction();
  if (callee && callee->hasName() &&
      callee->getName().equals("__kmpc_omp_task_begin_if0")) {
    task.execution_mode = TaskExecutionMode::Included;
  }

  const CallBase *task_alloc = findTaskAllocCall(task_call->getArgOperand(2));
  if (!task_alloc || task_alloc->arg_size() < 3) {
    return;
  }

  const auto *flags = dyn_cast<ConstantInt>(task_alloc->getArgOperand(2));
  if (!flags) {
    ++m_deferred_reason_counts["omp_task_flags_unresolved"];
    return;
  }

  const uint64_t value = flags->getZExtValue();
  const bool is_merged_if0 = (value & kLibompTaskMergedIf0Mask) != 0;
  task.is_final = (value & kLibompTaskFinalMask) != 0;
  task.is_proxy = (value & kLibompTaskProxyMask) != 0;
  task.is_detached = (value & kLibompTaskDetachableMask) != 0;
  task.is_untied = (value & kLibompTaskTiednessMask) == 0;

  if (task.is_final || task.execution_mode == TaskExecutionMode::Included) {
    task.is_untied = false;
  }

  if (is_merged_if0) {
    task.has_included_submission = true;
    task.has_deferred_submission = false;
  }

  if (task.execution_mode == TaskExecutionMode::Included || is_merged_if0) {
    task.execution_mode = TaskExecutionMode::Included;
  } else if (task.is_detached) {
    task.execution_mode = TaskExecutionMode::Detached;
  } else if (task.is_final) {
    task.execution_mode = TaskExecutionMode::Final;
  } else if (task.is_untied) {
    task.execution_mode = TaskExecutionMode::Untied;
  }
}

bool OpenMPSemantics::dependenciesConflict(const Dependency &d1,
                                           const Dependency &d2) const {
  return classifyDependencyConflict(d1, d2) == DependencyConflict::MustConflict;
}

DependencyConflict
OpenMPSemantics::classifyDependencyConflict(const Dependency &d1,
                                            const Dependency &d2) const {
  auto impreciseConflict = [&]() {
    ++m_deferred_imprecise_conflict_count;
    ++m_deferred_reason_counts["omp_depend_may_conflict"];
    if (d1.proof == DependencyProof::Unknown ||
        d2.proof == DependencyProof::Unknown) {
      return DependencyConflict::Unknown;
    }
    return DependencyConflict::MayConflict;
  };
  auto hasDefiniteEvidence = [](const Dependency &dep) {
    return dep.proof == DependencyProof::Definite &&
           dep.source_kind == DependencySourceKind::DirectAddress;
  };

  if (d1.type == DependType::UNKNOWN || d2.type == DependType::UNKNOWN) {
    return impreciseConflict();
  }

  if (d1.type == DependType::ALL_MEMORY ||
      d2.type == DependType::ALL_MEMORY) {
    return hasDefiniteEvidence(d1) && hasDefiniteEvidence(d2)
               ? DependencyConflict::MustConflict
               : impreciseConflict();
  }

  if (d1.type == DependType::INOUTSET &&
      d2.type == DependType::INOUTSET) {
    return DependencyConflict::NoConflict;
  }

  const DataLayout &DL = m_module.getDataLayout();
  int64_t offset1 = d1.offset;
  int64_t offset2 = d2.offset;
  bool precise1 = d1.has_precise_offset;
  bool precise2 = d2.has_precise_offset;
  const Value *base1 =
      d1.canonical_base
          ? d1.canonical_base
          : canonicalizeDependencyAddress(d1.address, DL, offset1, precise1);
  const Value *base2 =
      d2.canonical_base
          ? d2.canonical_base
          : canonicalizeDependencyAddress(d2.address, DL, offset2, precise2);

  if (!base1 || !base2) {
    return DependencyConflict::Unknown;
  }

  bool is_write1 =
      (d1.type == DependType::OUT || d1.type == DependType::INOUT ||
       d1.type == DependType::MUTEXINOUTSET ||
       d1.type == DependType::INOUTSET);
  bool is_write2 =
      (d2.type == DependType::OUT || d2.type == DependType::INOUT ||
       d2.type == DependType::MUTEXINOUTSET ||
       d2.type == DependType::INOUTSET);

  if (base1 != base2) {
    if ((is_write1 || is_write2) &&
        (d1.proof != DependencyProof::Definite ||
         d2.proof != DependencyProof::Definite ||
         d1.source_kind != DependencySourceKind::DirectAddress ||
         d2.source_kind != DependencySourceKind::DirectAddress)) {
      ++m_deferred_imprecise_conflict_count;
      ++m_deferred_reason_counts["omp_depend_distinct_base_may_alias"];
      return DependencyConflict::MayConflict;
    }
    return DependencyConflict::NoConflict;
  }

  if (!(is_write1 || is_write2)) {
    return DependencyConflict::NoConflict;
  }

  if (!hasDefiniteEvidence(d1) || !hasDefiniteEvidence(d2)) {
    return impreciseConflict();
  }

  if (precise1 && precise2 && d1.size != 0 && d2.size != 0) {
    uint64_t begin1 = static_cast<uint64_t>(offset1);
    uint64_t begin2 = static_cast<uint64_t>(offset2);
    uint64_t end1 = begin1 + d1.size;
    uint64_t end2 = begin2 + d2.size;
    return begin1 < end2 && begin2 < end1 ? DependencyConflict::MustConflict
                                          : DependencyConflict::NoConflict;
  }

  if (stripValue(d1.address) &&
      stripValue(d1.address) == stripValue(d2.address)) {
    return DependencyConflict::MustConflict;
  }

  return impreciseConflict();
}

bool OpenMPSemantics::isMutexLikeExclusion(const Dependency &d1,
                                           const Dependency &d2) const {
  return d1.type == DependType::MUTEXINOUTSET &&
         d2.type == DependType::MUTEXINOUTSET;
}

} // namespace OpenMP
