#include "Alias/InclusionBased/BootstrapAA/BootstrapAA.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus {
namespace bootstrap {
namespace {
const llvm::Function *directCallee(const llvm::CallBase &call) {
  const llvm::Value *value = call.getCalledOperand()->stripPointerCasts();
  std::set<const llvm::Value *> seen;
  while (const auto *alias = llvm::dyn_cast<llvm::GlobalAlias>(value)) {
    if (!seen.insert(value).second || !alias->getAliasee())
      return nullptr;
    value = alias->getAliasee()->stripPointerCasts();
  }
  return llvm::dyn_cast<llvm::Function>(value);
}

bool knownReadOnly(const llvm::Function *function) {
  if (!function || !function->isDeclaration())
    return false;
  const llvm::StringRef name = function->getName();
  return name == "strlen" || name == "strnlen" || name == "strcmp" ||
         name == "strncmp" || name == "strcasecmp" || name == "strncasecmp" ||
         name == "memcmp" || name == "bcmp" || name == "strchr" ||
         name == "strrchr" || name == "strstr" || name == "strpbrk" ||
         name == "memchr" || name == "rawmemchr" || name == "strspn" ||
         name == "strcspn" || name == "atoi" || name == "atol" ||
         name == "atoll" || name == "atof" || name == "abs" || name == "labs" ||
         name == "llabs" || name == "exit" || name == "_exit" ||
         name == "abort" || name == "__assert_fail";
}

bool writesMemory(const llvm::CallBase &call) {
  return !knownReadOnly(directCallee(call)) && !call.doesNotAccessMemory() &&
         !call.onlyReadsMemory();
}

enum class AllocationKind { None, Fresh, Reallocate };
struct AllocationModel {
  AllocationKind kind = AllocationKind::None;
  bool mayBeNull = false;
};

AllocationModel allocationModel(const llvm::CallBase &call) {
  const llvm::Function *function = directCallee(call);
  if (!function || !function->isDeclaration() || !call.getType()->isPointerTy())
    return {};
  const llvm::StringRef name = function->getName();
  auto integerArguments = [&](unsigned count) {
    if (call.arg_size() != count)
      return false;
    for (unsigned index = 0; index < count; ++index)
      if (!call.getArgOperand(index)->getType()->isIntegerTy())
        return false;
    return true;
  };
  if ((name == "malloc" || name == "valloc") && integerArguments(1))
    return {AllocationKind::Fresh, true};
  if ((name == "calloc" || name == "aligned_alloc" || name == "memalign") &&
      integerArguments(2))
    return {AllocationKind::Fresh, true};
  if (name == "realloc" && call.arg_size() == 2 &&
      call.getArgOperand(0)->getType()->isPointerTy() &&
      call.getArgOperand(1)->getType()->isIntegerTy())
    return {AllocationKind::Reallocate, true};
  if (name == "strdup" && call.arg_size() == 1 &&
      call.getArgOperand(0)->getType()->isPointerTy())
    return {AllocationKind::Fresh, true};
  if (name == "strndup" && call.arg_size() == 2 &&
      call.getArgOperand(0)->getType()->isPointerTy() &&
      call.getArgOperand(1)->getType()->isIntegerTy())
    return {AllocationKind::Fresh, true};
  if ((name == "_Znwm" || name == "_Znam" || name == "_Znwj" ||
       name == "_Znaj") &&
      integerArguments(1))
    return {AllocationKind::Fresh, false};
  return {};
}

bool deallocationCall(const llvm::CallBase &call) {
  const llvm::Function *function = directCallee(call);
  return function && function->isDeclaration() &&
         function->getName() == "free" && call.arg_size() == 1 &&
         call.getArgOperand(0)->getType()->isPointerTy() &&
         call.getType()->isVoidTy();
}

bool returnsInteriorPointer(const llvm::Function *function) {
  if (!knownReadOnly(function) || !function->getReturnType()->isPointerTy())
    return false;
  const llvm::StringRef name = function->getName();
  return name == "strchr" || name == "strrchr" || name == "strstr" ||
         name == "strpbrk" || name == "memchr" || name == "rawmemchr";
}

std::string nameOf(const llvm::Value &value) {
  if (value.hasName())
    return value.getName().str();
  std::string result;
  llvm::raw_string_ostream out(result);
  value.printAsOperand(out, false);
  return out.str();
}

// Flattened scalar leaf field descriptor (Kahlon PLDI '08 Remark 1).
struct FieldSpec {
  std::uint64_t offset = 0;
  std::uint64_t size_bytes = 0;
  std::uint64_t ptr_bytes = 0;
  bool is_pointer = false;
  bool inside_array = false;
  unsigned pointer_addrspace = 0;
};

constexpr std::size_t MAX_FLATTENED_FIELDS = 256;

bool flattenType(llvm::Type *ty, const llvm::DataLayout &layout,
                 std::uint64_t base_offset, bool inside_array,
                 std::vector<FieldSpec> &fields,
                 std::vector<ArrayRegion> &regions) {
  if (!ty || !ty->isSized())
    return false;
  if (fields.size() > MAX_FLATTENED_FIELDS)
    return false;
  if (ty->isPointerTy()) {
    const std::uint64_t psz = layout.getPointerTypeSize(ty);
    fields.push_back({base_offset, psz, psz, true, inside_array,
                      ty->getPointerAddressSpace()});
    return true;
  }
  if (auto *st = llvm::dyn_cast<llvm::StructType>(ty)) {
    const llvm::StructLayout *sl = layout.getStructLayout(st);
    for (unsigned i = 0; i < st->getNumElements(); ++i) {
      if (!flattenType(st->getElementType(i), layout,
                       base_offset + sl->getElementOffset(i), inside_array,
                       fields, regions))
        return false;
    }
    return true;
  }
  if (auto *at = llvm::dyn_cast<llvm::ArrayType>(ty)) {
    const std::uint64_t count = at->getNumElements();
    llvm::Type *elem = at->getElementType();
    if (!elem->isSized() || count == 0)
      return true;
    const std::uint64_t stride = layout.getTypeAllocSize(elem);
    if (stride == 0)
      return true;
    regions.push_back({base_offset, base_offset + count * stride, stride});
    return flattenType(elem, layout, base_offset, true, fields, regions);
  }
  const std::uint64_t sz = layout.getTypeAllocSize(ty);
  fields.push_back({base_offset, sz, 0, false, inside_array, 0});
  return true;
}

bool containsPointerType(llvm::Type *ty, std::set<llvm::Type *> &seen) {
  if (!ty || !seen.insert(ty).second)
    return false;
  if (ty->isPointerTy())
    return true;
  if (auto *st = llvm::dyn_cast<llvm::StructType>(ty)) {
    for (unsigned i = 0; i < st->getNumElements(); ++i)
      if (containsPointerType(st->getElementType(i), seen))
        return true;
  } else if (auto *at = llvm::dyn_cast<llvm::ArrayType>(ty)) {
    return containsPointerType(at->getElementType(), seen);
  }
  return false;
}

// Inspect bitcast users of a heap allocation to see if it is consistently cast
// to a single sized StructType (enabling field-sensitive heap objects!).
llvm::StructType *inferHeapStructType(const llvm::CallBase &call) {
  llvm::StructType *inferred = nullptr;
  for (const llvm::User *user : call.users()) {
    const auto *bc = llvm::dyn_cast<llvm::BitCastInst>(user);
    if (!bc)
      continue;
    auto *ptr_ty = llvm::dyn_cast<llvm::PointerType>(bc->getType());
    if (!ptr_ty)
      continue;
    auto *st =
        llvm::dyn_cast<llvm::StructType>(ptr_ty->getPointerElementType());
    if (!st || !st->isSized())
      return nullptr;
    if (inferred && inferred != st)
      return nullptr;
    inferred = st;
  }
  return inferred;
}

} // namespace

struct BootstrapAA::Impl {
  const llvm::Module &module;
  Program program;
  std::unique_ptr<Analysis> analysis;
  std::unordered_map<const llvm::Value *, Id> values;
  std::vector<const llvm::Value *> value_to_llvm;
  std::unordered_map<const llvm::Value *, Id> objects;
  std::map<std::pair<const llvm::Value *, std::uint64_t>, Id> field_objects;
  std::unordered_map<const llvm::Function *, Id> functions;
  std::unordered_map<const llvm::BasicBlock *, Id> blocks;
  std::unordered_map<const llvm::Instruction *, Id> sites;
  std::vector<const llvm::Value *> allocations{nullptr, nullptr};
  std::set<const llvm::Constant *> resolving;
  std::vector<bool> recursive;
  bool null_may_be_valid = false;
  Id null_value = INVALID;

