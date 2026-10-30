// Speculate on partial effects; certify their reads and commit in the exact
// reference worklist order. This preserves non-monotone replacement semantics.
#include "Alias/InclusionBased/FlowSensitive/Sparse/FlowSensitivePTA.h"

#include <algorithm>
#include <array>
#include <atomic>
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

class FlowSensitivePTA::OrderedExecution {
  enum Channel : unsigned { Top, In, Out, ChannelCount };
  struct Snapshot {
    StoredSet top;
    MemoryState in, out;
    std::array<std::size_t, ChannelCount> version{};
    // Different predictions may propose the same next version. Origins
    // certify that the particular predicted effect actually retired.
    std::array<std::uint64_t, ChannelCount> origin{};
    std::array<std::uint64_t, 2> wildcardOrigin{};
  };
  struct Read {
    std::shared_ptr<const Snapshot> snapshot;
    unsigned channels = 0;
    // Entry presence and contents are compared with the captured snapshot.
    std::array<PointsToSet, 2> objects;
    unsigned wildcards = 0;
  };
  struct Memo {
    std::unordered_map<std::size_t, Read> reads;
    bool strong = false, weak = false;
    std::size_t strongExecutions = 0, weakExecutions = 0;
  };
  struct Draft {
    std::uint64_t identity = 0;
    std::size_t forwardedReads = 0;
    std::size_t objectReads = 0, negativeReads = 0, wildcardReads = 0;
    std::shared_ptr<const Snapshot> result;
    std::shared_ptr<const Memo> replay;
    std::unordered_map<std::size_t, Read> reads;
    std::vector<std::pair<const llvm::CallBase *, const llvm::Function *>>
        calls;
    std::array<bool, ChannelCount> updates{};
    bool strong = false, weak = false;
    std::size_t strongExecutions = 0, weakExecutions = 0;
  };
  struct Slot {
    const SVFGNode *node;
    std::shared_ptr<const Snapshot> snapshot;
    std::unique_ptr<Draft> draft;
    std::shared_ptr<const Memo> memo;
    bool queued = false, running = false;
  };

  FlowSensitivePTA &owner;
  SCCInfo scc;
  std::vector<Slot> slots;
  std::unordered_map<NodeID, std::size_t> indices;
  MemoryState initial;
  PointsToSet unknownObjects;
  std::mutex mutex;
  std::condition_variable changed;
  std::condition_variable dispatch;
  std::deque<std::size_t> ready;
  std::vector<std::thread> workers;
  std::size_t active = 0, outstanding = 0;
  unsigned threadCount = 1;
  unsigned blockSize = 1;
  std::atomic<std::uint64_t> nextIdentity{1};
  bool stopped = false, paused = false;
  std::exception_ptr failure;

  static const PointsToSet &points(const StoredSet &set) {
    return set.sharedSet ? *set.sharedSet : set.mutableSet;
  }

  static bool sameSet(const StoredSet &a, const StoredSet &b) {
    return (a.sharedSet && a.sharedSet == b.sharedSet) ||
           points(a) == points(b);
  }

  static bool sameMemory(const MemoryState &a, const MemoryState &b) {
    if (a.size() != b.size())
      return false;
    for (const auto &entry : a) {
      const auto it = b.find(entry.first);
      if (it == b.end() || !sameSet(entry.second, it->second))
        return false;
    }
    return true;
  }

  std::unique_ptr<FlowSensitivePTA> makeEvaluator() {
    Config config = owner.config_;
    config.parallel = false;
    config.setBackend = PointsToSetBackend::Mutable;
    auto evaluator = std::make_unique<FlowSensitivePTA>(*owner.graph_, config);
    evaluator->persistentSets_ = owner.config_.shareParallelSets;
    evaluator->initialMemory_ = initial;
    evaluator->recursiveFunctions_ = owner.recursiveFunctions_;
    return evaluator;
  }

  using Overlay =
      std::unordered_map<std::size_t, std::shared_ptr<const Snapshot>>;

  std::size_t windowSize() const {
    return 2 * static_cast<std::size_t>(threadCount) * blockSize;
  }

  static bool sameObject(const MemoryState &actual, const MemoryState &observed,
                         ObjectID object) {
    const auto a = actual.find(object), b = observed.find(object);
    if (a == actual.end() || b == observed.end())
      return (a == actual.end()) == (b == observed.end());
    return sameSet(a->second, b->second);
  }

