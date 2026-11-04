#include "Verification/TraceAbstraction/TransitionFormula.h"

#include "Verification/TraceAbstraction/LinearIntegerSemantics.h"

#include <algorithm>
#include <limits>
#include <unordered_map>

#include <llvm/ADT/SmallString.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

namespace lotus::verification::traceabstraction {
namespace {
[[noreturn]] void unsupported(const std::string &why) {
  throw std::invalid_argument(why);
}

} // namespace

struct TransitionFormulaBuilder::Impl {
  ProgramAutomaton program;
  nwa::Context budget;
  std::shared_ptr<z3::context> owner = std::make_shared<z3::context>();
  z3::context &ctx = *owner;
  z3::func_decl pointerConstructor{ctx};
  z3::func_decl_vector pointerFields{ctx};
  z3::expr_vector in{ctx}, out{ctx}, hier{ctx};
  std::vector<StateVariable> vars;
  std::unordered_map<const llvm::Value *, unsigned> slots;
  std::map<nwa::Letter, TransitionFormula> cache;
  const llvm::Module &module;
  const llvm::DataLayout &layout;
  unsigned bits;
  bool memory = false;
  std::vector<const llvm::GlobalVariable *> globals;
  std::unordered_map<const llvm::GlobalVariable *, unsigned> globalIds;
  unsigned bytes = 0, initialized = 0, pointerData = 0, pointerTags = 0;
  unsigned sizes = 0, alive = 0, readonly = 0, owners = 0, active = 0;
  unsigned nextObject = 0, frame = 0, nextFrame = 0;
  unsigned byteEpoch = 0, epochs = 0, zeroObjects = 0;

  struct Effect {
    z3::expr guard, defined;
    std::map<unsigned, z3::expr> writes;
    std::set<unsigned> havoc;
    explicit Effect(z3::context &c)
        : guard(c.bool_val(true)), defined(c.bool_val(true)) {}
    void set(unsigned slot, const z3::expr &rhs) {
      writes.insert_or_assign(slot, rhs);
    }
  };