  // Path-sensitivity condition-variable canonicalization per function.
  struct PredKey {
    const llvm::Function *func = nullptr;
    unsigned pred = 0;
    const llvm::Value *lhs = nullptr;
    const llvm::Value *rhs = nullptr;
    bool operator<(const PredKey &o) const {
      return std::tie(func, pred, lhs, rhs) <
             std::tie(o.func, o.pred, o.lhs, o.rhs);
    }
  };
  std::map<PredKey, Id> icmp_conditions;
  std::unordered_map<const llvm::Value *, Id> value_conditions;
  std::unordered_map<const llvm::Instruction *, std::vector<Id>>
      inst_kills_conditions;
  bool module_has_ctors = false;

  Impl(const llvm::Module &m, const llvm::Function *entry, Options options)
      : module(m) {
    std::string diagnostics;
    llvm::raw_string_ostream out(diagnostics);
    if (llvm::verifyModule(module, &out))
      throw std::invalid_argument("BootstrapAA: invalid LLVM module: " +
                                  out.str());
    if (!entry)
      entry = module.getFunction("main");
    if (!entry || entry->getParent() != &module || entry->isDeclaration())
      throw std::invalid_argument(
          "BootstrapAA: supply a defined entry function (default: main)");

    for (const llvm::Function &function : module) {
      null_may_be_valid |=
          function.hasFnAttribute(llvm::Attribute::NullPointerIsValid);
      Id id = program.addFunction(function.getName().str(),
                                  function.isDeclaration());
      functions.emplace(&function, id);
      program.functions[id].writes_memory = !knownReadOnly(&function) &&
                                            !function.doesNotAccessMemory() &&
                                            !function.onlyReadsMemory();
      Object object;
      object.name = nameOf(function);
      object.kind = ObjectKind::Function;
      object.function = id;
      addObject(function, std::move(object));
    }
    program.entry = functions.at(entry);
    computeRecursion();
    const llvm::DataLayout &layout = module.getDataLayout();

    // Globals (with Remark 1 struct/array flattening).
    for (const llvm::GlobalVariable &global : module.globals()) {
      createStorageObjects(global, global.getValueType(), ObjectKind::Global,
                           /*allow_singleton=*/true, /*fresh_empty_init=*/false,
                           nameOf(global), layout,
                           global.getType()->getPointerAddressSpace());
    }

    // Pre-allocate function arguments, blocks, SSA IDs, stack allocas, and heap
    // objects before resolving constants or PHIs.
    for (const llvm::Function &function : module) {
      Id f = functions.at(&function);
      for (const llvm::Argument &argument : function.args()) {
        program.functions[f].parameters.push_back(valueId(argument));
        if (argument.hasByValAttr()) {
          llvm::Type *byval_ty = argument.getParamByValType();
          if (byval_ty && byval_ty->isSized() &&
              (byval_ty->isStructTy() || byval_ty->isArrayTy())) {
            createStorageObjects(
                argument, byval_ty, ObjectKind::Stack,
                /*allow_singleton=*/false, /*fresh_empty_init=*/false,
                function.getName().str() + ":byval:" + nameOf(argument), layout,
                argument.getType()->getPointerAddressSpace());
          } else {
            Object object;
            object.name =
                function.getName().str() + ":byval:" + nameOf(argument);
            object.kind = ObjectKind::Stack;
            if (byval_ty && byval_ty->isPointerTy())
              object.bytes = layout.getPointerTypeSize(byval_ty);
            addObject(argument, std::move(object));
          }
        }
      }
      for (const llvm::BasicBlock &block : function) {
        blocks.emplace(&block, program.addBlock(f));
        for (const llvm::Instruction &instruction : block) {
          if (instruction.getType()->isPointerTy())
            valueId(instruction);
          if (const auto *alloca =
                  llvm::dyn_cast<llvm::AllocaInst>(&instruction)) {
            const auto *count =
                llvm::dyn_cast<llvm::ConstantInt>(alloca->getArraySize());
            const bool is_static_one =
                alloca->isStaticAlloca() && count && count->isOne();
            const bool allow_singleton = is_static_one && !recursive[f];
            llvm::Type *alloc_ty = alloca->getAllocatedType();
            if (count && !count->isOne() && alloc_ty->isSized()) {
              // Static array allocation `alloca T, i32 N`: flatten as [N x T]
              // (non-singleton).
              llvm::Type *arr_ty =
                  llvm::ArrayType::get(alloc_ty, count->getZExtValue());
              createStorageObjects(
                  instruction, arr_ty, ObjectKind::Stack,
                  /*allow_singleton=*/false, /*fresh_empty_init=*/true,
                  function.getName().str() + ":" + nameOf(instruction), layout,
                  alloca->getType()->getPointerAddressSpace());
            } else {
              createStorageObjects(
                  instruction, alloc_ty, ObjectKind::Stack, allow_singleton,
                  /*fresh_empty_init=*/true,
                  function.getName().str() + ":" + nameOf(instruction), layout,
                  alloca->getType()->getPointerAddressSpace());
            }
          } else if (const auto *call =
                         llvm::dyn_cast<llvm::CallBase>(&instruction)) {
            const AllocationModel am = allocationModel(*call);
            if (am.kind != AllocationKind::None) {
              const bool fresh_empty = (am.kind == AllocationKind::Fresh);
              llvm::StructType *st = inferHeapStructType(*call);
              if (st) {
                createStorageObjects(
                    instruction, st, ObjectKind::Heap,
                    /*allow_singleton=*/false, fresh_empty,
                    function.getName().str() + ":" + nameOf(instruction),
                    layout, call->getType()->getPointerAddressSpace());
              } else {
                Object object;
                object.name =
                    function.getName().str() + ":" + nameOf(instruction);
                object.kind = ObjectKind::Heap;
                if (fresh_empty)
                  object.initial = PointsToSet{};
                // Untyped heap allocation: allow pointer-sized loads/stores
                // (weak updates) instead of collapsing all heap accesses to top.
                // Non-pointer stores still emit Havoc, preserving soundness.
                object.bytes = layout.getPointerSize(
                    call->getType()->getPointerAddressSpace());
                addObject(instruction, std::move(object));
              }
            }
          }
        }
      }
    }

    // Initialize globals (both scalar pointer globals and flattened struct /
    // array global initializers!).
    const bool has_ctors = module.getNamedGlobal("llvm.global_ctors") != nullptr;
    module_has_ctors = has_ctors;
    for (const llvm::GlobalVariable &global : module.globals()) {
      valueId(global);
      if (!global.hasInitializer() || global.isExternallyInitialized() ||
          has_ctors)
        continue;
      const Id base = objects.at(&global);
      if (global.getValueType()->isPointerTy()) {
        program.objects[base].initial =
            constantPoints(*global.getInitializer());
      } else if (program.objects[base].field_sensitive) {
        // Clear pointer fields' default top initializers before populating from
        // the concrete constant aggregate initializer.
        for (auto [off, fid] : program.fields_of_base[base])
          if (program.objects[fid].bytes > 0)
            program.objects[fid].initial = PointsToSet{};
        populateConstantAggregate(base, global.getInitializer(), 0, layout);
        // Any pointer field left empty (e.g. undef) conservatively becomes top.
        for (auto [off, fid] : program.fields_of_base[base])
          if (program.objects[fid].bytes > 0 &&
              program.objects[fid].initial.empty())
            program.objects[fid].initial = PointsToSet::top();
      }
    }

    for (const llvm::GlobalAlias &alias : module.aliases())
      valueId(alias);

    // Pre-assign boolean condition IDs for conditional branches (for optional
    // ROBDD path sensitivity, Kahlon §3 p. 258).
    for (const llvm::Function &function : module) {
      if (function.isDeclaration())
        continue;
      for (const llvm::BasicBlock &block : function) {
        const auto *br =
            llvm::dyn_cast<llvm::BranchInst>(block.getTerminator());
        if (br && br->isConditional())
          (void)resolveBranchCondition(function, *br->getCondition());
      }
    }

    for (const llvm::Function &function : module) {
      valueId(function);
      if (!function.isDeclaration()) {
        for (const llvm::Argument &argument : function.args()) {
          if (!argument.hasByValAttr())
            continue;
          Instruction copy;
          copy.opcode = Opcode::Allocate;
          copy.object = objects.at(&argument);
          copy.result = valueId(argument);
          program.append(functions.at(&function), 0, copy);
        }
      }
      for (const llvm::BasicBlock &block : function) {
        for (const llvm::Instruction &instruction : block) {
          for (const llvm::Use &operand : instruction.operands())
            if (operand->getType()->isPointerTy())
              valueId(*operand);
          lower(function, block, instruction);
        }
      }
    }

    for (const llvm::Function &function : module) {
      for (const llvm::BasicBlock &block : function) {
        const auto *br =
            llvm::dyn_cast<llvm::BranchInst>(block.getTerminator());
        Id br_cond_var = INVALID;
        bool br_cond_pol = true;
        if (br && br->isConditional())
          std::tie(br_cond_var, br_cond_pol) =
              resolveBranchCondition(function, *br->getCondition());

        const unsigned num_succ = block.getTerminator()->getNumSuccessors();
        for (unsigned sidx = 0; sidx < num_succ; ++sidx) {
          const llvm::BasicBlock *successor =
              block.getTerminator()->getSuccessor(sidx);
          Edge edge;
          edge.target = blocks.at(successor);
          if (br && br->isConditional() && br_cond_var != INVALID) {
            edge.cond_var = br_cond_var;
            edge.cond_polarity = (sidx == 0) ? br_cond_pol : !br_cond_pol;
          }
          for (const llvm::Instruction &instruction : *successor) {
            const auto *phi = llvm::dyn_cast<llvm::PHINode>(&instruction);
            if (!phi)
              break;
            if (phi->getType()->isPointerTy())
              edge.phi.emplace_back(
                  valueId(*phi),
                  valueId(*phi->getIncomingValueForBlock(&block)));
          }
          program.functions[functions.at(&function)]
              .blocks[blocks.at(&block)]
              .successors.push_back(std::move(edge));
        }
      }
    }
    analysis = std::make_unique<Analysis>(program, options);
  }

