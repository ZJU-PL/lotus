#include "Alias/InclusionBased/BootstrapAA/Engine.h"

#include <algorithm>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

using namespace lotus::bootstrap;
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x))                                                                  \
      throw std::runtime_error(std::string(__FILE__) + ":" +                   \
                               std::to_string(__LINE__) + ": " #x);            \
  } while (false)

namespace {
PointsToSet pts(std::initializer_list<Id> objects) {
  PointsToSet result;
  for (Id object : objects)
    result.insert(object);
  return result;
}
struct Fixture {
  Program p;
  Id main;
  Fixture() {
    main = p.addFunction("main");
    p.entry = main;
    p.addBlock(main);
  }
  Id value(std::string name = "v", Id function = INVALID) {
    Value v;
    v.name = std::move(name);
    v.function = function == INVALID ? main : function;
    return p.addValue(std::move(v));
  }
  Id object(std::string name, PointsToSet initial = PointsToSet::top(),
            bool singleton = true, ObjectKind kind = ObjectKind::Global) {
    Object o;
    o.name = std::move(name);
    o.initial = initial;
    o.singleton = singleton;
    o.kind = kind;
    return p.addObject(std::move(o));
  }
  Id addr(Id o) { return p.addConstant(PointsToSet(o)); }
  Id inst(Opcode op, Id dst = INVALID, std::vector<Id> args = {},
          Id f = INVALID, Id b = 0) {
    Instruction i;
    i.opcode = op;
    i.result = dst;
    i.operands = std::move(args);
    return p.append(f == INVALID ? main : f, b, i);
  }
  Id call(Id f, Id dst, std::vector<Id> args, Id caller = INVALID, Id b = 0) {
    Instruction i;
    i.opcode = Opcode::Call;
    i.result = dst;
    i.callee = f;
    i.operands = std::move(args);
    return p.append(caller == INVALID ? main : caller, b, i);
  }
  Id functionObject(Id f) {
    Object o;
    o.kind = ObjectKind::Function;
    o.function = f;
    o.name = p.functions[f].name;
    return p.addObject(std::move(o));
  }
};
Options refined() {
  Options o;
  o.andersen_threshold = 1;
  return o;
}
void sparsePointsToSets() {
  PointsToSet set;
  CHECK(set.insert(2));
  CHECK(set.insert(130));
  CHECK(set.insert(4097));
  CHECK(!set.insert(130));
  CHECK(set.size() == 3);
  CHECK(set.contains(2));
  CHECK(set.contains(130));
  CHECK(set.contains(4097));
  CHECK(!set.contains(4096));
  std::vector<Id> objects(set.begin(), set.end());
  CHECK(objects == std::vector<Id>({2, 130, 4097}));

  PointsToSet joined;
  joined.insert(64);
  joined.insert(4097);
  CHECK(set.join(joined));
  CHECK(set == pts({2, 64, 130, 4097}));
  CHECK(set.intersects(pts({64})));
  CHECK(!set.intersects(pts({65})));
  CHECK(set.join(PointsToSet::top()));
  CHECK(set.isTop());
  CHECK(set.objects().begin() != set.objects().end());
  CHECK(*set.objects().begin() == UNKNOWN);

  // Stress & differential verification of SBO + 256-bit chunked sparse
  // bitvector against `std::set<Id>` across empty, NULL, inline 1/2-word,
  // heap 1-chunk (3+ words), heap multi-chunk, copy/move/reuse, and Top.
  std::vector<PointsToSet> psets(32);
  std::vector<std::set<Id>> refs(32);
  std::uint64_t rng = 0x9E3779B97F4A7C15ULL;
  auto nextRng = [&]() -> std::uint64_t {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
  };
  auto checkMatch = [&](const PointsToSet &s, const std::set<Id> &r) {
    CHECK(s.empty() == r.empty());
    CHECK(s.size() == r.size());
    CHECK(!s.isTop());
    CHECK(s.hasNull() == (r.count(NULL_OBJECT) != 0));
    bool ref_non_null = false;
    for (Id o : r)
      if (o != NULL_OBJECT)
        ref_non_null = true;
    CHECK(s.hasNonNull() == ref_non_null);
    std::vector<Id> got(s.begin(), s.end());
    std::vector<Id> exp(r.begin(), r.end());
    CHECK(got == exp);
  };
  // Explicitly exercise 3 words inside a single 256-bit chunk (bits 2, 65, 130)
  // and inline/heap join edge cases.
  {
    PointsToSet a, b;
    CHECK(a.insert(NULL_OBJECT));
    CHECK(a.hasNull() && !a.hasNonNull() && a.size() == 1);
    CHECK(a.insert(2));
    CHECK(a.insert(65));
    CHECK(a.insert(130)); // spills to 1 HeapChunk with 3 non-zero words
    CHECK(a.size() == 4);
    CHECK(a.contains(NULL_OBJECT) && a.contains(2) && a.contains(65) &&
          a.contains(130) && !a.contains(131));
    CHECK(!a.intersectsNonNull(PointsToSet(NULL_OBJECT)));
    CHECK(a.intersects(PointsToSet(NULL_OBJECT)));
    CHECK(b.insert(130));
    CHECK(b.insert(50000));
    CHECK(a.intersectsNonNull(b));
    CHECK(b.intersectsNonNull(a));
    PointsToSet c(b);
    CHECK(c.join(a)); // inline joining heap
    CHECK(!c.join(a)); // idempotent no-op join
    CHECK(c.size() == 5);
    std::vector<Id> cv(c.begin(), c.end());
    CHECK(cv == std::vector<Id>({NULL_OBJECT, 2, 65, 130, 50000}));
    PointsToSet moved(std::move(c));
    CHECK(c.empty());
    CHECK(moved.size() == 5);
    PointsToSet reused;
    for (Id id : {2u, 65u, 130u, 300u, 700u, 2000u, 9000u})
      reused.insert(id);
    reused = moved; // heap copy-assignment buffer reuse
    CHECK(reused == moved);
    reused.clear();
    CHECK(reused.empty() && !reused.hasNull() && !reused.hasNonNull());
  }
  for (int iter = 0; iter < 1200; ++iter) {
    const std::size_t idx = static_cast<std::size_t>(nextRng() % psets.size());
    const unsigned op = static_cast<unsigned>(nextRng() % 5);
    if (op < 3) {
      Id val = (nextRng() % 10 == 0)
                   ? NULL_OBJECT
                   : static_cast<Id>(2 + (nextRng() % 20000));
      const bool c1 = psets[idx].insert(val);
      const bool c2 = refs[idx].insert(val).second;
      CHECK(c1 == c2);
    } else if (op == 3) {
      const std::size_t src =
          static_cast<std::size_t>(nextRng() % psets.size());
      const bool c1 = psets[idx].join(psets[src]);
      const std::size_t before = refs[idx].size();
      refs[idx].insert(refs[src].begin(), refs[src].end());
      const bool c2 = refs[idx].size() != before;
      CHECK(c1 == c2);
    } else {
      const std::size_t other =
          static_cast<std::size_t>(nextRng() % psets.size());
      bool ref_inter = false;
      bool ref_non_null_inter = false;
      for (Id o : refs[idx]) {
        if (refs[other].count(o)) {
          ref_inter = true;
          if (o != NULL_OBJECT)
            ref_non_null_inter = true;
        }
      }
      CHECK(psets[idx].intersects(psets[other]) == ref_inter);
      CHECK(psets[idx].intersectsNonNull(psets[other]) == ref_non_null_inter);
      CHECK((psets[idx] == psets[other]) == (refs[idx] == refs[other]));
      if (psets[idx] == psets[other]) {
        CHECK(!(psets[idx] < psets[other]) && !(psets[other] < psets[idx]));
      } else {
        CHECK((psets[idx] < psets[other]) != (psets[other] < psets[idx]));
      }
    }
    if (iter % 40 == 0)
      checkMatch(psets[idx], refs[idx]);
  }
  for (std::size_t i = 0; i < psets.size(); ++i)
    checkMatch(psets[i], refs[i]);
}
void hierarchyMetadata() {
  Fixture f;
  Id leaf = f.object("leaf");
  Id cell = f.object("cell", pts({leaf}));
  Id leafAddress = f.addr(leaf), cellAddress = f.addr(cell);
  f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  const auto &hierarchy = aa.hierarchy();
  const Id higher = hierarchy.value_components[cellAddress];
  const Id lower = hierarchy.value_components[leafAddress];
  CHECK(hierarchy.isHigher(higher, lower));
  CHECK(hierarchy.depth[lower] > hierarchy.depth[higher]);
  CHECK(aa.statistics().hierarchy_nodes == hierarchy.successors.size());
  CHECK(aa.statistics().hierarchy_edges != 0);
  CHECK(!aa.statistics().steensgaard_partition_sizes.empty());
}
void strongUpdates() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b");
  Id slot = f.object("slot", pts({a}));
  Id ps = f.addr(slot), pb = f.addr(b), x = f.value(), y = f.value();
  Id first = f.inst(Opcode::Load, x, {ps});
  f.inst(Opcode::Store, INVALID, {ps, pb});
  Id second = f.inst(Opcode::Load, y, {ps});
  f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, first, {}, Point::After).points_to == pts({a}));
  CHECK(aa.pointsTo(y, second, {}, Point::After).points_to == pts({b}));
  CHECK(aa.pointsTo(x, second).points_to == pts({a}));
  CHECK(!aa.mayAlias(x, pb, second));
}
void branchesAndKills() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), c = f.object("c");
  Id slot = f.object("slot", pts({NULL_OBJECT}));
  Id ps = f.addr(slot), pa = f.addr(a), pb = f.addr(b), pc = f.addr(c);
  Id left = f.p.addBlock(f.main), right = f.p.addBlock(f.main),
     merge = f.p.addBlock(f.main);
  f.p.functions[f.main].blocks[0].successors = {{left, {}}, {right, {}}};
  f.inst(Opcode::Store, INVALID, {ps, pa}, f.main, left);
  f.inst(Opcode::Store, INVALID, {ps, pb}, f.main, right);
  f.p.functions[f.main].blocks[left].successors = {{merge, {}}};
  f.p.functions[f.main].blocks[right].successors = {{merge, {}}};
  Id x = f.value(), y = f.value();
  Id first = f.inst(Opcode::Load, x, {ps}, f.main, merge);
  f.inst(Opcode::Store, INVALID, {ps, pc}, f.main, merge);
  Id last = f.inst(Opcode::Load, y, {ps}, f.main, merge);
  f.inst(Opcode::Return, INVALID, {}, f.main, merge);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, first, {}, Point::After).points_to == pts({a, b}));
  CHECK(aa.pointsTo(y, last, {}, Point::After).points_to == pts({c}));
}
void contextsAndReturns() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), pa = f.addr(a), pb = f.addr(b);
  Id identity = f.p.addFunction("identity");
  f.p.addBlock(identity);
  Id parameter = f.value("p", identity);
  f.p.functions[identity].parameters = {parameter};
  Id ret = f.inst(Opcode::Return, INVALID, {parameter}, identity);
  Id x = f.value(), y = f.value();
  Id ca = f.call(identity, x, {pa}), cb = f.call(identity, y, {pb});
  Id end = f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, end).points_to == pts({a}));
  CHECK(aa.pointsTo(y, end).points_to == pts({b}));
  CHECK(aa.pointsTo(parameter, ret, {ca}).points_to == pts({a}));
  CHECK(aa.pointsTo(parameter, ret, {cb}).points_to == pts({b}));
  CHECK(aa.pointsToAllContexts(parameter, ret).points_to == pts({a, b}));
  CHECK(aa.pointsTo(parameter, ret).status == QueryStatus::Unreachable);
  auto count = aa.statistics().contexts;
  aa.pointsTo(x, end);
  CHECK(count == aa.statistics().contexts);
}
void byReferenceAndMemoryContext() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), pa = f.addr(a), pb = f.addr(b);
  Id slot = f.object("slot", pts({a})), ps = f.addr(slot);
  Id setter = f.p.addFunction("setter");
  f.p.addBlock(setter);
  Id dst = f.value("dst", setter), src = f.value("src", setter);
  f.p.functions[setter].parameters = {dst, src};
  f.inst(Opcode::Store, INVALID, {dst, src}, setter);
  f.inst(Opcode::Return, INVALID, {}, setter);
  Id getter = f.p.addFunction("getter");
  f.p.addBlock(getter);
  Id tmp = f.value("tmp", getter);
  f.inst(Opcode::Load, tmp, {ps}, getter);
  f.inst(Opcode::Return, INVALID, {tmp}, getter);
  Id x = f.value(), y = f.value();
  f.call(getter, x, {});
  f.call(setter, INVALID, {ps, pb});
  f.call(getter, y, {});
  Id end = f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, end).points_to == pts({a}));
  CHECK(aa.pointsTo(y, end).points_to == pts({b}));
  CHECK(!aa.mayAlias(y, pa, end));
}
void conditionalCalleeWrite() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), pb = f.addr(b);
  Id slot = f.object("slot", pts({a})), ps = f.addr(slot);
  Id maybe = f.p.addFunction("maybe");
  f.p.addBlock(maybe);
  Id write = f.p.addBlock(maybe), skip = f.p.addBlock(maybe);
  f.p.functions[maybe].blocks[0].successors = {{write, {}}, {skip, {}}};
  f.inst(Opcode::Store, INVALID, {ps, pb}, maybe, write);
  f.inst(Opcode::Return, INVALID, {}, maybe, write);
  f.inst(Opcode::Return, INVALID, {}, maybe, skip);
  f.call(maybe, INVALID, {});
  Id x = f.value(), load = f.inst(Opcode::Load, x, {ps});
  f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, load, {}, Point::After).points_to == pts({a, b}));
}
void recursion() {
  Fixture f;
  Id a = f.object("a"), pa = f.addr(a);
  Id rec = f.p.addFunction("rec");
  f.p.addBlock(rec);
  Id base = f.p.addBlock(rec), step = f.p.addBlock(rec);
  Id p = f.value("p", rec), r = f.value("r", rec);
  f.p.functions[rec].parameters = {p};
  f.p.functions[rec].blocks[0].successors = {{base, {}}, {step, {}}};
  Id ret = f.inst(Opcode::Return, INVALID, {p}, rec, base);
  Id nested = f.call(rec, r, {p}, rec, step);
  f.inst(Opcode::Return, INVALID, {r}, rec, step);
  Id x = f.value(), outer = f.call(rec, x, {pa});
  Id end = f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, end).points_to == pts({a}));
  Context path(100, nested);
  path.insert(path.begin(), outer);
  CHECK(aa.pointsTo(p, ret, path).points_to == pts({a}));
  CHECK(aa.statistics().contexts < 40);
  CHECK(aa.statistics().recursive_call_graph_sccs != 0);
  CHECK(aa.statistics().scc_reschedules != 0);
}
void mutualRecursion() {
  Fixture f;
  Id a = f.object("a"), pa = f.addr(a);
  Id one = f.p.addFunction("one"), two = f.p.addFunction("two");
  f.p.addBlock(one);
  f.p.addBlock(two);
  Id base = f.p.addBlock(two), step = f.p.addBlock(two);
  f.p.functions[two].blocks[0].successors = {{base, {}}, {step, {}}};
  Id x = f.value("x", one), y = f.value("y", two), z = f.value();
  f.call(two, x, {}, one);
  f.inst(Opcode::Return, INVALID, {x}, one);
  f.inst(Opcode::Return, INVALID, {pa}, two, base);
  f.call(one, y, {}, two, step);
  f.inst(Opcode::Return, INVALID, {y}, two, step);
  f.call(one, z, {});
  Id end = f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(z, end).points_to == pts({a}));
}
void noReturnIsNotIdentity() {
  Fixture f;
  Id a = f.object("a"), pa = f.addr(a);
  Id rec = f.p.addFunction("never");
  f.p.addBlock(rec);
  f.call(rec, INVALID, {}, rec);
  f.inst(Opcode::Return, INVALID, {}, rec);
  Id call = f.call(rec, INVALID, {}), end = f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(pa, call).reachable());
  CHECK(aa.pointsTo(pa, end).status == QueryStatus::Unreachable);
}
void indirectCalls() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), pa = f.addr(a), pb = f.addr(b);
  Id fa = f.p.addFunction("a_fn"), fb = f.p.addFunction("b_fn");
  f.p.addBlock(fa);
  f.p.addBlock(fb);
  f.inst(Opcode::Return, INVALID, {pa}, fa);
  f.inst(Opcode::Return, INVALID, {pb}, fb);
  Id af = f.addr(f.functionObject(fa)), bf = f.addr(f.functionObject(fb));
  Id fp = f.value(), x = f.value(), y = f.value();
  f.inst(Opcode::Join, fp, {af, bf});
  Instruction i;
  i.opcode = Opcode::Call;
  i.indirect_target = fp;
  i.result = x;
  f.p.append(f.main, 0, i);
  i.indirect_target = af;
  i.result = y;
  f.p.append(f.main, 0, i);
  Id end = f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, end).points_to == pts({a, b}));
  CHECK(aa.pointsTo(y, end).points_to == pts({a}));
}
void externalAndUnknownStores() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), pb = f.addr(b);
  Id slot = f.object("slot", pts({a})), ps = f.addr(slot);
  Id ext = f.p.addFunction("external", true);
  Id x = f.value(), y = f.value(), z = f.value();
  f.call(ext, INVALID, {});
  Id l1 = f.inst(Opcode::Load, x, {ps});
  f.inst(Opcode::Store, INVALID, {ps, pb});
  Id l2 = f.inst(Opcode::Load, y, {ps});
  Id unknown = f.p.addConstant(PointsToSet::top());
  f.inst(Opcode::Store, INVALID, {unknown, unknown});
  Id l3 = f.inst(Opcode::Load, z, {ps});
  f.inst(Opcode::Return);
  // Default HavocPolicy::Noop: unmodeled external call is no-op, while
  // explicit Store of Top through Top address still propagates Top.
  {
    Analysis aa(f.p, refined());
    CHECK(aa.pointsTo(x, l1, {}, Point::After).points_to == pts({a}));
    CHECK(aa.pointsTo(y, l2, {}, Point::After).points_to == pts({b}));
    CHECK(aa.pointsTo(z, l3, {}, Point::After).points_to.isTop());
    CHECK(aa.mayAlias(unknown, pb, l3));
  }
  // HavocPolicy::Conservative: external call havocs memory to Top.
  {
    Options opt = refined();
    opt.havoc_policy = HavocPolicy::Conservative;
    Analysis aa(f.p, opt);
    CHECK(aa.pointsTo(x, l1, {}, Point::After).points_to.isTop());
    CHECK(aa.pointsTo(y, l2, {}, Point::After).points_to == pts({b}));
    CHECK(aa.pointsTo(z, l3, {}, Point::After).points_to.isTop());
    CHECK(aa.mayAlias(unknown, pb, l3));
  }
}
void readonlyExternal() {
  Fixture f;
  Id a = f.object("a"), slot = f.object("slot", pts({a})), ps = f.addr(slot);
  Id ext = f.p.addFunction("readonly", true);
  f.p.functions[ext].writes_memory = false;
  f.call(ext, INVALID, {});
  Id x = f.value(), load = f.inst(Opcode::Load, x, {ps});
  f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, load, {}, Point::After).points_to == pts({a}));
}
void weakSummaryObjects() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), pa = f.addr(a), pb = f.addr(b);
  Id heap = f.object("heap", pts({NULL_OBJECT}), false, ObjectKind::Heap);
  Id ph = f.value(), x = f.value();
  Instruction alloc;
  alloc.opcode = Opcode::Allocate;
  alloc.object = heap;
  alloc.result = ph;
  f.p.append(f.main, 0, alloc);
  f.inst(Opcode::Store, INVALID, {ph, pa});
  f.inst(Opcode::Store, INVALID, {ph, pb});
  Id load = f.inst(Opcode::Load, x, {ph});
  f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, load, {}, Point::After).points_to ==
        pts({NULL_OBJECT, a, b}));
}
void multiTargetWeakUpdate() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), c = f.object("c");
  Id s = f.object("s", pts({a})), t = f.object("t", pts({b}));
  Id ps = f.addr(s), pt = f.addr(t), pc = f.addr(c), both = f.value(),
     x = f.value();
  f.inst(Opcode::Join, both, {ps, pt});
  f.inst(Opcode::Store, INVALID, {both, pc});
  Id load = f.inst(Opcode::Load, x, {ps});
  f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, load, {}, Point::After).points_to == pts({a, c}));
}
void partialWriteIsWeak() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), pb = f.addr(b);
  Id slot = f.object("slot", pts({a}));
  f.p.objects[slot].bytes = 8;
  Id ps = f.addr(slot), x = f.value();
  Instruction store;
  store.opcode = Opcode::Store;
  store.operands = {ps, pb};
  store.write_bytes = 4;
  f.p.append(f.main, 0, store);
  Id load = f.inst(Opcode::Load, x, {ps});
  f.inst(Opcode::Return);
  // Default HavocPolicy::Noop ignores incompatible partial-width store.
  {
    Analysis aa(f.p, refined());
    CHECK(aa.pointsTo(x, load, {}, Point::After).points_to == pts({a}));
  }
  // HavocPolicy::Conservative: mixed pointer bits widen to Top.
  {
    Options opt = refined();
    opt.havoc_policy = HavocPolicy::Conservative;
    Analysis aa(f.p, opt);
    CHECK(aa.pointsTo(x, load, {}, Point::After).points_to.isTop());
  }
}
void phiParallelLoop() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), pa = f.addr(a), pb = f.addr(b);
  Id p = f.value(), q = f.value();
  Id loop = f.p.addBlock(f.main), end = f.p.addBlock(f.main);
  f.p.functions[f.main].blocks[0].successors = {{loop, {{p, pa}, {q, pb}}}};
  f.p.functions[f.main].blocks[loop].successors = {{loop, {{p, q}, {q, p}}},
                                                   {end, {}}};
  Id site = f.inst(Opcode::Nop, INVALID, {}, f.main, loop);
  f.inst(Opcode::Return, INVALID, {}, f.main, end);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(p, site).points_to == pts({a, b}));
  CHECK(aa.pointsTo(q, site).points_to == pts({a, b}));
}
void cyclicMemory() {
  Fixture f;
  Id holder = f.object("holder", pts({NULL_OBJECT})), ph = f.addr(holder),
     x = f.value();
  f.inst(Opcode::Store, INVALID, {ph, ph});
  Id load = f.inst(Opcode::Load, x, {ph});
  f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, load, {}, Point::After).points_to == pts({holder}));
}
void overlapAndCoverage() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), pa = f.addr(a), pb = f.addr(b),
     both = f.value();
  f.inst(Opcode::Join, both, {pa, pb});
  Id end = f.inst(Opcode::Return);
  Options parallel = refined();
  parallel.parallelism = 2;
  Analysis aa(f.p, parallel);
  unsigned occurrences = 0;
  for (const auto &cluster : aa.clusters())
    for (Id value : cluster)
      if (value == both)
        ++occurrences;
  CHECK(occurrences >= 2);
  const auto parallelResult = aa.pointsTo(both, end);
  CHECK(parallelResult.points_to == pts({a, b}));
  CHECK(aa.statistics().parallel_cluster_tasks >= 2);

  Options serial = parallel;
  serial.parallel_clusters = false;
  Analysis reference(f.p, serial);
  CHECK(reference.pointsTo(both, end).points_to == parallelResult.points_to);
}
void adaptiveAndersenThreshold() {
  Fixture f;
  Id first = f.object("first"), second = f.object("second");
  std::vector<Id> firstGroup, secondGroup;
  for (unsigned index = 0; index < 8; ++index)
    firstGroup.push_back(f.addr(first));
  for (unsigned index = 0; index < 5; ++index)
    secondGroup.push_back(f.addr(second));
  Id end = f.inst(Opcode::Return);

  Options adaptive = refined();
  adaptive.adaptive_andersen_threshold = true;
  Analysis adjusted(f.p, adaptive);
  CHECK(adjusted.pointsTo(firstGroup.front(), end).points_to == pts({first}));
  CHECK(adjusted.statistics().andersen_runs != 0);
  CHECK(adjusted.statistics().adaptive_refinement_rejections != 0);
  CHECK(adjusted.statistics().adaptive_refinement_skips != 0);
  CHECK(adjusted.statistics().effective_andersen_threshold >
        adaptive.andersen_threshold);

  Options fixed = adaptive;
  fixed.adaptive_andersen_threshold = false;
  Analysis reference(f.p, fixed);
  CHECK(reference.pointsTo(secondGroup.front(), end).points_to ==
        pts({second}));
  CHECK(reference.statistics().adaptive_refinement_skips == 0);

  Options guarded = adaptive;
  guarded.max_andersen_partition_size = 0;
  guarded.max_andersen_work = 4;
  Analysis costGuarded(f.p, guarded);
  CHECK(costGuarded.pointsTo(firstGroup.front(), end).points_to ==
        pts({first}));
  CHECK(costGuarded.statistics().adaptive_cost_skips == 2);
  CHECK(costGuarded.statistics().andersen_runs == 0);
}
void parallelPrecompute() {
  Fixture f;
  std::vector<std::pair<Id, Id>> pointers;
  for (unsigned index = 0; index < 6; ++index) {
    const Id object = f.object("object");
    pointers.emplace_back(f.addr(object), object);
  }
  const Id end = f.inst(Opcode::Return);

  Options parallel = refined();
  parallel.parallelism = 3;
  Analysis analysis(f.p, parallel);
  analysis.precomputeAll();
  CHECK(analysis.statistics().evaluated_clusters == analysis.clusters().size());
  CHECK(analysis.statistics().parallel_cluster_tasks >= 2);

  Options serial = parallel;
  serial.parallel_clusters = false;
  Analysis reference(f.p, serial);
  reference.precomputeAll();
  for (const auto &[pointer, object] : pointers) {
    CHECK(analysis.pointsTo(pointer, end).points_to == pts({object}));
    CHECK(reference.pointsTo(pointer, end).points_to ==
          analysis.pointsTo(pointer, end).points_to);
  }
}
void limitsAndValidation() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), pa = f.addr(a), pb = f.addr(b);
  Id unk = f.value("unk");
  f.p.functions[f.main].parameters = {unk};
  Id end = f.inst(Opcode::Return);
  Options zero = refined();
  zero.max_contexts = 0;
  Analysis aa(f.p, zero);
  // ResourceLimit gracefully falls back to sound upper bound (`{a}` for `pa`,
  // `Top` for `unk`), preserving non-aliasing between disjoint upper bounds.
  auto q_pa = aa.pointsTo(pa, end);
  CHECK(q_pa.status == QueryStatus::ResourceLimit);
  CHECK(q_pa.points_to == pts({a}));
  auto q_unk = aa.pointsTo(unk, end);
  CHECK(q_unk.status == QueryStatus::ResourceLimit);
  CHECK(q_unk.points_to.isTop());
  CHECK(!aa.mayAlias(pa, pb, end));
  auto al_pa = aa.aliases(pa, end);
  CHECK(al_pa.status == QueryStatus::ResourceLimit);
  CHECK(!al_pa.may_alias_all);
  CHECK(std::find(al_pa.aliases.begin(), al_pa.aliases.end(), pa) !=
        al_pa.aliases.end());
  CHECK(std::find(al_pa.aliases.begin(), al_pa.aliases.end(), pb) ==
        al_pa.aliases.end());
  zero.max_contexts = 10;
  zero.max_steps = 0;
  Analysis steps(f.p, zero);
  CHECK(steps.pointsTo(pa, end).status == QueryStatus::ResourceLimit);
  CHECK(steps.pointsTo(pa, end).points_to == pts({a}));
  bool threw = false;
  try {
    aa.pointsTo(INVALID, end);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  CHECK(threw);
  threw = false;
  try {
    aa.pointsTo(pa, end, {end});
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  CHECK(threw);
}
void sizedLoadsAndStores() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), pb = f.addr(b);
  Id slot = f.object("slot", pts({a}));
  f.p.objects[slot].bytes = 8;
  Id ps = f.addr(slot), x = f.value(), y = f.value();
  Instruction store;
  store.opcode = Opcode::Store;
  store.operands = {ps, pb};
  store.write_bytes = 8;
  f.p.append(f.main, 0, store);
  Instruction load;
  load.opcode = Opcode::Load;
  load.operands = {ps};
  load.result = x;
  load.read_bytes = 8;
  Id first = f.p.append(f.main, 0, load);
  load.result = y;
  load.read_bytes = 4;
  Id second = f.p.append(f.main, 0, load);
  f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, first, {}, Point::After).points_to == pts({b}));
  CHECK(aa.pointsTo(y, second, {}, Point::After).points_to.isTop());
}
void nullableAllocationAndHavocResult() {
  Fixture f;
  Id heap = f.object("heap", PointsToSet::top(), false, ObjectKind::Heap);
  Id p = f.value(), old = f.value();
  Instruction allocation;
  allocation.opcode = Opcode::Allocate;
  allocation.object = heap;
  allocation.result = p;
  allocation.may_be_null = true;
  Id site = f.p.append(f.main, 0, allocation);
  Id havoc = f.inst(Opcode::Havoc, old, {p});
  f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(p, site, {}, Point::After).points_to ==
        pts({heap, NULL_OBJECT}));
  CHECK(aa.pointsTo(old, havoc, {}, Point::After).points_to.isTop());
}
void implicitByvalAllocation() {
  Fixture f;
  Id cell = f.object("caller-cell", pts({NULL_OBJECT})), pc = f.addr(cell);
  Id copy = f.p.addFunction("copy");
  f.p.addBlock(copy);
  Id formal = f.value("formal", copy);
  f.p.functions[copy].parameters = {formal};
  Id hidden = f.object("byval", PointsToSet::top(), false, ObjectKind::Stack);
  Instruction allocation;
  allocation.opcode = Opcode::Allocate;
  allocation.object = hidden;
  allocation.result = formal;
  f.p.append(copy, 0, allocation);
  f.inst(Opcode::Return, INVALID, {formal}, copy);
  Id r = f.value();
  f.call(copy, r, {pc});
  Id end = f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(r, end).points_to == pts({hidden}));
  CHECK(!aa.mayAlias(r, pc, end));
}
void secondLevelDependencies() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), c = f.object("c");
  Id s = f.object("s", pts({a})), t = f.object("t", pts({b}));
  Id link = f.object("link", pts({s}));
  Id ps = f.addr(s), pt = f.addr(t), pl = f.addr(link), pc = f.addr(c);
  Id old_address = f.value(), x = f.value(), y = f.value();
  f.inst(Opcode::Load, old_address, {pl});
  f.inst(Opcode::Store, INVALID, {pl, pt});
  f.inst(Opcode::Store, INVALID, {old_address, pc});
  Id first = f.inst(Opcode::Load, x, {ps}),
     second = f.inst(Opcode::Load, y, {pt});
  f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, first, {}, Point::After).points_to == pts({c}));
  CHECK(aa.pointsTo(y, second, {}, Point::After).points_to == pts({b}));
}
void differentialSlices() {
  // Deterministic metamorphic test: bootstrapped/sliced and monolithic solvers
  // must agree on 200 generated straight-line programs with aliasing stores,
  // loads, pointer joins, and context-sensitive helper calls.
  std::mt19937 random(0xB007);
  for (unsigned trial = 0; trial < 200; ++trial) {
    Fixture f;
    std::vector<Id> objects, pointers, slots;
    for (unsigned i = 0; i < 4; ++i)
      objects.push_back(f.object("obj"));
    for (Id object : objects)
      pointers.push_back(f.addr(object));
    for (unsigned i = 0; i < 4; ++i)
      slots.push_back(f.addr(f.object("cell", pts({objects[i]}))));
    Id helper = f.p.addFunction("id");
    f.p.addBlock(helper);
    Id arg = f.value("arg", helper);
    f.p.functions[helper].parameters = {arg};
    f.inst(Opcode::Return, INVALID, {arg}, helper);
    for (unsigned step = 0; step < 16; ++step) {
      switch (random() % 4) {
      case 0:
        f.inst(Opcode::Store, INVALID,
               {slots[random() % slots.size()],
                pointers[random() % pointers.size()]});
        break;
      case 1: {
        Id v = f.value();
        f.inst(Opcode::Load, v, {slots[random() % slots.size()]});
        pointers.push_back(v);
        break;
      }
      case 2: {
        Id v = f.value();
        f.inst(Opcode::Join, v,
               {pointers[random() % pointers.size()],
                pointers[random() % pointers.size()]});
        pointers.push_back(v);
        break;
      }
      default: {
        Id v = f.value();
        f.call(helper, v, {pointers[random() % pointers.size()]});
        pointers.push_back(v);
        break;
      }
      }
    }
    Id end = f.inst(Opcode::Return);
    Options mono;
    mono.enable_clustering = false;
    mono.enable_slicing = false;
    Options sliced = refined();
    if (trial % 2 == 0)
      sliced.andersen_threshold = 60;
    Options serial = sliced;
    serial.parallel_clusters = false;
    Analysis reference(f.p, mono), bootstrapped(f.p, sliced),
        serialized(f.p, serial);
    for (Id pointer : pointers) {
      auto expected = reference.pointsTo(pointer, end),
           actual = bootstrapped.pointsTo(pointer, end),
           serialResult = serialized.pointsTo(pointer, end);
      CHECK(expected.status == actual.status);
      CHECK(expected.points_to == actual.points_to);
      CHECK(serialResult.status == actual.status);
      CHECK(serialResult.points_to == actual.points_to);
    }
  }
}

