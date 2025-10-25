//===- SSIfy.cpp - Transform programs to other program representations ----===//
//
//		This file is licensed under the General Public License v2.
//
//===----------------------------------------------------------------------===//
/*
 * 		SSIfy: a parameterized tool to convert programs for sparse analyses
 *		Copyright (C) 2013  Victor Hugo Sperle Campos
 */

 #include "IR/SSI/SSI.h"

 using namespace llvm;
 
 // Definition of static members of SSIfy
 const std::string SSIfy::phiname = "SSIfy_phi";
 const std::string SSIfy::signame = "SSIfy_sigma";
 const std::string SSIfy::copname = "SSIfy_copy";
 
 static cl::opt<bool> Verbose("v", cl::desc("Print details"));
 static cl::opt<std::string> ProgramPointOptions("set",
         cl::desc("Starting program points"), cl::Required);
 
bool SSIfy::runOnFunction(Function &F)
{
    this->F = &F;
    this->DTw = &getAnalysis<DominatorTreeWrapperPass>();
    this->DTmap = &(this->DTw->getDomTree());
    this->PDTmap = &getAnalysis<PostDominatorTreeWrapperPass>().getPostDomTree();
    this->DFmap = &getAnalysis<DominanceFrontierWrapperPass>().getDominanceFrontier();
    this->PDFmap = new PostDominanceFrontier(this->PDTmap);
 
     const std::string flags_str = std::string(ProgramPointOptions.c_str());
     for (int i = 0; i < 4; ++i) {
         this->flags[i] = flags_str[i] == '1';
     }
 
     if (Verbose) {
         errs() << "Running on function " << F.getName() << "\n";
     }
 
     // For every instruction in this function, call the SSIfy function
     Function::iterator Fit, Fend;
 
     for (Fit = F.begin(), Fend = F.end(); Fit != Fend; ++Fit) {
         BasicBlock& BB = *Fit;
 
         BasicBlock::iterator BBit, BBend;
         for (BBit = BB.begin(), BBend = BB.end(); BBit != BBend; ++BBit) {
             Instruction& I = *BBit;
 
             run(&I);
         }
     }
 
     clean();
 
     delete this->PDFmap;
 
     this->versions.clear();
 
     return true;
 }
 
 void SSIfy::run(Instruction* V)
 {
     std::set<ProgramPoint> Iup;
     std::set<ProgramPoint> Idown;
 
     // %condition = icmp i32 slt %V 0
     // br i1 %condition BB1 BB2
     // This example above explains this code section below
     // We have to check if a use of a use of V is a branch instruction to assess whether
     // it is a program point of Out(Conds) or not
     for (Value::user_iterator i = V->user_begin(), e = V->user_end(); i != e;
             ++i) {
         User *U = *i;
         Instruction* use_inst = dyn_cast<Instruction>(U);
 
         // Out(Conds)
         if (CmpInst* possible_cmp = dyn_cast<CmpInst>(use_inst)) {
             for (Value::user_iterator ii = possible_cmp->user_begin(), ee =
                     possible_cmp->user_end(); ii != ee; ++ii) {
                 User *Uu = *ii;
                 if (BranchInst* br_inst = dyn_cast<BranchInst>(Uu)) {
                     // (downwards)
                     if (flags[0]) {
                         Idown.insert(ProgramPoint(br_inst, ProgramPoint::Out));
                     }
 
                     // (upwards)
                     if (flags[1]) {
                         Iup.insert(ProgramPoint(br_inst, ProgramPoint::Out));
                     }
                 }
             }
         }
         // Uses
        //
        // EXCEPTIONS
        //  - Terminator instructions
        //  - PHINode
        //
        // These are exceptions because a copy created for them would
        // break the program, or not make sense.
        //
        else if (V->getType()->isIntegerTy()) {
            if (!use_inst->isTerminator() && !isa<PHINode>(use_inst)) {
                 // Uses (downwards)	FIXME: only with integer variables
                 if (flags[2]) {
                     Idown.insert(ProgramPoint(use_inst, ProgramPoint::Self));
                 }
 
                 // Uses (upwards)	FIXME: only with integer variables
                 if (flags[3]) {
                     Iup.insert(ProgramPoint(use_inst, ProgramPoint::Self));
                 }
             }
         }
     }
 
     split(V, Iup, Idown);
     rename_initial(V);
 }
 
 void SSIfy::split(Instruction* V, const std::set<ProgramPoint> &Iup,
         const std::set<ProgramPoint> &Idown)
 {
     std::set<ProgramPoint> Sup;
     std::set<ProgramPoint> Sdown;
 
     if (Verbose) {
         errs() << "Splitting " << V->getName() << "\n";
     }
 
     // Creation of the Sup set. Its logic is defined in the referenced paper.
     for (std::set<ProgramPoint>::iterator sit = Iup.begin(), send = Iup.end();
             sit != send; ++sit) {
         ProgramPoint point = *sit;
         Instruction* I = point.I;
         BasicBlock* BBparent = I->getParent();
 
         if (point.is_join()) {
             for (pred_iterator PI = pred_begin(BBparent), E = pred_end(
                     BBparent); PI != E; ++PI) {
                 BasicBlock *BBpred = *PI;
 
                 SmallPtrSet<BasicBlock*, 4> iterated_pdf = get_iterated_pdf(
                         BBpred);
 
                 for (SmallPtrSet<BasicBlock*, 4>::iterator sit =
                         iterated_pdf.begin(), send = iterated_pdf.end();
                         sit != send; ++sit) {
                     BasicBlock* BB = *sit;
                     Instruction& last = BB->back();
                     Sup.insert(ProgramPoint(&last, ProgramPoint::Out));
                 }
             }
         }
         else {
             SmallPtrSet<BasicBlock*, 4> iterated_pdf = get_iterated_pdf(
                     BBparent);
 
             for (SmallPtrSet<BasicBlock*, 4>::iterator sit =
                     iterated_pdf.begin(), send = iterated_pdf.end();
                     sit != send; ++sit) {
                 BasicBlock* BB = *sit;
                 Instruction& last = BB->back();
                 Sup.insert(ProgramPoint(&last, ProgramPoint::Out));
             }
         }
     }
 
     // Union of Sup, Idown and V
     std::set<ProgramPoint> NewSet;
     NewSet.insert(Sup.begin(), Sup.end());
     NewSet.insert(Idown.begin(), Idown.end());
 
     for (std::set<ProgramPoint>::iterator sit = NewSet.begin(), send =
     // Creation of Sdown. Logic defined in the paper as well.
             NewSet.end(); sit != send; ++sit) {
         ProgramPoint point = *sit;
         Instruction* I = point.I;
         BasicBlock* BBparent = I->getParent();
 
         if (point.is_branch()) {
             for (succ_iterator PI = succ_begin(BBparent), E = succ_end(
                     BBparent); PI != E; ++PI) {
                 BasicBlock *BBsucc = *PI;
 
                 SmallPtrSet<BasicBlock*, 4> iterated_df = get_iterated_df(
                         BBsucc);
 
                 for (SmallPtrSet<BasicBlock*, 4>::iterator sit =
                         iterated_df.begin(), send = iterated_df.end();
                         sit != send; ++sit) {
                     BasicBlock* BB = *sit;
                     Instruction& first = BB->front();
                     Sdown.insert(ProgramPoint(&first, ProgramPoint::In));
                 }
             }
         }
         else {
             SmallPtrSet<BasicBlock*, 4> iterated_df = get_iterated_df(BBparent);
 
             for (SmallPtrSet<BasicBlock*, 4>::iterator sit =
                     iterated_df.begin(), send = iterated_df.end(); sit != send;
                     ++sit) {
                 BasicBlock* BB = *sit;
                 Instruction& first = BB->front();
                 Sdown.insert(ProgramPoint(&first, ProgramPoint::In));
             }
         }
     }
 
     // Finally
     std::set<ProgramPoint> S;
     S.insert(Iup.begin(), Iup.end());
     S.insert(Idown.begin(), Idown.end());
     S.insert(Sup.begin(), Sup.end());
     S.insert(Sdown.begin(), Sdown.end());
 
     /*
      * 	Split live range of v by inserting sigma, phi, and copies
      */
     for (std::set<ProgramPoint>::iterator sit = S.begin(), send = S.end();
             sit != send; ++sit) {
         const ProgramPoint &point = *sit;
 
         if (point.not_definition_of(V)) {
             Instruction* insertion_point = point.I;
             ProgramPoint::Position relative_position = point.P;
 
             // Check if new variable is actually not necessary
             // NOTE: it only checks if it is NOT necessary. That doesn't
             // mean that it is necessary if the check returns false.
             // Removing this check makes this pass 10x slower.
             if (isNotNecessary(insertion_point, V)) {
                 continue;
             }
 
             if (point.is_join()) {
                 // phi
                 unsigned numReservedValues = std::distance(
                         pred_begin(insertion_point->getParent()),
                         pred_end(insertion_point->getParent()));
                 PHINode* new_phi = PHINode::Create(V->getType(),
                         numReservedValues, phiname);
 
                 // Add V multiple times as incoming value to the new phi
                 for (pred_iterator BBit = pred_begin(
                         insertion_point->getParent()), BBend = pred_end(
                         insertion_point->getParent()); BBit != BBend; ++BBit) {
                     BasicBlock* predBB = *BBit;
                     new_phi->addIncoming(V, predBB);
                 }
 
                 switch (relative_position) {
                     case ProgramPoint::In:
                         new_phi->insertBefore(insertion_point);
                         break;
                     default:
                         errs() << "Problem here1";
                         break;
                 }
 
                 if (Verbose) {
                     errs() << "Created " << new_phi->getName() << "\n";
                 }
 
                 this->versions[V].insert(new_phi);
                 ++NumPHIsCreated;
             }
             else if (point.is_branch()) {
                 // sigma
                 // Insert one sigma in each of the successors
                 BasicBlock* BBparent = point.I->getParent();
                 unsigned numReservedValues = 1;
 
                 for (succ_iterator PI = succ_begin(BBparent), E = succ_end(
                         BBparent); PI != E; ++PI) {
                     BasicBlock *BBsucc = *PI;
 
                     PHINode* new_sigma = PHINode::Create(V->getType(),
                             numReservedValues, signame, &BBsucc->front());
                     new_sigma->addIncoming(V, BBparent);
 
                     if (Verbose) {
                         errs() << "Created " << new_sigma->getName() << "\n";
                     }
 
                     this->versions[V].insert(new_sigma);
                     ++NumSigmasCreated;
                 }
             }
             else if (point.is_copy()) {
                 // copy
 
                 // Zero value
                 ConstantInt* zero = ConstantInt::get(
                         cast<IntegerType>(V->getType()), 0);
 
                 BinaryOperator* new_copy = BinaryOperator::Create(
                         Instruction::Add, V, zero, copname);
 
                 switch (relative_position) {
                     case ProgramPoint::Self:
                         new_copy->insertAfter(insertion_point);
                         break;
                     default:
                         errs() << "Problem here2";
                         break;
                 }
 
                 if (Verbose) {
                     errs() << "Created " << new_copy->getName() << "\n";
                 }
 
                 this->versions[V].insert(new_copy);
                 ++NumCopiesCreated;
             }
         }
     }
 }
 
 void SSIfy::rename_initial(Instruction* V)
 {
     RenamingStack stack(V);
 
     BasicBlock* root = V->getParent();
 
     rename(root, stack);
 }
 
 void SSIfy::rename(BasicBlock* BB, RenamingStack& stack)
 {
     const Value* V = stack.getValue();
 
     if (Verbose) {
         errs() << "Renaming " << V->getName() << " in " << BB->getName()
                 << "\n";
     }
 
     // Iterate over all instructions in BB
     for (BasicBlock::iterator iit = BB->begin(), iend = BB->end(); iit != iend;
             ++iit) {
         Instruction* I = cast<Instruction>(&*iit);
         PHINode* phi = dyn_cast<PHINode>(I);
 
         // foreach instruction u in n that uses v
         // We do this renaming only if it is not a SSI_phi
         // because renaming in SSI_phi is done in a step afterwards
         bool has_newdef = false;
 
         // Check if I has an use of V
         // If it does, then we mark I to be a new definition of V
         // then we call set_def on it later on
         for (User::op_iterator i = I->op_begin(), e = I->op_end(); i != e;
                 ++i) {
             Value *used = *i;
 
             if (used == V) {
                 if (!is_actual(I)) {
                     has_newdef = true;
                 }
 
                 if (!is_SSIphi(I)) {
                     set_use(stack, I);
                 }
 
                 break;
             }
         }
 
         // NEW DEFINITION OF V
         // sigma, phi or copy
         if (has_newdef) {
             if (phi) {
                 set_def(stack, phi);
             }
             // copy
             else if (is_SSIcopy(I)) {
                 set_def(stack, I);
             }
         }
     }
 
     // Searches for SSI_phis in the successors to rename uses of V in them
     for (succ_iterator sit = succ_begin(BB), send = succ_end(BB); sit != send;
             ++sit) {
         BasicBlock* BBsucc = *sit;
         for (BasicBlock::iterator BBit = BBsucc->begin(), BBend =
                 BBsucc->getFirstInsertionPt(); BBit != BBend; ++BBit) {
             PHINode* phi = dyn_cast<PHINode>(&*BBit);
 
             if (phi && is_SSIphi(phi)) {
                 set_use(stack, phi, BB);
             }
         }
     }
 
     // Now call recursively for all children in the dominance tree
     DomTreeNode* domtree = this->DTmap->getNode(BB);
     if (domtree) {
         for (DomTreeNode::iterator begin = domtree->begin(), end =
                 domtree->end(); begin != end; ++begin) {
             DomTreeNodeBase<BasicBlock> *DTN_children = *begin;
             BasicBlock *BB_children = DTN_children->getBlock();
             rename(BB_children, stack);
         }
     }
 }
 
 void SSIfy::set_use(RenamingStack& stack, Instruction* inst, BasicBlock* from)
 {
     Value* V = stack.getValue();
     Instruction* popped = 0;
 
     // If the stack is initially empty,
     // renaming didn't reach the initial
     // definition of V yet, so no point
     // in renaming yet
     if (stack.empty()) {
         return;
     }
 
     // If from != null, we are dealing with a renaming
     // inside a SSI_phi.
     if (!from) {
         while (!stack.empty()) {
             popped = stack.peek();
 
             if (!this->DTmap->dominates(popped, inst)) {
                 stack.pop();
 
                 if (Verbose) {
                     errs() << "set_use: Popping " << popped->getName()
                             << " from the stack of "
                             << stack.getValue()->getName() << "\n";
                 }
             }
             else {
                 break;
             }
         }
     }
     else {
         while (!stack.empty()) {
             popped = stack.peek();
 
             if ((popped->getParent() != from)
                     && (!this->DTmap->dominates(popped, from))) {
                 stack.pop();
 
                 if (Verbose) {
                     errs() << "set_usephi: Popping " << popped->getName()
                             << " from the stack of "
                             << stack.getValue()->getName() << "\n";
                 }
             }
             else {
                 break;
             }
         }
     }
 
     // If the stack has become empty, it means that the last valid
     // definition is actually V itself, not popped. Otherwise, popped
     // would still be in stack, therefore this wouldn't be empty.
     Instruction* new_name = stack.empty() ? cast<Instruction>(V) : popped;
 
     // We shouldn't perform renaming in any of the following cases
     if ((new_name != V) && (new_name != inst)) {
         if (!from) {
 
             if (Verbose) {
                 errs() << "set_use: Renaming uses of " << V->getName() << " in "
                         << inst->getName() << " to " << new_name->getName()
                         << "\n";
             }
 
             inst->replaceUsesOfWith(V, new_name);
         }
         else {
             PHINode* phi = cast<PHINode>(inst);
             int index = phi->getBasicBlockIndex(from);
 
             if (phi->getIncomingValue(index) == V) {
 
                 if (Verbose) {
                     errs() << "set_usephi: Renaming uses of " << V->getName()
                             << " in " << inst->getName() << " to "
                             << new_name->getName() << "\n";
                 }
 
                 phi->setIncomingValue(index, new_name);
             }
         }
     }
 }
 
 void SSIfy::set_def(RenamingStack& stack, Instruction* inst)
 {
     // Note that this function *doesn't* check if inst contains
     // an use of stack.Value!
     // Verification has to be done by the user of this function
 
     if (Verbose) {
         errs() << "set_def: Pushing " << inst->getName() << " to the stack of "
                 << stack.getValue()->getName() << "\n";
     }
 
     stack.push(inst);
 }
 
 void SSIfy::clean()
 {
     /*
      This structure saves all instructions that are marked to be erased.
      We cannot simply erase on sight because of cases like this:
      [V] -> {A B C D}
      [B] -> {...}
      If we visit V's set first and then erase B, the next iteration
      would try to access B, which would have been already erased.
      Thus, erases are performed afterwards.
      */
     SmallPtrSet<Instruction*, 16> to_be_erased;
 
     // This map associates instructions - that will be removed - to Values
     // to which their uses will be renamed.
     // In other words, this map is this->versions reversed, but containing
     // only instructions that will be erased for sure.
     DenseMap<Instruction*, Instruction*> maptooldvalues;
 
     // Please note that this next for is intended to identify what
     // instructions should be erased due to being either wrong or useless.
     // The actual remotion happens after.
     for (DenseMap<Value*, SmallPtrSet<Instruction*, 4> >::iterator mit =
             this->versions.begin(), mend = this->versions.end(); mit != mend;
             ++mit) {
 
         Instruction* V = cast<Instruction>(mit->first);
         SmallPtrSet<Instruction*, 4> created_vars = mit->second;
 
         for (SmallPtrSet<Instruction*, 4>::iterator sit = created_vars.begin(),
                 send = created_vars.end(); sit != send; ++sit) {
             Instruction* newvar = *sit;
 
             // The cleaning criteria for SSI_phi has two cases
             // First: phi whose incoming values are ALL V itself.
             // Second: phi that is not dominated by V.
             if (is_SSIphi(newvar)) {
                 PHINode* ssi_phi = cast<PHINode>(newvar);
                 bool any_value_diff_V = false;
 
                 // First case: phis with all incoming values corresponding to
                 // the original value.
                 for (unsigned i = 0, n = ssi_phi->getNumIncomingValues(); i < n;
                         ++i) {
                     const Value* incoming = ssi_phi->getIncomingValue(i);
 
                     if (incoming != V) {
                         any_value_diff_V = true;
                         break;
                     }
                 }
 
                 if (!any_value_diff_V) {
 
                     if (Verbose) {
                         errs() << "Erasing " << ssi_phi->getName() << "\n";
                     }
 
                     to_be_erased.insert(ssi_phi);
                     maptooldvalues[ssi_phi] = V;
 
                     continue;
                 }
 
                 // Second case
                 if (!this->DTmap->dominates(V, ssi_phi)) {
 
                     if (Verbose) {
                         errs() << "Erasing " << ssi_phi->getName() << "\n";
                     }
 
                     to_be_erased.insert(ssi_phi);
                     maptooldvalues[ssi_phi] = V;
 
                     continue;
                 }
 
                 if (ssi_phi->use_empty()) {
                     if (Verbose) {
                         errs() << "Erasing " << ssi_phi->getName() << "\n";
                     }
 
                     to_be_erased.insert(ssi_phi);
                     maptooldvalues[ssi_phi] = V;
 
                     continue;
                 }
             }
             // SSI_sigmas and SSI_copies have two cases for cleaning
             // First: they don't have any use
             // Second: they aren't dominated by V.
             else if (is_SSIsigma(newvar) || is_SSIcopy(newvar)) {
                 if (newvar->use_empty()) {
                     if (Verbose) {
                         errs() << "Erasing " << newvar->getName() << "\n";
                     }
                     to_be_erased.insert(newvar);
                 }
                 else if (!this->DTmap->dominates(V, newvar)) {
 
                     if (Verbose) {
                         errs() << "Erasing " << newvar->getName() << "\n";
                     }
 
                     to_be_erased.insert(newvar);
                     maptooldvalues[newvar] = V;
                 }
             }
             else {
                 errs() << "Problem here3\n";
             }
         }
     }
 
     // Create a topological sort of to be erased, based on this->versions
     // This way, we can remove instructions in a order that respects dependency
     SmallVector<Instruction*, 8> topsort = get_topsort_versions(to_be_erased);
 
     for (SmallVector<Instruction*, 8>::iterator sit = topsort.begin(), send =
             topsort.end(); sit != send; ++sit) {
         Instruction* I = *sit;
 
         DenseMap<Instruction*, Instruction*>::iterator it = maptooldvalues.find(
                 I);
 
         if (it != maptooldvalues.end()) {
             I->replaceAllUsesWith(it->second);
         }
 
         // STATISTICS
         if (is_SSIphi(I)) {
             ++NumPHIsDeleted;
         }
         else if (is_SSIsigma(I)) {
             ++NumSigmasDeleted;
         }
         else if (is_SSIcopy(I)) {
             ++NumCopiesDeleted;
         }
 
         I->eraseFromParent();
     }
 }
 
 /*void SSIfy::clean(Instruction* V) {
  std::set<Instruction*> defined;
  std::set<Instruction*> used;
  std::set<Instruction*> web = this->versions[V];
  std::set<Instruction*> active;
  bool atleastone = true;
 
  // CREATION OF THE DEFINED SET
 
  // initialize active with all actual instructions in the function that are a version of V
  for (std::set<Instruction*>::iterator web_it = web.begin(), web_end =
  web.end(); web_it != web_end; ++web_it) {
  Instruction* I = *web_it;
  if (is_actual(I)) {
  active.insert(I);
  }
  }
 
  while (atleastone) {
  atleastone = false;
 
  for (std::set<Instruction*>::iterator ait = active.begin(), aend =
  active.end(); ait != aend; ++ait) {
 
  Instruction* I = *ait;
 
  // IF I is already in the set Defined, we skip it
  if (defined.find(I) != defined.end()) {
  continue;
  }
 
  atleastone = true;
 
  for (Value::erator uit = I->use_begin(), uend = I->use_end();
  uit != uend; ++uit) {
  Instruction* use = cast<Instruction>(*uit);
 
  active.insert(use);
  }
 
  defined.insert(I);
  }
  }
 
  // CREATION OF THE USED SET
  active.clear();
 
  // Initialize active with all instructions that have any use which is in web
  for (std::set<Instruction*>::iterator wit = web.begin(), wend = web.end();
  wit != wend; ++wit) {
  Instruction* I = *wit;
 
  for (User::op_iterator oit = I->op_begin(), oend = I->op_end();
  oit != oend; ++oit) {
  Instruction* U = dyn_cast<Instruction>(*oit);
  if (U && is_actual(U)) {
  active.insert(U);
  }
  }
  }
 
  atleastone = true;
  while (atleastone) {
  atleastone = false;
 
  for (std::set<Instruction*>::iterator ait = active.begin(), aend =
  active.end(); ait != aend; ++ait) {
 
  Instruction* I = *ait;
 
  bool any_use_notin_used = false;
 
  for (Value::use_iterator uit = I->use_begin(), uend = I->use_end();
  uit != uend; ++uit) {
  Instruction* use = cast<Instruction>(*uit);
 
  // If not in used
  if (used.find(use) == used.end()) {
  any_use_notin_used = true;
  break;
  }
  }
 
  // If any use that isn't in used was found, we do not skip
  if (!any_use_notin_used) {
  continue;
  }
 
  atleastone = true;
 
  for (std::set<Instruction*>::iterator wit = web.begin(), wend =
  web.end(); wit != wend; ++wit) {
 
  Instruction* version = *wit;
 
  for (Value::use_iterator uit = I->use_begin(), uend =
  I->use_end(); uit != uend; ++uit) {
  Instruction* use = cast<Instruction>(*uit);
 
  if (version == use) {
  if (used.find(version) == used.end()) {
  active.insert(version);
  used.insert(version);
  }
  }
  }
  }
  }
  }
 
  // NOW, THE REMOVAL STEP
  std::set<Instruction*> live = set_intersection(defined, used);
 
  // For each non actual instruction 'version' in web
  for (std::set<Instruction*>::iterator wit = web.begin(), wend = web.end();
  wit != wend; ++wit) {
  Instruction* version = *wit;
 
  if (!is_actual(version)) {
  // For each v_i operand of 'version' so that v_i not in 'live'
  for (User::op_iterator oit = version->op_begin(), oend =
  version->op_end(); oit != oend; ++oit) {
  Instruction* operand = dyn_cast<Instruction>(*oit);
 
  if (operand && (web.find(operand) != web.end())) {
  // if v_i is not in live:
  // replace v_i with undef
  UndefValue* undef = UndefValue::get(operand->getType());
  version->replaceUsesOfWith(operand, undef);
  }
  }
 
  // if version == undef or all uses of version are undef
  // the latter basic means that the use set of version is empty
  if (version->use_empty()) {
  version->eraseFromParent();
  }
 
  }
  }
  }*/
 
 bool SSIfy::is_SSIphi(const Instruction* I)
 {
     return I->getName().startswith(phiname);
 }
 
 bool SSIfy::is_SSIsigma(const Instruction* I)
 {
     return I->getName().startswith(signame);
 }
 
 bool SSIfy::is_SSIcopy(const Instruction* I)
 {
     return I->getName().startswith(copname);
 }
 
 SmallPtrSet<BasicBlock*, 4> SSIfy::get_iterated_df(BasicBlock* BB) const
 {
     SmallPtrSet<BasicBlock*, 4> iterated_df;
 
     SmallVector<BasicBlock*, 4> stack;
     BasicBlock* current = BB;
 
     // Initialize the stack with the original BasicBlock
     // this stack is further populated with BasicBlocks
     // in the iterated DF of the original BB, until
     // this iterated DF ends.
     stack.push_back(current);
 
     while (!stack.empty()) {
         current = stack.back();
         stack.pop_back();
 
         const DominanceFrontier::DomSetType& frontier = this->DFmap->find(
                 current)->second;
 
         for (DominanceFrontier::DomSetType::iterator fit = frontier.begin(),
                 fend = frontier.end(); fit != fend; ++fit) {
             BasicBlock* BB_infrontier = *fit;
 
             // Only push to stack if this BasicBlock wasn't seen before
             // P.S.: insert returns a pair. The second refers to whether
             // the element was actually inserted or not.
             if ((iterated_df.insert(BB_infrontier)).second) {
                 stack.push_back(BB_infrontier);
             }
         }
     }
 
     return iterated_df;
 }
 
 SmallPtrSet<BasicBlock*, 4> SSIfy::get_iterated_pdf(BasicBlock* BB) const
 {
     SmallPtrSet<BasicBlock*, 4> iterated_pdf;
 
     SmallVector<BasicBlock*, 4> stack;
     BasicBlock* current = BB;
 
     // Initialize the stack with the original BasicBlock
     // this stack is further populated with BasicBlocks
     // in the iterated PDF of the original BB, until
     // this iterated PDF ends.
     stack.push_back(current);
 
     while (!stack.empty()) {
         current = stack.back();
         stack.pop_back();
 
         const PostDominanceFrontier::DomSetType& frontier = this->PDFmap->find(
                 current)->second;
 
         for (PostDominanceFrontier::DomSetType::iterator fit = frontier.begin(),
                 fend = frontier.end(); fit != fend; ++fit) {
             BasicBlock* BB_infrontier = *fit;
 
             // Only push to stack if this BasicBlock wasn't seen before
             // P.S.: insert returns a pair. The second refers to whether
             // the element was actually inserted or not.
             if ((iterated_pdf.insert(BB_infrontier)).second) {
                 stack.push_back(BB_infrontier);
             }
         }
     }
 
     return iterated_pdf;
 }
 
 bool SSIfy::is_actual(const Instruction* I)
 {
     if (is_SSIphi(I)) {
         return false;
     }
     if (is_SSIsigma(I)) {
         return false;
     }
     if (is_SSIcopy(I)) {
         return false;
     }
 
     return true;
 }
 
 SmallVector<Instruction*, 8> SSIfy::get_topsort_versions(
         const SmallPtrSet<Instruction*, 16>& to_be_erased) const
 {
     SmallVector<Instruction*, 8> topsort;
 
     // Create a graph of precedence from Versions' keys
     Graph g;
 
     // Add nodes
     for (SmallPtrSetIterator<Instruction*> sit = to_be_erased.begin(), send =
             to_be_erased.end(); sit != send; ++sit) {
         g.addNode(*sit);
     }
 
     // Add edges
     for (DenseMap<Value*, SmallPtrSet<Instruction*, 4> >::const_iterator mit =
             versions.begin(), mend = versions.end(); mit != mend; ++mit) {
         Value* V = mit->first;
         const SmallPtrSet<Instruction*, 4>& set = mit->second;
 
         if (!g.hasNode(V)) {
             continue;
         }
 
         for (SmallPtrSetIterator<Instruction*> sit = set.begin(), send =
                 set.end(); sit != send; ++sit) {
             g.addEdge(V, *sit);
         }
     }
 
     // Let's start, shall we
     SmallPtrSet<Value*, 8> unmarked_nodes(to_be_erased.begin(),
             to_be_erased.end());
 
     while (!unmarked_nodes.empty()) {
         SmallPtrSetIterator<Value*> sit = unmarked_nodes.begin();
         visit(g, unmarked_nodes, topsort, *sit);
     }
 
     // topsort now contains a topological sorting of nodes
     return topsort;
 }
 
 void SSIfy::visit(Graph& g, SmallPtrSet<Value*, 8>& unmarked_nodes,
         SmallVectorImpl<Instruction*>& list, Value* V) const
 {
     if (unmarked_nodes.count(V)) {
         const SmallPtrSet<Value*, 4>& adj_list = g.vertices[V];
 
         for (SmallPtrSetIterator<Value*> sit = adj_list.begin(), send =
                 adj_list.end(); sit != send; ++sit) {
             Value* m = *sit;
             visit(g, unmarked_nodes, list, m);
         }
 
         unmarked_nodes.erase(V);
 
         list.push_back(cast<Instruction>(V));
     }
 }
 
 bool SSIfy::isNotNecessary(const Instruction* insert_point,
         const Value* V) const
 {
     for (Value::const_user_iterator uit = V->user_begin(), uend = V->user_end();
             uit != uend; ++uit) {
         const User *U = *uit;
         const Instruction* use = cast<Instruction>(U);
 
         if (this->DTmap->dominates(insert_point, use)) {
             return false;
         }
     }
 
     return true;
 }
 