  bool matches(const Snapshot &actual, const Read &observed) const {
    for (unsigned channel = 0; channel < ChannelCount; ++channel)
      if ((observed.channels & (1U << channel)) &&
          (actual.version[channel] != observed.snapshot->version[channel] ||
           actual.origin[channel] != observed.snapshot->origin[channel]))
        return false;
    for (unsigned memory = 0; memory < 2; ++memory) {
      const unsigned channel = memory + In;
      if (actual.origin[channel] == observed.snapshot->origin[channel])
        continue;
      const MemoryState &a = memory == 0 ? actual.in : actual.out;
      const MemoryState &b =
          memory == 0 ? observed.snapshot->in : observed.snapshot->out;
      for (ObjectID object : observed.objects[memory])
        if (!sameObject(a, b, object))
          return false;
      if ((observed.wildcards & (1U << memory)) &&
          actual.wildcardOrigin[memory] !=
              observed.snapshot->wildcardOrigin[memory])
        return false;
    }
    return true;
  }

  bool salvaged(const Draft &draft) const {
    const auto &reads = draft.replay ? draft.replay->reads : draft.reads;
    for (const auto &entry : reads)
      for (unsigned memory = 0; memory < 2; ++memory) {
        const unsigned channel = memory + In;
        if (!(entry.second.channels & (1U << channel)) &&
            (!entry.second.objects[memory].empty() ||
             (entry.second.wildcards & (1U << memory))) &&
            slots[entry.first].snapshot->origin[channel] !=
                entry.second.snapshot->origin[channel])
          return true;
      }
    return false;
  }

  std::unique_ptr<Draft> replay(std::size_t task,
                                std::shared_ptr<const Memo> memo) {
    auto draft = std::make_unique<Draft>();
    draft->result = memo->reads.at(task).snapshot;
    draft->strong = memo->strong;
    draft->weak = memo->weak;
    draft->strongExecutions = memo->strongExecutions;
    draft->weakExecutions = memo->weakExecutions;
    draft->replay = std::move(memo);
    return draft;
  }