void paperFigure2SteensgaardVsAndersen() {
  // Kahlon PLDI '08 Fig. 2: p=&a; q=&b; r=&c; q=p; q=r;
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), c = f.object("c");
  Id pa = f.addr(a), pb = f.addr(b), pc = f.addr(c);
  Id p = f.value("p"), q = f.value("q"), r = f.value("r");
  f.inst(Opcode::Copy, p, {pa});
  f.inst(Opcode::Copy, r, {pc});
  f.inst(Opcode::Join, q, {pb, p, r});
  Id end = f.inst(Opcode::Return);
  Options o = refined();
  o.adaptive_andersen_threshold = false;
  Analysis aa(f.p, o);
  CHECK(aa.pointsTo(p, end).points_to == pts({a}));
  CHECK(aa.pointsTo(q, end).points_to == pts({a, b, c}));
  CHECK(aa.pointsTo(r, end).points_to == pts({c}));
  CHECK(aa.mayAlias(p, q, end));
  CHECK(aa.mayAlias(q, r, end));
  CHECK(!aa.mayAlias(p, r, end));
  // Theorem 7: q appears in multiple overlapping Andersen clusters while p/r
  // are separated into smaller clusters.
  CHECK(aa.statistics().overlapping_values > 0);
  CHECK(aa.statistics().largest_cluster <
        aa.statistics().steensgaard_largest_partition);
}

void paperFigure3RelevantSlicing() {
  // Kahlon PLDI '08 Fig. 3: 1a: x=&a; 2a: y=&b; 3a: p=x; 4a: *x=*y;
  Fixture f;
  Id ta = f.object("ta"), tb = f.object("tb");
  Id a = f.object("a", pts({ta})), b = f.object("b", pts({tb}));
  Id x = f.value("x"), y = f.value("y"), p = f.value("p");
  Id yv = f.value("yv"), av = f.value("av");
  f.inst(Opcode::Copy, x, {f.addr(a)});
  f.inst(Opcode::Copy, y, {f.addr(b)});
  f.inst(Opcode::Copy, p, {x});
  f.inst(Opcode::Load, yv, {y});
  f.inst(Opcode::Store, INVALID, {x, yv});
  Id loadA = f.inst(Opcode::Load, av, {f.addr(a)});
  f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(av, loadA, {}, Point::After).points_to == pts({tb}));
  CHECK(aa.pointsTo(p, loadA).points_to == pts({a}));
}

void paperFigure4MaximallyCompleteUpdateSequence() {
  // Kahlon PLDI '08 Fig. 4: 1a: b=c; 2a: x=&a; 3a: y=&b; 4a: *x=b;
  Fixture f;
  Id tc = f.object("tc"), ta = f.object("ta");
  Id a = f.object("a", pts({ta})), b = f.object("b", pts({ta})),
     c = f.object("c", pts({tc}));
  Id pa = f.addr(a), pb = f.addr(b), pc = f.addr(c);
  Id vc = f.value("vc"), vb = f.value("vb"), x = f.value("x"), y = f.value("y"),
     va = f.value("va");
  f.inst(Opcode::Load, vc, {pc});
  f.inst(Opcode::Store, INVALID, {pb, vc}); // 1a: b = c
  f.inst(Opcode::Copy, x, {pa});           // 2a: x = &a
  f.inst(Opcode::Copy, y, {pb});           // 3a: y = &b
  f.inst(Opcode::Load, vb, {pb});
  f.inst(Opcode::Store, INVALID, {x, vb}); // 4a: *x = b
  Id loadA = f.inst(Opcode::Load, va, {pa});
  f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(va, loadA, {}, Point::After).points_to == pts({tc}));
}

void paperFigure5GuardedSummariesAndSkipping() {
  // Kahlon PLDI '08 Fig. 5: main, foo, bar with guarded *x=d in bar and
  // untouched bar skipped for depth-0 partition P1={x,u,w,z}.
  Fixture f;
  Id t0 = f.object("t0"), td = f.object("td"), tb = f.object("tb"),
     tc = f.object("tc");
  Id a = f.object("a", pts({t0})), b = f.object("b", pts({tb})),
     c = f.object("c", pts({tc})), d = f.object("d", pts({td}));
  Id x = f.object("x", pts({a})), u = f.object("u", pts({b})),
     w = f.object("w", pts({c})), z = f.object("z", pts({a}));
  Id px = f.addr(x), pu = f.addr(u), pw = f.addr(w), pz = f.addr(z);
  Id pa = f.addr(a), pb = f.addr(b), pc = f.addr(c), pd = f.addr(d);

  // foo(): 1b: *x = d; 2b: a = b; 3b: x = w;
  Id foo = f.p.addFunction("foo");
  f.p.addBlock(foo);
  Id fx = f.value("fx", foo), fd = f.value("fd", foo), fb = f.value("fb", foo),
     fw = f.value("fw", foo);
  f.inst(Opcode::Load, fx, {px}, foo);
  f.inst(Opcode::Load, fd, {pd}, foo);
  f.inst(Opcode::Store, INVALID, {fx, fd}, foo);
  f.inst(Opcode::Load, fb, {pb}, foo);
  f.inst(Opcode::Store, INVALID, {pa, fb}, foo);
  f.inst(Opcode::Load, fw, {pw}, foo);
  f.inst(Opcode::Store, INVALID, {px, fw}, foo);
  f.inst(Opcode::Return, INVALID, {}, foo);

  // bar(): 1c: *x = d; 2c: a = b; (does NOT modify P1={x,u,w,z}!)
  Id bar = f.p.addFunction("bar");
  f.p.addBlock(bar);
  Id bx = f.value("bx", bar), bd = f.value("bd", bar), bb = f.value("bb", bar);
  f.inst(Opcode::Load, bx, {px}, bar);
  f.inst(Opcode::Load, bd, {pd}, bar);
  f.inst(Opcode::Store, INVALID, {bx, bd}, bar);
  f.inst(Opcode::Load, bb, {pb}, bar);
  f.inst(Opcode::Store, INVALID, {pa, bb}, bar);
  f.inst(Opcode::Return, INVALID, {}, bar);

  // main(): 1a: x=&c; 2a: w=u; 3a: foo(); 4a: z=x; 5a: *z=b; 6a: bar();
  Id mu = f.value("mu"), mx = f.value("mx"), mz = f.value("mz"),
     mb = f.value("mb"), finalZ = f.value("finalZ"), finalA = f.value("finalA");
  // Call bar() in two contexts (first with x=&c so 1c does NOT overwrite b,
  // then after foo() sets x=w=u=&b so 1c DOES overwrite b with d) so FSCI(x)
  // at 1c is {b, c} and bar() produces both guarded tuples t1 & t2!
  f.inst(Opcode::Store, INVALID, {px, pc});
  f.call(bar, INVALID, {});
  Id aFirst = f.value("aFirst");
  Id loadAFirst = f.inst(Opcode::Load, aFirst, {pa});
  f.inst(Opcode::Load, mu, {pu});
  f.inst(Opcode::Store, INVALID, {pw, mu});
  f.call(foo, INVALID, {});
  f.inst(Opcode::Load, mx, {px});
  f.inst(Opcode::Store, INVALID, {pz, mx});
  f.inst(Opcode::Load, mz, {pz});
  f.inst(Opcode::Load, mb, {pb});
  f.inst(Opcode::Store, INVALID, {mz, mb});
  f.call(bar, INVALID, {});
  Id loadZ = f.inst(Opcode::Load, finalZ, {pz});
  Id loadA = f.inst(Opcode::Load, finalA, {pa});
  f.inst(Opcode::Return);

  Analysis aa(f.p, refined());
  // First bar() saw x=&c -> *x=d did NOT overwrite b -> a took b (= tb)!
  CHECK(aa.pointsTo(aFirst, loadAFirst, {}, Point::After).points_to ==
        pts({tb}));
  // z takes value from u (= &b) via w=u, [x=w], z=x; bar is skipped for P1!
  CHECK(aa.pointsTo(finalZ, loadZ, {}, Point::After).points_to == pts({b}));
  // Second bar() saw x=&b -> *x=d overwrote b with td -> a took td!
  CHECK(aa.pointsTo(finalA, loadA, {}, Point::After).points_to == pts({td}));
  CHECK(aa.statistics().skipped_functions > 0);
  CHECK(aa.statistics().guard_atoms_created > 0);
  CHECK(aa.statistics().guards_pruned > 0);
}

void contextSensitiveStrongUpdateViaGuards() {
  // Single helper write_through(x, v) { *x = v; } called with two distinct
  // singleton cells. FSCI sees PT(x)={slotA, slotB}; FSCS prunes negative guard
  // x -/-> slotA in context 1 for a context-sensitive strong update!
  Fixture f;
  Id o1 = f.object("o1"), o2 = f.object("o2"), o3 = f.object("o3"),
     o4 = f.object("o4");
  Id slotA = f.object("slotA", pts({o1})), slotB = f.object("slotB", pts({o2}));
  Id pA = f.addr(slotA), pB = f.addr(slotB);
  Id p3 = f.addr(o3), p4 = f.addr(o4);

  Id writer = f.p.addFunction("write_through");
  f.p.addBlock(writer);
  Id px = f.value("x", writer), pv = f.value("v", writer);
  f.p.functions[writer].parameters = {px, pv};
  f.inst(Opcode::Store, INVALID, {px, pv}, writer);
  f.inst(Opcode::Return, INVALID, {}, writer);

  f.call(writer, INVALID, {pA, p3});
  Id a1 = f.value("a1"), b1 = f.value("b1");
  Id lA1 = f.inst(Opcode::Load, a1, {pA});
  Id lB1 = f.inst(Opcode::Load, b1, {pB});
  f.call(writer, INVALID, {pB, p4});
  Id a2 = f.value("a2"), b2 = f.value("b2");
  Id lA2 = f.inst(Opcode::Load, a2, {pA});
  Id lB2 = f.inst(Opcode::Load, b2, {pB});
  f.inst(Opcode::Return);

  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(a1, lA1, {}, Point::After).points_to == pts({o3}));
  CHECK(aa.pointsTo(b1, lB1, {}, Point::After).points_to == pts({o2}));
  CHECK(aa.pointsTo(a2, lA2, {}, Point::After).points_to == pts({o3}));
  CHECK(aa.pointsTo(b2, lB2, {}, Point::After).points_to == pts({o4}));
  CHECK(aa.statistics().guard_atoms_created > 0);
  CHECK(aa.statistics().guards_pruned > 0);
}

void algorithm4DerefAliasGuards() {
  // store_then_load(x, y, d) { *x = d; return *y; }
  // Exercises Algorithm 4 lines 28-35 (s ~ u and s !~ u alias/non-alias guards).
  Fixture f;
  Id o1 = f.object("o1"), o2 = f.object("o2"), od = f.object("od");
  Id s1 = f.object("s1", pts({o1})), s2 = f.object("s2", pts({o2}));
  Id ps1 = f.addr(s1), ps2 = f.addr(s2), pd = f.addr(od);

  Id fn = f.p.addFunction("store_then_load");
  f.p.addBlock(fn);
  Id x = f.value("x", fn), y = f.value("y", fn), d = f.value("d", fn),
     res = f.value("res", fn);
  f.p.functions[fn].parameters = {x, y, d};
  f.inst(Opcode::Store, INVALID, {x, d}, fn);
  f.inst(Opcode::Load, res, {y}, fn);
  f.inst(Opcode::Return, INVALID, {res}, fn);

  Id r_disjoint = f.value("r_disjoint"), r_aliased = f.value("r_aliased");
  f.call(fn, r_disjoint, {ps1, ps2, pd}); // x != y -> returns orig s2 ({o2})
  f.call(fn, r_aliased, {ps2, ps2, pd});  // x == y -> strong update ({od})
  Id end = f.inst(Opcode::Return);

  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(r_disjoint, end).points_to == pts({o2}));
  CHECK(aa.pointsTo(r_aliased, end).points_to == pts({od}));
  CHECK(aa.statistics().guard_atoms_created > 0);
  CHECK(aa.statistics().guards_pruned > 0);
}

void algorithm2MultiDepthDovetailing() {
  // 4-level pointer chain: l3 -> l2 -> l1 -> leafA / leafB with strong updates.
  Fixture f;
  Id leafA = f.object("leafA"), leafB = f.object("leafB");
  Id l1a = f.object("l1a", pts({leafA})), l1b = f.object("l1b", pts({leafB}));
  Id l2a = f.object("l2a", pts({l1a})), l2b = f.object("l2b", pts({l1b}));
  Id l3 = f.object("l3", pts({l2a}));

  Id p3 = f.addr(l3), p2b = f.addr(l2b);
  Id v2 = f.value("v2"), v1 = f.value("v1"), v0 = f.value("v0");
  Id w2 = f.value("w2"), w1 = f.value("w1"), w0 = f.value("w0");

  f.inst(Opcode::Load, v2, {p3});
  f.inst(Opcode::Load, v1, {v2});
  Id first = f.inst(Opcode::Load, v0, {v1});
  f.inst(Opcode::Store, INVALID, {p3, p2b});
  f.inst(Opcode::Load, w2, {p3});
  f.inst(Opcode::Load, w1, {w2});
  Id second = f.inst(Opcode::Load, w0, {w1});
  f.inst(Opcode::Return);

  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(v0, first, {}, Point::After).points_to == pts({leafA}));
  CHECK(aa.pointsTo(w0, second, {}, Point::After).points_to == pts({leafB}));
  CHECK(aa.hierarchy().depth.size() >= 4);
  CHECK(aa.statistics().hierarchy_max_depth >= 3);
  CHECK(aa.statistics().dovetail_levels >= 3);
}

void algorithm3DirectAliasSets() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b");
  Id pa = f.addr(a), pb = f.addr(b);
  Id x = f.value("x"), y = f.value("y"), z = f.value("z");
  f.inst(Opcode::Copy, x, {pa});
  f.inst(Opcode::Copy, y, {x});
  f.inst(Opcode::Copy, z, {pb});
  Id end = f.inst(Opcode::Return);

  Analysis aa(f.p, refined());
  AliasResult ax = aa.aliases(x, end);
  CHECK(ax.reachable());
  CHECK(std::find(ax.aliases.begin(), ax.aliases.end(), x) != ax.aliases.end());
  CHECK(std::find(ax.aliases.begin(), ax.aliases.end(), y) != ax.aliases.end());
  CHECK(std::find(ax.aliases.begin(), ax.aliases.end(), z) == ax.aliases.end());

  AliasResult axAll = aa.aliasesAllContexts(x, end);
  CHECK(axAll.reachable());
  CHECK(std::find(axAll.aliases.begin(), axAll.aliases.end(), y) !=
        axAll.aliases.end());
  CHECK(std::find(axAll.aliases.begin(), axAll.aliases.end(), z) ==
        axAll.aliases.end());
}