void SSIfy::getAnalysisUsage(AnalysisUsage &AU) const
{
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<PostDominatorTreeWrapperPass>();
    AU.addRequired<DominanceFrontierWrapperPass>();
}
 
 char SSIfy::ID = 0;
 static RegisterPass<SSIfy> X("ssify", "SSIfy pass");
 
 ProgramPoint::ProgramPoint(Instruction* I, Position P) :
         I(I), P(P)
 {
 }
 
 // Two ProgramPoints are equal iff they are of the same region type and:
 //     - if they are Self, their instruction should be the same.
 //     - if not, their instructions' parents should be the same.
 bool ProgramPoint::operator==(const ProgramPoint& o) const
 {
     if (this->P != o.P) {
         return false;
     }
 
     if (this->P == ProgramPoint::Self) {
         return this->I == o.I;
     }
 
     const BasicBlock* this_I_parent = this->I->getParent();
     const BasicBlock* o_I_parent = o.I->getParent();
 
     return this_I_parent == o_I_parent;
 }
 
 bool ProgramPoint::operator!=(const ProgramPoint& o) const
 {
     return !(*this == o);
 }
 
 bool ProgramPoint::operator<(const ProgramPoint& o) const
 {
     if (this->P < o.P) {
         return true;
     }
     if (this->P > o.P) {
         return false;
     }
 
     if (this->P == ProgramPoint::Self) {
         return this->I < o.I;
     }
 
     const BasicBlock* this_I_parent = this->I->getParent();
     const BasicBlock* o_I_parent = o.I->getParent();
 
     return this_I_parent < o_I_parent;
 }
 
 bool ProgramPoint::operator>(const ProgramPoint& o) const
 {
     return !(*this == o) && !(*this < o);
 }
 
 bool ProgramPoint::not_definition_of(const Value* V) const
 {
     const Instruction* I = this->I;
     const BasicBlock* BB = I->getParent();
 
     if (I == V)
         return false;
 
    switch (this->P) {
        case ProgramPoint::In:
            // phi case
            {
                const Instruction* FirstNonPHI = BB->getFirstNonPHI();
                for (BasicBlock::const_iterator BBit = BB->begin(), 
                     BBend = (FirstNonPHI ? FirstNonPHI->getIterator() : BB->end()); 
                     BBit != BBend; ++BBit) {

                    const PHINode* op = cast<PHINode>(&*BBit);

                    if (SSIfy::is_SSIphi(op)) {
                        unsigned n = op->getNumIncomingValues();
                        unsigned i;
                        for (i = 0; i < n; ++i) {
                            if (op->getIncomingValue(i) == V) {
                                return false;
                            }
                        }
                    }
                }
            }
            break;

        case ProgramPoint::Out:
            // sigma case
            for (const_succ_iterator BBsuccit = succ_begin(BB), BBsuccend =
                    succ_end(BB); BBsuccit != BBsuccend; ++BBsuccit) {
                const BasicBlock* BBsucc = *BBsuccit;

                const Instruction* FirstNonPHI = BBsucc->getFirstNonPHI();
                for (BasicBlock::const_iterator BBit = BBsucc->begin(), 
                     BBend = (FirstNonPHI ? FirstNonPHI->getIterator() : BBsucc->end()); 
                     BBit != BBend; ++BBit) {

                    const PHINode* op = cast<PHINode>(&*BBit);

                    if (SSIfy::is_SSIsigma(op)) {
                        unsigned n = op->getNumIncomingValues();
                        unsigned i;
                        for (i = 0; i < n; ++i) {
                            if (op->getIncomingValue(i) == V) {
                                return false;
                            }
                        }
                    }
                }
            }
            break;
 
        case ProgramPoint::Self:
            // copy case
            //
            // We walk through the instructions that follow I looking for
            // a created SSI_copy that redefines V already.
            // If it exists, then there's a definition already.
            for (BasicBlock::const_iterator bit = I->getIterator(); 
                 bit != BB->end() && SSIfy::is_SSIcopy(&*bit); ++bit) {

                const Instruction* next = &*bit;

                if (SSIfy::is_SSIcopy(next)) {
                    // Check if operand is V
                    if (next->getOperand(0) == V) {
                        return false;
                    }
                }
            }

            break;
     }
 
     return true;
 }
 
 const DominanceFrontier::DomSetType &
 PostDominanceFrontier::calculate(const PostDominatorTree &DT,
         const DomTreeNode *Node)
 {
 // Loop over CFG successors to calculate DFlocal[Node]
     BasicBlock *BB = Node->getBlock();
     DomSetType &S = Frontiers[BB]; // The new set to fill in...
     if (getRoots().empty())
         return S;
 
     if (BB)
         for (pred_iterator SI = pred_begin(BB), SE = pred_end(BB); SI != SE;
                 ++SI) {
             BasicBlock *P = *SI;
             // Does Node immediately dominate this predecessor?
             DomTreeNode *SINode = DT[P];
             if (SINode && SINode->getIDom() != Node)
                 S.insert(P);
         }
 
 // At this point, S is DFlocal.  Now we union in DFup's of our children...
 // Loop through and visit the nodes that Node immediately dominates (Node's
 // children in the IDomTree)
 //
     for (DomTreeNode::const_iterator NI = Node->begin(), NE = Node->end();
             NI != NE; ++NI) {
         DomTreeNode *IDominee = *NI;
         const DomSetType &ChildDF = calculate(DT, IDominee);
 
         DomSetType::const_iterator CDFI = ChildDF.begin(), CDFE = ChildDF.end();
         for (; CDFI != CDFE; ++CDFI) {
             if (!DT.properlyDominates(Node, DT[*CDFI]))
                 S.insert(*CDFI);
         }
     }
 
     return S;
 }
 
 //////////////////////////////////////////////////////////////////
 
 RenamingStack::RenamingStack(Value * V)
 {
     this->V = V;
 }
 
 Value * RenamingStack::getValue() const
 {
     return this->V;
 }
 
 void RenamingStack::push(Instruction* I)
 {
     this->stack.push_back(I);
 }
 
 void RenamingStack::pop()
 {
     this->stack.pop_back();
 }
 
 Instruction * RenamingStack::peek() const
 {
     return this->stack.back();
 }
 
 bool RenamingStack::empty() const
 {
     return this->stack.empty();
 }
 
 bool ProgramPoint::is_join() const
 {
     return !this->I->getParent()->getSinglePredecessor()
             && (this->P == ProgramPoint::In);
 }
 
 bool ProgramPoint::is_branch() const
 {
     return isa<BranchInst>(this->I) && (this->P == ProgramPoint::Out);
 }
 
 bool ProgramPoint::is_copy() const
 {
     return this->P == ProgramPoint::Self;
 }
 
 /*void GraphNode::addEdge(const GraphNode& to)
  {
  this->adjacency_list.insert(&to);
  }
 
  bool GraphNode::hasEdge(const GraphNode& to) const
  {
  return this->adjacency_list.count(&to);
  }*/
 
 void Graph::addNode(Value* V)
 {
     this->vertices[V];
 }
 
 bool Graph::hasNode(Value* V)
 {
     return this->vertices.count(V);
 }
 
 void Graph::addEdge(Value* from, Value* to)
 {
     DenseMap<Value*, SmallPtrSet<Value*, 4> >::iterator it =
             this->vertices.find(from);
 
     if (it != this->vertices.end()) {
         it->second.insert(to);
     }
 }
 
 bool Graph::hasEdge(Value* from, Value* to)
 {
     DenseMap<Value*, SmallPtrSet<Value*, 4> >::iterator it =
             this->vertices.find(from);
 
     if (it != this->vertices.end()) {
         return it->second.count(to);
     }
     else {
         return false;
     }
 }