  void createStorageObjects(const llvm::Value &allocation, llvm::Type *ty,
                            ObjectKind kind, bool allow_singleton,
                            bool fresh_empty_init,
                            const std::string &base_name,
                            const llvm::DataLayout &layout,
                            unsigned addrspace) {
    const PointsToSet default_init =
        fresh_empty_init ? PointsToSet{} : PointsToSet::top();
    if (ty->isPointerTy()) {
      Object obj;
      obj.name = base_name;
      obj.kind = kind;
      obj.singleton = allow_singleton;
      obj.bytes = layout.getPointerTypeSize(ty);
      obj.size_bytes = obj.bytes;
      obj.initial = default_init;
      const Id id = addObject(allocation, std::move(obj));
      field_objects[{&allocation, 0}] = id;
      return;
    }

    if (ty->isSized() && (ty->isStructTy() || ty->isArrayTy())) {
      std::vector<FieldSpec> fields;
      std::vector<ArrayRegion> regions;
      if (flattenType(ty, layout, 0, false, fields, regions) &&
          !fields.empty() && fields.size() <= MAX_FLATTENED_FIELDS) {
        // Sort and deduplicate by offset (keeping pointer fields if colliding).
        std::map<std::uint64_t, FieldSpec> by_off;
        for (const FieldSpec &fs : fields) {
          auto it = by_off.find(fs.offset);
          if (it == by_off.end() || (!it->second.is_pointer && fs.is_pointer))
            by_off[fs.offset] = fs;
        }
        // Ensure offset 0 exists as the base object.
        if (!by_off.count(0))
          by_off[0] = {0, 1, 0, false, false, 0};

        const FieldSpec &f0 = by_off[0];
        Object base_obj;
        base_obj.name = base_name;
        base_obj.kind = kind;
        base_obj.singleton =
            allow_singleton && f0.is_pointer && !f0.inside_array;
        base_obj.bytes = f0.ptr_bytes;
        base_obj.size_bytes = layout.getTypeAllocSize(ty);
        base_obj.initial = default_init;
        base_obj.field_sensitive = true;
        base_obj.array_regions = std::move(regions);
        const Id base_id = addObject(allocation, std::move(base_obj));
        field_objects[{&allocation, 0}] = base_id;

        for (const auto &[off, fs] : by_off) {
          if (off == 0)
            continue;
          Object fobj;
          fobj.name = base_name + "+" + std::to_string(off);
          fobj.kind = kind;
          fobj.singleton = allow_singleton && fs.is_pointer && !fs.inside_array;
          fobj.bytes = fs.ptr_bytes;
          fobj.size_bytes = fs.size_bytes;
          fobj.initial = default_init;
          const Id fid = program.addFieldObject(base_id, off, std::move(fobj));
          allocations.push_back(&allocation);
          field_objects[{&allocation, off}] = fid;
        }
        return;
      }
      // Fallback for oversized/opaque aggregates: allow pointer-sized accesses
      // (weak updates) if aggregate contains pointers.
      std::set<llvm::Type *> seen;
      Object obj;
      obj.name = base_name;
      obj.kind = kind;
      obj.singleton = false;
      obj.initial = default_init;
      if (containsPointerType(ty, seen))
        obj.bytes = layout.getPointerSize(addrspace);
      if (ty->isSized())
        obj.size_bytes = layout.getTypeAllocSize(ty);
      const Id id = addObject(allocation, std::move(obj));
      field_objects[{&allocation, 0}] = id;
      return;
    }

    // Scalar non-pointer storage (e.g., i8, i32, double).
    Object obj;
    obj.name = base_name;
    obj.kind = kind;
    obj.singleton = false;
    obj.bytes = 0;
    obj.initial = default_init;
    if (ty->isSized())
      obj.size_bytes = layout.getTypeAllocSize(ty);
    const Id id = addObject(allocation, std::move(obj));
    field_objects[{&allocation, 0}] = id;
  }

