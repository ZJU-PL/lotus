// Dependency-validated publication of partial sparse pointer-analysis effects.
// The transfer functions are shared with the sequential FlowSensitivePTA.
#include "Alias/InclusionBased/FlowSensitive/Sparse/FlowSensitivePTA.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

using namespace lotus::analysis;

namespace lotus::alias {

class FlowSensitivePTA::ParallelExecution {
  enum Channel : unsigned { Top, In, Out, ChannelCount };
  struct Snapshot {
    StoredSet top;
    MemoryState in, out;
    std::array<std::size_t, ChannelCount> version{};
  };
  struct Slot {
    const SVFGNode *node = nullptr;
    std::shared_ptr<const Snapshot> snapshot = std::make_shared<Snapshot>();
    std::array<std::unordered_set<std::size_t>, ChannelCount> readers;
    bool queued = false, running = false, rerun = false;
    bool strong = false, weak = false;
  };
  struct Read {
    std::shared_ptr<const Snapshot> snapshot;
    unsigned channels = 0;
  };
  using Calls =
      std::set<std::pair<const llvm::CallBase *, const llvm::Function *>>;

  FlowSensitivePTA &owner;
  std::vector<Slot> slots;
  std::unordered_map<NodeID, std::size_t> indices;
  std::deque<std::size_t> ready;
  std::mutex mutex;
  std::condition_variable changed;
  std::size_t active = 0;
  bool stopped = false;
  std::exception_ptr failure;
  Calls calls;
  MemoryState initialMemory;

  // Must be called with mutex held. A running task receives a persistent
  // rerun request, so publishing a dependency cannot lose a wake-up.
  void enqueue(std::size_t index) {
    Slot &slot = slots[index];
    if (!owner.inScope(slot.node))
      return;
    if (slot.running) {
      slot.rerun = true;
    } else if (!slot.queued) {
      slot.queued = true;
      ready.push_back(index);
    }
  }

  static bool sameMemory(const MemoryState &left, const MemoryState &right) {
    if (left.size() != right.size())
      return false;
    for (const auto &entry : left) {
      const auto it = right.find(entry.first);
      if (it == right.end() || entry.second.mutableSet != it->second.mutableSet)
        return false;
    }
    return true;
  }