  unsigned add(const std::string &name, const z3::sort &sort,
               const llvm::Value *v = nullptr, bool shared = false) {
    unsigned slot = vars.size();
    if (v)
      slots.emplace(v, slot);
    vars.push_back({v, name, shared});
    in.push_back(ctx.constant(("v" + std::to_string(slot)).c_str(), sort));
    out.push_back(ctx.constant(("w" + std::to_string(slot)).c_str(), sort));
    hier.push_back(ctx.constant(("h" + std::to_string(slot)).c_str(), sort));
    return slot;
  }
  z3::sort sort(llvm::Type *type) {
    if (type->isIntegerTy())
      return ctx.int_sort();
    if (type->isPointerTy() && type->getPointerAddressSpace() == 0)
      return pointerConstructor
          .range(); // Ultimate MemoryPointer2D: (base, offset)
    unsupported("unsupported scalar type or pointer address space");
  }
  void addValue(const llvm::Value *v, llvm::Type *type) {
    if (type->isIntegerTy() || type->isPointerTy())
      add(v->hasName() ? v->getName().str() : "value", sort(type), v);
  }
  Impl(const ProgramAutomaton &p, nwa::Context b)
      : program(p), budget(b), module(*p.functions.at(0)->getParent()),
        layout(module.getDataLayout()), bits(layout.getPointerSizeInBits(0)) {
    const char *names[] = {"base", "offset"};
    z3::sort fields[] = {ctx.int_sort(), ctx.int_sort()};
    pointerConstructor =
        ctx.tuple_sort("Pointer", 2, names, fields, pointerFields);
    for (auto *f : program.functions) {
      budget->check();
      addValue(f, f->getReturnType());
      for (const auto &arg : f->args()) {
        // These contracts need separate entry assumptions or implicit ABI
        // copies. Treating byval as an ordinary alias is unsound.
        for (auto attribute :
             {llvm::Attribute::ByVal, llvm::Attribute::InAlloca,
              llvm::Attribute::Preallocated, llvm::Attribute::NonNull,
              llvm::Attribute::NoAlias, llvm::Attribute::Dereferenceable,
              llvm::Attribute::DereferenceableOrNull})
          if (arg.hasAttribute(attribute))
            unsupported(
                "parameter ABI/alias contract requires an explicit model");
        addValue(&arg, arg.getType());
        memory |= arg.getType()->isPointerTy();
      }
      for (const auto &bb : *f)
        for (const auto &i : bb) {
          addValue(&i, i.getType());
          memory |= i.getType()->isPointerTy() ||
                    llvm::isa<llvm::LoadInst>(i) ||
                    llvm::isa<llvm::StoreInst>(i);
        }
    }
    for (const auto &g : module.globals()) {
      if (g.getName().startswith("llvm.")) {
        if (g.getName() == "llvm.global_ctors" &&
            (!g.hasInitializer() || !g.getInitializer()->isNullValue()))
          unsupported(
              "global constructors require explicit ICFG initialization");
        continue;
      }
      if (g.isThreadLocal() || g.getAddressSpace() != 0)
        unsupported("thread-local/non-default-address-space global");
      globalIds.emplace(&g, globals.size() + 1);
      globals.push_back(&g);
      memory = true;
    }
    if (memory) {
      auto index = ctx.int_sort();
      auto array = [&](const z3::sort &range) {
        return ctx.array_sort(index, range);
      };
      auto byteArray = [&](const z3::sort &range) {
        return ctx.array_sort(pointerConstructor.range(), range);
      };
      bytes = add("memory.bytes", byteArray(ctx.int_sort()), nullptr, true);
      initialized =
          add("memory.initialized", byteArray(ctx.bool_sort()), nullptr, true);
      pointerData = add("memory.pointers",
                        byteArray(pointerConstructor.range()), nullptr, true);
      pointerTags =
          add("memory.pointerBytes", byteArray(ctx.int_sort()), nullptr, true);
      byteEpoch = add("memory.byteEpoch", byteArray(index), nullptr, true);
      epochs = add("memory.epoch", array(index), nullptr, true);
      zeroObjects =
          add("memory.zeroInitialized", array(ctx.bool_sort()), nullptr, true);
      sizes = add("memory.length", array(index), nullptr, true);
      alive = add("memory.valid", array(ctx.bool_sort()), nullptr, true);
      readonly = add("memory.readonly", array(ctx.bool_sort()), nullptr, true);
      owners = add("memory.owner", array(index), nullptr, true);
      active =
          add("memory.activeFrames", array(ctx.bool_sort()), nullptr, true);
      nextObject = add("memory.nextBase", index, nullptr, true);
      nextFrame = add("memory.nextFrame", index, nullptr, true);
      frame = add("frame", index);
    }
  }
  z3::expr number(uint64_t v) { return ctx.int_val(v); }
  z3::expr null() { return ptr(number(0), number(0)); }
  z3::expr base(const z3::expr &p) { return pointerFields[0](p); }
  z3::expr offset(const z3::expr &p) { return pointerFields[1](p); }
  z3::expr ptr(const z3::expr &b, const z3::expr &o) {
    return pointerConstructor(b, o);
  }
  z3::expr constant(const llvm::APInt &v) {
    return lia::numeral(ctx, v, v.getBitWidth() != 1);
  }
  z3::expr unsignedOperand(const llvm::Value *v) {
    return lia::unsignedValue(value(v), v->getType()->getIntegerBitWidth());
  }
  z3::expr get(const Effect &e, unsigned slot) {
    auto it = e.writes.find(slot);
    return it == e.writes.end() ? in[slot] : it->second;
  }
  z3::expr address(const z3::expr &p, unsigned byte) {
    return ptr(base(p), offset(p) + number(byte));
  }
  z3::expr read(const z3::expr &a, const z3::expr &p, unsigned byte = 0) {
    return z3::select(a, address(p, byte));
  }
  z3::expr put(const z3::expr &a, const z3::expr &p, unsigned byte,
               const z3::expr &v) {
    return z3::store(a, address(p, byte), v);
  }
  z3::expr written(const z3::expr &p, unsigned j) {
    return read(in[byteEpoch], p, j) == z3::select(in[epochs], base(p));
  }
  z3::expr byteValid(const z3::expr &p, unsigned j) {
    return z3::ite(written(p, j), read(in[initialized], p, j),
                   z3::select(in[zeroObjects], base(p)));
  }
  z3::expr effective(unsigned slot, const z3::expr &p, unsigned j = 0) {
    auto v = read(in[slot], p, j);
    return z3::ite(written(p, j), v, slot == pointerData ? null() : number(0));
  }
  uint64_t allocationSize(llvm::Type *type) {
    if (!type->isSized())
      unsupported("unsized memory object");
    auto size = layout.getTypeAllocSize(type);
    if (size.isScalable())
      unsupported("scalable memory object");
    return size.getFixedValue();
  }
  unsigned storeSize(llvm::Type *type) {
    if (!type->isIntegerTy() && !type->isPointerTy())
      unsupported("memory access requires an integer or pointer scalar");
    auto n = layout.getTypeStoreSize(type);
    if (n.isScalable() || n.getFixedValue() > 4096)
      unsupported("unsupported scalar access size");
    return n.getFixedValue();
  }

