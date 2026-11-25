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
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, l1, {}, Point::After).points_to.isTop());
  CHECK(aa.pointsTo(y, l2, {}, Point::After).points_to == pts({b}));
  CHECK(aa.pointsTo(z, l3, {}, Point::After).points_to.isTop());
  CHECK(aa.mayAlias(unknown, pb, l3));
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
  Analysis aa(f.p, refined());
  // Mixed pointer bits are not restricted to either the old or the new object.
  CHECK(aa.pointsTo(x, load, {}, Point::After).points_to.isTop());
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
  Id a = f.object("a"), pa = f.addr(a), end = f.inst(Opcode::Return);
  Options zero = refined();
  zero.max_contexts = 0;
  Analysis aa(f.p, zero);
  CHECK(aa.pointsTo(pa, end).status == QueryStatus::ResourceLimit);
  CHECK(aa.pointsTo(pa, end).points_to.isTop());
  zero.max_contexts = 10;
  zero.max_steps = 0;
  Analysis steps(f.p, zero);
  CHECK(steps.pointsTo(pa, end).status == QueryStatus::ResourceLimit);
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