  void populateConstantAggregate(Id base, const llvm::Constant *c,
                                 std::uint64_t offset,
                                 const llvm::DataLayout &layout) {
    if (!c)
      return;
    llvm::Type *ty = c->getType();
    if (ty->isPointerTy()) {
      const std::uint64_t folded = program.foldOffset(base, offset);
      const PointsToSet target_field = program.resolveField(base, folded, true);
      const PointsToSet pts = constantPoints(*c);
      for (Id fid : target_field.objects())
        if (fid >= 2 && fid < program.objects.size() &&
            program.objects[fid].bytes > 0)
          program.objects[fid].initial.join(pts);
      return;
    }
    if (llvm::isa<llvm::ConstantAggregateZero>(c)) {
      const PointsToSet null_or_top =
          null_may_be_valid ? PointsToSet::top() : PointsToSet(NULL_OBJECT);
      const std::uint64_t span =
          ty->isSized() ? layout.getTypeAllocSize(ty) : 0;
      for (auto [foff, fid] : program.fields_of_base[base]) {
        if (program.objects[fid].bytes > 0 && foff >= offset &&
            (span == 0 || foff < offset + span))
          program.objects[fid].initial.join(null_or_top);
      }
      return;
    }
    if (const auto *cs = llvm::dyn_cast<llvm::ConstantStruct>(c)) {
      const llvm::StructLayout *sl = layout.getStructLayout(cs->getType());
      for (unsigned i = 0; i < cs->getNumOperands(); ++i)
        populateConstantAggregate(base, cs->getOperand(i),
                                  offset + sl->getElementOffset(i), layout);
      return;
    }
    if (const auto *ca = llvm::dyn_cast<llvm::ConstantArray>(c)) {
      llvm::Type *elem = ca->getType()->getElementType();
      const std::uint64_t stride =
          elem->isSized() ? layout.getTypeAllocSize(elem) : 0;
      for (unsigned i = 0; i < ca->getNumOperands(); ++i)
        populateConstantAggregate(base, ca->getOperand(i),
                                  offset + i * stride, layout);
      return;
    }
  }

