#include "Concurrency/Core/ConcurrencyContext.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Concurrency/CUDA/CUDAModel.h"
#include "Concurrency/Thread/ThreadMultiplicity.h"

#include <llvm/ADT/DenseSet.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;

namespace concurrency {

ConcurrencyContext::ConcurrencyContext(
    Module &module, lotus::AliasAnalysisWrapper *borrowed_aa)
    : m_module(module), m_thread_api(ThreadAPI::getThreadAPI()),
      m_alias_analysis(borrowed_aa) {}

ConcurrencyContext::~ConcurrencyContext() = default;

void ConcurrencyContext::setAliasAnalysis(lotus::AliasAnalysisWrapper *aa) {
  m_owned_alias_analysis.reset();
  m_alias_analysis = aa;
}

lotus::AliasAnalysisWrapper *ConcurrencyContext::getOrCreateAliasAnalysis() {
  if (!m_alias_analysis) {
    m_owned_alias_analysis = lotus::AliasAnalysisFactory::create(
        m_module, lotus::AAConfig::SparrowAA_NoCtx());
    m_alias_analysis = m_owned_alias_analysis.get();
  }
  return m_alias_analysis;
}

CallGraph &ConcurrencyContext::getCallGraph() {
  if (!m_call_graph) {
    m_call_graph = std::make_unique<CallGraph>(m_module);
  }
  return *m_call_graph;
}

const ModuleConcurrencySummary &ConcurrencyContext::getSummary() {
  if (!m_summary_built) {
    buildSummary();
  }
  return m_summary;
}

const DominatorTree &
ConcurrencyContext::getDominatorTree(const Function &func) {
  auto it = m_dom_trees.find(&func);
  if (it != m_dom_trees.end()) {
    return *it->second;
  }
  auto dt = std::make_unique<DominatorTree>(const_cast<Function &>(func));
  const DominatorTree &ref = *dt;
  m_dom_trees.emplace(&func, std::move(dt));
  return ref;
}

const PostDominatorTree &
ConcurrencyContext::getPostDominatorTree(const Function &func) {
  auto it = m_post_dom_trees.find(&func);
  if (it != m_post_dom_trees.end()) {
    return *it->second;
  }
  auto pdt = std::make_unique<PostDominatorTree>();
  pdt->recalculate(const_cast<Function &>(func));
  const PostDominatorTree &ref = *pdt;
  m_post_dom_trees.emplace(&func, std::move(pdt));
  return ref;
}

const LoopInfo &ConcurrencyContext::getLoopInfo(const Function &func) {
  auto it = m_loop_infos.find(&func);
  if (it != m_loop_infos.end()) {
    return *it->second;
  }
  const DominatorTree &dt = getDominatorTree(func);
  auto li = std::make_unique<LoopInfo>(dt);
  const LoopInfo &ref = *li;
  m_loop_infos.emplace(&func, std::move(li));
  return ref;
}

ThreadMultiplicityAnalysis &ConcurrencyContext::getThreadMultiplicity() {
  if (!m_thread_multiplicity) {
    m_thread_multiplicity =
        std::make_unique<ThreadMultiplicityAnalysis>(m_module, &getCallGraph());
  }
  return *m_thread_multiplicity;
}

void ConcurrencyContext::invalidate() {
  m_summary_built = false;
  m_summary = ModuleConcurrencySummary{};
  m_call_graph.reset();
  m_thread_multiplicity.reset();
  m_dom_trees.clear();
  m_post_dom_trees.clear();
  m_loop_infos.clear();
}

void ConcurrencyContext::buildSummary() {
  m_summary = ModuleConcurrencySummary{};
  m_summary_built = true;

  if (m_module.getNamedMetadata("nvvm.annotations") != nullptr ||
      StringRef(m_module.getTargetTriple()).contains("nvptx")) {
    m_summary.has_cuda = true;
  }

  for (const GlobalVariable &gv : m_module.globals()) {
    const unsigned as = gv.getAddressSpace();
    if (as == 1 || as == 3 || as == 4 || as == 101) {
      m_summary.has_cuda = true;
      break;
    }
    const StringRef name = gv.getName();
    if (name.contains("cuda") || name.contains("CUDA") ||
        name.contains("nvvm")) {
      m_summary.has_cuda = true;
      break;
    }
  }

  for (const Function &func : m_module) {
    const StringRef fn_name = func.getName();
    if (func.hasFnAttribute("nvvm.kernel") ||
        func.getCallingConv() == CallingConv::PTX_Kernel ||
        CUDAModel::isLegacyKernelConfiguration(fn_name) ||
        fn_name.contains("GraphLaunch") || fn_name.contains("cuda") ||
        fn_name.contains("CUDA") || fn_name.contains("nvvm") ||
        fn_name.contains("__device_stub__")) {
      m_summary.has_cuda = true;
    }
    if (m_thread_api) {
      const ThreadAPI::TD_TYPE fn_type = m_thread_api->getType(&func);
      if (fn_type != ThreadAPI::TD_DUMMY) {
        switch (m_thread_api->getRuntimeLibrary(&func)) {
        case RuntimeKind::OpenMP:
          m_summary.has_openmp = true;
          break;
        case RuntimeKind::MPI:
          m_summary.has_mpi = true;
          break;
        case RuntimeKind::CUDA:
          m_summary.has_cuda = true;
          break;
        case RuntimeKind::LinuxKernel:
          m_summary.has_linux_kernel = true;
          break;
        default:
          break;
        }
      }
    }

    if (func.isDeclaration()) {
      continue;
    }

    DenseSet<const Function *> seen_callees;
    for (const Instruction &inst : instructions(func)) {
      if (isa<FenceInst>(inst) || isa<AtomicRMWInst>(inst) ||
          isa<AtomicCmpXchgInst>(inst)) {
        m_summary.has_atomics = true;
        continue;
      }
      if (const auto *load = dyn_cast<LoadInst>(&inst)) {
        if (load->isAtomic()) {
          m_summary.has_atomics = true;
        }
        continue;
      }
      if (const auto *store = dyn_cast<StoreInst>(&inst)) {
        if (store->isAtomic()) {
          m_summary.has_atomics = true;
        }
        continue;
      }

      const auto *cb = dyn_cast<CallBase>(&inst);
      if (!cb) {
        continue;
      }

      if (m_thread_api) {
        const Function *callee = m_thread_api->getCallee(cb);
        if (callee && !callee->isDeclaration() &&
            seen_callees.insert(callee).second) {
          m_summary.direct_callees[&func].push_back(callee);
          m_summary.direct_callers[callee].push_back(&func);
        }

        const ThreadAPI::TD_TYPE type = m_thread_api->getType(cb);
        if (type != ThreadAPI::TD_DUMMY) {
          if (m_thread_api->isTDFork(cb)) {
            m_summary.has_forks = true;
            m_summary.fork_sites.push_back(cb);
          } else if (m_thread_api->isTDJoin(cb)) {
            m_summary.has_joins = true;
            m_summary.join_sites.push_back(cb);
          }
          if (m_thread_api->isTDAcquire(cb) || m_thread_api->isTDRelease(cb) ||
              m_thread_api->isReadLockAcquire(cb) ||
              m_thread_api->isWriteLockAcquire(cb) ||
              m_thread_api->isTryLock(cb)) {
            m_summary.has_locks = true;
          }
          if (m_thread_api->isTDCondWait(cb) ||
              m_thread_api->isTDCondSignal(cb) ||
              m_thread_api->isTDCondBroadcast(cb)) {
            m_summary.has_condvars = true;
          }
          if (m_thread_api->isTDBarWait(cb)) {
            m_summary.has_barriers = true;
          }
          switch (m_thread_api->getRuntimeLibrary(cb)) {
          case RuntimeKind::OpenMP:
            m_summary.has_openmp = true;
            break;
          case RuntimeKind::MPI:
            m_summary.has_mpi = true;
            break;
          case RuntimeKind::CUDA:
            m_summary.has_cuda = true;
            break;
          case RuntimeKind::LinuxKernel:
            m_summary.has_linux_kernel = true;
            break;
          default:
            break;
          }
        }
      }
    }
  }
}

} // namespace concurrency
