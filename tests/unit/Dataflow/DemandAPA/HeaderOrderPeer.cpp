#include "Dataflow/DemandAPA/TreeDecomposition.h"
#include "Dataflow/DemandAPA/Tarjan.h"
#include "Dataflow/DemandAPA/Support.h"
#include "Dataflow/DemandAPA/SparseTable.h"
#include "Dataflow/DemandAPA/SameBagPreprocessing.h"
#include "Dataflow/DemandAPA/SPReader.h"
#include "Dataflow/DemandAPA/SCC.h"
#include "Dataflow/DemandAPA/RegEx.h"
#include "Dataflow/DemandAPA/Reader.h"
#include "Dataflow/DemandAPA/Project.h"
#include "Dataflow/DemandAPA/PrePreprocess.h"
#include "Dataflow/DemandAPA/Naive.h"
#include "Dataflow/DemandAPA/LCA.h"
#include "Dataflow/DemandAPA/Intraprocedural.h"
#include "Dataflow/DemandAPA/Interprocedural.h"
#include "Dataflow/DemandAPA/IfdsReader.h"
#include "Dataflow/DemandAPA/FunctionSummaries.h"
#include "Dataflow/DemandAPA/DemandOmp.h"
#include "Dataflow/DemandAPA/Comparison.h"
#include "Dataflow/DemandAPA/CentroidPreprocessing.h"
#include "Dataflow/DemandAPA/BpReader.h"
#include "Dataflow/DemandAPA/ApaInstance.h"
#include "Dataflow/DemandAPA/Algorithm.h"
#include "Dataflow/DemandAPA/Algebra.h"

Algorithm<int> &demandAPAAlgorithmFromOtherTranslationUnit() { return algSP; }
void *demandAPACacheFromOtherTranslationUnit() { return IfdsstarCache; }
bdd demandAPAIdentityFromOtherTranslationUnit() { return PAone; }
int demandAPAProjectFromOtherTranslationUnit(int call, int summary) {
  return SPproject(call, summary);
}

// Instantiating work must bring in every template implementation without any
// caller-managed sequence of implementation-header includes.
void demandAPAInstantiateWork(Algorithm<int> &algorithm,
                              ApaInstance<int> &instance,
                              RowInExcelSheet row) {
  algorithm.work(&instance, row, 1);
}