  // Canonicalize a branch condition i1 Value into (cond_var, polarity).
  std::pair<Id, bool> resolveBranchCondition(const llvm::Function &function,
                                             const llvm::Value &cond) {
    (void)function;
    bool polarity = true;
    const llvm::Value *cur = &cond;
    while (const auto *bin = llvm::dyn_cast<llvm::BinaryOperator>(cur)) {
      if (bin->getOpcode() == llvm::Instruction::Xor) {
        if (const auto *ci =
                llvm::dyn_cast<llvm::ConstantInt>(bin->getOperand(1))) {
          if (ci->isOne()) {
            polarity = !polarity;
            cur = bin->getOperand(0);
            continue;
          }
        }
        if (const auto *ci =
                llvm::dyn_cast<llvm::ConstantInt>(bin->getOperand(0))) {
          if (ci->isOne()) {
            polarity = !polarity;
            cur = bin->getOperand(1);
            continue;
          }
        }
      }
      break;
    }
    if (llvm::isa<llvm::ConstantInt>(cur))
      return {INVALID, true};

    auto recordKill = [&](const llvm::Instruction *inst, Id cid) {
      if (!inst || cid == INVALID)
        return;
      auto &vec = inst_kills_conditions[inst];
      if (std::find(vec.begin(), vec.end(), cid) == vec.end())
        vec.push_back(cid);
    };

    auto recordEntryKill = [&](const llvm::Function *fn, Id cid) {
      if (!fn || cid == INVALID)
        return;
      auto fit = functions.find(fn);
      if (fit == functions.end())
        return;
      auto &vec = program.functions[fit->second].entry_kills_conditions;
      if (std::find(vec.begin(), vec.end(), cid) == vec.end())
        vec.push_back(cid);
    };

    auto recordGlobalCond = [&](Id cid) {
      if (cid == INVALID)
        return;
      auto &vec = program.global_conditions;
      if (std::find(vec.begin(), vec.end(), cid) == vec.end())
        vec.push_back(cid);
    };

    // Sound cross-function operand canonicalization for interprocedural ROBDD
    // path sensitivity:
    //  1. Unique actual argument passed by all direct call sites to a
    //     non-recursive, address-not-taken function.
    //  2. Direct load from a non-escaping constant or local-linkage global
    //     variable (recording all direct stores to that global as condition
    //     kills and marking mutable globals for callback soundness).
    auto isNonEscapingDirectGlobal = [&](const llvm::GlobalVariable *gv) {
      if (!gv)
        return false;
      if (gv->isConstant())
        return true;
      if (module_has_ctors || !gv->hasLocalLinkage())
        return false;
      for (const llvm::User *u : gv->users()) {
        if (const auto *ld = llvm::dyn_cast<llvm::LoadInst>(u)) {
          if (ld->getPointerOperand() != gv)
            return false;
        } else if (const auto *st = llvm::dyn_cast<llvm::StoreInst>(u)) {
          if (st->getPointerOperand() != gv)
            return false;
        } else {
          return false;
        }
      }
      return true;
    };

    auto canonicalizeOperand =
        [&](const llvm::Value *v,
            std::vector<const llvm::Instruction *> &kill_sites,
            std::vector<const llvm::Function *> &entry_kill_fns,
            bool &is_mutable_global) -> const llvm::Value * {
      const llvm::Value *cur_v = v;
      std::set<const llvm::Value *> visited;
      while (cur_v && visited.insert(cur_v).second) {
        if (const auto *arg = llvm::dyn_cast<llvm::Argument>(cur_v)) {
          const llvm::Function *parent = arg->getParent();
          auto fit = functions.find(parent);
          if (!parent || parent->isDeclaration() || fit == functions.end() ||
              recursive[fit->second] || parent->hasAddressTaken())
            break;
          const llvm::Value *shared_actual = nullptr;
          bool ok = true;
          for (const llvm::User *u : parent->users()) {
            const auto *cb = llvm::dyn_cast<llvm::CallBase>(u);
            if (!cb || directCallee(*cb) != parent ||
                arg->getArgNo() >= cb->arg_size()) {
              ok = false;
              break;
            }
            const llvm::Value *act = cb->getArgOperand(arg->getArgNo());
            if (!shared_actual)
              shared_actual = act;
            else if (shared_actual != act) {
              ok = false;
              break;
            }
          }
          if (!ok || !shared_actual)
            break;
          cur_v = shared_actual;
          continue;
        }
        if (const auto *ld = llvm::dyn_cast<llvm::LoadInst>(cur_v)) {
          if (const auto *gv = llvm::dyn_cast<llvm::GlobalVariable>(
                  ld->getPointerOperand()->stripPointerCasts())) {
            if (isNonEscapingDirectGlobal(gv)) {
              for (const llvm::User *u : gv->users()) {
                if (const auto *st = llvm::dyn_cast<llvm::StoreInst>(u)) {
                  kill_sites.push_back(st);
                  is_mutable_global = true;
                }
              }
              cur_v = gv;
              break;
            }
          }
        }
        break;
      }
      if (const auto *ci = llvm::dyn_cast<llvm::Instruction>(cur_v))
        kill_sites.push_back(ci);
      else if (const auto *arg = llvm::dyn_cast<llvm::Argument>(cur_v))
        entry_kill_fns.push_back(arg->getParent());
      return cur_v;
    };

    if (const auto *icmp = llvm::dyn_cast<llvm::ICmpInst>(cur)) {
      llvm::CmpInst::Predicate pred = icmp->getPredicate();
      std::vector<const llvm::Instruction *> kills;
      std::vector<const llvm::Function *> entry_kills;
      bool is_mutable_global = false;
      const llvm::Value *lhs = canonicalizeOperand(
          icmp->getOperand(0), kills, entry_kills, is_mutable_global);
      const llvm::Value *rhs = canonicalizeOperand(
          icmp->getOperand(1), kills, entry_kills, is_mutable_global);
      // Normalize inverted predicates (ne -> !eq, sge -> !slt, etc.) so
      // `if (x == 0)` and `if (x != 0)` share one boolean variable!
      switch (pred) {
      case llvm::CmpInst::ICMP_NE:
      case llvm::CmpInst::ICMP_SGE:
      case llvm::CmpInst::ICMP_SGT:
      case llvm::CmpInst::ICMP_UGE:
      case llvm::CmpInst::ICMP_UGT:
        pred = llvm::CmpInst::getInversePredicate(pred);
        polarity = !polarity;
        break;
      default:
        break;
      }
      if (pred == llvm::CmpInst::ICMP_EQ && rhs < lhs)
        std::swap(lhs, rhs);
      // Canonicalized llvm::Value pointers are module-unique per Instruction /
      // Argument / GlobalVariable / Constant, so module-global PredKey
      // matching unifies caller and callee comparisons over shared values.
      PredKey pk{nullptr, static_cast<unsigned>(pred), lhs, rhs};
      auto it = icmp_conditions.find(pk);
      Id cid = INVALID;
      if (it != icmp_conditions.end()) {
        cid = it->second;
      } else {
        cid = program.addCondition();
        icmp_conditions.emplace(pk, cid);
      }
      for (const llvm::Instruction *ki : kills)
        recordKill(ki, cid);
      for (const llvm::Function *kf : entry_kills)
        recordEntryKill(kf, cid);
      if (is_mutable_global)
        recordGlobalCond(cid);
      return {cid, polarity};
    }

    std::vector<const llvm::Instruction *> kills;
    std::vector<const llvm::Function *> entry_kills;
    bool is_mutable_global = false;
    const llvm::Value *canon_cur =
        canonicalizeOperand(cur, kills, entry_kills, is_mutable_global);
    auto it = value_conditions.find(canon_cur);
    Id cid = INVALID;
    if (it != value_conditions.end()) {
      cid = it->second;
    } else {
      cid = program.addCondition();
      value_conditions.emplace(canon_cur, cid);
    }
    for (const llvm::Instruction *ki : kills)
      recordKill(ki, cid);
    for (const llvm::Function *kf : entry_kills)
      recordEntryKill(kf, cid);
    if (is_mutable_global)
      recordGlobalCond(cid);
    return {cid, polarity};
  }

