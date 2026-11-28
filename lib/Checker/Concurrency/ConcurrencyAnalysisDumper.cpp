/**
 * @file ConcurrencyAnalysisDumper.cpp
 * @brief Implementation of concurrency analysis result dumping
 */

#include "Checker/Concurrency/ConcurrencyAnalysisDumper.h"

#include "Analysis/DebugInfo/DebugInfoAnalysis.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <vector>

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;
using namespace lotus;

namespace concurrency {

namespace {

// Collect MHP pairs efficiently by grouping instructions by ThreadID first so
// the query budget is spent on cross-thread and self-overlapping thread pairs
// rather than wasted on sequential single-thread instruction pairs.
struct MHPPairSample {
  std::vector<std::pair<const Instruction *, const Instruction *>> pairs;
  bool hitPairLimit = false;
  bool hitQueryLimit = false;
};

MHPPairSample sampleMHPPairs(Module &module, IMHPAnalysis *mhpAnalysis,
                             size_t maxPairs, size_t maxQueries) {
  MHPPairSample result;
  if (!mhpAnalysis)
    return result;

  std::map<ThreadID, std::vector<const Instruction *>> byThread;
  for (Function &func : module) {
    if (func.isDeclaration())
      continue;
    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      const Instruction *inst = &*I;
      byThread[mhpAnalysis->getThreadID(inst)].push_back(inst);
    }
  }

  std::vector<std::pair<ThreadID, const std::vector<const Instruction *> *>>
      groups;
  std::vector<bool> selfOverlaps;
  groups.reserve(byThread.size());
  selfOverlaps.reserve(byThread.size());
  for (const auto &kv : byThread) {
    groups.emplace_back(kv.first, &kv.second);
    const bool overlaps =
        !kv.second.empty() &&
        mhpAnalysis->mayHappenInParallel(kv.second.front(), kv.second.front());
    selfOverlaps.push_back(overlaps);
  }

  size_t queries = 0;

  // 1. Cross-thread pairs first.
  for (size_t g1 = 0; g1 < groups.size(); ++g1) {
    for (size_t g2 = g1 + 1; g2 < groups.size(); ++g2) {
      const auto &insts1 = *groups[g1].second;
      const auto &insts2 = *groups[g2].second;
      for (const Instruction *i1 : insts1) {
        for (const Instruction *i2 : insts2) {
          if (result.pairs.size() >= maxPairs) {
            result.hitPairLimit = true;
            return result;
          }
          if (queries++ >= maxQueries) {
            result.hitQueryLimit = true;
            return result;
          }
          if (mhpAnalysis->mayHappenInParallel(i1, i2)) {
            result.pairs.emplace_back(i1, i2);
          }
        }
      }
    }
  }

  // 2. Self-overlapping thread groups (multi-instance / unknown thread).
  for (size_t g = 0; g < groups.size(); ++g) {
    if (!selfOverlaps[g])
      continue;
    const auto &insts = *groups[g].second;
    for (size_t i = 0; i < insts.size(); ++i) {
      for (size_t j = i + 1; j < insts.size(); ++j) {
        if (result.pairs.size() >= maxPairs) {
          result.hitPairLimit = true;
          return result;
        }
        if (queries++ >= maxQueries) {
          result.hitQueryLimit = true;
          return result;
        }
        if (mhpAnalysis->mayHappenInParallel(insts[i], insts[j])) {
          result.pairs.emplace_back(insts[i], insts[j]);
        }
      }
    }
  }

  return result;
}

} // namespace

ConcurrencyAnalysisDumper::ConcurrencyAnalysisDumper(
    Module &module, IMHPAnalysis *mhpAnalysis, LockSetAnalysis *locksetAnalysis,
    EscapeAnalysis *escapeAnalysis, ThreadAPI *threadAPI)
    : m_module(module), m_mhpAnalysis(mhpAnalysis),
      m_locksetAnalysis(locksetAnalysis), m_escapeAnalysis(escapeAnalysis),
      m_threadAPI(threadAPI) {}