void pathSensitiveRobddPruning() {
  // Correlated branches: if (c) slot=&a; else slot=&b; if (c) x=*slot;
  Fixture f;
  Id a = f.object("a"), b = f.object("b");
  Id slot = f.object("slot", pts({NULL_OBJECT}));
  Id ps = f.addr(slot), pa = f.addr(a), pb = f.addr(b);
  Id cond = f.p.addCondition();

  Id t1 = f.p.addBlock(f.main), f1 = f.p.addBlock(f.main),
     join1 = f.p.addBlock(f.main), t2 = f.p.addBlock(f.main),
     exit = f.p.addBlock(f.main);
  f.p.functions[f.main].blocks[0].successors = {
      {t1, {}, cond, true}, {f1, {}, cond, false}};
  f.inst(Opcode::Store, INVALID, {ps, pa}, f.main, t1);
  f.p.functions[f.main].blocks[t1].successors = {{join1, {}}};
  f.inst(Opcode::Store, INVALID, {ps, pb}, f.main, f1);
  f.p.functions[f.main].blocks[f1].successors = {{join1, {}}};
  f.p.functions[f.main].blocks[join1].successors = {
      {t2, {}, cond, true}, {exit, {}, cond, false}};
  Id x = f.value("x");
  Id loadT2 = f.inst(Opcode::Load, x, {ps}, f.main, t2);
  f.p.functions[f.main].blocks[t2].successors = {{exit, {}}};
  f.inst(Opcode::Return, INVALID, {}, f.main, exit);

  Options insensitive = refined();
  insensitive.path_sensitive = false;
  Analysis pi(f.p, insensitive);
  CHECK(pi.pointsTo(x, loadT2, {}, Point::After).points_to == pts({a, b}));

  Options sensitive = refined();
  sensitive.path_sensitive = true;
  Analysis ps_aa(f.p, sensitive);
  CHECK(ps_aa.pointsTo(x, loadT2, {}, Point::After).points_to == pts({a}));
  CHECK(ps_aa.statistics().path_infeasible_prunes > 0);
  CHECK(ps_aa.statistics().bdd_nodes > 2);
}

void pathSensitiveLoopConditionSoundness() {
  // In a loop where cond is redefined each iteration, path sensitivity must
  // existentially quantify cond on loop redefinition and NOT falsely drop the
  // cross-iteration path!
  Fixture f;
  Id a = f.object("a"), b = f.object("b");
  Id slot = f.object("slot", pts({a}));
  Id ps = f.addr(slot), pb = f.addr(b);
  Id cond = f.p.addCondition();

  Id header = f.p.addBlock(f.main), writeB = f.p.addBlock(f.main),
     readS = f.p.addBlock(f.main), done = f.p.addBlock(f.main);
  f.p.functions[f.main].blocks[0].successors = {{header, {}}};
  Instruction defCond;
  defCond.opcode = Opcode::Nop;
  defCond.defines_condition = cond;
  f.p.append(f.main, header, defCond);
  f.p.functions[f.main].blocks[header].successors = {
      {writeB, {}, cond, true}, {readS, {}, cond, false}};
  f.inst(Opcode::Store, INVALID, {ps, pb}, f.main, writeB);
  f.p.functions[f.main].blocks[writeB].successors = {{header, {}}};
  Id x = f.value("x");
  Id loadSite = f.inst(Opcode::Load, x, {ps}, f.main, readS);
  f.p.functions[f.main].blocks[readS].successors = {{done, {}}};
  f.inst(Opcode::Return, INVALID, {}, f.main, done);

  Options sensitive = refined();
  sensitive.path_sensitive = true;
  Analysis aa(f.p, sensitive);
  // Iteration 1 takes writeB (cond=true), Iteration 2 redefines cond=false and
  // enters readS -> x MUST include BOTH {a, b}!
  CHECK(aa.pointsTo(x, loadSite, {}, Point::After).points_to == pts({a, b}));
}

void oneLevelFlowCascade() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), c = f.object("c");
  Id pa = f.addr(a), pb = f.addr(b), pc = f.addr(c);
  Id p = f.value("p"), q = f.value("q"), r = f.value("r");
  f.inst(Opcode::Copy, p, {pa});
  f.inst(Opcode::Copy, r, {pc});
  f.inst(Opcode::Join, q, {pb, p, r});
  Id end = f.inst(Opcode::Return);
  Options o = refined();
  o.enable_one_level_flow = true;
  Analysis aa(f.p, o);
  CHECK(aa.statistics().one_level_flow_runs > 0);
  CHECK(aa.pointsTo(p, end).points_to == pts({a}));
  CHECK(aa.pointsTo(q, end).points_to == pts({a, b, c}));
  CHECK(aa.pointsTo(r, end).points_to == pts({c}));
}

void solverBackendDifferentialFuzz() {
  // Compare GuardedSummary (PLDI '08) vs InputStateTabulation (monolithic
  // oracle) across 150 randomly generated programs with branches, multi-level
  // stores/loads, indirect calls, and helper calls.
  std::mt19937 rng(0xC0FFEE);
  for (unsigned trial = 0; trial < 150; ++trial) {
    Fixture f;
    std::vector<Id> leaves, cells, ptrs;
    for (unsigned i = 0; i < 3; ++i) {
      Id lf = f.object("leaf" + std::to_string(i));
      leaves.push_back(lf);
      ptrs.push_back(f.addr(lf));
    }
    for (unsigned i = 0; i < 3; ++i) {
      Id c = f.object("cell" + std::to_string(i), pts({leaves[i]}));
      cells.push_back(f.addr(c));
    }
    // Helper function that writes *dst = src and returns *other.
    Id helper = f.p.addFunction("helper");
    f.p.addBlock(helper);
    Id hdst = f.value("hdst", helper), hsrc = f.value("hsrc", helper),
       hother = f.value("hother", helper), hret = f.value("hret", helper);
    f.p.functions[helper].parameters = {hdst, hsrc, hother};
    f.inst(Opcode::Store, INVALID, {hdst, hsrc}, helper);
    f.inst(Opcode::Load, hret, {hother}, helper);
    f.inst(Opcode::Return, INVALID, {hret}, helper);

    // Build diamond CFG: entry -> {b1, b2} -> merge
    Id b1 = f.p.addBlock(f.main), b2 = f.p.addBlock(f.main),
       merge = f.p.addBlock(f.main);
    f.p.functions[f.main].blocks[0].successors = {{b1, {}}, {b2, {}}};
    f.p.functions[f.main].blocks[b1].successors = {{merge, {}}};
    f.p.functions[f.main].blocks[b2].successors = {{merge, {}}};

    auto emitRandom = [&](Id blk, unsigned steps, std::vector<Id> &avail) {
      for (unsigned s = 0; s < steps; ++s) {
        switch (rng() % 4) {
        case 0:
          f.inst(Opcode::Store, INVALID,
                 {cells[rng() % cells.size()], avail[rng() % avail.size()]},
                 f.main, blk);
          break;
        case 1: {
          Id v = f.value("v");
          f.inst(Opcode::Load, v, {cells[rng() % cells.size()]}, f.main, blk);
          avail.push_back(v);
          break;
        }
        case 2: {
          Id v = f.value("j");
          f.inst(Opcode::Join, v,
                 {avail[rng() % avail.size()], avail[rng() % avail.size()]},
                 f.main, blk);
          avail.push_back(v);
          break;
        }
        default: {
          Id v = f.value("r");
          f.call(helper, v,
                 {cells[rng() % cells.size()], avail[rng() % avail.size()],
                  cells[rng() % cells.size()]},
                 f.main, blk);
          avail.push_back(v);
          break;
        }
        }
      }
    };

    emitRandom(0, 4, ptrs);
    std::vector<Id> p1 = ptrs, p2 = ptrs;
    emitRandom(b1, 3, p1);
    emitRandom(b2, 3, p2);
    emitRandom(merge, 4, ptrs);
    Id end = f.inst(Opcode::Return, INVALID, {}, f.main, merge);

    Options oracle_opt;
    oracle_opt.solver_mode = SolverMode::InputStateTabulation;
    oracle_opt.enable_clustering = false;
    oracle_opt.enable_slicing = false;

    Options guarded_opt = refined();
    guarded_opt.solver_mode = SolverMode::GuardedSummary;
    if (trial % 3 == 0)
      guarded_opt.enable_one_level_flow = true;

    Analysis oracle(f.p, oracle_opt), guarded(f.p, guarded_opt);
    for (Id p : ptrs) {
      auto exp = oracle.pointsTo(p, end);
      auto act = guarded.pointsTo(p, end);
      CHECK(exp.status == act.status);
      CHECK(exp.points_to == act.points_to);
    }
  }
}

void pathSensitiveInterproceduralRobdd() {
  // Caller branches on `cond` to call `writer()` (which writes `@a` if `cond`
  // is true, else `@b`), and then reads `*slot` in the `cond == true` branch.
  // Interprocedural ROBDD path sensitivity carries `cond` across the callee
  // summary and proves `x == {a}`, whereas intraprocedural-only keeps `{a, b}`!
  Fixture f;
  Id a = f.object("a"), b = f.object("b");
  Id slot = f.object("slot");
  Id pa = f.addr(a), pb = f.addr(b), ps = f.addr(slot);
  Id cond = f.p.addCondition();

  // Callee `writer()`: if (cond) *slot = &a; else *slot = &b;
  Id writer = f.p.addFunction("writer");
  Id wEntry = f.p.addBlock(writer), wTrue = f.p.addBlock(writer),
     wFalse = f.p.addBlock(writer), wExit = f.p.addBlock(writer);
  f.inst(Opcode::Nop, INVALID, {}, writer, wEntry);
  f.p.functions[writer].blocks[wEntry].successors = {
      {wTrue, {}, cond, true}, {wFalse, {}, cond, false}};
  f.inst(Opcode::Store, INVALID, {ps, pa}, writer, wTrue);
  f.p.functions[writer].blocks[wTrue].successors = {{wExit, {}}};
  f.inst(Opcode::Store, INVALID, {ps, pb}, writer, wFalse);
  f.p.functions[writer].blocks[wFalse].successors = {{wExit, {}}};
  f.inst(Opcode::Return, INVALID, {}, writer, wExit);

  // Caller `main()`: writer(); if (cond) x = *slot; else y = *slot;
  Id mTrue = f.p.addBlock(f.main), mFalse = f.p.addBlock(f.main),
     mDone = f.p.addBlock(f.main);
  Instruction call;
  call.opcode = Opcode::Call;
  call.callee = writer;
  f.p.append(f.main, 0, call);
  f.p.functions[f.main].blocks[0].successors = {
      {mTrue, {}, cond, true}, {mFalse, {}, cond, false}};
  Id x = f.value("x"), y = f.value("y");
  Id loadX = f.inst(Opcode::Load, x, {ps}, f.main, mTrue);
  f.p.functions[f.main].blocks[mTrue].successors = {{mDone, {}}};
  Id loadY = f.inst(Opcode::Load, y, {ps}, f.main, mFalse);
  f.p.functions[f.main].blocks[mFalse].successors = {{mDone, {}}};
  f.inst(Opcode::Return, INVALID, {}, f.main, mDone);

  Options intra = refined();
  intra.path_sensitive = true;
  intra.path_sensitive_interprocedural = false;
  Analysis intra_aa(f.p, intra);
  CHECK(intra_aa.pointsTo(x, loadX, {}, Point::After).points_to == pts({a, b}));

  Options inter = refined();
  inter.path_sensitive = true;
  inter.path_sensitive_interprocedural = true;
  Analysis inter_aa(f.p, inter);
  CHECK(inter_aa.pointsTo(x, loadX, {}, Point::After).points_to == pts({a}));
  CHECK(inter_aa.pointsTo(y, loadY, {}, Point::After).points_to == pts({b}));
  CHECK(inter_aa.statistics().path_infeasible_prunes > 0);
}

void cyclicLinkedListLoopDifferential() {
  // Cyclic self-referential heap allocation traversed in a loop:
  //   node1 = Alloc(heap1); *node1 = node2;
  //   node2 = Alloc(heap2); *node2 = node1;
  //   p = phi(node1, next); next = *p;
  // Both GuardedSummary (cyclic Pass 1 + Pass 2 dovetailing) and
  // InputStateTabulation must converge cleanly and agree on {heap1, heap2}!
  Fixture f;
  Object h1;
  h1.name = "node1";
  h1.kind = ObjectKind::Heap;
  h1.singleton = false;
  h1.initial = PointsToSet{};
  Id o1 = f.p.addObject(h1);
  Object h2 = h1;
  h2.name = "node2";
  Id o2 = f.p.addObject(h2);

  Id n1 = f.value("n1"), n2 = f.value("n2");
  Instruction a1;
  a1.opcode = Opcode::Allocate;
  a1.result = n1;
  a1.object = o1;
  f.p.append(f.main, 0, a1);
  Instruction a2;
  a2.opcode = Opcode::Allocate;
  a2.result = n2;
  a2.object = o2;
  f.p.append(f.main, 0, a2);
  f.inst(Opcode::Store, INVALID, {n1, n2});
  f.inst(Opcode::Store, INVALID, {n2, n1});

  Id loop = f.p.addBlock(f.main), exit = f.p.addBlock(f.main);
  Id p = f.value("p"), next = f.value("next");
  f.p.functions[f.main].blocks[0].successors = {{loop, {{p, n1}}}};
  Id loadNext = f.inst(Opcode::Load, next, {p}, f.main, loop);
  f.p.functions[f.main].blocks[loop].successors = {
      {loop, {{p, next}}}, {exit, {}}};
  Id ret = f.inst(Opcode::Return, INVALID, {}, f.main, exit);

  Options g_opt = refined();
  g_opt.solver_mode = SolverMode::GuardedSummary;
  Analysis guarded(f.p, g_opt);

  Options t_opt = refined();
  t_opt.solver_mode = SolverMode::InputStateTabulation;
  Analysis tab(f.p, t_opt);

  CHECK(guarded.pointsTo(p, loadNext).points_to == pts({o1, o2}));
  CHECK(guarded.pointsTo(next, loadNext, {}, Point::After).points_to ==
        pts({o1, o2}));
  CHECK(guarded.pointsTo(p, ret).points_to == tab.pointsTo(p, ret).points_to);
  CHECK(guarded.pointsTo(next, ret).points_to ==
        tab.pointsTo(next, ret).points_to);
}

void algorithm3ForwardQInterproceduralAliases() {
  // Verify Algorithm 3 Step 1 (backward A) + Step 2 (forward Q) across a
  // helper called from two distinct contexts:
  //   callee(p, q): r = p; s = q; return;
  //   c1: callee(&a, &a)  -> FSCS aliases(r, c1) == {p, q, r, s}
  //   c2: callee(&a, &b)  -> FSCS aliases(r, c2) == {p, r} (excludes q, s!)
  //   FSCI aliasesAllContexts(r) includes {p, q, r, s}.
  Fixture f;
  Id a = f.object("a"), b = f.object("b");
  Id pa = f.addr(a), pb = f.addr(b);

  Id callee = f.p.addFunction("callee");
  f.p.addBlock(callee);
  Id p = f.value("p", callee), q = f.value("q", callee),
     r = f.value("r", callee), s = f.value("s", callee);
  f.p.functions[callee].parameters = {p, q};
  f.inst(Opcode::Copy, r, {p}, callee);
  f.inst(Opcode::Copy, s, {q}, callee);
  Id cRet = f.inst(Opcode::Return, INVALID, {}, callee);

  Instruction call1;
  call1.opcode = Opcode::Call;
  call1.callee = callee;
  call1.operands = {pa, pa};
  Id c1 = f.p.append(f.main, 0, call1);

  Instruction call2;
  call2.opcode = Opcode::Call;
  call2.callee = callee;
  call2.operands = {pa, pb};
  Id c2 = f.p.append(f.main, 0, call2);
  f.inst(Opcode::Return);

  Analysis aa(f.p, refined());
  auto has = [](const AliasResult &ar, Id v) {
    return std::find(ar.aliases.begin(), ar.aliases.end(), v) !=
           ar.aliases.end();
  };

  auto fscs1 = aa.aliases(r, cRet, {c1});
  CHECK(fscs1.reachable());
  CHECK(has(fscs1, p) && has(fscs1, q) && has(fscs1, r) && has(fscs1, s));

  auto fscs2 = aa.aliases(r, cRet, {c2});
  CHECK(fscs2.reachable());
  CHECK(has(fscs2, p) && has(fscs2, r));
  CHECK(!has(fscs2, q) && !has(fscs2, s));

  auto fsci = aa.aliasesAllContexts(r, cRet);
  CHECK(fsci.reachable());
  CHECK(has(fsci, p) && has(fsci, q) && has(fsci, r) && has(fsci, s));
}

void pathSensitiveUpwardCallerSplicing() {
  // Part A: Upward ROBDD path propagation (`con_b`) across callers for both
  // FSCS (`pointsTo`) and FSCI (`pointsToAllContexts`).
  //   main:
  //     *g = &a;
  //     if (!cond) callee();
  //   callee:
  //     if (cond)  r_infeas = *g;   // infeasible under caller's !cond!
  //     else       r_feas   = *g;   // feasible -> {a}
  {
    Fixture f;
    Id g = f.object("g");
    Id a = f.object("a");
    Id pg = f.addr(g), pa = f.addr(a);
    Id cond = f.p.addCondition();

    Id callee = f.p.addFunction("callee");
    Id cbEntry = f.p.addBlock(callee);
    Id cbThen = f.p.addBlock(callee);
    Id cbElse = f.p.addBlock(callee);
    Id rInfeas = f.value("r_infeas", callee);
    Id rFeas = f.value("r_feas", callee);
    f.inst(Opcode::Nop, INVALID, {}, callee, cbEntry);
    Id sInfeas = f.inst(Opcode::Load, rInfeas, {pg}, callee, cbThen);
    f.inst(Opcode::Return, INVALID, {}, callee, cbThen);
    Id sFeas = f.inst(Opcode::Load, rFeas, {pg}, callee, cbElse);
    f.inst(Opcode::Return, INVALID, {}, callee, cbElse);
    f.p.functions[callee].blocks[cbEntry].successors = {
        {cbThen, {}, cond, true}, {cbElse, {}, cond, false}};

    Id bCall = f.p.addBlock(f.main);
    Id bSkip = f.p.addBlock(f.main);
    f.inst(Opcode::Store, INVALID, {pg, pa}, f.main, 0);
    Instruction call;
    call.opcode = Opcode::Call;
    call.callee = callee;
    Id cSite = f.p.append(f.main, bCall, call);
    f.inst(Opcode::Return, INVALID, {}, f.main, bCall);
    f.inst(Opcode::Return, INVALID, {}, f.main, bSkip);
    f.p.functions[f.main].blocks[0].successors = {{bSkip, {}, cond, true},
                                                  {bCall, {}, cond, false}};

    Options opt_ps = refined();
    opt_ps.path_sensitive = true;
    opt_ps.path_sensitive_interprocedural = true;
    Analysis aa_ps(f.p, opt_ps);

    auto fscs_infeas = aa_ps.pointsTo(rInfeas, sInfeas, {cSite}, Point::After);
    CHECK(fscs_infeas.status == QueryStatus::Unreachable);
    auto fsci_infeas = aa_ps.pointsToAllContexts(rInfeas, sInfeas, Point::After);
    CHECK(fsci_infeas.status == QueryStatus::Unreachable);

    auto fscs_feas = aa_ps.pointsTo(rFeas, sFeas, {cSite}, Point::After);
    CHECK(fscs_feas.reachable());
    CHECK(fscs_feas.points_to == PointsToSet(a));
    auto fsci_feas = aa_ps.pointsToAllContexts(rFeas, sFeas, Point::After);
    CHECK(fsci_feas.reachable());
    CHECK(fsci_feas.points_to == PointsToSet(a));

    // Without interprocedural path sensitivity, `r_infeas` stays reachable.
    Options opt_nops = refined();
    opt_nops.path_sensitive = true;
    opt_nops.path_sensitive_interprocedural = false;
    Analysis aa_nops(f.p, opt_nops);
    CHECK(aa_nops.pointsTo(rInfeas, sInfeas, {cSite}, Point::After).points_to ==
          PointsToSet(a));
  }

  // Part B: Soundness when a memory-irrelevant (skipped) callee kills a
  // tracked condition variable via `kills_conditions`.
  {
    Fixture f;
    Id g = f.object("g");
    Id a = f.object("a"), b = f.object("b");
    Id pg = f.addr(g), pa = f.addr(a), pb = f.addr(b);
    Id cond = f.p.addCondition();

    // `killer` does not touch `g`, so memory mod-ref skips it, but it kills
    // `cond` (simulating a store to a global flag).
    Id killer = f.p.addFunction("killer");
    f.p.addBlock(killer);
    Instruction kill_inst;
    kill_inst.opcode = Opcode::Nop;
    kill_inst.kills_conditions = {cond};
    f.p.append(killer, 0, kill_inst);
    f.inst(Opcode::Return, INVALID, {}, killer);

    Id bThen1 = f.p.addBlock(f.main);
    Id bSkip1 = f.p.addBlock(f.main);
    Id bThen2 = f.p.addBlock(f.main);
    Id bElse2 = f.p.addBlock(f.main);
    f.inst(Opcode::Store, INVALID, {pg, pa}, f.main, 0);
    f.p.functions[f.main].blocks[0].successors = {{bThen1, {}, cond, true},
                                                  {bSkip1, {}, cond, false}};
    Instruction call_killer;
    call_killer.opcode = Opcode::Call;
    call_killer.callee = killer;
    f.p.append(f.main, bThen1, call_killer);
    f.p.functions[f.main].blocks[bThen1].successors = {
        {bThen2, {}, cond, false}, {bElse2, {}, cond, true}};
    f.inst(Opcode::Store, INVALID, {pg, pb}, f.main, bThen2);
    Id r = f.value("r");
    Id load_r = f.inst(Opcode::Load, r, {pg}, f.main, bThen2);
    f.inst(Opcode::Return, INVALID, {}, f.main, bThen2);
    f.inst(Opcode::Return, INVALID, {}, f.main, bElse2);
    f.inst(Opcode::Return, INVALID, {}, f.main, bSkip1);

    Options opt = refined();
    opt.path_sensitive = true;
    opt.path_sensitive_interprocedural = true;
    Analysis aa(f.p, opt);
    auto res = aa.pointsTo(r, load_r, {}, Point::After);
    CHECK(res.reachable());
    CHECK(res.points_to == PointsToSet(b));
  }
}

void flowSensitiveIndirectCallReachability() {
  // `main` initializes global fp = &dead_fn, then strong-updates fp = &live_fn,
  // then invokes (*fp)(&x). Flow-sensitive reachability must recognize
  // `dead_fn` as unreachable for both FSCS and FSCI queries!
  Fixture f;
  Id fp = f.object("fp");
  Id x = f.object("x");
  Id pfp = f.addr(fp), px = f.addr(x);

  Id deadFn = f.p.addFunction("dead_fn");
  f.p.addBlock(deadFn);
  Id deadP = f.value("dead_p", deadFn), deadQ = f.value("dead_q", deadFn);
  f.p.functions[deadFn].parameters = {deadP};
  f.inst(Opcode::Copy, deadQ, {deadP}, deadFn);
  Id deadRet = f.inst(Opcode::Return, INVALID, {}, deadFn);

  Id liveFn = f.p.addFunction("live_fn");
  f.p.addBlock(liveFn);
  Id liveP = f.value("live_p", liveFn), liveQ = f.value("live_q", liveFn);
  f.p.functions[liveFn].parameters = {liveP};
  f.inst(Opcode::Copy, liveQ, {liveP}, liveFn);
  Id liveRet = f.inst(Opcode::Return, INVALID, {}, liveFn);

  Object fDead;
  fDead.name = "fn_dead";
  fDead.kind = ObjectKind::Function;
  fDead.function = deadFn;
  Id oDead = f.p.addObject(fDead);

  Object fLive;
  fLive.name = "fn_live";
  fLive.kind = ObjectKind::Function;
  fLive.function = liveFn;
  Id oLive = f.p.addObject(fLive);

  Id cDead = f.addr(oDead), cLive = f.addr(oLive);
  Id target = f.value("target");
  f.inst(Opcode::Store, INVALID, {pfp, cDead});
  f.inst(Opcode::Store, INVALID, {pfp, cLive});
  f.inst(Opcode::Load, target, {pfp});
  Instruction icall;
  icall.opcode = Opcode::Call;
  icall.indirect_target = target;
  icall.operands = {px};
  Id icSite = f.p.append(f.main, 0, icall);
  f.inst(Opcode::Return);

  Analysis aa(f.p, refined());
  CHECK(aa.pointsToAllContexts(deadQ, deadRet).status ==
        QueryStatus::Unreachable);
  CHECK(aa.aliasesAllContexts(deadQ, deadRet).status ==
        QueryStatus::Unreachable);
  CHECK(aa.pointsTo(deadQ, deadRet, {icSite}).status ==
        QueryStatus::Unreachable);

  auto liveFsci = aa.pointsToAllContexts(liveQ, liveRet);
  CHECK(liveFsci.reachable());
  CHECK(liveFsci.points_to == PointsToSet(x));
  auto liveFscs = aa.pointsTo(liveQ, liveRet, {icSite});
  CHECK(liveFscs.reachable());
  CHECK(liveFscs.points_to == PointsToSet(x));
}

