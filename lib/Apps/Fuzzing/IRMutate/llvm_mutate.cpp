// Copyright (C) 2013 Eric Schulte
#include <stdio.h>
#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Constants.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

static cl::opt<unsigned>
Inst1("inst1", cl::init(0), cl::desc("first statement to mutate"));

static cl::opt<unsigned>
Inst2("inst2", cl::init(0), cl::desc("second statement to mutate"));

// Use the result of instruction I somewhere in the basic block in
// which it is defined.  Ideally in the immediately subsequent
// instruction.
void useResult(Instruction *I){
  // we don't care if already used, use it again!
  // if(!I->use_empty()){ errs()<<"already used!\n" };
  BasicBlock *B = I->getParent();
  BasicBlock::iterator Begin = BasicBlock::iterator(I); ++Begin;
  for (BasicBlock::iterator it = Begin, E = B->end(); it != E; ++it){
    Instruction *Inst = &*it;
    int counter = -1;
    for (User::op_iterator oi = I->op_begin(), e = I->op_end(); oi != e; ++oi){
      counter++;
      Value *v = oi->get();
      if (v->getType() == I->getType()){
        Inst->setOperand(counter, I);
        return; } } }
  errs()<<"could find no use for result\n";
}

// Find a value of Type T which can be used at Instruction I.  Search
// in this order.
// 1. values in Basic Block before I
// 2. arguments to the function containing I
// 3. global values
// 4. null of the correct type
// 5. return a 0 that the caller can stick where the sun don't shine
Value *findInstanceOfType(Instruction *I, Type *T){
  bool isPointer = I->getType()->isPointerTy();

  // local inside the Basic Block
  BasicBlock *B = I->getParent();
  for (BasicBlock::iterator prev = B->begin(); &*prev != I; ++prev){
    if((isPointer && prev->getType()->isPointerTy()) ||
       (prev->getType() == T)){
      errs()<<"found local replacement: "<< &*prev <<"\n";
      return cast<Value>(&*prev); } }

  // arguments to the function
  Function *F = B->getParent();
  for (Function::arg_iterator arg = F->arg_begin(), E = F->arg_end();
       arg != E; ++arg){
    if((isPointer && arg->getType()->isPointerTy()) ||
       (arg->getType() == T)){
      errs()<<"found arg replacement: "<<arg<<"\n";
      return cast<Value>(arg); } }

  // global values
  Module *M = F->getParent();
  for (Module::global_iterator g = M->global_begin(), E = M->global_end();
       g != E; ++g){
    if((isPointer && g->getType()->isPointerTy()) ||
       (g->getType() == T)){
      errs()<<"found global replacement: "<< &*g <<"\n";
      return cast<Value>(&*g); } }

  // TODO: types which could be replaced with sane default
  //       - result of comparisons
  //       - nulls or zeros for number types
  // pulled from getNullValue
  if (T->isIntegerTy() ||
      T->isFloatingPointTy() ||
      T->isPointerTy() ||
      T->isStructTy() ||
      T->isArrayTy() ||
      T->isVectorTy()) {
    return Constant::getNullValue(T);
  } else {
    return nullptr;
  }
}

// Replace the operands of Instruction I with in-scope values of the
// same type.  If the operands are already in scope, then retain them.
void replaceOperands(Instruction *I){
  // don't touch arguments of branch instructions
  if(isa<BranchInst>(I)) return;

  // loop through operands,
  int counter = -1;
  for (User::op_iterator oi = I->op_begin(), e = I->op_end(); oi != e; ++oi) {
    counter++;
    Value *v = oi->get();

    // don't touch global or constant values
    if (!isa<GlobalValue>(v) && !isa<Constant>(v)){

      // don't touch arguments to the current function
      Function *F = I->getParent()->getParent();
      bool isAnArgument = false;
      for (Function::arg_iterator arg = F->arg_begin(), E = F->arg_end();
           arg != E; ++arg) {
        if( arg == v ){ isAnArgument = true; break; } }

      if(!isAnArgument) {
        // Don't touch operands which are in scope
        BasicBlock *B = I->getParent();
        bool isInScope = false;
        for (BasicBlock::iterator it = B->begin();
             &*it != I; ++it)
          if(&*it == v) { isInScope = true; break; }

        if(!isInScope){
          // If we've made it this far we really do have to find a replacement
          Value *val = findInstanceOfType(I, v->getType());
          if(val != nullptr){
            errs() << "replacing argument: " << v << "\n";
            I->setOperand(counter, val); } } } } }
}

namespace {
  struct Ids : public ModulePass {
    static char ID;
    Ids() : ModulePass(ID) {}