void ConcurrencyAnalysisDumper::dump(raw_ostream &os, bool jsonFormat) const {
  DebugInfoAnalysis debugInfo;

  if (jsonFormat) {
    dumpJSON(os, debugInfo);
  } else {
    dumpText(os, debugInfo);
  }
}

std::string
ConcurrencyAnalysisDumper::improveLocation(const Instruction *inst,
                                           DebugInfoAnalysis &debugInfo,
                                           const std::string &funcName) const {
  std::string loc = debugInfo.getSourceLocation(inst);

  if (loc == "unknown:0") {
    std::string file = debugInfo.getSourceFile(inst);
    int line = debugInfo.getSourceLine(inst);
    if (!file.empty() && line > 0) {
      loc = file + ":" + std::to_string(line);
    } else if (!file.empty()) {
      loc = file + ":?";
    } else if (inst->getFunction()) {
      loc = "function:" + funcName;
    }
  }

  return loc;
}

void ConcurrencyAnalysisDumper::dumpText(raw_ostream &os,
                                         DebugInfoAnalysis &debugInfo) const {
  os << "\n";
  os << "========================================\n";
  os << "  CONCURRENCY ANALYSIS RESULTS\n";
  os << "  (Source-Level Facts)\n";
  os << "========================================\n\n";

  auto locksetStats = m_locksetAnalysis->getStatistics();

  os << "=== Analysis Statistics ===\n";
  os << "MHP Pairs: " << m_mhpAnalysis->getMhpPairCount() << "\n";
  os << "Total Locks: " << locksetStats.num_locks << "\n";
  os << "Lock Acquires: " << locksetStats.num_acquires << "\n";
  os << "Lock Releases: " << locksetStats.num_releases << "\n";
  if (m_extended.hasOpenMP) {
    os << "OpenMP Tasks: " << m_extended.openMP.task_count
       << " (parallel regions: " << m_extended.openMP.parallel_region_count
       << ", barriers: " << m_extended.openMP.barrier_count << ")\n";
  }
  if (m_extended.hasMPI) {
    os << "MPI Operations: " << m_extended.mpi.operation_count
       << " (collectives: " << m_extended.mpi.collective_operation_count
       << ", RMA: " << m_extended.mpi.rma_operation_count << ")\n";
  }
  if (m_extended.hasCUDA) {
    os << "CUDA Kernels: " << m_extended.cuda.kernel_count
       << " (launches: " << m_extended.cuda.kernel_launch_count
       << ", shared races: " << m_extended.cuda.shared_race_count
       << ", global races: " << m_extended.cuda.global_race_count << ")\n";
  }
  if (m_extended.hasLinuxKernel) {
    os << "Linux Kernel Ops: " << m_extended.linuxKernel.operation_count
       << " (locks: " << m_extended.linuxKernel.lock_count
       << ", RCU sections: " << m_extended.linuxKernel.rcu_section_count
       << ")\n";
  }
  if (m_extended.hasSparse) {
    os << "Sparse Refinement: interference edges="
       << m_extended.sparseInterferenceEdges
       << ", points-to facts=" << m_extended.sparsePointsToFacts
       << ", memory regions=" << m_extended.sparseMemoryRegions
       << ", sliced nodes=" << m_extended.sparseSlicedNodes << "/"
       << m_extended.sparseOriginalNodes << "\n";
  }
  os << "\n";

  // 1. Dump MHP pairs grouped by thread so cross-thread pairs are surfaced
  // immediately.
  os << "=== May-Happen-in-Parallel (MHP) Pairs ===\n";
  const size_t maxMhpPairsToShow = 100;
  const size_t maxMhpChecks = 200000;
  MHPPairSample sample =
      sampleMHPPairs(m_module, m_mhpAnalysis, maxMhpPairsToShow, maxMhpChecks);

  constexpr ThreadID kUnknownThread = std::numeric_limits<ThreadID>::max();
  for (const auto &[inst1, inst2] : sample.pairs) {
    std::string func1 = debugInfo.getFunctionName(inst1);
    std::string func2 = debugInfo.getFunctionName(inst2);
    std::string loc1 = improveLocation(inst1, debugInfo, func1);
    std::string loc2 = improveLocation(inst2, debugInfo, func2);
    std::string src1 = debugInfo.getSourceCodeStatement(inst1);
    std::string src2 = debugInfo.getSourceCodeStatement(inst2);

    os << "  MHP Pair:\n";
    os << "    Instruction 1:\n";
    os << "      Function: " << func1 << "\n";
    os << "      Location: " << loc1 << "\n";
    if (!src1.empty()) {
      os << "      Source: " << src1 << "\n";
    }
    if (auto *load = dyn_cast<LoadInst>(inst1)) {
      std::string var = debugInfo.getVariableName(load->getPointerOperand());
      if (!var.empty())
        os << "      Variable: " << var << "\n";
    } else if (auto *store = dyn_cast<StoreInst>(inst1)) {
      std::string var = debugInfo.getVariableName(store->getPointerOperand());
      if (!var.empty())
        os << "      Variable: " << var << "\n";
    }

    os << "    Instruction 2:\n";
    os << "      Function: " << func2 << "\n";
    os << "      Location: " << loc2 << "\n";
    if (!src2.empty()) {
      os << "      Source: " << src2 << "\n";
    }
    if (auto *load = dyn_cast<LoadInst>(inst2)) {
      std::string var = debugInfo.getVariableName(load->getPointerOperand());
      if (!var.empty())
        os << "      Variable: " << var << "\n";
    } else if (auto *store = dyn_cast<StoreInst>(inst2)) {
      std::string var = debugInfo.getVariableName(store->getPointerOperand());
      if (!var.empty())
        os << "      Variable: " << var << "\n";
    }

    ThreadID tid1 = m_mhpAnalysis->getThreadID(inst1);
    ThreadID tid2 = m_mhpAnalysis->getThreadID(inst2);
    if (tid1 != kUnknownThread) {
      os << "    Thread 1 ID: " << tid1 << "\n";
    }
    if (tid2 != kUnknownThread) {
      os << "    Thread 2 ID: " << tid2 << "\n";
    }
    os << "\n";
  }
  if (sample.hitPairLimit) {
    os << "... (showing first " << maxMhpPairsToShow << " MHP pairs)\n";
  } else if (sample.hitQueryLimit) {
    os << "... (stopped after " << maxMhpChecks << " MHP queries)\n";
  }
  os << "\n";

  // 2. Dump lock sets at key instructions
  os << "=== Lock Sets at Key Instructions ===\n";
  size_t lockSetCount = 0;
  const size_t maxLockSetsToShow = 100;

  for (Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      const Instruction *inst = &*I;

      bool shouldShow = false;
      if (m_threadAPI->isTDAcquire(inst) || m_threadAPI->isTDRelease(inst)) {
        shouldShow = true;
      } else if (isa<LoadInst>(inst) || isa<StoreInst>(inst)) {
        if (inst->getNumOperands() > 0) {
          const Value *ptr = isa<LoadInst>(inst)
                                 ? cast<LoadInst>(inst)->getPointerOperand()
                                 : cast<StoreInst>(inst)->getPointerOperand();
          if (m_escapeAnalysis && m_escapeAnalysis->isEscaped(ptr)) {
            shouldShow = true;
          }
        }
      }

      if (!shouldShow)
        continue;

      if (lockSetCount++ >= maxLockSetsToShow) {
        os << "... (showing first " << maxLockSetsToShow << " lock sets)\n";
        goto lockset_done;
      }

      std::string funcName = debugInfo.getFunctionName(inst);
      std::string loc = improveLocation(inst, debugInfo, funcName);
      std::string src = debugInfo.getSourceCodeStatement(inst);

      os << "  Instruction:\n";
      os << "    Function: " << funcName << "\n";
      os << "    Location: " << loc << "\n";
      if (!src.empty()) {
        os << "    Source: " << src << "\n";
      }

      if (auto *load = dyn_cast<LoadInst>(inst)) {
        std::string var = debugInfo.getVariableName(load->getPointerOperand());
        if (!var.empty())
          os << "    Variable: " << var << "\n";
      } else if (auto *store = dyn_cast<StoreInst>(inst)) {
        std::string var = debugInfo.getVariableName(store->getPointerOperand());
        if (!var.empty())
          os << "    Variable: " << var << "\n";
      }

      LockSet mayLocks = m_locksetAnalysis->getMayLockSetAt(inst);
      LockSet mustLocks = m_locksetAnalysis->getMustLockSetAt(inst);

      if (!mayLocks.empty()) {
        os << "    May hold locks: ";
        bool first = true;
        for (LockID lock : mayLocks) {
          if (!first)
            os << ", ";
          std::string lockName = debugInfo.getVariableName(lock);
          if (lockName.empty()) {
            lockName =
                "lock@" + std::to_string(reinterpret_cast<uintptr_t>(lock));
          }
          os << lockName;
          first = false;
        }
        os << "\n";
      }

      if (!mustLocks.empty()) {
        os << "    Must hold locks: ";
        bool first = true;
        for (LockID lock : mustLocks) {
          if (!first)
            os << ", ";
          std::string lockName = debugInfo.getVariableName(lock);
          if (lockName.empty()) {
            lockName =
                "lock@" + std::to_string(reinterpret_cast<uintptr_t>(lock));
          }
          os << lockName;
          first = false;
        }
        os << "\n";
      }

      if (mayLocks.empty() && mustLocks.empty()) {
        os << "    No locks held\n";
      }
      os << "\n";
    }
  }
