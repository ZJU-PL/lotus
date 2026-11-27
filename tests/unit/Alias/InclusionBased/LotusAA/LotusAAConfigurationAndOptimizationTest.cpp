#include "LotusAATestSupport.h"

TEST(LotusAA, FullyAnalyzedCalleeDoesNotAddSummaryValueNoise) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_enable_score_computation = true;
  IntraLotusAAConfig::lotus_enable_summary_value = true;
  IntraLotusAAConfig::lotus_restrict_inline_depth = 2;

  const char *IR = R"(
    define void @setp(i8** %p, i8* %x) {
    entry:
      store i8* %x, i8** %p
      ret void
    }

    define void @wrapper(i8** %p, i8* %a, i8* %b) {
    entry:
      store i8* %a, i8** %p
      call void @setp(i8** %p, i8* %b)
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Wrapper = M->getFunction("wrapper");
  auto *PTG = Pass->getPtGraph(Wrapper);
  ASSERT_NE(PTG, nullptr);
  auto *Output = findOutputItem(PTG, &*Wrapper->arg_begin(), 0);
  ASSERT_NE(Output, nullptr);

  bool saw_summary = false;
  for (auto &ret_pair : Output->getVal()) {
    saw_summary |= containsSummaryValue(ret_pair.second);
  }

  EXPECT_FALSE(saw_summary);
}
TEST(LotusAA, OutputPseudoPointsToMergesDuplicateEntries) {
  const char *IR = R"(
    define i8* @main(i1 %cond, i8* %a) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    merge:
      %p = phi i8* [ %a, %then ], [ %a, %else ]
      ret i8* %p
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *PTG = Pass->getPtGraph(M->getFunction("main"));
  ASSERT_NE(PTG, nullptr);
  ASSERT_FALSE(PTG->outputs.empty());

  auto &pseudo_pts = PTG->outputs.front()->getPseudoPointTo();
  ASSERT_EQ(pseudo_pts.size(), 1u);
  EXPECT_TRUE(PTG->isSatisfiable(pseudo_pts.front().first));
  EXPECT_TRUE(PTG->isAlwaysSatisfied(pseudo_pts.front().first));
}
TEST(LotusAA, PseudoOutputNodesPreserveNonFirstClassTypes) {
  const char *IR = R"(
    define void @callee() {
    entry:
      ret void
    }

    define void @caller() {
    entry:
      call void @callee()
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Caller = M->getFunction("caller");
  Function *Callee = M->getFunction("callee");
  ASSERT_NE(Caller, nullptr);
  ASSERT_NE(Callee, nullptr);

  auto *PTG = Pass->getPtGraph(Caller);
  ASSERT_NE(PTG, nullptr);

  Instruction *SyntheticCallsite = Caller->getEntryBlock().getTerminator();
  ASSERT_NE(SyntheticCallsite, nullptr);

  auto *RetItem = new IntraLotusAA::OutputItem;
  RetItem->setType(Type::getVoidTy(Ctx));

  auto *AggregateItem = new IntraLotusAA::OutputItem;
  auto *StructTy = StructType::create(Ctx, "lotus.synthetic.output");
  StructTy->setBody({Type::getInt8PtrTy(Ctx), Type::getInt8PtrTy(Ctx)});
  AggregateItem->setType(StructTy);

  std::vector<IntraLotusAA::OutputItem *> Outputs = {RetItem, AggregateItem};
  std::vector<Value *> &PseudoValues =
      PTG->createPseudoOutputNodes(Outputs, SyntheticCallsite, Callee);

  ASSERT_EQ(PseudoValues.size(), 2u);
  EXPECT_EQ(PseudoValues[1]->getType(), StructTy);
}
TEST(LotusAA, RestrictInterStructureMergesRecursiveEscapes) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_restrict_inter_structure = 0;

  const char *IR = R"(
    %node = type { %node* }

    define %node* @main(i1 %cond, %node* %arg) {
    entry:
      %a = alloca %node
      %b = alloca %node
      %a.next = getelementptr inbounds %node, %node* %a, i32 0, i32 0
      %b.next = getelementptr inbounds %node, %node* %b, i32 0, i32 0
      store %node* %arg, %node** %a.next
      store %node* %arg, %node** %b.next
      br i1 %cond, label %then, label %else
    then:
      ret %node* %a
    else:
      ret %node* %b
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *PTG = Pass->getPtGraph(M->getFunction("main"));
  ASSERT_NE(PTG, nullptr);

  EXPECT_EQ(PTG->escape_objs.size(), 1u);
  EXPECT_GE(PTG->real_to_pseudo_map.size(), 2u);
}
TEST(LotusAA, OutputPseudoPointsToRespectsConfiguredLimit) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_restrict_output_pts = 1;

  const char *IR = R"(
    define i8* @main(i1 %cond, i8* %a, i8* %b) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    merge:
      %p = phi i8* [ %a, %then ], [ %b, %else ]
      ret i8* %p
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *PTG = Pass->getPtGraph(M->getFunction("main"));
  ASSERT_NE(PTG, nullptr);
  ASSERT_FALSE(PTG->outputs.empty());

  EXPECT_TRUE(PTG->outputs.front()->getPseudoPointTo().empty());
}
TEST(LotusAA, PtResultOptimizationKeepsConfiguredTruncationSticky) {
  LotusConfigScope ConfigScope;
  const char *IR = R"(
    define i8* @main(i1 %cond) {
    entry:
      %x = alloca i8
      %y = alloca i8
      %z = alloca i8
      %w = alloca i8
      br i1 %cond, label %left, label %right
    left:
      br i1 %cond, label %then, label %mid
    then:
      br label %merge
    mid:
      br label %merge
    right:
      br i1 %cond, label %else, label %last
    else:
      br label %merge
    last:
      br label %merge
    merge:
      %p = phi i8* [ %x, %then ], [ %y, %mid ], [ %z, %else ], [ %w, %last ]
      ret i8* %p
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *PTG = Pass->getPtGraph(M->getFunction("main"));
  ASSERT_NE(PTG, nullptr);

  Value *Phi = findValueByName(*M->getFunction("main"), "p");
  ASSERT_NE(Phi, nullptr);
  PTResult *PhiPts = PTG->findPTResult(Phi, false);
  ASSERT_NE(PhiPts, nullptr);

  PTResultIterator first_iter(PhiPts, PTG);
  ASSERT_EQ(first_iter.size(), 3);
  EXPECT_TRUE(PhiPts->derived_list.empty());

  PTResultIterator second_iter(PhiPts, PTG);
  EXPECT_EQ(second_iter.size(), 3);
}
TEST(LotusAA, RightValueTrackingRespectsConfiguredLimit) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_restrict_right_value_count = 1;

  const char *IR = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @bar(i32 %x) {
    entry:
      %inc = add i32 %x, 1
      ret i32 %inc
    }

    define i32 @main(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    merge:
      %fp = phi i32 (i32)* [ @foo, %then ], [ @bar, %else ]
      %res = call i32 %fp(i32 7)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *PTG = Pass->getPtGraph(M->getFunction("main"));
  ASSERT_NE(PTG, nullptr);

  Value *Fp = findValueByName(*M->getFunction("main"), "fp");
  ASSERT_NE(Fp, nullptr);

  mem_value_t values;
  PTG->trackPtrRightValue(Fp, values);
  PTG->refineResult(values);
  EXPECT_EQ(values.size(), 1u);
}
TEST(LotusAA, RightValueTrackingUsesLegacyOuterRefinementTiming) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_restrict_right_value_count = 2;

  const char *IR = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @bar(i32 %x) {
    entry:
      %inc = add i32 %x, 1
      ret i32 %inc
    }

    define i32 @main(i1 %outer, i1 %inner) {
    entry:
      br i1 %outer, label %left, label %right
    left:
      br i1 %inner, label %then, label %mid
    then:
      br label %merge
    mid:
      br label %merge
    right:
      br label %merge
    merge:
      %fp = phi i32 (i32)* [ @foo, %then ], [ @foo, %mid ], [ @bar, %right ]
      %res = call i32 %fp(i32 7)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *Main = M->getFunction("main");
  auto *Foo = M->getFunction("foo");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Foo, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  Value *Fp = findValueByName(*Main, "fp");
  ASSERT_NE(Fp, nullptr);

  mem_value_t values;
  PTG->trackPtrRightValue(Fp, values);
  PTG->refineResult(values);

  ASSERT_EQ(values.size(), 1u);
  EXPECT_EQ(values.front().val, Foo);
}
TEST(LotusAA, FixedCallGraphModeSeedsDirectCallTargetsPerCallsite) {
  const char *IR = R"(
    define void @callee() {
    entry:
      ret void
    }

    define void @caller() {
    entry:
      call void @callee()
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  std::unique_ptr<LotusAA> Pass(runLotusAA(*M, 2));
  auto *Caller = M->getFunction("caller");
  auto *Callee = M->getFunction("callee");
  ASSERT_NE(Caller, nullptr);
  ASSERT_NE(Callee, nullptr);

  CallBase *Call = findCallByCallee(*Caller, "callee");
  ASSERT_NE(Call, nullptr);

  CallTargetSet *Targets = Pass->getCallees(Caller, Call);
  ASSERT_NE(Targets, nullptr);
  ASSERT_EQ(Targets->size(), 1u);
  EXPECT_EQ(Targets->begin()->first, Callee);
}
TEST(LotusAA, DefaultModeUsesFixedCallGraphScheduler) {
  const char *IR = R"(
    define void @callee() {
    entry:
      ret void
    }

    define void @caller() {
    entry:
      call void @callee()
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  std::unique_ptr<LotusAA> Pass(runLotusAADefaultMode(*M));
  ASSERT_EQ(Pass->analysisWaves_.size(), 2u);
  ASSERT_EQ(Pass->parallelSingletonCounts_.size(), 2u);

  auto *Caller = M->getFunction("caller");
  ASSERT_NE(Caller, nullptr);
  CallBase *Call = findCallByCallee(*Caller, "callee");
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(Pass->getCallees(Caller, Call), nullptr);
}
TEST(LotusAA, FixedCallGraphModeBuildsParallelSingletonWaves) {
  const char *IR = R"(
    define void @leaf_a() {
    entry:
      ret void
    }

    define void @leaf_b() {
    entry:
      ret void
    }

    define void @caller_a() {
    entry:
      call void @leaf_a()
      ret void
    }

    define void @caller_b() {
    entry:
      call void @leaf_b()
      ret void
    }

    define void @main() {
    entry:
      call void @caller_a()
      call void @caller_b()
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  std::unique_ptr<LotusAA> Pass(runLotusAA(*M, 2));
  ASSERT_EQ(Pass->analysisWaves_.size(), 3u);
  ASSERT_EQ(Pass->parallelSingletonCounts_.size(), 3u);

  ASSERT_EQ(Pass->analysisWaves_[0].size(), 2u);
  EXPECT_EQ(collectWaveGroups(Pass->analysisWaves_[0]),
            (std::multiset<std::set<std::string>>{
                {"leaf_a"},
                {"leaf_b"},
            }));
  EXPECT_EQ(Pass->parallelSingletonCounts_[0], 2u);

  ASSERT_EQ(Pass->analysisWaves_[1].size(), 2u);
  EXPECT_EQ(collectWaveGroups(Pass->analysisWaves_[1]),
            (std::multiset<std::set<std::string>>{
                {"caller_a"},
                {"caller_b"},
            }));
  EXPECT_EQ(Pass->parallelSingletonCounts_[1], 2u);

  ASSERT_EQ(Pass->analysisWaves_[2].size(), 1u);
  EXPECT_EQ(collectFunctionNames(Pass->analysisWaves_[2][0]),
            std::set<std::string>({"main"}));
  EXPECT_EQ(Pass->parallelSingletonCounts_[2], 0u);
}
TEST(LotusAA, FixedCallGraphModeKeepsRecursiveSccSequential) {
  const char *IR = R"(
    define void @mut_a(i1 %cond) {
    entry:
      br i1 %cond, label %recurse, label %exit
    recurse:
      call void @mut_b(i1 false)
      br label %exit
    exit:
      ret void
    }

    define void @mut_b(i1 %cond) {
    entry:
      br i1 %cond, label %recurse, label %exit
    recurse:
      call void @mut_a(i1 false)
      br label %exit
    exit:
      ret void
    }

    define void @main() {
    entry:
      call void @mut_a(i1 true)
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  std::unique_ptr<LotusAA> Pass(runLotusAA(*M, 2));
  ASSERT_EQ(Pass->analysisWaves_.size(), 2u);
  ASSERT_EQ(Pass->parallelSingletonCounts_.size(), 2u);

  ASSERT_EQ(Pass->analysisWaves_[0].size(), 1u);
  EXPECT_EQ(collectFunctionNames(Pass->analysisWaves_[0][0]),
            std::set<std::string>({"mut_a", "mut_b"}));
  EXPECT_EQ(Pass->parallelSingletonCounts_[0], 0u);

  ASSERT_EQ(Pass->analysisWaves_[1].size(), 1u);
  EXPECT_EQ(collectFunctionNames(Pass->analysisWaves_[1][0]),
            std::set<std::string>({"main"}));
  EXPECT_EQ(Pass->parallelSingletonCounts_[1], 0u);
}
TEST(LotusAA, ReassigningSSAValueTriggersAssertion) {
  const char *IR = R"(
    define void @main() {
    entry:
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *PTG = Pass->getPtGraph(M->getFunction("main"));
  ASSERT_NE(PTG, nullptr);

  EXPECT_DEATH(
      {
        Argument *Synthetic = new Argument(PTGraph::DEFAULT_POINTER_TYPE);
        MemObject *Obj = PTG->newObject(Synthetic, MemObject::CONCRETE);
        PTG->addPointsTo(Synthetic, Obj, 0, PTG->getEmptyCond());
        PTG->addPointsTo(Synthetic, Obj, 0, PTG->getEmptyCond());
      },
      "Re-assigning value");
}

TEST(LotusAA, PathCondSmtTogglePreservesSoundnessOnDisjunctiveAig) {
  LotusConfigScope ConfigScope;
  EXPECT_FALSE(IntraLotusAAConfig::lotus_enable_pathcond_smt);

  PathCondBddManager Mgr;
  uint32_t a = Mgr.aigVar(Mgr.allocVar());
  uint32_t b = Mgr.aigVar(Mgr.allocVar());
  uint32_t c = Mgr.aigVar(Mgr.allocVar());

  // Direct contradiction caught by exact BCP regardless of SMT toggle:
  // (a & b) & !(a & c) & c is unsatisfiable by unit propagation.
  uint32_t ab = Mgr.aigAnd(a, b);
  uint32_t ac = Mgr.aigAnd(a, c);
  uint32_t bcp_unsat = Mgr.aigAnd(Mgr.aigAnd(ab, PathCondBddManager::aigNot(ac)), c);
  IntraLotusAAConfig::lotus_enable_pathcond_smt = false;
  EXPECT_FALSE(Mgr.isAigSatisfiable(bcp_unsat));

  // Unsatisfiable 2-SAT / resolution formula with no top-level unit clauses:
  // (a | b) & (!a | b) & (a | !b) & (!a | !b).
  uint32_t c1 = Mgr.aigOr(a, b);
  uint32_t c2 = Mgr.aigOr(PathCondBddManager::aigNot(a), b);
  uint32_t c3 = Mgr.aigOr(a, PathCondBddManager::aigNot(b));
  uint32_t c4 = Mgr.aigOr(PathCondBddManager::aigNot(a), PathCondBddManager::aigNot(b));
  uint32_t hard_unsat = Mgr.aigAnd(Mgr.aigAnd(c1, c2), Mgr.aigAnd(c3, c4));

  // With SMT disabled, inconclusive disjunctive cones over-approximate as SAT
  // (never dropping feasible alias/points-to edges) and do not pollute cache.
  IntraLotusAAConfig::lotus_enable_pathcond_smt = false;
  EXPECT_TRUE(Mgr.isAigSatisfiable(hard_unsat));

  // Enabling SMT solves the exact cone via incremental Z3 SAT.
  IntraLotusAAConfig::lotus_enable_pathcond_smt = true;
  EXPECT_FALSE(Mgr.isAigSatisfiable(hard_unsat));
}

TEST(LotusAA, NonPointerBitCastAndVectorGepDoNotCrash) {
  const char *IR = R"(
    define i64 @main(<2 x i32> %vec, float %f, <2 x i8*> %ptrs) {
    entry:
      %bc1 = bitcast <2 x i32> %vec to i64
      %bc2 = bitcast float %f to i32
      %gep = getelementptr i8, <2 x i8*> %ptrs, <2 x i64> <i64 1, i64 2>
      %ext = extractelement <2 x i8*> %gep, i32 0
      %pi = ptrtoint i8* %ext to i64
      %z = zext i32 %bc2 to i64
      %sum = add i64 %bc1, %z
      %res = add i64 %sum, %pi
      ret i64 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *PTG = Pass->getPtGraph(M->getFunction("main"));
  ASSERT_NE(PTG, nullptr);
}

TEST(LotusAA, MayAliasMatchesAliasConditionAndBlockReachesHandlesCycles) {
  const char *IR = R"(
    define i8* @main(i1 %cond) {
    entry:
      %a = alloca i8
      %b = alloca i8
      br i1 %cond, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    merge:
      %p = phi i8* [ %a, %then ], [ %b, %else ]
      br label %loop
    loop:
      br i1 %cond, label %loop, label %exit
    exit:
      ret i8* %p
    dead:
      ret i8* %a
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  Value *A = findValueByName(*Main, "a");
  Value *B = findValueByName(*Main, "b");
  Value *P = findValueByName(*Main, "p");
  ASSERT_NE(A, nullptr);
  ASSERT_NE(B, nullptr);
  ASSERT_NE(P, nullptr);

  EXPECT_EQ(PTG->mayAlias(A, B), PTG->isSatisfiable(PTG->getAliasCondition(A, B)));
  EXPECT_FALSE(PTG->mayAlias(A, B));
  EXPECT_EQ(PTG->mayAlias(P, A), PTG->isSatisfiable(PTG->getAliasCondition(P, A)));
  EXPECT_TRUE(PTG->mayAlias(P, A));
  EXPECT_EQ(PTG->mayAlias(P, B), PTG->isSatisfiable(PTG->getAliasCondition(P, B)));
  EXPECT_TRUE(PTG->mayAlias(P, B));

  BasicBlock *Entry = &Main->getEntryBlock();
  BasicBlock *Loop = nullptr;
  BasicBlock *Exit = nullptr;
  BasicBlock *Dead = nullptr;
  for (BasicBlock &BB : *Main) {
    if (BB.getName() == "loop")
      Loop = &BB;
    else if (BB.getName() == "exit")
      Exit = &BB;
    else if (BB.getName() == "dead")
      Dead = &BB;
  }
  ASSERT_NE(Loop, nullptr);
  ASSERT_NE(Exit, nullptr);
  ASSERT_NE(Dead, nullptr);

  EXPECT_TRUE(PTG->blockReaches(Entry, Exit));
  EXPECT_TRUE(PTG->blockReaches(Loop, Loop));
  EXPECT_FALSE(PTG->blockReaches(Exit, Entry));
  EXPECT_FALSE(PTG->blockReaches(Entry, Dead));
  EXPECT_NE(PTG->findOrCreateBBRegion(Entry, Loop), nullptr);
}