  std::unique_ptr<Draft> evaluate(std::size_t task, FlowSensitivePTA &evaluator,
                                  Overlay *overlay = nullptr) {
    if (owner.config_.parallelMemoize) {
      std::lock_guard<std::mutex> lock(mutex);
      const auto memo = slots[task].memo;
      bool reusable = memo != nullptr;
      if (memo)
        for (const auto &entry : memo->reads) {
          auto predicted =
              overlay ? overlay->find(entry.first) : Overlay::iterator{};
          const Snapshot &actual = overlay && predicted != overlay->end()
                                       ? *predicted->second
                                       : *slots[entry.first].snapshot;
          reusable &= matches(actual, entry.second);
        }
      if (reusable) {
        auto draft = replay(task, memo);
        if (overlay)
          (*overlay)[task] = draft->result;
        return draft;
      }
    }
    auto draft = std::make_unique<Draft>();
    draft->identity = nextIdentity.fetch_add(1, std::memory_order_relaxed);
    const Snapshot empty;
    auto read = [&](const SVFGNode *node, Channel channel,
                    const PointsToSet *objects = nullptr) -> const Snapshot & {
      const auto it = node ? indices.find(node->getId()) : indices.end();
      if (it == indices.end())
        return empty;
      auto inserted = draft->reads.emplace(it->second, Read{});
      Read &record = inserted.first->second;
      if (inserted.second) {
        if (overlay) {
          const auto predicted = overlay->find(it->second);
          if (predicted != overlay->end()) {
            record.snapshot = predicted->second;
            ++draft->forwardedReads;
          }
        }
        if (!record.snapshot) {
          std::lock_guard<std::mutex> lock(mutex);
          record.snapshot = slots[it->second].snapshot;
        }
      }
      if (channel == Top || !objects ||
          !owner.config_.parallelObjectCertificates) {
        record.channels |= 1U << channel;
      } else if (!(record.channels & (1U << channel))) {
        const unsigned memory = channel - In;
        const MemoryState &state =
            channel == Out ? record.snapshot->out : record.snapshot->in;
        for (ObjectID object : *objects) {
          const bool inserted = record.objects[memory].insert(object).second;
          if (inserted)
            ++draft->objectReads;
          if (state.count(object) == 0) {
            if (inserted)
              ++draft->negativeReads;
            // The transfer scans unknown entries only when a precise entry
            // is missing. Track the entire wildcard namespace, including a
            // future unknown entry appearing in a previously empty namespace.
            if (!(record.wildcards & (1U << memory))) {
              record.wildcards |= 1U << memory;
              ++draft->wildcardReads;
            }
          }
        }
      }
      return *record.snapshot;
    };
    evaluator.topRead_ = [&](const SVFGNode *node) -> const StoredSet & {
      return read(node, Top).top;
    };
    evaluator.memoryRead_ =
        [&](const SVFGNode *node, bool outgoing,
            const PointsToSet *objects) -> const MemoryState & {
      const auto &snapshot = read(node, outgoing ? Out : In, objects);
      return outgoing ? snapshot.out : snapshot.in;
    };
    evaluator.config_.connectIndirectCall =
        owner.config_.connectIndirectCall
            ? Config::IndirectCallConnector([&](const llvm::CallBase *site,
                                                const llvm::Function *target) {
                draft->calls.emplace_back(site, target);
                return false;
              })
            : Config::IndirectCallConnector{};
    evaluator.strongUpdateSites_.clear();
    evaluator.weakUpdateSites_.clear();
    const auto strongBefore = evaluator.stats_.strongUpdateExecutions;
    const auto weakBefore = evaluator.stats_.weakUpdateExecutions;
    const NodeID id = slots[task].node->getId();
    // Comparing potentially large memory maps belongs to evaluation. Record
    // all of this task's old channels so that its change decision is validated
    // along with the actual transfer reads before retirement.
    const Snapshot &before = read(slots[task].node, Top);
    read(slots[task].node, In);
    read(slots[task].node, Out);
    evaluator.transfer(*slots[task].node);
    auto result = std::make_shared<Snapshot>();
    result->top = std::move(evaluator.topLevelPointsTo_.at(id));
    result->in = std::move(evaluator.dfIn_.at(id));
    result->out = std::move(evaluator.dfOut_.at(id));
    draft->updates[Top] = !sameSet(before.top, result->top);
    result->wildcardOrigin = before.wildcardOrigin;
    for (unsigned memory = 0; memory < 2; ++memory) {
      const MemoryState &oldState = memory == 0 ? before.in : before.out;
      const MemoryState &newState = memory == 0 ? result->in : result->out;
      draft->updates[memory + In] = !sameMemory(oldState, newState);
      if (!owner.config_.parallelObjectCertificates ||
          !draft->updates[memory + In])
        continue;
      for (ObjectID object : unknownObjects)
        if (!sameObject(oldState, newState, object)) {
          result->wildcardOrigin[memory] = draft->identity;
          break;
        }
    }
    result->version = before.version;
    result->origin = before.origin;
    for (unsigned channel = 0; channel < ChannelCount; ++channel)
      if (draft->updates[channel]) {
        ++result->version[channel];
        result->origin[channel] = draft->identity;
      }
    draft->result = std::move(result);
    if (overlay)
      (*overlay)[task] = draft->result;
    evaluator.topLevelPointsTo_.clear();
    evaluator.dfIn_.clear();
    evaluator.dfOut_.clear();
    draft->strong = evaluator.strongUpdateSites_.count(id) != 0;
    draft->weak = evaluator.weakUpdateSites_.count(id) != 0;
    draft->strongExecutions =
        evaluator.stats_.strongUpdateExecutions - strongBefore;
    draft->weakExecutions = evaluator.stats_.weakUpdateExecutions - weakBefore;
    return draft;
  }

  void begin(std::size_t task) {
    slots[task].queued = false;
    slots[task].running = true;
    ++active;
    owner.stats_.parallelPeakWorkers =
        std::max(owner.stats_.parallelPeakWorkers, active);
  }