  void worker() {
    try {
      if (owner.timeLimitExceeded()) {
        owner.noteTimeout();
        {
          std::lock_guard<std::mutex> lock(mutex);
          stopped = true;
        }
        changed.notify_all();
        return;
      }
      Config config = owner.config_;
      config.parallel = false;
      // Immutable snapshots contain ordinary sets. Interning is performed
      // once, on export, if the public result backend is hash-consed.
      config.setBackend = PointsToSetBackend::Mutable;
      FlowSensitivePTA evaluator(*owner.graph_, config);
      evaluator.initialMemory_ = initialMemory;
      evaluator.recursiveFunctions_ = owner.recursiveFunctions_;
      while (true) {
        std::size_t task;
        {
          std::unique_lock<std::mutex> lock(mutex);
          changed.wait(lock, [&] { return stopped || !ready.empty(); });
          if (stopped)
            return;
          task = ready.front();
          ready.pop_front();
          slots[task].queued = false;
          slots[task].running = true;
          ++active;
          owner.stats_.parallelPeakWorkers =
              std::max(owner.stats_.parallelPeakWorkers, active);
        }

        std::unordered_map<std::size_t, Read> reads;
        const Snapshot empty;
        auto read = [&](const SVFGNode *node,
                        Channel channel) -> const Snapshot & {
          const auto it = node ? indices.find(node->getId()) : indices.end();
          if (it == indices.end())
            return empty;
          auto inserted = reads.emplace(it->second, Read{});
          Read &record = inserted.first->second;
          if (inserted.second) {
            std::lock_guard<std::mutex> lock(mutex);
            record.snapshot = slots[it->second].snapshot;
          }
          record.channels |= 1U << channel;
          return *record.snapshot;
        };
        evaluator.topRead_ = [&](const SVFGNode *node) -> const StoredSet & {
          return read(node, Top).top;
        };
        evaluator.memoryRead_ =
            [&](const SVFGNode *node, bool outgoing,
                const PointsToSet *) -> const MemoryState & {
          const Snapshot &snapshot = read(node, outgoing ? Out : In);
          return outgoing ? snapshot.out : snapshot.in;
        };
        Calls proposedCalls;
        evaluator.config_.connectIndirectCall =
            owner.config_.connectIndirectCall
                ? Config::IndirectCallConnector(
                      [&](const llvm::CallBase *site,
                          const llvm::Function *target) {
                        proposedCalls.emplace(site, target);
                        return false;
                      })
                : Config::IndirectCallConnector{};
        // The worker only proposes candidates; the owner applies the bound
        // when it retires them, so rejections are counted exactly once.
        evaluator.config_.isIndirectCallTargetAllowed = {};

        const NodeID id = slots[task].node->getId();
        evaluator.strongUpdateSites_.clear();
        evaluator.weakUpdateSites_.clear();
        const auto strongBefore = evaluator.stats_.strongUpdateExecutions;
        const auto weakBefore = evaluator.stats_.weakUpdateExecutions;
        // Reads go through the snapshot accessors; only this node's outputs
        // enter the evaluator's maps. No transfer mutates another task's state.
        evaluator.transfer(*slots[task].node);
        auto result = std::make_shared<Snapshot>();
        result->top = std::move(evaluator.topLevelPointsTo_.at(id));
        result->in = std::move(evaluator.dfIn_.at(id));
        result->out = std::move(evaluator.dfOut_.at(id));
        evaluator.topLevelPointsTo_.clear();
        evaluator.dfIn_.clear();
        evaluator.dfOut_.clear();

        {
          std::lock_guard<std::mutex> lock(mutex);
          ++owner.stats_.nodeProcesses;
          Slot &slot = slots[task];
          bool valid = true;
          for (const auto &entry : reads) {
            const Snapshot &current = *slots[entry.first].snapshot;
            for (unsigned channel = 0; channel < ChannelCount; ++channel)
              if ((entry.second.channels & (1U << channel)) &&
                  current.version[channel] !=
                      entry.second.snapshot->version[channel])
                valid = false;
          }
          if (!valid) {
            ++owner.stats_.parallelConflicts;
            slot.rerun = true;
          } else {
            ++owner.stats_.parallelCommits;
            // Register even unchanged results and reads of empty sets.
            // Future discoveries can change both a value and its read set.
            for (const auto &entry : reads)
              for (unsigned channel = 0; channel < ChannelCount; ++channel)
                if (entry.second.channels & (1U << channel))
                  slots[entry.first].readers[channel].insert(task);
            const Snapshot &previous = *slot.snapshot;
            const std::array<bool, ChannelCount> updates{
                previous.top.mutableSet != result->top.mutableSet,
                !sameMemory(previous.in, result->in),
                !sameMemory(previous.out, result->out)};
            result->version = previous.version;
            for (unsigned channel = 0; channel < ChannelCount; ++channel) {
              if (!updates[channel])
                continue;
              ++result->version[channel];
              ++owner.stats_.parallelPublications;
              for (std::size_t reader : slot.readers[channel]) {
                enqueue(reader);
                ++owner.stats_.parallelNotifications;
              }
            }
            if (std::any_of(updates.begin(), updates.end(),
                            [](bool update) { return update; }))
              slot.snapshot = std::move(result);
            slot.strong = evaluator.strongUpdateSites_.count(id) != 0;
            slot.weak = evaluator.weakUpdateSites_.count(id) != 0;
            owner.stats_.strongUpdateExecutions +=
                evaluator.stats_.strongUpdateExecutions - strongBefore;
            owner.stats_.weakUpdateExecutions +=
                evaluator.stats_.weakUpdateExecutions - weakBefore;
            calls.insert(proposedCalls.begin(), proposedCalls.end());
          }
          slot.running = false;
          --active;
          if (slot.rerun) {
            slot.rerun = false;
            enqueue(task);
          }
          if (ready.empty() && active == 0)
            stopped = true;
        }
        changed.notify_all();
      }
    } catch (...) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (!failure)
          failure = std::current_exception();
        stopped = true;
      }
      changed.notify_all();
    }
  }

