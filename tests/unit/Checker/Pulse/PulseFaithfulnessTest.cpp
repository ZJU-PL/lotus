/**
 * @file PulseFaithfulnessTest.cpp
 * @brief End-to-end and unit tests verifying Infer-faithful Pulse algorithms:
 * conditional bi-abduction, interprocedural MustBeValid/MustBeInitialized/
 * MustNotBeTainted checks, incorporateNewEqs, on-demand alias specialization,
 * latent-issue deferral and discharge, memory/resource leak detection,
 * PulseFormula solver/join fixes, AbductiveDomain::leq graph isomorphism,
 * PulseJoin metadata preservation, non-disjunctive copy/const-ref tracking,
 * demangled C++ STL models + InvokeInst, and TOPL heap reachability + Z3 guards.
 */

#include "Checker/Pulse/Core/PulseSubstitution.h"
#include "Checker/Pulse/Domain/PulseJoin.h"
#include "Checker/Pulse/Domain/PulseTaintConfig.h"
#include "Checker/Pulse/Interproc/PulseTransitiveInfo.h"
#include "Checker/Pulse/Report/PulseOptions.h"
#include "Checker/Pulse/Report/PulseReport.h"
#include "Checker/Pulse/Topl/PulseTopl.h"
#include "PulseCheckerFixture.h"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>

using namespace llvm;
using namespace pulse;
using lotus::unittest::findCallTo;
using lotus::unittest::parseModule;

TEST_F(PulseCheckerTest, ConditionalBiAbductionOnlyAbducesPreAddresses) {
  auto module = parseModule(context, R"(
    define i32 @load_local_and_formal(i32* %formal) {
    entry:
      %local = alloca i32
      store i32 42, i32* %local
      %a = load i32, i32* %local
      %b = load i32, i32* %formal
      %sum = add i32 %a, %b
      ret i32 %sum
    }
  )");
  ASSERT_NE(module, nullptr);
  Function *F = module->getFunction("load_local_and_formal");
  ASSERT_NE(F, nullptr);

  PulseChecker checker(module.get());
  ExecutionDomain state = checker.initializeFunction(F);
  auto *init_astate = state.getAstate();
  ASSERT_NE(init_astate, nullptr);

  AbstractValue formal_av = checker.getFactory().getOrCreate(F->getArg(0));
  EXPECT_TRUE(init_astate->isInPre(formal_av));
  EXPECT_NE(init_astate->getPreStack().find(F->getArg(0)), nullptr);

  auto *alloca_inst = findNthInstruction<AllocaInst>(F, 0);
  auto *second_load = findNthInstruction<LoadInst>(F, 1);
  ASSERT_NE(alloca_inst, nullptr);
  ASSERT_NE(second_load, nullptr);

  state = executeEntryBlock(checker, F, second_load);
  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);

  AbstractValue local_av = checker.getFactory().getOrCreate(alloca_inst);
  EXPECT_FALSE(astate->isInPre(local_av));
  EXPECT_EQ(astate->getPreMustBeValid().count(astate->getCanonical(local_av)),
            0u);
  EXPECT_GT(astate->getPreMustBeValid().count(astate->getCanonical(formal_av)),
            0u);
  EXPECT_GT(
      astate->getPreMustBeInitialized().count(astate->getCanonical(formal_av)),
      0u);
  EXPECT_NE(astate->getPreHeap().findEdge(astate->getCanonical(formal_av),
                                          Access(AccessKind::Dereference)),
            nullptr);
}

TEST_F(PulseCheckerTest, InterproceduralNestedPointerMaterializationEndToEnd) {
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @free_indirect(i8** %pp) {
    entry:
      %p = load i8*, i8** %pp
      call void @free(i8* %p)
      ret void
    }

    define i8 @caller() {
    entry:
      %slot = alloca i8*
      %buf = call i8* @malloc(i64 8)
      store i8* %buf, i8** %slot
      call void @free_indirect(i8** %slot)
      %v = load i8, i8* %buf
      ret i8 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before = getReportCountForType(mgr, IssueType::UseAfterFree);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }
  EXPECT_EQ(getReportCountForType(mgr, IssueType::UseAfterFree), before + 1);
}