  z3::expr gep(const llvm::GEPOperator &g, Effect *effect) {
    if (layout.getIndexSizeInBits(g.getPointerAddressSpace()) != bits)
      unsupported("GEP index width differs from pointer width");
    auto p = value(g.getPointerOperand());
    auto raw = number(0), length = number(0);
    if (effect)
      length = z3::select(in[sizes], base(p));
    else {
      auto object = base(p).simplify();
      uint64_t id = 0;
      if (!object.is_numeral_u64(id) || id > globals.size())
        unsupported("constant GEP requires a global or null base");
      if (id)
        length = number(allocationSize(globals[id - 1]->getValueType()));
    }
    auto defined =
        base(p) != number(0) && offset(p) >= 0 && offset(p) <= length;
    auto allZero = ctx.bool_val(true);
    for (auto it = llvm::gep_type_begin(&g), end = llvm::gep_type_end(&g);
         it != end; ++it) {
      auto delta = number(0);
      if (auto *st = it.getStructTypeOrNull()) {
        auto *field = llvm::cast<llvm::ConstantInt>(it.getOperand());
        allZero = allZero && ctx.bool_val(field->isZero());
        delta = number(layout.getStructLayout(st)->getElementOffset(
            field->getZExtValue()));
      } else {
        unsigned width = it.getOperand()->getType()->getIntegerBitWidth();
        auto original = value(it.getOperand());
        allZero = allZero && original == number(0);
        if (width == 1)
          original = lia::signedValue(original, 1);
        auto index = width > bits ? lia::normalize(original, bits) : original;
        defined = defined && index == original;
        delta = index * number(allocationSize(it.getIndexedType()));
      }
      raw = raw + delta;
      defined = defined && lia::range(delta, bits) && lia::range(raw, bits) &&
                offset(p) + raw >= 0 && offset(p) + raw <= length;
    }
    auto result = ptr(base(p), lia::unsignedValue(offset(p) + raw, bits));
    if (g.isInBounds()) {
      defined = (allZero || defined).simplify();
      if (effect)
        effect->defined = effect->defined && defined;
      else if (!defined.is_true())
        unsupported("poisoned inbounds constant GEP");
    }
    return result;
  }
  z3::expr value(const llvm::Value *v) {
    if (auto *c = llvm::dyn_cast<llvm::ConstantInt>(v))
      return constant(c->getValue());
    if (llvm::isa<llvm::ConstantPointerNull>(v))
      return null();
    if (auto *g = llvm::dyn_cast<llvm::GlobalVariable>(v)) {
      auto found = globalIds.find(g);
      if (found == globalIds.end())
        unsupported("unmodeled global address");
      return ptr(number(found->second), number(0));
    }
    if (auto *ce = llvm::dyn_cast<llvm::ConstantExpr>(v)) {
      if (ce->getOpcode() == llvm::Instruction::BitCast)
        return value(ce->getOperand(0));
      if (auto *g = llvm::dyn_cast<llvm::GEPOperator>(ce))
        return gep(*g, nullptr);
      unsupported("unsupported constant expression");
    }
    if (llvm::isa<llvm::Function>(v))
      unsupported("function-pointer values are unsupported");
    auto found = slots.find(v);
    if (found == slots.end())
      unsupported("unsupported operand (undef, poison or non-scalar)");
    return in[found->second];
  }
  z3::expr accessible(const z3::expr &p, const z3::expr &length,
                      bool write = false) {
    auto object = base(p), off = offset(p);
    auto size = z3::select(in[sizes], object);
    auto frameId = z3::select(in[owners], object);
    auto valid = object != number(0) && z3::select(in[alive], object) &&
                 z3::select(in[active], frameId) && off >= 0 && length >= 0 &&
                 off <= size && length <= size - off;
    if (write)
      valid = valid && !z3::select(in[readonly], object);
    return valid;
  }
  void storeValue(Effect &e, const z3::expr &p, const z3::expr &v,
                  llvm::Type *type, bool check = true) {
    unsigned n = storeSize(type);
    if (check)
      e.defined = e.defined && accessible(p, number(n), true);
    bool pointer = type->isPointerTy();
    auto raw = pointer ? offset(v) : lia::unsignedValue(v, n * 8);
    auto data = get(e, bytes), init = get(e, initialized);
    auto pointers = get(e, pointerData), tags = get(e, pointerTags);
    auto stamps = get(e, byteEpoch);
    for (unsigned j = 0; j < n; ++j) {
      unsigned byte = layout.isLittleEndian() ? j : n - 1 - j;
      data = put(data, p, j,
                 z3::mod(raw / lia::power2(ctx, byte * 8), ctx.int_val(256)));
      init = put(init, p, j, ctx.bool_val(true));
      stamps = put(stamps, p, j, z3::select(get(e, epochs), base(p)));
      pointers = put(pointers, p, j, pointer ? v : null());
      tags = put(tags, p, j, number(pointer ? j + 1 : 0));
    }
    e.set(bytes, data);
    e.set(initialized, init);
    e.set(pointerData, pointers);
    e.set(pointerTags, tags);
    e.set(byteEpoch, stamps);
  }
  z3::expr loadValue(Effect &e, const z3::expr &p, llvm::Type *type) {
    unsigned n = storeSize(type);
    e.defined = e.defined && accessible(p, number(n));
    z3::expr raw = effective(bytes, p, layout.isLittleEndian() ? n - 1 : 0);
    for (unsigned j = 1; j < n; ++j)
      raw = raw * ctx.int_val(256) +
            effective(bytes, p, layout.isLittleEndian() ? n - j - 1 : j);
    auto noPointers = ctx.bool_val(true), tagged = ctx.bool_val(true);
    auto saved = effective(pointerData, p);
    for (unsigned j = 0; j < n; ++j) {
      e.defined = e.defined && byteValid(p, j);
      auto tag = effective(pointerTags, p, j);
      noPointers = noPointers && tag == number(0);
      tagged = tagged && tag == number(j + 1) &&
               effective(pointerData, p, j) == saved;
    }
    if (type->isPointerTy()) {
      auto zero = raw == number(0) && noPointers;
      e.defined = e.defined && (zero || tagged);
      return z3::ite(zero, null(), saved);
    }
    // Reinterpreting pointer representations as integers requires a concrete
    // address/provenance policy. Do not invent one from the object identifier.
    e.defined = e.defined && noPointers;
    return lia::normalize(raw, type->getIntegerBitWidth());
  }
  void newObject(Effect &e, const z3::expr &object, const z3::expr &size,
                 const z3::expr &ownerId, bool zero, bool immutable = false) {
    e.set(sizes, z3::store(get(e, sizes), object, size));
    e.set(alive, z3::store(get(e, alive), object, ctx.bool_val(true)));
    e.set(readonly,
          z3::store(get(e, readonly), object, ctx.bool_val(immutable)));
    e.set(owners, z3::store(get(e, owners), object, ownerId));
    e.set(epochs, z3::store(get(e, epochs), object, number(1)));
    e.set(zeroObjects,
          z3::store(get(e, zeroObjects), object, ctx.bool_val(zero)));
  }
  void initializeConstant(Effect &e, const z3::expr &p,
                          const llvm::Constant *c) {
    budget->check();
    auto *type = c->getType();
    if (c->isNullValue())
      return; // allocation already zero-filled
    if (type->isIntegerTy() || type->isPointerTy()) {
      storeValue(e, p, value(c), type, false);
      return;
    }
    if (auto *st = llvm::dyn_cast<llvm::StructType>(type)) {
      auto *sl = layout.getStructLayout(st);
      for (unsigned j = 0; j < st->getNumElements(); ++j) {
        auto *element = c->getAggregateElement(j);
        if (!element)
          unsupported("unsupported aggregate initializer");
        initializeConstant(
            e, ptr(base(p), offset(p) + number(sl->getElementOffset(j))),
            element);
      }
    } else if (auto *at = llvm::dyn_cast<llvm::ArrayType>(type)) {
      if (at->getNumElements() > 65536)
        unsupported("global initializer resource limit");
      auto stride = allocationSize(at->getElementType());
      for (unsigned j = 0; j < at->getNumElements(); ++j) {
        auto *element = c->getAggregateElement(j);
        if (!element)
          unsupported("unsupported array initializer");
        initializeConstant(e, ptr(base(p), offset(p) + number(j * stride)),
                           element);
      }
    } else
      unsupported("unsupported global initializer");
  }
  void initialize(Effect &e) {
    if (!memory)
      return;
    auto index = ctx.int_sort();
    auto addrSort = pointerConstructor.range();
    e.set(bytes, z3::const_array(addrSort, number(0)));
    e.set(initialized, z3::const_array(addrSort, ctx.bool_val(false)));
    e.set(pointerData, z3::const_array(addrSort, null()));
    e.set(pointerTags, z3::const_array(addrSort, number(0)));
    e.set(byteEpoch, z3::const_array(addrSort, number(0)));
    e.set(epochs, z3::const_array(index, number(0)));
    e.set(zeroObjects, z3::const_array(index, ctx.bool_val(false)));
    e.set(sizes, z3::const_array(index, number(0)));
    e.set(alive, z3::const_array(index, ctx.bool_val(false)));
    e.set(readonly, z3::const_array(index, ctx.bool_val(false)));
    e.set(owners, z3::const_array(index, number(0)));
    auto frames = z3::const_array(index, ctx.bool_val(false));
    e.set(active, z3::store(z3::store(frames, number(0), ctx.bool_val(true)),
                            number(1), ctx.bool_val(true)));
    e.set(frame, number(1));
    e.set(nextFrame, number(2));
    e.set(nextObject, number(globals.size() + 1));
    for (auto *g : globals) {
      if (!g->hasInitializer())
        unsupported("external global needs an initialization contract");
      auto p = value(g);
      newObject(e, base(p), number(allocationSize(g->getValueType())),
                number(0), true, g->isConstant());
      initializeConstant(e, p, g->getInitializer());
    }
  }