    bool runOnModule(Module &M){
      count = 0;
      for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I)
        walkFunction(&*I);

      errs() << count << "\n";

      return false;
    }

    // We don't modify the program, so we preserve all analyses
    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.setPreservesAll(); }

  private:
    int unsigned count;
    void walkFunction(Function *F){
      for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I)
        count += 1; }
  };
}

namespace {
  struct List : public ModulePass {
    static char ID;
    List() : ModulePass(ID) {}

    bool runOnModule(Module &M){
      count = 0;
      for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I)
        walkFunction(&*I);
      return false;
    }

    // We don't modify the program, so we preserve all analyses
    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.setPreservesAll(); }

  private:
    int unsigned count;
    void walkFunction(Function *F){
      for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
        count += 1;
        errs() << count << "\t" << I->getType();
        for (User::op_iterator oi = I->op_begin(), e = I->op_end(); oi != e; ++oi) {
          Value *operandVal = oi->get();
          errs() << "\t" << operandVal->getType();
        }
        errs() << "\n"; } }
  };
}

namespace {
  struct Name : public ModulePass {
    static char ID;
    Name() : ModulePass(ID) {}

    bool runOnModule(Module &M){
      count = 0;
      for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I)
        walkFunction(&*I);
      return false;
    }

    // We don't modify the program, so we preserve all analyses
    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.setPreservesAll(); }

  private:
    int unsigned count;

    void walkFunction(Function *F){
      for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
        count += 1;

        if (!I->getType()->isVoidTy()){
          char buf[24];
          snprintf(buf, sizeof(buf), "inst.%d", count);
          I->setName(buf); } } }
  };
}

namespace {
  struct Trace : public ModulePass {
    static char ID;
    Trace() : ModulePass(ID) {}

    bool runOnModule(Module &M){
      count = 0;
      // Build function type: void (i32)
      FunctionType *TraceFnTy = FunctionType::get(
        Type::getVoidTy(M.getContext()),
        { Type::getInt32Ty(M.getContext()) },
        false);
      PutFn = M.getOrInsertFunction("llvm_mutate_trace", TraceFnTy);
      for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I)
        walkFunction(&*I);
      return true;
    }

  private:
    int unsigned count;
    FunctionCallee PutFn;
    CallInst *PutCall;

    void walkFunction(Function *F){
      for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
        Instruction *Inst = &*I;
        count += 1;
        // to avoid: PHI nodes not grouped at top of basic block!
        if(!isa<PHINode>(Inst)){
          // turn the count into an argument array of constant integer values
          Value *Args[1];
          Args[0] = ConstantInt::get(Type::getInt32Ty(F->getContext()), count);
          PutCall = CallInst::Create(PutFn, Args, "", Inst);
        }
      }
    }
  };
}

namespace {
  struct Cut : public ModulePass {
    static char ID;
    Cut() : ModulePass(ID) {}

    bool runOnModule(Module &M){
      count = 0;
      changed_p = false;
      for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I)
        if(walkFunction(&*I)) break;

      if(changed_p) errs() << "cut " << Inst1 << "\n";
      else          errs() << "cut failed\n";

      return changed_p; }

  private:
    int unsigned count;
    bool changed_p;
    bool walkFunction(Function *F){
      for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
        count += 1;
        if(count == Inst1){
          // TODO: once more thinking it would be good to glue basic
          //       blocks when we delete a terminating instruction
          Instruction *Inst = &*I;
          if(!I->use_empty()){
            Value *Val = findInstanceOfType(Inst,I->getType());
            if(Val != nullptr){
              I->replaceAllUsesWith(Val); } }
          I->eraseFromParent();
          changed_p = true;
          return true; } }
      return false; }

  };
}

namespace {
  struct Insert : public ModulePass {
    static char ID;
    Insert() : ModulePass(ID) {}

    bool runOnModule(Module &M){
      count = 0;
      changed_p = false;
      for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I)
        if(walkCollect(&*I)) break;
      count = 0;
      for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I)
        if(walkPlace(&*I)) break;

      if(changed_p) errs()<<"inserted "<<Inst2<<" before "<<Inst1<<"\n";
      else          errs()<<"insertion failed\n";

