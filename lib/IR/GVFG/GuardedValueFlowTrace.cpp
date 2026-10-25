#include "IR/GVFG/GuardedValueFlowTrace.h"
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>

namespace lotus {
namespace gvfg {
using namespace llvm;



using namespace std;

namespace {
GuardedValueFlowGraph *graphFor(GuardedValueFlowGraphBuilderPass *builder,
                               Function *function) {
  return builder->hasGraphFor(*function) ? &builder->getGraph(*function) : nullptr;
}
} // namespace

GuardedValueFlowTrace::GuardedValueFlowTrace() {}

GuardedValueFlowTrace::~GuardedValueFlowTrace() {}

GuardedValueFlowTrace::GuardedValueFlowTrace(const std::vector<const GuardedValueFlowObject *> &T)
    : Trace<const GuardedValueFlowObject *>(T) {}

GuardedValueFlowTrace::GuardedValueFlowTrace(const GuardedValueFlowTrace &T)
    : Trace<const GuardedValueFlowObject *>(T), STys(T.STys),
      AdditionalConds(T.AdditionalConds) {}

GuardedValueFlowTrace::GuardedValueFlowTrace(const GuardedValueFlowObject *Obj) { push(Obj); }

GuardedValueFlowTrace::GuardedValueFlowTrace(const GuardedValueFlowObject *Obj1, const GuardedValueFlowObject *Obj2) {
  push(Obj1);
  push(Obj2);
}

GuardedValueFlowTrace::GuardedValueFlowTrace(const LLVMValueTrace *ValueTrace,
                   GuardedValueFlowGraphBuilderPass *GVFGs) {
  resetWithLLVMValueTrace(ValueTrace, GVFGs);
}

GuardedValueFlowTrace &GuardedValueFlowTrace::operator=(const GuardedValueFlowTrace &T) {
  if (this != &T) {
    STys = T.STys;
    AdditionalConds = T.AdditionalConds;
    Trace<const GuardedValueFlowObject *>::operator=(T);
  }
  return *this;
}

static void pushValuePair(GuardedValueFlowTrace *Trace, Value *Oprd, Value *UseSite,
                          GuardedValueFlowGraphBuilderPass *GVFGs) {
  if (UseSite) {
    if (Instruction *UseInst = dyn_cast<Instruction>(UseSite)) {
      Function *Func = UseInst->getParent()->getParent();
      GuardedValueFlowGraph *GVFG = graphFor(GVFGs, Func);
      if (GVFG) {
        if (isa_and_nonnull<BasicBlock>(Oprd)) {
          // Branch Choice

          // First, we add the control dep for the branch inst itself
          if (GuardedValueFlowNode *UseSiteNode = GVFG->findNode(UseSite)) {
            if (GuardedValueFlowRegionNode *Region = UseSiteNode->getRegion()) {
              Trace->addAdditionalCond(Region);
            }
          }

          // Second, we add the path choice
          if (BranchInst *BrInst = dyn_cast<BranchInst>(UseSite)) {
            if (BrInst->isConditional()) {
              Value *CondVal = BrInst->getCondition();
              unsigned int NumSucc = BrInst->getNumSuccessors();
              if (NumSucc == 2) {
                // True and False Branches
                bool Cond = true;
                bool Valid = true;

                if (Oprd == BrInst->getSuccessor(0)) {
                  Cond = true;
                } else if (Oprd == BrInst->getSuccessor(1)) {
                  Cond = false;
                } else {
                  Valid = false;
                }

                if (Valid) {
                  GuardedValueFlowNode *CondNode = GVFG->findNode(CondVal);
                  if (CondNode) {
                    GuardedValueFlowRegionNode *CondRegion =
                        GVFG->findUnitRegion(CondNode, Cond);
                    if (CondRegion) {
                      Trace->addAdditionalCond(CondRegion);
                    }
                  }
                }
              }
            }
          }
        } else {
          GuardedValueFlowNode *OprdNode =
              Oprd != nullptr ? GVFG->findNode(Oprd) : nullptr;
          GuardedValueFlowNode *UseSiteNode =
              UseSite != nullptr ? GVFG->findNode(UseSite) : nullptr;

          if (OprdNode) {
            if (Trace->get_length() > 0) {
              if (Trace->tail() != OprdNode) {
                Trace->push(OprdNode);
                Trace->setStepType(Trace->get_length() - 1,
                                   GuardedValueFlowTrace::STY_VALUE |
                                       GuardedValueFlowTrace::STY_USE_SITE);
              } else {
                Trace->setStepType(Trace->get_length() - 1,
                                   Trace->getStepType(Trace->get_length() - 1) |
                                       GuardedValueFlowTrace::STY_VALUE);
              }
            } else {
              Trace->push(OprdNode);
              Trace->setStepType(Trace->get_length() - 1,
                                 GuardedValueFlowTrace::STY_VALUE | GuardedValueFlowTrace::STY_USE_SITE);
            }
          }

          if (UseSiteNode) {
            if (Trace->get_length() > 0) {
              if (Trace->tail() != UseSiteNode) {
                Trace->push(UseSiteNode);
                Trace->setStepType(Trace->get_length() - 1,
                                   GuardedValueFlowTrace::STY_USE_SITE);
              } else {
                Trace->setStepType(Trace->get_length() - 1,
                                   Trace->getStepType(Trace->get_length() - 1) |
                                       GuardedValueFlowTrace::STY_USE_SITE);
              }
            } else {
              Trace->push(UseSiteNode);
              Trace->setStepType(Trace->get_length() - 1,
                                 GuardedValueFlowTrace::STY_USE_SITE);
            }
          }
        }
      }
    }
  }
}

void GuardedValueFlowTrace::resetWithLLVMValueTrace(const LLVMValueTrace *ValueTrace,
                                       GuardedValueFlowGraphBuilderPass *GVFGs) {

  clear();

  if (!ValueTrace)
    return;

  set_bug_type_importance(ValueTrace->get_bug_type_importance());
  set_constructive_confidence(ValueTrace->get_constructive_confidence());
  set_score(ValueTrace->get_score());
  set_trace_type(ValueTrace->get_trace_type());


  const std::pair<Value *, Value *> *FromStep = nullptr;
  const std::pair<Value *, Value *> *ToStep = nullptr;

  bool IsCallSiteValueFlow = false;

  for (auto &TraceItem : *ValueTrace) {
    IsCallSiteValueFlow = false;

    // Test CallSite Value Flow
    FromStep = ToStep;
    ToStep = &TraceItem;

    if (FromStep) {
      Value *FromOprd = FromStep->first;
      Value *FromUseSite = FromStep->second;
      Value *ToOprd = ToStep->first;
      Value *ToUseSite = ToStep->second;

      if (FromOprd && FromUseSite && ToOprd && ToUseSite) {
        // Caller -> Callee
        if (isa<CallInst>(FromUseSite) && isa<Argument>(ToOprd) &&
            isa<Function>(ToUseSite)) {
          CallInst *FromUseSiteInst = dyn_cast<CallInst>(FromUseSite);
          Function *FromFunc = FromUseSiteInst->getParent()->getParent();
          GuardedValueFlowGraph *FromGVFG = graphFor(GVFGs, FromFunc);
          Function *ToFunc = dyn_cast<Function>(ToUseSite);
          GuardedValueFlowGraph *ToGVFG = graphFor(GVFGs, ToFunc);

          if (FromGVFG && ToGVFG) {
            GuardedValueFlowNode *FromOprdNode = FromGVFG->findNode(FromOprd);
            GuardedValueFlowCallSite *FromUseSiteNode =
                FromGVFG->findSite<GuardedValueFlowCallSite>(FromUseSiteInst);
            GuardedValueFlowNode *ToOprdNode = ToGVFG->findNode(ToOprd);

            if (FromOprdNode && FromUseSiteNode && ToOprdNode) {
              if (get_length() > 0) {
                if (tail() != FromOprdNode) {
                  push(FromOprdNode);
                  setStepType(get_length() - 1, STY_VALUE | STY_USE_SITE);
                } else {
                  setStepType(get_length() - 1,
                              getStepType(get_length() - 1) | STY_VALUE);
                }
              } else {
                push(FromOprdNode);
                setStepType(get_length() - 1, STY_VALUE | STY_USE_SITE);
              }

              push(FromUseSiteNode);
              setStepType(get_length() - 1, STY_USE_SITE);
              push(ToOprdNode);
              setStepType(get_length() - 1, STY_VALUE | STY_USE_SITE);

              IsCallSiteValueFlow = true;
            }
          }
        }

        if (isa<ReturnInst>(FromUseSite) && isa<Function>(ToOprd) &&
            isa<CallInst>(ToUseSite)) {
          // Callee -> Caller
          ReturnInst *FromUseSiteInst = dyn_cast<ReturnInst>(FromUseSite);
          Function *FromFunc = FromUseSiteInst->getParent()->getParent();
          GuardedValueFlowGraph *FromGVFG = graphFor(GVFGs, FromFunc);
          CallInst *ToUseSiteInst = dyn_cast<CallInst>(ToUseSite);
          Function *ToFunc = ToUseSiteInst->getParent()->getParent();
          GuardedValueFlowGraph *ToGVFG = graphFor(GVFGs, ToFunc);

          if (FromGVFG && ToGVFG) {
            GuardedValueFlowNode *FromOprdNode = FromGVFG->findNode(FromOprd);
            GuardedValueFlowReturnSite *FromUseSiteNode =
                FromGVFG->findSite<GuardedValueFlowReturnSite>(FromUseSiteInst);
            GuardedValueFlowNode *ToUseSiteNode = ToGVFG->findNode(ToUseSite);

            if (FromOprdNode && FromUseSiteNode && ToUseSiteNode) {
              if (get_length() > 0) {
                if (tail() != FromOprdNode) {
                  push(FromOprdNode);
                  setStepType(get_length() - 1, STY_VALUE | STY_USE_SITE);
                } else {
                  setStepType(get_length() - 1,
                              getStepType(get_length() - 1) | STY_VALUE);
                }
              } else {
                push(FromOprdNode);
                setStepType(get_length() - 1, STY_VALUE | STY_USE_SITE);
              }

              push(FromUseSiteNode);
              setStepType(get_length() - 1, STY_USE_SITE);
              push(ToUseSiteNode);
              setStepType(get_length() - 1, STY_USE_SITE);
              IsCallSiteValueFlow = true;
            }
          }
        }
      }

      if (!IsCallSiteValueFlow) {
        pushValuePair(this, FromOprd, FromUseSite, GVFGs);
      }
    }
  }

  // Process Last Step
  if ((!IsCallSiteValueFlow) && ToStep != nullptr) {
    Value *ToOprd = ToStep->first;
    Value *ToUseSite = ToStep->second;
    pushValuePair(this, ToOprd, ToUseSite, GVFGs);
  }
}

int GuardedValueFlowTrace::getValidStartIdx() const {
  if (get_length() == 0) {
    return IDX_INVALID;
  }

  int Result = 0;

  switch (get_trace_type()) {
  case TraceType::SOURCE_SINK:
  case TraceType::SOURCE_NO_SINK:
  case TraceType::SIMPLE_VALUE_FLOW:
  case TraceType::NON_VALUE_FLOW:
  default:
    Result = 0;
  }

  return Result;
}

int GuardedValueFlowTrace::getKeyIdx() const {
  if (get_length() == 0) {
    return IDX_INVALID;
  }

  int Result = 0;

  switch (get_trace_type()) {
  case TraceType::SOURCE_SINK:
    Result = get_length() - 1;
    break;
  case TraceType::SOURCE_NO_SINK:
    Result = 0;
    break;
  case TraceType::SIMPLE_VALUE_FLOW:
  case TraceType::NON_VALUE_FLOW:
  default:
    Result = get_length() - 1;
  }

  return Result;
}

int GuardedValueFlowTrace::getLEVFSrcIdx(bool IsConsiderLoad) const {
  if (get_length() == 0) {
    return IDX_INVALID;
  }

  int Result = 0;

  switch (get_trace_type()) {
  case TraceType::SOURCE_SINK:
    Result = guessLEVFSrcIdx(IsConsiderLoad);
    break;
  case TraceType::SOURCE_NO_SINK:
    Result = get_length() - 1;
    break;
  case TraceType::SIMPLE_VALUE_FLOW:
    Result = guessLEVFSrcIdx(IsConsiderLoad);
    break;
  case TraceType::NON_VALUE_FLOW:
  default:
    Result = get_length() - 1;
  }

  return Result;
}

int GuardedValueFlowTrace::guessLEVFSrcIdx(bool IsConsiderLoad) const {
  if (get_length() == 0) {
    return IDX_INVALID;
  }

  int Result;
  for (Result = get_length() - 2; Result >= getValidStartIdx(); Result--) {
    const GuardedValueFlowObject *ResultNode = at(Result);
    if (!ResultNode) {
      continue;
    }

    Value *ResultVal = ResultNode->getDebugValue();
    if (isa_and_nonnull<Argument>(ResultVal)) {
      break;
    }

    Instruction *ResultInst = ResultNode->getDebugInstruction();
    if (!ResultInst) {
      continue;
    }

    if (isa<GEPOperator>(ResultInst) || isa<CastInst>(ResultInst)) {
      continue;
    }

    if ((!IsConsiderLoad) && Result > getValidStartIdx()) {
      const GuardedValueFlowObject *PreNode = at(Result - 1);
      if (PreNode && (isa<GuardedValueFlowNode>(PreNode) && cast<GuardedValueFlowNode>(PreNode)->getKind() == GuardedValueFlowNode::Kind::LoadMemory)) {
        continue;
      }
    }

    break;
  }

  if (Result < 0) {
    return 0;
  }

  return Result;
}

GuardedValueFlowTrace::StepType GuardedValueFlowTrace::getStepType(int StepIdx) const {
  assert(StepIdx >= 0 && StepIdx < get_length() && "Invalid trace idx");
  if (STys.size() <= StepIdx) {
    return DEFAULT_STEP_TYPE;
  }

  return STys[StepIdx];
}

// get the trace step type for the given StepIdx to STy
void GuardedValueFlowTrace::setStepType(int StepIdx, GuardedValueFlowTrace::StepType STy) {
  assert(StepIdx >= 0 && StepIdx < get_length() && "Invalid trace idx");

  if (STys.size() <= static_cast<size_t>(StepIdx))
    STys.resize(StepIdx + 1, DEFAULT_STEP_TYPE);
  STys[StepIdx] = STy;
}

int GuardedValueFlowTrace::getNumAdditionalConds() const { return AdditionalConds.size(); }

GuardedValueFlowRegionNode *GuardedValueFlowTrace::getAdditionalCond(int idx) const {
  assert(idx >= 0 && idx < getNumAdditionalConds() &&
         "Incorrect index for additional conditions");
  return AdditionalConds[idx];
}

void GuardedValueFlowTrace::addAdditionalCond(GuardedValueFlowRegionNode *cond) {
  AdditionalConds.push_back(cond);
}

void GuardedValueFlowTrace::clearAdditionalCond() { AdditionalConds.clear(); }

void GuardedValueFlowTrace::print(raw_ostream &O) {
  O << "GuardedValueFlowTrace:";
  for (const GuardedValueFlowObject *t : *this) {
    if (!t) {
      continue;
    }
    if (t->getDebugInstruction()) {
      O << "\t[" << *(t->getDebugInstruction()) << "]"
        << "==>\n";
    } else if (t->getDebugValue()) {
      O << "\t[" << *(t->getDebugValue()) << "]"
        << "==>\n";
    } else {
      O << "\t[" << "node" << "]"
        << "==>\n";
    }
  }
}
} // namespace gvfg
} // namespace lotus
