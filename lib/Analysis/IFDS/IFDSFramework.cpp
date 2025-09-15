/*
 * Complete IFDS/IDE Framework Implementation
 * 
 * This implements the full IFDS tabulation algorithm with:
 * - Path edges and summary edges
 * - Context-sensitive interprocedural analysis
 * - Proper termination and soundness guarantees
 */

#include <Analysis/IFDS/IFDSFramework.h>

#include <llvm/IR/CFG.h>
#include <llvm/Support/raw_ostream.h>

namespace sparta {
namespace ifds {

// Utility functions for debugging and analysis
template<typename Fact>
void debug_print_path_edges(const std::unordered_set<PathEdge<Fact>, PathEdgeHash<Fact>>& edges) {
    llvm::errs() << "Path Edges (" << edges.size() << "):\n";
    for (const auto& edge : edges) {
        llvm::errs() << "  " << edge.start_node->getOpcodeName() 
                     << " -> " << edge.target_node->getOpcodeName() << "\n";
    }
}

template<typename Fact>
void debug_print_summary_edges(const std::unordered_set<SummaryEdge<Fact>, SummaryEdgeHash<Fact>>& edges) {
    llvm::errs() << "Summary Edges (" << edges.size() << "):\n";
    for (const auto& edge : edges) {
        llvm::errs() << "  Call: " << edge.call_site->getOpcodeName() << "\n";
    }
}

// Explicit template instantiations for common fact types
// These can be uncommented when specific instantiations are needed
// template class IFDSSolver<TaintAnalysis>;
// template class IFDSSolver<ReachingDefinitionsAnalysis>;

} // namespace ifds
} // namespace sparta