void eagerAlgorithm2DovetailingConsistency() {
  // Multi-depth pointer chain + helper functions + untouched function:
  // compare literal Algorithm 2 eager summary dovetailing against demand-driven
  // dovetailing.
  Fixture f;
  Id pp = f.object("pp");
  Id p = f.object("p");
  Id a = f.object("a"), b = f.object("b");
  Id ppp = f.addr(pp), p_addr = f.addr(p), pa = f.addr(a), pb = f.addr(b);

  Id writer = f.p.addFunction("writer");
  f.p.addBlock(writer);
  Id wp = f.value("wp", writer), wv = f.value("wv", writer);
  f.p.functions[writer].parameters = {wp, wv};
  f.inst(Opcode::Store, INVALID, {wp, wv}, writer);
  f.inst(Opcode::Return, INVALID, {wv}, writer);

  Id relay = f.p.addFunction("relay");
  f.p.addBlock(relay);
  Id rp = f.value("rp", relay), rv = f.value("rv", relay),
     rres = f.value("rres", relay);
  f.p.functions[relay].parameters = {rp, rv};
  Instruction inner;
  inner.opcode = Opcode::Call;
  inner.callee = writer;
  inner.result = rres;
  inner.operands = {rp, rv};
  f.p.append(relay, 0, inner);
  f.inst(Opcode::Return, INVALID, {rres}, relay);

  f.inst(Opcode::Store, INVALID, {ppp, p_addr});
  Id r1 = f.value("r1"), r2 = f.value("r2");
  Instruction c1;
  c1.opcode = Opcode::Call;
  c1.callee = relay;
  c1.result = r1;
  c1.operands = {p_addr, pa};
  f.p.append(f.main, 0, c1);

  Id mid = f.value("mid"), l1 = f.value("l1"), l2 = f.value("l2");
  Id sMid = f.inst(Opcode::Load, mid, {ppp});
  Id sL1 = f.inst(Opcode::Load, l1, {mid});

  Instruction c2;
  c2.opcode = Opcode::Call;
  c2.callee = relay;
  c2.result = r2;
  c2.operands = {mid, pb};
  f.p.append(f.main, 0, c2);
  Id sL2 = f.inst(Opcode::Load, l2, {mid});
  Id sRet = f.inst(Opcode::Return);
  (void)sMid;

  Options demand_opt = refined();
  demand_opt.eager_dovetail_summaries = false;
  Options eager_opt = refined();
  eager_opt.eager_dovetail_summaries = true;

  Analysis demand(f.p, demand_opt);
  Analysis eager(f.p, eager_opt);
  eager.precomputeAll();

  // Eager Algorithm 2 precomputation must populate summaries immediately.
  CHECK(eager.statistics().summary_tuples > 0);
  CHECK(eager.pointsTo(l1, sL1, {}, Point::After).points_to == PointsToSet(a));
  CHECK(eager.pointsTo(l2, sL2, {}, Point::After).points_to == PointsToSet(b));
  CHECK(eager.pointsTo(r1, sRet).points_to ==
        demand.pointsTo(r1, sRet).points_to);
  CHECK(eager.pointsTo(r2, sRet).points_to ==
        demand.pointsTo(r2, sRet).points_to);
  CHECK(eager.pointsToAllContexts(l1, sL1, Point::After).points_to ==
        demand.pointsToAllContexts(l1, sL1, Point::After).points_to);
  CHECK(eager.pointsToAllContexts(l2, sL2, Point::After).points_to ==
        demand.pointsToAllContexts(l2, sL2, Point::After).points_to);
}

void calleeInCallerLoopGuardContradictionSoundness() {
  // Non-loop callee `maybe_write(ptr, val)` called inside a loop in `main`:
  // Iteration 0 writes `x = &c`, iteration 1 writes `y = &c`. Walking backward
  // across two loop iterations conjoins `([C], s, p -/-> x)` and `([C], s, p -> x)`.
  // `atomsContradict` must NOT prune this path since `C` is in a loop!
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), c = f.object("c");
  Id x = f.object("x"), y = f.object("y");
  Id pa = f.addr(a), pb = f.addr(b), pc = f.addr(c);
  Id px = f.addr(x), py = f.addr(y);

  Id maybeWrite = f.p.addFunction("maybe_write");
  f.p.addBlock(maybeWrite);
  Id mp = f.value("mp", maybeWrite), mv = f.value("mv", maybeWrite);
  f.p.functions[maybeWrite].parameters = {mp, mv};
  f.inst(Opcode::Store, INVALID, {mp, mv}, maybeWrite, 0);
  f.inst(Opcode::Return, INVALID, {}, maybeWrite, 0);

  Id loop = f.p.addBlock(f.main);
  Id exit = f.p.addBlock(f.main);
  Id ptr = f.value("ptr");
  Id res = f.value("res");

  f.inst(Opcode::Store, INVALID, {px, pa}, f.main, 0);
  f.inst(Opcode::Store, INVALID, {py, pb}, f.main, 0);
  Edge toLoop;
  toLoop.target = loop;
  toLoop.phi = {{ptr, px}};
  f.p.functions[f.main].blocks[0].successors.push_back(toLoop);

  f.call(maybeWrite, INVALID, {ptr, pc}, f.main, loop);
  Edge back;
  back.target = loop;
  back.phi = {{ptr, py}};
  Edge out;
  out.target = exit;
  f.p.functions[f.main].blocks[loop].successors.push_back(back);
  f.p.functions[f.main].blocks[loop].successors.push_back(out);

  Id sLoad = f.inst(Opcode::Load, res, {px}, f.main, exit);
  f.inst(Opcode::Return, INVALID, {}, f.main, exit);

  Options gOpt = refined();
  gOpt.solver_mode = SolverMode::GuardedSummary;
  Options tOpt = refined();
  tOpt.solver_mode = SolverMode::InputStateTabulation;

  Analysis guarded(f.p, gOpt);
  Analysis tab(f.p, tOpt);

  QueryResult gr = guarded.pointsTo(res, sLoad, {}, Point::After);
  QueryResult tr = tab.pointsTo(res, sLoad, {}, Point::After);
  CHECK(gr.reachable());
  CHECK(gr.points_to.contains(c));
  CHECK(tr.points_to.contains(c));
  CHECK(gr.points_to == tr.points_to);
}

void emptyBlockGuardAndPathPreservation() {
  // Two paths from `bUse` reach consecutive empty blocks in the reverse CFG:
  // Path 1 enters `bEmpty2` FIRST carrying `!c0` (which later contradicts
  // `entry -> bLeft` with `c0=true`); Path 2 enters `bEmpty2` SECOND carrying
  // `c0=true` (feasible, delivering `&a`). `seen_eb` must key on `(guard, path)`
  // so Path 1 cannot shadow Path 2!
  Fixture f;
  Id a = f.object("a");
  Id slot = f.object("slot");
  Id pa = f.addr(a), pslot = f.addr(slot);
  Id c0 = f.p.addCondition();

  Id bLeft = f.p.addBlock(f.main);
  Id bEmpty1 = f.p.addBlock(f.main); // intentionally empty
  Id bEmpty2 = f.p.addBlock(f.main); // intentionally empty
  Id bBadMid = f.p.addBlock(f.main); // intentionally empty
  Id bGoodMid = f.p.addBlock(f.main); // intentionally empty
  Id bUse = f.p.addBlock(f.main);

  f.inst(Opcode::Store, INVALID, {pslot, pa}, f.main, 0);
  f.p.functions[f.main].blocks[0].successors.push_back({bLeft, {}, c0, true});

  f.inst(Opcode::Nop, INVALID, {}, f.main, bLeft);
  f.p.functions[f.main].blocks[bLeft].successors.push_back(
      {bEmpty1, {}, INVALID, true});
  f.p.functions[f.main].blocks[bEmpty1].successors.push_back(
      {bEmpty2, {}, INVALID, true});

  // bEmpty2 splits to bBadMid (c0=false) and bGoodMid (c0=true), both empty,
  // both merging into bUse (bBadMid registered first so backward BFS visits
  // bEmpty2 with `!c0` before `c0`!).
  f.p.functions[f.main].blocks[bEmpty2].successors.push_back(
      {bBadMid, {}, c0, false});
  f.p.functions[f.main].blocks[bEmpty2].successors.push_back(
      {bGoodMid, {}, c0, true});
  f.p.functions[f.main].blocks[bBadMid].successors.push_back(
      {bUse, {}, INVALID, true});
  f.p.functions[f.main].blocks[bGoodMid].successors.push_back(
      {bUse, {}, INVALID, true});

  Id res = f.value("res");
  Id sLoad = f.inst(Opcode::Load, res, {pslot}, f.main, bUse);
  f.inst(Opcode::Return, INVALID, {}, f.main, bUse);

  Options psOpt = refined();
  psOpt.path_sensitive = true;
  Analysis aa(f.p, psOpt);
  QueryResult qr = aa.pointsTo(res, sLoad, {}, Point::After);
  CHECK(qr.reachable());
  CHECK(qr.points_to == PointsToSet(a));
}

void deadBranchPruneDoesNotFakeUnreachable() {
  // Reachable point reading an uninitialized local alloca (initial = {}) after
  // an unrelated dead branch `if (c) { if (!c) *slot = &a; }` was pruned must
  // remain Complete (with top), NOT falsely reported as Unreachable!
  Fixture f;
  Id a = f.object("a");
  Id pa = f.addr(a);
  Id slot = f.object("slot", PointsToSet{}, true, ObjectKind::Stack);
  Id c = f.p.addCondition();

  Id bT1 = f.p.addBlock(f.main);
  Id bDead = f.p.addBlock(f.main);
  Id bAfter = f.p.addBlock(f.main);

  Id pslot = f.value("pslot");
  Instruction alloc;
  alloc.opcode = Opcode::Allocate;
  alloc.object = slot;
  alloc.result = pslot;
  f.p.append(f.main, 0, alloc);
  f.p.functions[f.main].blocks[0].successors.push_back({bT1, {}, c, true});
  f.p.functions[f.main].blocks[0].successors.push_back({bAfter, {}, c, false});

  f.inst(Opcode::Nop, INVALID, {}, f.main, bT1);
  f.p.functions[f.main].blocks[bT1].successors.push_back({bDead, {}, c, false});
  f.p.functions[f.main].blocks[bT1].successors.push_back({bAfter, {}, c, true});

  f.inst(Opcode::Store, INVALID, {pslot, pa}, f.main, bDead);
  Id deadVal = f.value("dead_val");
  Id sDeadLoad = f.inst(Opcode::Load, deadVal, {pslot}, f.main, bDead);
  f.p.functions[f.main].blocks[bDead].successors.push_back({bAfter, {}, INVALID, true});

  Id liveVal = f.value("live_val");
  Id sLiveLoad = f.inst(Opcode::Load, liveVal, {pslot}, f.main, bAfter);
  f.inst(Opcode::Return, INVALID, {}, f.main, bAfter);

  Options psOpt = refined();
  psOpt.path_sensitive = true;
  Analysis aa(f.p, psOpt);

  // Dead branch itself IS genuinely unreachable:
  CHECK(aa.pointsTo(deadVal, sDeadLoad, {}, Point::After).status ==
        QueryStatus::Unreachable);
  CHECK(aa.pointsToAllContexts(deadVal, sDeadLoad, Point::After).status ==
        QueryStatus::Unreachable);

  // Post-merge live load IS reachable (even though dead branch was pruned):
  QueryResult liveFscs = aa.pointsTo(liveVal, sLiveLoad, {}, Point::After);
  CHECK(liveFscs.status == QueryStatus::Complete);
  CHECK(liveFscs.points_to.isTop());

  QueryResult liveFsci = aa.pointsToAllContexts(liveVal, sLiveLoad, Point::After);
  CHECK(liveFsci.status == QueryStatus::Complete);
  CHECK(liveFsci.points_to.isTop());
}

void interproceduralUpwardPathSensitiveKillAndAliases() {
  // Part A: `main` branches on `!c`, kills `c` (e.g., mutating a global flag),
  // then calls `callee`, which branches on `c`. Upward splicing into `main`
  // must quantify `c` before conjoining with `!c`, keeping `callee` reachable.
  Fixture f;
  Id a = f.object("a"), b = f.object("b");
  Id slot = f.object("slot", PointsToSet(a));
  Id pa = f.addr(a), pb = f.addr(b), pslot = f.addr(slot);
  Id c = f.p.addCondition();

  Id callee = f.p.addFunction("callee");
  f.p.addBlock(callee);
  Id cbThen = f.p.addBlock(callee);
  Id cbElse = f.p.addBlock(callee);
  f.inst(Opcode::Nop, INVALID, {}, callee, 0);
  f.p.functions[callee].blocks[0].successors.push_back({cbThen, {}, c, true});
  f.p.functions[callee].blocks[0].successors.push_back({cbElse, {}, c, false});

  f.inst(Opcode::Store, INVALID, {pslot, pb}, callee, cbThen);
  Id vThen = f.value("v_then", callee);
  Id sThen = f.inst(Opcode::Load, vThen, {pslot}, callee, cbThen);
  f.inst(Opcode::Return, INVALID, {}, callee, cbThen);
  f.inst(Opcode::Return, INVALID, {}, callee, cbElse);

  Id bCall = f.p.addBlock(f.main);
  Id bDone = f.p.addBlock(f.main);
  f.inst(Opcode::Store, INVALID, {pslot, pa}, f.main, 0);
  f.p.functions[f.main].blocks[0].successors.push_back({bCall, {}, c, false});
  f.p.functions[f.main].blocks[0].successors.push_back({bDone, {}, c, true});

  // Mutate / kill `c` before calling `callee`!
  Instruction killC;
  killC.opcode = Opcode::Nop;
  killC.defines_condition = c;
  f.p.append(f.main, bCall, killC);
  Id cs = f.call(callee, INVALID, {}, f.main, bCall);
  Id afterMain = f.value("after_main");
  Id sAfter = f.inst(Opcode::Load, afterMain, {pslot}, f.main, bCall);
  f.inst(Opcode::Return, INVALID, {}, f.main, bCall);
  f.inst(Opcode::Return, INVALID, {}, f.main, bDone);

  Options opt = refined();
  opt.path_sensitive = true;
  opt.path_sensitive_interprocedural = true;
  Analysis aa(f.p, opt);

  QueryResult insideFscs = aa.pointsTo(vThen, sThen, {cs}, Point::After);
  CHECK(insideFscs.reachable());
  CHECK(insideFscs.points_to == PointsToSet(b));

  QueryResult insideFsci = aa.pointsToAllContexts(vThen, sThen, Point::After);
  CHECK(insideFsci.reachable());
  CHECK(insideFsci.points_to == PointsToSet(b));

  QueryResult afterRes = aa.pointsTo(afterMain, sAfter, {}, Point::After);
  CHECK(afterRes.reachable());
  CHECK(afterRes.points_to.contains(b));
}

void steensgaardNoNullBridge() {
  // Storing `null` into disjoint stack slots (or merging `null` in disjoint
  // PHIs) must NOT unify unrelated partitions through a global NULL vertex.
  Fixture f;
  Id a = f.object("a"), b = f.object("b");
  Id s1 = f.object("s1", PointsToSet{}, true, ObjectKind::Stack);
  Id s2 = f.object("s2", PointsToSet{}, true, ObjectKind::Stack);
  Id pa = f.addr(a), pb = f.addr(b), ps1 = f.addr(s1), ps2 = f.addr(s2);
  Id cnull = f.addr(NULL_OBJECT);

  Id b1 = f.p.addBlock(f.main), b2 = f.p.addBlock(f.main);
  f.inst(Opcode::Store, INVALID, {ps1, cnull}, f.main, 0);
  f.inst(Opcode::Store, INVALID, {ps2, cnull}, f.main, 0);
  f.p.functions[f.main].blocks[0].successors = {{b1, {}}, {b2, {}}};
  f.inst(Opcode::Store, INVALID, {ps1, pa}, f.main, b1);
  f.inst(Opcode::Store, INVALID, {ps2, pb}, f.main, b1);
  f.p.functions[f.main].blocks[b1].successors = {{b2, {}}};

  Id r1 = f.value("r1"), r2 = f.value("r2");
  Id l1 = f.inst(Opcode::Load, r1, {ps1}, f.main, b2);
  Id l2 = f.inst(Opcode::Load, r2, {ps2}, f.main, b2);
  f.inst(Opcode::Return, INVALID, {}, f.main, b2);

  Options opt = refined();
  opt.andersen_threshold = 1000; // purely Steensgaard partitioning
  Analysis aa(f.p, opt);
  // `r1` and `r2` must remain in distinct Steensgaard partitions!
  CHECK(aa.clustersFor(r1) != aa.clustersFor(r2));
  CHECK(aa.pointsTo(r1, l1, {}, Point::After).points_to ==
        pts({a, NULL_OBJECT}));
  CHECK(aa.pointsTo(r2, l2, {}, Point::After).points_to ==
        pts({b, NULL_OBJECT}));
}

void oneLevelFlowMemoryPrecision() {
  // Genuine Das (2000) one-level-flow over memory: Steensgaard unifies `s1`
  // and `s2` via a shared read-only source `common` stored into both slots,
  // while directional memory-slot inclusion separates `s1` (also holds `a`)
  // from `s2` (also holds `b`) without running Andersen!
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), c = f.object("c");
  Id s1 = f.object("s1", PointsToSet{}, true, ObjectKind::Stack);
  Id s2 = f.object("s2", PointsToSet{}, true, ObjectKind::Stack);
  Id pa = f.addr(a), pb = f.addr(b), pc = f.addr(c);
  Id ps1 = f.addr(s1), ps2 = f.addr(s2);
  Id shared = f.value("shared");
  Id b1 = f.p.addBlock(f.main), b2 = f.p.addBlock(f.main);
  f.inst(Opcode::Copy, shared, {pc}, f.main, 0);
  f.p.functions[f.main].blocks[0].successors = {{b1, {}}, {b2, {}}};
  f.inst(Opcode::Store, INVALID, {ps1, shared}, f.main, b1);
  f.inst(Opcode::Store, INVALID, {ps2, shared}, f.main, b1);
  f.p.functions[f.main].blocks[b1].successors = {{b2, {}}};
  f.inst(Opcode::Store, INVALID, {ps1, pa}, f.main, b2);
  f.inst(Opcode::Store, INVALID, {ps2, pb}, f.main, b2);
  Id l1 = f.value("l1"), l2 = f.value("l2");
  Id sL1 = f.inst(Opcode::Load, l1, {ps1}, f.main, b2);
  Id sL2 = f.inst(Opcode::Load, l2, {ps2}, f.main, b2);
  f.inst(Opcode::Return, INVALID, {}, f.main, b2);

  Options olfOpt = refined();
  olfOpt.enable_one_level_flow = true;
  olfOpt.andersen_threshold = 4; // Steensgaard merges > 4; OLF splits <= 4!
  Analysis olf(f.p, olfOpt);
  CHECK(olf.statistics().one_level_flow_runs > 0);
  CHECK(olf.statistics().andersen_runs == 0);
  CHECK(olf.statistics().largest_cluster <
        olf.statistics().steensgaard_largest_partition);
  CHECK(olf.pointsTo(l1, sL1, {}, Point::After).points_to == PointsToSet(a));
  CHECK(olf.pointsTo(l2, sL2, {}, Point::After).points_to == PointsToSet(b));

  Options tabOpt = refined();
  tabOpt.solver_mode = SolverMode::InputStateTabulation;
  Analysis tab(f.p, tabOpt);
  CHECK(olf.pointsTo(l1, sL1, {}, Point::After).points_to ==
        tab.pointsTo(l1, sL1, {}, Point::After).points_to);
  CHECK(olf.pointsTo(l2, sL2, {}, Point::After).points_to ==
        tab.pointsTo(l2, sL2, {}, Point::After).points_to);
}

void fieldAwareDovetailCyclicStruct() {
  // Cyclic field-sensitive struct `Node { i8* data @0, Node* next @8 }`
  // traversed in a loop via `Opcode::Field(+8)` + `Opcode::Load`.
  Fixture f;
  Id a = f.object("a"), b = f.object("b");
  Id pa = f.addr(a), pb = f.addr(b);
  auto makeNode = [&](const char *name) -> std::pair<Id, Id> {
    Object base;
    base.name = name;
    base.kind = ObjectKind::Heap;
    base.singleton = false;
    base.bytes = 8;
    base.size_bytes = 16;
    base.field_sensitive = true;
    Id bid = f.p.addObject(base);
    Object next_f;
    next_f.name = std::string(name) + "+8";
    next_f.kind = ObjectKind::Heap;
    next_f.singleton = false;
    next_f.bytes = 8;
    next_f.size_bytes = 8;
    Id nid = f.p.addFieldObject(bid, 8, next_f);
    return {bid, nid};
  };
  auto [n1, n1_next] = makeNode("n1");
  auto [n2, n2_next] = makeNode("n2");
  (void)n1_next;
  (void)n2_next;
  Id pn1 = f.addr(n1), pn2 = f.addr(n2);

  Id f1Next = f.value("f1_next"), f2Next = f.value("f2_next");
  Instruction g1;
  g1.opcode = Opcode::Field;
  g1.result = f1Next;
  g1.operands = {pn1};
  g1.field_offset = 8;
  g1.field_known = true;
  f.p.append(f.main, 0, g1);

  Instruction g2;
  g2.opcode = Opcode::Field;
  g2.result = f2Next;
  g2.operands = {pn2};
  g2.field_offset = 8;
  g2.field_known = true;
  f.p.append(f.main, 0, g2);

  f.inst(Opcode::Store, INVALID, {pn1, pa}, f.main, 0);
  f.inst(Opcode::Store, INVALID, {f1Next, pn2}, f.main, 0);
  f.inst(Opcode::Store, INVALID, {pn2, pb}, f.main, 0);
  f.inst(Opcode::Store, INVALID, {f2Next, pn1}, f.main, 0);

  Id loop = f.p.addBlock(f.main), exit = f.p.addBlock(f.main);
  Id cur = f.value("cur"), curNextPtr = f.value("cur_next_ptr"),
     nextVal = f.value("next_val"), dataVal = f.value("data_val");
  f.p.functions[f.main].blocks[0].successors = {{loop, {{cur, pn1}}}};
  Instruction gCur;
  gCur.opcode = Opcode::Field;
  gCur.result = curNextPtr;
  gCur.operands = {cur};
  gCur.field_offset = 8;
  gCur.field_known = true;
  f.p.append(f.main, loop, gCur);
  f.inst(Opcode::Load, nextVal, {curNextPtr}, f.main, loop);
  Id sData = f.inst(Opcode::Load, dataVal, {cur}, f.main, loop);
  f.p.functions[f.main].blocks[loop].successors = {
      {loop, {{cur, nextVal}}}, {exit, {}}};
  f.inst(Opcode::Return, INVALID, {}, f.main, exit);

  Options gOpt = refined();
  gOpt.solver_mode = SolverMode::GuardedSummary;
  Options tOpt = refined();
  tOpt.solver_mode = SolverMode::InputStateTabulation;
  Analysis guarded(f.p, gOpt);
  Analysis tab(f.p, tOpt);

  CHECK(guarded.pointsTo(cur, sData).points_to == pts({n1, n2}));
  CHECK(guarded.pointsTo(dataVal, sData, {}, Point::After).points_to ==
        pts({a, b}));
  CHECK(guarded.pointsTo(dataVal, sData, {}, Point::After).points_to ==
        tab.pointsTo(dataVal, sData, {}, Point::After).points_to);
}