  void complete(std::size_t task, std::unique_ptr<Draft> draft,
                bool single = true) {
    std::lock_guard<std::mutex> lock(mutex);
    owner.stats_.parallelForwardedReads += draft->forwardedReads;
    owner.stats_.parallelObjectReads += draft->objectReads;
    owner.stats_.parallelNegativeReads += draft->negativeReads;
    owner.stats_.parallelWildcardReads += draft->wildcardReads;
    if (!draft->replay)
      ++owner.stats_.nodeProcesses;
    slots[task].draft = std::move(draft);
    slots[task].running = false;
    if (single)
      --active;
    changed.notify_one();
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
        dispatch.notify_all();
        return;
      }
      auto evaluator = makeEvaluator();
      while (true) {
        std::vector<std::size_t> block;
        {
          std::unique_lock<std::mutex> lock(mutex);
          dispatch.wait(lock,
                        [&] { return stopped || (!paused && !ready.empty()); });
          if (stopped)
            return;
          while (!ready.empty() && block.size() < blockSize) {
            const std::size_t task = ready.front();
            ready.pop_front();
            slots[task].queued = false;
            slots[task].running = true;
            block.push_back(task);
          }
          ++active;
          ++owner.stats_.parallelBlocks;
          owner.stats_.parallelPeakWorkers =
              std::max(owner.stats_.parallelPeakWorkers, active);
        }
        Overlay overlay;
        for (std::size_t task : block)
          complete(task, evaluate(task, *evaluator, &overlay), false);
        {
          std::lock_guard<std::mutex> lock(mutex);
          --active;
        }
        changed.notify_one();
      }
    } catch (...) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (!failure)
          failure = std::current_exception();
        stopped = true;
      }
      changed.notify_all();
      dispatch.notify_all();
    }
  }

  void schedule(std::size_t task, bool required = false) {
    Slot &slot = slots[task];
    if (slot.queued || slot.running || slot.draft)
      return;
    if (!required && outstanding >= windowSize())
      return;
    slot.queued = true;
    ++outstanding;
    if (required)
      ready.push_front(task);
    else
      ready.push_back(task);
  }

  // The caller participates in transfer execution, rather than becoming an
  // extra busy-waiting coordinator outside the requested worker count.
  std::unique_ptr<Draft> obtain(std::size_t task, FlowSensitivePTA &evaluator) {
    while (true) {
      bool execute = false;
      std::size_t executionTask = task;
      {
        std::unique_lock<std::mutex> lock(mutex);
        if (failure)
          std::rethrow_exception(failure);
        Slot &slot = slots[task];
        if (slot.draft)
          return std::move(slot.draft);
        if (!slot.running) {
          if (owner.config_.parallelMemoize && slot.memo) {
            bool reusable = true;
            for (const auto &entry : slot.memo->reads)
              reusable &= matches(*slots[entry.first].snapshot, entry.second);
            if (reusable) {
              if (!slot.queued)
                schedule(task, true);
              ready.erase(std::find(ready.begin(), ready.end(), task));
              slot.queued = false;
              return replay(task, slot.memo);
            }
          }
          schedule(task, true);
          ready.erase(std::find(ready.begin(), ready.end(), task));
          begin(task);
          execute = true;
        } else if (!ready.empty()) {
          // Help evaluate another prediction while a background worker owns
          // the reference head, instead of leaving the caller idle.
          executionTask = ready.front();
          ready.pop_front();
          begin(executionTask);
          execute = true;
        } else {
          changed.wait(lock, [&] {
            return failure || slots[task].draft != nullptr || owner.timedOut();
          });
          if (!slot.draft && owner.timedOut())
            return nullptr;
        }
      }
      if (execute)
        complete(executionTask, evaluate(executionTask, evaluator));
    }
  }

  bool valid(const Draft &draft) const {
    const auto &reads = draft.replay ? draft.replay->reads : draft.reads;
    for (const auto &entry : reads)
      if (!matches(*slots[entry.first].snapshot, entry.second))
        return false;
    return true;
  }

  void finishWorkers() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopped = true;
    }
    changed.notify_all();
    dispatch.notify_all();
    for (auto &worker : workers)
      if (worker.joinable())
        worker.join();
  }

  void exportSolution() {
    auto store = [&](const StoredSet &set) {
      StoredSet result;
      if (owner.config_.setBackend == PointsToSetBackend::HashConsed)
        result.interned = owner.arena_.intern(points(set));
      else
        result.mutableSet = points(set);
      return result;
    };
    for (const Slot &slot : slots) {
      if (!owner.inScope(slot.node))
        continue;
      const NodeID id = slot.node->getId();
      owner.topLevelPointsTo_[id] = store(slot.snapshot->top);
      MemoryState incoming, outgoing;
      for (const auto &entry : slot.snapshot->in)
        incoming.emplace(entry.first, store(entry.second));
      for (const auto &entry : slot.snapshot->out)
        outgoing.emplace(entry.first, store(entry.second));
      owner.dfIn_[id] = std::move(incoming);
      owner.dfOut_[id] = std::move(outgoing);
    }
  }

