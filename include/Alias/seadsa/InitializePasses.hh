#ifndef __DSA_INITIALIZE_PASSES_HH_
#define __DSA_INITIALIZE_PASSES_HH_

#include "llvm/InitializePasses.h"

namespace llvm {
void initializeRemovePtrToIntPass(PassRegistry &);
void initializeAllocWrapInfoPass(PassRegistry &);
void initializeAllocSiteInfoPass(PassRegistry &);
void initializeDsaLibFuncInfoPass(PassRegistry &);
void initializeDsaAnalysisPass(PassRegistry &);
void initializeCompleteCallGraphPass(PassRegistry &);
void initializeDsaInfoPassPass(PassRegistry &);
void initializeShadowMemPassPass(PassRegistry &);
void initializeStripShadowMemPassPass(PassRegistry &);
void initializeSeaDsaAAWrapperPassPass(PassRegistry &);
} // namespace llvm

namespace seadsa {

void initializeAnalysisPasses(llvm::PassRegistry &Registry);

} // namespace seadsa

#endif