void compoundRobddSelectEdgePtrGuardsAndCallCondSubst() {
  // Part A: Compound ROBDD (And/Or/Not) + path-sensitive Select (`Opcode::Join`
  // with `select_cond_var`).
  {
    Fixture f;
    Id a = f.object("a"), b = f.object("b");
    Id pa = f.addr(a), pb = f.addr(b);
    Id c1 = f.p.addCondition(), c2 = f.p.addCondition();
    Id c1AndC2 = f.p.addCondAnd(c1, c2);
    Id notC1 = f.p.addCondNot(c1);
    Id sel = f.value("sel"), out = f.value("out");

    Id bThen = f.p.addBlock(f.main), bElse = f.p.addBlock(f.main);
    Instruction joinInst;
    joinInst.opcode = Opcode::Join;
    joinInst.result = sel;
    joinInst.operands = {pa, pb};
    joinInst.select_cond_var = c1;
    joinInst.select_cond_polarity = true;
    f.p.append(f.main, 0, joinInst);
    f.p.functions[f.main].blocks[0].successors = {
        {bThen, {}, c1AndC2, true}, {bElse, {}, notC1, true}};

    Id sThen = f.inst(Opcode::Copy, out, {sel}, f.main, bThen);
    f.inst(Opcode::Return, INVALID, {}, f.main, bThen);
    Id out2 = f.value("out2");
    Id sElse = f.inst(Opcode::Copy, out2, {sel}, f.main, bElse);
    f.inst(Opcode::Return, INVALID, {}, f.main, bElse);

    Options opt = refined();
    opt.path_sensitive = true;
    Analysis aa(f.p, opt);
    CHECK(aa.pointsTo(out, sThen, {}, Point::After).points_to ==
          PointsToSet(a));
    CHECK(aa.pointsTo(out2, sElse, {}, Point::After).points_to ==
          PointsToSet(b));
  }

  // Part B: EdgePtrGuard pruning (`p == &a` vs `p != &a` context-sensitively,
  // plus `NULL == NULL` soundness).
  {
    Fixture f;
    Id a = f.object("a"), b = f.object("b"), x = f.object("x");
    Id outSlot = f.object("out_slot", PointsToSet{}, true, ObjectKind::Stack);
    Id pa = f.addr(a), pb = f.addr(b), px = f.addr(x);
    Id pout = f.addr(outSlot);
    Id cnull = f.addr(NULL_OBJECT);

    Id helper = f.p.addFunction("helper");
    f.p.addBlock(helper);
    Id bEq = f.p.addBlock(helper), bNe = f.p.addBlock(helper),
       bJoin = f.p.addBlock(helper);
    Id hp = f.value("hp", helper);
    f.p.functions[helper].parameters = {hp};
    Id brSite = f.inst(Opcode::Nop, INVALID, {}, helper, 0);
    Edge eEq;
    eEq.target = bEq;
    eEq.ptr_guards.push_back({EdgeGuardKind::PointsTo, hp, a, brSite});
    Edge eNe;
    eNe.target = bNe;
    eNe.ptr_guards.push_back({EdgeGuardKind::NotPointsTo, hp, a, brSite});
    f.p.functions[helper].blocks[0].successors = {eEq, eNe};
    f.inst(Opcode::Store, INVALID, {pout, px}, helper, bEq);
    f.p.functions[helper].blocks[bEq].successors = {{bJoin, {}}};
    f.inst(Opcode::Store, INVALID, {pout, hp}, helper, bNe);
    f.p.functions[helper].blocks[bNe].successors = {{bJoin, {}}};
    f.inst(Opcode::Return, INVALID, {}, helper, bJoin);

    f.call(helper, INVALID, {pa}, f.main, 0);
    Id r1 = f.value("r1"), r2 = f.value("r2");
    Id sR1 = f.inst(Opcode::Load, r1, {pout}, f.main, 0);
    f.call(helper, INVALID, {pb}, f.main, 0);
    Id sR2 = f.inst(Opcode::Load, r2, {pout}, f.main, 0);

    // NULL == NULL branch must remain reachable (soundness!).
    Id np1 = f.value("np1"), np2 = f.value("np2");
    f.inst(Opcode::Copy, np1, {cnull}, f.main, 0);
    Id sCmp = f.inst(Opcode::Copy, np2, {cnull}, f.main, 0);
    Id bNullEq = f.p.addBlock(f.main), bEnd = f.p.addBlock(f.main);
    Edge eNullEq;
    eNullEq.target = bNullEq;
    eNullEq.ptr_guards.push_back({EdgeGuardKind::Alias, np1, np2, sCmp});
    f.p.functions[f.main].blocks[0].successors = {eNullEq, {bEnd, {}}};
    Id rNull = f.value("r_null");
    Id sNull = f.inst(Opcode::Copy, rNull, {px}, f.main, bNullEq);
    f.p.functions[f.main].blocks[bNullEq].successors = {{bEnd, {}}};
    f.inst(Opcode::Return, INVALID, {}, f.main, bEnd);

    Options opt = refined();
    opt.path_sensitive = true;
    Analysis aa(f.p, opt);
    CHECK(aa.pointsTo(r1, sR1, {}, Point::After).points_to == PointsToSet(x));
    CHECK(aa.pointsTo(r2, sR2, {}, Point::After).points_to == PointsToSet(b));
    auto nr = aa.pointsTo(rNull, sNull, {}, Point::After);
    CHECK(nr.reachable());
    CHECK(nr.points_to == PointsToSet(x));
  }

  // Part C: General call-site `cond_subst` for constant and variable actuals.
  {
    Fixture f;
    Id a = f.object("a"), b = f.object("b");
    Id s1 = f.object("s1", PointsToSet{}, true, ObjectKind::Stack);
    Id s2 = f.object("s2", PointsToSet{}, true, ObjectKind::Stack);
    Id s3 = f.object("s3", PointsToSet{}, true, ObjectKind::Stack);
    Id pa = f.addr(a), pb = f.addr(b);
    Id ps1 = f.addr(s1), ps2 = f.addr(s2), ps3 = f.addr(s3);
    Id calleeCond = f.p.addCondition();
    Id callerCond = f.p.addCondition();

    Id pick = f.p.addFunction("pick");
    f.p.functions[pick].entry_kills_conditions = {calleeCond};
    f.p.addBlock(pick);
    Id bT = f.p.addBlock(pick), bF = f.p.addBlock(pick),
       bDone = f.p.addBlock(pick);
    Id dst = f.value("dst", pick);
    f.p.functions[pick].parameters = {dst};
    f.inst(Opcode::Nop, INVALID, {}, pick, 0);
    f.p.functions[pick].blocks[0].successors = {{bT, {}, calleeCond, true},
                                                {bF, {}, calleeCond, false}};
    f.inst(Opcode::Store, INVALID, {dst, pa}, pick, bT);
    f.p.functions[pick].blocks[bT].successors = {{bDone, {}}};
    f.inst(Opcode::Store, INVALID, {dst, pb}, pick, bF);
    f.p.functions[pick].blocks[bF].successors = {{bDone, {}}};
    f.inst(Opcode::Return, INVALID, {}, pick, bDone);

    Instruction cTrue;
    cTrue.opcode = Opcode::Call;
    cTrue.callee = pick;
    cTrue.operands = {ps1};
    cTrue.cond_subst.push_back({calleeCond, INVALID, true});
    f.p.append(f.main, 0, cTrue);

    Instruction cFalse;
    cFalse.opcode = Opcode::Call;
    cFalse.callee = pick;
    cFalse.operands = {ps2};
    cFalse.cond_subst.push_back({calleeCond, INVALID, false});
    f.p.append(f.main, 0, cFalse);

    Id bMainT = f.p.addBlock(f.main), bMainF = f.p.addBlock(f.main);
    f.p.functions[f.main].blocks[0].successors = {
        {bMainT, {}, callerCond, true}, {bMainF, {}, callerCond, false}};
    Instruction cVar;
    cVar.opcode = Opcode::Call;
    cVar.callee = pick;
    cVar.operands = {ps3};
    cVar.cond_subst.push_back({calleeCond, callerCond, true});
    f.p.append(f.main, bMainT, cVar);

    Id r1 = f.value("r1"), r2 = f.value("r2"), r3 = f.value("r3");
    Id l1 = f.inst(Opcode::Load, r1, {ps1}, f.main, bMainT);
    Id l2 = f.inst(Opcode::Load, r2, {ps2}, f.main, bMainT);
    Id l3 = f.inst(Opcode::Load, r3, {ps3}, f.main, bMainT);
    f.inst(Opcode::Return, INVALID, {}, f.main, bMainT);
    f.inst(Opcode::Return, INVALID, {}, f.main, bMainF);

    Options opt = refined();
    opt.path_sensitive = true;
    opt.path_sensitive_interprocedural = true;
    Analysis aa(f.p, opt);
    CHECK(aa.pointsTo(r1, l1, {}, Point::After).points_to == PointsToSet(a));
    CHECK(aa.pointsTo(r2, l2, {}, Point::After).points_to == PointsToSet(b));
    CHECK(aa.pointsTo(r3, l3, {}, Point::After).points_to == PointsToSet(a));
  }
}

void perPartitionAndersenRejectionAndHybridSolver() {
  // Part A: Per-partition Andersen rejection without global escalation.
  {
    Fixture f;
    // Partition 1 (size 4): cyclic copies where Andersen cannot reduce size.
    Id a = f.object("a"), pa = f.addr(a);
    Id u1 = f.value("u1"), u2 = f.value("u2"), u3 = f.value("u3");
    Id b1 = f.p.addBlock(f.main), b2 = f.p.addBlock(f.main);
    f.p.functions[f.main].blocks[0].successors = {{b1, {{u1, pa}}},
                                                  {b2, {{u1, pa}}}};
    f.inst(Opcode::Copy, u2, {u1}, f.main, b1);
    f.inst(Opcode::Copy, u3, {u2}, f.main, b1);
    f.p.functions[f.main].blocks[b1].successors = {{b2, {}}};
    // Partition 2 (size 5): directional fan-in where Andersen splits cleanly.
    Id x = f.object("x"), y = f.object("y");
    Id px = f.addr(x), py = f.addr(y);
    Id vx = f.value("vx"), vy = f.value("vy"), vm = f.value("vm");
    f.inst(Opcode::Copy, vx, {px}, f.main, b2);
    f.inst(Opcode::Copy, vy, {py}, f.main, b2);
    f.inst(Opcode::Join, vm, {vx, vy}, f.main, b2);
    Id sRet = f.inst(Opcode::Return, INVALID, {}, f.main, b2);

    Options opt = refined();
    opt.andersen_threshold = 2;
    opt.adaptive_andersen_threshold = true;
    opt.adaptive_global_threshold_escalation = false;
    Analysis aa(f.p, opt);
    CHECK(aa.statistics().effective_andersen_threshold == 2);
    CHECK(aa.statistics().andersen_runs >= 2);
    CHECK(aa.pointsTo(vx, sRet).points_to == PointsToSet(x));
    CHECK(aa.pointsTo(vy, sRet).points_to == PointsToSet(y));
  }

  // Part B: Hybrid per-cluster solver selection (threshold + custom selector).
  {
    Fixture f;
    Id a = f.object("a"), b = f.object("b"), c = f.object("c");
    Id pa = f.addr(a), pb = f.addr(b), pc = f.addr(c);
    Id sSmall = f.object("s_small", PointsToSet{}, true, ObjectKind::Stack);
    Id psSmall = f.addr(sSmall);
    f.inst(Opcode::Store, INVALID, {psSmall, pa});
    Id vSmall = f.value("v_small");
    Id sLoadSmall = f.inst(Opcode::Load, vSmall, {psSmall});

    Id p1 = f.value("p1"), p2 = f.value("p2"), p3 = f.value("p3"),
       p4 = f.value("p4"), p5 = f.value("p5");
    f.inst(Opcode::Copy, p1, {pb});
    f.inst(Opcode::Copy, p2, {p1});
    f.inst(Opcode::Copy, p3, {p2});
    f.inst(Opcode::Join, p4, {p3, pc});
    Id sP5 = f.inst(Opcode::Copy, p5, {p4});
    f.inst(Opcode::Return);

    Options hOpt = refined();
    hOpt.hybrid_tabulation_max_cluster_size = 3;
    std::size_t selector_calls = 0;
    hOpt.cluster_solver_selector = [&](std::size_t, std::size_t csz,
                                       std::size_t) {
      ++selector_calls;
      return csz <= 3 ? SolverMode::InputStateTabulation
                      : SolverMode::GuardedSummary;
    };
    Analysis hybrid(f.p, hOpt);
    CHECK(hybrid.pointsTo(vSmall, sLoadSmall, {}, Point::After).points_to ==
          PointsToSet(a));
    CHECK(hybrid.pointsTo(p5, sP5, {}, Point::After).points_to == pts({b, c}));
    CHECK(selector_calls >= 2);
  }
}

void selectivePrecomputationObjectQueriesAndMustAlias() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b");
  Id gWriteOnly = f.object("g_wo", PointsToSet(NULL_OBJECT), true,
                           ObjectKind::Global);
  Id sOther = f.object("s_other", PointsToSet{}, true, ObjectKind::Stack);
  Id h1 = f.object("h1", PointsToSet{}, false, ObjectKind::Heap);
  Id h2 = f.object("h2", PointsToSet{}, false, ObjectKind::Heap);
  Id pa = f.addr(a), pb = f.addr(b), pg = f.addr(gWriteOnly),
     psOther = f.addr(sOther);
  Id cnull = f.addr(NULL_OBJECT);

  // Write-only global `gWriteOnly` (no Load from `gWriteOnly`!).
  f.inst(Opcode::Store, INVALID, {pg, pa});
  Id aliasOfG = f.value("alias_of_g");
  f.inst(Opcode::Copy, aliasOfG, {pa});

  // Unrelated cluster for selective precomputation.
  f.inst(Opcode::Store, INVALID, {psOther, pb});
  Id vOther = f.value("v_other");
  f.inst(Opcode::Load, vOther, {psOther});

  // Symbolic heap must-alias (`qHeap = pHeap`) vs distinct heap allocations.
  Id pHeap = f.value("p_heap"), qHeap = f.value("q_heap"),
     rHeap = f.value("r_heap");
  Instruction alloc1;
  alloc1.opcode = Opcode::Allocate;
  alloc1.object = h1;
  alloc1.result = pHeap;
  f.p.append(f.main, 0, alloc1);
  f.inst(Opcode::Copy, qHeap, {pHeap});
  Instruction alloc2;
  alloc2.opcode = Opcode::Allocate;
  alloc2.object = h2;
  alloc2.result = rHeap;
  f.p.append(f.main, 0, alloc2);

  // Strict NULL pointers must not alias each other.
  Id n1 = f.value("n1"), n2 = f.value("n2");
  f.inst(Opcode::Copy, n1, {cnull});
  f.inst(Opcode::Copy, n2, {cnull});

  Id sRet = f.inst(Opcode::Return);

  // Test selective precomputation first on a fresh instance.
  {
    Analysis sel(f.p, refined());
    CHECK(sel.statistics().evaluated_clusters == 0);
    const auto otherClusters = sel.clustersFor(vOther);
    CHECK(!otherClusters.empty());
    sel.precomputeForValues({vOther});
    CHECK(sel.statistics().evaluated_clusters == otherClusters.size());
    CHECK(sel.statistics().evaluated_clusters < sel.clusters().size());
  }

  for (SolverMode mode :
       {SolverMode::GuardedSummary, SolverMode::InputStateTabulation}) {
    Options opt = refined();
    opt.solver_mode = mode;
    Analysis aa(f.p, opt);

    // Direct memory-object queries (Paper Fig. 5) on write-only global!
    auto gPts = aa.objectPointsTo(gWriteOnly, sRet);
    CHECK(gPts.reachable());
    CHECK(gPts.points_to == PointsToSet(a));
    auto gFsci = aa.objectPointsToAllContexts(gWriteOnly, sRet);
    CHECK(gFsci.reachable());
    CHECK(gFsci.points_to == PointsToSet(a));
    auto gAl = aa.objectAliases(gWriteOnly, sRet);
    CHECK(gAl.reachable());
    CHECK(std::find(gAl.aliases.begin(), gAl.aliases.end(), aliasOfG) !=
          gAl.aliases.end());

    // Must-alias checks.
    CHECK(aa.mustAlias(aliasOfG, pa, sRet));
    CHECK(!aa.mustAlias(pHeap, rHeap, sRet));
    if (mode == SolverMode::GuardedSummary)
      CHECK(aa.mustAlias(pHeap, qHeap, sRet));

    // Consistent non-null `mayAlias` / `aliases` / `mustAlias` semantics!
    CHECK(!aa.mayAlias(n1, n2, sRet));
    CHECK(!aa.mustAlias(n1, n2, sRet));
    auto nAl = aa.aliases(n1, sRet);
    CHECK(nAl.reachable());
    CHECK(std::find(nAl.aliases.begin(), nAl.aliases.end(), n2) ==
          nAl.aliases.end());
  }
}

void steensgaardTopStorePreservesRhsAndNullAliasExcludesTop() {
  // 1. Storing `&a` through an already-top pointer (`argv`) must NOT merge `a`
  //    into `top` in Steensgaard Pass 1.
  // 2. `aliases(null_ptr)` must NOT include `top_component` values (`argv`)
  //    when `null_ptr` strictly holds `NULL_OBJECT` (`mayAlias == false`).
  Fixture f;
  Id argv = f.value("argv");
  f.p.functions[f.main].parameters = {argv};
  Id a = f.object("a", PointsToSet{}, true, ObjectKind::Stack);
  Id b = f.object("b", PointsToSet{}, true, ObjectKind::Stack);
  Id pa = f.addr(a), pb = f.addr(b);
  Id cnull = f.addr(NULL_OBJECT);
  f.inst(Opcode::Store, INVALID, {argv, pa});
  Id va = f.value("va"), vb = f.value("vb"), vn = f.value("vn");
  f.inst(Opcode::Copy, va, {pa});
  f.inst(Opcode::Copy, vb, {pb});
  f.inst(Opcode::Copy, vn, {cnull});
  Id sRet = f.inst(Opcode::Return);

  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(va, sRet).points_to == PointsToSet(a));
  CHECK(aa.pointsTo(vb, sRet).points_to == PointsToSet(b));
  CHECK(aa.clustersFor(va) != aa.clustersFor(argv));
  CHECK(!aa.mayAlias(vn, argv, sRet));
  auto vnAl = aa.aliases(vn, sRet);
  CHECK(vnAl.reachable());
  CHECK(std::find(vnAl.aliases.begin(), vnAl.aliases.end(), argv) ==
        vnAl.aliases.end());
}

void edgePtrGuardPostDefAndInterprocAnchorAliasSoundness() {
  // Part A: Outgoing EdgePtrGuard from a block ending directly in a defining
  // Load must evaluate at Point::After of that Load (not Point::Before where
  // the loaded SSA value is not yet defined!).
  {
    Fixture f;
    Id a = f.object("a");
    Id g = f.object("g", PointsToSet(a), true, ObjectKind::Global);
    Id pa = f.addr(a), pg = f.addr(g);
    Id b1 = f.p.addBlock(f.main);

    Id v1 = f.value("v1"), v2 = f.value("v2");
    f.inst(Opcode::Copy, v1, {pa}, f.main, 0);
    Id sLoad = f.inst(Opcode::Load, v2, {pg}, f.main, 0);
    Edge e;
    e.target = b1;
    e.ptr_guards.push_back({EdgeGuardKind::Alias, v1, v2, sLoad});
    f.p.functions[f.main].blocks[0].successors = {e};
    Id res = f.value("res");
    Id sRes = f.inst(Opcode::Copy, res, {v2}, f.main, b1);
    f.inst(Opcode::Return, INVALID, {}, f.main, b1);

    Options opt = refined();
    opt.path_sensitive = true;
    Analysis aa(f.p, opt);
    auto qr = aa.pointsTo(res, sRes, {}, Point::After);
    CHECK(qr.reachable());
    CHECK(qr.points_to == PointsToSet(a));
  }

  // Part B: Interprocedural path-sensitive forward anchor propagation when
  // entry global `g` initially holds `&a`, `main` loads `p = *g`, overwrites
  // `*g = &b`, and passes `p` to `callee(param)` which defines `q = &a`.
  // `aliases(param)` MUST include `q` even though `Mem(g)` no longer holds `a`!
  {
    Fixture f;
    Id a = f.object("a"), b = f.object("b");
    Id g = f.object("g", PointsToSet(a), true, ObjectKind::Global);
    Id pa = f.addr(a), pb = f.addr(b), pg = f.addr(g);

    Id callee = f.p.addFunction("callee");
    f.p.addBlock(callee);
    Id param = f.value("param", callee);
    Id q = f.value("q", callee);
    f.p.functions[callee].parameters = {param};
    f.inst(Opcode::Copy, q, {pa}, callee, 0);
    Id cRet = f.inst(Opcode::Return, INVALID, {}, callee, 0);

    Id p = f.value("p");
    f.inst(Opcode::Load, p, {pg}, f.main, 0);
    f.inst(Opcode::Store, INVALID, {pg, pb}, f.main, 0);
    Id cs = f.call(callee, INVALID, {p}, f.main, 0);
    f.inst(Opcode::Return, INVALID, {}, f.main, 0);

    Options opt = refined();
    opt.path_sensitive = true;
    opt.path_sensitive_interprocedural = true;
    Analysis aa(f.p, opt);
    auto alFscs = aa.aliases(param, cRet, {cs});
    CHECK(alFscs.reachable());
    CHECK(std::find(alFscs.aliases.begin(), alFscs.aliases.end(), q) !=
          alFscs.aliases.end());
    auto alFsci = aa.aliasesAllContexts(param, cRet);
    CHECK(alFsci.reachable());
    CHECK(std::find(alFsci.aliases.begin(), alFsci.aliases.end(), q) !=
          alFsci.aliases.end());
  }
}