  void memoryIntrinsic(Effect &e, const llvm::CallBase &call,
                       llvm::Intrinsic::ID id) {
    if (id == llvm::Intrinsic::lifetime_start ||
        id == llvm::Intrinsic::lifetime_end) {
      auto p = value(call.getArgOperand(1));
      auto length = value(call.getArgOperand(0));
      e.defined = e.defined && base(p) != number(0) && offset(p) == number(0) &&
                  z3::select(in[owners], base(p)) == in[frame] &&
                  (length == ctx.int_val(-1) ||
                   length == z3::select(in[sizes], base(p)));
      e.set(alive,
            z3::store(in[alive], base(p),
                      ctx.bool_val(id == llvm::Intrinsic::lifetime_start)));
      if (id == llvm::Intrinsic::lifetime_start) {
        auto epoch = z3::select(in[epochs], base(p));

        e.set(epochs, z3::store(in[epochs], base(p), epoch + number(1)));
        e.set(zeroObjects,
              z3::store(in[zeroObjects], base(p), ctx.bool_val(false)));
      }
      return;
    }
    auto *length = llvm::dyn_cast<llvm::ConstantInt>(call.getArgOperand(2));
    if (!length || length->getValue().getLimitedValue() > 4096)
      unsupported(
          "memory intrinsic requires a bounded constant length (<=4096 bytes)");
    auto *vol = llvm::dyn_cast<llvm::ConstantInt>(call.getArgOperand(3));
    if (!vol || !vol->isZero())
      unsupported("volatile memory intrinsic");
    unsigned n = length->getZExtValue();
    if (!n)
      return;
    auto dest = value(call.getArgOperand(0));
    e.defined = e.defined && accessible(dest, number(n), true);
    auto data = in[bytes], init = in[initialized], pointers = in[pointerData],
         tags = in[pointerTags];
    auto stamps = in[byteEpoch];
    if (id == llvm::Intrinsic::memset) {
      auto v = lia::unsignedValue(value(call.getArgOperand(1)), 8);
      for (unsigned j = 0; j < n; ++j) {
        data = put(data, dest, j, v);
        init = put(init, dest, j, ctx.bool_val(true));
        stamps = put(stamps, dest, j, z3::select(in[epochs], base(dest)));
        pointers = put(pointers, dest, j, null());
        tags = put(tags, dest, j, number(0));
      }
    } else {
      auto src = value(call.getArgOperand(1));
      e.defined = e.defined && accessible(src, number(n));
      if (id == llvm::Intrinsic::memcpy)
        e.defined = e.defined && (base(dest) != base(src) ||
                                  offset(dest) + number(n) <= offset(src) ||
                                  offset(src) + number(n) <= offset(dest));
      for (unsigned j = 0; j < n; ++j) {
        data = put(data, dest, j, effective(bytes, src, j));
        init = put(init, dest, j, byteValid(src, j));
        pointers = put(pointers, dest, j, effective(pointerData, src, j));
        tags = put(tags, dest, j, effective(pointerTags, src, j));
        stamps = put(stamps, dest, j, z3::select(in[epochs], base(dest)));
      }
    }
    e.set(bytes, data);
    e.set(initialized, init);
    e.set(pointerData, pointers);
    e.set(pointerTags, tags);
    e.set(byteEpoch, stamps);
  }

