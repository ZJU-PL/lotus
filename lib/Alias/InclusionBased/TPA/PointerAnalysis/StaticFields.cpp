#include "Alias/InclusionBased/TPA/Context/AdaptiveContext.h"
#include "Alias/InclusionBased/TPA/Context/Context.h"
#include "Alias/InclusionBased/TPA/Context/KLimitContext.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/MemoryManager.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/Type/ArrayLayout.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/Type/PointerLayout.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/Type/TypeLayout.h"

// C++ won't guarantee the order of static variable initialization across
// compilation units This is bad for us since we rely on the fact that
// MemoryManager is initialized after Context and the two Layouts Therefore we
// need to put all the static variable definitions here in order to control
// their init order

namespace context {

std::unordered_set<Context> Context::ctxSet;
unsigned KLimitContext::defaultLimit = 0u;
std::unordered_set<ProgramPoint> AdaptiveContext::trackedCallsites;

} // namespace context

namespace tpa {

std::unordered_set<ArrayLayout> ArrayLayout::layoutSet;
const ArrayLayout *ArrayLayout::defaultLayout =
    ArrayLayout::getLayout(std::vector<ArrayTriple>());

std::unordered_set<PointerLayout> PointerLayout::layoutSet;
const PointerLayout *PointerLayout::emptyLayout =
    PointerLayout::getLayout(util::VectorSet<size_t>());
const PointerLayout *PointerLayout::singlePointerLayout =
    PointerLayout::getLayout({0});

std::unordered_set<TypeLayout> TypeLayout::typeSet;

const MemoryBlock MemoryManager::uBlock = MemoryBlock(
    AllocSite::getUniversalAllocSite(), TypeLayout::getByteArrayTypeLayout());
const MemoryBlock MemoryManager::nBlock = MemoryBlock(
    AllocSite::getNullAllocSite(), TypeLayout::getPointerTypeLayoutWithSize(0));
const MemoryObject MemoryManager::uObj = MemoryObject(&uBlock, 0, true, 0u);
const MemoryObject MemoryManager::nObj = MemoryObject(&nBlock, 0, false, 1u);

} // namespace tpa