void arrayFoldingWraparoundInteriorStrongUpdateMutualRecAndLoadFuncSoundness() {
  // Part A & B: A raw non-field-sensitive singleton (`size_bytes == 0`) with
  // an interior `Opcode::Field` offset must NOT strong-update across distinct
  // offsets, must NOT `mustAlias(p, q)`, and must NOT let an `EdgePtrGuard`
  // `p != q` contradict a Def. 8 store-alias guard on the same object.
  {
    Fixture f;
    Id x = f.object("x"), y = f.object("y");
    Id buf = f.object("buf", PointsToSet{}, /*singleton=*/true);
    Id px = f.addr(x), py = f.addr(y), pbuf = f.addr(buf);
    Id b1 = f.p.addBlock(f.main);

    Id q = f.value("q");
    Instruction fld;
    fld.opcode = Opcode::Field;
    fld.result = q;
    fld.operands = {pbuf};
    fld.field_offset = 8;
    fld.field_known = true;
    f.p.append(f.main, 0, fld);

    f.inst(Opcode::Store, INVALID, {pbuf, px}, f.main, 0);
    Edge e;
    e.target = b1;
    e.ptr_guards.push_back({EdgeGuardKind::NotAlias, pbuf, q, INVALID});
    f.p.functions[f.main].blocks[0].successors = {e};

    f.inst(Opcode::Store, INVALID, {q, py}, f.main, b1);
    Id ld = f.value("ld");
    Id sLd = f.inst(Opcode::Load, ld, {pbuf}, f.main, b1);
    Id sRet = f.inst(Opcode::Return, INVALID, {}, f.main, b1);

    for (SolverMode mode :
         {SolverMode::GuardedSummary, SolverMode::InputStateTabulation}) {
      Options opt = refined();
      opt.solver_mode = mode;
      opt.path_sensitive = true;
      Analysis aa(f.p, opt);
      auto qr = aa.pointsTo(ld, sLd, {}, Point::After);
      CHECK(qr.reachable());
      CHECK(qr.points_to.contains(x));
      CHECK(qr.points_to.contains(y));
      CHECK(!aa.mustAlias(pbuf, q, sRet, {}));
    }
  }

  // Part C: Loading from a pointer that may hold both a memory object and a
  // `Function` object must soundly produce Top in both backends (and
  // Steensgaard must agree).
  {
    Fixture f;
    Id target = f.object("target");
    Id m = f.object("m", PointsToSet(target), true, ObjectKind::Global);
    Id fn = f.p.addFunction("dummy");
    f.p.addBlock(fn);
    f.inst(Opcode::Return, INVALID, {}, fn, 0);
    Object fobj;
    fobj.name = "dummy_fn";
    fobj.kind = ObjectKind::Function;
    fobj.function = fn;
    Id fobj_id = f.p.addObject(fobj);

    Id pm = f.addr(m), pf = f.addr(fobj_id);
    Id ptr = f.value("ptr"), ld = f.value("ld");
    f.inst(Opcode::Join, ptr, {pm, pf}, f.main, 0);
    Id sLd = f.inst(Opcode::Load, ld, {ptr}, f.main, 0);
    f.inst(Opcode::Return, INVALID, {}, f.main, 0);

    for (SolverMode mode :
         {SolverMode::GuardedSummary, SolverMode::InputStateTabulation}) {
      Options opt = refined();
      opt.solver_mode = mode;
      Analysis aa(f.p, opt);
      auto qr = aa.pointsTo(ld, sLd, {}, Point::After);
      CHECK(qr.reachable());
      CHECK(qr.points_to.isTop());
    }
  }

  // Part D: Mutual recursion A -> B -> A under `path_sensitive_interprocedural`
  // must not leak frame-1's condition `cA == true` across B into frame-2 of A
  // where `cA == false`.
  {
    Fixture f;
    Id a = f.object("a"), b = f.object("b");
    Id pa = f.addr(a), pb = f.addr(b);
    Id fnA = f.p.addFunction("A");
    Id fnB = f.p.addFunction("B");
    Id a0 = f.p.addBlock(fnA), aTrue = f.p.addBlock(fnA),
       aFalse = f.p.addBlock(fnA);
    Id b0 = f.p.addBlock(fnB);

    Id cA = f.p.addCondition();
    f.p.functions[fnA].entry_kills_conditions.push_back(cA);

    Id pA = f.value("pA", fnA), qA = f.value("qA", fnA);
    f.p.functions[fnA].parameters = {pA};
    f.inst(Opcode::Nop, INVALID, {}, fnA, a0);
    f.p.functions[fnA].blocks[a0].successors = {{aTrue, {}, cA, true},
                                                {aFalse, {}, cA, false}};

    Id csAB = f.call(fnB, INVALID, {pA}, fnA, aTrue);
    f.inst(Opcode::Return, INVALID, {}, fnA, aTrue);

    f.inst(Opcode::Copy, qA, {pa}, fnA, aFalse);
    Id sQuery = f.inst(Opcode::Return, INVALID, {}, fnA, aFalse);

    Id pB = f.value("pB", fnB);
    f.p.functions[fnB].parameters = {pB};
    Instruction callBA;
    callBA.opcode = Opcode::Call;
    callBA.callee = fnA;
    callBA.operands = {pB};
    callBA.cond_subst.push_back({cA, INVALID, false});
    Id csBA = f.p.append(fnB, b0, callBA);
    f.inst(Opcode::Return, INVALID, {}, fnB, b0);

    Instruction callMainA;
    callMainA.opcode = Opcode::Call;
    callMainA.callee = fnA;
    callMainA.operands = {pa};
    callMainA.cond_subst.push_back({cA, INVALID, true});
    Id csMainA = f.p.append(f.main, 0, callMainA);
    f.inst(Opcode::Return, INVALID, {}, f.main, 0);
    (void)pb;

    Options opt = refined();
    opt.path_sensitive = true;
    opt.path_sensitive_interprocedural = true;
    Analysis aa(f.p, opt);
    Context ctx{csMainA, csAB, csBA};
    auto qr = aa.pointsTo(pA, sQuery, ctx);
    CHECK(qr.reachable());
    CHECK(qr.points_to == PointsToSet(a));
    auto alFscs = aa.aliases(pA, sQuery, ctx);
    CHECK(alFscs.reachable());
    CHECK(std::find(alFscs.aliases.begin(), alFscs.aliases.end(), qA) !=
          alFscs.aliases.end());
    auto alFsci = aa.aliasesAllContexts(pA, sQuery);
    CHECK(alFsci.reachable());
    CHECK(std::find(alFsci.aliases.begin(), alFsci.aliases.end(), qA) !=
          alFsci.aliases.end());
  }

  // Part E: Chained unaligned interior `Opcode::Field` arithmetic
  // (`(s + 1) + 15 = s + 16`, `((s + 1) + 15) + 1 = s + 17`) must neither drop
  // field `s+16` in Steensgaard/Tabulation/Guarded nor falsely report
  // `mustAlias(s + 16, s + 17)`!
  {
    Fixture f;
    Id x = f.object("x");
    Id px = f.addr(x);
    Object s0;
    s0.name = "s0";
    s0.kind = ObjectKind::Global;
    s0.singleton = true;
    s0.bytes = 8;
    s0.size_bytes = 24;
    s0.field_sensitive = true;
    s0.initial = PointsToSet(NULL_OBJECT);
    Id bid = f.p.addObject(s0);
    Object f8 = s0;
    f8.name = "s0+8";
    f8.size_bytes = 8;
    f.p.addFieldObject(bid, 8, f8);
    Object f16 = s0;
    f16.name = "s0+16";
    f16.size_bytes = 8;
    Id fid16 = f.p.addFieldObject(bid, 16, f16);

    Id ps0 = f.addr(bid), pf16 = f.addr(fid16);
    Id v_plus1 = f.value("v_plus1");
    Id v_plus16 = f.value("v_plus16");
    Id v_plus17 = f.value("v_plus17");
    Instruction i1;
    i1.opcode = Opcode::Field;
    i1.result = v_plus1;
    i1.operands = {ps0};
    i1.field_offset = 1;
    i1.field_known = true;
    f.p.append(f.main, 0, i1);
    Instruction i2;
    i2.opcode = Opcode::Field;
    i2.result = v_plus16;
    i2.operands = {v_plus1};
    i2.field_offset = 15;
    i2.field_known = true;
    f.p.append(f.main, 0, i2);
    Instruction i3;
    i3.opcode = Opcode::Field;
    i3.result = v_plus17;
    i3.operands = {v_plus16};
    i3.field_offset = 1;
    i3.field_known = true;
    f.p.append(f.main, 0, i3);
    f.inst(Opcode::Store, INVALID, {v_plus16, px}, f.main, 0);
    Id ld = f.value("ld");
    Id sLd = f.inst(Opcode::Load, ld, {pf16}, f.main, 0);
    Id sRet = f.inst(Opcode::Return, INVALID, {}, f.main, 0);

    for (SolverMode mode :
         {SolverMode::GuardedSummary, SolverMode::InputStateTabulation}) {
      Options opt = refined();
      opt.solver_mode = mode;
      opt.path_sensitive = true;
      Analysis aa(f.p, opt);
      auto qr = aa.pointsTo(ld, sLd, {}, Point::After);
      CHECK(qr.reachable());
      CHECK(qr.points_to.contains(x));
      CHECK(!aa.mustAlias(v_plus16, v_plus17, sRet, {}));
    }
  }

  // Part F: Path-pruned dead branch dereference precomputed during
  // `runDovetailing()` must still report `QueryStatus::Unreachable` on
  // `pointsToAllContexts` (not `Complete` + `Top` from `m_fsci_cache` hit).
  {
    Fixture f;
    Id a = f.object("a"), slot = f.object("slot", PointsToSet(a), true);
    Id pa = f.addr(a), pslot = f.addr(slot);
    Id c = f.p.addCondition();
    Id bT = f.p.addBlock(f.main), bDead = f.p.addBlock(f.main),
       bExit = f.p.addBlock(f.main);
    Id p = f.value("p"), x = f.value("x");
    f.inst(Opcode::Copy, p, {pslot}, f.main, 0);
    f.p.functions[f.main].blocks[0].successors = {{bT, {}, c, true},
                                                  {bExit, {}, c, false}};
    f.inst(Opcode::Nop, INVALID, {}, f.main, bT);
    f.p.functions[f.main].blocks[bT].successors = {{bDead, {}, c, false},
                                                   {bExit, {}, c, true}};
    Id sDeadLoad = f.inst(Opcode::Load, x, {p}, f.main, bDead);
    f.p.functions[f.main].blocks[bDead].successors = {{bExit, {}}};
    f.inst(Opcode::Return, INVALID, {}, f.main, bExit);
    (void)pa;

    Options opt = refined();
    opt.path_sensitive = true;
    opt.path_sensitive_interprocedural = false;
    Analysis aa(f.p, opt);
    auto qFscs = aa.pointsTo(p, sDeadLoad, {}, Point::Before);
    auto qFsci = aa.pointsToAllContexts(p, sDeadLoad, Point::Before);
    CHECK(!qFscs.reachable());
    CHECK(!qFsci.reachable());
  }
}

void round5And6UnslicedIndirectCallPackedStoreBddBudgetAndNullAliasSoundness() {
  // Part A: Disjoint-cluster indirect call in TabulationSolver and Hybrid mode:
  // `main` invokes an indirect callee whose function pointer lives in cluster 1,
  // followed by a load in a completely disjoint cluster 2. Cluster 2 must stay
  // reachable and exact across GuardedSummary, Tabulation, and Hybrid modes.
  {
    Program p;
    Object t1;
    t1.name = "t1";
    t1.kind = ObjectKind::Global;
    t1.singleton = true;
    t1.bytes = 8;
    t1.initial = PointsToSet(NULL_OBJECT);
    Id o_t1 = p.addObject(t1);

    Object t2;
    t2.name = "t2";
    t2.kind = ObjectKind::Global;
    t2.singleton = true;
    t2.bytes = 8;
    t2.initial = PointsToSet(NULL_OBJECT);
    Id o_t2 = p.addObject(t2);

    Object g2;
    g2.name = "g2";
    g2.kind = ObjectKind::Global;
    g2.singleton = true;
    g2.bytes = 8;
    g2.initial = PointsToSet(o_t2);
    Id o_g2 = p.addObject(g2);

    Id fn_main = p.addFunction("main");
    Id fn_callee = p.addFunction("callee");
    p.entry = fn_main;

    Object fo;
    fo.name = "fn_callee";
    fo.kind = ObjectKind::Function;
    fo.singleton = false;
    fo.function = fn_callee;
    fo.initial = PointsToSet::top();
    Id o_fn = p.addObject(fo);

    Object gfn;
    gfn.name = "g_fn";
    gfn.kind = ObjectKind::Global;
    gfn.singleton = true;
    gfn.bytes = 8;
    gfn.initial = PointsToSet(o_fn);
    Id o_gfn = p.addObject(gfn);

    Id cb = p.addBlock(fn_callee);
    Id cv = p.addValue({"c.t1", fn_callee, false, {}});
    Instruction c_addr;
    c_addr.opcode = Opcode::Address;
    c_addr.result = cv;
    c_addr.object = o_t1;
    p.append(fn_callee, cb, c_addr);
    Instruction c_ret;
    c_ret.opcode = Opcode::Return;
    c_ret.operands = {cv};
    p.append(fn_callee, cb, c_ret);

    Id mb = p.addBlock(fn_main);
    Id v_gfn = p.addValue({"gfn_p", fn_main, false, {}});
    Id v_fp = p.addValue({"fp", fn_main, false, {}});
    Id v_cr = p.addValue({"cr", fn_main, false, {}});
    Id v_g2 = p.addValue({"g2_p", fn_main, false, {}});
    Id v_q2 = p.addValue({"q2", fn_main, false, {}});

    Instruction i_agfn;
    i_agfn.opcode = Opcode::Address;
    i_agfn.result = v_gfn;
    i_agfn.object = o_gfn;
    p.append(fn_main, mb, i_agfn);

    Instruction i_lfp;
    i_lfp.opcode = Opcode::Load;
    i_lfp.result = v_fp;
    i_lfp.operands = {v_gfn};
    i_lfp.read_bytes = 8;
    p.append(fn_main, mb, i_lfp);

    Instruction i_call;
    i_call.opcode = Opcode::Call;
    i_call.indirect_target = v_fp;
    i_call.result = v_cr;
    p.append(fn_main, mb, i_call);

    Instruction i_ag2;
    i_ag2.opcode = Opcode::Address;
    i_ag2.result = v_g2;
    i_ag2.object = o_g2;
    p.append(fn_main, mb, i_ag2);

    Instruction i_lq2;
    i_lq2.opcode = Opcode::Load;
    i_lq2.result = v_q2;
    i_lq2.operands = {v_g2};
    i_lq2.read_bytes = 8;
    Id s_lq2 = p.append(fn_main, mb, i_lq2);

    Instruction i_ret;
    i_ret.opcode = Opcode::Return;
    i_ret.operands = {v_q2};
    p.append(fn_main, mb, i_ret);
    p.validate();

    for (int mode = 0; mode < 3; ++mode) {
      Options opt = refined();
      if (mode == 0)
        opt.solver_mode = SolverMode::GuardedSummary;
      else if (mode == 1)
        opt.solver_mode = SolverMode::InputStateTabulation;
      else {
        opt.solver_mode = SolverMode::GuardedSummary;
        opt.hybrid_tabulation_max_cluster_size = 8;
      }
      Analysis aa(p, opt);
      auto qr = aa.pointsTo(v_q2, s_lq2, {}, Point::After);
      CHECK(qr.reachable());
      CHECK(qr.points_to == PointsToSet(o_t2));
      auto qf = aa.pointsToAllContexts(v_q2, s_lq2, Point::After);
      CHECK(qf.reachable());
      CHECK(qf.points_to == PointsToSet(o_t2));
    }
  }

  // Part B: Cross-field pointer Store overlapping a sibling field (packed
  // `<{ i32@0 (4B), i8*@4 (8B) }>` written with an 8-byte Store at offset 0)
  // must widen field `+4` to Top across all solvers.
  {
    Program p;
    Object a;
    a.name = "a";
    a.kind = ObjectKind::Global;
    a.singleton = true;
    a.bytes = 8;
    a.initial = PointsToSet(NULL_OBJECT);
    Id o_a = p.addObject(a);

    Object b;
    b.name = "b";
    b.kind = ObjectKind::Global;
    b.singleton = true;
    b.bytes = 8;
    b.initial = PointsToSet(NULL_OBJECT);
    Id o_b = p.addObject(b);

    Object f0;
    f0.name = "packed.f0";
    f0.kind = ObjectKind::Global;
    f0.singleton = false;
    f0.bytes = 0;
    f0.size_bytes = 12;
    f0.field_sensitive = true;
    f0.initial = PointsToSet::top();
    Id o_f0 = p.addObject(f0);

    Object f4;
    f4.name = "packed.f4";
    f4.kind = ObjectKind::Global;
    f4.singleton = true;
    f4.bytes = 8;
    f4.size_bytes = 8;
    f4.field_sensitive = true;
    f4.initial = PointsToSet(o_a);
    Id o_f4 = p.addFieldObject(o_f0, 4, f4);

    Id fn_main = p.addFunction("main");
    p.entry = fn_main;
    Id mb = p.addBlock(fn_main);
    Id v_f0 = p.addValue({"pf0", fn_main, false, {}});
    Id v_f4 = p.addValue({"pf4", fn_main, false, {}});
    Id v_b = p.addValue({"pb", fn_main, false, {}});
    Id v_ld = p.addValue({"ld", fn_main, false, {}});

    Instruction i0;
    i0.opcode = Opcode::Address;
    i0.result = v_f0;
    i0.object = o_f0;
    p.append(fn_main, mb, i0);

    Instruction i1;
    i1.opcode = Opcode::Address;
    i1.result = v_f4;
    i1.object = o_f4;
    p.append(fn_main, mb, i1);

    Instruction i2;
    i2.opcode = Opcode::Address;
    i2.result = v_b;
    i2.object = o_b;
    p.append(fn_main, mb, i2);

    Instruction i3;
    i3.opcode = Opcode::Store;
    i3.operands = {v_f0, v_b};
    i3.write_bytes = 8; // spans [0, 8) -> partially overwrites f4 at [4, 12)!
    p.append(fn_main, mb, i3);

    Instruction i4;
    i4.opcode = Opcode::Load;
    i4.result = v_ld;
    i4.operands = {v_f4};
    i4.read_bytes = 8;
    Id s_ld = p.append(fn_main, mb, i4);

    Instruction i5;
    i5.opcode = Opcode::Return;
    i5.operands = {v_ld};
    p.append(fn_main, mb, i5);
    p.validate();

    for (int mode = 0; mode < 4; ++mode) {
      Options opt = refined();
      opt.havoc_policy = HavocPolicy::Conservative;
      if (mode == 0)
        opt.solver_mode = SolverMode::GuardedSummary;
      else if (mode == 1)
        opt.solver_mode = SolverMode::InputStateTabulation;
      else if (mode == 2) {
        opt.solver_mode = SolverMode::GuardedSummary;
        opt.enable_one_level_flow = true;
      } else {
        opt.solver_mode = SolverMode::GuardedSummary;
        opt.eager_dovetail_summaries = true;
      }
      Analysis aa(p, opt);
      auto qr = aa.pointsTo(v_ld, s_ld, {}, Point::After);
      CHECK(qr.reachable());
      CHECK(qr.points_to.isTop());
    }
  }

  // Part C: Tiny ROBDD budget (`max_bdd_nodes = 4`) overflow with negated &
  // compound conditions must soundly widen to TRUE and never prune feasible
  // branches; and null-deref empty candidate summaries under `path_sensitive`
  // must stay consistent in `aliasesAllContexts`.
  {
    Program p;
    Object a;
    a.name = "a";
    a.kind = ObjectKind::Global;
    a.singleton = true;
    a.bytes = 8;
    a.initial = PointsToSet(NULL_OBJECT);
    Id o_a = p.addObject(a);

    Object b;
    b.name = "b";
    b.kind = ObjectKind::Global;
    b.singleton = true;
    b.bytes = 8;
    b.initial = PointsToSet(NULL_OBJECT);
    Id o_b = p.addObject(b);

    Id c0 = p.addCondition();
    Id c1 = p.addCondition();
    Id c2 = p.addCondition();
    Id c_and = p.addCondAnd(c0, c1);
    Id c_or = p.addCondOr(c_and, p.addCondNot(c2));

    Id fn_main = p.addFunction("main");
    p.entry = fn_main;
    Id b0 = p.addBlock(fn_main);
    Id b1 = p.addBlock(fn_main);
    Id b2 = p.addBlock(fn_main);
    Id b3 = p.addBlock(fn_main);

    Object slot;
    slot.name = "slot";
    slot.kind = ObjectKind::Global;
    slot.singleton = true;
    slot.bytes = 8;
    slot.initial = PointsToSet(o_a);
    Id o_slot = p.addObject(slot);

    Id v_a = p.addValue({"va", fn_main, false, {}});
    Id v_b = p.addValue({"vb", fn_main, false, {}});
    Id v_phi = p.addValue({"vphi", fn_main, false, {}});
    Id v_null = p.addConstant(PointsToSet(NULL_OBJECT), "null");
    Id v_pslot = p.addValue({"vpslot", fn_main, false, {}});
    Id v_p = p.addValue({"vp", fn_main, false, {}});
    Id v_bad = p.addValue({"vbad", fn_main, false, {}});

    Instruction ia;
    ia.opcode = Opcode::Address;
    ia.result = v_a;
    ia.object = o_a;
    p.append(fn_main, b0, ia);
    Instruction ib;
    ib.opcode = Opcode::Address;
    ib.result = v_b;
    ib.object = o_b;
    p.append(fn_main, b0, ib);
    Instruction islot;
    islot.opcode = Opcode::Address;
    islot.result = v_pslot;
    islot.object = o_slot;
    p.append(fn_main, b0, islot);
    // Assign v_p = &slot, then overwrite v_p = null so Steensgaard places
    // v_bad = *v_p in v_a's cluster while flow-sensitively *v_p has an empty
    // (non-path-pruned) backward summary.
    Instruction ip1;
    ip1.opcode = Opcode::Copy;
    ip1.result = v_p;
    ip1.operands = {v_pslot};
    p.append(fn_main, b0, ip1);
    Instruction ip2;
    ip2.opcode = Opcode::Copy;
    ip2.result = v_p;
    ip2.operands = {v_null};
    p.append(fn_main, b0, ip2);
    Instruction ilnull;
    ilnull.opcode = Opcode::Load;
    ilnull.result = v_bad;
    ilnull.operands = {v_p};
    ilnull.read_bytes = 8;
    p.append(fn_main, b0, ilnull);
    p.functions[fn_main].blocks[b0].successors = {{b1, {}, c_or, true},
                                                  {b2, {}, c_or, false}};
    Instruction n1;
    n1.opcode = Opcode::Nop;
    p.append(fn_main, b1, n1);
    p.functions[fn_main].blocks[b1].successors = {{b3, {{v_phi, v_a}}}};
    Instruction n2;
    n2.opcode = Opcode::Nop;
    p.append(fn_main, b2, n2);
    p.functions[fn_main].blocks[b2].successors = {{b3, {{v_phi, v_b}}}};
    Instruction ir;
    ir.opcode = Opcode::Return;
    ir.operands = {v_phi};
    Id s_ret = p.append(fn_main, b3, ir);
    p.validate();

    Options opt = refined();
    opt.path_sensitive = true;
    opt.max_bdd_nodes = 4; // Force immediate ROBDD budget exhaustion!
    Analysis aa(p, opt);
    auto qr = aa.pointsTo(v_phi, s_ret, {}, Point::Before);
    CHECK(qr.reachable());
    CHECK(qr.points_to.contains(o_a));
    CHECK(qr.points_to.contains(o_b));
    auto ar = aa.aliasesAllContexts(v_a, s_ret, Point::Before);
    CHECK(ar.reachable());
    CHECK(ar.may_alias_all ||
          std::find(ar.aliases.begin(), ar.aliases.end(), v_bad) !=
              ar.aliases.end());
  }
}