TEST_F(PulseCheckerTest, InterproceduralNullDereferenceViaMustBeValid) {
  auto module = parseModule(context, R"(
    define i32 @deref_arg(i32* %p) {
    entry:
      %v = load i32, i32* %p
      ret i32 %v
    }

    define i32 @caller_null() {
    entry:
      %r = call i32 @deref_arg(i32* null)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before = getReportCountForType(mgr, IssueType::NullDereference);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }
  EXPECT_EQ(getReportCountForType(mgr, IssueType::NullDereference), before + 1);
}

TEST_F(PulseCheckerTest, InterproceduralUseAfterFreeViaMustBeValid) {
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define i8 @read_byte(i8* %p) {
    entry:
      %v = load i8, i8* %p
      ret i8 %v
    }

    define i8 @caller_uaf() {
    entry:
      %p = call i8* @malloc(i64 4)
      call void @free(i8* %p)
      %v = call i8 @read_byte(i8* %p)
      ret i8 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before = getReportCountForType(mgr, IssueType::UseAfterFree);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }
  EXPECT_EQ(getReportCountForType(mgr, IssueType::UseAfterFree), before + 1);
}

TEST_F(PulseCheckerTest, InterproceduralUninitializedReadViaMustBeInitialized) {
  auto module = parseModule(context, R"(
    define i32 @read_int(i32* %p) {
    entry:
      %v = load i32, i32* %p
      ret i32 %v
    }

    define i32 @init_then_read(i32* %p) {
    entry:
      store i32 9, i32* %p
      %v = load i32, i32* %p
      ret i32 %v
    }

    define i32 @bad_caller() {
    entry:
      %x = alloca i32
      %v = call i32 @read_int(i32* %x)
      ret i32 %v
    }

    define i32 @safe_caller() {
    entry:
      %y = alloca i32
      %v = call i32 @init_then_read(i32* %y)
      ret i32 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before =
      getReportCountForType(mgr, IssueType::UninitializedRead);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }
  EXPECT_EQ(getReportCountForType(mgr, IssueType::UninitializedRead),
            before + 1);
}

TEST_F(PulseCheckerTest,
       IncorporateNewEqsDetectsLatentInvalidAccessAndDistinctAllocUnsat) {
  // Part 1: Direct unit test of distinct allocation UNSAT in incorporateNewEqs.
  {
    AbstractValueFactory factory;
    PulseOperations ops(&factory);
    AbductiveDomain d;
    AbstractValue a = factory.createFresh();
    AbstractValue b = factory.createFresh();
    ops.allocate(d, a, nullptr, AllocatorKind::CMalloc);
    ops.allocate(d, b, nullptr, AllocatorKind::CMalloc);
    ASSERT_TRUE(d.getPathFormula().addEquality(a, b));
    auto res = d.incorporatePendingNewEqs();
    EXPECT_TRUE(res.isUnsat());
  }

  // Part 2: Deref-then-null-check creates LatentInvalidAccess (0 reports alone)
  // and fires NullDereference when called with NULL.
  auto module = parseModule(context, R"(
    define void @deref_then_check(i32* %p) {
    entry:
      store i32 1, i32* %p
      %isnull = icmp eq i32* %p, null
      br i1 %isnull, label %then, label %else
    then:
      ret void
    else:
      ret void
    }

    define void @caller_triggers_null() {
    entry:
      call void @deref_then_check(i32* null)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before = getReportCountForType(mgr, IssueType::NullDereference);
  {
    PulseChecker checker(module.get());
    checker.analyzeFunction(module->getFunction("deref_then_check"));
    DiagnosticManager::getInstance().flush();
    EXPECT_EQ(getReportCountForType(mgr, IssueType::NullDereference), before);

    checker.analyzeFunction(module->getFunction("caller_triggers_null"));
  }
  EXPECT_EQ(getReportCountForType(mgr, IssueType::NullDereference), before + 1);
}

TEST_F(PulseCheckerTest, OnDemandAliasSpecializationDetectsAliasedUAF) {
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define i8 @free_first_read_second(i8* %a, i8* %b) {
    entry:
      call void @free(i8* %a)
      %v = load i8, i8* %b
      ret i8 %v
    }

    define i8 @caller_aliased() {
    entry:
      %p = call i8* @malloc(i64 8)
      %r = call i8 @free_first_read_second(i8* %p, i8* %p)
      ret i8 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before = getReportCountForType(mgr, IssueType::UseAfterFree);
  {
    PulseChecker checker(module.get());
    checker.analyzeFunction(module->getFunction("free_first_read_second"));
    DiagnosticManager::getInstance().flush();
    // Standalone analysis has distinct formals a and b: no UAF yet.
    EXPECT_EQ(getReportCountForType(mgr, IssueType::UseAfterFree), before);

    checker.analyzeFunction(module->getFunction("caller_aliased"));
  }
  // Specialized re-analysis under arg0 == arg1 uncovers the aliased UAF!
  EXPECT_EQ(getReportCountForType(mgr, IssueType::UseAfterFree), before + 1);
}

TEST_F(PulseCheckerTest, LatentNullDerefDeferredInCalleeAndDischargedInCaller) {
  auto module = parseModule(context, R"(
    define i8 @latent_npd(i8* %p) {
    entry:
      %z = icmp eq i8* %p, null
      br i1 %z, label %bad, label %ok
    bad:
      %v = load i8, i8* %p
      ret i8 %v
    ok:
      ret i8 0
    }

    define i8 @safe_caller() {
    entry:
      %x = alloca i8
      store i8 1, i8* %x
      %r = call i8 @latent_npd(i8* %x)
      ret i8 %r
    }

    define i8 @bad_caller() {
    entry:
      %r = call i8 @latent_npd(i8* null)
      ret i8 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before = getReportCountForType(mgr, IssueType::NullDereference);
  {
    PulseChecker checker(module.get());
    checker.analyzeFunction(module->getFunction("latent_npd"));
    checker.analyzeFunction(module->getFunction("safe_caller"));
    DiagnosticManager::getInstance().flush();
    EXPECT_EQ(getReportCountForType(mgr, IssueType::NullDereference), before);

    checker.analyzeFunction(module->getFunction("bad_caller"));
  }
  EXPECT_EQ(getReportCountForType(mgr, IssueType::NullDereference), before + 1);
}

TEST_F(PulseCheckerTest, InterproceduralTaintSinkAndSanitizer) {
  auto module = parseModule(context, R"(
    declare i8* @getenv(i8*)
    declare i32 @system(i8*)
    declare i32 @atoi(i8*)

    define void @run_cmd(i8* %cmd) {
    entry:
      %r = call i32 @system(i8* %cmd)
      ret void
    }

    define void @sanitized_caller(i8* %name) {
    entry:
      %v = call i8* @getenv(i8* %name)
      %n = call i32 @atoi(i8* %v)
      call void @run_cmd(i8* %v)
      ret void
    }

    define void @unsanitized_caller(i8* %name) {
    entry:
      %v = call i8* @getenv(i8* %name)
      call void @run_cmd(i8* %v)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before = getReportCountForType(mgr, IssueType::TaintError);
  {
    PulseChecker checker(module.get());
    checker.analyzeFunction(module->getFunction("run_cmd"));
    checker.analyzeFunction(module->getFunction("sanitized_caller"));
    EXPECT_EQ(getReportCountForType(mgr, IssueType::TaintError), before);

    checker.analyzeFunction(module->getFunction("unsanitized_caller"));
  }
  EXPECT_EQ(getReportCountForType(mgr, IssueType::TaintError), before + 1);
}

TEST_F(PulseCheckerTest, MemoryLeakAndResourceLeakDetection) {
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)
    declare i8* @fopen(i8*, i8*)
    declare i32 @fclose(i8*)

    define void @leak_mem() {
    entry:
      %p = call i8* @malloc(i64 16)
      ret void
    }

    define void @no_leak_freed() {
    entry:
      %p = call i8* @malloc(i64 16)
      call void @free(i8* %p)
      ret void
    }

    define i8* @no_leak_returned() {
    entry:
      %p = call i8* @malloc(i64 16)
      ret i8* %p
    }

    define i8* @no_leak_interior_ptr() {
    entry:
      %p = call i8* @malloc(i64 16)
      %q = getelementptr i8, i8* %p, i64 4
      ret i8* %q
    }

    define void @leak_file() {
    entry:
      %f = call i8* @fopen(i8* null, i8* null)
      ret void
    }

    define void @no_leak_closed_file() {
    entry:
      %f = call i8* @fopen(i8* null, i8* null)
      %r = call i32 @fclose(i8* %f)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t memBefore = getReportCountForType(mgr, IssueType::MemoryLeak);
  const size_t resBefore = getReportCountForType(mgr, IssueType::ResourceLeak);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }
  EXPECT_EQ(getReportCountForType(mgr, IssueType::MemoryLeak), memBefore + 1);
  EXPECT_EQ(getReportCountForType(mgr, IssueType::ResourceLeak), resBefore + 1);
}

TEST_F(PulseCheckerTest, FormulaSolverFixesAndConditionGuidedJoin) {
  AbstractValue v1(nullptr, 1001);
  AbstractValue v2(nullptr, 1002);
  AbstractValue v3(nullptr, 1003);
  AbstractValue v4(nullptr, 1004);

  // 1. addNonNull contradicts addNull instead of silently clearing it.
  {
    PulseFormula f;
    ASSERT_TRUE(f.addNull(v1));
    EXPECT_FALSE(f.addNonNull(v1));
  }

  // 2. addEquality merges bounds and rejects disjoint bounds.
  {
    PulseFormula f;
    ASSERT_TRUE(f.addBounds(v1, 2, 10));
    ASSERT_TRUE(f.addBounds(v2, 5, 20));
    ASSERT_TRUE(f.addEquality(v1, v2));
    EXPECT_EQ(f.getLowerBound(v1), 5);
    EXPECT_EQ(f.getUpperBound(v1), 10);

    ASSERT_TRUE(f.addBounds(v3, 25, 30));
    EXPECT_FALSE(f.addEquality(v1, v3));
  }

  // 3. merge & applySubstitution preserve arithmetic_ops_ (Z3 proves v3 == 7).
  {
    PulseFormula f1;
    ASSERT_TRUE(f1.addBounds(v1, 3, 3));
    ASSERT_TRUE(f1.addBounds(v2, 4, 4));
    PulseFormula f2;
    ASSERT_TRUE(f2.addArithmeticOperation(v3, v1, v2, "+"));
    PulseFormula merged = PulseFormula::merge(f1, f2);
    ASSERT_TRUE(merged.isConsistent());
    auto eq7 = merged.impliesComparison(v3, std::nullopt, "==", std::nullopt, 7);
    ASSERT_TRUE(eq7.has_value());
    EXPECT_TRUE(*eq7);

    Substitution subst;
    subst.add(v3, v4);
    PulseFormula mapped = merged.applySubstitution(subst);
    auto eq7_mapped =
        mapped.impliesComparison(v4, std::nullopt, "==", std::nullopt, 7);
    ASSERT_TRUE(eq7_mapped.has_value());
    EXPECT_TRUE(*eq7_mapped);
  }

  // 4. Condition-guided join preserves independent facts (v2 == 42) while
  //    dropping conflicting branch condition on v1.
  {
    PulseFormula f1;
    ASSERT_TRUE(f1.addNull(v1));
    f1.recordCondition(PathConditionAtom::eqZero(v1), 0);
    ASSERT_TRUE(f1.addBounds(v2, 42, 42));

    PulseFormula f2;
    ASSERT_TRUE(f2.addNonNull(v1));
    f2.recordCondition(PathConditionAtom::neZero(v1), 0);
    ASSERT_TRUE(f2.addBounds(v2, 42, 42));

    PulseFormula joined = PulseFormula::join(f1, f2);
    EXPECT_TRUE(joined.isConsistent());
    EXPECT_FALSE(joined.isNull(v1));
    EXPECT_FALSE(joined.isNonNull(v1));
    EXPECT_EQ(joined.asConstantInt(v2), 42);
  }
}

TEST_F(PulseCheckerTest, AbductiveDomainLeqIsomorphismAndPulseJoinMetadata) {
  auto module = parseModule(context, R"(
    define void @dummy(i8* %p) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);
  Argument *p = module->getFunction("dummy")->getArg(0);

  AbstractValueFactory factory;
  AbductiveDomain d1;
  AbstractValue p1 = factory.createFresh();
  AbstractValue c1 = factory.createFresh();
  d1.getPostStack().add(p, Address(p1));
  d1.getPreStack().add(p, Address(p1));
  d1.registerPreAddress(p1);
  d1.getPostHeap().addEdge(p1, Access(AccessKind::Dereference), Address(c1));
  d1.getPostAttrs().add(p1, pulse::Attribute::Allocated);
  d1.setAllocationSize(p1, 16);

  AbductiveDomain d2;
  AbstractValue p2 = factory.createFresh();
  AbstractValue c2 = factory.createFresh();
  d2.getPostStack().add(p, Address(p2));
  d2.getPreStack().add(p, Address(p2));
  d2.registerPreAddress(p2);
  d2.getPostHeap().addEdge(p2, Access(AccessKind::Dereference), Address(c2));
  d2.getPostAttrs().add(p2, pulse::Attribute::Allocated);
  d2.setAllocationSize(p2, 16);

  EXPECT_TRUE(AbductiveDomain::leq(d1, d2));
  EXPECT_TRUE(AbductiveDomain::leq(d2, d1));

  // Mutating an attribute breaks isomorphism.
  d2.getPostAttrs().add(c2, pulse::Attribute::Invalid);
  d2.setInvalidationInfo(c2, InvalidationKind::CFree, nullptr);
  EXPECT_FALSE(AbductiveDomain::leq(d1, d2));

  // PulseJoin preserves allocation size (tighter bound) and taint.
  d1.setAllocationSize(p1, 32);
  TaintOperations::taint(d1, p1, TaintKind::UserInput(), "getenv");
  auto joined = PulseJoin::joinAbductive(d1, d2);
  ASSERT_TRUE(joined.has_value());
  const Address *jp = joined->getPostStack().find(p);
  ASSERT_NE(jp, nullptr);
  EXPECT_EQ(joined->getAllocationSize(jp->addr), 16u);
  EXPECT_TRUE(joined->getTaintDomain().has(jp->addr));
}

TEST_F(PulseCheckerTest, NonDisjunctiveUnmodifiedAggregateCopyAndConstRefParam) {
  auto module = parseModule(context, R"(
    %Big = type { i64, i64, i64, i64 }

    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

    define i64 @unmodified_copy(%Big* %src) {
    entry:
      %copy = alloca %Big
      %dst8 = bitcast %Big* %copy to i8*
      %src8 = bitcast %Big* %src to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst8, i8* %src8, i64 32, i1 false)
      %f0 = getelementptr %Big, %Big* %copy, i32 0, i32 0
      %v = load i64, i64* %f0
      ret i64 %v
    }

    define i64 @modified_copy(%Big* %src) {
    entry:
      %copy = alloca %Big
      %dst8 = bitcast %Big* %copy to i8*
      %src8 = bitcast %Big* %src to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst8, i8* %src8, i64 32, i1 false)
      %f0 = getelementptr %Big, %Big* %copy, i32 0, i32 0
      store i64 7, i64* %f0
      %v = load i64, i64* %f0
      ret i64 %v
    }

    define i64 @const_refable(%Big* byval(%Big) %param) {
    entry:
      %f0 = getelementptr %Big, %Big* %param, i32 0, i32 0
      %v = load i64, i64* %f0
      ret i64 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t copyBefore =
      getReportCountForType(mgr, IssueType::UnnecessaryCopy);
  const size_t paramBefore =
      getReportCountForType(mgr, IssueType::ConstRefableParam);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }
  // unmodified_copy triggers 1 UnnecessaryCopy; modified_copy triggers 0.
  EXPECT_EQ(getReportCountForType(mgr, IssueType::UnnecessaryCopy),
            copyBefore + 1);
  // const_refable triggers 1 ConstRefableParam.
  EXPECT_EQ(getReportCountForType(mgr, IssueType::ConstRefableParam),
            paramBefore + 1);
}

TEST_F(PulseCheckerTest, CppDemangledModelsVectorOptionalAndInvokeInst) {
  auto module = parseModule(context, R"(
    %vec = type { i32* }
    %opt = type { i32, i8 }

    declare i32* @_ZNSt6vectorIiSaIiEE4dataEv(%vec*)
    declare void @_ZNSt6vectorIiSaIiEE9push_backERKi(%vec*, i32*)
    declare void @_ZNSt8optionalIiE5resetEv(%opt*)
    declare i32* @_ZNSt8optionalIiE5valueEv(%opt*)
    declare i8* @_Znwm(i64)
    declare void @_ZdlPvm(i8*, i64)
    declare i32 @__gxx_personality_v0(...)

    define i32 @vec_invalidation(%vec* %v, i32* %elem) {
    entry:
      %ptr = call i32* @_ZNSt6vectorIiSaIiEE4dataEv(%vec* %v)
      call void @_ZNSt6vectorIiSaIiEE9push_backERKi(%vec* %v, i32* %elem)
      %val = load i32, i32* %ptr
      ret i32 %val
    }

    define i32 @empty_optional_value(%opt* %o) {
    entry:
      call void @_ZNSt8optionalIiE5resetEv(%opt* %o)
      %p = call i32* @_ZNSt8optionalIiE5valueEv(%opt* %o)
      ret i32 0
    }

    define i8 @invoke_new_sized_delete_uaf() personality i8* bitcast (i32 (...)* @__gxx_personality_v0 to i8*) {
    entry:
      %p = invoke i8* @_Znwm(i64 8)
           to label %ok unwind label %lpad
    ok:
      call void @_ZdlPvm(i8* %p, i64 8)
      %v = load i8, i8* %p
      ret i8 %v
    lpad:
      %lp = landingpad { i8*, i32 } cleanup
      ret i8 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t uafBefore = getReportCountForType(mgr, IssueType::UseAfterFree);
  const size_t npdBefore =
      getReportCountForType(mgr, IssueType::NullDereference);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }
  // 2 UAFs: vec_invalidation + invoke_new_sized_delete_uaf.
  EXPECT_EQ(getReportCountForType(mgr, IssueType::UseAfterFree), uafBefore + 2);
  // 1 NPD: empty_optional_value.
  EXPECT_EQ(getReportCountForType(mgr, IssueType::NullDereference),
            npdBefore + 1);
}

TEST_F(PulseCheckerTest, ToplHeapReachabilityAndRelationalZ3Guard) {
  // 1. TOPL heap reachability predicate ~~> and !~>
  {
    auto prog = ToplProgram::parse(R"(
      property Reach
        start -> error: check(A, B, _) when A ~~> B
    )");
    ASSERT_TRUE(static_cast<bool>(prog));

    Heap heap;
    PulseFormula formula;
    AbstractValue a(nullptr, 2001);
    AbstractValue b(nullptr, 2002);
    AbstractValue c(nullptr, 2003);
    heap.addEdge(a, Access(0), Address(b));

    ToplHistory hit;
    hit.append({ToplEvent::Kind::Call,
                "check",
                {ToplValue{a, std::nullopt}, ToplValue{b, std::nullopt},
                 ToplValue{}},
                nullptr,
                {}});
    EXPECT_EQ(prog->evaluate(hit, formula, &heap).size(), 1u);

    ToplHistory miss;
    miss.append({ToplEvent::Kind::Call,
                 "check",
                 {ToplValue{a, std::nullopt}, ToplValue{c, std::nullopt},
                  ToplValue{}},
                 nullptr,
                 {}});
    EXPECT_TRUE(prog->evaluate(miss, formula, &heap).empty());
  }

  // 2. TOPL relational Z3-entailed guard (y = x + 1 |= x < y)
  {
    auto prog = ToplProgram::parse(R"(
      property Rel
        start -> error: step(X, Y, _) when X < Y
    )");
    ASSERT_TRUE(static_cast<bool>(prog));

    PulseFormula formula;
    AbstractValue x(nullptr, 3001);
    AbstractValue one(nullptr, 3002);
    AbstractValue y(nullptr, 3003);
    ASSERT_TRUE(formula.addBounds(one, 1, 1));
    ASSERT_TRUE(formula.addArithmeticOperation(y, x, one, "+"));

    ToplHistory hist;
    hist.append({ToplEvent::Kind::Call,
                 "step",
                 {ToplValue{x, std::nullopt}, ToplValue{y, std::nullopt},
                  ToplValue{}},
                 nullptr,
                 {}});
    EXPECT_EQ(prog->evaluate(hist, formula).size(), 1u);
  }
}

TEST_F(PulseCheckerTest, UnifiedTaintConfigJsonAndCustomPolicy) {
  auto cfg_or = TaintConfig::loadFromJson(R"({
    "sources": [
      { "procedure": "read_secret", "target": "ArgumentPositions", "positions": [1], "kinds": ["Secret"] },
      { "procedure": "fetch_token", "target": "ReturnValue", "kinds": ["Secret"] }
    ],
    "sinks": [
      { "procedure": "send_http", "target": "ArgumentPositions", "positions": [0], "kinds": ["PublicSink"] }
    ],
    "sanitizers": [
      { "procedure": "redact_secret", "target": "AllArguments", "kinds": ["SecretSanitizer"] }
    ],
    "policies": [
      { "sink_kind": "PublicSink", "source_kinds": ["Secret"], "sanitizer_kinds": ["SecretSanitizer"], "description": "Secret leaked to HTTP" }
    ]
  })",
                                          /*extend_default=*/true);
  if (!cfg_or) {
    FAIL() << llvm::toString(cfg_or.takeError());
  }

  struct ScopedTaintConfig {
    explicit ScopedTaintConfig(const TaintConfig *cfg) {
      TaintOperations::setConfig(cfg);
    }
    ~ScopedTaintConfig() { TaintOperations::setConfig(nullptr); }
  } guard(&*cfg_or);

  auto module = parseModule(context, R"(
    declare void @read_secret(i32, i8*)
    declare i8* @fetch_token()
    declare void @redact_secret(i8*)
    declare void @send_http(i8*, i8*)

    define void @bad_arg_flow(i8* %buf, i8* %hdr) {
    entry:
      call void @read_secret(i32 0, i8* %buf)
      call void @send_http(i8* %buf, i8* %hdr)
      ret void
    }

    define void @sanitized_flow(i8* %hdr) {
    entry:
      %tok = call i8* @fetch_token()
      call void @redact_secret(i8* %tok)
      call void @send_http(i8* %tok, i8* %hdr)
      ret void
    }

    define void @wrong_arg_pos_safe(i8* %safe) {
    entry:
      %tok = call i8* @fetch_token()
      call void @send_http(i8* %safe, i8* %tok)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before = getReportCountForType(mgr, IssueType::TaintError);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }
  EXPECT_EQ(getReportCountForType(mgr, IssueType::TaintError), before + 1);
}

TEST_F(PulseCheckerTest, IndirectCallResolutionAndDynamicTypeSpecialization) {
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @free_helper(i8* %p) {
    entry:
      call void @free(i8* %p)
      ret void
    }

    define i8 @direct_fp_slot_uaf() {
    entry:
      %slot = alloca void (i8*)*
      store void (i8*)* @free_helper, void (i8*)** %slot
      %fp = load void (i8*)*, void (i8*)** %slot
      %p = call i8* @malloc(i64 8)
      call void %fp(i8* %p)
      %v = load i8, i8* %p
      ret i8 %v
    }

    define void @apply(void (i8*)* %cb, i8* %arg) {
    entry:
      call void %cb(i8* %arg)
      ret void
    }

    define i8 @caller_higher_order_uaf() {
    entry:
      %p = call i8* @malloc(i64 8)
      call void @apply(void (i8*)* @free_helper, i8* %p)
      %v = load i8, i8* %p
      ret i8 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before = getReportCountForType(mgr, IssueType::UseAfterFree);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }
  // 2 UAFs: direct_fp_slot_uaf + caller_higher_order_uaf (via DynamicTypeNeeded
  // specialization of apply).
  EXPECT_EQ(getReportCountForType(mgr, IssueType::UseAfterFree), before + 2);
}

TEST_F(PulseCheckerTest, AggregateInsertExtractIntToPtrAndDeepMemcpy) {
  auto module = parseModule(context, R"(
    %Pair = type { i8*, i64 }

    declare i8* @malloc(i64)
    declare void @free(i8*)
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

    define i8 @insert_extract_uaf() {
    entry:
      %p = call i8* @malloc(i64 8)
      call void @free(i8* %p)
      %s0 = insertvalue %Pair undef, i8* %p, 0
      %s1 = insertvalue %Pair %s0, i64 42, 1
      %q = extractvalue %Pair %s1, 0
      %v = load i8, i8* %q
      ret i8 %v
    }

    define i8 @inttoptr_roundtrip_uaf() {
    entry:
      %p = call i8* @malloc(i64 8)
      call void @free(i8* %p)
      %i = ptrtoint i8* %p to i64
      %q = inttoptr i64 %i to i8*
      %v = load i8, i8* %q
      ret i8 %v
    }

    define i8 @deep_struct_memcpy_uaf() {
    entry:
      %src = alloca %Pair
      %dst = alloca %Pair
      %p = call i8* @malloc(i64 8)
      call void @free(i8* %p)
      %src_f0 = getelementptr %Pair, %Pair* %src, i32 0, i32 0
      store i8* %p, i8** %src_f0
      %src8 = bitcast %Pair* %src to i8*
      %dst8 = bitcast %Pair* %dst to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst8, i8* %src8, i64 16, i1 false)
      %dst_f0 = getelementptr %Pair, %Pair* %dst, i32 0, i32 0
      %q = load i8*, i8** %dst_f0
      %v = load i8, i8* %q
      ret i8 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before = getReportCountForType(mgr, IssueType::UseAfterFree);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }
  EXPECT_EQ(getReportCountForType(mgr, IssueType::UseAfterFree), before + 3);
}

TEST_F(PulseCheckerTest, ExpandedCCppModelsInteriorFreeReallocAndSmartPointers) {
  auto module = parseModule(context, R"(
    %uptr = type { i32* }
    %sptr = type { i32*, i8* }

    declare i8* @malloc(i64)
    declare i8* @realloc(i8*, i64)
    declare void @free(i8*)
    declare i8* @_Znwm(i64)
    declare void @_ZNSt10unique_ptrIiSt14default_deleteIiEEC1EPi(%uptr*, i32*)
    declare void @_ZNSt10unique_ptrIiSt14default_deleteIiEEC1EOS2_(%uptr*, %uptr*)
    declare i32* @_ZNKSt10unique_ptrIiSt14default_deleteIiEE3getEv(%uptr*)
    declare void @_ZNSt10unique_ptrIiSt14default_deleteIiEED1Ev(%uptr*)
    declare void @_ZNSt10shared_ptrIiEC1EPi(%sptr*, i32*)
    declare void @_ZNSt10shared_ptrIiEC1ERKS0_(%sptr*, %sptr*)
    declare i32* @_ZNKSt10shared_ptrIiE3getEv(%sptr*)
    declare void @_ZNSt10shared_ptrIiED1Ev(%sptr*)

    define void @interior_free_bug() {
    entry:
      %p = call i8* @malloc(i64 16)
      %q = getelementptr i8, i8* %p, i64 4
      call void @free(i8* %q)
      ret void
    }

    define i8 @realloc_zero_uaf() {
    entry:
      %p = call i8* @malloc(i64 16)
      %r = call i8* @realloc(i8* %p, i64 0)
      %v = load i8, i8* %p
      ret i8 %v
    }

    define i32 @unique_ptr_move_src_null_dst_valid() {
    entry:
      %raw8 = call i8* @_Znwm(i64 4)
      %raw = bitcast i8* %raw8 to i32*
      store i32 7, i32* %raw
      %u1 = alloca %uptr
      %u2 = alloca %uptr
      call void @_ZNSt10unique_ptrIiSt14default_deleteIiEEC1EPi(%uptr* %u1, i32* %raw)
      call void @_ZNSt10unique_ptrIiSt14default_deleteIiEEC1EOS2_(%uptr* %u2, %uptr* %u1)
      %p2 = call i32* @_ZNKSt10unique_ptrIiSt14default_deleteIiEE3getEv(%uptr* %u2)
      %ok = load i32, i32* %p2
      %p1 = call i32* @_ZNKSt10unique_ptrIiSt14default_deleteIiEE3getEv(%uptr* %u1)
      %bad = load i32, i32* %p1
      call void @_ZNSt10unique_ptrIiSt14default_deleteIiEED1Ev(%uptr* %u2)
      call void @_ZNSt10unique_ptrIiSt14default_deleteIiEED1Ev(%uptr* %u1)
      %sum = add i32 %ok, %bad
      ret i32 %sum
    }

    define i32 @shared_ptr_refcount_lifecycle() {
    entry:
      %raw8 = call i8* @_Znwm(i64 4)
      %raw = bitcast i8* %raw8 to i32*
      store i32 11, i32* %raw
      %s1 = alloca %sptr
      %s2 = alloca %sptr
      call void @_ZNSt10shared_ptrIiEC1EPi(%sptr* %s1, i32* %raw)
      call void @_ZNSt10shared_ptrIiEC1ERKS0_(%sptr* %s2, %sptr* %s1)
      call void @_ZNSt10shared_ptrIiED1Ev(%sptr* %s1)
      %alive = call i32* @_ZNKSt10shared_ptrIiE3getEv(%sptr* %s2)
      %ok = load i32, i32* %alive
      call void @_ZNSt10shared_ptrIiED1Ev(%sptr* %s2)
      %bad = load i32, i32* %alive
      %sum = add i32 %ok, %bad
      ret i32 %sum
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t invFreeBefore = getReportCountForType(mgr, IssueType::InvalidFree);
  const size_t uafBefore = getReportCountForType(mgr, IssueType::UseAfterFree);
  const size_t npdBefore =
      getReportCountForType(mgr, IssueType::NullDereference);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }
  EXPECT_EQ(getReportCountForType(mgr, IssueType::InvalidFree),
            invFreeBefore + 1);
  // 2 UAFs: realloc_zero_uaf + shared_ptr_refcount_lifecycle (after 2nd dtor).
  EXPECT_EQ(getReportCountForType(mgr, IssueType::UseAfterFree), uafBefore + 2);
  // 1 NPD: moved-from unique_ptr u1.get() dereference.
  EXPECT_EQ(getReportCountForType(mgr, IssueType::NullDereference),
            npdBefore + 1);
}

TEST_F(PulseCheckerTest,
       MidFunctionLeakSiteShortestTraceDedupAndInterprocNonDisj) {
  auto module = parseModule(context, R"(
    %Big = type { i64, i64, i64, i64 }

    declare i8* @malloc(i64)
    declare void @free(i8*)
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

    define void @overwrite_leak() {
    entry:
      %slot = alloca i8*
      %a = call i8* @malloc(i64 16)
      store i8* %a, i8** %slot
      %b = call i8* @malloc(i64 16)
      store i8* %b, i8** %slot
      call void @free(i8* %b)
      ret void
    }

    define void @mutate_big(%Big* %s) {
    entry:
      %f0 = getelementptr %Big, %Big* %s, i32 0, i32 0
      store i64 99, i64* %f0
      ret void
    }

    define i64 @caller_copies_and_mutates_in_callee(%Big* %src) {
    entry:
      %copy = alloca %Big
      %dst8 = bitcast %Big* %copy to i8*
      %src8 = bitcast %Big* %src to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst8, i8* %src8, i64 32, i1 false)
      call void @mutate_big(%Big* %copy)
      %f0 = getelementptr %Big, %Big* %copy, i32 0, i32 0
      %v = load i64, i64* %f0
      ret i64 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *leakFn = module->getFunction("overwrite_leak");
  ASSERT_NE(leakFn, nullptr);
  StoreInst *secondStore = findNthInstruction<StoreInst>(leakFn, 1);
  ASSERT_NE(secondStore, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t leakBefore = getReportCountForType(mgr, IssueType::MemoryLeak);
  const size_t copyBefore =
      getReportCountForType(mgr, IssueType::UnnecessaryCopy);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }

  // 1. Mid-function leak reported at the overwriting store, not ret.
  EXPECT_EQ(getReportCountForType(mgr, IssueType::MemoryLeak), leakBefore + 1);
  const BugReport *leakReport =
      getLastReportForType(mgr, IssueType::MemoryLeak);
  ASSERT_NE(leakReport, nullptr);
  ASSERT_FALSE(leakReport->get_steps().empty());
  EXPECT_EQ(leakReport->get_steps().back()->inst, secondStore);

  // 2. Interprocedural NonDisjunctiveSummary suppresses UnnecessaryCopy.
  EXPECT_EQ(getReportCountForType(mgr, IssueType::UnnecessaryCopy), copyBefore);

  // 3. DiagnosticManager prefers shorter trace on duplicate (location, type).
  const size_t npdBefore =
      getReportCountForType(mgr, IssueType::NullDereference);
  {
    auto &dm = DiagnosticManager::getInstance();
    Trace long_trace;
    long_trace.addEvent(secondStore, "step 1");
    long_trace.addEvent(secondStore, "step 2");
    long_trace.addEvent(secondStore, "step 3");
    long_trace.addEvent(secondStore, "step 4");
    dm.report(std::make_unique<AccessToInvalidAddress>(
        secondStore, "Null pointer dereference", "long", "fix",
        IssueType::NullDereference, std::move(long_trace)));

    Trace short_trace;
    short_trace.addEvent(secondStore, "short 1");
    dm.report(std::make_unique<AccessToInvalidAddress>(
        secondStore, "Null pointer dereference", "short", "fix",
        IssueType::NullDereference, std::move(short_trace)));
    dm.flush();
  }
  EXPECT_EQ(getReportCountForType(mgr, IssueType::NullDereference),
            npdBefore + 1);
  const BugReport *dedupReport =
      getLastReportForType(mgr, IssueType::NullDereference);
  ASSERT_NE(dedupReport, nullptr);
  // AccessToInvalidAddress adds the trace steps + error location + suggestion
  // (1 + 2 = 3 steps vs 4 + 2 = 6 for the replaced longer trace).
  EXPECT_EQ(dedupReport->get_steps().size(), 3u);
  EXPECT_TRUE(reportContainsTip(dedupReport, "short 1"));
  EXPECT_FALSE(reportContainsTip(dedupReport, "step 1"));
}

TEST_F(PulseCheckerTest, ExceptionRaisedInterprocSummaryAndTwoPassRecursiveSCC) {
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)
    declare void @__cxa_throw(i8*, i8*, i8*)
    declare i32 @__gxx_personality_v0(...)

    define void @always_throws() {
    entry:
      call void @__cxa_throw(i8* null, i8* null, i8* null)
      unreachable
    }

    define i8 @caller_catches_exception() personality i8* bitcast (i32 (...)* @__gxx_personality_v0 to i8*) {
    entry:
      %p = call i8* @malloc(i64 8)
      call void @free(i8* %p)
      invoke void @always_throws()
          to label %ok unwind label %catch
    ok:
      %never = load i8, i8* null
      ret i8 %never
    catch:
      %lp = landingpad { i8*, i32 }
          catch i8* null
      %v = load i8, i8* %p
      ret i8 %v
    }

    define void @rec_free(i8* %p, i32 %n) {
    entry:
      %done = icmp eq i32 %n, 0
      br i1 %done, label %base, label %step
    base:
      call void @free(i8* %p)
      ret void
    step:
      %n1 = sub i32 %n, 1
      call void @rec_free(i8* %p, i32 %n1)
      ret void
    }

    define i8 @caller_rec_uaf() {
    entry:
      %p = call i8* @malloc(i64 8)
      call void @rec_free(i8* %p, i32 1)
      %v = load i8, i8* %p
      ret i8 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t uafBefore = getReportCountForType(mgr, IssueType::UseAfterFree);
  const size_t npdBefore =
      getReportCountForType(mgr, IssueType::NullDereference);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }
  // %ok after invoke @always_throws() must NOT execute -> 0 NPD!
  EXPECT_EQ(getReportCountForType(mgr, IssueType::NullDereference), npdBefore);
  // 2 UAFs: catch landing pad load of freed %p + caller_rec_uaf after 2-pass
  // recursive SCC summary of rec_free!
  EXPECT_EQ(getReportCountForType(mgr, IssueType::UseAfterFree), uafBefore + 2);
}

TEST_F(PulseCheckerTest, FormulaLinearTableauInstanceOfFloatAndDisjunctiveJoin) {
  // 1. Incremental RREF linear tableau eagerly surfaces NewEqs (even without Z3).
  {
    AbstractValue x(nullptr, 4001);
    AbstractValue y(nullptr, 4002);
    AbstractValue z(nullptr, 4003);
    AbstractValue a(nullptr, 4004);
    AbstractValue b(nullptr, 4005);
    AbstractValue c(nullptr, 4006);
    AbstractValue d(nullptr, 4007);

    // x + y == 0 ∧ y == 0 => x == 0 (surfaced as NewEq::EqZero!)
    PulseFormula f;
    LinearConstraint sum_zero;
    sum_zero.kind = ConstraintKind::Equal;
    sum_zero.constant = 0;
    sum_zero.terms.emplace_back(x, 1);
    sum_zero.terms.emplace_back(y, 1);
    ASSERT_TRUE(f.addLinearConstraint(sum_zero));
    f.clearNewEqs();
    ASSERT_TRUE(f.addNull(y));
    EXPECT_TRUE(f.isNull(x));
    NewEqs eqs = f.takeNewEqs();
    bool saw_x_zero = false;
    for (const auto &eq : eqs) {
      if (eq.kind == NewEq::Kind::EqZero && eq.lhs == x) {
        saw_x_zero = true;
      }
    }
    EXPECT_TRUE(saw_x_zero);

    // a = b + c ∧ d = b + c => a == d (eager equality!)
    PulseFormula f2;
    ASSERT_TRUE(f2.addArithmeticOperation(a, b, c, "+"));
    ASSERT_TRUE(f2.addArithmeticOperation(d, b, c, "+"));
    EXPECT_TRUE(f2.areEqual(a, d));

    // Bound propagation on tableau: z == y + 1 ∧ y >= 5 ∧ z <= 5 => UNSAT!
    PulseFormula f3;
    ASSERT_TRUE(f3.addBounds(x, 1, 1));
    ASSERT_TRUE(f3.addArithmeticOperation(z, y, x, "+"));
    ASSERT_TRUE(f3.addBounds(y, 5, 100));
    EXPECT_FALSE(f3.addBounds(z, -100, 5));
  }

  // 2. Type / InstanceOf predicates & equality merge conflict detection.
  {
    AbstractValue o1(nullptr, 4101);
    AbstractValue o2(nullptr, 4102);
    PulseFormula tf;
    ASSERT_TRUE(tf.addExactType(o1, "DerivedA"));
    EXPECT_FALSE(tf.addExactType(o1, "DerivedB"));
    EXPECT_FALSE(tf.addNotInstanceOf(o1, "DerivedA"));
    ASSERT_TRUE(tf.addExactType(o2, "DerivedB"));
    EXPECT_FALSE(tf.addEquality(o1, o2));
  }

  // 3. Disjunctive equality preservation across PulseJoin (v ∈ {1, 2}).
  {
    auto module = parseModule(context, R"(
      define void @join_target(i32 %k) {
      entry:
        ret void
      }
    )");
    ASSERT_NE(module, nullptr);
    Argument *k = module->getFunction("join_target")->getArg(0);

    AbstractValueFactory factory;
    AbductiveDomain d1;
    AbstractValue v1 = factory.createFresh();
    d1.getPostStack().add(k, Address(v1));
    ASSERT_TRUE(d1.getPathFormula().addBounds(v1, 1, 1));

    AbductiveDomain d2;
    AbstractValue v2 = factory.createFresh();
    d2.getPostStack().add(k, Address(v2));
    ASSERT_TRUE(d2.getPathFormula().addBounds(v2, 2, 2));

    auto joined = PulseJoin::joinAbductive(d1, d2);
    ASSERT_TRUE(joined.has_value());
    const Address *jk = joined->getPostStack().find(k);
    ASSERT_NE(jk, nullptr);
    EXPECT_TRUE(joined->getPathFormula().isNonNull(jk->addr));
    auto eq3 = joined->getPathFormula().impliesComparison(
        jk->addr, std::nullopt, "==", std::nullopt, 3);
    ASSERT_TRUE(eq3.has_value());
    EXPECT_FALSE(*eq3);
    EXPECT_FALSE(joined->getPathFormula().addBounds(jk->addr, 3, 3));
  }

  // 4. Floating-point FAdd + FCmp path feasibility end-to-end.
  auto fpModule = parseModule(context, R"(
    define i8 @fp_branches() {
    entry:
      %s = fadd double 1.5, 2.5
      %bad = fcmp olt double %s, 3.0
      br i1 %bad, label %dead, label %live
    dead:
      %never = load i8, i8* null
      ret i8 %never
    live:
      %ok = fcmp oeq double %s, 4.0
      br i1 %ok, label %fire, label %exit
    fire:
      %boom = load i8, i8* null
      ret i8 %boom
    exit:
      ret i8 0
    }
  )");
  ASSERT_NE(fpModule, nullptr);
  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t npdBefore =
      getReportCountForType(mgr, IssueType::NullDereference);
  {
    PulseChecker checker(fpModule.get());
    checker.analyze();
  }
  // %dead is pruned (4.0 < 3.0 is UNSAT), %fire triggers 1 NullDereference.
  EXPECT_EQ(getReportCountForType(mgr, IssueType::NullDereference),
            npdBefore + 1);
}

TEST_F(PulseCheckerTest, LoopInvariantInferenceAndMonotonicWidening) {
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define i8 @loop_then_uaf(i32 %n) {
    entry:
      %p = call i8* @malloc(i64 16)
      br label %header
    header:
      %i = phi i32 [ 0, %entry ], [ %next, %body ]
      %cond = icmp slt i32 %i, %n
      br i1 %cond, label %body, label %exit
    body:
      %next = add i32 %i, 1
      br label %header
    exit:
      call void @free(i8* %p)
      %v = load i8, i8* %p
      ret i8 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  // Unit check: monotonic widening & LoopInvariantUnderInference marker.
  {
    Function *F = module->getFunction("loop_then_uaf");
    ASSERT_NE(F, nullptr);
    BasicBlock *header = nullptr;
    for (auto &BB : *F) {
      if (BB.getName() == "header")
        header = &BB;
    }
    ASSERT_NE(header, nullptr);
    auto *phi = dyn_cast<PHINode>(&header->front());
    ASSERT_NE(phi, nullptr);

    DominatorTree DT(*F);
    LoopInfo LI;
    LI.analyze(DT);
    LoopAbstraction la;
    la.initialize(LI);

    AbstractValueFactory factory;
    ExecutionDomain s0;
    AbstractValue i0 = factory.createFresh();
    s0.getAstate()->getPostStack().add(phi, Address(i0));
    ASSERT_TRUE(s0.getAstate()->getPathFormula().addBounds(i0, 0, 0));

    ExecutionDomain s1;
    AbstractValue i1 = factory.createFresh();
    s1.getAstate()->getPostStack().add(phi, Address(i1));
    ASSERT_TRUE(s1.getAstate()->getPathFormula().addBounds(i1, 1, 1));

    EXPECT_FALSE(la.visitHeader(header, s0));
    EXPECT_TRUE(la.visitHeader(header, s1));
    auto inv = la.inferInvariant(header, s0, s1);
    ASSERT_TRUE(inv.has_value());
    ASSERT_NE(inv->getAstate(), nullptr);
    EXPECT_TRUE(inv->getAstate()->isLoopInvariantUnderInference(header));
    const Address *inv_i = inv->getAstate()->getPostStack().find(phi);
    ASSERT_NE(inv_i, nullptr);
    EXPECT_EQ(inv->getAstate()->getPathFormula().getLowerBound(inv_i->addr), 0);
  }

  // End-to-end: loop converges cleanly and catches post-loop UAF.
  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t uafBefore = getReportCountForType(mgr, IssueType::UseAfterFree);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }
  EXPECT_EQ(getReportCountForType(mgr, IssueType::UseAfterFree), uafBefore + 1);
}

TEST_F(PulseCheckerTest, CppVtableVirtualDispatchIntraAndInterprocedural) {
  auto module = parseModule(context, R"(
    %Obj = type { i8** }

    declare i8* @malloc(i64)
    declare void @free(i8*)

    @_ZTV7Derived = constant { [4 x i8*] } {
      [4 x i8*] [
        i8* null,
        i8* null,
        i8* bitcast (void (i8*)* @vfn_noop to i8*),
        i8* bitcast (void (i8*)* @vfn_free to i8*)
      ]
    }

    define void @vfn_noop(i8* %p) {
    entry:
      ret void
    }

    define void @vfn_free(i8* %p) {
    entry:
      call void @free(i8* %p)
      ret void
    }

    define i8 @intra_vtable_slot1_uaf() {
    entry:
      %obj = alloca %Obj
      %vptr = getelementptr inbounds %Obj, %Obj* %obj, i32 0, i32 0
      store i8** getelementptr inbounds ({ [4 x i8*] }, { [4 x i8*] }* @_ZTV7Derived, i32 0, i32 0, i32 2), i8*** %vptr
      %vt = load i8**, i8*** %vptr
      %slot1 = getelementptr inbounds i8*, i8** %vt, i64 1
      %raw_fn = load i8*, i8** %slot1
      %fn = bitcast i8* %raw_fn to void (i8*)*
      %p = call i8* @malloc(i64 8)
      call void %fn(i8* %p)
      %v = load i8, i8* %p
      ret i8 %v
    }

    define void @invoke_virtual_slot1(%Obj* %this, i8* %p) {
    entry:
      %vptr = getelementptr inbounds %Obj, %Obj* %this, i32 0, i32 0
      %vt = load i8**, i8*** %vptr
      %slot1 = getelementptr inbounds i8*, i8** %vt, i64 1
      %raw_fn = load i8*, i8** %slot1
      %fn = bitcast i8* %raw_fn to void (i8*)*
      call void %fn(i8* %p)
      ret void
    }

    define i8 @interproc_vtable_dispatch_uaf() {
    entry:
      %obj = alloca %Obj
      %vptr = getelementptr inbounds %Obj, %Obj* %obj, i32 0, i32 0
      store i8** getelementptr inbounds ({ [4 x i8*] }, { [4 x i8*] }* @_ZTV7Derived, i32 0, i32 0, i32 2), i8*** %vptr
      %p = call i8* @malloc(i64 8)
      call void @invoke_virtual_slot1(%Obj* %obj, i8* %p)
      %v = load i8, i8* %p
      ret i8 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t uafBefore = getReportCountForType(mgr, IssueType::UseAfterFree);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }
  // 2 UAFs: intra_vtable_slot1_uaf + interproc_vtable_dispatch_uaf (resolved
  // through DynamicTypeNeeded specialization on the vtable pointer!).
  EXPECT_EQ(getReportCountForType(mgr, IssueType::UseAfterFree), uafBefore + 2);
}

TEST_F(PulseCheckerTest,
       MutualRecursionCycleDetectionAndTerminatingNoFalsePositive) {
  auto module = parseModule(context, R"(
    define void @direct_infinite(i32 %x) {
    entry:
      call void @direct_infinite(i32 %x)
      ret void
    }

    define void @ping(i32 %x) {
    entry:
      call void @pong(i32 %x)
      ret void
    }

    define void @pong(i32 %y) {
    entry:
      call void @ping(i32 %y)
      ret void
    }

    define void @countdown(i32 %n) {
    entry:
      %done = icmp eq i32 %n, 0
      br i1 %done, label %ret, label %step
    step:
      %n1 = sub i32 %n, 1
      call void @countdown(i32 %n1)
      br label %ret
    ret:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before =
      getReportCountForType(mgr, IssueType::MutualRecursionCycle);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }
  // Direct infinite recursion (direct_infinite) + mutual cycle (ping <-> pong),
  // while terminating countdown produces 0 reports.
  const size_t added =
      getReportCountForType(mgr, IssueType::MutualRecursionCycle) - before;
  EXPECT_GE(added, 2u);
  EXPECT_LE(added, 3u);
}

TEST_F(PulseCheckerTest, TypedExceptionCatchDispatchAndUncaughtPropagation) {
  auto module = parseModule(context, R"(
    @_ZTI4ErrA = external constant i8*
    @_ZTI4ErrB = external constant i8*

    declare i8* @malloc(i64)
    declare void @free(i8*)
    declare void @__cxa_throw(i8*, i8*, i8*)
    declare i32 @__gxx_personality_v0(...)
    declare i32 @llvm.eh.typeid.for(i8*)

    define void @throw_err_b() {
    entry:
      %exc = call i8* @malloc(i64 8)
      call void @__cxa_throw(i8* %exc, i8* bitcast (i8** @_ZTI4ErrB to i8*), i8* null)
      unreachable
    }

    define i8 @multi_catch_selects_err_b() personality i8* bitcast (i32 (...)* @__gxx_personality_v0 to i8*) {
    entry:
      %p = call i8* @malloc(i64 8)
      call void @free(i8* %p)
      invoke void @throw_err_b()
          to label %ok unwind label %lpad
    ok:
      ret i8 0
    lpad:
      %lp = landingpad { i8*, i32 }
          catch i8* bitcast (i8** @_ZTI4ErrA to i8*)
          catch i8* bitcast (i8** @_ZTI4ErrB to i8*)
      %sel = extractvalue { i8*, i32 } %lp, 1
      %tidA = call i32 @llvm.eh.typeid.for(i8* bitcast (i8** @_ZTI4ErrA to i8*))
      %isA = icmp eq i32 %sel, %tidA
      br i1 %isA, label %catchA, label %checkB
    catchA:
      %dead = load i8, i8* null
      ret i8 %dead
    checkB:
      %tidB = call i32 @llvm.eh.typeid.for(i8* bitcast (i8** @_ZTI4ErrB to i8*))
      %isB = icmp eq i32 %sel, %tidB
      br i1 %isB, label %catchB, label %rethrow
    catchB:
      %v = load i8, i8* %p
      ret i8 %v
    rethrow:
      resume { i8*, i32 } %lp
    }

    define void @mid_only_catches_err_a() personality i8* bitcast (i32 (...)* @__gxx_personality_v0 to i8*) {
    entry:
      invoke void @throw_err_b()
          to label %ok unwind label %lpad
    ok:
      ret void
    lpad:
      %lp = landingpad { i8*, i32 }
          catch i8* bitcast (i8** @_ZTI4ErrA to i8*)
      %never = load i8, i8* null
      ret void
    }

    define i8 @outer_catches_propagated_err_b() personality i8* bitcast (i32 (...)* @__gxx_personality_v0 to i8*) {
    entry:
      %q = call i8* @malloc(i64 8)
      call void @free(i8* %q)
      invoke void @mid_only_catches_err_a()
          to label %ok unwind label %lpad
    ok:
      ret i8 0
    lpad:
      %lp = landingpad { i8*, i32 }
          catch i8* bitcast (i8** @_ZTI4ErrB to i8*)
      %v = load i8, i8* %q
      ret i8 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t uafBefore = getReportCountForType(mgr, IssueType::UseAfterFree);
  const size_t npdBefore =
      getReportCountForType(mgr, IssueType::NullDereference);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }
  // Neither catchA nor mid_only_catches_err_a's lpad should run -> 0 NPD!
  EXPECT_EQ(getReportCountForType(mgr, IssueType::NullDereference), npdBefore);
  // Both multi_catch_selects_err_b::catchB and outer_catches_propagated_err_b
  // catch ErrB and hit their UAFs -> 2 UAF!
  EXPECT_EQ(getReportCountForType(mgr, IssueType::UseAfterFree), uafBefore + 2);
}

TEST_F(PulseCheckerTest, SolverSimplexBitvectorStringAndSubtypeHierarchy) {
  struct SubtypeGuard {
    ~SubtypeGuard() { PulseFormula::clearSubtypeHierarchy(); }
  } guard;

  // 1. Simplex difference-constraint closure: negative cycle & antisymmetry.
  {
    AbstractValue x(nullptr, 5001);
    AbstractValue y(nullptr, 5002);
    AbstractValue z(nullptr, 5003);

    PulseFormula f;
    LinearConstraint xy;
    xy.kind = ConstraintKind::Less;
    xy.terms = {{x, 1}, {y, -1}};
    LinearConstraint yz;
    yz.kind = ConstraintKind::Less;
    yz.terms = {{y, 1}, {z, -1}};
    LinearConstraint zx;
    zx.kind = ConstraintKind::Less;
    zx.terms = {{z, 1}, {x, -1}};
    ASSERT_TRUE(f.addLinearConstraint(xy));
    ASSERT_TRUE(f.addLinearConstraint(yz));
    EXPECT_FALSE(f.addLinearConstraint(zx));

    // x <= y ∧ y <= x => x == y eagerly surfaced as NewEq!
    PulseFormula f2;
    LinearConstraint le1;
    le1.kind = ConstraintKind::LessEqual;
    le1.terms = {{x, 1}, {y, -1}};
    LinearConstraint le2;
    le2.kind = ConstraintKind::LessEqual;
    le2.terms = {{y, 1}, {x, -1}};
    ASSERT_TRUE(f2.addLinearConstraint(le1));
    f2.clearNewEqs();
    ASSERT_TRUE(f2.addLinearConstraint(le2));
    EXPECT_TRUE(f2.areEqual(x, y));
    EXPECT_FALSE(f2.takeNewEqs().empty());
  }

  // 2. Bitvector bitwise ops (algebraic bounds + Z3 64-bit BV semantics).
  {
    AbstractValue x(nullptr, 5101);
    AbstractValue y(nullptr, 5102);
    AbstractValue c7(nullptr, 5103);
    AbstractValue c4(nullptr, 5104);
    AbstractValue masked(nullptr, 5105);
    AbstractValue ored(nullptr, 5106);
    AbstractValue a(nullptr, 5107);
    AbstractValue b(nullptr, 5108);
    AbstractValue c(nullptr, 5109);

    PulseFormula bf;
    ASSERT_TRUE(bf.addBounds(c7, 7, 7));
    ASSERT_TRUE(bf.addBounds(c4, 4, 4));
    ASSERT_TRUE(bf.addArithmeticOperation(masked, x, c7, "&"));
    EXPECT_EQ(bf.getLowerBound(masked), 0);
    EXPECT_EQ(bf.getUpperBound(masked), 7);
    ASSERT_TRUE(bf.addArithmeticOperation(ored, x, c4, "|"));
    EXPECT_TRUE(bf.isNonNull(ored));

    // Identity (x & y) & (x ^ y) == 0: asserting == 1 must be UNSAT via Z3 BV.
    PulseFormula bv2;
    bv2.addIntegerConstraint(x);
    bv2.addIntegerConstraint(y);
    ASSERT_TRUE(bv2.addArithmeticOperation(a, x, y, "&"));
    ASSERT_TRUE(bv2.addArithmeticOperation(b, x, y, "^"));
    ASSERT_TRUE(bv2.addArithmeticOperation(c, a, b, "&"));
    ASSERT_TRUE(bv2.addBounds(c, 1, 1));
    EXPECT_TRUE(bv2.isUnsat());
  }

  // 3. String & regexp theory (constants, concat, length, regex match).
  {
    AbstractValue s1(nullptr, 5201);
    AbstractValue s2(nullptr, 5202);
    AbstractValue s3(nullptr, 5203);
    AbstractValue len3(nullptr, 5204);

    PulseFormula sf;
    ASSERT_TRUE(sf.addStringConstant(s1, "foo"));
    ASSERT_TRUE(sf.addStringConstant(s2, "bar"));
    ASSERT_TRUE(sf.addStringConcat(s3, s1, s2));
    ASSERT_EQ(sf.asConstantString(s3), std::optional<std::string>("foobar"));
    ASSERT_TRUE(sf.addStringLength(s3, len3));
    EXPECT_EQ(sf.asConstantInt(len3), std::optional<int64_t>(6));
    EXPECT_TRUE(sf.addStringRegexMatch(s3, "^foo.*bar$", true));
    EXPECT_FALSE(sf.addStringRegexMatch(s3, "^baz", true));
  }

  // 4. Itanium ABI RTTI subtype hierarchy: catch(Base&) catches thrown Derived.
  auto module = parseModule(context, R"(
    @_ZTVN10__cxxabiv120__si_class_type_infoE = external global i8*
    @_ZTS4Base = constant [6 x i8] c"4Base\00"
    @_ZTS7Derived = constant [9 x i8] c"7Derived\00"
    @_ZTI4Base = constant { i8*, i8* } {
      i8* bitcast (i8** @_ZTVN10__cxxabiv120__si_class_type_infoE to i8*),
      i8* getelementptr inbounds ([6 x i8], [6 x i8]* @_ZTS4Base, i32 0, i32 0)
    }
    @_ZTI7Derived = constant { i8*, i8*, i8* } {
      i8* bitcast (i8** @_ZTVN10__cxxabiv120__si_class_type_infoE to i8*),
      i8* getelementptr inbounds ([9 x i8], [9 x i8]* @_ZTS7Derived, i32 0, i32 0),
      i8* bitcast ({ i8*, i8* }* @_ZTI4Base to i8*)
    }

    declare i8* @malloc(i64)
    declare void @free(i8*)
    declare void @__cxa_throw(i8*, i8*, i8*)
    declare i32 @__gxx_personality_v0(...)

    define void @throw_derived() {
    entry:
      %e = call i8* @malloc(i64 8)
      call void @__cxa_throw(i8* %e, i8* bitcast ({ i8*, i8*, i8* }* @_ZTI7Derived to i8*), i8* null)
      unreachable
    }

    define i8 @catch_base_catches_derived() personality i8* bitcast (i32 (...)* @__gxx_personality_v0 to i8*) {
    entry:
      %p = call i8* @malloc(i64 8)
      call void @free(i8* %p)
      invoke void @throw_derived()
          to label %ok unwind label %lpad
    ok:
      ret i8 0
    lpad:
      %lp = landingpad { i8*, i32 }
          catch i8* bitcast ({ i8*, i8* }* @_ZTI4Base to i8*)
      %v = load i8, i8* %p
      ret i8 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t uafBefore = getReportCountForType(mgr, IssueType::UseAfterFree);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }
  EXPECT_EQ(getReportCountForType(mgr, IssueType::UseAfterFree), uafBefore + 1);
}

TEST_F(PulseCheckerTest, MultiHopDynamicTypeSpecializationAndLoopStacks) {
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @free_cb(i8* %p) {
    entry:
      call void @free(i8* %p)
      ret void
    }

    define void @apply_cb(void (i8*)* %fp, i8* %p) {
    entry:
      call void %fp(i8* %p)
      ret void
    }

    define void @forward_cb(void (i8*)* %cb, i8* %p) {
    entry:
      call void @apply_cb(void (i8*)* %cb, i8* %p)
      ret void
    }

    define i8 @two_hop_uaf() {
    entry:
      %p = call i8* @malloc(i64 8)
      call void @forward_cb(void (i8*)* @free_cb, i8* %p)
      %v = load i8, i8* %p
      ret i8 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  // Per-disjunct nested loop stack unit test.
  {
    Function *F = module->getFunction("two_hop_uaf");
    const BasicBlock *outer = &F->getEntryBlock();
    const BasicBlock *inner = &module->getFunction("apply_cb")->getEntryBlock();
    AbductiveDomain dom;
    dom.recordLoopPathStamp(outer, 11);
    dom.recordLoopPathStamp(inner, 22);
    dom.recordLoopPathStamp(inner, 33);
    EXPECT_EQ(dom.getDisjunctLoopVisits(outer), 1u);
    EXPECT_EQ(dom.getDisjunctLoopVisits(inner), 2u);
    dom.popLoopHeader(inner);
    EXPECT_EQ(dom.getDisjunctLoopVisits(outer), 1u);
    EXPECT_EQ(dom.getDisjunctLoopVisits(inner), 0u);
  }

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t uafBefore = getReportCountForType(mgr, IssueType::UseAfterFree);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }
  EXPECT_EQ(getReportCountForType(mgr, IssueType::UseAfterFree), uafBefore + 1);
}

TEST_F(PulseCheckerTest,
       SpecializedCheckersRetainCycleInfiniteLoopAndTransitiveAccess) {
  auto module = parseModule(context, R"(
    %Node = type { %sptr, %wptr }
    %sptr = type { %Node*, i8* }
    %wptr = type { %Node*, i8* }

    declare i8* @malloc(i64)
    declare void @free(i8*)
    declare i8* @_Znwm(i64)
    declare void @_ZNSt10shared_ptrI4NodeEC1EPS0_(%sptr*, %Node*)
    declare %sptr* @_ZNSt10shared_ptrI4NodeEaSERKS1_(%sptr*, %sptr*)
    declare void @_ZNSt10shared_ptrI4NodeED1Ev(%sptr*)
    declare %wptr* @_ZNSt8weak_ptrI4NodeEaSERKSt10shared_ptrIS0_E(%wptr*, %sptr*)

    define void @strong_retain_cycle() {
    entry:
      %ra = call i8* @_Znwm(i64 32)
      %a = bitcast i8* %ra to %Node*
      %rb = call i8* @_Znwm(i64 32)
      %b = bitcast i8* %rb to %Node*
      %sa = alloca %sptr
      %sb = alloca %sptr
      call void @_ZNSt10shared_ptrI4NodeEC1EPS0_(%sptr* %sa, %Node* %a)
      call void @_ZNSt10shared_ptrI4NodeEC1EPS0_(%sptr* %sb, %Node* %b)
      %a_next = getelementptr %Node, %Node* %a, i32 0, i32 0
      %b_next = getelementptr %Node, %Node* %b, i32 0, i32 0
      call %sptr* @_ZNSt10shared_ptrI4NodeEaSERKS1_(%sptr* %a_next, %sptr* %sb)
      call %sptr* @_ZNSt10shared_ptrI4NodeEaSERKS1_(%sptr* %b_next, %sptr* %sa)
      call void @_ZNSt10shared_ptrI4NodeED1Ev(%sptr* %sb)
      call void @_ZNSt10shared_ptrI4NodeED1Ev(%sptr* %sa)
      ret void
    }

    define void @weak_ptr_breaks_cycle() {
    entry:
      %ra = call i8* @_Znwm(i64 32)
      %a = bitcast i8* %ra to %Node*
      %rb = call i8* @_Znwm(i64 32)
      %b = bitcast i8* %rb to %Node*
      %sa = alloca %sptr
      %sb = alloca %sptr
      call void @_ZNSt10shared_ptrI4NodeEC1EPS0_(%sptr* %sa, %Node* %a)
      call void @_ZNSt10shared_ptrI4NodeEC1EPS0_(%sptr* %sb, %Node* %b)
      %a_next = getelementptr %Node, %Node* %a, i32 0, i32 0
      %b_prev = getelementptr %Node, %Node* %b, i32 0, i32 1
      call %sptr* @_ZNSt10shared_ptrI4NodeEaSERKS1_(%sptr* %a_next, %sptr* %sb)
      call %wptr* @_ZNSt8weak_ptrI4NodeEaSERKSt10shared_ptrIS0_E(%wptr* %b_prev, %sptr* %sa)
      call void @_ZNSt10shared_ptrI4NodeED1Ev(%sptr* %sb)
      call void @_ZNSt10shared_ptrI4NodeED1Ev(%sptr* %sa)
      ret void
    }

    define void @manifest_infinite_loop() {
    entry:
      br label %loop
    loop:
      %x = phi i32 [ 0, %entry ], [ %x, %loop ]
      br label %loop
    }

    define i32 @terminating_loop() {
    entry:
      br label %header
    header:
      %i = phi i32 [ 0, %entry ], [ %next, %body ]
      %cond = icmp slt i32 %i, 3
      br i1 %cond, label %body, label %exit
    body:
      %next = add i32 %i, 1
      br label %header
    exit:
      ret i32 %i
    }

    define void @mid_alloc_helper() {
    entry:
      %p = call i8* @malloc(i64 16)
      call void @free(i8* %p)
      ret void
    }

    define void @realtime_audio_cb() {
    entry:
      call void @mid_alloc_helper()
      ret void
    }

    define void @background_init() {
    entry:
      call void @mid_alloc_helper()
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto policies_or = loadTransitiveAccessPoliciesFromJson(R"json({
    "policies": [
      {
        "name": "no-alloc-in-rt",
        "source_functions": ["^realtime_"],
        "forbidden_callees": ["^malloc$"],
        "description": "Realtime callback must not transitively allocate"
      }
    ]
  })json");
  ASSERT_TRUE(static_cast<bool>(policies_or));

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t rcBefore = getReportCountForType(mgr, IssueType::RetainCycle);
  const size_t infBefore = getReportCountForType(mgr, IssueType::InfiniteLoop);
  const size_t taBefore =
      getReportCountForType(mgr, IssueType::TransitiveAccess);
  {
    PulseChecker checker(module.get());
    checker.setTransitiveAccessPolicies(std::move(*policies_or));
    checker.analyze();
  }
  // 1 RetainCycle (strong_retain_cycle only; weak_ptr_breaks_cycle emits 0).
  EXPECT_EQ(getReportCountForType(mgr, IssueType::RetainCycle), rcBefore + 1);
  // 1 InfiniteLoop (manifest_infinite_loop only; terminating_loop emits 0).
  EXPECT_EQ(getReportCountForType(mgr, IssueType::InfiniteLoop), infBefore + 1);
  // 1 TransitiveAccess (realtime_audio_cb -> mid_alloc_helper -> malloc).
  EXPECT_EQ(getReportCountForType(mgr, IssueType::TransitiveAccess),
            taBefore + 1);
}

TEST_F(PulseCheckerTest,
       LLVMIRGlobalsNoReturnDynamicAllocaVectorsAndExpandedSTLModels) {
  auto module = parseModule(context, R"(
    %str = type { i8*, i64 }
    %"class.std::basic_string_view" = type { i8*, i64 }
    %sptr = type { i32*, i8* }
    %wptr = type { i32*, i8* }

    @.hello = private constant [6 x i8] c"hello\00"

    declare i8* @malloc(i64)
    declare void @free(i8*)
    declare i8* @_Znwm(i64)
    declare i64 @strlen(i8*)
    declare void @exit(i32) noreturn
    declare i32 @pipe(i32*)
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)
    declare i8* @_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE5c_strEv(%str*)
    declare void @_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEED1Ev(%str*)
    declare void @_ZNSt17basic_string_viewIcSt11char_traitsIcEEC1EPKc(%"class.std::basic_string_view"*, i8*)
    declare i8* @_ZNKSt17basic_string_viewIcSt11char_traitsIcEE4dataEv(%"class.std::basic_string_view"*)
    declare void @_ZNSt10shared_ptrIiEC1EPi(%sptr*, i32*)
    declare void @_ZNSt10shared_ptrIiED1Ev(%sptr*)
    declare void @_ZNSt8weak_ptrIiEC1ERKSt10shared_ptrIiE(%wptr*, %sptr*)
    declare void @_ZNKSt8weak_ptrIiE4lockEv(%sptr*, %wptr*)
    declare i32* @_ZNKSt10shared_ptrIiE3getEv(%sptr*)

    define i8 @global_strlen_exact() {
    entry:
      %len = call i64 @strlen(i8* getelementptr inbounds ([6 x i8], [6 x i8]* @.hello, i64 0, i64 0))
      %bad = icmp ne i64 %len, 5
      br i1 %bad, label %dead, label %ok
    dead:
      %never = load i8, i8* null
      ret i8 %never
    ok:
      ret i8 0
    }

    define i8 @noreturn_suppresses_post_exit() {
    entry:
      %p = call i8* @malloc(i64 16)
      call void @free(i8* %p)
      %q = call i8* @malloc(i64 16)
      call void @exit(i32 1)
      %v = load i8, i8* %p
      ret i8 %v
    }

    define i8 @dyn_alloca_symbolic_oob(i64 %n, i64 %i) {
    entry:
      %buf = alloca i8, i64 %n
      %oob = icmp sge i64 %i, %n
      br i1 %oob, label %bad, label %ok
    bad:
      %p = getelementptr i8, i8* %buf, i64 %i
      %v = load i8, i8* %p
      ret i8 %v
    ok:
      ret i8 0
    }

    define i8 @vector_insert_extract_uaf() {
    entry:
      %p = call i8* @malloc(i64 8)
      call void @free(i8* %p)
      %v0 = insertelement <2 x i8*> undef, i8* %p, i32 1
      %q = extractelement <2 x i8*> %v0, i32 1
      %x = load i8, i8* %q
      ret i8 %x
    }

    define void @cheap_string_view_copy(%"class.std::basic_string_view"* %src) {
    entry:
      %copy = alloca %"class.std::basic_string_view"
      %d = bitcast %"class.std::basic_string_view"* %copy to i8*
      %s = bitcast %"class.std::basic_string_view"* %src to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %d, i8* %s, i64 16, i1 false)
      ret void
    }

    define i8 @string_view_dangling_uaf() {
    entry:
      %s = alloca %str
      %cs = call i8* @_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE5c_strEv(%str* %s)
      %sv = alloca %"class.std::basic_string_view"
      call void @_ZNSt17basic_string_viewIcSt11char_traitsIcEEC1EPKc(%"class.std::basic_string_view"* %sv, i8* %cs)
      call void @_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEED1Ev(%str* %s)
      %dp = call i8* @_ZNKSt17basic_string_viewIcSt11char_traitsIcEE4dataEv(%"class.std::basic_string_view"* %sv)
      %c = load i8, i8* %dp
      ret i8 %c
    }

    define i32 @weak_ptr_lock_after_dtor_null() {
    entry:
      %r8 = call i8* @_Znwm(i64 4)
      %r = bitcast i8* %r8 to i32*
      %sp = alloca %sptr
      %wp = alloca %wptr
      %locked = alloca %sptr
      call void @_ZNSt10shared_ptrIiEC1EPi(%sptr* %sp, i32* %r)
      call void @_ZNSt8weak_ptrIiEC1ERKSt10shared_ptrIiE(%wptr* %wp, %sptr* %sp)
      call void @_ZNSt10shared_ptrIiED1Ev(%sptr* %sp)
      call void @_ZNKSt8weak_ptrIiE4lockEv(%sptr* %locked, %wptr* %wp)
      %p = call i32* @_ZNKSt10shared_ptrIiE3getEv(%sptr* %locked)
      %v = load i32, i32* %p
      ret i32 %v
    }

    define void @pipe_resource_leak() {
    entry:
      %fds = alloca [2 x i32]
      %p0 = getelementptr [2 x i32], [2 x i32]* %fds, i64 0, i64 0
      %rc = call i32 @pipe(i32* %p0)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t uafBefore = getReportCountForType(mgr, IssueType::UseAfterFree);
  const size_t npdBefore =
      getReportCountForType(mgr, IssueType::NullDereference);
  const size_t oobBefore = getReportCountForType(mgr, IssueType::OutOfBounds);
  const size_t leakBefore = getReportCountForType(mgr, IssueType::MemoryLeak);
  const size_t rleakBefore =
      getReportCountForType(mgr, IssueType::ResourceLeak);
  const size_t copyBefore =
      getReportCountForType(mgr, IssueType::UnnecessaryCopy);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }
  // 2 UAFs: vector_insert_extract_uaf + string_view_dangling_uaf (0 from noreturn!).
  EXPECT_EQ(getReportCountForType(mgr, IssueType::UseAfterFree), uafBefore + 2);
  // 1 NPD: weak_ptr_lock_after_dtor_null (0 from global_strlen_exact dead branch!).
  EXPECT_EQ(getReportCountForType(mgr, IssueType::NullDereference),
            npdBefore + 1);
  // 1 OOB: dyn_alloca_symbolic_oob.
  EXPECT_EQ(getReportCountForType(mgr, IssueType::OutOfBounds), oobBefore + 1);
  // 0 MemoryLeaks: noreturn exit suppresses leak of %q.
  EXPECT_EQ(getReportCountForType(mgr, IssueType::MemoryLeak), leakBefore);
  // >= 1 ResourceLeak from pipe_resource_leak.
  EXPECT_GE(getReportCountForType(mgr, IssueType::ResourceLeak),
            rleakBefore + 1);
  // 0 UnnecessaryCopy: std::basic_string_view is in cheap-copy allowlist.
  EXPECT_EQ(getReportCountForType(mgr, IssueType::UnnecessaryCopy), copyBefore);
}

TEST_F(PulseCheckerTest, FieldSensitiveTaintDataFlowOnlyAndAllocationSources) {
  auto cfg_or = TaintConfig::loadFromJson(R"json({
    "sources": [
      {
        "procedure": "taint_f0",
        "target": "ArgumentPositions",
        "positions": [0],
        "field_index": 0,
        "kinds": ["UserInput"]
      },
      {
        "procedure": "df_only_source",
        "target": "ReturnValue",
        "kinds": ["InternalTag"],
        "data_flow_only": true
      }
    ],
    "sinks": [
      {
        "procedure": "sink_f0",
        "target": "ArgumentPositions",
        "positions": [0],
        "field_index": 0,
        "kinds": ["UserInput"]
      },
      {
        "procedure": "sink_f1",
        "target": "ArgumentPositions",
        "positions": [0],
        "field_index": 1,
        "kinds": ["UserInput"]
      },
      {
        "procedure": "generic_sink",
        "target": "AllArguments",
        "kinds": ["UserInput"]
      }
    ],
    "sanitizers": [
      {
        "procedure": "clean_f0",
        "target": "ArgumentPositions",
        "positions": [0],
        "field_index": 0,
        "kinds": ["Unknown"]
      }
    ],
    "allocation_sources": [
      {
        "class_name": "SecretCred",
        "kinds": ["Sensitive"]
      }
    ],
    "source_blocks": [
      {
        "procedure": "block_source",
        "target": "ReturnValue",
        "kinds": ["UserInput"]
      }
    ]
  })json", /*extend_default=*/false);
  ASSERT_TRUE(static_cast<bool>(cfg_or));
  TaintConfig cfg = std::move(*cfg_or);

  struct TaintGuard {
    explicit TaintGuard(const TaintConfig *c) { TaintOperations::setConfig(c); }
    ~TaintGuard() { TaintOperations::setConfig(nullptr); }
  } guard(&cfg);

  auto module = parseModule(context, R"(
    %Pair = type { i32, i32 }
    %struct.SecretCred = type { i64 }

    declare void @taint_f0(%Pair*)
    declare void @clean_f0(%Pair*)
    declare void @sink_f0(%Pair*)
    declare void @sink_f1(%Pair*)
    declare i8* @df_only_source()
    declare i8* @block_source()
    declare void @generic_sink(i8*)

    define void @field0_does_not_taint_field1() {
    entry:
      %p = alloca %Pair
      call void @taint_f0(%Pair* %p)
      call void @sink_f1(%Pair* %p)
      ret void
    }

    define void @field0_taints_field0() {
    entry:
      %p = alloca %Pair
      call void @taint_f0(%Pair* %p)
      call void @sink_f0(%Pair* %p)
      ret void
    }

    define void @field0_sanitized_clean() {
    entry:
      %p = alloca %Pair
      call void @taint_f0(%Pair* %p)
      call void @clean_f0(%Pair* %p)
      call void @sink_f0(%Pair* %p)
      ret void
    }

    define void @data_flow_only_not_reported() {
    entry:
      %x = call i8* @df_only_source()
      call void @generic_sink(i8* %x)
      ret void
    }

    define void @allocation_source_reported() {
    entry:
      %cred = alloca %struct.SecretCred
      %raw = bitcast %struct.SecretCred* %cred to i8*
      call void @generic_sink(i8* %raw)
      ret void
    }

    define void @source_block_reported() {
    entry:
      %b = call i8* @block_source()
      call void @generic_sink(i8* %b)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t taintBefore = getReportCountForType(mgr, IssueType::TaintError);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }
  // Exactly 3 TaintErrors:
  // 1. field0_taints_field0 (while field0_does_not_taint_field1 and
  //    field0_sanitized_clean emit 0)
  // 2. allocation_source_reported (while data_flow_only_not_reported emits 0)
  // 3. source_block_reported
  EXPECT_EQ(getReportCountForType(mgr, IssueType::TaintError), taintBefore + 3);
}

TEST_F(PulseCheckerTest, RobustnessNonDoubleFloatAndSharedPtrLifecycle) {
  // Regression 1: Non-IEEEdouble ConstantFP (x86_fp80 / half) must convert
  // safely via APFloat::convert instead of asserting in convertToDouble().
  // Regression 2: std::shared_ptr copy-assignment and reset() must reclaim
  // moved ExecutionDomain after delegating to destructor (no use-after-move
  // crash).
  auto module = parseModule(context, R"(
    %struct.SP = type { i32* }

    declare void @_ZNSt3__110shared_ptrIiEC1EPi(%struct.SP*, i32*)
    declare %struct.SP* @_ZNSt3__110shared_ptrIiEaSERKS1_(%struct.SP*, %struct.SP*)
    declare void @_ZNSt3__110shared_ptrIiE5resetEv(%struct.SP*)
    declare i32* @_ZNKSt3__110shared_ptrIiE3getEv(%struct.SP*)
    declare i8* @malloc(i64)

    define i32 @x86_fp80_and_half_safe(x86_fp80 %x, half %h) {
    entry:
      %s80 = fadd x86_fp80 %x, 0xK4000C00000000000000
      %c80 = fcmp ogt x86_fp80 %s80, 0xK0000000000000000000
      %sh = fadd half %h, 0xH3C00
      %ch = fcmp oeq half %sh, 0xH4000
      %both = and i1 %c80, %ch
      %res = zext i1 %both to i32
      ret i32 %res
    }

    define i32 @shared_ptr_copy_assign_and_reset() {
    entry:
      %a = alloca %struct.SP
      %b = alloca %struct.SP
      %raw1 = call i8* @malloc(i64 4)
      %p1 = bitcast i8* %raw1 to i32*
      store i32 7, i32* %p1
      %raw2 = call i8* @malloc(i64 4)
      %p2 = bitcast i8* %raw2 to i32*
      store i32 9, i32* %p2
      call void @_ZNSt3__110shared_ptrIiEC1EPi(%struct.SP* %a, i32* %p1)
      call void @_ZNSt3__110shared_ptrIiEC1EPi(%struct.SP* %b, i32* %p2)
      %ref = call %struct.SP* @_ZNSt3__110shared_ptrIiEaSERKS1_(%struct.SP* %a, %struct.SP* %b)
      call void @_ZNSt3__110shared_ptrIiE5resetEv(%struct.SP* %a)
      %gp = call i32* @_ZNKSt3__110shared_ptrIiE3getEv(%struct.SP* %b)
      %v = load i32, i32* %gp
      ret i32 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  PulseChecker checker(module.get());
  checker.analyze();
  EXPECT_TRUE(checker.getSummaryManager().hasSummary(
      module->getFunction("x86_fp80_and_half_safe")));
  EXPECT_TRUE(checker.getSummaryManager().hasSummary(
      module->getFunction("shared_ptr_copy_assign_and_reset")));
}

TEST_F(PulseCheckerTest,
       RobustnessSummaryMaterializeCanonicalizationAndJoinBounds) {
  // 1. Interprocedural summary application where aliased struct fields force
  //    canonicalization inside materializePreFromAddress (regression for
  //    dangling caller_target / caller_hist references across canonicalize()).
  auto module = parseModule(context, R"(
    %Node = type { %Node*, i32 }

    define i32 @read_both(%Node* %a, %Node* %b) {
    entry:
      %a_next_p = getelementptr inbounds %Node, %Node* %a, i32 0, i32 0
      %a_next = load %Node*, %Node** %a_next_p
      %b_next_p = getelementptr inbounds %Node, %Node* %b, i32 0, i32 0
      %b_next = load %Node*, %Node** %b_next_p
      %eq = icmp eq %Node* %a_next, %b_next
      br i1 %eq, label %same, label %diff
    same:
      %vp = getelementptr inbounds %Node, %Node* %a_next, i32 0, i32 1
      %v = load i32, i32* %vp
      ret i32 %v
    diff:
      ret i32 0
    }

    define i32 @caller_aliased() {
    entry:
      %n1 = alloca %Node
      %n2 = alloca %Node
      %leaf = alloca %Node
      %lvp = getelementptr inbounds %Node, %Node* %leaf, i32 0, i32 1
      store i32 42, i32* %lvp
      %n1p = getelementptr inbounds %Node, %Node* %n1, i32 0, i32 0
      store %Node* %leaf, %Node** %n1p
      %n2p = getelementptr inbounds %Node, %Node* %n2, i32 0, i32 0
      store %Node* %leaf, %Node** %n2p
      %r = call i32 @read_both(%Node* %n1, %Node* %n2)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);
  {
    PulseChecker checker(module.get());
    checker.analyze();
    EXPECT_TRUE(checker.getSummaryManager().hasSummary(
        module->getFunction("caller_aliased")));
  }

  // 2. ValueHistory depth cap + iterative destructor.
  {
    ValueHistory hist;
    for (unsigned i = 0; i < 150; ++i) {
      hist.addEvent(ValueHistory::EventKind::Unknown, nullptr, nullptr);
    }
    EXPECT_LE(hist.getEvents().size(), 80u);
  }

  // 3. TransitiveInfo deduplication and entry cap (256).
  {
    TransitiveInfo ti;
    Function *F = module->getFunction("caller_aliased");
    const Instruction *I = &F->getEntryBlock().front();
    for (unsigned i = 0; i < 400; ++i) {
      ti.recordCallResolution(I, F, TransitiveInfo::CallResolution::Resolved);
    }
    EXPECT_EQ(ti.getCallResolutions().size(), 1u);
    for (unsigned i = 0; i < 350; ++i) {
      ti.recordTransitiveAccess(I, F);
    }
    EXPECT_EQ(ti.getTransitiveAccesses().size(), 1u);
  }

  // 4. Cyclic heap join + in-place canonicalization must terminate and preserve
  //    metadata without accumulating dead equalities on one-sided values.
  {
    Argument *argA = module->getFunction("read_both")->getArg(0);
    AbstractValueFactory factory;
    AbductiveDomain d1;
    AbductiveDomain d2;
    AbstractValue a1 = factory.createFresh();
    AbstractValue a2 = factory.createFresh();
    AbstractValue extra1 = factory.createFresh();
    d1.getPostStack().add(argA, Address(a1));
    d1.getPostHeap().addEdge(a1, Access(0u), Address(a1)); // self-cycle
    d1.getPostHeap().addEdge(a1, Access(1u), Address(extra1));
    ASSERT_TRUE(d1.getPathFormula().addBounds(extra1, 10, 20));
    d1.setAllocationSize(a1, 16);

    d2.getPostStack().add(argA, Address(a2));
    d2.getPostHeap().addEdge(a2, Access(0u), Address(a2)); // self-cycle
    d2.setAllocationSize(a2, 32);

    auto joined = PulseJoin::joinAbductive(d1, d2);
    ASSERT_TRUE(joined.has_value());
    const Address *ja = joined->getPostStack().find(argA);
    ASSERT_NE(ja, nullptr);
    EXPECT_FALSE(joined->getPathFormula().hasEqualities());
    auto sz = joined->getAllocationSize(ja->addr);
    ASSERT_TRUE(sz.has_value());
    EXPECT_EQ(*sz, 16u);

    // Exercise in-place canonicalize() when equalities merge two AVs.
    AbstractValue m1 = factory.createFresh();
    AbstractValue m2 = factory.createFresh();
    joined->getPostStack().add(module->getFunction("read_both")->getArg(1),
                               Address(m2));
    joined->setAllocationSize(m2, 64);
    joined->addEquality(m1, m2);
    joined->canonicalize();
    AbstractValue rep = joined->getCanonical(m2);
    const Address *jb =
        joined->getPostStack().find(module->getFunction("read_both")->getArg(1));
    ASSERT_NE(jb, nullptr);
    EXPECT_EQ(jb->addr, rep);
    EXPECT_EQ(joined->getAllocationSize(rep), 64u);
  }
}

TEST_F(PulseCheckerTest, RobustnessFormulaSmtSlicingAndToggleSoundness) {
  struct SmtOptionGuard {
    bool prev;
    SmtOptionGuard() : prev(pulse::options::disableSMT()) {}
    ~SmtOptionGuard() { pulse::options::setDisableSMT(prev); }
  } guard;

  AbstractValue x(nullptr, 9101);
  AbstractValue y(nullptr, 9102);
  AbstractValue z(nullptr, 9103);
  AbstractValue mask1(nullptr, 9104);
  AbstractValue mask4(nullptr, 9105);
  AbstractValue r1(nullptr, 9106);
  AbstractValue r2(nullptr, 9107);
  AbstractValue uninit_u(nullptr, 9108);
  AbstractValue uninit_v(nullptr, 9109);

  // 1. SAT cache must not return stale SMT-off SAT result when SMT is toggled
  //    back ON for a relational bitwise contradiction `(x & 3) == 1 && (x & 1) == 0`.
  {
    AbstractValue mask3(nullptr, 9110);
    PulseFormula f;
    ASSERT_TRUE(f.addBounds(mask3, 3, 3));
    ASSERT_TRUE(f.addBounds(mask1, 1, 1));
    ASSERT_TRUE(f.addBitwiseAnd(r1, x, mask3));
    ASSERT_TRUE(f.addBitwiseAnd(r2, x, mask1));
    ASSERT_TRUE(f.addBounds(r1, 1, 1));
    ASSERT_TRUE(f.addBounds(r2, 0, 0));

    pulse::options::setDisableSMT(true);
    EXPECT_FALSE(f.isUnsat());

    pulse::options::setDisableSMT(false);
    EXPECT_TRUE(f.isUnsat());
    // Repeated query hits QF_BV cache and stays consistently UNSAT.
    EXPECT_TRUE(f.isUnsat());
  }

  // 2. Single-use bitwise witness peeling: `x != 0 && (x & 4) != 0` is SAT,
  //    while `(x & 1) != 0 && (x & 1) == 0` is UNSAT.
  {
    pulse::options::setDisableSMT(false);
    PulseFormula sat_f;
    ASSERT_TRUE(sat_f.addNonNull(x));
    ASSERT_TRUE(sat_f.addBounds(mask4, 4, 4));
    ASSERT_TRUE(sat_f.addBitwiseAnd(r1, x, mask4));
    ASSERT_TRUE(sat_f.addNonNull(r1));
    EXPECT_FALSE(sat_f.isUnsat());

    PulseFormula unsat_f;
    ASSERT_TRUE(unsat_f.addBounds(mask1, 1, 1));
    ASSERT_TRUE(unsat_f.addBitwiseAnd(r1, x, mask1));
    ASSERT_TRUE(unsat_f.addBitwiseAnd(r2, x, mask1));
    ASSERT_TRUE(unsat_f.addNonNull(r1));
    ASSERT_TRUE(unsat_f.addNull(r2));
    EXPECT_TRUE(unsat_f.isUnsat());
  }

  // 3. Connected-component slicing in impliesComparison: proves linear
  //    entailment `x < y && y < z => x < z` while returning std::nullopt
  //    immediately for unrelated unconstrained variables `uninit_u < uninit_v`.
  {
    pulse::options::setDisableSMT(false);
    PulseFormula lf;
    LinearConstraint c1;
    c1.kind = ConstraintKind::Less;
    c1.terms = {{x, 1}, {y, -1}};
    LinearConstraint c2;
    c2.kind = ConstraintKind::Less;
    c2.terms = {{y, 1}, {z, -1}};
    ASSERT_TRUE(lf.addLinearConstraint(c1));
    ASSERT_TRUE(lf.addLinearConstraint(c2));

    auto entailed = lf.impliesComparison(x, std::nullopt, "<", z, std::nullopt);
    ASSERT_TRUE(entailed.has_value());
    EXPECT_TRUE(*entailed);

    auto unrelated =
        lf.impliesComparison(uninit_u, std::nullopt, "<", uninit_v, std::nullopt);
    EXPECT_FALSE(unrelated.has_value());
  }
}