  TransitionFormula encode(nwa::Letter letter) {
    budget->check();
    const auto &s = program.statements.at(letter);
    const auto *i = s.instruction;
    Effect e(ctx);
    auto assign = [&](const llvm::Value *v, const z3::expr &rhs) {
      auto found = slots.find(v);
      if (found == slots.end())
        unsupported("unsupported assignment type");
      e.set(found->second, rhs);
    };
    auto truth = [&](const llvm::Value *v) {
      auto x = value(v);
      return x != number(0);
    };
    if (i && !s.error && i->getMetadata(llvm::LLVMContext::MD_range))
      unsupported("range metadata requires poison-aware semantics");
    if (auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(i))
      if (!s.error && call->hasOperandBundles())
        unsupported("call operand bundles are unsupported");
    if (s.initialize) {
      initialize(e);
    } else if (s.error || !i) {
    } else if (s.kind == nwa::LetterKind::Call) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(i);
      if (!call || !s.callee || s.callee->isVarArg() ||
          call->arg_size() != s.callee->arg_size())
        unsupported("unsupported call signature");
      unsigned n = 0;
      for (const auto &arg : s.callee->args())
        assign(&arg, value(call->getArgOperand(n++)));
      if (memory) {

        e.set(frame, in[nextFrame]);
        e.set(nextFrame, in[nextFrame] + number(1));
        e.set(active, z3::store(in[active], in[nextFrame], ctx.bool_val(true)));
      }
    } else if (s.kind == nwa::LetterKind::Return) {
      // Ultimate's return substitution: shared state comes from the linear
      // predecessor, caller locals from the hierarchical predecessor.
      for (unsigned j = 0; j < vars.size(); ++j)
        if (!vars[j].shared)
          e.set(j, hier[j]);
      if (!i->getType()->isVoidTy())
        assign(i, in[slots.at(s.callee)]);
      if (memory)
        e.set(active, z3::store(in[active], in[frame], ctx.bool_val(false)));
    } else if (auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(i)) {
      auto a = value(i->getOperand(0)), b = value(i->getOperand(1));
      unsigned width = i->getType()->getIntegerBitWidth();
      if (width == 1) {
        a = lia::signedValue(a, width);
        b = lia::signedValue(b, width);
      }
      auto ua = lia::unsignedValue(a, width), ub = lia::unsignedValue(b, width);
      auto raw = number(0), unsignedRaw = number(0);
      auto *constantLeft = llvm::dyn_cast<llvm::ConstantInt>(i->getOperand(0));
      auto *constantRight = llvm::dyn_cast<llvm::ConstantInt>(i->getOperand(1));
      switch (i->getOpcode()) {
      case llvm::Instruction::Add:
        raw = a + b;
        unsignedRaw = ua + ub;
        break;
      case llvm::Instruction::Sub:
        raw = a - b;
        unsignedRaw = ua - ub;
        break;
      case llvm::Instruction::Mul:
        if (!constantLeft && !constantRight)
          unsupported("symbolic multiplication is outside linear arithmetic");
        raw = a * b;
        unsignedRaw = ua * ub;
        break;
      case llvm::Instruction::UDiv:
      case llvm::Instruction::URem:
      case llvm::Instruction::SDiv:
      case llvm::Instruction::SRem: {
        if (!constantRight)
          unsupported("symbolic divisor is outside linear arithmetic");
        bool sign = i->getOpcode() == llvm::Instruction::SDiv ||
                    i->getOpcode() == llvm::Instruction::SRem;
        auto lhs = sign ? a : ua, rhs = sign ? b : ub;
        e.defined = e.defined && rhs != number(0);
        if (sign)
          e.defined = e.defined && !(a == -lia::power2(ctx, width - 1) &&
                                     b == ctx.int_val(-1));
        auto absL = z3::ite(lhs < 0, -lhs, lhs),
             absR = z3::ite(rhs < 0, -rhs, rhs);
        auto quotient =
            z3::ite((lhs < 0) != (rhs < 0), -(absL / absR), absL / absR);
        raw = i->getOpcode() == llvm::Instruction::UDiv ||
                      i->getOpcode() == llvm::Instruction::SDiv
                  ? quotient
                  : lhs - quotient * rhs;
        if ((i->getOpcode() == llvm::Instruction::UDiv ||
             i->getOpcode() == llvm::Instruction::SDiv) &&
            binary->isExact())
          e.defined = e.defined && lhs == quotient * rhs;
        break;
      }
      case llvm::Instruction::Shl:
      case llvm::Instruction::LShr:
      case llvm::Instruction::AShr: {
        if (!constantRight)
          unsupported("symbolic shift is outside linear arithmetic");
        uint64_t shift = constantRight->getValue().getLimitedValue();
        if (shift >= width) {
          e.defined = ctx.bool_val(false);
          break;
        }
        auto factor = lia::power2(ctx, shift);
        if (i->getOpcode() == llvm::Instruction::Shl) {
          raw = a * factor;
          unsignedRaw = ua * factor;
        } else {
          auto operand = i->getOpcode() == llvm::Instruction::LShr ? ua : a;
          raw = operand / factor;
          if (binary->isExact())
            e.defined = e.defined && z3::mod(operand, factor) == number(0);
        }
        break;
      }
      case llvm::Instruction::And:
      case llvm::Instruction::Or:
      case llvm::Instruction::Xor:
        for (unsigned bit = 0; bit < width; ++bit) {
          auto weight = lia::power2(ctx, bit);
          auto x = z3::mod(ua / weight, ctx.int_val(2)) == number(1);
          auto y = z3::mod(ub / weight, ctx.int_val(2)) == number(1);
          auto set = i->getOpcode() == llvm::Instruction::And  ? x && y
                     : i->getOpcode() == llvm::Instruction::Or ? x || y
                                                               : x != y;
          raw = raw + z3::ite(set, weight, number(0));
        }
        break;
      default:
        unsupported("unsupported integer operation");
      }
      if (auto *op = llvm::dyn_cast<llvm::OverflowingBinaryOperator>(binary)) {
        if (op->hasNoSignedWrap())
          e.defined = e.defined && raw >= -lia::power2(ctx, width - 1) &&
                      raw < lia::power2(ctx, width - 1);
        if (op->hasNoUnsignedWrap())
          e.defined = e.defined && unsignedRaw >= 0 &&
                      unsignedRaw < lia::power2(ctx, width);
      }
      auto *overflowing =
          llvm::dyn_cast<llvm::OverflowingBinaryOperator>(binary);
      assign(i, width > 1 && overflowing && overflowing->hasNoSignedWrap()
                    ? raw
                    : lia::normalize(raw, width));
    } else if (auto *cmp = llvm::dyn_cast<llvm::ICmpInst>(i)) {
      auto a = value(i->getOperand(0)), b = value(i->getOperand(1));
      bool pointer = i->getOperand(0)->getType()->isPointerTy();
      if (pointer) {
        if (cmp->isSigned())
          unsupported("signed pointer comparisons require native addresses");
        if (cmp->isEquality()) {
          auto interior = [&](const z3::expr &p) {
            return p == null() || accessible(p, number(1));
          };
          e.defined =
              e.defined && (base(a) == base(b) || a == null() || b == null() ||
                            (interior(a) && interior(b)));
        } else {
          e.defined = e.defined && base(a) == base(b);
          a = offset(a);
          b = offset(b);
        }
      } else if (cmp->isUnsigned()) {
        auto width = i->getOperand(0)->getType()->getIntegerBitWidth();
        a = lia::unsignedValue(a, width);
        b = lia::unsignedValue(b, width);
      } else if (cmp->isSigned()) {
        auto width = i->getOperand(0)->getType()->getIntegerBitWidth();
        if (width == 1) {
          a = lia::signedValue(a, width);
          b = lia::signedValue(b, width);
        }
      }
      auto condition = ctx.bool_val(false);
      switch (cmp->getPredicate()) {
      case llvm::CmpInst::ICMP_EQ:
        condition = a == b;
        break;
      case llvm::CmpInst::ICMP_NE:
        condition = a != b;
        break;
      case llvm::CmpInst::ICMP_SGT:
      case llvm::CmpInst::ICMP_UGT:
        condition = a > b;
        break;
      case llvm::CmpInst::ICMP_SGE:
      case llvm::CmpInst::ICMP_UGE:
        condition = a >= b;
        break;
      case llvm::CmpInst::ICMP_SLT:
      case llvm::CmpInst::ICMP_ULT:
        condition = a < b;
        break;
      case llvm::CmpInst::ICMP_SLE:
      case llvm::CmpInst::ICMP_ULE:
        condition = a <= b;
        break;
      default:
        unsupported("unsupported comparison");
      }
      assign(i, z3::ite(condition, number(1), number(0)));
    } else if (auto *br = llvm::dyn_cast<llvm::BranchInst>(i)) {
      if (!s.successor)
        unsupported("branch has no successor");
      if (br->isConditional()) {
        e.guard = ctx.bool_val(false);
        if (br->getSuccessor(0) == s.successor)
          e.guard = e.guard || truth(br->getCondition());
        if (br->getSuccessor(1) == s.successor)
          e.guard = e.guard || !truth(br->getCondition());
      } else if (br->getSuccessor(0) != s.successor)
        unsupported("invalid ICFG branch");
    } else if (auto *sw = llvm::dyn_cast<llvm::SwitchInst>(i)) {
      auto x = value(sw->getCondition()), fallback = ctx.bool_val(true);
      e.guard = ctx.bool_val(false);
      for (const auto &c : sw->cases()) {
        auto match = x == value(c.getCaseValue());
        fallback = fallback && !match;
        if (c.getCaseSuccessor() == s.successor)
          e.guard = e.guard || match;
      }
      if (sw->getDefaultDest() == s.successor)
        e.guard = e.guard || fallback;
    } else if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(i)) {
      if (ret->getReturnValue())
        assign(ret->getFunction(), value(ret->getReturnValue()));
    } else if (auto *a = llvm::dyn_cast<llvm::AllocaInst>(i)) {
      auto count = unsignedOperand(a->getArraySize());
      auto size = count * number(allocationSize(a->getAllocatedType()));
      e.defined = e.defined && size < lia::power2(ctx, bits);
      newObject(e, in[nextObject], size, in[frame], false);
      assign(i, ptr(in[nextObject], number(0)));
      e.set(nextObject, in[nextObject] + number(1));
    } else if (auto *g = llvm::dyn_cast<llvm::GetElementPtrInst>(i)) {
      assign(i, gep(*llvm::cast<llvm::GEPOperator>(g), &e));
    } else if (auto *load = llvm::dyn_cast<llvm::LoadInst>(i)) {
      if (load->isVolatile() || load->isAtomic())
        unsupported("volatile/atomic load");
      auto p = value(load->getPointerOperand());
      e.defined =
          e.defined &&
          z3::mod(offset(p), number(load->getAlign().value())) == number(0);
      assign(i, loadValue(e, p, load->getType()));
    } else if (auto *store = llvm::dyn_cast<llvm::StoreInst>(i)) {
      if (store->isVolatile() || store->isAtomic())
        unsupported("volatile/atomic store");
      auto p = value(store->getPointerOperand());
      e.defined =
          e.defined &&
          z3::mod(offset(p), number(store->getAlign().value())) == number(0);
      storeValue(e, p, value(store->getValueOperand()),
                 store->getValueOperand()->getType());
    } else if (auto *sel = llvm::dyn_cast<llvm::SelectInst>(i)) {
      assign(i, z3::ite(truth(sel->getCondition()), value(sel->getTrueValue()),
                        value(sel->getFalseValue())));
    } else if (auto *freeze = llvm::dyn_cast<llvm::FreezeInst>(i)) {
      if (llvm::isa<llvm::UndefValue>(freeze->getOperand(0)))
        e.havoc.insert(slots.at(i));
      else
        assign(i, value(freeze->getOperand(0)));
    } else if (auto *cast = llvm::dyn_cast<llvm::CastInst>(i)) {
      auto x = value(cast->getOperand(0));
      switch (cast->getOpcode()) {
      case llvm::Instruction::BitCast:
        if (cast->getSrcTy()->isPointerTy() && cast->getDestTy()->isPointerTy())
          assign(i, x);
        else
          unsupported("unsupported bitcast");
        break;
      case llvm::Instruction::Trunc:
      case llvm::Instruction::ZExt:
      case llvm::Instruction::SExt:
        if (cast->getOpcode() == llvm::Instruction::ZExt)
          x = lia::unsignedValue(x, cast->getSrcTy()->getIntegerBitWidth());
        if (cast->getOpcode() == llvm::Instruction::SExt &&
            cast->getSrcTy()->getIntegerBitWidth() == 1)
          x = lia::signedValue(x, cast->getSrcTy()->getIntegerBitWidth());
        assign(i, lia::normalize(x, cast->getType()->getIntegerBitWidth()));
        break;
      default:
        unsupported(
            "pointer/integer or floating-point cast requires an address model");
      }
    } else if (auto *call = llvm::dyn_cast<llvm::CallBase>(i)) {
      auto *f = llvm::dyn_cast<llvm::Function>(
          call->getCalledOperand()->stripPointerCasts());
      if (!f)
        unsupported("unresolved indirect call");
      auto name = f->getName();
      if (name == "__VERIFIER_assume" || name == "llvm.assume") {
        if (call->arg_size() != 1 || !call->getType()->isVoidTy())
          unsupported("invalid assume signature");
        e.guard = truth(call->getArgOperand(0));
      } else if (name.startswith("__VERIFIER_nondet_") && call->arg_empty() &&
                 call->getType()->isIntegerTy()) {
        e.havoc.insert(slots.at(i));
      } else if (llvm::isa<llvm::DbgInfoIntrinsic>(call)) {
      } else if (f->isIntrinsic() &&
                 (f->getIntrinsicID() == llvm::Intrinsic::memcpy ||
                  f->getIntrinsicID() == llvm::Intrinsic::memmove ||
                  f->getIntrinsicID() == llvm::Intrinsic::memset ||
                  f->getIntrinsicID() == llvm::Intrinsic::lifetime_start ||
                  f->getIntrinsicID() == llvm::Intrinsic::lifetime_end)) {
        memoryIntrinsic(e, *call, f->getIntrinsicID());
      } else if ((name == "malloc" || name == "calloc") &&
                 call->getType()->isPointerTy()) {
        if (call->arg_size() != (name == "calloc" ? 2u : 1u))
          unsupported("invalid allocation signature");
        for (const auto &argument : call->args())
          if (!argument->getType()->isIntegerTy(bits))
            unsupported("allocation size must have size_t width");
        auto size = unsignedOperand(call->getArgOperand(0));
        if (name == "calloc") {
          if (!llvm::isa<llvm::ConstantInt>(call->getArgOperand(0)) &&
              !llvm::isa<llvm::ConstantInt>(call->getArgOperand(1)))
            unsupported("symbolic calloc product is outside linear arithmetic");
          auto n = unsignedOperand(call->getArgOperand(1));
          size = size * n;
        }
        auto result = out[slots.at(i)], object = in[nextObject];
        e.guard = result == null() || (size < lia::power2(ctx, bits) &&
                                       result == ptr(object, number(0)));
        e.havoc.insert(slots.at(i));

        newObject(e, object, size, number(0), name == "calloc");
        for (auto &write : e.writes)
          write.second =
              z3::ite(result == null(), in[write.first], write.second);
        e.set(nextObject,
              z3::ite(result == null(), object, object + number(1)));
      } else if (name == "free" && call->arg_size() == 1) {
        if (!call->getType()->isVoidTy() ||
            !call->getArgOperand(0)->getType()->isPointerTy())
          unsupported("invalid free signature");
        auto p = value(call->getArgOperand(0));
        e.defined =
            e.defined &&
            (p == null() || (base(p) != number(0) && offset(p) == number(0) &&
                             z3::select(in[alive], base(p)) &&
                             z3::select(in[owners], base(p)) == number(0) &&
                             base(p) > number(globals.size())));
        e.set(alive,
              z3::ite(p == null(), in[alive],
                      z3::store(in[alive], base(p), ctx.bool_val(false))));
      } else
        unsupported("unsupported external call: " + name.str());
    } else
      unsupported(std::string("unsupported instruction: ") +
                  i->getOpcodeName());