void auditFixesCyclicPass1NullCoverForwardAliasesAndInteriorGuards() {
  // Part A: Cyclic dovetailing Pass-1 must NOT poison a lower-depth callee's
  // `ExitReturn` summary when that callee performs an indirect call through a
  // cyclic higher-depth function pointer `fp`.
  {
    Program p;
    Object o_ans;
    o_ans.name = "ans";
    o_ans.kind = ObjectKind::Global;
    o_ans.singleton = true;
    o_ans.bytes = 8;
    o_ans.initial = PointsToSet(NULL_OBJECT);
    Id ans = p.addObject(o_ans);

    Object o_cyc;
    o_cyc.name = "cyc";
    o_cyc.kind = ObjectKind::Global;
    o_cyc.singleton = false;
    o_cyc.bytes = 8;
    o_cyc.initial = PointsToSet{};
    Id cyc = p.addObject(o_cyc);

    Object o_slot;
    o_slot.name = "slot";
    o_slot.kind = ObjectKind::Global;
    o_slot.singleton = true;
    o_slot.bytes = 8;
    o_slot.initial = PointsToSet(NULL_OBJECT);
    Id slot = p.addObject(o_slot);

    Id fn_main = p.addFunction("main");
    Id fn_bar = p.addFunction("bar");
    Id fn_leaf = p.addFunction("leaf");
    p.entry = fn_main;

    Object o_leaf_fn;
    o_leaf_fn.name = "leaf_fn";
    o_leaf_fn.kind = ObjectKind::Function;
    o_leaf_fn.function = fn_leaf;
    o_leaf_fn.initial = PointsToSet::top();
    Id f_leaf = p.addObject(o_leaf_fn);

    // leaf(): returns &ans (depth 0).
    Id lb = p.addBlock(fn_leaf);
    Id l_ans = p.addValue({"l.ans", fn_leaf, false, {}});
    Instruction il0;
    il0.opcode = Opcode::Address;
    il0.result = l_ans;
    il0.object = ans;
    p.append(fn_leaf, lb, il0);
    Instruction il1;
    il1.opcode = Opcode::Return;
    il1.operands = {l_ans};
    p.append(fn_leaf, lb, il1);

    // bar(fp): calls (*fp)() and returns its result. `ExitReturn` of `bar` has
    // depth 0 (`ans`), while `fp` lives in cyclic depth >= 1 (`cyc`).
    Id bb = p.addBlock(fn_bar);
    Id b_fp = p.addValue({"b.fp", fn_bar, false, {}});
    Id b_res = p.addValue({"b.res", fn_bar, false, {}});
    p.functions[fn_bar].parameters = {b_fp};
    Instruction ib0;
    ib0.opcode = Opcode::Call;
    ib0.indirect_target = b_fp;
    ib0.result = b_res;
    p.append(fn_bar, bb, ib0);
    Instruction ib1;
    ib1.opcode = Opcode::Return;
    ib1.operands = {b_res};
    p.append(fn_bar, bb, ib1);

    // main(): build cyclic component containing `cyc` and `&leaf` via
    // `*(&cyc) = &cyc; *(&cyc) = &leaf;` and push `bar`'s result through
    // another cyclic pointer so Pass 1 of cyclic depth triggers `bar` lazily.
    Id mb = p.addBlock(fn_main);
    Id v_pcyc = p.addValue({"pcyc", fn_main, false, {}});
    Id v_pleaf = p.addValue({"pleaf", fn_main, false, {}});
    Id v_pslot = p.addValue({"pslot", fn_main, false, {}});
    Id v_fp1 = p.addValue({"fp1", fn_main, false, {}});
    Id v_fp2 = p.addValue({"fp2", fn_main, false, {}});
    Id v_bar = p.addValue({"bar_r", fn_main, false, {}});
    Id v_out = p.addValue({"out", fn_main, false, {}});

    Instruction im0;
    im0.opcode = Opcode::Address;
    im0.result = v_pcyc;
    im0.object = cyc;
    p.append(fn_main, mb, im0);
    Instruction im1;
    im1.opcode = Opcode::Address;
    im1.result = v_pleaf;
    im1.object = f_leaf;
    p.append(fn_main, mb, im1);
    Instruction im2;
    im2.opcode = Opcode::Address;
    im2.result = v_pslot;
    im2.object = slot;
    p.append(fn_main, mb, im2);
    Instruction im3;
    im3.opcode = Opcode::Store;
    im3.operands = {v_pcyc, v_pcyc};
    p.append(fn_main, mb, im3);
    Instruction im4;
    im4.opcode = Opcode::Store;
    im4.operands = {v_pcyc, v_pleaf};
    p.append(fn_main, mb, im4);
    Instruction im5;
    im5.opcode = Opcode::Load;
    im5.result = v_fp1;
    im5.operands = {v_pcyc};
    p.append(fn_main, mb, im5);
    Instruction im6;
    im6.opcode = Opcode::Load;
    im6.result = v_fp2;
    im6.operands = {v_fp1};
    p.append(fn_main, mb, im6);
    Instruction im7;
    im7.opcode = Opcode::Call;
    im7.callee = fn_bar;
    im7.operands = {v_fp2};
    im7.result = v_bar;
    Id s_bar = p.append(fn_main, mb, im7);
    Instruction im8;
    im8.opcode = Opcode::Store;
    im8.operands = {v_pslot, v_bar};
    p.append(fn_main, mb, im8);
    Instruction im9;
    im9.opcode = Opcode::Load;
    im9.result = v_out;
    im9.operands = {v_pslot};
    Id s_out = p.append(fn_main, mb, im9);
    Instruction im10;
    im10.opcode = Opcode::Return;
    im10.operands = {v_out};
    p.append(fn_main, mb, im10);
    p.validate();

    for (int m = 0; m < 3; ++m) {
      Options opt = refined();
      if (m == 0)
        opt.solver_mode = SolverMode::GuardedSummary;
      else if (m == 1) {
        opt.solver_mode = SolverMode::GuardedSummary;
        opt.eager_dovetail_summaries = true;
      } else {
        opt.solver_mode = SolverMode::InputStateTabulation;
      }
      Analysis aa(p, opt);
      auto qb = aa.pointsTo(v_bar, s_bar, {}, Point::After);
      CHECK(qb.reachable());
      CHECK(qb.points_to.contains(ans));
      auto qo = aa.pointsTo(v_out, s_out, {}, Point::After);
      CHECK(qo.reachable());
      CHECK(qo.points_to.contains(ans));
      auto qf = aa.pointsToAllContexts(v_out, s_out, Point::After);
      CHECK(qf.reachable());
      CHECK(qf.points_to.contains(ans));
    }
  }

  // Part B: `NULL_OBJECT` must NOT bridge nullable pointers into one giant
  // inverse cluster during Andersen or One-Level-Flow cover construction.
  {
    Fixture f;
    Id cnull = f.addr(NULL_OBJECT);
    std::vector<Id> objs;
    std::vector<Id> ptrs;
    for (unsigned k = 0; k < 8; ++k) {
      Id o = f.object("o" + std::to_string(k));
      objs.push_back(o);
      Id pa = f.addr(o);
      Id p_nullable = f.value("p" + std::to_string(k));
      f.inst(Opcode::Join, p_nullable, {pa, cnull});
      ptrs.push_back(p_nullable);
    }
    // Single sink Join puts all 8 nullable pointers into ONE Steensgaard
    // partition (bidirectional unification), while directionally each `p_k`
    // only points to `{o_k, NULL_OBJECT}`.
    Id sink = f.value("sink");
    f.inst(Opcode::Join, sink, ptrs);
    Id sRet = f.inst(Opcode::Return);

    for (bool use_olf : {false, true}) {
      Options opt = refined();
      opt.andersen_threshold = 4;
      opt.adaptive_andersen_threshold = true;
      opt.enable_one_level_flow = use_olf;
      Analysis aa(f.p, opt);
      CHECK(aa.statistics().steensgaard_largest_partition >= 9);
      CHECK(aa.statistics().adaptive_refinement_rejections == 0);
      CHECK(aa.statistics().largest_cluster <= 3);
      for (unsigned k = 0; k < objs.size(); ++k) {
        auto qr = aa.pointsTo(ptrs[k], sRet);
        CHECK(qr.reachable());
        CHECK(qr.points_to == pts({objs[k], NULL_OBJECT}));
      }
      auto qs = aa.pointsTo(sink, sRet);
      CHECK(qs.reachable());
      for (Id o : objs)
        CHECK(qs.points_to.contains(o));
    }
  }

  // Part C: Algorithm 3 Step 2 forward alias propagation must remain sound when
  // a call omits a formal pointer argument (`actual == INVALID` -> `Top`),
  // passes an uninitialized fresh-slot load (`upper` empty -> widens to `Top`),
  // or passes an in-cluster null-deref load whose caller summary is empty.
  {
    Fixture f;
    Id a = f.object("a"), pa = f.addr(a);
    Id slot_a = f.object("slot_a", PointsToSet(a), true, ObjectKind::Global);
    Id pslot_a = f.addr(slot_a);
    Id cnull = f.addr(NULL_OBJECT);
    Id callee = f.p.addFunction("callee");
    f.p.addBlock(callee);
    Id p1 = f.value("p1", callee), p2 = f.value("p2", callee),
       p3 = f.value("p3", callee), p4 = f.value("p4", callee);
    f.p.functions[callee].parameters = {p1, p2, p3, p4};
    Id cRet = f.inst(Opcode::Return, INVALID, {}, callee, 0);

    // Uninitialized pointer `uninit` (defined by loading a fresh empty stack
    // slot), omitted 2nd argument (`INVALID`), and `v_bad` (in `p1`'s cluster
    // via Steensgaard but empty backward summary after overwriting `vp = null`).
    Id empty_slot =
        f.object("empty_slot", PointsToSet{}, true, ObjectKind::Stack);
    Id p_empty = f.addr(empty_slot);
    Id v_uninit = f.value("uninit");
    f.inst(Opcode::Load, v_uninit, {p_empty}, f.main, 0);
    Id vp = f.value("vp"), v_bad = f.value("v_bad");
    f.inst(Opcode::Copy, vp, {pslot_a}, f.main, 0);
    f.inst(Opcode::Copy, vp, {cnull}, f.main, 0);
    f.inst(Opcode::Load, v_bad, {vp}, f.main, 0);
    Id cs = f.call(callee, INVALID, {pa, INVALID, v_uninit, v_bad}, f.main, 0);
    f.inst(Opcode::Return, INVALID, {}, f.main, 0);

    for (int m = 0; m < 3; ++m) {
      Options opt = refined();
      if (m == 1) {
        opt.path_sensitive = true;
        opt.path_sensitive_interprocedural = true;
      } else if (m == 2) {
        opt.solver_mode = SolverMode::InputStateTabulation;
      }
      Analysis aa(f.p, opt);
      CHECK(aa.mayAlias(p1, p2, cRet, {cs}));
      CHECK(aa.mayAlias(p1, p3, cRet, {cs}));
      CHECK(aa.mayAlias(p1, p4, cRet, {cs}));
      auto alFscs = aa.aliases(p1, cRet, {cs});
      CHECK(alFscs.reachable());
      for (Id px : {p2, p3, p4}) {
        CHECK(alFscs.may_alias_all ||
              std::find(alFscs.aliases.begin(), alFscs.aliases.end(), px) !=
                  alFscs.aliases.end());
      }
      auto alFsci = aa.aliasesAllContexts(p1, cRet);
      CHECK(alFsci.reachable());
      for (Id px : {p2, p3, p4}) {
        CHECK(alFsci.may_alias_all ||
              std::find(alFsci.aliases.begin(), alFsci.aliases.end(), px) !=
                  alFsci.aliases.end());
      }
    }
  }

  // Part D: Unaligned interior pointer widened across two fields of the same
  // struct must NOT trigger a false `PointsTo(f0) && PointsTo(f8)` guard
  // contradiction in `atomsContradict`.
  {
    Fixture f;
    Id target = f.object("target");
    Object s0;
    s0.name = "s0";
    s0.kind = ObjectKind::Global;
    s0.singleton = false;
    s0.bytes = 8;
    s0.size_bytes = 16;
    s0.field_sensitive = true;
    s0.initial = PointsToSet(target);
    Id f0 = f.p.addObject(s0);
    Object s8 = s0;
    s8.name = "s8";
    s8.size_bytes = 8;
    s8.initial = PointsToSet(NULL_OBJECT);
    Id f8 = f.p.addFieldObject(f0, 8, s8);

    Id pf0 = f.addr(f0), pf8 = f.addr(f8);
    Id p_unaligned = f.value("p_unaligned");
    Instruction fi;
    fi.opcode = Opcode::Field;
    fi.result = p_unaligned;
    fi.operands = {pf0};
    fi.field_offset = 1; // Unaligned interior byte offset -> widens to {f0, f8}
    fi.field_known = true;
    f.p.append(f.main, 0, fi);
    Id v_ld = f.value("v_ld");
    f.inst(Opcode::Load, v_ld, {p_unaligned}, f.main, 0);
    f.inst(Opcode::Store, INVALID, {p_unaligned, v_ld}, f.main, 0);
    Id v_f8 = f.value("v_f8");
    Id s_f8 = f.inst(Opcode::Load, v_f8, {pf8}, f.main, 0);
    f.inst(Opcode::Return, INVALID, {}, f.main, 0);

    for (SolverMode mode :
         {SolverMode::GuardedSummary, SolverMode::InputStateTabulation}) {
      Options opt = refined();
      opt.solver_mode = mode;
      Analysis aa(f.p, opt);
      auto qr = aa.pointsTo(v_f8, s_f8, {}, Point::After);
      CHECK(qr.reachable());
      CHECK(qr.points_to.contains(target));
    }
  }
}

void objectAliasesAndMayAliasAllSymmetry() {
  // Part A: Memory-object aliases (`AliasResult::object_aliases`) covering
  // interprocedural writes, write-only indirect stores, out-of-slice
  // read-only globals, and flow-sensitive kills (matching Kahlon Fig. 5).
  {
    Fixture f;
    Id t1 = f.object("t1", PointsToSet{}, false, ObjectKind::Global);
    f.p.objects[t1].bytes = 0;
    f.p.objects[t1].size_bytes = 4;
    Id t2 = f.object("t2", PointsToSet{}, false, ObjectKind::Global);
    f.p.objects[t2].bytes = 0;
    f.p.objects[t2].size_bytes = 4;

    auto mkPtrGlobal = [&](const char *name, PointsToSet init) {
      Id o = f.object(name, std::move(init), true, ObjectKind::Global);
      f.p.objects[o].bytes = 8;
      f.p.objects[o].size_bytes = 8;
      return o;
    };
    Id g_a = mkPtrGlobal("g_a", PointsToSet(NULL_OBJECT));
    Id g_b = mkPtrGlobal("g_b", PointsToSet(t1));
    Id g_c = mkPtrGlobal("g_c", PointsToSet(NULL_OBJECT));
    Id g_init_only = mkPtrGlobal("g_init_only", PointsToSet(t1));
    Id g_other = mkPtrGlobal("g_other", PointsToSet(t2));
    Id g_killed = mkPtrGlobal("g_killed", PointsToSet(t1));

    Id pa = f.addr(g_a), pb = f.addr(g_b), pc = f.addr(g_c),
       pkilled = f.addr(g_killed), pt2 = f.addr(t2);

    Id bar = f.p.addFunction("bar");
    f.p.addBlock(bar);
    Id b_ld = f.value("b_ld", bar);
    f.inst(Opcode::Load, b_ld, {pb}, bar, 0);
    f.inst(Opcode::Store, INVALID, {pa, b_ld}, bar, 0);
    f.inst(Opcode::Return, INVALID, {}, bar, 0);

    Id z = f.value("z"), bv = f.value("bv"), ld_a = f.value("ld_a");
    f.inst(Opcode::Copy, z, {pc});
    f.inst(Opcode::Load, bv, {pb});
    f.inst(Opcode::Store, INVALID, {z, bv}); // write-only indirect store to g_c
    f.inst(Opcode::Store, INVALID, {pkilled, pt2}); // kills t1 in g_killed
    f.call(bar, INVALID, {});
    Id sLd = f.inst(Opcode::Load, ld_a, {pa});
    Id sRet = f.inst(Opcode::Return);
    (void)sLd;

    auto hasObj = [](const AliasResult &r, Id o) {
      return std::find(r.object_aliases.begin(), r.object_aliases.end(), o) !=
             r.object_aliases.end();
    };

    for (SolverMode mode :
         {SolverMode::GuardedSummary, SolverMode::InputStateTabulation}) {
      Options opt = refined();
      opt.solver_mode = mode;
      Analysis aa(f.p, opt);
      for (const AliasResult &ar : {aa.aliases(ld_a, sRet),
                                    aa.aliasesAllContexts(ld_a, sRet),
                                    aa.objectAliases(g_a, sRet),
                                    aa.objectAliasesAllContexts(g_a, sRet)}) {
        CHECK(ar.reachable());
        CHECK(!ar.may_alias_all);
        CHECK(hasObj(ar, g_a));
        CHECK(hasObj(ar, g_b));
        CHECK(hasObj(ar, g_c));
        CHECK(hasObj(ar, g_init_only));
        CHECK(!hasObj(ar, g_other));
        CHECK(!hasObj(ar, g_killed));
        CHECK(!hasObj(ar, t1));
        CHECK(!hasObj(ar, t2));
      }
    }
  }

  // Part B: `may_alias_all` symmetry — `aliases(argv)` must list all locally
  // reachable non-null-capable SSA values and non-null pointer memory objects
  // while excluding strictly-NULL pointers and values defined after the site.
  {
    Fixture f;
    Id argv = f.value("argv");
    f.p.functions[f.main].parameters = {argv};
    Id t = f.object("t", PointsToSet{}, false, ObjectKind::Global);
    f.p.objects[t].bytes = 0;
    f.p.objects[t].size_bytes = 4;
    Id g_ptr = f.object("g_ptr", PointsToSet(t), true, ObjectKind::Global);
    f.p.objects[g_ptr].bytes = 8;
    f.p.objects[g_ptr].size_bytes = 8;
    Id g_null =
        f.object("g_null", PointsToSet(NULL_OBJECT), true, ObjectKind::Global);
    f.p.objects[g_null].bytes = 8;
    f.p.objects[g_null].size_bytes = 8;

    Id pt = f.addr(t), cnull = f.addr(NULL_OBJECT);
    Id va = f.value("va"), vn = f.value("vn"), v_after = f.value("v_after");
    f.inst(Opcode::Copy, va, {pt});
    f.inst(Opcode::Copy, vn, {cnull});
    Id sQuery = f.inst(Opcode::Nop);
    f.inst(Opcode::Copy, v_after, {pt});
    f.inst(Opcode::Return);

    auto hasVal = [](const AliasResult &r, Id v) {
      return std::find(r.aliases.begin(), r.aliases.end(), v) !=
             r.aliases.end();
    };
    auto hasObj = [](const AliasResult &r, Id o) {
      return std::find(r.object_aliases.begin(), r.object_aliases.end(), o) !=
             r.object_aliases.end();
    };

    for (SolverMode mode :
         {SolverMode::GuardedSummary, SolverMode::InputStateTabulation}) {
      Options opt = refined();
      opt.solver_mode = mode;
      Analysis aa(f.p, opt);
      auto topAl = aa.aliases(argv, sQuery);
      CHECK(topAl.reachable());
      CHECK(topAl.may_alias_all);
      CHECK(hasVal(topAl, argv));
      CHECK(hasVal(topAl, va));
      CHECK(!hasVal(topAl, vn));
      CHECK(!hasVal(topAl, v_after));
      CHECK(hasObj(topAl, g_ptr));
      CHECK(!hasObj(topAl, g_null));
      CHECK(!hasObj(topAl, t));

      auto vaAl = aa.aliases(va, sQuery);
      CHECK(vaAl.reachable());
      CHECK(hasVal(vaAl, argv));
      CHECK(hasObj(vaAl, g_ptr));
      CHECK(!hasObj(vaAl, g_null));

      auto vnAl = aa.aliases(vn, sQuery);
      CHECK(vnAl.reachable());
      CHECK(!hasVal(vnAl, argv));
      CHECK(vnAl.object_aliases.empty());
    }
  }
}