lockset_done:
  os << "\n";

  // 3. Dump escape analysis results
  os << "=== Escape Analysis (Shared Variables) ===\n";
  size_t escapedCount = 0;
  const size_t maxEscapedToShow = 100;
  std::set<const Value *> shownValues;

  for (Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      const Instruction *inst = &*I;
      const Value *ptr = nullptr;
      if (auto *load = dyn_cast<LoadInst>(inst)) {
        ptr = load->getPointerOperand();
      } else if (auto *store = dyn_cast<StoreInst>(inst)) {
        ptr = store->getPointerOperand();
      }
      if (!ptr || !m_escapeAnalysis || !m_escapeAnalysis->isEscaped(ptr) ||
          !shownValues.insert(ptr).second) {
        continue;
      }
      if (escapedCount++ >= maxEscapedToShow) {
        os << "... (showing first " << maxEscapedToShow
           << " escaped values)\n";
        goto escape_done;
      }

      std::string loc = debugInfo.getSourceLocation(inst);
      std::string funcName = debugInfo.getFunctionName(inst);
      std::string varName = debugInfo.getVariableName(ptr);

      os << "  Escaped Value: ";
      if (!varName.empty()) {
        os << varName;
      } else {
        os << "value@" << std::to_string(reinterpret_cast<uintptr_t>(ptr));
      }
      os << " (accessed at " << funcName << ":" << loc << ")\n";
    }
  }