public:
  explicit OrderedExecution(FlowSensitivePTA &owner)
      : owner(owner), scc(owner.computeSCCs()) {
    unknownObjects = owner.graph_->getUnknownObjects();
    auto materialized = [&](const StoredSet &set) {
      StoredSet result;
      if (!owner.config_.shareParallelSets)
        result.mutableSet = owner.materialize(set);
      else if (!owner.materialize(set).empty())
        result.sharedSet =
            std::make_shared<const PointsToSet>(owner.materialize(set));
      return result;
    };
    for (const auto &entry : *owner.graph_) {
      auto snapshot = std::make_shared<Snapshot>();
      snapshot->top = materialized(owner.topSet(entry.second));
      for (const auto &fact : owner.inState(entry.second))
        snapshot->in.emplace(fact.first, materialized(fact.second));
      for (const auto &fact : owner.outState(entry.second))
        snapshot->out.emplace(fact.first, materialized(fact.second));
      indices.emplace(entry.first, slots.size());
      slots.push_back(
          {entry.second, std::move(snapshot), nullptr, nullptr, false, false});
    }
    for (const auto &entry : owner.initialMemory_)
      initial.emplace(entry.first, materialized(entry.second));
    threadCount = owner.config_.workerThreads;
    if (!threadCount)
      threadCount = std::max(1U, std::thread::hardware_concurrency());
    threadCount = static_cast<unsigned>(std::min<std::size_t>(
        threadCount, std::max<std::size_t>(1, slots.size())));
    blockSize = static_cast<unsigned>(
        std::min<std::size_t>(std::max(1U, owner.config_.parallelBlockSize),
                              std::max<std::size_t>(1, slots.size())));
  }

  void run() {
    if (scc.components.empty())
      return;
    owner.stats_.parallelThreads = threadCount;
    try {
      for (unsigned i = 1; i < threadCount; ++i)
        workers.emplace_back([&] { worker(); });
      auto evaluator = makeEvaluator();
      std::deque<std::size_t> components;
      std::vector<bool> queued(scc.components.size(), true);
      for (std::size_t i = 0; i < scc.components.size(); ++i)
        components.push_back(i);
      while (!components.empty() && !owner.topologyChanged_ &&
             !owner.timedOut()) {
        if (owner.timeLimitExceeded()) {
          owner.noteTimeout();
          break;
        }
        const std::size_t component = components.front();
        components.pop_front();
        queued[component] = false;
        bool componentChanged = false;
        std::deque<const SVFGNode *> local;
        std::unordered_set<NodeID> locallyQueued;
        for (const SVFGNode *node : scc.components[component]) {
          local.push_back(node);
          locallyQueued.insert(node->getId());
        }
        while (!local.empty() && !owner.topologyChanged_ && !owner.timedOut()) {
          const SVFGNode *node = local.front();
          const std::size_t task = indices.at(node->getId());
          {
            std::lock_guard<std::mutex> lock(mutex);
            schedule(task, true);
            for (const SVFGNode *future : local) {
              if (outstanding >= windowSize())
                break;
              schedule(indices.at(future->getId()));
            }
            for (std::size_t futureComponent : components) {
              if (outstanding >= windowSize())
                break;
              for (const SVFGNode *future : scc.components[futureComponent]) {
                if (outstanding >= windowSize())
                  break;
                schedule(indices.at(future->getId()));
              }
            }
          }
          dispatch.notify_all();
          auto draft = obtain(task, *evaluator);
          if (!draft)
            break;
          bool anyChanged, topChanged;
          {
            std::lock_guard<std::mutex> lock(mutex);
            --outstanding;
            if (!valid(*draft)) {
              ++owner.stats_.parallelConflicts;
              continue;
            }
            ++owner.stats_.parallelCommits;
            if (salvaged(*draft))
              ++owner.stats_.parallelObjectSalvages;
            owner.stats_.parallelAcceptedForwardedReads +=
                draft->forwardedReads;
            const auto &updates = draft->updates;
            for (unsigned channel = 0; channel < ChannelCount; ++channel)
              if (updates[channel])
                ++owner.stats_.parallelPublications;
            anyChanged = updates[Top] || updates[In] || updates[Out];
            topChanged = updates[Top];
            if (anyChanged)
              slots[task].snapshot = draft->result;
            if (draft->replay) {
              ++owner.stats_.parallelMemoHits;
            } else if (owner.config_.parallelMemoize && !anyChanged) {
              auto memo = std::make_shared<Memo>();
              memo->reads = std::move(draft->reads);
              memo->strong = draft->strong;
              memo->weak = draft->weak;
              memo->strongExecutions = draft->strongExecutions;
              memo->weakExecutions = draft->weakExecutions;
              slots[task].memo = std::move(memo);
            } else {
              slots[task].memo.reset();
            }
            if (draft->strong)
              owner.strongUpdateSites_.insert(node->getId());
            if (draft->weak)
              owner.weakUpdateSites_.insert(node->getId());
            owner.stats_.strongUpdateExecutions += draft->strongExecutions;
            owner.stats_.weakUpdateExecutions += draft->weakExecutions;
          }
          local.pop_front();
          locallyQueued.erase(node->getId());
          if (topChanged && !draft->calls.empty()) {
            // Pause dispatch and drain every active evaluator before allowing
            // the connector to modify graph or LLVM metadata.
            {
              std::unique_lock<std::mutex> lock(mutex);
              paused = true;
              changed.wait(lock, [&] {
                return failure || active == 0 || owner.timedOut();
              });
              if (failure)
                std::rethrow_exception(failure);
            }
            for (const auto &call : draft->calls)
              if (owner.config_.connectIndirectCall(call.first, call.second)) {
                ++owner.stats_.indirectCallEdges;
                owner.topologyChanged_ = true;
              }
            {
              std::lock_guard<std::mutex> lock(mutex);
              if (!owner.topologyChanged_)
                paused = false;
            }
            dispatch.notify_all();
          }
          if (owner.topologyChanged_)
            break;
          if (!anyChanged)
            continue;
          componentChanged = true;
          for (const SVFGEdge *edge : node->getOutEdges()) {
            const SVFGNode *successor = edge ? edge->getDstNode() : nullptr;
            if (!owner.inScope(successor) ||
                scc.nodeToComponent.at(successor->getId()) != component ||
                locallyQueued.count(successor->getId()))
              continue;
            local.push_back(successor);
            locallyQueued.insert(successor->getId());
          }
        }
        if (!owner.topologyChanged_ && componentChanged)
          for (std::size_t successor : scc.successors[component])
            if (!queued[successor]) {
              queued[successor] = true;
              components.push_back(successor);
            }
      }
      finishWorkers();
      if (failure)
        std::rethrow_exception(failure);
      exportSolution();
    } catch (...) {
      finishWorkers();
      throw;
    }
  }
};

