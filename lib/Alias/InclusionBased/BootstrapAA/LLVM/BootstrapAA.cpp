#include "Alias/InclusionBased/BootstrapAA/BootstrapAA.h"
#include "Alias/Infrastructure/Spec/AliasSpecManager.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <numeric>
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
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus {
namespace bootstrap {
namespace {
std::vector<std::string> discoverSpecFiles() {
  std::vector<std::string> out;
  for (const std::string &path : lotus::alias::getDefaultSpecFiles()) {
    if (llvm::sys::fs::exists(path))
      out.push_back(path);
  }
  if (!out.empty())
    return out;
  llvm::SmallString<256> cwd;
  if (!llvm::sys::fs::current_path(cwd)) {
    llvm::SmallString<256> dir = cwd;
    for (int up = 0; up < 4 && out.empty(); ++up) {
      llvm::SmallString<256> cfg = dir;
      llvm::sys::path::append(cfg, "config");
      for (const char *name : {"ptr.spec", "modref.spec"}) {
        llvm::SmallString<256> full = cfg;
        llvm::sys::path::append(full, name);
        if (llvm::sys::fs::exists(full))
          out.push_back(full.str().str());
      }
      llvm::sys::path::remove_filename(dir);
    }
  }
  return out;
}

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
  return name == "strlen" || name == "strnlen" || name == "wcslen" ||
         name == "strcmp" || name == "strncmp" || name == "wcscmp" ||
         name == "wcsncmp" || name == "strcasecmp" || name == "strncasecmp" ||
         name == "strcoll" || name == "memcmp" || name == "bcmp" ||
         name == "strchr" || name == "strrchr" || name == "wcschr" ||
         name == "strstr" || name == "strpbrk" || name == "memchr" ||
         name == "rawmemchr" || name == "strspn" || name == "strcspn" ||
         name == "index" || name == "rindex" || name == "atoi" ||
         name == "atol" || name == "atoll" || name == "atof" || name == "abs" ||
         name == "labs" || name == "llabs" || name == "exit" ||
         name == "_exit" || name == "abort" || name == "__assert_fail" ||
         name == "__cxa_pure_virtual" || name == "puts" || name == "fputs" ||
         name == "putchar" || name == "getchar" || name == "fwrite" ||
         name == "fclose" || name == "fflush" || name == "fseek" ||
         name == "fseeko" || name == "ftell" || name == "ftello" ||
         name == "rewind" || name == "feof" || name == "ferror" ||
         name == "clearerr" || name == "fileno" || name == "access" ||
         name == "open" || name == "close" || name == "unlink" ||
         name == "remove" || name == "rename" || name == "mkdir" ||
         name == "rmdir" || name == "chmod" || name == "chown" ||
         name == "chdir" || name == "perror" || name == "isatty" ||
         name == "printf" || name == "fprintf" || name == "vprintf" ||
         name == "vfprintf";
}

bool isSideEffectFreeIntrinsic(const llvm::Function *function) {
  if (!function || !function->isIntrinsic())
    return false;
  const llvm::StringRef name = function->getName();
  return name.startswith("llvm.dbg.") || name.startswith("llvm.lifetime.") ||
         name == "llvm.assume" || name.startswith("llvm.expect") ||
         name.startswith("llvm.invariant.start") ||
         name.startswith("llvm.invariant.end") ||
         name.startswith("llvm.prefetch") ||
         name.startswith("llvm.objectsize") || name.startswith("llvm.bswap") ||
         name.startswith("llvm.ctlz") || name.startswith("llvm.cttz") ||
         name.startswith("llvm.ctpop") || name.startswith("llvm.bitreverse") ||
         name.startswith("llvm.fshl") || name.startswith("llvm.fshr") ||
         name.startswith("llvm.sadd.") || name.startswith("llvm.uadd.") ||
         name.startswith("llvm.ssub.") || name.startswith("llvm.usub.") ||
         name.startswith("llvm.smul.") || name.startswith("llvm.umul.") ||
         name == "llvm.trap" || name.startswith("llvm.var.annotation") ||
         name.startswith("llvm.is.constant") ||
         name.startswith("llvm.annotation");
}

bool isPassThroughPtrIntrinsic(const llvm::Function *function) {
  if (!function || !function->isIntrinsic())
    return false;
  const llvm::StringRef name = function->getName();
  return name.startswith("llvm.launder.invariant.group") ||
         name.startswith("llvm.strip.invariant.group") ||
         name.startswith("llvm.ptr.annotation");
}

bool writesMemory(const llvm::CallBase &call,
                  const lotus::alias::AliasSpecManager &spec_mgr) {
  const llvm::Function *callee = directCallee(call);
  if (knownReadOnly(callee) || isSideEffectFreeIntrinsic(callee) ||
      isPassThroughPtrIntrinsic(callee) || call.doesNotAccessMemory() ||
      call.onlyReadsMemory())
    return false;
  if (callee && callee->isDeclaration()) {
    if (spec_mgr.isExitFunction(callee) || spec_mgr.isDeallocator(callee))
      return false;
    bool has_ptr_arg = false;
    for (const llvm::Use &arg : call.args())
      if (arg->getType()->isPointerTy())
        has_ptr_arg = true;
    if (!has_ptr_arg && spec_mgr.isNoEffect(callee))
      return false;
  }
  return true;
}

enum class AllocationKind { None, Fresh, FreshZeroed, Reallocate, OutParam };
struct AllocationModel {
  AllocationKind kind = AllocationKind::None;
  bool mayBeNull = false;
  int outArgIndex = -1;
};

bool isThrowingCppNew(llvm::StringRef name) {
  if (!(name.startswith("_Znw") || name.startswith("_Zna")))
    return false;
  if (name == "_ZnwmPv" || name == "_ZnamPv" || name == "_ZnwjPv" ||
      name == "_ZnajPv")
    return false;
  return !name.contains("nothrow");
}

AllocationModel allocationModel(const llvm::CallBase &call,
                                const lotus::alias::AliasSpecManager &spec_mgr) {
  const llvm::Function *function = directCallee(call);
  if (!function || !function->isDeclaration())
    return {};
  const llvm::StringRef name = function->getName();
  // C++ placement new returns its pointer operand without allocating.
  if (name == "_ZnwmPv" || name == "_ZnamPv" || name == "_ZnwjPv" ||
      name == "_ZnajPv")
    return {};
  auto integerArguments = [&](unsigned count) {
    if (call.arg_size() != count)
      return false;
    for (unsigned index = 0; index < count; ++index)
      if (!call.getArgOperand(index)->getType()->isIntegerTy())
        return false;
    return true;
  };
  if (call.getType()->isPointerTy()) {
    if ((name == "malloc" || name == "valloc" || name == "pvalloc" ||
         name == "kmalloc") &&
        integerArguments(1))
      return {AllocationKind::Fresh, true, -1};
    if (name == "calloc" && integerArguments(2))
      return {AllocationKind::FreshZeroed, true, -1};
    if ((name == "aligned_alloc" || name == "memalign") && integerArguments(2))
      return {AllocationKind::Fresh, true, -1};
    if ((name == "realloc" || name == "reallocf") && call.arg_size() == 2 &&
        call.getArgOperand(0)->getType()->isPointerTy() &&
        call.getArgOperand(1)->getType()->isIntegerTy())
      return {AllocationKind::Reallocate, true, -1};
    if (name == "strdup" && call.arg_size() == 1 &&
        call.getArgOperand(0)->getType()->isPointerTy())
      return {AllocationKind::Fresh, true, -1};
    if (name == "strndup" && call.arg_size() == 2 &&
        call.getArgOperand(0)->getType()->isPointerTy() &&
        call.getArgOperand(1)->getType()->isIntegerTy())
      return {AllocationKind::Fresh, true, -1};
    if ((name == "_Znwm" || name == "_Znam" || name == "_Znwj" ||
         name == "_Znaj") &&
        integerArguments(1))
      return {AllocationKind::Fresh, false, -1};
    if (name.startswith("_Znw") || name.startswith("_Zna"))
      return {AllocationKind::Fresh, !isThrowingCppNew(name), -1};
  }
  if (name == "posix_memalign" && call.arg_size() == 3 &&
      call.getArgOperand(0)->getType()->isPointerTy())
    return {AllocationKind::OutParam, false, 0};

  if (auto info = spec_mgr.getAllocatorInfo(function)) {
    if (!info->returnsPointer && info->ptrOutArgIndex >= 0) {
      const unsigned idx = static_cast<unsigned>(info->ptrOutArgIndex);
      if (idx < call.arg_size() &&
          call.getArgOperand(idx)->getType()->isPointerTy())
        return {AllocationKind::OutParam, false, info->ptrOutArgIndex};
    } else if (info->returnsPointer && call.getType()->isPointerTy()) {
      if (spec_mgr.getCategory(function) ==
              lotus::alias::FunctionCategory::Reallocator &&
          call.arg_size() >= 1 &&
          call.getArgOperand(0)->getType()->isPointerTy())
        return {AllocationKind::Reallocate, true, -1};
      if (info->initializesToZero)
        return {AllocationKind::FreshZeroed, true, -1};
      return {AllocationKind::Fresh, !isThrowingCppNew(name), -1};
    }
  }
  return {};
}

bool deallocationCall(const llvm::CallBase &call,
                      const lotus::alias::AliasSpecManager &spec_mgr) {
  const llvm::Function *function = directCallee(call);
  if (!function || !function->isDeclaration() ||
      call.getType()->isPointerTy() || call.arg_size() == 0 ||
      !call.getArgOperand(0)->getType()->isPointerTy())
    return false;
  const llvm::StringRef name = function->getName();
  if (call.arg_size() == 1 &&
      (name == "free" || name == "cfree" || name == "_ZdlPv" ||
       name == "_ZdaPv"))
    return true;
  if (call.arg_size() == 2 && call.getArgOperand(1)->getType()->isIntegerTy() &&
      (name == "_ZdlPvm" || name == "_ZdaPvm" || name == "_ZdlPvj" ||
       name == "_ZdaPvj"))
    return true;
  return spec_mgr.isDeallocator(function);
}

bool returnsInteriorPointer(const llvm::Function *function) {
  if (!knownReadOnly(function) || !function->getReturnType()->isPointerTy())
    return false;
  const llvm::StringRef name = function->getName();
  return name == "strchr" || name == "strrchr" || name == "wcschr" ||
         name == "strstr" || name == "strpbrk" || name == "memchr" ||
         name == "rawmemchr" || name == "index" || name == "rindex";
}

bool isPlacementNewCall(const llvm::CallBase &call) {
  const llvm::Function *function = directCallee(call);
  if (!function || !function->isDeclaration() ||
      !call.getType()->isPointerTy() || call.arg_size() != 2 ||
      !call.getArgOperand(1)->getType()->isPointerTy())
    return false;
  const llvm::StringRef name = function->getName();
  return name == "_ZnwmPv" || name == "_ZnamPv" || name == "_ZnwjPv" ||
         name == "_ZnajPv";
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

std::uint64_t foldOffsetWithRegions(std::uint64_t off,
                                    const std::vector<ArrayRegion> &regions) {
  for (const ArrayRegion &ar : regions) {
    if (ar.stride > 0 && off >= ar.begin && off < ar.end)
      off = ar.begin + ((off - ar.begin) % ar.stride);
  }
  return off;
}

bool flattenAggregateFields(llvm::Type *ty, const llvm::DataLayout &layout,
                            std::vector<FieldSpec> &all_fields,
                            std::vector<FieldSpec> &ptr_fields,
                            std::vector<ArrayRegion> &regions) {
  all_fields.clear();
  ptr_fields.clear();
  regions.clear();
  if (!ty || ty->isPointerTy() || !ty->isSized() ||
      !(ty->isStructTy() || ty->isArrayTy()))
    return false;
  std::vector<FieldSpec> raw;
  if (!flattenType(ty, layout, 0, false, raw, regions) || raw.empty() ||
      raw.size() > MAX_FLATTENED_FIELDS)
    return false;
  std::map<std::uint64_t, FieldSpec> by_off;
  for (const FieldSpec &fs : raw) {
    auto it = by_off.find(fs.offset);
    if (it == by_off.end() || (!it->second.is_pointer && fs.is_pointer))
      by_off[fs.offset] = fs;
  }
  for (const auto &[off, fs] : by_off) {
    (void)off;
    all_fields.push_back(fs);
    if (fs.is_pointer)
      ptr_fields.push_back(fs);
  }
  return !ptr_fields.empty();
}

bool computeExtractInsertOffset(llvm::Type *agg_ty,
                                llvm::ArrayRef<unsigned> indices,
                                const llvm::DataLayout &layout,
                                std::uint64_t &byte_off, llvm::Type *&leaf_ty,
                                bool &inside_array) {
  byte_off = 0;
  inside_array = false;
  llvm::Type *cur = agg_ty;
  for (unsigned idx : indices) {
    if (!cur || !cur->isSized())
      return false;
    if (auto *st = llvm::dyn_cast<llvm::StructType>(cur)) {
      if (idx >= st->getNumElements())
        return false;
      const llvm::StructLayout *sl = layout.getStructLayout(st);
      byte_off += sl->getElementOffset(idx);
      cur = st->getElementType(idx);
    } else if (auto *at = llvm::dyn_cast<llvm::ArrayType>(cur)) {
      if (idx >= at->getNumElements())
        return false;
      llvm::Type *elem = at->getElementType();
      if (!elem->isSized())
        return false;
      inside_array = true;
      byte_off += static_cast<std::uint64_t>(idx) * layout.getTypeAllocSize(elem);
      cur = elem;
    } else {
      return false;
    }
  }
  leaf_ty = cur;
  return cur != nullptr;
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

// Inspect users of a heap allocation (transitively through Freeze, zero-GEP,
// and single-source Select/PHI with null) to see if it is exclusively cast to a
// single sized StructType (aside from null checks, free, memset, and
// debug/lifetime intrinsics). Escaping raw void* uses inhibit flattening to
// stay sound against cross-function type punning.
llvm::StructType *
inferHeapStructType(const llvm::CallBase &call,
                    const lotus::alias::AliasSpecManager &spec_mgr) {
  llvm::StructType *inferred = nullptr;
  std::deque<const llvm::Value *> work{&call};
  std::set<const llvm::Value *> visited{&call};
  while (!work.empty()) {
    const llvm::Value *cur = work.front();
    work.pop_front();
    for (const llvm::User *user : cur->users()) {
      if (const auto *bc = llvm::dyn_cast<llvm::BitCastInst>(user)) {
        auto *ptr_ty = llvm::dyn_cast<llvm::PointerType>(bc->getType());
        if (!ptr_ty)
          return nullptr;
        auto *st =
            llvm::dyn_cast<llvm::StructType>(ptr_ty->getPointerElementType());
        if (!st || !st->isSized())
          return nullptr;
        if (inferred && inferred != st)
          return nullptr;
        inferred = st;
        continue;
      }
      if (const auto *fr = llvm::dyn_cast<llvm::FreezeInst>(user)) {
        if (visited.insert(fr).second)
          work.push_back(fr);
        continue;
      }
      if (const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user)) {
        if (gep->hasAllZeroIndices()) {
          if (visited.insert(gep).second)
            work.push_back(gep);
          continue;
        }
        return nullptr;
      }
      if (const auto *sel = llvm::dyn_cast<llvm::SelectInst>(user)) {
        const llvm::Value *other =
            sel->getTrueValue() == cur ? sel->getFalseValue()
                                       : sel->getTrueValue();
        if (llvm::isa<llvm::ConstantPointerNull>(other)) {
          if (visited.insert(sel).second)
            work.push_back(sel);
          continue;
        }
        return nullptr;
      }
      if (llvm::isa<llvm::ICmpInst>(user))
        continue;
      if (const auto *cb = llvm::dyn_cast<llvm::CallBase>(user)) {
        if (deallocationCall(*cb, spec_mgr) || llvm::isa<llvm::MemSetInst>(cb))
          continue;
        const llvm::Function *callee = directCallee(*cb);
        if (isSideEffectFreeIntrinsic(callee))
          continue;
      }
      return nullptr;
    }
  }
  return inferred;
}

} // namespace

struct BootstrapAA::Impl {
  const llvm::Module &module;
  const llvm::Function *entry_fn = nullptr;
  lotus::alias::AliasSpecManager spec_mgr;
  Program program;
  std::unique_ptr<Analysis> analysis;
  std::unordered_map<const llvm::Value *, Id> values;
  std::vector<const llvm::Value *> value_to_llvm;
  std::unordered_map<const llvm::Value *, Id> objects;
  std::map<std::pair<const llvm::Value *, std::uint64_t>, Id> field_objects;
  std::unordered_map<const llvm::Function *, Id> functions;
  std::vector<const llvm::Function *> func_to_llvm;
  std::unordered_map<const llvm::BasicBlock *, Id> blocks;
  std::unordered_map<const llvm::Instruction *, Id> sites;
  std::vector<const llvm::Instruction *> site_to_llvm;
  std::vector<const llvm::Value *> allocations{nullptr, nullptr};
  std::set<const llvm::Constant *> resolving;
  std::vector<bool> recursive;
  bool null_may_be_valid = false;
  Id null_value = INVALID;

  // Parsed `llvm.global_ctors` startup constructors (priority-sorted) and
  // fallback flag when any ctor is external/unparseable or entry is recursive.
  std::vector<const llvm::Function *> parsed_ctors;
  bool module_has_external_ctors = false;

  // Aggregate SSA leaf value decomposition (`extractvalue` / `insertvalue` /
  // aggregate `load` / `store` / `phi` / `select` / `call` / `ret`).
  std::map<std::pair<const llvm::Value *, std::uint64_t>, Id> agg_values;
  std::unordered_map<const llvm::Function *, Id> agg_ret_slot_base;
  std::unordered_map<const llvm::Argument *, Id> agg_arg_slot_base;
  std::unordered_map<const llvm::Function *, Id> static_return_objects;

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
  struct ParamCondInfo {
    Id cid = INVALID;
    unsigned arg_no = 0;
    bool is_bool_arg = false;
    unsigned pred = 0;
    bool arg_is_lhs = true;
    const llvm::Constant *const_rhs = nullptr;
  };
  std::map<PredKey, Id> icmp_conditions;
  std::unordered_map<const llvm::Value *, Id> value_conditions;
  std::unordered_map<const llvm::Instruction *, std::vector<Id>>
      inst_kills_conditions;
  std::unordered_map<const llvm::Function *, std::vector<ParamCondInfo>>
      param_conds;
  std::unordered_map<const llvm::CallBase *,
                     std::vector<Instruction::CondSubst>>
      call_cond_substs;
  std::unordered_map<const llvm::GlobalVariable *,
                     std::set<const llvm::Function *>>
      global_mutating_funcs;

  void parseGlobalCtors(const llvm::Function *entry) {
    const llvm::GlobalVariable *ctors =
        module.getNamedGlobal("llvm.global_ctors");
    if (!ctors)
      return;
    if (!ctors->hasInitializer()) {
      module_has_external_ctors = true;
      return;
    }
    if (llvm::isa<llvm::ConstantAggregateZero>(ctors->getInitializer()))
      return;
    const auto *arr =
        llvm::dyn_cast<llvm::ConstantArray>(ctors->getInitializer());
    if (!arr) {
      module_has_external_ctors = true;
      return;
    }
    // Prepending ctor calls to entry block 0 is exact only if entry itself is
    // not invoked from inside the module (no re-execution of ctors).
    if (!entry->use_empty()) {
      module_has_external_ctors = true;
      return;
    }
    struct Entry {
      std::uint64_t priority = 65535;
      unsigned order = 0;
      const llvm::Function *fn = nullptr;
    };
    std::vector<Entry> items;
    for (unsigned idx = 0; idx < arr->getNumOperands(); ++idx) {
      const auto *cs = llvm::dyn_cast<llvm::ConstantStruct>(arr->getOperand(idx));
      if (!cs || cs->getNumOperands() < 2) {
        module_has_external_ctors = true;
        return;
      }
      const auto *prio = llvm::dyn_cast<llvm::ConstantInt>(cs->getOperand(0));
      const llvm::Constant *fn_c = cs->getOperand(1);
      if (!prio || !fn_c) {
        module_has_external_ctors = true;
        return;
      }
      if (llvm::isa<llvm::ConstantPointerNull>(fn_c))
        continue;
      const auto *fn =
          llvm::dyn_cast<llvm::Function>(fn_c->stripPointerCasts());
      if (!fn || fn->isDeclaration() || !fn->arg_empty()) {
        module_has_external_ctors = true;
        return;
      }
      items.push_back({prio->getZExtValue(), idx, fn});
    }
    std::stable_sort(items.begin(), items.end(),
                     [](const Entry &a, const Entry &b) {
                       return std::tie(a.priority, a.order) <
                              std::tie(b.priority, b.order);
                     });
    for (const Entry &e : items)
      parsed_ctors.push_back(e.fn);
  }

  Id createSyntheticSlot(llvm::Type *ty, const std::string &base_name,
                         const llvm::DataLayout &layout) {
    std::vector<FieldSpec> all_f, ptr_f;
    std::vector<ArrayRegion> regions;
    if (!flattenAggregateFields(ty, layout, all_f, ptr_f, regions))
      return INVALID;
    std::map<std::uint64_t, FieldSpec> by_off;
    for (const FieldSpec &fs : all_f)
      by_off[fs.offset] = fs;
    if (!by_off.count(0))
      by_off[0] = {0, 1, 0, false, false, 0};
    const FieldSpec &f0 = by_off[0];
    Object base_obj;
    base_obj.name = base_name;
    base_obj.kind = ObjectKind::Global;
    base_obj.singleton = f0.is_pointer && !f0.inside_array;
    base_obj.bytes = f0.ptr_bytes;
    base_obj.size_bytes = layout.getTypeAllocSize(ty);
    base_obj.initial = PointsToSet{};
    base_obj.field_sensitive = true;
    base_obj.array_regions = std::move(regions);
    const Id base_id = program.addObject(std::move(base_obj));
    allocations.push_back(nullptr);
    for (const auto &[off, fs] : by_off) {
      if (off == 0)
        continue;
      Object fobj;
      fobj.name = base_name + "+" + std::to_string(off);
      fobj.kind = ObjectKind::Global;
      fobj.singleton = fs.is_pointer && !fs.inside_array;
      fobj.bytes = fs.ptr_bytes;
      fobj.size_bytes = fs.size_bytes;
      fobj.initial = PointsToSet{};
      program.addFieldObject(base_id, off, std::move(fobj));
      allocations.push_back(nullptr);
    }
    return base_id;
  }

  Id recordSite(const llvm::Instruction &inst, Id fn_id, Id bb_id,
                Instruction lowered) {
    const Id sid = program.append(fn_id, bb_id, std::move(lowered));
    sites[&inst] = sid;
    if (site_to_llvm.size() <= sid)
      site_to_llvm.resize(sid + 1, nullptr);
    site_to_llvm[sid] = &inst;
    return sid;
  }

  Impl(const llvm::Module &m, const llvm::Function *entry, Options options)
      : module(m), spec_mgr(discoverSpecFiles()) {
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
    entry_fn = entry;
    spec_mgr.initialize(module);

    for (const llvm::Function &function : module) {
      null_may_be_valid |=
          function.hasFnAttribute(llvm::Attribute::NullPointerIsValid);
      Id id = program.addFunction(function.getName().str(),
                                  function.isDeclaration());
      functions.emplace(&function, id);
      if (func_to_llvm.size() <= id)
        func_to_llvm.resize(id + 1, nullptr);
      func_to_llvm[id] = &function;
      bool fn_writes = !knownReadOnly(&function) &&
                       !isSideEffectFreeIntrinsic(&function) &&
                       !isPassThroughPtrIntrinsic(&function) &&
                       !function.doesNotAccessMemory() &&
                       !function.onlyReadsMemory();
      if (function.isDeclaration()) {
        if (spec_mgr.isExitFunction(&function) ||
            spec_mgr.isDeallocator(&function))
          fn_writes = false;
      }
      program.functions[id].writes_memory = fn_writes;
      Object object;
      object.name = nameOf(function);
      object.kind = ObjectKind::Function;
      object.function = id;
      addObject(function, std::move(object));
    }
    program.entry = functions.at(entry);
    parseGlobalCtors(entry);
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
      if (!function.isDeclaration()) {
        std::vector<FieldSpec> all_f, ptr_f;
        std::vector<ArrayRegion> regs;
        if (flattenAggregateFields(function.getReturnType(), layout, all_f,
                                   ptr_f, regs)) {
          Id slot = createSyntheticSlot(
              function.getReturnType(),
              "<agg-ret:" + function.getName().str() + ">", layout);
          if (slot != INVALID)
            agg_ret_slot_base[&function] = slot;
        }
      }
      for (const llvm::Argument &argument : function.args()) {
        program.functions[f].parameters.push_back(valueId(argument));
        if (!function.isDeclaration() && !function.hasAddressTaken() &&
            !argument.hasByValAttr()) {
          std::vector<FieldSpec> all_f, ptr_f;
          std::vector<ArrayRegion> regs;
          if (flattenAggregateFields(argument.getType(), layout, all_f, ptr_f,
                                     regs)) {
            Id slot = createSyntheticSlot(
                argument.getType(),
                "<agg-arg:" + function.getName().str() + ":" +
                    nameOf(argument) + ">",
                layout);
            if (slot != INVALID)
              agg_arg_slot_base[&argument] = slot;
          }
        }
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
                  alloca->getType()->getPointerAddressSpace(), f);
            } else {
              // Defer recursive-frame singleton demotion to Analysis::Impl's
              // resolved call graph so unrelated indirect calls do not disable
              // strong updates on stack allocas in non-recursive functions.
              createStorageObjects(
                  instruction, alloc_ty, ObjectKind::Stack, is_static_one,
                  /*fresh_empty_init=*/true,
                  function.getName().str() + ":" + nameOf(instruction), layout,
                  alloca->getType()->getPointerAddressSpace(), f);
            }
          } else if (const auto *call =
                         llvm::dyn_cast<llvm::CallBase>(&instruction)) {
            const AllocationModel am = allocationModel(*call, spec_mgr);
            if (am.kind != AllocationKind::None) {
              const bool fresh_empty = (am.kind == AllocationKind::Fresh ||
                                        am.kind == AllocationKind::FreshZeroed ||
                                        am.kind == AllocationKind::OutParam);
              const bool zero_init = (am.kind == AllocationKind::FreshZeroed);
              const PointsToSet zero_ptr = null_may_be_valid
                                               ? PointsToSet::top()
                                               : PointsToSet(NULL_OBJECT);
              llvm::StructType *st = (am.kind == AllocationKind::OutParam)
                                         ? nullptr
                                         : inferHeapStructType(*call, spec_mgr);
              if (st) {
                createStorageObjects(
                    instruction, st, ObjectKind::Heap,
                    /*allow_singleton=*/false, fresh_empty,
                    function.getName().str() + ":" + nameOf(instruction),
                    layout, call->getType()->getPointerAddressSpace());
                if (zero_init) {
                  const Id base = objects.at(&instruction);
                  if (program.objects[base].field_sensitive) {
                    for (auto [off, fid] : program.fields_of_base[base])
                      if (program.objects[fid].bytes > 0)
                        program.objects[fid].initial = zero_ptr;
                  } else if (program.objects[base].bytes > 0) {
                    program.objects[base].initial = zero_ptr;
                  }
                }
              } else {
                Object object;
                object.name =
                    function.getName().str() + ":" + nameOf(instruction);
                object.kind = ObjectKind::Heap;
                if (zero_init)
                  object.initial = zero_ptr;
                else if (fresh_empty)
                  object.initial = PointsToSet{};
                const unsigned as =
                    call->getType()->isPointerTy()
                        ? call->getType()->getPointerAddressSpace()
                        : 0;
                object.bytes = layout.getPointerSize(as);
                addObject(instruction, std::move(object));
              }
            }
          }
        }
      }
    }

    // Initialize globals (both scalar pointer globals and flattened struct /
    // array global initializers!). When `llvm.global_ctors` contains only
    // defined module functions, their startup calls are prepended to `entry` so
    // static initializers remain exact rather than degrading to Top.
    for (const llvm::GlobalVariable &global : module.globals()) {
      valueId(global);
      if (!global.hasInitializer() || global.isExternallyInitialized() ||
          module_has_external_ctors)
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

    // Pre-assign boolean condition IDs for conditional branches, switches, and
    // pointer selects (for optional ROBDD path sensitivity, Kahlon §3 p. 258).
    for (const llvm::Function &function : module) {
      if (function.isDeclaration())
        continue;
      for (const llvm::BasicBlock &block : function) {
        for (const llvm::Instruction &inst : block) {
          if (const auto *sel = llvm::dyn_cast<llvm::SelectInst>(&inst)) {
            if (sel->getType()->isPointerTy() ||
                sel->getType()->isStructTy() || sel->getType()->isArrayTy())
              (void)resolveBranchCondition(function, *sel->getCondition(), sel);
          }
        }
        const llvm::Instruction *term = block.getTerminator();
        if (const auto *br = llvm::dyn_cast<llvm::BranchInst>(term)) {
          if (br->isConditional())
            (void)resolveBranchCondition(function, *br->getCondition(), br);
        } else if (const auto *sw = llvm::dyn_cast<llvm::SwitchInst>(term)) {
          if (sw->getNumCases() <= 16) {
            for (const auto &c : sw->cases())
              (void)resolveSwitchCaseCondition(function, *sw->getCondition(),
                                               c.getCaseValue(), sw);
          }
        }
      }
    }

    // Populate call-site formal-to-actual condition substitutions for general
    // interprocedural ROBDD path sensitivity across multi-call-site & recursive
    // functions.
    populateCallCondSubsts();

    for (const llvm::Function &function : module) {
      valueId(function);
      const Id fn_id = functions.at(&function);
      if (!function.isDeclaration()) {
        if (&function == entry_fn) {
          // Run `llvm.global_ctors` in priority order at startup before `entry`
          // executes, preserving `context = {}` for `entry`!
          for (const llvm::Function *ctor_fn : parsed_ctors) {
            Instruction ctor_call;
            ctor_call.opcode = Opcode::Call;
            ctor_call.callee = functions.at(ctor_fn);
            ctor_call.writes_memory = true;
            program.append(fn_id, 0, std::move(ctor_call));
          }
        }
        for (const llvm::Argument &argument : function.args()) {
          if (argument.hasByValAttr()) {
            Instruction copy;
            copy.opcode = Opcode::Allocate;
            copy.object = objects.at(&argument);
            copy.result = valueId(argument);
            program.append(fn_id, 0, copy);
            continue;
          }
          auto asit = agg_arg_slot_base.find(&argument);
          if (asit != agg_arg_slot_base.end()) {
            std::vector<FieldSpec> all_f, ptr_f;
            std::vector<ArrayRegion> regs;
            if (flattenAggregateFields(argument.getType(), layout, all_f, ptr_f,
                                       regs)) {
              for (const FieldSpec &fs : ptr_f) {
                const Id slot_obj =
                    program.resolveField(asit->second,
                                         static_cast<std::int64_t>(fs.offset),
                                         true)
                        .objects()
                        .begin() !=
                            program
                                .resolveField(
                                    asit->second,
                                    static_cast<std::int64_t>(fs.offset), true)
                                .objects()
                                .end()
                        ? *program
                               .resolveField(
                                   asit->second,
                                   static_cast<std::int64_t>(fs.offset), true)
                               .begin()
                        : asit->second;
                const Id addr_val = program.addConstant(
                    PointsToSet(slot_obj), "<agg-arg-addr>");
                Instruction ld;
                ld.opcode = Opcode::Load;
                ld.result = aggValueId(argument, fs.offset, fn_id);
                ld.operands = {addr_val};
                ld.read_bytes = fs.ptr_bytes;
                program.append(fn_id, 0, std::move(ld));
              }
            }
          }
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
      const Id fn_id = functions.at(&function);
      for (const llvm::BasicBlock &block : function) {
        const llvm::Instruction *term = block.getTerminator();
        const auto *br = llvm::dyn_cast<llvm::BranchInst>(term);
        const auto *sw = llvm::dyn_cast<llvm::SwitchInst>(term);
        Id br_cond_var = INVALID;
        bool br_cond_pol = true;
        if (br && br->isConditional())
          std::tie(br_cond_var, br_cond_pol) =
              resolveBranchCondition(function, *br->getCondition(), br);

        std::vector<Id> sw_case_conds;
        if (sw && sw->getNumCases() <= 16) {
          sw_case_conds.reserve(sw->getNumCases());
          for (const auto &c : sw->cases())
            sw_case_conds.push_back(resolveSwitchCaseCondition(
                function, *sw->getCondition(), c.getCaseValue(), sw));
        }

        const unsigned num_succ = term->getNumSuccessors();
        const Id term_site =
            sites.count(term) ? sites.at(term) : INVALID;
        for (unsigned sidx = 0; sidx < num_succ; ++sidx) {
          const llvm::BasicBlock *successor = term->getSuccessor(sidx);
          Edge edge;
          edge.target = blocks.at(successor);
          if (br && br->isConditional()) {
            if (br_cond_var != INVALID) {
              edge.cond_var = br_cond_var;
              edge.cond_polarity = (sidx == 0) ? br_cond_pol : !br_cond_pol;
            }
            extractEdgePtrGuards(*br->getCondition(), sidx == 0, term_site,
                                 edge.ptr_guards);
          } else if (sw && sw->getNumCases() <= 16 && !sw_case_conds.empty()) {
            if (sidx == 0) {
              // Default edge: none of the explicit cases matched.
              Id def_cond = INVALID;
              for (Id cc : sw_case_conds) {
                if (cc == INVALID) {
                  def_cond = INVALID;
                  break;
                }
                Id neg = program.addCondNot(cc);
                def_cond = (def_cond == INVALID)
                               ? neg
                               : program.addCondAnd(def_cond, neg);
              }
              edge.cond_var = def_cond;
              edge.cond_polarity = true;
            } else {
              const std::size_t cidx = sidx - 1;
              if (cidx < sw_case_conds.size() &&
                  sw_case_conds[cidx] != INVALID) {
                Id case_cond = sw_case_conds[cidx];
                // For compact switches (<= 8 cases), encode pairwise mutual
                // exclusion so `case 0` directly refutes `x == 1` in ROBDDs.
                if (sw_case_conds.size() <= 8) {
                  for (std::size_t j = 0; j < sw_case_conds.size(); ++j) {
                    if (j != cidx && sw_case_conds[j] != INVALID)
                      case_cond = program.addCondAnd(
                          case_cond, program.addCondNot(sw_case_conds[j]));
                  }
                }
                edge.cond_var = case_cond;
                edge.cond_polarity = true;
              }
            }
          }
          for (const llvm::Instruction &instruction : *successor) {
            const auto *phi = llvm::dyn_cast<llvm::PHINode>(&instruction);
            if (!phi)
              break;
            if (phi->getType()->isPointerTy()) {
              edge.phi.emplace_back(
                  valueId(*phi),
                  valueId(*phi->getIncomingValueForBlock(&block)));
            } else {
              std::vector<FieldSpec> all_f, ptr_f;
              std::vector<ArrayRegion> regs;
              if (flattenAggregateFields(phi->getType(), layout, all_f, ptr_f,
                                         regs)) {
                const llvm::Value *inc = phi->getIncomingValueForBlock(&block);
                for (const FieldSpec &fs : ptr_f) {
                  edge.phi.emplace_back(aggValueId(*phi, fs.offset, fn_id),
                                        aggValueId(*inc, fs.offset, fn_id));
                }
              }
            }
          }
          program.functions[fn_id]
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
                            const llvm::DataLayout &layout, unsigned addrspace,
                            Id owner_function = INVALID) {
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
      obj.function = owner_function;
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
        base_obj.function = owner_function;
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
          fobj.function = owner_function;
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
      obj.function = owner_function;
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
    obj.function = owner_function;
    if (ty->isSized())
      obj.size_bytes = layout.getTypeAllocSize(ty);
    const Id id = addObject(allocation, std::move(obj));
    field_objects[{&allocation, 0}] = id;
  }

  void joinFoldedLeaf(Id base, std::uint64_t offset, const PointsToSet &pts) {
    const std::uint64_t folded = program.foldOffset(base, offset);
    const PointsToSet target_field = program.resolveField(base, folded, true);
    if (target_field.isTop()) {
      if (base < program.fields_of_base.size()) {
        for (auto [foff, fid] : program.fields_of_base[base])
          if (fid >= 2 && fid < program.objects.size() &&
              program.objects[fid].bytes > 0)
            program.objects[fid].initial.join(pts);
      }
      return;
    }
    for (Id fid : target_field.objects())
      if (fid >= 2 && fid < program.objects.size() &&
          program.objects[fid].bytes > 0)
        program.objects[fid].initial.join(pts);
  }

  void fillAggregateType(Id base, llvm::Type *ty, std::uint64_t offset,
                         const PointsToSet &leaf_pts,
                         const llvm::DataLayout &layout) {
    if (!ty)
      return;
    if (ty->isPointerTy()) {
      joinFoldedLeaf(base, offset, leaf_pts);
      return;
    }
    if (auto *st = llvm::dyn_cast<llvm::StructType>(ty)) {
      if (!st->isSized())
        return;
      const llvm::StructLayout *sl = layout.getStructLayout(st);
      for (unsigned i = 0; i < st->getNumElements(); ++i)
        fillAggregateType(base, st->getElementType(i),
                          offset + sl->getElementOffset(i), leaf_pts, layout);
      return;
    }
    if (auto *at = llvm::dyn_cast<llvm::ArrayType>(ty)) {
      if (at->getNumElements() == 0)
        return;
      llvm::Type *elem = at->getElementType();
      if (!elem->isSized() || layout.getTypeAllocSize(elem) == 0)
        return;
      // Every element of `at` folds identically onto element 0 under
      // `program.foldOffset`.
      fillAggregateType(base, elem, offset, leaf_pts, layout);
      return;
    }
  }

  void populateConstantAggregate(Id base, const llvm::Constant *c,
                                 std::uint64_t offset,
                                 const llvm::DataLayout &layout) {
    if (!c)
      return;
    llvm::Type *ty = c->getType();
    if (ty->isPointerTy()) {
      joinFoldedLeaf(base, offset, constantPoints(*c));
      return;
    }
    if (llvm::isa<llvm::ConstantAggregateZero>(c)) {
      const PointsToSet null_or_top =
          null_may_be_valid ? PointsToSet::top() : PointsToSet(NULL_OBJECT);
      fillAggregateType(base, ty, offset, null_or_top, layout);
      return;
    }
    if (llvm::isa<llvm::UndefValue>(c)) {
      fillAggregateType(base, ty, offset, PointsToSet::top(), layout);
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
    if (llvm::isa<llvm::ConstantDataSequential>(c))
      return;
    std::set<llvm::Type *> seen;
    if (containsPointerType(ty, seen))
      fillAggregateType(base, ty, offset, PointsToSet::top(), layout);
  }

  bool isNonEscapingDirectGlobal(const llvm::GlobalVariable *gv) const {
    if (!gv)
      return false;
    if (gv->isConstant())
      return true;
    if (module_has_external_ctors || !gv->hasLocalLinkage() ||
        gv->isExternallyInitialized())
      return false;
    for (const llvm::User *u : gv->users()) {
      if (const auto *ld = llvm::dyn_cast<llvm::LoadInst>(u)) {
        if (ld->getPointerOperand() != gv || ld->isVolatile())
          return false;
      } else if (const auto *st = llvm::dyn_cast<llvm::StoreInst>(u)) {
        if (st->getPointerOperand() != gv || st->getValueOperand() == gv ||
            st->isVolatile())
          return false;
      } else {
        return false;
      }
    }
    return true;
  }

  const std::set<const llvm::Function *> &
  getGlobalMutatingFuncs(const llvm::GlobalVariable *gv) {
    auto it = global_mutating_funcs.find(gv);
    if (it != global_mutating_funcs.end())
      return it->second;
    std::set<const llvm::Function *> writers;
    for (const llvm::User *u : gv->users()) {
      if (const auto *st = llvm::dyn_cast<llvm::StoreInst>(u))
        writers.insert(st->getFunction());
    }
    bool changed = true;
    while (changed) {
      changed = false;
      for (const llvm::Function &fn : module) {
        if (fn.isDeclaration() || writers.count(&fn))
          continue;
        bool writes_gv = false;
        for (const llvm::BasicBlock &bb : fn) {
          for (const llvm::Instruction &inst : bb) {
            const auto *cb = llvm::dyn_cast<llvm::CallBase>(&inst);
            if (!cb || !writesMemory(*cb, spec_mgr))
              continue;
            const llvm::Function *callee = directCallee(*cb);
            if (!callee || callee->isDeclaration() || writers.count(callee)) {
              writes_gv = true;
              break;
            }
          }
          if (writes_gv)
            break;
        }
        if (writes_gv) {
          writers.insert(&fn);
          changed = true;
        }
      }
    }
    return global_mutating_funcs.emplace(gv, std::move(writers)).first->second;
  }

  bool instMayClobberGlobal(const llvm::Instruction &inst,
                            const llvm::GlobalVariable *gv,
                            const std::set<const llvm::Function *> &writers) {
    if (const auto *st = llvm::dyn_cast<llvm::StoreInst>(&inst))
      return st->getPointerOperand() == gv;
    if (const auto *cb = llvm::dyn_cast<llvm::CallBase>(&inst)) {
      if (!writesMemory(*cb, spec_mgr))
        return false;
      const llvm::Function *callee = directCallee(*cb);
      return !callee || callee->isDeclaration() || writers.count(callee);
    }
    return false;
  }

  // Sound CFG check: verifies that `ld` dominates `use_inst` inside the same
  // function AND no path from `ld` to `use_inst` executes a store or call that
  // may mutate `gv`. If false, `ld` may hold a stale value of `gv` and MUST NOT
  // be canonicalized to the live global `gv`.
  bool isGlobalLoadFreshAt(const llvm::LoadInst *ld,
                           const llvm::Instruction *use_inst,
                           const llvm::GlobalVariable *gv) {
    if (!ld || !use_inst || !gv)
      return false;
    if (gv->isConstant())
      return true;
    const llvm::Function *fn = use_inst->getFunction();
    if (ld->getFunction() != fn)
      return false;
    const auto &writers = getGlobalMutatingFuncs(gv);
    if (writers.empty())
      return true;

    const llvm::BasicBlock *entry_bb = &fn->getEntryBlock();
    const llvm::BasicBlock *use_bb = use_inst->getParent();
    for (const llvm::Instruction *cur = use_inst->getPrevNode(); cur != nullptr;
         cur = cur->getPrevNode()) {
      if (cur == ld)
        return true;
      if (instMayClobberGlobal(*cur, gv, writers))
        return false;
    }
    if (use_bb == entry_bb)
      return false;

    std::deque<const llvm::BasicBlock *> work;
    std::set<const llvm::BasicBlock *> visited;
    for (const llvm::BasicBlock *pred : llvm::predecessors(use_bb)) {
      if (visited.insert(pred).second)
        work.push_back(pred);
    }
    while (!work.empty()) {
      const llvm::BasicBlock *bb = work.front();
      work.pop_front();
      bool found_ld = false;
      for (auto it = bb->rbegin(); it != bb->rend(); ++it) {
        const llvm::Instruction *cur = &*it;
        if (cur == ld) {
          found_ld = true;
          break;
        }
        if (instMayClobberGlobal(*cur, gv, writers))
          return false;
      }
      if (found_ld)
        continue;
      if (bb == entry_bb)
        return false;
      for (const llvm::BasicBlock *pred : llvm::predecessors(bb)) {
        if (visited.insert(pred).second)
          work.push_back(pred);
      }
    }
    return true;
  }

  // Verify no instruction on any path from `fn`'s entry to `use_inst` may
  // mutate `gv` (used when a caller's global load is forwarded via an Argument).
  bool isGlobalUnmodifiedBefore(const llvm::Instruction *use_inst,
                                const llvm::GlobalVariable *gv) {
    if (!use_inst || !gv || gv->isConstant())
      return true;
    const auto &writers = getGlobalMutatingFuncs(gv);
    if (writers.empty())
      return true;
    const llvm::Function *fn = use_inst->getFunction();
    const llvm::BasicBlock *use_bb = use_inst->getParent();
    for (const llvm::Instruction *cur = use_inst->getPrevNode(); cur != nullptr;
         cur = cur->getPrevNode()) {
      if (instMayClobberGlobal(*cur, gv, writers))
        return false;
    }
    std::deque<const llvm::BasicBlock *> work;
    std::set<const llvm::BasicBlock *> visited;
    for (const llvm::BasicBlock *pred : llvm::predecessors(use_bb)) {
      if (visited.insert(pred).second)
        work.push_back(pred);
    }
    while (!work.empty()) {
      const llvm::BasicBlock *bb = work.front();
      work.pop_front();
      for (const llvm::Instruction &cur : *bb) {
        if (instMayClobberGlobal(cur, gv, writers))
          return false;
      }
      for (const llvm::BasicBlock *pred : llvm::predecessors(bb)) {
        if (visited.insert(pred).second)
          work.push_back(pred);
      }
    }
    (void)fn;
    return true;
  }

  void recordKill(const llvm::Instruction *inst, Id cid) {
    if (!inst || cid == INVALID)
      return;
    auto &vec = inst_kills_conditions[inst];
    if (std::find(vec.begin(), vec.end(), cid) == vec.end())
      vec.push_back(cid);
  }

  void recordEntryKill(const llvm::Function *fn, Id cid) {
    if (!fn || cid == INVALID)
      return;
    auto fit = functions.find(fn);
    if (fit == functions.end())
      return;
    auto &vec = program.functions[fit->second].entry_kills_conditions;
    if (std::find(vec.begin(), vec.end(), cid) == vec.end())
      vec.push_back(cid);
  }

  void recordGlobalCond(Id cid) {
    if (cid == INVALID)
      return;
    auto &vec = program.global_conditions;
    if (std::find(vec.begin(), vec.end(), cid) == vec.end())
      vec.push_back(cid);
  }

  void recordParamCond(const llvm::Function *fn, ParamCondInfo info) {
    if (!fn || info.cid == INVALID)
      return;
    auto &vec = param_conds[fn];
    for (const auto &e : vec)
      if (e.cid == info.cid)
        return;
    vec.push_back(info);
  }

  const llvm::Value *
  canonicalizeOperand(const llvm::Value *v, const llvm::Instruction *use_inst,
                      std::vector<const llvm::Instruction *> &kill_sites,
                      std::vector<const llvm::Function *> &entry_kill_fns,
                      bool &is_mutable_global) {
    const llvm::Value *cur_v = v;
    const llvm::Instruction *cur_use = use_inst;
    std::vector<const llvm::Instruction *> intermediate_uses;
    std::set<const llvm::Value *> visited;
    while (cur_v && visited.insert(cur_v).second) {
      if (const auto *arg = llvm::dyn_cast<llvm::Argument>(cur_v)) {
        const llvm::Function *parent = arg->getParent();
        auto fit = functions.find(parent);
        if (!parent || parent->isDeclaration() || fit == functions.end() ||
            fit->second == program.entry || recursive[fit->second] ||
            parent->hasAddressTaken() || arg->hasByValAttr())
          break;
        const llvm::Value *shared_actual = nullptr;
        const llvm::CallBase *single_call = nullptr;
        std::vector<const llvm::CallBase *> all_calls;
        bool ok = true;
        for (const llvm::User *u : parent->users()) {
          const auto *cb = llvm::dyn_cast<llvm::CallBase>(u);
          if (!cb || directCallee(*cb) != parent ||
              arg->getArgNo() >= cb->arg_size()) {
            ok = false;
            break;
          }
          all_calls.push_back(cb);
          const llvm::Value *act = cb->getArgOperand(arg->getArgNo());
          if (!shared_actual) {
            shared_actual = act;
            single_call = cb;
          } else if (shared_actual != act) {
            ok = false;
            break;
          }
        }
        if (!ok || !shared_actual)
          break;
        // If forwarding a mutable global load across multiple call sites, only
        // follow single-call chains (multi-call freshness requires checking
        // every call site individually).
        if (all_calls.size() == 1) {
          intermediate_uses.push_back(cur_use);
          cur_use = single_call;
        } else {
          cur_use = nullptr;
        }
        cur_v = shared_actual;
        continue;
      }
      if (const auto *ld = llvm::dyn_cast<llvm::LoadInst>(cur_v);
          ld && !ld->isVolatile()) {
        if (const auto *gv = llvm::dyn_cast<llvm::GlobalVariable>(
                ld->getPointerOperand()->stripPointerCasts())) {
          if (isNonEscapingDirectGlobal(gv)) {
            bool fresh = gv->isConstant();
            if (!fresh && cur_use != nullptr &&
                isGlobalLoadFreshAt(ld, cur_use, gv)) {
              fresh = true;
              for (const llvm::Instruction *iu : intermediate_uses) {
                if (!isGlobalUnmodifiedBefore(iu, gv)) {
                  fresh = false;
                  break;
                }
              }
            }
            if (fresh) {
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
      }
      break;
    }
    if (const auto *ci = llvm::dyn_cast<llvm::Instruction>(cur_v))
      kill_sites.push_back(ci);
    else if (const auto *arg = llvm::dyn_cast<llvm::Argument>(cur_v))
      entry_kill_fns.push_back(arg->getParent());
    return cur_v;
  }

  Id internAtomicICmp(llvm::CmpInst::Predicate pred, const llvm::Value *lhs_raw,
                      const llvm::Value *rhs_raw,
                      const llvm::Instruction *use_inst, bool &polarity) {
    std::vector<const llvm::Instruction *> kills;
    std::vector<const llvm::Function *> entry_kills;
    bool is_mutable_global = false;
    const llvm::Value *lhs =
        canonicalizeOperand(lhs_raw, use_inst, kills, entry_kills,
                            is_mutable_global);
    const llvm::Value *rhs =
        canonicalizeOperand(rhs_raw, use_inst, kills, entry_kills,
                            is_mutable_global);
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
    // Record formal-argument parameter predicates for call-site substitution
    // (skip `byval` arguments whose callee address is a fresh stack copy).
    if (const auto *arg = llvm::dyn_cast<llvm::Argument>(lhs)) {
      if (!arg->hasByValAttr()) {
        if (const auto *c = llvm::dyn_cast<llvm::Constant>(rhs))
          recordParamCond(arg->getParent(),
                          {cid, arg->getArgNo(), false,
                           static_cast<unsigned>(pred), true, c});
      }
    } else if (const auto *arg = llvm::dyn_cast<llvm::Argument>(rhs)) {
      if (!arg->hasByValAttr()) {
        if (const auto *c = llvm::dyn_cast<llvm::Constant>(lhs))
          recordParamCond(arg->getParent(),
                          {cid, arg->getArgNo(), false,
                           static_cast<unsigned>(pred), false, c});
      }
    }
    return cid;
  }

  Id resolveSwitchCaseCondition(const llvm::Function &function,
                                const llvm::Value &switch_val,
                                const llvm::ConstantInt *case_val,
                                const llvm::Instruction *use_inst) {
    (void)function;
    if (!case_val)
      return INVALID;
    bool pol = true;
    return internAtomicICmp(llvm::CmpInst::ICMP_EQ, &switch_val, case_val,
                            use_inst, pol);
  }

  // Canonicalize a branch/select i1 condition into a (CondNode ID, polarity)
  // pair, supporting compound boolean operators (`xor`, `and`, `or`, `select`),
  // normalized `icmp` predicates, and fresh-verified non-escaping globals.
  std::pair<Id, bool>
  resolveBranchCondition(const llvm::Function &function,
                         const llvm::Value &cond,
                         const llvm::Instruction *use_inst,
                         unsigned depth = 0) {
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

    auto applyPol = [&](std::pair<Id, bool> p) -> Id {
      if (p.first == INVALID)
        return INVALID;
      return p.second ? p.first : program.addCondNot(p.first);
    };

    if (depth < 8) {
      if (const auto *bin = llvm::dyn_cast<llvm::BinaryOperator>(cur)) {
        if (bin->getType()->isIntegerTy(1) &&
            (bin->getOpcode() == llvm::Instruction::And ||
             bin->getOpcode() == llvm::Instruction::Or)) {
          auto l =
              resolveBranchCondition(function, *bin->getOperand(0), use_inst,
                                     depth + 1);
          auto r =
              resolveBranchCondition(function, *bin->getOperand(1), use_inst,
                                     depth + 1);
          const Id ln = applyPol(l);
          const Id rn = applyPol(r);
          if (ln != INVALID && rn != INVALID) {
            const Id combined = (bin->getOpcode() == llvm::Instruction::And)
                                    ? program.addCondAnd(ln, rn)
                                    : program.addCondOr(ln, rn);
            return {combined, polarity};
          }
        }
      } else if (const auto *sel = llvm::dyn_cast<llvm::SelectInst>(cur)) {
        if (sel->getType()->isIntegerTy(1)) {
          const auto *tc =
              llvm::dyn_cast<llvm::ConstantInt>(sel->getTrueValue());
          const auto *fc =
              llvm::dyn_cast<llvm::ConstantInt>(sel->getFalseValue());
          if (tc && tc->isOne()) {
            // select c, true, b  ===  c || b
            const Id cn = applyPol(resolveBranchCondition(
                function, *sel->getCondition(), use_inst, depth + 1));
            const Id bn = applyPol(resolveBranchCondition(
                function, *sel->getFalseValue(), use_inst, depth + 1));
            if (cn != INVALID && bn != INVALID)
              return {program.addCondOr(cn, bn), polarity};
          } else if (fc && fc->isZero()) {
            // select c, b, false ===  c && b
            const Id cn = applyPol(resolveBranchCondition(
                function, *sel->getCondition(), use_inst, depth + 1));
            const Id bn = applyPol(resolveBranchCondition(
                function, *sel->getTrueValue(), use_inst, depth + 1));
            if (cn != INVALID && bn != INVALID)
              return {program.addCondAnd(cn, bn), polarity};
          } else if (tc && tc->isZero()) {
            // select c, false, b === !c && b
            auto cp = resolveBranchCondition(function, *sel->getCondition(),
                                             use_inst, depth + 1);
            cp.second = !cp.second;
            const Id cn = applyPol(cp);
            const Id bn = applyPol(resolveBranchCondition(
                function, *sel->getFalseValue(), use_inst, depth + 1));
            if (cn != INVALID && bn != INVALID)
              return {program.addCondAnd(cn, bn), polarity};
          } else if (fc && fc->isOne()) {
            // select c, b, true  === !c || b
            auto cp = resolveBranchCondition(function, *sel->getCondition(),
                                             use_inst, depth + 1);
            cp.second = !cp.second;
            const Id cn = applyPol(cp);
            const Id bn = applyPol(resolveBranchCondition(
                function, *sel->getTrueValue(), use_inst, depth + 1));
            if (cn != INVALID && bn != INVALID)
              return {program.addCondOr(cn, bn), polarity};
          }
        }
      }
    }

    if (const auto *icmp = llvm::dyn_cast<llvm::ICmpInst>(cur)) {
      const Id cid = internAtomicICmp(icmp->getPredicate(), icmp->getOperand(0),
                                      icmp->getOperand(1), use_inst, polarity);
      return {cid, polarity};
    }

    std::vector<const llvm::Instruction *> kills;
    std::vector<const llvm::Function *> entry_kills;
    bool is_mutable_global = false;
    const llvm::Value *canon_cur = canonicalizeOperand(
        cur, use_inst, kills, entry_kills, is_mutable_global);
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
    if (const auto *arg = llvm::dyn_cast<llvm::Argument>(canon_cur))
      recordParamCond(arg->getParent(),
                      {cid, arg->getArgNo(), true, 0, true, nullptr});
    return {cid, polarity};
  }

  void extractEdgePtrGuards(const llvm::Value &cond, bool taken_true,
                            Id branch_site,
                            std::vector<EdgePtrGuard> &out) {
    bool pol = taken_true;
    const llvm::Value *cur = &cond;
    while (const auto *bin = llvm::dyn_cast<llvm::BinaryOperator>(cur)) {
      if (bin->getOpcode() == llvm::Instruction::Xor) {
        if (const auto *ci =
                llvm::dyn_cast<llvm::ConstantInt>(bin->getOperand(1))) {
          if (ci->isOne()) {
            pol = !pol;
            cur = bin->getOperand(0);
            continue;
          }
        }
        if (const auto *ci =
                llvm::dyn_cast<llvm::ConstantInt>(bin->getOperand(0))) {
          if (ci->isOne()) {
            pol = !pol;
            cur = bin->getOperand(1);
            continue;
          }
        }
      }
      break;
    }
    const auto *icmp = llvm::dyn_cast<llvm::ICmpInst>(cur);
    if (!icmp)
      return;
    const auto pred = icmp->getPredicate();
    if (pred != llvm::CmpInst::ICMP_EQ && pred != llvm::CmpInst::ICMP_NE)
      return;
    const llvm::Value *lhs = icmp->getOperand(0);
    const llvm::Value *rhs = icmp->getOperand(1);
    if (!lhs->getType()->isPointerTy() || !rhs->getType()->isPointerTy())
      return;
    const bool is_eq = (pred == llvm::CmpInst::ICMP_EQ) ? pol : !pol;
    auto singleConstObj = [&](const llvm::Value *v) -> Id {
      const auto *c = llvm::dyn_cast<llvm::Constant>(v);
      if (!c)
        return INVALID;
      const PointsToSet pts = constantPoints(*c);
      if (pts.isTop() || pts.size() != 1)
        return INVALID;
      return *pts.begin();
    };
    const Id lobj = singleConstObj(lhs);
    const Id robj = singleConstObj(rhs);
    if (lobj == INVALID && robj != INVALID) {
      const Id lval = valueId(*lhs);
      if (lval != INVALID)
        out.push_back({is_eq ? EdgeGuardKind::PointsTo
                             : EdgeGuardKind::NotPointsTo,
                       lval, robj, branch_site});
    } else if (robj == INVALID && lobj != INVALID) {
      const Id rval = valueId(*rhs);
      if (rval != INVALID)
        out.push_back({is_eq ? EdgeGuardKind::PointsTo
                             : EdgeGuardKind::NotPointsTo,
                       rval, lobj, branch_site});
    } else if (lobj == INVALID && robj == INVALID) {
      const Id lval = valueId(*lhs);
      const Id rval = valueId(*rhs);
      if (lval != INVALID && rval != INVALID && lval != rval)
        out.push_back({is_eq ? EdgeGuardKind::Alias : EdgeGuardKind::NotAlias,
                       lval, rval, branch_site});
    }
  }

  void populateCallCondSubsts() {
    for (const llvm::Function &fn : module) {
      if (fn.isDeclaration())
        continue;
      for (const llvm::BasicBlock &bb : fn) {
        for (const llvm::Instruction &inst : bb) {
          const auto *cb = llvm::dyn_cast<llvm::CallBase>(&inst);
          if (!cb)
            continue;
          const llvm::Function *callee = directCallee(*cb);
          if (!callee || callee->isDeclaration())
            continue;
          auto pit = param_conds.find(callee);
          if (pit == param_conds.end() || pit->second.empty())
            continue;
          auto &subs = call_cond_substs[cb];
          for (const ParamCondInfo &pc : pit->second) {
            if (pc.arg_no >= cb->arg_size())
              continue;
            const llvm::Value *actual = cb->getArgOperand(pc.arg_no);
            if (pc.is_bool_arg) {
              if (const auto *ci = llvm::dyn_cast<llvm::ConstantInt>(actual)) {
                if (ci->getBitWidth() == 1)
                  subs.push_back({pc.cid, INVALID, !ci->isZero()});
              } else if (actual->getType()->isIntegerTy(1)) {
                std::vector<const llvm::Instruction *> dummy_k;
                std::vector<const llvm::Function *> dummy_ek;
                bool dummy_g = false;
                const llvm::Value *canon_act = canonicalizeOperand(
                    actual, cb, dummy_k, dummy_ek, dummy_g);
                auto vit = value_conditions.find(canon_act);
                if (vit != value_conditions.end())
                  subs.push_back({pc.cid, vit->second, true});
              }
              continue;
            }
            const auto pred = static_cast<llvm::CmpInst::Predicate>(pc.pred);
            if (const auto *act_ci =
                    llvm::dyn_cast<llvm::ConstantInt>(actual)) {
              if (const auto *rhs_ci =
                      llvm::dyn_cast_or_null<llvm::ConstantInt>(pc.const_rhs)) {
                if (act_ci->getBitWidth() == rhs_ci->getBitWidth()) {
                  const bool val =
                      pc.arg_is_lhs
                          ? llvm::ICmpInst::compare(act_ci->getValue(),
                                                    rhs_ci->getValue(), pred)
                          : llvm::ICmpInst::compare(rhs_ci->getValue(),
                                                    act_ci->getValue(), pred);
                  subs.push_back({pc.cid, INVALID, val});
                }
              }
              continue;
            }
            if (!pc.const_rhs || actual->getType() != pc.const_rhs->getType())
              continue;
            // Non-constant actual: match against existing caller condition.
            std::vector<const llvm::Instruction *> dummy_k;
            std::vector<const llvm::Function *> dummy_ek;
            bool dummy_g = false;
            const llvm::Value *canon_act =
                canonicalizeOperand(actual, cb, dummy_k, dummy_ek, dummy_g);
            const llvm::Value *qlhs = pc.arg_is_lhs ? canon_act : pc.const_rhs;
            const llvm::Value *qrhs = pc.arg_is_lhs ? pc.const_rhs : canon_act;
            if (pred == llvm::CmpInst::ICMP_EQ && qrhs < qlhs)
              std::swap(qlhs, qrhs);
            auto iit = icmp_conditions.find({nullptr, pc.pred, qlhs, qrhs});
            if (iit != icmp_conditions.end())
              subs.push_back({pc.cid, iit->second, true});
          }
        }
      }
    }
  }

  Id addObject(const llvm::Value &allocation, Object object) {
    Id id = program.addObject(std::move(object));
    objects.emplace(&allocation, id);
    allocations.push_back(&allocation);
    return id;
  }

  Id nullValue() {
    if (null_value == INVALID)
      null_value = program.addConstant(
          null_may_be_valid ? PointsToSet::top() : PointsToSet(NULL_OBJECT),
          "<null-value>");
    return null_value;
  }

  void computeRecursion() {
    std::vector<std::set<Id>> graph(program.functions.size());
    for (const llvm::Function *ctor : parsed_ctors)
      graph[program.entry].insert(functions.at(ctor));
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
    bool delta_valid = true;
    std::uint64_t var_stride = 0;
    auto addDelta = [&](std::int64_t step) {
      if (!delta_valid)
        return;
      if ((step > 0 &&
           const_delta > std::numeric_limits<std::int64_t>::max() - step) ||
          (step < 0 &&
           const_delta < std::numeric_limits<std::int64_t>::min() - step)) {
        delta_valid = false;
        return;
      }
      const_delta += step;
    };
    for (auto gti = llvm::gep_type_begin(gep), gte = llvm::gep_type_end(gep);
         gti != gte; ++gti) {
      llvm::Value *idx = gti.getOperand();
      if (llvm::StructType *st = gti.getStructTypeOrNull()) {
        if (const auto *ci = llvm::dyn_cast<llvm::ConstantInt>(idx)) {
          const llvm::StructLayout *sl = layout.getStructLayout(st);
          const std::uint64_t elem_off =
              sl->getElementOffset(ci->getZExtValue());
          if (elem_off > static_cast<std::uint64_t>(
                             std::numeric_limits<std::int64_t>::max()))
            delta_valid = false;
          else
            addDelta(static_cast<std::int64_t>(elem_off));
        }
      } else {
        llvm::Type *elem = gti.getIndexedType();
        const std::uint64_t stride =
            elem && elem->isSized() ? layout.getTypeAllocSize(elem) : 0;
        if (const auto *ci = llvm::dyn_cast<llvm::ConstantInt>(idx)) {
          if (stride > static_cast<std::uint64_t>(
                           std::numeric_limits<std::int64_t>::max())) {
            delta_valid = false;
          } else {
            const std::int64_t s = static_cast<std::int64_t>(stride);
            const std::int64_t k = ci->getSExtValue();
            if (s != 0 &&
                (k > std::numeric_limits<std::int64_t>::max() / s ||
                 k < std::numeric_limits<std::int64_t>::min() / s))
              delta_valid = false;
            else
              addDelta(k * s);
          }
        } else if (stride > 0) {
          var_stride =
              (var_stride == 0) ? stride : std::gcd(var_stride, stride);
        }
      }
    }
    if (!delta_valid)
      return {0, false, 0};
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
            for (Id obj : base_pts.objects()) {
              const PointsToSet resolved =
                  program.resolveField(obj, delta, known, stride);
              result.join(resolved);
              // Mark any object reached at a non-exact interior byte offset.
              if ((delta != 0 || !known) && !resolved.isTop() && obj >= 2 &&
                  obj < program.objects.size()) {
                const Id b = program.baseOf(obj);
                if (!program.objects[b].field_sensitive) {
                  program.objects[obj].has_interior_pointer = true;
                } else if (known) {
                  const std::int64_t ts =
                      static_cast<std::int64_t>(program.objects[obj].offset) +
                      delta;
                  if (ts >= 0) {
                    const std::uint64_t folded =
                        program.foldOffset(b, static_cast<std::uint64_t>(ts));
                    for (Id ro : resolved.objects())
                      if (ro >= 2 && ro < program.objects.size() &&
                          (resolved.size() > 1 ||
                           program.objects[ro].offset != folded))
                        program.objects[ro].has_interior_pointer = true;
                  }
                } else {
                  for (Id ro : resolved.objects())
                    if (ro >= 2 && ro < program.objects.size())
                      program.objects[ro].has_interior_pointer = true;
                }
              }
            }
          }
        }
      }
    } else if (const auto *expression =
                   llvm::dyn_cast<llvm::ConstantExpr>(&constant)) {
      if (expression->getOpcode() == llvm::Instruction::BitCast ||
          expression->getOpcode() == llvm::Instruction::AddrSpaceCast) {
        result = constantPoints(
            *llvm::cast<llvm::Constant>(expression->getOperand(0)));
      } else if (expression->getOpcode() == llvm::Instruction::Select &&
                 expression->getNumOperands() == 3) {
        result = constantPoints(
            *llvm::cast<llvm::Constant>(expression->getOperand(1)));
        result.join(constantPoints(
            *llvm::cast<llvm::Constant>(expression->getOperand(2))));
      } else if (expression->getOpcode() == llvm::Instruction::IntToPtr) {
        if (const auto *p2i = llvm::dyn_cast<llvm::ConstantExpr>(
                expression->getOperand(0));
            p2i && p2i->getOpcode() == llvm::Instruction::PtrToInt) {
          result = constantPoints(
              *llvm::cast<llvm::Constant>(p2i->getOperand(0)));
        }
      }
    }
    resolving.erase(&constant);
    return result;
  }

  PointsToSet extractConstantLeafPoints(const llvm::Constant &c,
                                        std::uint64_t target_off,
                                        const llvm::DataLayout &layout,
                                        const std::vector<ArrayRegion> &regions,
                                        std::uint64_t cur_off = 0) {
    llvm::Type *ty = c.getType();
    if (ty->isPointerTy()) {
      return foldOffsetWithRegions(cur_off, regions) == target_off
                 ? constantPoints(c)
                 : PointsToSet{};
    }
    if (llvm::isa<llvm::ConstantAggregateZero>(&c)) {
      return null_may_be_valid ? PointsToSet::top() : PointsToSet(NULL_OBJECT);
    }
    if (llvm::isa<llvm::UndefValue>(&c))
      return PointsToSet::top();
    if (const auto *cs = llvm::dyn_cast<llvm::ConstantStruct>(&c)) {
      const llvm::StructLayout *sl = layout.getStructLayout(cs->getType());
      PointsToSet out;
      for (unsigned i = 0; i < cs->getNumOperands(); ++i)
        out.join(extractConstantLeafPoints(
            *cs->getOperand(i), target_off, layout, regions,
            cur_off + sl->getElementOffset(i)));
      return out;
    }
    if (const auto *ca = llvm::dyn_cast<llvm::ConstantArray>(&c)) {
      llvm::Type *elem = ca->getType()->getElementType();
      const std::uint64_t stride =
          elem->isSized() ? layout.getTypeAllocSize(elem) : 0;
      PointsToSet out;
      for (unsigned i = 0; i < ca->getNumOperands(); ++i)
        out.join(extractConstantLeafPoints(*ca->getOperand(i), target_off,
                                           layout, regions,
                                           cur_off + i * stride));
      return out;
    }
    return PointsToSet::top();
  }

  Id aggValueId(const llvm::Value &agg, std::uint64_t offset, Id fallback_fn) {
    auto key = std::make_pair(&agg, offset);
    auto it = agg_values.find(key);
    if (it != agg_values.end())
      return it->second;
    Value normalized;
    normalized.name = nameOf(agg) + ".agg+" + std::to_string(offset);
    if (const auto *c = llvm::dyn_cast<llvm::Constant>(&agg)) {
      normalized.constant = true;
      const llvm::DataLayout &layout = module.getDataLayout();
      std::vector<FieldSpec> all_f, ptr_f;
      std::vector<ArrayRegion> regs;
      flattenAggregateFields(c->getType(), layout, all_f, ptr_f, regs);
      PointsToSet pts = extractConstantLeafPoints(*c, offset, layout, regs);
      normalized.initial = pts.empty() ? PointsToSet::top() : pts;
    } else if (const auto *inst = llvm::dyn_cast<llvm::Instruction>(&agg)) {
      normalized.function = functions.at(inst->getFunction());
    } else if (const auto *arg = llvm::dyn_cast<llvm::Argument>(&agg)) {
      normalized.function = functions.at(arg->getParent());
    } else {
      normalized.function = fallback_fn;
    }
    Id id = program.addValue(std::move(normalized));
    if (value_to_llvm.size() <= id)
      value_to_llvm.resize(id + 1, nullptr);
    agg_values.emplace(key, id);
    return id;
  }

  Id makeTempValue(Id fn_id, std::string name) {
    Value v;
    v.name = std::move(name);
    v.function = fn_id;
    const Id id = program.addValue(std::move(v));
    if (value_to_llvm.size() <= id)
      value_to_llvm.resize(id + 1, nullptr);
    return id;
  }

  Id emitFieldPtr(Id fn_id, Id bb_id, Id base_ptr, std::uint64_t offset) {
    if (offset == 0)
      return base_ptr;
    const Id tmp = makeTempValue(fn_id, "<agg-ptr+" + std::to_string(offset) + ">");
    Instruction fi;
    fi.opcode = Opcode::Field;
    fi.result = tmp;
    fi.operands = {base_ptr};
    fi.field_offset = static_cast<std::int64_t>(offset);
    fi.field_known = true;
    fi.field_array_stride = 0;
    program.append(fn_id, bb_id, std::move(fi));
    return tmp;
  }

  Id slotFieldAddr(Id base_slot, std::uint64_t offset) {
    const PointsToSet resolved =
        program.resolveField(base_slot, static_cast<std::int64_t>(offset), true);
    const Id obj = (!resolved.isTop() && resolved.size() == 1)
                       ? *resolved.begin()
                       : base_slot;
    return program.addConstant(PointsToSet(obj),
                               "<slot+" + std::to_string(offset) + ">");
  }

  Id getStaticReturnObject(const llvm::Function *callee,
                           const llvm::DataLayout &layout, unsigned addrspace) {
    auto it = static_return_objects.find(callee);
    if (it != static_return_objects.end())
      return it->second;
    Object obj;
    obj.name = "<ext-static:" + callee->getName().str() + ">";
    obj.kind = ObjectKind::Global;
    obj.singleton = false;
    obj.bytes = layout.getPointerSize(addrspace);
    obj.initial = PointsToSet::top();
    const Id id = program.addObject(std::move(obj));
    allocations.push_back(nullptr);
    static_return_objects.emplace(callee, id);
    return id;
  }

  // Recover exact source pointer(s) for `inttoptr(ptrtoint(p))` round-trips
  // (including width-preserving integer casts and select/phi of ptrtoints).
  bool collectPtrToIntSources(const llvm::Value *v, unsigned ptr_bits,
                              std::vector<const llvm::Value *> &out,
                              std::set<const llvm::Value *> &seen,
                              unsigned depth = 0) const {
    if (!v || depth > 8)
      return false;
    if (!seen.insert(v).second)
      return true;
    if (const auto *p2i = llvm::dyn_cast<llvm::PtrToIntInst>(v)) {
      if (p2i->getType()->getIntegerBitWidth() >= ptr_bits) {
        out.push_back(p2i->getPointerOperand());
        return true;
      }
      return false;
    }
    if (const auto *ce = llvm::dyn_cast<llvm::ConstantExpr>(v);
        ce && ce->getOpcode() == llvm::Instruction::PtrToInt) {
      if (ce->getType()->getIntegerBitWidth() >= ptr_bits) {
        out.push_back(ce->getOperand(0));
        return true;
      }
      return false;
    }
    if (const auto *ci = llvm::dyn_cast<llvm::CastInst>(v)) {
      if ((llvm::isa<llvm::ZExtInst>(ci) || llvm::isa<llvm::SExtInst>(ci) ||
           llvm::isa<llvm::BitCastInst>(ci) || llvm::isa<llvm::TruncInst>(ci)) &&
          ci->getSrcTy()->isIntegerTy() &&
          ci->getSrcTy()->getIntegerBitWidth() >= ptr_bits &&
          ci->getDestTy()->isIntegerTy() &&
          ci->getDestTy()->getIntegerBitWidth() >= ptr_bits)
        return collectPtrToIntSources(ci->getOperand(0), ptr_bits, out, seen,
                                      depth + 1);
      return false;
    }
    if (const auto *sel = llvm::dyn_cast<llvm::SelectInst>(v)) {
      return collectPtrToIntSources(sel->getTrueValue(), ptr_bits, out, seen,
                                    depth + 1) &&
             collectPtrToIntSources(sel->getFalseValue(), ptr_bits, out, seen,
                                    depth + 1);
    }
    return false;
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
    const Id fn_id = functions.at(&function);
    const Id bb_id = blocks.at(&block);
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
      } else {
        std::vector<FieldSpec> all_f, ptr_f;
        std::vector<ArrayRegion> regs;
        if (flattenAggregateFields(load->getType(), layout, all_f, ptr_f,
                                   regs)) {
          const Id ptr_val = valueId(*load->getPointerOperand());
          Id last_sid = INVALID;
          for (const FieldSpec &fs : ptr_f) {
            const Id fptr = emitFieldPtr(fn_id, bb_id, ptr_val, fs.offset);
            Instruction ld;
            ld.opcode = Opcode::Load;
            ld.result = aggValueId(instruction, fs.offset, fn_id);
            ld.operands = {fptr};
            ld.read_bytes = fs.ptr_bytes;
            last_sid = program.append(fn_id, bb_id, std::move(ld));
          }
          if (last_sid != INVALID) {
            sites[&instruction] = last_sid;
            if (site_to_llvm.size() <= last_sid)
              site_to_llvm.resize(last_sid + 1, nullptr);
            site_to_llvm[last_sid] = &instruction;
            return;
          }
        }
      }
    } else if (const auto *store =
                   llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
      const llvm::Value *val_op = store->getValueOperand();
      const Id dst_ptr = valueId(*store->getPointerOperand());
      if (val_op->getType()->isPointerTy()) {
        i.opcode = Opcode::Store;
        i.operands = {dst_ptr, valueId(*val_op)};
        i.write_bytes = layout.getPointerTypeSize(val_op->getType());
      } else {
        std::vector<FieldSpec> all_f, ptr_f;
        std::vector<ArrayRegion> regs;
        if (flattenAggregateFields(val_op->getType(), layout, all_f, ptr_f,
                                   regs)) {
          Id last_sid = INVALID;
          for (const FieldSpec &fs : all_f) {
            const Id fptr = emitFieldPtr(fn_id, bb_id, dst_ptr, fs.offset);
            Instruction si;
            if (fs.is_pointer) {
              si.opcode = Opcode::Store;
              si.operands = {fptr, aggValueId(*val_op, fs.offset, fn_id)};
              si.write_bytes = fs.ptr_bytes;
            } else {
              si.opcode = Opcode::Havoc;
              si.operands = {fptr};
              si.write_bytes = fs.size_bytes;
            }
            last_sid = program.append(fn_id, bb_id, std::move(si));
          }
          if (last_sid != INVALID) {
            sites[&instruction] = last_sid;
            if (site_to_llvm.size() <= last_sid)
              site_to_llvm.resize(last_sid + 1, nullptr);
            site_to_llvm[last_sid] = &instruction;
            return;
          }
        }
        // Non-pointer store: havoc ONLY fields overlapping the written byte
        // range (leaving sibling struct pointer fields untouched!).
        i.opcode = Opcode::Havoc;
        i.operands = {dst_ptr};
        llvm::Type *vty = val_op->getType();
        if (vty->isSized())
          i.write_bytes = layout.getTypeStoreSize(vty);
      }
    } else if (const auto *bitcast =
                   llvm::dyn_cast<llvm::BitCastInst>(&instruction)) {
      if (i.result != INVALID) {
        i.opcode = Opcode::Copy;
        i.operands = {valueId(*bitcast->getOperand(0))};
      }
    } else if (const auto *asc =
                   llvm::dyn_cast<llvm::AddrSpaceCastInst>(&instruction)) {
      if (i.result != INVALID &&
          asc->getOperand(0)->getType()->isPointerTy()) {
        i.opcode = Opcode::Copy;
        i.operands = {valueId(*asc->getOperand(0))};
      }
    } else if (const auto *freeze =
                   llvm::dyn_cast<llvm::FreezeInst>(&instruction)) {
      if (i.result != INVALID) {
        i.opcode = Opcode::Copy;
        i.operands = {valueId(*freeze->getOperand(0))};
      } else {
        std::vector<FieldSpec> all_f, ptr_f;
        std::vector<ArrayRegion> regs;
        if (flattenAggregateFields(freeze->getType(), layout, all_f, ptr_f,
                                   regs)) {
          Id last_sid = INVALID;
          for (const FieldSpec &fs : ptr_f) {
            Instruction cp;
            cp.opcode = Opcode::Copy;
            cp.result = aggValueId(instruction, fs.offset, fn_id);
            cp.operands = {aggValueId(*freeze->getOperand(0), fs.offset, fn_id)};
            last_sid = program.append(fn_id, bb_id, std::move(cp));
          }
          if (last_sid != INVALID) {
            sites[&instruction] = last_sid;
            if (site_to_llvm.size() <= last_sid)
              site_to_llvm.resize(last_sid + 1, nullptr);
            site_to_llvm[last_sid] = &instruction;
            return;
          }
        }
      }
    } else if (const auto *i2p =
                   llvm::dyn_cast<llvm::IntToPtrInst>(&instruction)) {
      if (i.result != INVALID) {
        const unsigned pbits =
            layout.getPointerTypeSizeInBits(i2p->getType());
        std::vector<const llvm::Value *> srcs;
        std::set<const llvm::Value *> seen;
        if (collectPtrToIntSources(i2p->getOperand(0), pbits, srcs, seen) &&
            !srcs.empty()) {
          std::vector<Id> op_ids;
          for (const llvm::Value *sv : srcs) {
            const Id vid = valueId(*sv);
            if (vid != INVALID)
              op_ids.push_back(vid);
          }
          std::sort(op_ids.begin(), op_ids.end());
          op_ids.erase(std::unique(op_ids.begin(), op_ids.end()), op_ids.end());
          if (op_ids.size() == 1) {
            i.opcode = Opcode::Copy;
            i.operands = std::move(op_ids);
          } else if (op_ids.size() > 1) {
            i.opcode = Opcode::Join;
            i.operands = std::move(op_ids);
          }
        }
      }
    } else if (const auto *ev =
                   llvm::dyn_cast<llvm::ExtractValueInst>(&instruction)) {
      const llvm::Value *agg = ev->getAggregateOperand();
      std::vector<FieldSpec> all_f, ptr_f;
      std::vector<ArrayRegion> regs;
      std::uint64_t ext_off = 0;
      llvm::Type *leaf_ty = nullptr;
      bool in_arr = false;
      if (flattenAggregateFields(agg->getType(), layout, all_f, ptr_f, regs) &&
          computeExtractInsertOffset(agg->getType(), ev->getIndices(), layout,
                                     ext_off, leaf_ty, in_arr)) {
        if (i.result != INVALID) {
          const std::uint64_t folded = foldOffsetWithRegions(ext_off, regs);
          i.opcode = Opcode::Copy;
          i.operands = {aggValueId(*agg, folded, fn_id)};
        } else {
          std::vector<FieldSpec> sub_all, sub_ptr;
          std::vector<ArrayRegion> sub_regs;
          if (flattenAggregateFields(leaf_ty, layout, sub_all, sub_ptr,
                                     sub_regs)) {
            Id last_sid = INVALID;
            for (const FieldSpec &sfs : sub_ptr) {
              const std::uint64_t src_off =
                  foldOffsetWithRegions(ext_off + sfs.offset, regs);
              Instruction cp;
              cp.opcode = Opcode::Copy;
              cp.result = aggValueId(instruction, sfs.offset, fn_id);
              cp.operands = {aggValueId(*agg, src_off, fn_id)};
              last_sid = program.append(fn_id, bb_id, std::move(cp));
            }
            if (last_sid != INVALID) {
              sites[&instruction] = last_sid;
              if (site_to_llvm.size() <= last_sid)
                site_to_llvm.resize(last_sid + 1, nullptr);
              site_to_llvm[last_sid] = &instruction;
              return;
            }
          }
        }
      }
    } else if (const auto *iv =
                   llvm::dyn_cast<llvm::InsertValueInst>(&instruction)) {
      const llvm::Value *agg = iv->getAggregateOperand();
      const llvm::Value *ins = iv->getInsertedValueOperand();
      std::vector<FieldSpec> all_f, ptr_f;
      std::vector<ArrayRegion> regs;
      std::uint64_t ins_off = 0;
      llvm::Type *ins_ty = nullptr;
      bool in_arr = false;
      if (flattenAggregateFields(iv->getType(), layout, all_f, ptr_f, regs) &&
          computeExtractInsertOffset(iv->getType(), iv->getIndices(), layout,
                                     ins_off, ins_ty, in_arr)) {
        const std::uint64_t ins_size =
            ins_ty->isSized() ? layout.getTypeAllocSize(ins_ty) : 0;
        std::vector<FieldSpec> sub_all, sub_ptr;
        std::vector<ArrayRegion> sub_regs;
        const bool ins_is_agg = flattenAggregateFields(ins_ty, layout, sub_all,
                                                       sub_ptr, sub_regs);
        std::map<std::uint64_t, std::uint64_t> written_sub_off;
        if (ins_ty->isPointerTy()) {
          written_sub_off[foldOffsetWithRegions(ins_off, regs)] = 0;
        } else if (ins_is_agg) {
          for (const FieldSpec &sfs : sub_ptr)
            written_sub_off[foldOffsetWithRegions(ins_off + sfs.offset, regs)] =
                sfs.offset;
        }
        Id last_sid = INVALID;
        for (const FieldSpec &fs : ptr_f) {
          const Id dst_v = aggValueId(instruction, fs.offset, fn_id);
          const Id old_v = aggValueId(*agg, fs.offset, fn_id);
          auto wit = written_sub_off.find(fs.offset);
          Instruction op;
          op.result = dst_v;
          if (wit != written_sub_off.end()) {
            const Id new_v = ins_ty->isPointerTy()
                                 ? valueId(*ins)
                                 : aggValueId(*ins, wit->second, fn_id);
            if (in_arr || fs.inside_array) {
              op.opcode = Opcode::Join;
              op.operands = {new_v, old_v};
            } else {
              op.opcode = Opcode::Copy;
              op.operands = {new_v};
            }
          } else if (ins_size > 0 && !ins_ty->isPointerTy() && !ins_is_agg &&
                     fs.offset >= foldOffsetWithRegions(ins_off, regs) &&
                     fs.offset < foldOffsetWithRegions(ins_off, regs) + ins_size) {
            op.opcode = Opcode::Unknown;
          } else {
            op.opcode = Opcode::Copy;
            op.operands = {old_v};
          }
          last_sid = program.append(fn_id, bb_id, std::move(op));
        }
        if (last_sid != INVALID) {
          sites[&instruction] = last_sid;
          if (site_to_llvm.size() <= last_sid)
            site_to_llvm.resize(last_sid + 1, nullptr);
          site_to_llvm[last_sid] = &instruction;
          return;
        }
      }
    } else if (const auto *gep =
                   llvm::dyn_cast<llvm::GetElementPtrInst>(&instruction)) {
      if (i.result != INVALID &&
          (gep->isInBounds() || gep->hasAllZeroIndices())) {
        const auto *gep_op = llvm::cast<llvm::GEPOperator>(gep);
        auto [delta, known, stride] = analyzeGEP(*gep_op);
        const llvm::Value *base_ptr = gep->getPointerOperand();
        // Fold consecutive constant-offset `inbounds` GEP / BitCast chains
        // inside the same function (e.g., clang -O0 lowering of `s.arr[i].f`)
        // so forward solvers (`Steensgaard`, `OLF`, `Andersen`, `Tabulation`)
        // match `GuardedSummary`'s symbolic offset accumulation without losing
        // sub-array bounds precision.
        if (known && stride == 0) {
          std::set<const llvm::Value *> seen_chain{gep};
          while (base_ptr && seen_chain.insert(base_ptr).second) {
            if (const auto *bc = llvm::dyn_cast<llvm::BitCastInst>(base_ptr)) {
              if (bc->getFunction() != &function)
                break;
              base_ptr = bc->getOperand(0);
              continue;
            }
            const auto *prev_gep =
                llvm::dyn_cast<llvm::GetElementPtrInst>(base_ptr);
            if (!prev_gep || prev_gep->getFunction() != &function ||
                !(prev_gep->isInBounds() || prev_gep->hasAllZeroIndices()))
              break;
            auto [pdelta, pknown, pstride] =
                analyzeGEP(*llvm::cast<llvm::GEPOperator>(prev_gep));
            if (!pknown || pstride != 0)
              break;
            if ((pdelta > 0 &&
                 delta > std::numeric_limits<std::int64_t>::max() - pdelta) ||
                (pdelta < 0 &&
                 delta < std::numeric_limits<std::int64_t>::min() - pdelta))
              break;
            delta += pdelta;
            base_ptr = prev_gep->getPointerOperand();
          }
        }
        if (known && delta == 0 && stride == 0) {
          i.opcode = Opcode::Copy;
          i.operands = {valueId(*base_ptr)};
        } else {
          i.opcode = Opcode::Field;
          i.operands = {valueId(*base_ptr)};
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
        std::tie(i.select_cond_var, i.select_cond_polarity) =
            resolveBranchCondition(function, *select->getCondition(), select);
      } else {
        std::vector<FieldSpec> all_f, ptr_f;
        std::vector<ArrayRegion> regs;
        if (flattenAggregateFields(select->getType(), layout, all_f, ptr_f,
                                   regs)) {
          auto [cvar, cpol] =
              resolveBranchCondition(function, *select->getCondition(), select);
          Id last_sid = INVALID;
          for (const FieldSpec &fs : ptr_f) {
            Instruction j;
            j.opcode = Opcode::Join;
            j.result = aggValueId(instruction, fs.offset, fn_id);
            j.operands = {
                aggValueId(*select->getTrueValue(), fs.offset, fn_id),
                aggValueId(*select->getFalseValue(), fs.offset, fn_id)};
            j.select_cond_var = cvar;
            j.select_cond_polarity = cpol;
            last_sid = program.append(fn_id, bb_id, std::move(j));
          }
          if (last_sid != INVALID) {
            sites[&instruction] = last_sid;
            if (site_to_llvm.size() <= last_sid)
              site_to_llvm.resize(last_sid + 1, nullptr);
            site_to_llvm[last_sid] = &instruction;
            return;
          }
        }
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
      const AllocationModel allocation = allocationModel(*call, spec_mgr);
      const llvm::Function *callee = directCallee(*call);
      const llvm::StringRef name = callee ? callee->getName() : llvm::StringRef();
      if (allocation.kind == AllocationKind::OutParam &&
          allocation.outArgIndex >= 0) {
        const llvm::Value *out_arg =
            call->getArgOperand(static_cast<unsigned>(allocation.outArgIndex));
        const unsigned as = out_arg->getType()->getPointerAddressSpace();
        const std::uint64_t psz = layout.getPointerSize(as);
        if (null_may_be_valid || as != 0) {
          i.opcode = Opcode::Havoc;
          i.operands = {valueId(*out_arg)};
          i.write_bytes = psz;
        } else {
          const Id tmp_alloc =
              makeTempValue(fn_id, nameOf(instruction) + ".out_alloc");
          Instruction al;
          al.opcode = Opcode::Allocate;
          al.object = objects.at(&instruction);
          al.result = tmp_alloc;
          al.may_be_null = false;
          program.append(fn_id, bb_id, std::move(al));
          const Id tmp_old =
              makeTempValue(fn_id, nameOf(instruction) + ".out_old");
          Instruction ld;
          ld.opcode = Opcode::Load;
          ld.result = tmp_old;
          ld.operands = {valueId(*out_arg)};
          ld.read_bytes = psz;
          program.append(fn_id, bb_id, std::move(ld));
          const Id tmp_join =
              makeTempValue(fn_id, nameOf(instruction) + ".out_join");
          Instruction jn;
          jn.opcode = Opcode::Join;
          jn.result = tmp_join;
          jn.operands = {tmp_alloc, tmp_old};
          program.append(fn_id, bb_id, std::move(jn));
          i.opcode = Opcode::Store;
          i.operands = {valueId(*out_arg), tmp_join};
          i.write_bytes = psz;
        }
      } else if (allocation.kind != AllocationKind::None &&
                 i.result != INVALID) {
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
      } else if (deallocationCall(*call, spec_mgr)) {
        i.opcode = Opcode::Nop;
      } else if (isPlacementNewCall(*call)) {
        i.opcode = Opcode::Copy;
        i.operands = {valueId(*call->getArgOperand(1))};
      } else if (isPassThroughPtrIntrinsic(callee) && i.result != INVALID &&
                 call->arg_size() >= 1 &&
                 call->getArgOperand(0)->getType()->isPointerTy()) {
        i.opcode = Opcode::Copy;
        i.operands = {valueId(*call->getArgOperand(0))};
      } else if (isSideEffectFreeIntrinsic(callee) && i.result == INVALID) {
        i.opcode = Opcode::Nop;
      } else if (callee && callee->isDeclaration() &&
                 (name == "memcpy" || name == "memmove" || name == "wmemcpy") &&
                 call->arg_size() >= 3 &&
                 call->getArgOperand(0)->getType()->isPointerTy() &&
                 call->getArgOperand(1)->getType()->isPointerTy()) {
        const auto *len_ci =
            llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(2));
        const std::uint64_t bytes =
            (len_ci && name != "wmemcpy") ? len_ci->getZExtValue() : 0;
        if (!(len_ci && len_ci->isZero())) {
          Instruction mc;
          mc.opcode = Opcode::MemCopy;
          mc.operands = {valueId(*call->getArgOperand(0)),
                         valueId(*call->getArgOperand(1))};
          mc.write_bytes = bytes;
          program.append(fn_id, bb_id, std::move(mc));
        }
        if (i.result != INVALID) {
          i.opcode = Opcode::Copy;
          i.operands = {valueId(*call->getArgOperand(0))};
        } else {
          i.opcode = Opcode::Nop;
        }
      } else if (callee && callee->isDeclaration() && name == "bcopy" &&
                 call->arg_size() >= 3 &&
                 call->getArgOperand(0)->getType()->isPointerTy() &&
                 call->getArgOperand(1)->getType()->isPointerTy()) {
        const auto *len_ci =
            llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(2));
        if (len_ci && len_ci->isZero()) {
          i.opcode = Opcode::Nop;
        } else {
          i.opcode = Opcode::MemCopy;
          i.operands = {valueId(*call->getArgOperand(1)),
                        valueId(*call->getArgOperand(0))};
          i.write_bytes = len_ci ? len_ci->getZExtValue() : 0;
        }
      } else if (callee && callee->isDeclaration() && name == "memset" &&
                 call->arg_size() >= 3 &&
                 call->getArgOperand(0)->getType()->isPointerTy()) {
        const auto *len_ci =
            llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(2));
        if (!(len_ci && len_ci->isZero())) {
          Instruction hv;
          hv.opcode = Opcode::Havoc;
          hv.operands = {valueId(*call->getArgOperand(0))};
          hv.write_bytes = len_ci ? len_ci->getZExtValue() : 0;
          program.append(fn_id, bb_id, std::move(hv));
        }
        if (i.result != INVALID) {
          i.opcode = Opcode::Copy;
          i.operands = {valueId(*call->getArgOperand(0))};
        } else {
          i.opcode = Opcode::Nop;
        }
      } else if (callee && callee->isDeclaration() &&
                 (name == "strcpy" || name == "strncpy" || name == "strcat" ||
                  name == "strncat" || name == "wcscpy" || name == "wcsncpy" ||
                  name == "wcscat" || name == "wcsncat" || name == "fgets" ||
                  name == "fgetws" || name == "gets" || name == "getcwd" ||
                  name == "tmpnam") &&
                 call->arg_size() >= 1 &&
                 call->getArgOperand(0)->getType()->isPointerTy()) {
        Instruction hv;
        hv.opcode = Opcode::Havoc;
        hv.operands = {valueId(*call->getArgOperand(0))};
        hv.write_bytes = 0;
        program.append(fn_id, bb_id, std::move(hv));
        if (i.result != INVALID) {
          const bool may_null =
              (name == "fgets" || name == "fgetws" || name == "gets" ||
               name == "getcwd" || name == "tmpnam");
          if (may_null) {
            i.opcode = Opcode::Join;
            i.operands = {valueId(*call->getArgOperand(0)), nullValue()};
          } else {
            i.opcode = Opcode::Copy;
            i.operands = {valueId(*call->getArgOperand(0))};
          }
        } else {
          i.opcode = Opcode::Nop;
        }
      } else if (callee && callee->isDeclaration() &&
                 (name == "strtod" || name == "strtof" || name == "strtol" ||
                  name == "strtoul") &&
                 call->arg_size() >= 2 &&
                 call->getArgOperand(0)->getType()->isPointerTy() &&
                 call->getArgOperand(1)->getType()->isPointerTy()) {
        const Id tmp_int =
            makeTempValue(fn_id, nameOf(instruction) + ".endptr");
        Instruction fi;
        fi.opcode = Opcode::Field;
        fi.result = tmp_int;
        fi.operands = {valueId(*call->getArgOperand(0))};
        fi.field_known = false;
        fi.field_offset = 0;
        fi.field_array_stride = 0;
        program.append(fn_id, bb_id, std::move(fi));
        i.opcode = Opcode::Store;
        i.operands = {valueId(*call->getArgOperand(1)), tmp_int};
        i.write_bytes = layout.getPointerSize(
            call->getArgOperand(0)->getType()->getPointerAddressSpace());
      } else if (i.result != INVALID && returnsInteriorPointer(callee) &&
                 call->arg_size() != 0 &&
                 call->getArgOperand(0)->getType()->isPointerTy()) {
        const Id tmp = makeTempValue(fn_id, nameOf(instruction) + ".interior");
        Instruction field_inst;
        field_inst.opcode = Opcode::Field;
        field_inst.result = tmp;
        field_inst.operands = {valueId(*call->getArgOperand(0))};
        field_inst.field_known = false;
        field_inst.field_offset = 0;
        field_inst.field_array_stride = 0;
        program.append(fn_id, bb_id, std::move(field_inst));
        i.opcode = Opcode::Join;
        i.operands = {tmp, nullValue()};
      } else if (callee && callee->isDeclaration() && i.result != INVALID &&
                 !spec_mgr.getReturnAliasInfo(callee).empty() &&
                 !spec_mgr.getReturnAliasInfo(callee)[0].isRegion &&
                 spec_mgr.getReturnAliasInfo(callee)[0].isStatic) {
        const auto mr = spec_mgr.getModRefInfo(callee);
        for (int idx : mr.modifiedArgs) {
          if (idx >= 0 && static_cast<unsigned>(idx) < call->arg_size() &&
              call->getArgOperand(static_cast<unsigned>(idx))
                  ->getType()
                  ->isPointerTy()) {
            Instruction hv;
            hv.opcode = Opcode::Havoc;
            hv.operands = {
                valueId(*call->getArgOperand(static_cast<unsigned>(idx)))};
            program.append(fn_id, bb_id, std::move(hv));
          }
        }
        const Id sobj = getStaticReturnObject(
            callee, layout, call->getType()->getPointerAddressSpace());
        const Id tmp_addr =
            makeTempValue(fn_id, nameOf(instruction) + ".static");
        Instruction addr_i;
        addr_i.opcode = Opcode::Address;
        addr_i.object = sobj;
        addr_i.result = tmp_addr;
        program.append(fn_id, bb_id, std::move(addr_i));
        i.opcode = Opcode::Join;
        i.operands = {tmp_addr, nullValue()};
      } else if (callee && callee->isDeclaration() && i.result == INVALID &&
                 spec_mgr.isExitFunction(callee)) {
        i.opcode = Opcode::Nop;
      } else if (callee && callee->isDeclaration() && i.result == INVALID &&
                 !spec_mgr.getModRefInfo(callee).modifiedArgs.empty() &&
                 !callee->isVarArg()) {
        const auto mr = spec_mgr.getModRefInfo(callee);
        Id last_sid = INVALID;
        for (int idx : mr.modifiedArgs) {
          if (idx >= 0 && static_cast<unsigned>(idx) < call->arg_size() &&
              call->getArgOperand(static_cast<unsigned>(idx))
                  ->getType()
                  ->isPointerTy()) {
            Instruction hv;
            hv.opcode = Opcode::Havoc;
            hv.operands = {
                valueId(*call->getArgOperand(static_cast<unsigned>(idx)))};
            last_sid = program.append(fn_id, bb_id, std::move(hv));
          }
        }
        if (last_sid != INVALID) {
          sites[&instruction] = last_sid;
          if (site_to_llvm.size() <= last_sid)
            site_to_llvm.resize(last_sid + 1, nullptr);
          site_to_llvm[last_sid] = &instruction;
          return;
        }
        i.opcode = Opcode::Nop;
      } else {
        // Store aggregate arguments to direct defined callee's synthetic slots
        // before emitting the Call instruction.
        if (callee && !callee->isDeclaration()) {
          unsigned aidx = 0;
          for (const llvm::Argument &param : callee->args()) {
            if (aidx >= call->arg_size())
              break;
            auto asit = agg_arg_slot_base.find(&param);
            if (asit != agg_arg_slot_base.end()) {
              std::vector<FieldSpec> all_f, ptr_f;
              std::vector<ArrayRegion> regs;
              const llvm::Value *act = call->getArgOperand(aidx);
              if (flattenAggregateFields(act->getType(), layout, all_f, ptr_f,
                                         regs)) {
                for (const FieldSpec &fs : ptr_f) {
                  Instruction st;
                  st.opcode = Opcode::Store;
                  st.operands = {slotFieldAddr(asit->second, fs.offset),
                                 aggValueId(*act, fs.offset, fn_id)};
                  st.write_bytes = fs.ptr_bytes;
                  program.append(fn_id, bb_id, std::move(st));
                }
              }
            }
            ++aidx;
          }
        }
        i.opcode = Opcode::Call;
        i.writes_memory = writesMemory(*call, spec_mgr);
        if (callee)
          i.callee = functions.at(callee);
        else if (!call->isInlineAsm())
          i.indirect_target = valueId(*call->getCalledOperand());
        for (const llvm::Use &argument : call->args())
          i.operands.push_back(valueId(*argument));
        auto csit = call_cond_substs.find(call);
        if (csit != call_cond_substs.end())
          i.cond_subst = csit->second;
        i.opaque_alternative = llvm::isa<llvm::InvokeInst>(call) ||
                               llvm::isa<llvm::CallBrInst>(call);
        const Id call_sid = recordSite(instruction, fn_id, bb_id, std::move(i));
        (void)call_sid;
        // Load aggregate return value leaves immediately after Call so
        // `sites[&call]` remains the `Opcode::Call` instruction itself.
        std::vector<FieldSpec> all_f, ptr_f;
        std::vector<ArrayRegion> regs;
        if (flattenAggregateFields(call->getType(), layout, all_f, ptr_f,
                                   regs)) {
          auto rsit =
              (callee && !callee->isDeclaration())
                  ? agg_ret_slot_base.find(callee)
                  : agg_ret_slot_base.end();
          for (const FieldSpec &fs : ptr_f) {
            Instruction leaf_i;
            leaf_i.result = aggValueId(instruction, fs.offset, fn_id);
            if (rsit != agg_ret_slot_base.end()) {
              leaf_i.opcode = Opcode::Load;
              leaf_i.operands = {slotFieldAddr(rsit->second, fs.offset)};
              leaf_i.read_bytes = fs.ptr_bytes;
            } else {
              leaf_i.opcode = Opcode::Unknown;
            }
            program.append(fn_id, bb_id, std::move(leaf_i));
          }
        }
        return;
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
      if (ret->getReturnValue()) {
        if (ret->getReturnValue()->getType()->isPointerTy()) {
          i.operands = {valueId(*ret->getReturnValue())};
        } else {
          auto rsit = agg_ret_slot_base.find(&function);
          if (rsit != agg_ret_slot_base.end()) {
            std::vector<FieldSpec> all_f, ptr_f;
            std::vector<ArrayRegion> regs;
            if (flattenAggregateFields(ret->getReturnValue()->getType(), layout,
                                       all_f, ptr_f, regs)) {
              for (const FieldSpec &fs : ptr_f) {
                Instruction st;
                st.opcode = Opcode::Store;
                st.operands = {
                    slotFieldAddr(rsit->second, fs.offset),
                    aggValueId(*ret->getReturnValue(), fs.offset, fn_id)};
                st.write_bytes = fs.ptr_bytes;
                program.append(fn_id, bb_id, std::move(st));
              }
            }
          }
        }
      }
    } else if (llvm::isa<llvm::ResumeInst>(instruction) ||
               (llvm::isa<llvm::CleanupReturnInst>(instruction) &&
                llvm::cast<llvm::CleanupReturnInst>(instruction)
                    .unwindsToCaller())) {
      i.opcode = Opcode::Return;
    }
    recordSite(instruction, fn_id, bb_id, std::move(i));
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
    out.object_aliases = raw.object_aliases;
    for (Id id : raw.aliases) {
      if (id < value_to_llvm.size() && value_to_llvm[id])
        out.aliases.push_back(value_to_llvm[id]);
    }
    return out;
  }

  LLVMCallTargets translateCallTargets(const CallTargets &raw) const {
    LLVMCallTargets out;
    out.status = raw.status;
    out.may_call_unknown = raw.may_call_unknown;
    for (Id fid : raw.functions) {
      if (fid < func_to_llvm.size() && func_to_llvm[fid])
        out.functions.push_back(func_to_llvm[fid]);
    }
    return out;
  }

  Id queryFunction(const llvm::Function &function) const {
    auto it = functions.find(&function);
    if (it == functions.end())
      throw std::invalid_argument(
          "BootstrapAA: function is in another module");
    return it->second;
  }

  bool defaultQuerySite(const llvm::Value &v, Id &out_site,
                        Point &out_point) const {
    if (const auto *inst = llvm::dyn_cast<llvm::Instruction>(&v)) {
      auto sit = sites.find(inst);
      if (sit == sites.end())
        return false;
      out_site = sit->second;
      out_point = Point::After;
      return true;
    }
    if (const auto *arg = llvm::dyn_cast<llvm::Argument>(&v)) {
      const llvm::Function *fn = arg->getParent();
      if (!fn || fn->isDeclaration() || fn->empty() ||
          fn->getEntryBlock().empty())
        return false;
      auto sit = sites.find(&fn->getEntryBlock().front());
      if (sit == sites.end())
        return false;
      out_site = sit->second;
      out_point = Point::Before;
      return true;
    }
    if (llvm::isa<llvm::Constant>(&v) && entry_fn && !entry_fn->empty() &&
        !entry_fn->getEntryBlock().empty()) {
      auto sit = sites.find(&entry_fn->getEntryBlock().front());
      if (sit == sites.end())
        return false;
      out_site = sit->second;
      out_point = Point::Before;
      return true;
    }
    return false;
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
                                  const CallContext &context, Point point,
                                  ContextMatch match) {
  return m_impl->analysis->pointsTo(m_impl->queryValue(value),
                                    m_impl->querySite(site),
                                    m_impl->context(context), point, match);
}

QueryResult BootstrapAA::pointsToAllContexts(const llvm::Value &value,
                                             const llvm::Instruction &site,
                                             Point point) {
  return m_impl->analysis->pointsToAllContexts(m_impl->queryValue(value),
                                               m_impl->querySite(site), point);
}

BootstrapAA::LLVMAliasResult
BootstrapAA::aliases(const llvm::Value &value, const llvm::Instruction &site,
                     const CallContext &context, Point point,
                     ContextMatch match) {
  return m_impl->translateAliases(m_impl->analysis->aliases(
      m_impl->queryValue(value), m_impl->querySite(site),
      m_impl->context(context), point, match));
}

BootstrapAA::LLVMAliasResult
BootstrapAA::aliasesAllContexts(const llvm::Value &value,
                                const llvm::Instruction &site, Point point) {
  return m_impl->translateAliases(m_impl->analysis->aliasesAllContexts(
      m_impl->queryValue(value), m_impl->querySite(site), point));
}

QueryResult BootstrapAA::objectPointsTo(Id object,
                                        const llvm::Instruction &site,
                                        const CallContext &context, Point point,
                                        ContextMatch match) {
  return m_impl->analysis->objectPointsTo(object, m_impl->querySite(site),
                                          m_impl->context(context), point,
                                          match);
}

QueryResult BootstrapAA::objectPointsToAllContexts(
    Id object, const llvm::Instruction &site, Point point) {
  return m_impl->analysis->objectPointsToAllContexts(
      object, m_impl->querySite(site), point);
}

BootstrapAA::LLVMAliasResult
BootstrapAA::objectAliases(Id object, const llvm::Instruction &site,
                           const CallContext &context, Point point,
                           ContextMatch match) {
  return m_impl->translateAliases(m_impl->analysis->objectAliases(
      object, m_impl->querySite(site), m_impl->context(context), point, match));
}

BootstrapAA::LLVMAliasResult
BootstrapAA::objectAliasesAllContexts(Id object, const llvm::Instruction &site,
                                      Point point) {
  return m_impl->translateAliases(m_impl->analysis->objectAliasesAllContexts(
      object, m_impl->querySite(site), point));
}

void BootstrapAA::precomputeAll() { m_impl->analysis->precomputeAll(); }

void BootstrapAA::precomputeForValues(
    const std::vector<const llvm::Value *> &targets) {
  std::vector<Id> ids;
  ids.reserve(targets.size());
  for (const llvm::Value *v : targets) {
    if (!v)
      throw std::invalid_argument("BootstrapAA: null value in precompute");
    ids.push_back(m_impl->queryValue(*v));
  }
  m_impl->analysis->precomputeForValues(ids);
}

std::vector<std::size_t>
BootstrapAA::clustersFor(const llvm::Value &value) const {
  return m_impl->analysis->clustersFor(m_impl->queryValue(value));
}

std::vector<std::vector<const llvm::Value *>> BootstrapAA::clusters() const {
  const auto &raw = m_impl->analysis->clusters();
  std::vector<std::vector<const llvm::Value *>> out;
  out.reserve(raw.size());
  for (const auto &c : raw) {
    std::vector<const llvm::Value *> vals;
    vals.reserve(c.size());
    for (Id id : c)
      if (id < m_impl->value_to_llvm.size() && m_impl->value_to_llvm[id])
        vals.push_back(m_impl->value_to_llvm[id]);
    out.push_back(std::move(vals));
  }
  return out;
}

bool BootstrapAA::mayAlias(const llvm::Value &lhs, const llvm::Value &rhs,
                           const llvm::Instruction &site,
                           const CallContext &context, ContextMatch match) {
  return m_impl->analysis->mayAlias(
      m_impl->queryValue(lhs), m_impl->queryValue(rhs), m_impl->querySite(site),
      m_impl->context(context), match);
}

bool BootstrapAA::mayAliasAcrossSites(const llvm::Value &lhs,
                                      const llvm::Instruction &lhs_site,
                                      const llvm::Value &rhs,
                                      const llvm::Instruction &rhs_site,
                                      Point lhs_point, Point rhs_point) {
  return m_impl->analysis->mayAliasAcrossSites(
      m_impl->queryValue(lhs), m_impl->querySite(lhs_site),
      m_impl->queryValue(rhs), m_impl->querySite(rhs_site), lhs_point,
      rhs_point);
}

bool BootstrapAA::mayAliasAcrossSites(
    const llvm::Value &lhs, const llvm::Instruction &lhs_site,
    const CallContext &lhs_context, const llvm::Value &rhs,
    const llvm::Instruction &rhs_site, const CallContext &rhs_context,
    Point lhs_point, Point rhs_point, ContextMatch lhs_match,
    ContextMatch rhs_match) {
  return m_impl->analysis->mayAliasAcrossSites(
      m_impl->queryValue(lhs), m_impl->querySite(lhs_site),
      m_impl->context(lhs_context), m_impl->queryValue(rhs),
      m_impl->querySite(rhs_site), m_impl->context(rhs_context), lhs_point,
      rhs_point, lhs_match, rhs_match);
}

AliasKind BootstrapAA::queryAlias(const llvm::Value &lhs,
                                  std::uint64_t lhs_size,
                                  const llvm::Instruction &lhs_site,
                                  const llvm::Value &rhs,
                                  std::uint64_t rhs_size,
                                  const llvm::Instruction &rhs_site,
                                  Point lhs_point, Point rhs_point) {
  return m_impl->analysis->queryAlias(
      m_impl->queryValue(lhs), lhs_size, m_impl->querySite(lhs_site),
      m_impl->queryValue(rhs), rhs_size, m_impl->querySite(rhs_site), lhs_point,
      rhs_point);
}

AliasKind BootstrapAA::queryAlias(const llvm::Value &lhs,
                                  std::uint64_t lhs_size,
                                  const llvm::Instruction &lhs_site,
                                  const CallContext &lhs_context,
                                  const llvm::Value &rhs,
                                  std::uint64_t rhs_size,
                                  const llvm::Instruction &rhs_site,
                                  const CallContext &rhs_context,
                                  Point lhs_point, Point rhs_point,
                                  ContextMatch lhs_match,
                                  ContextMatch rhs_match) {
  return m_impl->analysis->queryAlias(
      m_impl->queryValue(lhs), lhs_size, m_impl->querySite(lhs_site),
      m_impl->context(lhs_context), m_impl->queryValue(rhs), rhs_size,
      m_impl->querySite(rhs_site), m_impl->context(rhs_context), lhs_point,
      rhs_point, lhs_match, rhs_match);
}

bool BootstrapAA::mayAliasGlobal(const llvm::Value &lhs,
                                 const llvm::Value &rhs) {
  auto lit = m_impl->values.find(&lhs);
  auto rit = m_impl->values.find(&rhs);
  if (lit == m_impl->values.end() || rit == m_impl->values.end())
    return true;
  Id lsite = INVALID, rsite = INVALID;
  Point lpt = Point::Before, rpt = Point::Before;
  if (!m_impl->defaultQuerySite(lhs, lsite, lpt) ||
      !m_impl->defaultQuerySite(rhs, rsite, rpt))
    return true;
  return m_impl->analysis->mayAliasAcrossSites(lit->second, lsite, rit->second,
                                               rsite, lpt, rpt);
}

bool BootstrapAA::mustAlias(const llvm::Value &lhs, const llvm::Value &rhs,
                            const llvm::Instruction &site,
                            const CallContext &context, ContextMatch match) {
  return m_impl->analysis->mustAlias(
      m_impl->queryValue(lhs), m_impl->queryValue(rhs), m_impl->querySite(site),
      m_impl->context(context), match);
}

bool BootstrapAA::mayNull(const llvm::Value &value,
                          const llvm::Instruction &site, Point point) {
  return m_impl->analysis->mayNull(m_impl->queryValue(value),
                                   m_impl->querySite(site), point);
}

bool BootstrapAA::mayNull(const llvm::Value &value,
                          const llvm::Instruction &site,
                          const CallContext &context, Point point,
                          ContextMatch match) {
  return m_impl->analysis->mayNull(m_impl->queryValue(value),
                                   m_impl->querySite(site),
                                   m_impl->context(context), point, match);
}

BootstrapAA::LLVMCallTargets
BootstrapAA::callTargets(const llvm::CallBase &call) {
  return m_impl->translateCallTargets(
      m_impl->analysis->callTargets(m_impl->querySite(call)));
}

BootstrapAA::LLVMCallTargets
BootstrapAA::callTargets(const llvm::CallBase &call, const CallContext &context,
                         ContextMatch match) {
  return m_impl->translateCallTargets(m_impl->analysis->callTargets(
      m_impl->querySite(call), m_impl->context(context), match));
}

std::vector<const llvm::CallBase *>
BootstrapAA::callersOf(const llvm::Function &function) const {
  std::vector<const llvm::CallBase *> out;
  for (Id cs : m_impl->analysis->callersOf(m_impl->queryFunction(function))) {
    if (cs < m_impl->site_to_llvm.size() && m_impl->site_to_llvm[cs]) {
      if (const auto *cb =
              llvm::dyn_cast<llvm::CallBase>(m_impl->site_to_llvm[cs]))
        out.push_back(cb);
    }
  }
  return out;
}

bool BootstrapAA::isRecursiveFunction(const llvm::Function &function) const {
  return m_impl->analysis->isRecursiveFunction(m_impl->queryFunction(function));
}

ModRefResult BootstrapAA::functionModRef(const llvm::Function &function) {
  return m_impl->analysis->functionModRef(m_impl->queryFunction(function));
}

ModRefResult BootstrapAA::callModRef(const llvm::CallBase &call) {
  return m_impl->analysis->callModRef(m_impl->querySite(call));
}

ModRefResult BootstrapAA::callModRef(const llvm::CallBase &call,
                                     const CallContext &context,
                                     ContextMatch match) {
  return m_impl->analysis->callModRef(m_impl->querySite(call),
                                      m_impl->context(context), match);
}

BootstrapAA::LLVMReversePointsToResult
BootstrapAA::reversePointsTo(Id object) {
  ReversePointsToResult raw = m_impl->analysis->reversePointsTo(object);
  LLVMReversePointsToResult out;
  out.memory_objects = std::move(raw.memory_objects);
  out.may_be_pointed_by_unknown = raw.may_be_pointed_by_unknown;
  for (Id vid : raw.values) {
    if (vid < m_impl->value_to_llvm.size() && m_impl->value_to_llvm[vid])
      out.values.push_back(m_impl->value_to_llvm[vid]);
  }
  return out;
}

bool BootstrapAA::hasValue(const llvm::Value &value) const {
  return m_impl->values.count(&value) != 0;
}

bool BootstrapAA::hasSite(const llvm::Instruction &site) const {
  return m_impl->sites.count(&site) != 0;
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
  return m_impl->analysis->objectInfo(object);
}

const Statistics &BootstrapAA::statistics() const {
  return m_impl->analysis->statistics();
}

const SteensgaardHierarchy &BootstrapAA::hierarchy() const {
  return m_impl->analysis->hierarchy();
}

} // namespace bootstrap
} // namespace lotus