    if (s.successor)
      for (const auto &phi : s.successor->phis()) {
        int j = phi.getBasicBlockIndex(i->getParent());
        if (j < 0)
          unsupported("missing PHI predecessor");
        assign(&phi, value(phi.getIncomingValue(j)));
      }
    auto relation = e.guard;
    for (unsigned j = 0; j < vars.size(); ++j) {
      auto *v = vars[j].value;
      auto *type = v ? (llvm::isa<llvm::Function>(v)
                            ? llvm::cast<llvm::Function>(v)->getReturnType()
                            : v->getType())
                     : nullptr;
      if (type && type->isIntegerTy())
        relation = relation && lia::range(in[j], type->getIntegerBitWidth()) &&
                   lia::range(out[j], type->getIntegerBitWidth());
    }
    auto guard = relation;
    std::set<unsigned> writes;
    for (unsigned j = 0; j < vars.size(); ++j) {
      if (e.havoc.count(j)) {
        writes.insert(j);
        continue;
      }
      auto found = e.writes.find(j);
      if (found == e.writes.end())
        relation = relation && out[j] == in[j];
      else {
        writes.insert(j);
        relation = relation && z3::implies(e.defined, out[j] == found->second);
      }
    }
    std::set<unsigned> reads, hreads;
    std::unordered_map<unsigned, unsigned> ids, hids;
    for (unsigned j = 0; j < vars.size(); ++j) {
      ids.emplace(in[j].id(), j);
      hids.emplace(hier[j].id(), j);
    }
    std::vector<z3::expr> todo{e.guard, e.defined};
    for (const auto &w : e.writes)
      todo.push_back(w.second);
    std::set<unsigned> seen;
    while (!todo.empty()) {
      auto term = todo.back();
      todo.pop_back();
      if (!seen.insert(term.id()).second)
        continue;
      auto found = ids.find(term.id()), hf = hids.find(term.id());
      if (found != ids.end())
        reads.insert(found->second);
      if (hf != hids.end())
        hreads.insert(hf->second);
      if (term.is_app())
        for (unsigned j = 0; j < term.num_args(); ++j)
          todo.push_back(term.arg(j));
    }
    return {relation.simplify(),  guard.simplify(),    e.guard.simplify(),
            e.defined.simplify(), std::move(e.writes), std::move(reads),
            std::move(hreads),    std::move(writes),   std::move(e.havoc)};
  }
};

TransitionFormulaBuilder::TransitionFormulaBuilder(const ProgramAutomaton &p,
                                                   nwa::Context b)
    : impl_(new Impl(p, b)) {}
TransitionFormulaBuilder::~TransitionFormulaBuilder() = default;
std::shared_ptr<z3::context> TransitionFormulaBuilder::context() const {
  return impl_->owner;
}
const z3::expr_vector &TransitionFormulaBuilder::inputs() const {
  return impl_->in;
}
const z3::expr_vector &TransitionFormulaBuilder::outputs() const {
  return impl_->out;
}
const z3::expr_vector &TransitionFormulaBuilder::hierarchicalInputs() const {
  return impl_->hier;
}
const std::vector<StateVariable> &TransitionFormulaBuilder::variables() const {
  return impl_->vars;
}
const TransitionFormula &TransitionFormulaBuilder::build(nwa::Letter letter) {
  auto found = impl_->cache.find(letter);
  if (found == impl_->cache.end())
    found = impl_->cache.emplace(letter, impl_->encode(letter)).first;
  return found->second;
}
} // namespace lotus::verification::traceabstraction
