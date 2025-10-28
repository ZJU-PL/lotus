/// @file GraphAnalysis.cpp
/// @brief Implementation of graph analysis algorithms for ICFG and CFG.
///
/// Provides back edge detection, shortest path computation, and reachability
/// analysis for both intraprocedural and interprocedural control flow graphs.

 #include <set>
 #include <map>
 #include <vector>
 #include <list>
 
 #include <llvm/IR/IRBuilder.h>
 #include <llvm/IR/Module.h>
 #include <llvm/IR/Dominators.h>
 #include <llvm/Analysis/CFG.h>
 #include <llvm/ADT/DepthFirstIterator.h>
 #include <llvm/ADT/PostOrderIterator.h>
 #include <llvm/ADT/STLExtras.h>
 #include <llvm/Analysis/LoopInfo.h>
 #include <llvm/IR/InstIterator.h>
 
 #include "IR/ICFG/GraphAnalysis.h"
 #include "Support/System.h"
 
 using namespace llvm;
 using namespace std;
 
 
void findFunctionBackedgesIntra(
        llvm::Function* func,
        std::set<BBEdgePair>& res)
{
    findBackedgesFromBasicBlock(&func->getEntryBlock(), res);
}

void findBackedgesFromBasicBlock(
        llvm::BasicBlock* sourceBB,
        std::set<BBEdgePair>& res)
 {
     auto BB = sourceBB;
     if (succ_empty(BB))
         return;
 
     set<BasicBlock*> Visited;
     vector<std::pair<BasicBlock*, succ_iterator>> VisitStack;
     set<BasicBlock*> InStack;
 
     Visited.insert(BB);
     VisitStack.emplace_back(BB, succ_begin(BB));
     InStack.insert(BB);
     do {
         std::pair<BasicBlock*, succ_iterator> &Top = VisitStack.back();
         BasicBlock *ParentBB = Top.first;
         succ_iterator &I = Top.second;
 
         bool FoundNew = false;
         while (I != succ_end(ParentBB)) {
             BB = *I++;
 
             if (Visited.insert(BB).second) {
                 FoundNew = true;
                 break;
             }
             // Successor is in VisitStack, it's a back edge.
             if (InStack.count(BB))
                 res.insert(std::make_pair(ParentBB, BB));
         }
 
         if (FoundNew) {
 
             // Go down one level if there is a unvisited successor.
             InStack.insert(BB);
             VisitStack.emplace_back(BB, succ_begin(BB));
         } else {
 
             // Go up one level.
             auto val = VisitStack.back();
             InStack.erase(val.first);
             VisitStack.pop_back();
         }
     } while (!VisitStack.empty());
 }
 
 void findFunctionBackedgesIntraICFG(
         ICFG* icfg,
         const llvm::Function* func,
         std::set<ICFGEdge*>& res)
 {
     assert(icfg->hasIntraBlockNode(&func->getEntryBlock()) && "ICFG not complete");
 
     ICFGNode* Node = icfg->getIntraBlockNode(&func->getEntryBlock());
 
     if (Node->getOutEdges().empty())
         return;
 
     set<ICFGNode*> Visited;
     vector<ICFGNode*> VisitStack;
     set<ICFGNode*> InStack;
 
     Visited.insert(Node);
     VisitStack.emplace_back(Node);
     InStack.insert(Node);
     do {
 
         ICFGNode* ParentNode = VisitStack.back();
         auto I = ParentNode->OutEdgeBegin();
 
         bool FoundNew = false;
         while (I != ParentNode->OutEdgeEnd()) {
 
             ICFGEdge* Edge = *I++;
 
             // skip edges other than intra block edge
             if (!Edge->isIntraCFGEdge())
                 continue;
 
             Node = Edge->getDstNode();
 
             if (Visited.insert(Node).second) {
                 FoundNew = true;
                 break;
             }
 
             // Successor is in VisitStack, it's a back edge.
             if (InStack.count(Node)) {
 
                 res.insert(Edge);
 //                outs() << "goto " << Node->getBasicBlock()->getName() << "\n";
 //                outs() << Edge->getSrcNode()->toString() << " -> " << Edge->getDstNode()->toString() << "\n";
             }
         }
 
         if (FoundNew) {
 
             // Go down one level if there is a unvisited successor.
             InStack.insert(Node);
             VisitStack.emplace_back(Node);
         } else {
 
             // Go up one level.
             auto val = VisitStack.back();
             InStack.erase(val);
             VisitStack.pop_back();
             assert(InStack.size() == VisitStack.size());
         }
     } while (!VisitStack.empty());
 }
 
 void findFunctionBackedgesInterICFG(
         ICFG* icfg,
         const llvm::Function* func,
         std::set<ICFGEdge*>& res)
 {
     assert(icfg->hasIntraBlockNode(&func->getEntryBlock()) && "ICFG not complete");
 
     ICFGNode* Node = icfg->getIntraBlockNode(&func->getEntryBlock());
 
     if (Node->getOutEdges().empty())
         return;
 
     set<ICFGNode*> Visited;
     vector<ICFGNode*> VisitStack;
     set<ICFGNode*> InStack;
 
     Visited.insert(Node);
     VisitStack.emplace_back(Node);
     InStack.insert(Node);
     do {
 
         ICFGNode* ParentNode = VisitStack.back();
         auto I = ParentNode->OutEdgeBegin();
 
         bool FoundNew = false;
         while (I != ParentNode->OutEdgeEnd()) {
 
             ICFGEdge* Edge = *I++;
 
             // skip return edge
             if (Edge->isRetCFGEdge())
                 continue;
 
             Node = Edge->getDstNode();
 
             if (Visited.insert(Node).second) {
                 FoundNew = true;
                 break;
             }
 
             // call the main function, it is a backedge
             if (Node->getBasicBlock() == &func->getEntryBlock()) {
 
                 res.insert(Edge);
             }
         }
 
         if (FoundNew) {
 
             // Go down one level if there is a unvisited successor.
             InStack.insert(Node);
             VisitStack.emplace_back(Node);
         } else {
 
             // Go up one level.
             auto val = VisitStack.back();
             InStack.erase(val);
             VisitStack.pop_back();
             assert(InStack.size() == VisitStack.size());
         }
     } while (!VisitStack.empty());
 }
 