const FlowSensitivePTA::Statistics &FlowSensitivePTA::solveOrderedParallel() {
  const auto started = std::chrono::steady_clock::now();
  stats_ = {};
  topLevelPointsTo_.clear();
  dfIn_.clear();
  dfOut_.clear();
  initialMemory_.clear();
  strongUpdateSites_.clear();
  weakUpdateSites_.clear();
  if (config_.setBackend == PointsToSetBackend::HashConsed)
    arena_.reset();
  hasDeadline_ = config_.timeLimitSeconds != 0;
  if (hasDeadline_)
    deadline_ = started +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(config_.timeLimitSeconds));
  {
    const auto initializeStarted = std::chrono::steady_clock::now();
    initializeGlobalMemory();
    stats_.initializeSeconds +=
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      initializeStarted)
            .count();
  }
  do {
    if (timeLimitExceeded()) {
      stats_.timedOut = true;
      break;
    }
    ++stats_.topologyEpochs;
    topologyChanged_ = false;
    initializeRecursiveFunctions();
    OrderedExecution(*this).run();
  } while (topologyChanged_ && !stats_.timedOut);
  const auto scc = computeSCCs();
  stats_.sccs = scc.components.size();
  for (const auto &component : scc.components) {
    stats_.nodes += component.size();
    stats_.maxSccSize = std::max(stats_.maxSccSize, component.size());
  }
  for (const auto &entry : topLevelPointsTo_)
    stats_.topLevelFacts += materialize(entry.second).size();
  for (const auto &entry : dfIn_)
    for (const auto &fact : entry.second)
      stats_.memoryInFacts += materialize(fact.second).size();
  for (const auto &entry : dfOut_)
    for (const auto &fact : entry.second)
      stats_.memoryOutFacts += materialize(fact.second).size();
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