escape_done:
  os << "\n";

  // 4. Dump thread information
  os << "=== Thread Information ===\n";
  std::map<ThreadID, std::vector<const Instruction *>> threadInstructions;

  for (Function &func : m_module) {
    if (func.isDeclaration())
      continue;
    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      const Instruction *inst = &*I;
      ThreadID tid = m_mhpAnalysis->getThreadID(inst);
      if (tid != kUnknownThread) {
        threadInstructions[tid].push_back(inst);
      }
    }
  }

  for (const auto &pair : threadInstructions) {
    ThreadID tid = pair.first;
    os << "  Thread ID " << tid << ":\n";

    size_t sampleSize = std::min(size_t(5), pair.second.size());
    for (size_t i = 0; i < sampleSize; ++i) {
      const Instruction *inst = pair.second[i];
      std::string funcName = debugInfo.getFunctionName(inst);
      std::string loc = improveLocation(inst, debugInfo, funcName);
      os << "    - " << funcName << " at " << loc << "\n";
    }
    if (pair.second.size() > sampleSize) {
      os << "    ... (" << (pair.second.size() - sampleSize)
         << " more instructions)\n";
    }
  }
  os << "\n";

  // 5. Dump lock operations
  os << "=== Lock Operations ===\n";
  size_t lockOpCount = 0;
  const size_t maxLockOpsToShow = 50;

  for (Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      const Instruction *inst = &*I;

      if (m_threadAPI->isTDAcquire(inst) || m_threadAPI->isTDRelease(inst)) {
        if (lockOpCount++ >= maxLockOpsToShow) {
          os << "... (showing first " << maxLockOpsToShow
             << " lock operations)\n";
          goto lockops_done;
        }

        std::string funcName = debugInfo.getFunctionName(inst);
        std::string loc = improveLocation(inst, debugInfo, funcName);
        std::string src = debugInfo.getSourceCodeStatement(inst);
        std::string opType =
            m_threadAPI->isTDAcquire(inst) ? "ACQUIRE" : "RELEASE";

        os << "  " << opType << ":\n";
        os << "    Function: " << funcName << "\n";
        os << "    Location: " << loc << "\n";
        if (!src.empty()) {
          os << "    Source: " << src << "\n";
        }

        if (auto *call = dyn_cast<CallInst>(inst)) {
          if (call->arg_size() > 0) {
            const Value *lockArg = call->getArgOperand(0);
            std::string lockName = debugInfo.getVariableName(lockArg);
            if (!lockName.empty()) {
              os << "    Lock: " << lockName << "\n";
            }
          }
        }
      }
    }
  }