void suffixContextsAndDerivedQueries() {
  // Part A: Suffix contexts (1-CFA, 2-CFA, empty suffix == AllContexts) across
  // both GuardedSummary and InputStateTabulation backends, plus cross-site
  // mayAlias, callTargets, callersOf, isRecursiveFunction, functionModRef,
  // callModRef, mayNull, and reversePointsTo.
  {
    Program p;
    auto makeGlobalObj = [&](const char *nm) {
      Object o;
      o.name = nm;
      o.kind = ObjectKind::Global;
      o.singleton = true;
      o.bytes = 8;
      o.initial = PointsToSet{};
      return p.addObject(std::move(o));
    };
    auto makeFuncObj = [&](const char *nm, Id fn) {
      Object o;
      o.name = nm;
      o.kind = ObjectKind::Function;
      o.function = fn;
      o.initial = PointsToSet::top();
      return p.addObject(std::move(o));
    };
    Id a = makeGlobalObj("a");
    Id b = makeGlobalObj("b");
    Id c = makeGlobalObj("c");
    Id slot = makeGlobalObj("slot");
    Id read_src = makeGlobalObj("read_src");
    p.objects[read_src].initial = PointsToSet(c);

    Id fn_main = p.addFunction("main");
    Id fn_mid = p.addFunction("mid");
    Id fn_leaf = p.addFunction("leaf");
    Id fn_w1 = p.addFunction("write_a");
    Id fn_w2 = p.addFunction("write_b");
    Id fn_rec = p.addFunction("rec");
    p.entry = fn_main;

    Id f_w1 = makeFuncObj("write_a", fn_w1);
    Id f_w2 = makeFuncObj("write_b", fn_w2);

    // leaf(p): reads *(&read_src) into `r`, stores `p` into *(&slot), returns p.
    Id lb = p.addBlock(fn_leaf);
    Id l_p = p.addValue({"l.p", fn_leaf, false, {}});
    Id l_raddr = p.addValue({"l.raddr", fn_leaf, false, {}});
    Id l_rval = p.addValue({"l.rval", fn_leaf, false, {}});
    Id l_saddr = p.addValue({"l.saddr", fn_leaf, false, {}});
    p.functions[fn_leaf].parameters = {l_p};
    {
      Instruction i0;
      i0.opcode = Opcode::Address;
      i0.result = l_raddr;
      i0.object = read_src;
      p.append(fn_leaf, lb, i0);
      Instruction i1;
      i1.opcode = Opcode::Load;
      i1.result = l_rval;
      i1.operands = {l_raddr};
      i1.read_bytes = 8;
      p.append(fn_leaf, lb, i1);
      Instruction i2;
      i2.opcode = Opcode::Address;
      i2.result = l_saddr;
      i2.object = slot;
      p.append(fn_leaf, lb, i2);
      Instruction i3;
      i3.opcode = Opcode::Store;
      i3.operands = {l_saddr, l_p};
      i3.write_bytes = 8;
      p.append(fn_leaf, lb, i3);
    }
    Instruction l_ret_i;
    l_ret_i.opcode = Opcode::Return;
    l_ret_i.operands = {l_p};
    Id s_leaf_ret = p.append(fn_leaf, lb, l_ret_i);

    // mid(x, fp): calls leaf(x) [c_l1], calls leaf(&c) [c_l2], calls (*fp)() [c_ind].
    Id mb = p.addBlock(fn_mid);
    Id m_x = p.addValue({"m.x", fn_mid, false, {}});
    Id m_fp = p.addValue({"m.fp", fn_mid, false, {}});
    Id m_c = p.addValue({"m.c", fn_mid, false, {}});
    Id m_r1 = p.addValue({"m.r1", fn_mid, false, {}});
    Id m_r2 = p.addValue({"m.r2", fn_mid, false, {}});
    p.functions[fn_mid].parameters = {m_x, m_fp};
    {
      Instruction i0;
      i0.opcode = Opcode::Address;
      i0.result = m_c;
      i0.object = c;
      p.append(fn_mid, mb, i0);
    }
    Instruction i_cl1;
    i_cl1.opcode = Opcode::Call;
    i_cl1.callee = fn_leaf;
    i_cl1.operands = {m_x};
    i_cl1.result = m_r1;
    Id c_l1 = p.append(fn_mid, mb, i_cl1);

    Instruction i_cl2;
    i_cl2.opcode = Opcode::Call;
    i_cl2.callee = fn_leaf;
    i_cl2.operands = {m_c};
    i_cl2.result = m_r2;
    Id c_l2 = p.append(fn_mid, mb, i_cl2);

    Instruction i_cind;
    i_cind.opcode = Opcode::Call;
    i_cind.indirect_target = m_fp;
    Id c_ind = p.append(fn_mid, mb, i_cind);
    {
      Instruction i_ret;
      i_ret.opcode = Opcode::Return;
      p.append(fn_mid, mb, i_ret);
    }

    // write_a(): stores &a into *(&a); write_b(): stores &b into *(&b).
    auto buildWriter = [&](Id fn, Id target_obj) {
      Id blk = p.addBlock(fn);
      Id va = p.addValue({"w.a", fn, false, {}});
      Instruction i0;
      i0.opcode = Opcode::Address;
      i0.result = va;
      i0.object = target_obj;
      p.append(fn, blk, i0);
      Instruction i1;
      i1.opcode = Opcode::Store;
      i1.operands = {va, va};
      i1.write_bytes = 8;
      p.append(fn, blk, i1);
      Instruction i2;
      i2.opcode = Opcode::Return;
      p.append(fn, blk, i2);
    };
    buildWriter(fn_w1, a);
    buildWriter(fn_w2, b);

    // rec(): branches to either base return or recursive call `rec()`.
    {
      Id rb = p.addBlock(fn_rec);
      Id rb_step = p.addBlock(fn_rec);
      Id rb_ret = p.addBlock(fn_rec);
      Instruction nop;
      nop.opcode = Opcode::Nop;
      p.append(fn_rec, rb, nop);
      p.functions[fn_rec].blocks[rb].successors.push_back({rb_step, {}});
      p.functions[fn_rec].blocks[rb].successors.push_back({rb_ret, {}});
      Instruction rc;
      rc.opcode = Opcode::Call;
      rc.callee = fn_rec;
      p.append(fn_rec, rb_step, rc);
      p.functions[fn_rec].blocks[rb_step].successors.push_back({rb_ret, {}});
      Instruction rr;
      rr.opcode = Opcode::Return;
      p.append(fn_rec, rb_ret, rr);
    }

    // main(): calls mid(&a, &write_a) [c_m1], mid(&b, &write_b) [c_m2], rec(),
    // plus a nullable join `v_null = {a, NULL}`.
    Id eb = p.addBlock(fn_main);
    Id v_a = p.addValue({"va", fn_main, false, {}});
    Id v_b = p.addValue({"vb", fn_main, false, {}});
    Id v_w1 = p.addValue({"vw1", fn_main, false, {}});
    Id v_w2 = p.addValue({"vw2", fn_main, false, {}});
    Id v_cnull = p.addConstant(PointsToSet(NULL_OBJECT), "null");
    Id v_nullable = p.addValue({"vnull", fn_main, false, {}});
    {
      Instruction i0;
      i0.opcode = Opcode::Address;
      i0.result = v_a;
      i0.object = a;
      p.append(fn_main, eb, i0);
      Instruction i1;
      i1.opcode = Opcode::Address;
      i1.result = v_b;
      i1.object = b;
      p.append(fn_main, eb, i1);
      Instruction i2;
      i2.opcode = Opcode::Address;
      i2.result = v_w1;
      i2.object = f_w1;
      p.append(fn_main, eb, i2);
      Instruction i3;
      i3.opcode = Opcode::Address;
      i3.result = v_w2;
      i3.object = f_w2;
      p.append(fn_main, eb, i3);
      Instruction i4;
      i4.opcode = Opcode::Join;
      i4.result = v_nullable;
      i4.operands = {v_a, v_cnull};
      p.append(fn_main, eb, i4);
    }
    Instruction i_m1;
    i_m1.opcode = Opcode::Call;
    i_m1.callee = fn_mid;
    i_m1.operands = {v_a, v_w1};
    Id c_m1 = p.append(fn_main, eb, i_m1);

    Instruction i_m2;
    i_m2.opcode = Opcode::Call;
    i_m2.callee = fn_mid;
    i_m2.operands = {v_b, v_w2};
    Id c_m2 = p.append(fn_main, eb, i_m2);

    Instruction i_rec;
    i_rec.opcode = Opcode::Call;
    i_rec.callee = fn_rec;
    p.append(fn_main, eb, i_rec);

    Instruction i_mret;
    i_mret.opcode = Opcode::Return;
    Id s_main_ret = p.append(fn_main, eb, i_mret);
    p.validate();

    for (SolverMode mode :
         {SolverMode::GuardedSummary, SolverMode::InputStateTabulation}) {
      Options opt = refined();
      opt.solver_mode = mode;
      Analysis aa(p, opt);

      // 1-CFA suffix `{c_l1}` unions both `m1` and `m2` -> `{a, b}` (not `c`).
      auto q_s1 = aa.pointsTo(l_p, s_leaf_ret, {c_l1}, Point::Before,
                              ContextMatch::Suffix);
      CHECK(q_s1.reachable());
      CHECK(q_s1.points_to == pts({a, b}));

      // 1-CFA suffix `{c_l2}` -> strictly `{c}`.
      auto q_s2 = aa.pointsTo(l_p, s_leaf_ret, {c_l2}, Point::Before,
                              ContextMatch::Suffix);
      CHECK(q_s2.reachable());
      CHECK(q_s2.points_to == pts({c}));

      // Empty suffix == AllContexts -> `{a, b, c}`.
      auto q_empty = aa.pointsTo(l_p, s_leaf_ret, {}, Point::Before,
                                 ContextMatch::Suffix);
      CHECK(q_empty.reachable());
      CHECK(q_empty.points_to == pts({a, b, c}));

      // 2-CFA suffix `{c_m1, c_l1}` == `{a}`.
      auto q_s2lvl = aa.pointsTo(l_p, s_leaf_ret, {c_m1, c_l1}, Point::Before,
                                 ContextMatch::Suffix);
      CHECK(q_s2lvl.reachable());
      CHECK(q_s2lvl.points_to == pts({a}));

      // Invalid suffix `{c_m1}` directly for `leaf` -> Unreachable.
      auto q_bad = aa.pointsTo(l_p, s_leaf_ret, {c_m1}, Point::Before,
                               ContextMatch::Suffix);
      CHECK(!q_bad.reachable());

      // Object points-to and aliases under suffix context `{c_l2}`:
      // `slot` was just overwritten with `l_p == &c`, aliasing `l_rval == &c`.
      auto qo_s2 = aa.objectPointsTo(slot, s_leaf_ret, {c_l2}, Point::Before,
                                     ContextMatch::Suffix);
      CHECK(qo_s2.reachable());
      CHECK(qo_s2.points_to == pts({c}));
      auto al_s2 = aa.aliases(l_p, s_leaf_ret, {c_l2}, Point::Before,
                              ContextMatch::Suffix);
      CHECK(al_s2.reachable());
      CHECK(std::find(al_s2.aliases.begin(), al_s2.aliases.end(), l_rval) !=
            al_s2.aliases.end());
      CHECK(std::find(al_s2.object_aliases.begin(), al_s2.object_aliases.end(),
                      slot) != al_s2.object_aliases.end());

      // Cross-site mayAlias across different functions (`main` vs `leaf`) and
      // per-side contexts!
      CHECK(aa.mayAliasAcrossSites(v_a, s_main_ret, l_p, s_leaf_ret));
      CHECK(!aa.mayAliasAcrossSites(v_a, s_main_ret, {}, l_p, s_leaf_ret,
                                    {c_l2}, Point::Before, Point::Before,
                                    ContextMatch::Full, ContextMatch::Suffix));
      CHECK(!aa.mayAliasAcrossSites(l_p, s_leaf_ret, {c_m1, c_l1}, l_p,
                                    s_leaf_ret, {c_m2, c_l1}, Point::Before,
                                    Point::Before, ContextMatch::Suffix,
                                    ContextMatch::Suffix));

      // mayNull
      CHECK(aa.mayNull(v_nullable, s_main_ret));
      CHECK(!aa.mayNull(v_a, s_main_ret));
      CHECK(!aa.mayNull(l_p, s_leaf_ret, {c_l1}, Point::Before,
                        ContextMatch::Suffix));

      // Context-sensitive indirect callTargets & callersOf & isRecursiveFunction
      auto ct_all = aa.callTargets(c_ind);
      CHECK(ct_all.reachable() && !ct_all.may_call_unknown);
      CHECK(ct_all.functions.size() == 2);
      auto ct_m1 = aa.callTargets(c_ind, {c_m1}, ContextMatch::Suffix);
      CHECK(ct_m1.reachable() && !ct_m1.may_call_unknown);
      CHECK(ct_m1.functions == std::vector<Id>{fn_w1});
      auto ct_m2 = aa.callTargets(c_ind, {c_m2}, ContextMatch::Suffix);
      CHECK(ct_m2.functions == std::vector<Id>{fn_w2});
      CHECK(aa.callersOf(fn_leaf).size() == 2);
      CHECK(aa.isRecursiveFunction(fn_rec));
      CHECK(!aa.isRecursiveFunction(fn_leaf));

      // functionModRef & context-sensitive callModRef
      auto mr_leaf = aa.functionModRef(fn_leaf);
      CHECK(!mr_leaf.may_mod_unknown && !mr_leaf.may_ref_unknown);
      CHECK(mr_leaf.modified == pts({slot}));
      CHECK(mr_leaf.referenced == pts({read_src}));
      auto mr_cm1 = aa.callModRef(c_ind, {c_m1}, ContextMatch::Suffix);
      CHECK(mr_cm1.modified == pts({a}));
      auto mr_cm2 = aa.callModRef(c_ind, {c_m2}, ContextMatch::Suffix);
      CHECK(mr_cm2.modified == pts({b}));

      // reversePointsTo
      auto rev_a = aa.reversePointsTo(a);
      CHECK(std::find(rev_a.values.begin(), rev_a.values.end(), v_a) !=
            rev_a.values.end());
      CHECK(std::find(rev_a.values.begin(), rev_a.values.end(), v_b) ==
            rev_a.values.end());
    }
  }

  // Part B: Four-state byte-range `queryAlias` on flattened struct fields.
  {
    Program p;
    Object base;
    base.name = "s";
    base.kind = ObjectKind::Global;
    base.singleton = true;
    base.bytes = 8;
    base.size_bytes = 16;
    base.field_sensitive = true;
    base.initial = PointsToSet{};
    Id s0 = p.addObject(base);
    Object f1;
    f1.name = "s+8";
    f1.kind = ObjectKind::Global;
    f1.singleton = true;
    f1.bytes = 8;
    f1.size_bytes = 8;
    f1.initial = PointsToSet{};
    Id s8 = p.addFieldObject(s0, 8, f1);
    Object o_x;
    o_x.name = "x";
    o_x.kind = ObjectKind::Global;
    o_x.singleton = true;
    o_x.bytes = 8;
    o_x.initial = PointsToSet{};
    Id x = p.addObject(std::move(o_x));

    Id fn = p.addFunction("main");
    p.entry = fn;
    Id bb = p.addBlock(fn);
    Id p0 = p.addValue({"p0", fn, false, {}});
    Id p8 = p.addValue({"p8", fn, false, {}});
    Id px = p.addValue({"px", fn, false, {}});
    Id pmay = p.addValue({"pmay", fn, false, {}});
    {
      Instruction i0;
      i0.opcode = Opcode::Address;
      i0.result = p0;
      i0.object = s0;
      p.append(fn, bb, i0);
      Instruction i1;
      i1.opcode = Opcode::Field;
      i1.result = p8;
      i1.operands = {p0};
      i1.field_offset = 8;
      i1.field_known = true;
      p.append(fn, bb, i1);
      Instruction i2;
      i2.opcode = Opcode::Address;
      i2.result = px;
      i2.object = x;
      p.append(fn, bb, i2);
      Instruction i3;
      i3.opcode = Opcode::Join;
      i3.result = pmay;
      i3.operands = {p0, px};
      p.append(fn, bb, i3);
    }
    Instruction ret;
    ret.opcode = Opcode::Return;
    Id sRet = p.append(fn, bb, ret);
    p.validate();
    (void)s8;

    Analysis aa(p, refined());
    CHECK(aa.queryAlias(p0, 8, sRet, p0, 8, sRet) == AliasKind::MustAlias);
    CHECK(aa.queryAlias(p0, 8, sRet, p8, 8, sRet) == AliasKind::NoAlias);
    CHECK(aa.queryAlias(p0, 16, sRet, p8, 8, sRet) == AliasKind::PartialAlias);
    CHECK(aa.queryAlias(p0, 4, sRet, p0, 8, sRet) == AliasKind::PartialAlias);
    CHECK(aa.queryAlias(pmay, 8, sRet, p0, 8, sRet) == AliasKind::MayAlias);
    CHECK(aa.queryAlias(p8, 8, sRet, px, 8, sRet) == AliasKind::NoAlias);
  }
}

void zeroFillHavocAddressTakenAndNonDuplicatedTopCover() {
  // Part A: `zero_fill` Havoc strong-updates singleton fields to NULL_OBJECT,
  // weak-updates non-singleton heap fields (preserving prior target + NULL),
  // and widens partial overwrites to Top — matching across both solvers.
  {
    Fixture f;
    Id a = f.object("a");
    Id pa = f.addr(a);

    Object sg;
    sg.name = "sg";
    sg.kind = ObjectKind::Global;
    sg.singleton = true;
    sg.bytes = 8;
    sg.size_bytes = 8;
    sg.initial = PointsToSet(a);
    Id o_sg = f.p.addObject(sg);

    Object hg;
    hg.name = "hg";
    hg.kind = ObjectKind::Heap;
    hg.singleton = false;
    hg.bytes = 8;
    hg.size_bytes = 8;
    hg.initial = PointsToSet(a);
    Id o_hg = f.p.addObject(hg);

    Id psg = f.addr(o_sg), phg = f.addr(o_hg);
    f.inst(Opcode::Store, INVALID, {phg, pa});

    Instruction z_sg;
    z_sg.opcode = Opcode::Havoc;
    z_sg.operands = {psg};
    z_sg.write_bytes = 8;
    z_sg.zero_fill = true;
    f.p.append(f.main, 0, z_sg);
    Id ld_sg = f.value("ld_sg");
    Id s_ld_sg = f.inst(Opcode::Load, ld_sg, {psg});

    Instruction z_hg;
    z_hg.opcode = Opcode::Havoc;
    z_hg.operands = {phg};
    z_hg.write_bytes = 8;
    z_hg.zero_fill = true;
    f.p.append(f.main, 0, z_hg);
    Id ld_hg = f.value("ld_hg");
    Id s_ld_hg = f.inst(Opcode::Load, ld_hg, {phg});

    Instruction z_part;
    z_part.opcode = Opcode::Havoc;
    z_part.operands = {psg};
    z_part.write_bytes = 4;
    z_part.zero_fill = true;
    f.p.append(f.main, 0, z_part);
    Id ld_part = f.value("ld_part");
    Id s_ld_part = f.inst(Opcode::Load, ld_part, {psg});
    f.inst(Opcode::Return);

    for (HavocPolicy policy : {HavocPolicy::Noop, HavocPolicy::Conservative}) {
      for (SolverMode mode :
           {SolverMode::GuardedSummary, SolverMode::InputStateTabulation}) {
        Options opt = refined();
        opt.solver_mode = mode;
        opt.havoc_policy = policy;
        Analysis aa(f.p, opt);
        CHECK(aa.pointsTo(ld_sg, s_ld_sg, {}, Point::After).points_to ==
              PointsToSet(NULL_OBJECT));
        CHECK(aa.pointsTo(ld_hg, s_ld_hg, {}, Point::After).points_to ==
              pts({a, NULL_OBJECT}));
        if (policy == HavocPolicy::Noop) {
          CHECK(aa.pointsTo(ld_part, s_ld_part, {}, Point::After).points_to ==
                PointsToSet(NULL_OBJECT));
        } else {
          CHECK(
              aa.pointsTo(ld_part, s_ld_part, {}, Point::After).points_to.isTop());
        }
      }
    }
  }

  // Part B: `address_taken == false` excludes a function from Top indirect
  // calls, and `buildInverseCover` does not duplicate `top_vals` into every
  // concrete object cluster while preserving full `aliases()` symmetry.
  {
    Fixture f;
    Id unk = f.value("unk");
    f.p.functions[f.main].parameters = {unk};
    f.p.functions[f.main].address_taken = false;

    Id taken = f.p.addFunction("taken");
    f.p.functions[taken].address_taken = true;
    f.p.addBlock(taken);
    f.inst(Opcode::Return, INVALID, {}, taken, 0);

    Id not_taken = f.p.addFunction("not_taken");
    f.p.functions[not_taken].address_taken = false;
    f.p.addBlock(not_taken);
    f.inst(Opcode::Return, INVALID, {}, not_taken, 0);

    Id x = f.object("x"), y = f.object("y");
    Id px = f.addr(x), py = f.addr(y);
    Id a1 = f.value("a1"), a2 = f.value("a2"), b1 = f.value("b1");
    f.inst(Opcode::Copy, a1, {px});
    f.inst(Opcode::Copy, a2, {px});
    f.inst(Opcode::Copy, b1, {py});
    // Force `a1, b1, unk` through a directional join so Steensgaard unifies
    // them into one partition that Andersen splits into `{x}`, `{y}`, `{TOP}`.
    Id j = f.value("j");
    f.inst(Opcode::Join, j, {a1, b1, unk});
    Instruction ic;
    ic.opcode = Opcode::Call;
    ic.indirect_target = unk;
    ic.writes_memory = false;
    Id sCall = f.p.append(f.main, 0, ic);
    Id sRet = f.inst(Opcode::Return);

    Options opt = refined();
    opt.andersen_threshold = 1;
    opt.adaptive_andersen_threshold = false;
    Analysis aa(f.p, opt);

    // Top values (`j`, `unk`) are kept in their own cluster, NOT duplicated
    // into both `{x}` and `{y}` concrete clusters!
    CHECK(aa.clustersFor(j).size() == 1);
    CHECK(aa.clustersFor(unk).size() == 1);

    // `aliases()` symmetry holds in both directions between concrete and Top.
    auto al_a1 = aa.aliases(a1, sRet);
    CHECK(al_a1.reachable());
    CHECK(std::find(al_a1.aliases.begin(), al_a1.aliases.end(), a2) !=
          al_a1.aliases.end());
    CHECK(std::find(al_a1.aliases.begin(), al_a1.aliases.end(), j) !=
          al_a1.aliases.end());
    auto al_j = aa.aliases(j, sRet);
    CHECK(al_j.reachable() && al_j.may_alias_all);
    CHECK(std::find(al_j.aliases.begin(), al_j.aliases.end(), a1) !=
          al_j.aliases.end());
    CHECK(std::find(al_j.aliases.begin(), al_j.aliases.end(), b1) !=
          al_j.aliases.end());

    // `callTargets` on Top indirect call includes `taken` and excludes
    // `not_taken` and `main`.
    auto ct = aa.callTargets(sCall);
    CHECK(ct.may_call_unknown);
    CHECK(std::find(ct.functions.begin(), ct.functions.end(), taken) !=
          ct.functions.end());
    CHECK(std::find(ct.functions.begin(), ct.functions.end(), not_taken) ==
          ct.functions.end());
    CHECK(std::find(ct.functions.begin(), ct.functions.end(), f.main) ==
          ct.functions.end());
  }
}
} // namespace

TEST(BootstrapEngineTest, StrongUpdates) { strongUpdates(); }
TEST(BootstrapEngineTest, SparsePointsToSets) { sparsePointsToSets(); }
TEST(BootstrapEngineTest, HierarchyMetadata) { hierarchyMetadata(); }
TEST(BootstrapEngineTest, BranchesAndKills) { branchesAndKills(); }
TEST(BootstrapEngineTest, ContextsAndReturns) { contextsAndReturns(); }
TEST(BootstrapEngineTest, ByReferenceAndMemoryContext) {
  byReferenceAndMemoryContext();
}
TEST(BootstrapEngineTest, ConditionalCalleeWrite) { conditionalCalleeWrite(); }
TEST(BootstrapEngineTest, Recursion) { recursion(); }
TEST(BootstrapEngineTest, MutualRecursion) { mutualRecursion(); }
TEST(BootstrapEngineTest, NonreturningRecursion) { noReturnIsNotIdentity(); }
TEST(BootstrapEngineTest, IndirectCalls) { indirectCalls(); }
TEST(BootstrapEngineTest, ExternalAndUnknownStores) {
  externalAndUnknownStores();
}
TEST(BootstrapEngineTest, ReadonlyExternal) { readonlyExternal(); }
TEST(BootstrapEngineTest, SummaryHeapObjectsUseWeakUpdates) {
  weakSummaryObjects();
}
TEST(BootstrapEngineTest, MultiTargetStoreUsesWeakUpdate) {
  multiTargetWeakUpdate();
}
TEST(BootstrapEngineTest, PartialWriteIsWeak) { partialWriteIsWeak(); }
TEST(BootstrapEngineTest, ParallelLoopPhis) { phiParallelLoop(); }
TEST(BootstrapEngineTest, CyclicMemory) { cyclicMemory(); }
TEST(BootstrapEngineTest, OverlappingCover) { overlapAndCoverage(); }
TEST(BootstrapEngineTest, AdaptiveAndersenThreshold) {
  adaptiveAndersenThreshold();
}
TEST(BootstrapEngineTest, ParallelPrecompute) { parallelPrecompute(); }
TEST(BootstrapEngineTest, LimitsAndValidation) { limitsAndValidation(); }
TEST(BootstrapEngineTest, SizedLoadsAndStores) { sizedLoadsAndStores(); }
TEST(BootstrapEngineTest, NullableAllocationAndHavocResult) {
  nullableAllocationAndHavocResult();
}
TEST(BootstrapEngineTest, ImplicitByvalAllocation) {
  implicitByvalAllocation();
}
TEST(BootstrapEngineTest, SecondLevelDependencies) {
  secondLevelDependencies();
}
TEST(BootstrapEngineTest, DifferentialSlicing) { differentialSlices(); }
TEST(BootstrapEngineTest, PaperFigure2SteensgaardVsAndersen) {
  paperFigure2SteensgaardVsAndersen();
}
TEST(BootstrapEngineTest, PaperFigure3RelevantSlicing) {
  paperFigure3RelevantSlicing();
}
TEST(BootstrapEngineTest, PaperFigure4MaximallyCompleteUpdateSequence) {
  paperFigure4MaximallyCompleteUpdateSequence();
}
TEST(BootstrapEngineTest, PaperFigure5GuardedSummariesAndSkipping) {
  paperFigure5GuardedSummariesAndSkipping();
}
TEST(BootstrapEngineTest, ContextSensitiveStrongUpdateViaGuards) {
  contextSensitiveStrongUpdateViaGuards();
}
TEST(BootstrapEngineTest, Algorithm4DerefAliasGuards) {
  algorithm4DerefAliasGuards();
}
TEST(BootstrapEngineTest, Algorithm2MultiDepthDovetailing) {
  algorithm2MultiDepthDovetailing();
}
TEST(BootstrapEngineTest, Algorithm3DirectAliasSets) {
  algorithm3DirectAliasSets();
}
TEST(BootstrapEngineTest, PathSensitiveRobddPruning) {
  pathSensitiveRobddPruning();
}
TEST(BootstrapEngineTest, PathSensitiveLoopConditionSoundness) {
  pathSensitiveLoopConditionSoundness();
}
TEST(BootstrapEngineTest, OneLevelFlowCascade) { oneLevelFlowCascade(); }
TEST(BootstrapEngineTest, SolverBackendDifferentialFuzz) {
  solverBackendDifferentialFuzz();
}
TEST(BootstrapEngineTest, PathSensitiveInterproceduralRobdd) {
  pathSensitiveInterproceduralRobdd();
}
TEST(BootstrapEngineTest, CyclicLinkedListLoopDifferential) {
  cyclicLinkedListLoopDifferential();
}
TEST(BootstrapEngineTest, Algorithm3ForwardQInterproceduralAliases) {
  algorithm3ForwardQInterproceduralAliases();
}
TEST(BootstrapEngineTest, PathSensitiveUpwardCallerSplicing) {
  pathSensitiveUpwardCallerSplicing();
}
TEST(BootstrapEngineTest, FlowSensitiveIndirectCallReachability) {
  flowSensitiveIndirectCallReachability();
}
TEST(BootstrapEngineTest, EagerAlgorithm2DovetailingConsistency) {
  eagerAlgorithm2DovetailingConsistency();
}
TEST(BootstrapEngineTest, CalleeInCallerLoopGuardContradictionSoundness) {
  calleeInCallerLoopGuardContradictionSoundness();
}
TEST(BootstrapEngineTest, EmptyBlockGuardAndPathPreservation) {
  emptyBlockGuardAndPathPreservation();
}
TEST(BootstrapEngineTest, DeadBranchPruneDoesNotFakeUnreachable) {
  deadBranchPruneDoesNotFakeUnreachable();
}
TEST(BootstrapEngineTest, InterproceduralUpwardPathSensitiveKillAndAliases) {
  interproceduralUpwardPathSensitiveKillAndAliases();
}
TEST(BootstrapEngineTest, SteensgaardNoNullBridge) {
  steensgaardNoNullBridge();
}
TEST(BootstrapEngineTest, OneLevelFlowMemoryPrecision) {
  oneLevelFlowMemoryPrecision();
}
TEST(BootstrapEngineTest, FieldAwareDovetailCyclicStruct) {
  fieldAwareDovetailCyclicStruct();
}
TEST(BootstrapEngineTest, CompoundRobddSelectEdgePtrGuardsAndCallCondSubst) {
  compoundRobddSelectEdgePtrGuardsAndCallCondSubst();
}
TEST(BootstrapEngineTest, PerPartitionAndersenRejectionAndHybridSolver) {
  perPartitionAndersenRejectionAndHybridSolver();
}
TEST(BootstrapEngineTest, SelectivePrecomputationObjectQueriesAndMustAlias) {
  selectivePrecomputationObjectQueriesAndMustAlias();
}
TEST(BootstrapEngineTest,
     SteensgaardTopStorePreservesRhsAndNullAliasExcludesTop) {
  steensgaardTopStorePreservesRhsAndNullAliasExcludesTop();
}
TEST(BootstrapEngineTest,
     EdgePtrGuardPostDefAndInterprocAnchorAliasSoundness) {
  edgePtrGuardPostDefAndInterprocAnchorAliasSoundness();
}
TEST(BootstrapEngineTest,
     ArrayFoldingWraparoundInteriorStrongUpdateMutualRecAndLoadFuncSoundness) {
  arrayFoldingWraparoundInteriorStrongUpdateMutualRecAndLoadFuncSoundness();
}
TEST(BootstrapEngineTest,
     Round5And6UnslicedIndirectCallPackedStoreBddBudgetAndNullAliasSoundness) {
  round5And6UnslicedIndirectCallPackedStoreBddBudgetAndNullAliasSoundness();
}
TEST(BootstrapEngineTest,
     AuditFixesCyclicPass1NullCoverForwardAliasesAndInteriorGuards) {
  auditFixesCyclicPass1NullCoverForwardAliasesAndInteriorGuards();
}
TEST(BootstrapEngineTest, ObjectAliasesAndMayAliasAllSymmetry) {
  objectAliasesAndMayAliasAllSymmetry();
}
TEST(BootstrapEngineTest, SuffixContextsAndDerivedQueries) {
  suffixContextsAndDerivedQueries();
}
TEST(BootstrapEngineTest, ZeroFillHavocAddressTakenAndNonDuplicatedTopCover) {
  zeroFillHavocAddressTakenAndNonDuplicatedTopCover();
}