public:
  explicit ParallelExecution(FlowSensitivePTA &owner) : owner(owner) {
    std::vector<const SVFGNode *> nodes;
    for (const auto &entry : *owner.graph_)
      nodes.push_back(entry.second);
    // Stable initial order makes the one-worker schedule reproducible.
    std::sort(nodes.begin(), nodes.end(),
              [](const SVFGNode *a, const SVFGNode *b) {
                return a->getId() < b->getId();
              });
    slots.resize(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      slots[i].node = nodes[i];
      indices.emplace(nodes[i]->getId(), i);
      enqueue(i);
    }
    for (const auto &entry : owner.initialMemory_) {
      StoredSet set;
      set.mutableSet = owner.materialize(entry.second);
      initialMemory.emplace(entry.first, std::move(set));
    }
  }

  Calls run() {
    if (ready.empty())
      return {};
    unsigned threads = owner.config_.workerThreads;
    if (threads == 0)
      threads = std::max(1U, std::thread::hardware_concurrency());
    threads =
        static_cast<unsigned>(std::min<std::size_t>(threads, ready.size()));
    owner.stats_.parallelThreads = threads;
    std::vector<std::thread> workers;
    try {
      for (unsigned i = 1; i < threads; ++i)
        workers.emplace_back([&] { worker(); });
    } catch (...) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        stopped = true;
      }
      changed.notify_all();
      for (auto &thread : workers)
        thread.join();
      throw;
    }
    worker();
    for (auto &thread : workers)
      thread.join();
    if (failure)
      std::rethrow_exception(failure);

    auto exportSet = [&](const StoredSet &set) {
      StoredSet result;
      if (owner.config_.setBackend == PointsToSetBackend::HashConsed)
        result.interned = owner.arena_.intern(set.mutableSet);
      else
        result.mutableSet = set.mutableSet;
      return result;
    };
    for (const Slot &slot : slots) {
      if (!owner.inScope(slot.node) || !slot.snapshot)
        continue;
      const NodeID id = slot.node->getId();
      owner.topLevelPointsTo_[id] = exportSet(slot.snapshot->top);
      for (const auto &entry : slot.snapshot->in)
        owner.dfIn_[id].emplace(entry.first, exportSet(entry.second));
      for (const auto &entry : slot.snapshot->out)
        owner.dfOut_[id].emplace(entry.first, exportSet(entry.second));
      if (slot.strong)
        owner.strongUpdateSites_.insert(id);
      if (slot.weak)
        owner.weakUpdateSites_.insert(id);
    }
    return calls;
  }
};

const FlowSensitivePTA::Statistics &FlowSensitivePTA::solveParallel() {
  if (!config_.unorderedParallel)
    return solveOrderedParallel();
  const auto started = std::chrono::steady_clock::now();
  stats_ = {};
  buildObjectClustering();
  hasDeadline_ = config_.timeLimitSeconds != 0;
  if (hasDeadline_)
    deadline_ = started +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(config_.timeLimitSeconds));
  bool changed;
  do {
    if (timeLimitExceeded()) {
      stats_.timedOut = true;
      break;
    }
    ++stats_.topologyEpochs;
    topLevelPointsTo_.clear();
    dfIn_.clear();
    dfOut_.clear();
    initialMemory_.clear();
    strongUpdateSites_.clear();
    weakUpdateSites_.clear();
    if (config_.setBackend == PointsToSetBackend::HashConsed)
      arena_.reset();
    initializeRecursiveFunctions();
    initializeGlobalMemory();
    ParallelExecution execution(*this);
    const auto calls = execution.run();
    changed = false;
    for (const auto &call : calls) {
      if (config_.isIndirectCallTargetAllowed &&
          !config_.isIndirectCallTargetAllowed(call.first, call.second)) {
        ++stats_.indirectCallsRejected;
        continue;
      }
      if (config_.connectIndirectCall(call.first, call.second)) {
        changed = true;
        ++stats_.indirectCallEdges;
        if (!config_.isIndirectCallTargetAllowed)
          ++stats_.unboundedIndirectCalls;
      }
    }
  } while (changed && !stats_.timedOut);
  const SCCInfo scc = computeSCCs();
  stats_.sccs = scc.components.size();
  for (const auto &component : scc.components) {
    stats_.nodes += component.size();
    stats_.maxSccSize = std::max(stats_.maxSccSize, component.size());
  }
  for (const auto &entry : topLevelPointsTo_)
    stats_.topLevelFacts += materialize(entry.second).size();
  collectStatistics();
  stats_.strongUpdates = strongUpdateSites_.size();
  stats_.weakUpdates = weakUpdateSites_.size();
  stats_.svfgHasSU = !strongUpdateSites_.empty();
  if (config_.setBackend == PointsToSetBackend::HashConsed) {
    const auto hashStats = arena_.statistics();
    stats_.hashConsedUniqueSets = hashStats.uniqueSets;
    stats_.hashConsedUnionCacheHits = hashStats.unionCacheHits;
  }
  stats_.solveSeconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
          .count();
  return stats_;
}

} // namespace lotus::alias