  Id addObject(const llvm::Value &allocation, Object object) {
    Id id = program.addObject(std::move(object));
    objects.emplace(&allocation, id);
    allocations.push_back(&allocation);
    return id;
  }

  Id nullValue() {
    if (null_value == INVALID)
      null_value =
          program.addConstant(PointsToSet(NULL_OBJECT), "<null-value>");
    return null_value;
  }

  void computeRecursion() {
    std::vector<std::set<Id>> graph(program.functions.size());
    for (const llvm::Function &function : module)
      for (const llvm::BasicBlock &block : function)
        for (const llvm::Instruction &instruction : block)
          if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
            if (const llvm::Function *callee = directCallee(*call)) {
              if (!callee->isDeclaration())
                graph[functions.at(&function)].insert(functions.at(callee));
            } else {
              for (const llvm::Function &candidate : module)
                if (!candidate.isDeclaration())
                  graph[functions.at(&function)].insert(
                      functions.at(&candidate));
            }
          }
    recursive.assign(graph.size(), false);
    for (Id f = 0; f < graph.size(); ++f) {
      std::vector<Id> work(graph[f].begin(), graph[f].end());
      std::set<Id> seen;
      while (!work.empty()) {
        Id current = work.back();
        work.pop_back();
        if (current == f) {
          recursive[f] = true;
          break;
        }
        if (seen.insert(current).second)
          work.insert(work.end(), graph[current].begin(), graph[current].end());
      }
    }
  }

  // Analyze GEP indices into (constant_byte_offset, all_const, array_stride).
  std::tuple<std::int64_t, bool, std::uint64_t>
  analyzeGEP(const llvm::GEPOperator &gep) const {
    const llvm::DataLayout &layout = module.getDataLayout();
    const unsigned as = gep.getPointerAddressSpace();
    llvm::APInt ap_off(layout.getIndexSizeInBits(as), 0);
    if (gep.accumulateConstantOffset(layout, ap_off))
      return {ap_off.getSExtValue(), true, 0};

    std::int64_t const_delta = 0;
    std::uint64_t var_stride = 0;
    for (auto gti = llvm::gep_type_begin(gep), gte = llvm::gep_type_end(gep);
         gti != gte; ++gti) {
      llvm::Value *idx = gti.getOperand();
      if (llvm::StructType *st = gti.getStructTypeOrNull()) {
        if (const auto *ci = llvm::dyn_cast<llvm::ConstantInt>(idx)) {
          const llvm::StructLayout *sl = layout.getStructLayout(st);
          const_delta +=
              static_cast<std::int64_t>(sl->getElementOffset(ci->getZExtValue()));
        }
      } else {
        llvm::Type *elem = gti.getIndexedType();
        const std::uint64_t stride =
            elem && elem->isSized() ? layout.getTypeAllocSize(elem) : 0;
        if (const auto *ci = llvm::dyn_cast<llvm::ConstantInt>(idx)) {
          const_delta += ci->getSExtValue() * static_cast<std::int64_t>(stride);
        } else if (stride > 0) {
          var_stride = stride;
        }
      }
    }
    return {const_delta, false, var_stride};
  }

  PointsToSet constantPoints(const llvm::Constant &constant) {
    auto object = objects.find(&constant);
    if (object != objects.end())
      return PointsToSet(object->second);
    if (llvm::isa<llvm::ConstantPointerNull>(constant)) {
      if (null_may_be_valid ||
          constant.getType()->getPointerAddressSpace() != 0)
        return PointsToSet::top();
      return PointsToSet(NULL_OBJECT);
    }
    if (!resolving.insert(&constant).second)
      return PointsToSet::top();
    PointsToSet result = PointsToSet::top();
    if (const auto *alias = llvm::dyn_cast<llvm::GlobalAlias>(&constant)) {
      if (alias->getAliasee())
        result = constantPoints(*alias->getAliasee());
    } else if (const auto *gep = llvm::dyn_cast<llvm::GEPOperator>(&constant)) {
      if (gep->isInBounds() || gep->hasAllZeroIndices()) {
        if (const auto *base =
                llvm::dyn_cast<llvm::Constant>(gep->getPointerOperand())) {
          const PointsToSet base_pts = constantPoints(*base);
          auto [delta, known, stride] = analyzeGEP(*gep);
          if (base_pts.isTop()) {
            result = PointsToSet::top();
          } else {
            result = PointsToSet{};
            for (Id obj : base_pts.objects())
              result.join(program.resolveField(obj, delta, known, stride));
          }
        }
      }
    } else if (const auto *expression =
                   llvm::dyn_cast<llvm::ConstantExpr>(&constant)) {
      if (expression->getOpcode() == llvm::Instruction::BitCast)
        result = constantPoints(
            *llvm::cast<llvm::Constant>(expression->getOperand(0)));
    }
    resolving.erase(&constant);
    return result;
  }

