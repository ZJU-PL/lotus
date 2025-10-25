/*
Static Single Information (SSI) IR

SSI additionally guarantees that
- Every definition of a variable dominates all its uses (SSA property)
- Every use of a variable post-dominates all its reaching definitions

Static Single Information (SSI) form = SSA + σ-functions
1. Start from SSA form.
2. Compute the iterated post-dominance frontier (analogous to dominance frontier) to decide where σ’s are needed.
3. Insert σ-functions at each control-flow split whose successors are not in the same post-dom tree region.
4. Rename variables again to give unique names to σ results (mirrors SSA renaming).

 * 	Command-line options
 * 		-v: 		verbose mode
 * 		-set xxxx:	set what will be the initial points (x either 1 or 0)
 *			- 1st: exit of conditionals, downwards
 *			- 2nd: exit of conditionals, upwards
 *			- 3rd: uses, downwards
 *			- 4th: uses, upwards
 */

 #ifndef SSIFY_H_
 #define SSIFY_H_
 
 #include "llvm/ADT/Statistic.h"
 #include "llvm/ADT/SmallVector.h"
 #include "llvm/ADT/SmallPtrSet.h"
 #include "llvm/IR/Function.h"
 #include "llvm/IR/Instructions.h"
 #include "llvm/IR/Constants.h"
 #include "llvm/Support/raw_ostream.h"
 #include "llvm/IR/CFG.h"
 #include "llvm/IR/Dominators.h"
 #include "llvm/Analysis/PostDominators.h"
 #include "llvm/Analysis/DominanceFrontier.h"
 #include "llvm/IR/InstIterator.h"
 #include "llvm/Support/CommandLine.h"
 #include <set>
 #include <string>
 #include <algorithm>
 //#include <cstdlib>
 
 #define DEBUG_TYPE "SSIfy"
 
 namespace llvm {
 
 STATISTIC(NumPHIsCreated, "Number of SSIfy_phis created");
 STATISTIC(NumSigmasCreated, "Number of SSIfy_sigmas created");
 STATISTIC(NumCopiesCreated, "Number of SSIfy_copies created");
 STATISTIC(NumPHIsDeleted, "Number of SSIfy_phis deleted");
 STATISTIC(NumSigmasDeleted, "Number of SSIfy_sigmas deleted");
 STATISTIC(NumCopiesDeleted, "Number of SSIfy_copies deleted");
 
 // Forward declarations
 class ProgramPoint;
 class RenamingStack;
 class Graph;
 struct PostDominanceFrontier;
 
 // SSIfy
 struct SSIfy: public FunctionPass {
     static const std::string phiname;
     static const std::string signame;
     static const std::string copname;
 
     static char ID; // Pass identification, replacement for typeid
     const Function *F;
     const DominatorTreeWrapperPass *DTw;
     const DominatorTree *DTmap;
     PostDominatorTree *PDTmap;
     const DominanceFrontier *DFmap;
     const PostDominanceFrontier *PDFmap;
 
     // Command-line options for program points
     bool flags[4];
 
     // This map associates variables with the set of new variables
     // that have been created for them
     DenseMap<Value*, SmallPtrSet<Instruction*, 4> > versions;
 
     SSIfy() :
             FunctionPass(ID), F(0), DTw(0), DTmap(0), PDTmap(0), DFmap(0), PDFmap(
                     0)
     {
         memset(flags, 0, 4 * sizeof(bool));
     }
 
     virtual bool runOnFunction(Function &F);
 
     /*
      * 	Determines what is the splitting strategy for the variable V
      * 	and calls the SSIfy functions in order
      * 		- split
      * 		- rename
      */
     void run(Instruction* V);
 
     /*
      * 	Splits live range of the variable V according to the splitting strategy
      * 	defined as input.
      */
     void split(Instruction* V, const std::set<ProgramPoint> &Iup,
             const std::set<ProgramPoint> &Idown);
 
     bool isNotNecessary(const Instruction* insert_point, const Value* V) const;
 
     /*
      * 	Renaming function
      * 	Called after the creation of new variables (split function)
      */
     void rename_initial(Instruction* V);
 
     /*
      * 	Passes through all instructions in BB to update uses of the
      * 	variable V to its most recent definition, as well as registering
      * 	new definitions when it takes place
      */
     void rename(BasicBlock* BB, RenamingStack& stack);
 
     /*
      * 	Pushes into the stack a new definition of the variable V, that being the
      * 	instruction inst
      */
     void set_def(RenamingStack& stack, Instruction* inst);
 
     /*
      * 	Renames uses of the variable V in the instruction inst to its last definition according
      * 	to the stack of definitions. Note that it pops definitions from the stack until it finds
      * 	one that is correct, i.e., dominates the instruction inst.
      *
      * 	from stands for pointer to the predecessor block. It is used when renaming variables
      * 	inside a SSI_phi to tell which incoming value should be renamed.
      *
      */
     void set_use(RenamingStack& stack, Instruction* inst, BasicBlock* from = 0);
 
     /*
      * 	Look at this->versions, which contains all new variables created and what Value
      * 	were they created for, and determines which ones should be removed for not being
      * 	useful or simply wrong.
      */
     void clean();
 
     /*
      *	These functions check whether an instruction is of the custom types that
      *	we create in this pass.
      *	This check is performed by looking at its name.
      */
     static bool is_SSIphi(const Instruction* I);
     static bool is_SSIsigma(const Instruction* I);
     static bool is_SSIcopy(const Instruction* I);
 
     /*
      * 	Checks if I is an actual instruction.
      * 	Actual instruction is defined as not being created by us,
      * 	that is, sigma, artificial phi, and copy.
      * 	It uses the three functions just above.
      */
     static bool is_actual(const Instruction* I);
 
     // For a given BasicBlock, return its iterated dominance frontier as a set
     SmallPtrSet<BasicBlock*, 4> get_iterated_df(BasicBlock* BB) const;
 
     // For a given BasicBlock, return its iterated post-dominance frontier as a set
     SmallPtrSet<BasicBlock*, 4> get_iterated_pdf(BasicBlock* BB) const;
 
     /*
      * 	Creates a topological sorting of instructions in to_be_erased,
      * 	based on relations from this->versions.
      * 	That is, we sort to_be_erased in a way that, when we traverse it later,
      * 	we are able to eraseFromParent in a order that doesn't break.
      *
      * 	Algorithm from Cormen 2001 and Wikipedia.
      */
     SmallVector<Instruction*, 8> get_topsort_versions(
             const SmallPtrSet<Instruction*, 16>& to_be_erased) const;
     void visit(Graph& g, SmallPtrSet<Value*, 8>& unmarked_nodes,
             SmallVectorImpl<Instruction*>& list, Value* V) const;
 
     void getAnalysisUsage(AnalysisUsage &AU) const;
 };
 
 /*
  * 	A program point is a pair of one instruction and a region.
  *
  * 	Region can be: in (entry of block, join point, phi insertion)
  * 	               self (middle of block, parallel copy insertion)
  * 	               out (exit of block, branch point, sigma insertion)
  *
  * 	Instructions have different uses depending on the region associated
  * 	      in:   instruction is used only to determine what BasicBlock the
  * 	            program point refers to.
  * 	      self: instruction is the precise insertion point of the parallel
  * 	            copy. Thus, the future copy will be inserted just after the
  * 	            instruction here.
  * 	      out:  instruction is the branch instruction that is followed by the
  * 	            two outgoing edges.
  */
 class ProgramPoint {
 public:
     typedef enum {
         In, Self, Out
     } Position;
 
 public:
     explicit ProgramPoint(Instruction* I, Position P);
 
     /*
      * Checks if this program point doesn't have a definition of V already
      * We have three cases: sigma, phi, or copy
      * Each one has a different logic.
      */
     bool not_definition_of(const Value* V) const;
 
     /*
      * 	These are used to differentiate program points.
      * 	Since we store them in sets, these are useful
      * 	to implement the prevention of duplication, as well
      * 	as ordering.
      */
     bool operator==(const ProgramPoint& o) const;
     bool operator!=(const ProgramPoint& o) const;
     bool operator<(const ProgramPoint& o) const;
     bool operator>(const ProgramPoint& o) const;
 
     /*
      * 	Return the type of a program point
      */
     inline bool is_join() const;
     inline bool is_branch() const;
     inline bool is_copy() const;
 
     Instruction* I;
     Position P;
 };
 
 /*
  *  Wrapper class for the stack, in order to store the Value for which
  *  this stack exists in the first place
  */
 class RenamingStack {
 public:
     SmallVector<Instruction*, 4> stack;
     Value* V;
 
 public:
     RenamingStack(Value* V);
     inline Value* getValue() const;
     inline void push(Instruction* I);
     inline void pop();
     inline Instruction* peek() const;
     inline bool empty() const;
 };
 
 /*
  * 	Used to determine topological ordering in the clean function
  */
 class Graph {
 public:
     // Map from Values to adjacency lists
     DenseMap<Value*, SmallPtrSet<Value*, 4> > vertices;
 
     void addNode(Value* V);
     bool hasNode(Value* V);
 
     /*
      *  Add edge to graph.
      *  If from is not in the graph, we do not add it!
      */
     void addEdge(Value* from, Value* to);
 
     /*
      *  Return if an edge is present in the graph
      *  We do not create any new nodes.
      */
     bool hasEdge(Value* from, Value* to);
 };
 
 /// PostDominanceFrontier Class - Concrete subclass of DominanceFrontier that is
 /// used to compute the a post-dominance frontier.
 ///
 struct PostDominanceFrontier {
     static char ID;
 public:
     typedef std::set<BasicBlock*> DomSetType; // Dom set for a bb
     typedef std::map<BasicBlock*, DomSetType> DomSetMapType; // Dom set map
 
     DomSetMapType Frontiers;
     std::vector<BasicBlock*> Roots;
     const bool IsPostDominators;
 
     PostDominanceFrontier(PostDominatorTree* DTmap) :
             IsPostDominators(true)
     {
         calculate_frontiers(DTmap);
     }
 
     virtual ~PostDominanceFrontier()
     {
         releaseMemory();
     }
 
     /// getRoots - Return the root blocks of the current CFG.  This may include
     /// multiple blocks if we are computing post dominators.  For forward
     /// dominators, this will always be a single block (the entry node).
     ///
     inline const std::vector<BasicBlock*> &getRoots() const
     {
         return Roots;
     }
 
     /// isPostDominator - Returns true if analysis based of postdoms
     ///
     bool isPostDominator() const
     {
         return IsPostDominators;
     }
 
     virtual void releaseMemory()
     {
         Frontiers.clear();
     }
 
     // Accessor interface:
     typedef DomSetMapType::iterator iterator;
     typedef DomSetMapType::const_iterator const_iterator;
     iterator begin()
     {
         return Frontiers.begin();
     }
     const_iterator begin() const
     {
         return Frontiers.begin();
     }
     iterator end()
     {
         return Frontiers.end();
     }
     const_iterator end() const
     {
         return Frontiers.end();
     }
     iterator find(BasicBlock *B)
     {
         return Frontiers.find(B);
     }
     const_iterator find(BasicBlock *B) const
     {
         return Frontiers.find(B);
     }
 
     iterator addBasicBlock(BasicBlock *BB, const DomSetType &frontier)
     {
         assert(find(BB) == end() && "Block already in DominanceFrontier!");
         return Frontiers.insert(std::make_pair(BB, frontier)).first;
     }
 
     /// removeBlock - Remove basic block BB's frontier.
     void removeBlock(BasicBlock *BB)
     {
         assert(find(BB) != end() && "Block is not in DominanceFrontier!");
         for (iterator I = begin(), E = end(); I != E; ++I)
             I->second.erase(BB);
         Frontiers.erase(BB);
     }
 
     void addToFrontier(iterator I, BasicBlock *Node)
     {
         assert(I != end() && "BB is not in DominanceFrontier!");
         I->second.insert(Node);
     }
 
     void removeFromFrontier(iterator I, BasicBlock *Node)
     {
         assert(I != end() && "BB is not in DominanceFrontier!");
         assert(
                 I->second.count(Node)
                         && "Node is not in DominanceFrontier of BB");
         I->second.erase(Node);
     }
 
     /// compareDomSet - Return false if two domsets match. Otherwise
     /// return true;
     bool compareDomSet(DomSetType &DS1, const DomSetType &DS2) const
     {
         std::set<BasicBlock *> tmpSet;
         for (DomSetType::const_iterator I = DS2.begin(), E = DS2.end(); I != E;
                 ++I)
             tmpSet.insert(*I);
 
         for (DomSetType::const_iterator I = DS1.begin(), E = DS1.end(); I != E;
                 ) {
             BasicBlock *Node = *I++;
 
             if (tmpSet.erase(Node) == 0)
                 // Node is in DS1 but not in DS2.
                 return true;
         }
 
         if (!tmpSet.empty())
             // There are nodes that are in DS2 but not in DS1.
             return true;
 
         // DS1 and DS2 matches.
         return false;
     }
 
     /// compare - Return true if the other dominance frontier base matches
     /// this dominance frontier base. Otherwise return false.
     bool compare(DominanceFrontier &Other) const
     {
         DomSetMapType tmpFrontiers;
         for (DomSetMapType::const_iterator I = Other.begin(), E = Other.end();
                 I != E; ++I)
             tmpFrontiers.insert(std::make_pair(I->first, I->second));
 
         for (DomSetMapType::iterator I = tmpFrontiers.begin(), E =
                 tmpFrontiers.end(); I != E;) {
             BasicBlock *Node = I->first;
             const_iterator DFI = find(Node);
             if (DFI == end())
                 return true;
 
             if (compareDomSet(I->second, DFI->second))
                 return true;
 
             ++I;
             tmpFrontiers.erase(Node);
         }
 
         if (!tmpFrontiers.empty())
             return true;
 
         return false;
     }
 
    bool calculate_frontiers(PostDominatorTree* DTmap)
    {
        Frontiers.clear();
        // In LLVM 14+, PostDominatorTree doesn't have getRoots()
        // Compute roots manually from the function
        Roots.clear();
        if (const DomTreeNode *Root = DTmap->getRootNode()) {
            // For post-dominator tree, roots are exit blocks
            // We can get them by checking all blocks without successors
            calculate(*DTmap, Root);
        }
        return false;
    }
 
 private:
     const DomSetType &calculate(const PostDominatorTree &DT,
             const DomTreeNode *Node);
 };
 
 }
 
 #endif /* SSIFY_H_ */