std::map<llvm::BasicBlock*, uint64_t> calculateDistanceMapIntra(
        llvm::BasicBlock* sourceBB)
{
    std::set<BBEdgePair> backEdges;
    findFunctionBackedgesIntra(sourceBB->getParent(), backEdges);
    return calculateDistanceMapIntra(sourceBB, backEdges);
}

std::map<llvm::BasicBlock*, uint64_t> calculateDistanceMapIntra(
        llvm::BasicBlock* sourceBB,
        const std::set<BBEdgePair>& backEdges)
 {
     typedef std::pair<uint64_t, llvm::BasicBlock*> DisBBPair;
 
     Function* func = sourceBB->getParent();
 
     // initialize INF distance from source to other blocks
     std::map<llvm::BasicBlock*, uint64_t> distanceMap;
     for (auto& bb : *func) {
 
         distanceMap[&bb] = INF;
     }
 
     // Distance from source to itself is 0
     distanceMap[sourceBB] = 0;
 
     // set of pair indicates <distance, block>
     std::set<DisBBPair> distanceBlockSet;
     distanceBlockSet.insert(DisBBPair(0, sourceBB));
 
     while (!distanceBlockSet.empty()) {
 
         auto top = *distanceBlockSet.begin();
         distanceBlockSet.erase(distanceBlockSet.begin());
 
         auto currentSourceBB = top.second;
 
         for (auto succIt = succ_begin(currentSourceBB), e = succ_end(currentSourceBB); succIt != e; ++succIt) {
 
             auto *adjBB = *succIt;
             uint64_t distanceToAdj = 1;
 
            // we dijkstras on an acyclic CFG
            if (backEdges.find(BBEdgePair(currentSourceBB, adjBB)) != backEdges.end()) {
                continue;
            }
 
             // Edge relaxation
             if (distanceMap[adjBB] > distanceToAdj + distanceMap[currentSourceBB]) {
 
                 // If the distance to the adjacent node is not INF,
                 // means the pair <dist, block> is in the set
                 // Remove the pair before updating it in the set.
                 if (distanceMap[adjBB] != INF) {
                     distanceBlockSet.erase(distanceBlockSet.find(DisBBPair(distanceMap[adjBB], adjBB)));
                 }
                 distanceMap[adjBB] = distanceToAdj + distanceMap[currentSourceBB];
                 distanceBlockSet.insert(DisBBPair(distanceMap[adjBB], adjBB));
             }
         }
     }
 
     return distanceMap;
 }
 
 void calculateDistanceMapInterICFGWithDistanceMap(
         ICFG* icfg,
         ICFGNode* sourceBB,
         std::map<ICFGNode*, uint64_t>& distanceMap)
 {
     typedef std::pair<uint64_t, ICFGNode*> DisBBPair;
     typedef ICFGEdge Edge;
 
     // initialize INF distance from source to other blocks
     std::for_each(std::begin(distanceMap), std::end(distanceMap),
                   [](auto &p) { p.second = INF; });
 
     // Distance from source to itself is 0
     distanceMap[sourceBB] = 0;
 
     // set of pair indicates <distance, block>
     std::set<DisBBPair> distanceBlockSet;
     distanceBlockSet.insert(DisBBPair(0, sourceBB));
 
     while (!distanceBlockSet.empty()) {
 
         auto top = *distanceBlockSet.begin();
         distanceBlockSet.erase(distanceBlockSet.begin());
 
         auto currentSourceBB = top.second;
 
         for (auto I = currentSourceBB->OutEdgeBegin(); I != currentSourceBB->OutEdgeEnd(); I++) {
 
             ICFGEdge* edge = *I;
             ICFGNode* adjBB = edge->getDstNode();
 
             if (edge->isRetCFGEdge())
                 continue;
 
             uint64_t distanceToAdj = 1;
 
             // Edge relaxation
             if (distanceMap[adjBB] > distanceToAdj + distanceMap[currentSourceBB]) {
 
                 // If the distance to the adjacent node is not INF,
                 // means the pair <dist, block> is in the set
                 // Remove the pair before updating it in the set.
                 if (distanceMap[adjBB] != INF) {
                     distanceBlockSet.erase(distanceBlockSet.find(DisBBPair(distanceMap[adjBB], adjBB)));
                 }
                 distanceMap[adjBB] = distanceToAdj + distanceMap[currentSourceBB];
                 distanceBlockSet.insert(DisBBPair(distanceMap[adjBB], adjBB));
             }
         }
     }
 }
 
 std::map<ICFGNode*, uint64_t> calculateDistanceMapInterICFG(
         ICFG* icfg,
         ICFGNode* sourceBB)
 {
     typedef std::pair<uint64_t, ICFGNode*> DisBBPair;
     typedef ICFGEdge Edge;
 
     auto func = sourceBB->getFunction();
 
     // initialize INF distance from source to other blocks
     std::map<ICFGNode*, uint64_t> distanceMap;
     auto funcMap = icfg->getFunctionEntryMap();
 
     for (auto p : funcMap) {
 
         for (auto& bb : *p.first) {
 
             ICFGNode* node = icfg->getIntraBlockNode(&bb);
             distanceMap[node] = INF;
         }
     }
 
     // Distance from source to itself is 0
     distanceMap[sourceBB] = 0;
 
     // set of pair indicates <distance, block>
     std::set<DisBBPair> distanceBlockSet;
     distanceBlockSet.insert(DisBBPair(0, sourceBB));
 
     while (!distanceBlockSet.empty()) {
 
         auto top = *distanceBlockSet.begin();
         distanceBlockSet.erase(distanceBlockSet.begin());
 
         auto currentSourceBB = top.second;
 
         for (auto I = currentSourceBB->OutEdgeBegin(); I != currentSourceBB->OutEdgeEnd(); I++) {
 
             ICFGEdge* edge = *I;
             ICFGNode* adjBB = edge->getDstNode();
 
             if (edge->isRetCFGEdge())
                 continue;
 
             uint64_t distanceToAdj = 1;
 
             // Edge relaxation
             if (distanceMap[adjBB] > distanceToAdj + distanceMap[currentSourceBB]) {
 
                 // If the distance to the adjacent node is not INF,
                 // means the pair <dist, block> is in the set
                 // Remove the pair before updating it in the set.
                 if (distanceMap[adjBB] != INF) {
                     distanceBlockSet.erase(distanceBlockSet.find(DisBBPair(distanceMap[adjBB], adjBB)));
                 }
                 distanceMap[adjBB] = distanceToAdj + distanceMap[currentSourceBB];
                 distanceBlockSet.insert(DisBBPair(distanceMap[adjBB], adjBB));
             }
         }
     }
 
     return distanceMap;
 }
 
 // https://www.geeksforgeeks.org/shortest-path-unweighted-graph/
 bool calculateShortestPathIntra(
         llvm::BasicBlock* sourceBB,
         llvm::BasicBlock* destBB,
         std::vector<llvm::BasicBlock*>& path)
 {
     Function* func = sourceBB->getParent();
     path.clear();
 
     // predecessor[i] array stores predecessor of
     // i and distance array stores distance of i
     // from s
     std::map<llvm::BasicBlock*, llvm::BasicBlock*> pred;
     std::map<llvm::BasicBlock*, int> dist;
 
     // a queue to maintain queue of vertices whose
     // adjacency list is to be scanned as per normal
     // DFS algorithm
     list<llvm::BasicBlock*> queue;
 
     // boolean array visited[] which stores the
     // information whether ith vertex is reached
     // at least once in the Breadth first search
     std::map<llvm::BasicBlock*, bool> visited;
 
     // initially all vertices are unvisited
     // so v[i] for all i is false
     // and as no path is yet constructed
     // dist[i] for all i set to infinity
     for (auto& bb : *func) {
 
         visited[&bb] = false;
         dist[&bb] = INT_MAX;
         pred[&bb] = nullptr;
     }
 
     // now source is first to be visited and
     // distance from source to itself should be 0
     visited[sourceBB] = true;
     dist[sourceBB] = 0;
     queue.push_back(sourceBB);
 
     bool found = false;
     // standard BFS algorithm
     while (!queue.empty()) {
 
         auto currentBB = queue.front();
         queue.pop_front();
 
         for (auto succIt = succ_begin(currentBB), e = succ_end(currentBB); succIt != e; ++succIt) {
 
             auto *adjBB = *succIt;
             uint64_t distanceToAdj = 1;
 
             if (!visited[adjBB]) {
 
                 visited[adjBB] = true;
                 dist[adjBB] = dist[currentBB] + 1;
                 pred[adjBB] = currentBB;
                 queue.push_back(adjBB);
 
                 // We stop BFS when we find
                 // destination.
                 if (adjBB == destBB) {
 
                     found = true;
                     break;
                 }
             }
         }
 
         if (found)
             break;
     }
 
     if (!found)
         return false;
 
     // vector path stores the shortest path
     llvm::BasicBlock* crawl = destBB;
     path.push_back(crawl);
     while (pred[crawl]) {
 
         path.insert(path.begin(), pred[crawl]);
         crawl = pred[crawl];
     }
 
     return true;
 }
 
 // LoopInfo contains a mapping from basic block to the innermost loop. Find
 // the outermost loop in the loop nest that contains BB.
 static const Loop *getOutermostLoop(const LoopInfo *LI, const BasicBlock *BB) {
     const Loop *L = LI->getLoopFor(BB);
     if (L) {
         while (const Loop *Parent = L->getParentLoop())
             L = Parent;
     }
     return L;
 }
 
 // True if there is a loop which contains both BB1 and BB2.
 static bool loopContainsBoth(const LoopInfo *LI,
                              const BasicBlock *BB1, const BasicBlock *BB2) {
     const Loop *L1 = getOutermostLoop(LI, BB1);
     const Loop *L2 = getOutermostLoop(LI, BB2);
     return L1 != nullptr && L1 == L2;
 }
 
 /// whether from can reach to based on DominatorTree and LoopInfo
 bool isReachableFrom(llvm::BasicBlock* from, llvm::BasicBlock* to,
                      const llvm::DominatorTree* DT, const llvm::LoopInfo* LI,
                      int& iterCount)
 {
     SmallVector<BasicBlock*, 32> Worklist;
     Worklist.push_back(const_cast<BasicBlock*>(from));
 
     // When the stop block is unreachable, it's dominated from everywhere,
     // regardless of whether there's a path between the two blocks.
     if (DT && !DT->isReachableFromEntry(to))
         DT = nullptr;
 
     // Limit the number of blocks we visit.
     unsigned Limit = 64;
 
     // number of iterations
     iterCount = 0;
 
     SmallPtrSet<const BasicBlock*, 32> Visited;
     do {
 
         iterCount++;
         BasicBlock *BB = Worklist.pop_back_val();
         if (!Visited.insert(BB).second)
             continue;
         if (BB == to)
             return true;
         if (DT && DT->dominates(BB, to))
             return true;
         if (LI && loopContainsBoth(LI, BB, to))
             return true;
 
         if (!--Limit)
             break;
 
         if (const Loop *Outer = LI ? getOutermostLoop(LI, BB) : nullptr) {
             // All blocks in a single loop are reachable from all other blocks. From
             // any of these blocks, we can skip directly to the exits of the loop,
             // ignoring any other blocks inside the loop body.
             Outer->getExitBlocks(Worklist);
         } else {
             Worklist.append(succ_begin(BB), succ_end(BB));
         }
 
     } while (!Worklist.empty());
 
     // We haven't been able to prove it one way or the other, answer false
     return false;
 }
 