  Id valueId(const llvm::Value &value) {
    if (!value.getType()->isPointerTy())
      return INVALID;
    auto it = values.find(&value);
    if (it != values.end())
      return it->second;
    Value normalized;
    normalized.name = nameOf(value);
    if (const auto *constant = llvm::dyn_cast<llvm::Constant>(&value)) {
      normalized.constant = true;
      normalized.initial = constantPoints(*constant);
    } else if (const auto *instruction =
                   llvm::dyn_cast<llvm::Instruction>(&value)) {
      normalized.function = functions.at(instruction->getFunction());
    } else if (const auto *argument = llvm::dyn_cast<llvm::Argument>(&value)) {
      normalized.function = functions.at(argument->getParent());
    } else {
      normalized.constant = true;
      normalized.initial = PointsToSet::top();
    }
    Id id = program.addValue(std::move(normalized));
    values.emplace(&value, id);
    if (value_to_llvm.size() <= id)
      value_to_llvm.resize(id + 1, nullptr);
    value_to_llvm[id] = &value;
    return id;
  }

  void lower(const llvm::Function &function, const llvm::BasicBlock &block,
             const llvm::Instruction &instruction) {
    const llvm::DataLayout &layout = module.getDataLayout();
    Instruction i;
    i.result = valueId(instruction);
    i.opcode = i.result == INVALID ? Opcode::Nop : Opcode::Unknown;
    auto cond_it = inst_kills_conditions.find(&instruction);
    if (cond_it != inst_kills_conditions.end() && !cond_it->second.empty()) {
      i.defines_condition = cond_it->second.front();
      if (cond_it->second.size() > 1)
        i.kills_conditions.assign(cond_it->second.begin() + 1,
                                  cond_it->second.end());
    }

    if (llvm::isa<llvm::AllocaInst>(instruction)) {
      i.opcode = Opcode::Allocate;
      i.object = objects.at(&instruction);
    } else if (const auto *load =
                   llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
      if (i.result != INVALID) {
        i.opcode = Opcode::Load;
        i.operands = {valueId(*load->getPointerOperand())};
        i.read_bytes = layout.getPointerTypeSize(load->getType());
      }
    } else if (const auto *store =
                   llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
      i.operands = {valueId(*store->getPointerOperand())};
      if (store->getValueOperand()->getType()->isPointerTy()) {
        i.opcode = Opcode::Store;
        i.operands.push_back(valueId(*store->getValueOperand()));
        i.write_bytes =
            layout.getPointerTypeSize(store->getValueOperand()->getType());
      } else {
        // Non-pointer store: havoc ONLY fields overlapping the written byte
        // range (leaving sibling struct pointer fields untouched!).
        i.opcode = Opcode::Havoc;
        llvm::Type *vty = store->getValueOperand()->getType();
        if (vty->isSized())
          i.write_bytes = layout.getTypeStoreSize(vty);
      }
    } else if (const auto *bitcast =
                   llvm::dyn_cast<llvm::BitCastInst>(&instruction)) {
      if (i.result != INVALID) {
        i.opcode = Opcode::Copy;
        i.operands = {valueId(*bitcast->getOperand(0))};
      }
    } else if (const auto *gep =
                   llvm::dyn_cast<llvm::GetElementPtrInst>(&instruction)) {
      if (i.result != INVALID &&
          (gep->isInBounds() || gep->hasAllZeroIndices())) {
        const auto *gep_op = llvm::cast<llvm::GEPOperator>(gep);
        auto [delta, known, stride] = analyzeGEP(*gep_op);
        if (known && delta == 0 && stride == 0) {
          i.opcode = Opcode::Copy;
          i.operands = {valueId(*gep->getPointerOperand())};
        } else {
          i.opcode = Opcode::Field;
          i.operands = {valueId(*gep->getPointerOperand())};
          i.field_offset = delta;
          i.field_known = known;
          i.field_array_stride = stride;
        }
      }
    } else if (const auto *select =
                   llvm::dyn_cast<llvm::SelectInst>(&instruction)) {
      if (i.result != INVALID) {
        i.opcode = Opcode::Join;
        i.operands = {valueId(*select->getTrueValue()),
                      valueId(*select->getFalseValue())};
      }
    } else if (llvm::isa<llvm::PHINode>(instruction)) {
      i.opcode = Opcode::Nop; // Simultaneous assignments are on incoming edges.
    } else if (const auto *transfer =
                   llvm::dyn_cast<llvm::MemTransferInst>(&instruction)) {
      const auto *length =
          llvm::dyn_cast<llvm::ConstantInt>(transfer->getLength());
      if (length && length->isZero()) {
        i.opcode = Opcode::Nop;
      } else {
        i.opcode = Opcode::MemCopy;
        i.operands = {valueId(*transfer->getRawDest()),
                      valueId(*transfer->getRawSource())};
        i.write_bytes = length ? length->getZExtValue() : 0;
      }
    } else if (const auto *memory =
                   llvm::dyn_cast<llvm::MemIntrinsic>(&instruction)) {
      const auto *length =
          llvm::dyn_cast<llvm::ConstantInt>(memory->getLength());
      if (length && length->isZero()) {
        i.opcode = Opcode::Nop;
      } else {
        i.opcode = Opcode::Havoc;
        i.operands = {valueId(*memory->getRawDest())};
        i.write_bytes = length ? length->getZExtValue() : 0;
      }
    } else if (const auto *call =
                   llvm::dyn_cast<llvm::CallBase>(&instruction)) {
      const AllocationModel allocation = allocationModel(*call);
      if (allocation.kind != AllocationKind::None) {
        if (null_may_be_valid ||
            call->getType()->getPointerAddressSpace() != 0) {
          i.opcode = Opcode::Unknown;
        } else {
          i.opcode = Opcode::Allocate;
          i.object = objects.at(&instruction);
          i.may_be_null = allocation.mayBeNull;
          if (allocation.kind == AllocationKind::Reallocate)
            i.operands = {valueId(*call->getArgOperand(0))};
        }
      } else if (deallocationCall(*call)) {
        i.opcode = Opcode::Nop;
      } else {
        const llvm::Function *callee = directCallee(*call);
        if (returnsInteriorPointer(callee) && call->arg_size() != 0 &&
            call->getArgOperand(0)->getType()->isPointerTy()) {
          i.opcode = Opcode::Join;
          i.operands = {valueId(*call->getArgOperand(0)), nullValue()};
          sites.emplace(&instruction, program.append(functions.at(&function),
                                                     blocks.at(&block), i));
          return;
        }
        llvm::StringRef name = callee ? callee->getName() : llvm::StringRef();
        if (name.startswith("llvm.dbg.") || name.startswith("llvm.lifetime.") ||
            name == "llvm.assume") {
          i.opcode = Opcode::Nop;
        } else {
          i.opcode = Opcode::Call;
          i.writes_memory = writesMemory(*call);
          if (callee)
            i.callee = functions.at(callee);
          else if (!call->isInlineAsm())
            i.indirect_target = valueId(*call->getCalledOperand());
          for (const llvm::Use &argument : call->args())
            i.operands.push_back(valueId(*argument));
          i.opaque_alternative = llvm::isa<llvm::InvokeInst>(call) ||
                                 llvm::isa<llvm::CallBrInst>(call);
        }
      }
    } else if (const auto *atomic =
                   llvm::dyn_cast<llvm::AtomicRMWInst>(&instruction)) {
      i.opcode = Opcode::Havoc;
      i.operands = {valueId(*atomic->getPointerOperand())};
      if (atomic->getValOperand()->getType()->isSized())
        i.write_bytes =
            layout.getTypeStoreSize(atomic->getValOperand()->getType());
    } else if (const auto *atomic =
                   llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&instruction)) {
      i.opcode = Opcode::Havoc;
      i.operands = {valueId(*atomic->getPointerOperand())};
      if (atomic->getNewValOperand()->getType()->isSized())
        i.write_bytes =
            layout.getTypeStoreSize(atomic->getNewValOperand()->getType());
    } else if (const auto *ret =
                   llvm::dyn_cast<llvm::ReturnInst>(&instruction)) {
      i.opcode = Opcode::Return;
      if (ret->getReturnValue() &&
          ret->getReturnValue()->getType()->isPointerTy())
        i.operands = {valueId(*ret->getReturnValue())};
    } else if (llvm::isa<llvm::ResumeInst>(instruction) ||
               (llvm::isa<llvm::CleanupReturnInst>(instruction) &&
                llvm::cast<llvm::CleanupReturnInst>(instruction)
                    .unwindsToCaller())) {
      i.opcode = Opcode::Return;
    }
    sites.emplace(&instruction, program.append(functions.at(&function),
                                               blocks.at(&block), i));
  }

  Id queryValue(const llvm::Value &value) const {
    auto it = values.find(&value);
    if (it == values.end())
      throw std::invalid_argument(
          "BootstrapAA: query must name a scalar pointer in this module");
    return it->second;
  }

  Id querySite(const llvm::Instruction &instruction) const {
    auto it = sites.find(&instruction);
    if (it == sites.end())
      throw std::invalid_argument(
          "BootstrapAA: query site is in another module");
    return it->second;
  }

  Context context(const CallContext &path) const {
    Context result;
    for (const llvm::CallBase *call : path) {
      if (!call)
        throw std::invalid_argument("BootstrapAA: null call site in context");
      result.push_back(querySite(*call));
    }
    return result;
  }

  LLVMAliasResult translateAliases(const AliasResult &raw) const {
    LLVMAliasResult out;
    out.status = raw.status;
    out.may_alias_all = raw.may_alias_all;
    for (Id id : raw.aliases) {
      if (id < value_to_llvm.size() && value_to_llvm[id])
        out.aliases.push_back(value_to_llvm[id]);
    }
    return out;
  }
};

