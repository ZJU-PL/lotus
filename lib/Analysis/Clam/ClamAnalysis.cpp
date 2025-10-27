#ifdef HAVE_CLAM

#include "Analysis/Clam/ClamAnalysis.h"
#include "clam/CfgBuilder.hh"
#include "clam/DummyHeapAbstraction.hh"
#include "clam/SeaDsaHeapAbstraction.hh"
#include "clam/CfgBuilderParams.hh"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/Instructions.h"

namespace lotus {
namespace clam {

ClamAnalysisWrapper::ClamAnalysisWrapper(llvm::Module &M, ::clam::CrabBuilderManager &man)
    : m_module(M), m_analyzed(false) {
  // Create CLAM analysis instance
  m_clam = std::make_unique<clam_analysis_t>(M, man);
}

void ClamAnalysisWrapper::analyze(::clam::AnalysisParams &params,
                                   const abs_dom_map_t &assumptions) {
  // Run the analysis
  m_clam->analyze(params, assumptions);
  m_analyzed = true;
}

llvm::Optional<::clam::clam_abstract_domain> 
ClamAnalysisWrapper::getPre(const llvm::BasicBlock *BB, bool keep_shadows) const {
  if (!m_clam || !m_analyzed || !BB) {
    return llvm::None;
  }
  
  return m_clam->getPre(BB, keep_shadows);
}

llvm::Optional<::clam::clam_abstract_domain> 
ClamAnalysisWrapper::getPost(const llvm::BasicBlock *BB, bool keep_shadows) const {
  if (!m_clam || !m_analyzed || !BB) {
    return llvm::None;
  }
  
  return m_clam->getPost(BB, keep_shadows);
}

bool ClamAnalysisWrapper::isDefinitelyNull(const llvm::BasicBlock *BB, 
                                            const llvm::Value *V) const {
  if (!BB || !V || !V->getType()->isPointerTy()) {
    return false;
  }
  
  auto pre = getPre(BB);
  if (!pre.hasValue()) {
    return false;
  }
  
  // Check if the value is definitely equal to null (0)
  // This would require accessing CRAB's internal representation
  // For now, return conservative result
  // TODO: Implement proper null check using CRAB domain operations
  return false;
}

bool ClamAnalysisWrapper::isDefinitelyNotNull(const llvm::BasicBlock *BB,
                                                const llvm::Value *V) const {
  if (!BB || !V || !V->getType()->isPointerTy()) {
    return false;
  }
  
  auto pre = getPre(BB);
  if (!pre.hasValue()) {
    return false;
  }
  
  // Check if the value is definitely not equal to null
  // This would require accessing CRAB's internal representation
  // TODO: Implement proper non-null check using CRAB domain operations
  return false;
}

const ClamAnalysisWrapper::checks_db_t &
ClamAnalysisWrapper::getChecksDB() const {
  return m_clam->getChecksDB();
}

void ClamAnalysisWrapper::printStats(llvm::raw_ostream &OS) const {
  if (!m_clam || !m_analyzed) {
    OS << "CLAM analysis not performed\n";
    return;
  }
  
  // Print basic statistics
  OS << "CLAM Analysis Statistics:\n";
  OS << "  Module: " << m_module.getName() << "\n";
  OS << "  Functions analyzed: " << m_module.getFunctionList().size() << "\n";
  // TODO: Add more detailed statistics from CLAM
}

::clam::AnalysisParams &ClamAnalysisWrapper::getDefaultParams() {
  static ::clam::AnalysisParams params;
  return params;
}

// Factory functions

std::unique_ptr<::clam::CrabBuilderManager>
createCrabBuilderManager(const ::clam::CrabBuilderParams &params,
                         llvm::TargetLibraryInfoWrapperPass &tli) {
  // Create with dummy heap abstraction (no pointer tracking)
  auto heap = std::make_unique<::clam::DummyHeapAbstraction>();
  return std::make_unique<::clam::CrabBuilderManager>(
      const_cast<::clam::CrabBuilderParams&>(params), tli, std::move(heap));
}

std::unique_ptr<ClamAnalysisWrapper> 
createClamAnalysis(llvm::Module &M, ::clam::CrabBuilderManager &man) {
  return std::make_unique<ClamAnalysisWrapper>(M, man);
}

} // namespace clam
} // namespace lotus

#endif // HAVE_CLAM
