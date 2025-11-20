#include "Dataflow/NPA/BitVectorSolver.h"
#include <sstream>

namespace npa {

// Helper to generate unique symbols for blocks
static std::string getBlockSymbol(const llvm::BasicBlock *BB, const char* suffix) {
    std::ostringstream oss;
    oss << (const void*)BB << "_" << suffix;
    return oss.str();
}

BitVectorSolver::Result BitVectorSolver::run(llvm::Function &F, 
                                             const BitVectorInfo &info, 
                                             SolverStrategy strategy,
                                             bool verbose) {
    // 1. Setup Domain
    // Note: This sets global state for BitVectorDomain. 
    // Ensure this is not run concurrently with different widths.
    BitVectorDomain::setBitWidth(info.getBitWidth());
    
    bool forward = info.isForward();
    llvm::APInt boundary = info.getBoundaryVal();

    using D   = BitVectorDomain;
    using Exp = Exp0<D>;
    using E   = E0<D>; // shared_ptr<Exp0<D>>
    
    std::vector<std::pair<Symbol, E>> eqns;
    
    // 2. Build Equations
    for (auto &BB : F) {
        // Symbols
        std::string inSym = getBlockSymbol(&BB, "IN");
        std::string outSym = getBlockSymbol(&BB, "OUT");
        
        llvm::APInt gen = info.getGen(&BB);
        llvm::APInt kill = info.getKill(&BB);
        llvm::APInt notKill = ~kill;
        
        if (forward) {
            // Forward Analysis
            // IN[B] = U OUT[P]
            E inExpr = nullptr;
            
            // Check if entry block
            bool isEntry = (&BB == &F.getEntryBlock());
            
            if (isEntry) {
                 inExpr = Exp::term(boundary);
            } else {
                bool hasPreds = false;
                for (auto *Pred : predecessors(&BB)) {
                    hasPreds = true;
                    std::string predOut = getBlockSymbol(Pred, "OUT");
                    auto pHole = Exp::hole(predOut);
                    if (!inExpr) inExpr = pHole;
                    else inExpr = Exp::ndet(inExpr, pHole);
                }
                if (!hasPreds) {
                    // Unreachable block or just no preds in CFG?
                    // Treat as empty (Zero)
                    inExpr = Exp::term(D::zero());
                }
            }
            eqns.emplace_back(inSym, inExpr);
            
            // OUT[B] = GEN[B] U (IN[B] - KILL[B])
            // OUT[B] = GEN[B] U (IN[B] & ~KILL[B])
            // seq(c, t) -> extend(c, t) -> c & t
            auto outBody = Exp::seq(notKill, Exp::hole(inSym));
            auto outExpr = Exp::ndet(Exp::term(gen), outBody);
            eqns.emplace_back(outSym, outExpr);

        } else {
            // Backward Analysis
            // OUT[B] = U IN[S]
             E outExpr = nullptr;
             
             bool hasSuccs = false;
             for (auto *Succ : successors(&BB)) {
                 hasSuccs = true;
                 std::string succIn = getBlockSymbol(Succ, "IN");
                 auto sHole = Exp::hole(succIn);
                 if (!outExpr) outExpr = sHole;
                 else outExpr = Exp::ndet(outExpr, sHole);
             }
             
             if (!hasSuccs) {
                 // Exit block
                 outExpr = Exp::term(boundary);
             }
             
             eqns.emplace_back(outSym, outExpr);
             
             // IN[B] = GEN[B] U (OUT[B] - KILL[B])
             auto inBody = Exp::seq(notKill, Exp::hole(outSym));
             auto inExpr = Exp::ndet(Exp::term(gen), inBody);
             eqns.emplace_back(inSym, inExpr);
        }
    }
    
    // 3. Solve
    std::pair<std::vector<std::pair<Symbol, D::value_type>>, Stat> rawRes;
    if (strategy == SolverStrategy::Newton) {
        rawRes = NewtonSolver<D>::solve(eqns, verbose);
    } else {
        rawRes = KleeneSolver<D>::solve(eqns, verbose);
    }
    
    
    // 4. Parse Result
    Result result;
    result.stats = rawRes.second;
    
    std::unordered_map<std::string, llvm::APInt> rawMap;
    for (auto &p : rawRes.first) {
        rawMap[p.first] = p.second;
    }
    
    for (auto &BB : F) {
        std::string inSym = getBlockSymbol(&BB, "IN");
        std::string outSym = getBlockSymbol(&BB, "OUT");
        
        // Use 0 (Empty) if symbol missing (e.g. unreachable blocks might not be solved if equations omitted? 
        // Actually we generated equations for all blocks, so they should be there.
        if (rawMap.count(inSym)) result.IN[&BB] = rawMap[inSym];
        else result.IN[&BB] = D::zero();
        
        if (rawMap.count(outSym)) result.OUT[&BB] = rawMap[outSym];
        else result.OUT[&BB] = D::zero();
    }
    
    return result;
}

} // namespace npa