      return changed_p; }

  private:
    int unsigned count;
    bool result_ignorable_p;
    bool changed_p;
    Instruction *temp;

    bool walkCollect(Function *F){
      for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
        count += 1;
        if(count == Inst2) {
          // check if I generates a result which is used, if not then
          // it is probably run for side effects and we don't need to
          // wire the result of the copy of I to something later on
          result_ignorable_p = I->use_empty();
          temp = I->clone();
          if (!temp->getType()->isVoidTy())
            temp->setName(I->getName()+".insert");
          return true; } }
      return false; }

    bool walkPlace(Function *F){
      for (Function::iterator B = F->begin(), E = F->end(); B != E; ++B) {
        for (BasicBlock::iterator I = B->begin(), E = B->end(); I != E; ++I) {
          count += 1;
          if(count == Inst1){
            temp->insertBefore(&*I); // insert temp before I
            replaceOperands(temp); // wire incoming edges of CFG into temp
            if(!result_ignorable_p)
              useResult(temp); // wire outgoing results of temp into CFG
            changed_p = true;
            return true; } } }
      return false; }
  };
}

namespace {
  struct Replace : public ModulePass {
    static char ID; // pass identification
    Replace() : ModulePass(ID) {}

    bool runOnModule(Module &M){
      count = 0;
      changed_p = false;
      for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I)
        walkCollect(&*I);
      count = 0;
      for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I)
        if(walkPlace(&*I)) break;

      if(changed_p) errs() << "replaced " << Inst1 << " with " << Inst2 << "\n";
      else          errs() << "replace failed\n";

      return changed_p; }

  private:
    int unsigned count;
    bool changed_p;
    Instruction *temp;

    void walkCollect(Function *F){
      for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
        count += 1;
        if (count == Inst2) {
          temp = I->clone();
          if (!temp->getType()->isVoidTy()) {
            temp->setName(I->getName()+".replace1"); } } } }

    bool walkPlace(Function *F){
      for (Function::iterator B = F->begin(), E = F->end(); B != E; ++B) {
        for (BasicBlock::iterator I = B->begin(), E = B->end(); I != E; ++I) {
          count += 1;
          if(count == Inst1){
            ReplaceInstWithInst(I->getParent()->getInstList(), I, temp);
            replaceOperands(temp);
            if(changed_p) return true;
            changed_p = true; } } }
      return false; }
  };
}

namespace {
  struct Swap : public ModulePass {
    static char ID; // pass identification
    Swap() : ModulePass(ID) {}

    bool runOnModule(Module &M){
      count = 0;
      changed_p = false;
      for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I)
        walkCollect(&*I);

      count = 0;
      // confirm that the types match
      if(temp1->getType() != temp2->getType()){
        errs() << "type mismatch " <<
          temp1->getType() << " and " <<
          temp2->getType() << "\n";
        return changed_p; }

      for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I)
        if(walkPlace(&*I)) break;

      if(changed_p) errs() << "swapped " << Inst1 << " with " << Inst2 << "\n";
      else          errs() << "swap failed\n";

      return changed_p; }

  private:
    int unsigned count;
    bool changed_p;
    Instruction *temp1, *temp2;

    void walkCollect(Function *F){
      for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
        count += 1;
        if (count == Inst1) {
          temp1 = I->clone();
          if (!temp1->getType()->isVoidTy()) {
            temp1->setName(I->getName()+".swap1"); } }
        if (count == Inst2) {
          temp2 = I->clone();
          if (!temp2->getType()->isVoidTy()){
            temp2->setName(I->getName()+".swap2"); } } } }

    bool walkPlace(Function *F){
      for (Function::iterator B = F->begin(), E = F->end(); B != E; ++B) {
        for (BasicBlock::iterator I = B->begin(), E = B->end(); I != E; ++I) {
          count += 1;
          if(count == Inst2){
            ReplaceInstWithInst(I->getParent()->getInstList(), I, temp1);
            replaceOperands(temp1);
            if(changed_p) return true;
            changed_p = true; }
          if(count == Inst1){
            ReplaceInstWithInst(I->getParent()->getInstList(), I, temp2);
            replaceOperands(temp2);
            if(changed_p) return true;
            changed_p = true; } } }
      return false; }
  };
}

char Ids::ID = 0;
char List::ID = 0;
char Name::ID = 0;
char Trace::ID = 0;
char Cut::ID = 0;
char Insert::ID = 0;
char Replace::ID = 0;
char Swap::ID = 0;
static RegisterPass<Ids>     S("ids",     "print the number of instructions");
static RegisterPass<List>    T("list",    "list instruction's type and id");
static RegisterPass<Name>    U("name",    "name each instruction by its id");
static RegisterPass<Trace>   V("trace",   "instrument to print inst. trace");
static RegisterPass<Cut>     W("cut",     "cut instruction number inst1");
static RegisterPass<Insert>  X("insert",  "insert inst2 before inst1");
static RegisterPass<Replace> Y("replace", "replace inst1 with inst2");
static RegisterPass<Swap>    Z("swap",    "swap inst1 and inst2");