BootstrapAA::BootstrapAA(const llvm::Module &module,
                         const llvm::Function *entry, Options options)
    : m_impl(std::make_unique<Impl>(module, entry, options)) {}
BootstrapAA::~BootstrapAA() = default;
BootstrapAA::BootstrapAA(BootstrapAA &&) noexcept = default;
BootstrapAA &BootstrapAA::operator=(BootstrapAA &&) noexcept = default;

QueryResult BootstrapAA::pointsTo(const llvm::Value &value,
                                  const llvm::Instruction &site,
                                  const CallContext &context, Point point) {
  return m_impl->analysis->pointsTo(m_impl->queryValue(value),
                                    m_impl->querySite(site),
                                    m_impl->context(context), point);
}

QueryResult BootstrapAA::pointsToAllContexts(const llvm::Value &value,
                                             const llvm::Instruction &site,
                                             Point point) {
  return m_impl->analysis->pointsToAllContexts(m_impl->queryValue(value),
                                               m_impl->querySite(site), point);
}

BootstrapAA::LLVMAliasResult
BootstrapAA::aliases(const llvm::Value &value, const llvm::Instruction &site,
                     const CallContext &context, Point point) {
  return m_impl->translateAliases(m_impl->analysis->aliases(
      m_impl->queryValue(value), m_impl->querySite(site),
      m_impl->context(context), point));
}

BootstrapAA::LLVMAliasResult
BootstrapAA::aliasesAllContexts(const llvm::Value &value,
                                const llvm::Instruction &site, Point point) {
  return m_impl->translateAliases(m_impl->analysis->aliasesAllContexts(
      m_impl->queryValue(value), m_impl->querySite(site), point));
}

void BootstrapAA::precomputeAll() { m_impl->analysis->precomputeAll(); }

bool BootstrapAA::mayAlias(const llvm::Value &lhs, const llvm::Value &rhs,
                           const llvm::Instruction &site,
                           const CallContext &context) {
  return m_impl->analysis->mayAlias(
      m_impl->queryValue(lhs), m_impl->queryValue(rhs), m_impl->querySite(site),
      m_impl->context(context));
}

const llvm::Value *BootstrapAA::allocationSite(Id object) const {
  return m_impl->allocations.at(object);
}

Id BootstrapAA::objectId(const llvm::Value &allocation) const {
  auto it = m_impl->objects.find(&allocation);
  return it == m_impl->objects.end() ? INVALID : it->second;
}

Id BootstrapAA::fieldObjectId(const llvm::Value &allocation,
                              std::uint64_t byte_offset) const {
  auto it = m_impl->field_objects.find({&allocation, byte_offset});
  return it == m_impl->field_objects.end() ? INVALID : it->second;
}

const Object &BootstrapAA::objectInfo(Id object) const {
  return m_impl->program.objects.at(object);
}

const Statistics &BootstrapAA::statistics() const {
  return m_impl->analysis->statistics();
}

const SteensgaardHierarchy &BootstrapAA::hierarchy() const {
  return m_impl->analysis->hierarchy();
}

} // namespace bootstrap
} // namespace lotus