lockops_done:
  os << "\n";

  os << "========================================\n";
  os << "End of Analysis Results\n";
  os << "========================================\n\n";
}

void ConcurrencyAnalysisDumper::dumpJSON(raw_ostream &os,
                                         DebugInfoAnalysis &debugInfo) const {
  auto escapeJson = [](const std::string &str) -> std::string {
    std::string result;
    for (char c : str) {
      if (c == '"')
        result += "\\\"";
      else if (c == '\\')
        result += "\\\\";
      else if (c == '\n')
        result += "\\n";
      else if (c == '\r')
        result += "\\r";
      else if (c == '\t')
        result += "\\t";
      else
        result += c;
    }
    return result;
  };

  os << "{\n";
  os << "  \"analysis_type\": \"concurrency\",\n";
  os << "  \"module\": \"" << escapeJson(m_module.getModuleIdentifier())
     << "\",\n";

  auto locksetStats = m_locksetAnalysis->getStatistics();

  os << "  \"statistics\": {\n";
  os << "    \"mhp_pairs\": " << m_mhpAnalysis->getMhpPairCount() << ",\n";
  os << "    \"total_locks\": " << locksetStats.num_locks << ",\n";
  os << "    \"lock_acquires\": " << locksetStats.num_acquires << ",\n";
  os << "    \"lock_releases\": " << locksetStats.num_releases;
  if (m_extended.hasOpenMP) {
    os << ",\n    \"openmp\": {\"tasks\": " << m_extended.openMP.task_count
       << ", \"parallel_regions\": " << m_extended.openMP.parallel_region_count
       << ", \"barriers\": " << m_extended.openMP.barrier_count << "}";
  }
  if (m_extended.hasMPI) {
    os << ",\n    \"mpi\": {\"operations\": " << m_extended.mpi.operation_count
       << ", \"collectives\": " << m_extended.mpi.collective_operation_count
       << ", \"rma_operations\": " << m_extended.mpi.rma_operation_count << "}";
  }
  if (m_extended.hasCUDA) {
    os << ",\n    \"cuda\": {\"kernels\": " << m_extended.cuda.kernel_count
       << ", \"launches\": " << m_extended.cuda.kernel_launch_count
       << ", \"shared_races\": " << m_extended.cuda.shared_race_count
       << ", \"global_races\": " << m_extended.cuda.global_race_count << "}";
  }
  if (m_extended.hasLinuxKernel) {
    os << ",\n    \"linux_kernel\": {\"operations\": "
       << m_extended.linuxKernel.operation_count
       << ", \"locks\": " << m_extended.linuxKernel.lock_count
       << ", \"rcu_sections\": " << m_extended.linuxKernel.rcu_section_count
       << "}";
  }
  if (m_extended.hasSparse) {
    os << ",\n    \"sparse_refinement\": {\"interference_edges\": "
       << m_extended.sparseInterferenceEdges
       << ", \"lock_span_pruned_pairs\": "
       << m_extended.sparseLockSpanPrunedPairs
       << ", \"points_to_facts\": " << m_extended.sparsePointsToFacts
       << ", \"memory_regions\": " << m_extended.sparseMemoryRegions
       << ", \"original_nodes\": " << m_extended.sparseOriginalNodes
       << ", \"sliced_nodes\": " << m_extended.sparseSlicedNodes << "}";
  }
  os << "\n  },\n";

  // MHP Pairs (thread-grouped sampling)
  os << "  \"mhp_pairs\": [\n";
  const size_t maxMhpPairsToShow = 1000;
  const size_t maxMhpChecks = 500000;
  MHPPairSample sample =
      sampleMHPPairs(m_module, m_mhpAnalysis, maxMhpPairsToShow, maxMhpChecks);

  constexpr ThreadID kUnknownThread = std::numeric_limits<ThreadID>::max();
  bool firstMhp = true;
  for (const auto &[inst1, inst2] : sample.pairs) {
    if (!firstMhp)
      os << ",\n";
    firstMhp = false;

    std::string func1 = debugInfo.getFunctionName(inst1);
    std::string func2 = debugInfo.getFunctionName(inst2);
    std::string loc1 = debugInfo.getSourceLocation(inst1);
    std::string loc2 = debugInfo.getSourceLocation(inst2);
    std::string src1 = debugInfo.getSourceCodeStatement(inst1);
    std::string src2 = debugInfo.getSourceCodeStatement(inst2);

    ThreadID tid1 = m_mhpAnalysis->getThreadID(inst1);
    ThreadID tid2 = m_mhpAnalysis->getThreadID(inst2);

    auto emitInstObj = [&](const Instruction *inst, const std::string &fn,
                           const std::string &loc, const std::string &src,
                           ThreadID tid) {
      os << "        \"function\": \"" << escapeJson(fn) << "\",\n";
      os << "        \"location\": \"" << escapeJson(loc) << "\",\n";
      if (!src.empty()) {
        os << "        \"source\": \"" << escapeJson(src) << "\",\n";
      }
      if (tid != kUnknownThread) {
        os << "        \"thread_id\": " << tid << ",\n";
      }
      if (auto *load = dyn_cast<LoadInst>(inst)) {
        std::string var = debugInfo.getVariableName(load->getPointerOperand());
        if (!var.empty()) {
          os << "        \"variable\": \"" << escapeJson(var) << "\",\n";
        }
      } else if (auto *store = dyn_cast<StoreInst>(inst)) {
        std::string var = debugInfo.getVariableName(store->getPointerOperand());
        if (!var.empty()) {
          os << "        \"variable\": \"" << escapeJson(var) << "\",\n";
        }
      }
      os << "        \"type\": \""
         << (isa<LoadInst>(inst)    ? "load"
             : isa<StoreInst>(inst) ? "store"
                                    : "other")
         << "\"\n";
    };

    os << "    {\n";
    os << "      \"instruction1\": {\n";
    emitInstObj(inst1, func1, loc1, src1, tid1);
    os << "      },\n";
    os << "      \"instruction2\": {\n";
    emitInstObj(inst2, func2, loc2, src2, tid2);
    os << "      }\n";
    os << "    }";
  }
  os << "\n  ],\n";

  // Lock Operations
  os << "  \"lock_operations\": [\n";
  size_t lockOpCount = 0;
  const size_t maxLockOpsToShow = 500;
  bool firstLock = true;

  for (Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      const Instruction *inst = &*I;

      if (m_threadAPI->isTDAcquire(inst) || m_threadAPI->isTDRelease(inst)) {
        if (lockOpCount++ >= maxLockOpsToShow)
          goto lockops_json_done;

        if (!firstLock)
          os << ",\n";
        firstLock = false;

        std::string funcName = debugInfo.getFunctionName(inst);
        std::string loc = debugInfo.getSourceLocation(inst);
        std::string src = debugInfo.getSourceCodeStatement(inst);
        std::string opType =
            m_threadAPI->isTDAcquire(inst) ? "acquire" : "release";

        os << "    {\n";
        os << "      \"operation\": \"" << opType << "\",\n";
        os << "      \"function\": \"" << escapeJson(funcName) << "\",\n";
        os << "      \"location\": \"" << escapeJson(loc) << "\",\n";
        if (!src.empty()) {
          os << "      \"source\": \"" << escapeJson(src) << "\",\n";
        }

        if (auto *call = dyn_cast<CallInst>(inst)) {
          if (call->arg_size() > 0) {
            const Value *lockArg = call->getArgOperand(0);
            std::string lockName = debugInfo.getVariableName(lockArg);
            if (!lockName.empty()) {
              os << "      \"lock\": \"" << escapeJson(lockName) << "\",\n";
            }
          }
        }

        LockSet mayLocks = m_locksetAnalysis->getMayLockSetAt(inst);
        if (!mayLocks.empty()) {
          os << "      \"may_hold_locks\": [";
          bool first = true;
          for (LockID lock : mayLocks) {
            if (!first)
              os << ", ";
            std::string lockName = debugInfo.getVariableName(lock);
            if (lockName.empty()) {
              lockName =
                  "lock@" + std::to_string(reinterpret_cast<uintptr_t>(lock));
            }
            os << "\"" << escapeJson(lockName) << "\"";
            first = false;
          }
          os << "],\n";
        }

        os << "      \"must_hold_locks\": [";
        LockSet mustLocks = m_locksetAnalysis->getMustLockSetAt(inst);
        bool firstMust = true;
        for (LockID lock : mustLocks) {
          if (!firstMust)
            os << ", ";
          std::string lockName = debugInfo.getVariableName(lock);
          if (lockName.empty()) {
            lockName =
                "lock@" + std::to_string(reinterpret_cast<uintptr_t>(lock));
          }
          os << "\"" << escapeJson(lockName) << "\"";
          firstMust = false;
        }
        os << "]\n";
        os << "    }";
      }
    }
  }
lockops_json_done:
  os << "\n  ],\n";

  // Escape Analysis
  os << "  \"escaped_variables\": [\n";
  std::set<const Value *> shownValues;
  bool firstEscaped = true;

  for (Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      const Instruction *inst = &*I;
      const Value *ptr = nullptr;
      if (auto *load = dyn_cast<LoadInst>(inst)) {
        ptr = load->getPointerOperand();
      } else if (auto *store = dyn_cast<StoreInst>(inst)) {
        ptr = store->getPointerOperand();
      }
      if (!ptr || !m_escapeAnalysis || !m_escapeAnalysis->isEscaped(ptr) ||
          !shownValues.insert(ptr).second) {
        continue;
      }

      if (!firstEscaped)
        os << ",\n";
      firstEscaped = false;

      std::string funcName = debugInfo.getFunctionName(inst);
      std::string loc = debugInfo.getSourceLocation(inst);
      std::string varName = debugInfo.getVariableName(ptr);
      if (varName.empty()) {
        varName =
            "value@" + std::to_string(reinterpret_cast<uintptr_t>(ptr));
      }

      os << "    {\n";
      os << "      \"variable\": \"" << escapeJson(varName) << "\",\n";
      os << "      \"function\": \"" << escapeJson(funcName) << "\",\n";
      os << "      \"location\": \"" << escapeJson(loc) << "\"\n";
      os << "    }";
    }
  }
  os << "\n  ]\n";

  os << "}\n";
}

} // namespace concurrency
