#include "Concurrency/CUDA/CUDAFunctionSummary.h"

#include "Concurrency/API/ThreadAPI.h"

#include <optional>
#include <unordered_map>
#include <unordered_set>

#include <llvm/ADT/STLExtras.h>
#include <llvm/Analysis/PostDominators.h>

namespace concurrency::cuda {

namespace {

bool isCUDACall(ThreadAPI::TD_TYPE type) { return type != ThreadAPI::TD_DUMMY; }

std::optional<CUDAEffectClass> getEffectClass(ThreadAPI::TD_TYPE type,
                                              const llvm::CallBase *call) {
  const llvm::Function *callee = call ? call->getCalledFunction() : nullptr;
  const llvm::StringRef name = callee ? callee->getName() : llvm::StringRef{};
  if ((type == ThreadAPI::TD_CUDA_STREAM || type == ThreadAPI::TD_CUDA_EVENT) &&
      (name.contains("Synchronize") || name.contains("WaitEvent"))) {
    return CUDAEffectClass::Synchronization;
  }
  switch (type) {
  case ThreadAPI::TD_CUDA_KERNEL_LAUNCH:
    return CUDAEffectClass::KernelLaunch;
  case ThreadAPI::TD_CUDA_MEMCPY:
  case ThreadAPI::TD_CUDA_MEMSET:
  case ThreadAPI::TD_CUDA_MALLOC:
  case ThreadAPI::TD_CUDA_FREE:
  case ThreadAPI::TD_CUDA_UNIFIED_MEMORY:
    return CUDAEffectClass::MemoryTransfer;
  case ThreadAPI::TD_CUDA_DEVICE_SYNC:
  case ThreadAPI::TD_CUDA_BARRIER:
  case ThreadAPI::TD_CUDA_WARP_BARRIER:
  case ThreadAPI::TD_CUDA_MEMORY_BARRIER:
    return CUDAEffectClass::Synchronization;
  case ThreadAPI::TD_CUDA_ATOMIC:
    return CUDAEffectClass::Atomic;
  case ThreadAPI::TD_CUDA_STREAM:
    return CUDAEffectClass::Stream;
  case ThreadAPI::TD_CUDA_EVENT:
    return CUDAEffectClass::Event;
  case ThreadAPI::TD_CUDA_TEXTURE:
    return CUDAEffectClass::Texture;
  case ThreadAPI::TD_CUDA_SURFACE:
    return CUDAEffectClass::Surface;
  default:
    return std::nullopt;
  }
}

void appendUnique(std::vector<const llvm::CallBase *> &calls,
                  const llvm::CallBase *call) {
  if (call && !llvm::is_contained(calls, call)) {
    calls.push_back(call);
  }
}

bool sameInstantiatedEffect(const CUDAInstantiatedEffect &lhs,
                            const CUDAInstantiatedEffect &rhs) {
  return lhs.effect_class == rhs.effect_class && lhs.origin == rhs.origin &&
         lhs.callsite == rhs.callsite && lhs.bindings == rhs.bindings &&
         lhs.must_execute == rhs.must_execute;
}

void appendUnique(std::vector<CUDAInstantiatedEffect> &effects,
                  CUDAInstantiatedEffect effect) {
  if (!llvm::any_of(effects, [&](const CUDAInstantiatedEffect &existing) {
        return sameInstantiatedEffect(existing, effect);
      })) {
    effects.push_back(std::move(effect));
  }
}

const llvm::Value *substituteValue(
    const llvm::Value *value,
    const std::vector<std::pair<const llvm::Argument *, const llvm::Value *>>
        &arguments) {
  const auto *argument = llvm::dyn_cast_or_null<llvm::Argument>(value);
  if (!argument) {
    return value;
  }
  for (const auto &mapping : arguments) {
    if (mapping.first == argument) {
      return mapping.second;
    }
  }
  return value;
}

} // anonymous namespace

CUDAFunctionSummaryAnalysis::CUDAFunctionSummaryAnalysis(
    const llvm::Module &module)
    : m_module(module) {}

void CUDAFunctionSummaryAnalysis::runAnalysis() {
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  if (!api) {
    return;
  }

  // First pass: collect direct CUDA operations and call-graph edges without
  // eagerly computing PostDominatorTree for functions unrelated to CUDA.
  std::unordered_set<const llvm::Function *> direct_cuda_funcs;
  std::unordered_map<const llvm::Function *,
                     std::vector<const llvm::Function *>>
      reverse_callers;
  for (const auto &fn : m_module.functions()) {
    if (fn.isDeclaration()) {
      continue;
    }
    bool has_direct_cuda = false;
    for (const auto &bb : fn) {
      for (const auto &inst : bb) {
        const auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
        if (!call) {
          continue;
        }
        auto td_type = api->getType(call);
        if (api->getRuntimeLibrary(call) == ThreadAPI::RuntimeLibrary::CUDA &&
            isCUDACall(td_type)) {
          has_direct_cuda = true;
        }
        if (const llvm::Function *callee = api->getCallee(call)) {
          if (!callee->isDeclaration()) {
            reverse_callers[callee].push_back(&fn);
          }
        }
      }
    }
    if (has_direct_cuda) {
      direct_cuda_funcs.insert(&fn);
    }
  }

  std::unordered_set<const llvm::Function *> cuda_relevant_funcs(
      direct_cuda_funcs.begin(), direct_cuda_funcs.end());
  std::vector<const llvm::Function *> rel_worklist(direct_cuda_funcs.begin(),
                                                   direct_cuda_funcs.end());
  for (size_t head = 0; head < rel_worklist.size(); ++head) {
    auto it = reverse_callers.find(rel_worklist[head]);
    if (it == reverse_callers.end()) {
      continue;
    }
    for (const llvm::Function *caller : it->second) {
      if (cuda_relevant_funcs.insert(caller).second) {
        rel_worklist.push_back(caller);
      }
    }
  }

  for (const auto &fn : m_module.functions()) {
    if (fn.isDeclaration()) {
      continue;
    }
    CUDAFunctionSummary summary;
    summary.function = &fn;
    const bool is_relevant = cuda_relevant_funcs.count(&fn) != 0;
    std::unique_ptr<llvm::PostDominatorTree> post_dom_tree;
    if (is_relevant) {
      post_dom_tree = std::make_unique<llvm::PostDominatorTree>();
      post_dom_tree->recalculate(const_cast<llvm::Function &>(fn));
    }

    for (const auto &bb : fn) {
      for (const auto &inst : bb) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
        if (!call) {
          continue;
        }
        if (is_relevant) {
          auto td_type = api->getType(call);
          if (api->getRuntimeLibrary(call) == ThreadAPI::RuntimeLibrary::CUDA &&
              isCUDACall(td_type)) {
            switch (td_type) {
            case ThreadAPI::TD_CUDA_KERNEL_LAUNCH:
              summary.kernel_launches.push_back(call);
              break;
            case ThreadAPI::TD_CUDA_MEMCPY:
            case ThreadAPI::TD_CUDA_MEMSET:
            case ThreadAPI::TD_CUDA_MALLOC:
            case ThreadAPI::TD_CUDA_FREE:
            case ThreadAPI::TD_CUDA_UNIFIED_MEMORY:
              summary.memory_transfers.push_back(call);
              break;
            case ThreadAPI::TD_CUDA_DEVICE_SYNC:
            case ThreadAPI::TD_CUDA_BARRIER:
            case ThreadAPI::TD_CUDA_WARP_BARRIER:
            case ThreadAPI::TD_CUDA_MEMORY_BARRIER:
              summary.synchronizations.push_back(call);
              break;
            case ThreadAPI::TD_CUDA_ATOMIC:
              summary.atomics.push_back(call);
              break;
            case ThreadAPI::TD_CUDA_STREAM:
              summary.stream_ops.push_back(call);
              break;
            case ThreadAPI::TD_CUDA_EVENT:
              summary.event_ops.push_back(call);
              break;
            case ThreadAPI::TD_CUDA_TEXTURE:
              summary.texture_ops.push_back(call);
              break;
            case ThreadAPI::TD_CUDA_SURFACE:
              summary.surface_ops.push_back(call);
              break;
            default:
              break;
            }
            if (auto effect_class = getEffectClass(td_type, call)) {
              CUDAEffectSummary &effect = summary.effects[*effect_class];
              appendUnique(effect.may, call);
              const llvm::BasicBlock *entry = &fn.getEntryBlock();
              const llvm::BasicBlock *call_block = call->getParent();
              if (post_dom_tree->getNode(call_block) &&
                  post_dom_tree->getNode(entry) &&
                  post_dom_tree->dominates(call_block, entry)) {
                appendUnique(effect.must, call);
              }
              CUDAInstantiatedEffect instantiated;
              instantiated.effect_class = *effect_class;
              instantiated.origin = call;
              instantiated.must_execute = llvm::is_contained(effect.must, call);
              for (const llvm::Argument &argument : fn.args()) {
                instantiated.bindings.push_back({&argument, &argument});
              }
              appendUnique(summary.instantiated_effects,
                           std::move(instantiated));
            }
          }
        }

        const llvm::Function *callee = api->getCallee(call);
        if (callee && !callee->isDeclaration()) {
          if (!llvm::is_contained(summary.callees, callee)) {
            summary.callees.push_back(callee);
          }
          if (is_relevant && cuda_relevant_funcs.count(callee) != 0) {
            CUDAFunctionCallsite callsite;
            callsite.callsite = call;
            callsite.callee = callee;
            callsite.must_execute =
                post_dom_tree->getNode(call->getParent()) &&
                post_dom_tree->getNode(&fn.getEntryBlock()) &&
                post_dom_tree->dominates(call->getParent(),
                                         &fn.getEntryBlock());
            unsigned index = 0;
            for (const llvm::Argument &formal : callee->args()) {
              if (index >= call->arg_size()) {
                break;
              }
              callsite.arguments.push_back(
                  {&formal, call->getArgOperand(index++)});
            }
            summary.callsites.push_back(std::move(callsite));
          }
        }
      }
    }

    m_summaries[&fn] = std::move(summary);
  }

  bool changed = !cuda_relevant_funcs.empty();
  size_t iterations = 0;
  constexpr size_t kMaxSummaryIterations = 64;
  while (changed && ++iterations <= kMaxSummaryIterations) {
    changed = false;

    for (const llvm::Function *fn : cuda_relevant_funcs) {
      CUDAFunctionSummary &summary = m_summaries[fn];
      size_t orig_kernel_count = summary.kernel_launches.size();
      size_t orig_transfer_count = summary.memory_transfers.size();
      size_t orig_sync_count = summary.synchronizations.size();
      size_t orig_atomic_count = summary.atomics.size();
      size_t orig_stream_count = summary.stream_ops.size();
      size_t orig_event_count = summary.event_ops.size();
      size_t orig_texture_count = summary.texture_ops.size();
      size_t orig_surface_count = summary.surface_ops.size();
      size_t orig_effect_count = 0;
      for (const auto &effect : summary.effects) {
        orig_effect_count += effect.second.may.size();
        orig_effect_count += effect.second.must.size();
      }
      const size_t orig_instantiated_count =
          summary.instantiated_effects.size();

      for (const llvm::Function *callee : summary.callees) {
        auto it = m_summaries.find(callee);
        if (it == m_summaries.end()) {
          continue;
        }
        const CUDAFunctionSummary &callee_summary = it->second;

        for (const auto *call : callee_summary.kernel_launches) {
          bool already_added = false;
          for (const auto *existing : summary.kernel_launches) {
            if (existing == call) {
              already_added = true;
              break;
            }
          }
          if (!already_added) {
            summary.kernel_launches.push_back(call);
          }
        }

        for (const auto *call : callee_summary.memory_transfers) {
          bool already_added = false;
          for (const auto *existing : summary.memory_transfers) {
            if (existing == call) {
              already_added = true;
              break;
            }
          }
          if (!already_added) {
            summary.memory_transfers.push_back(call);
          }
        }

        for (const auto *call : callee_summary.synchronizations) {
          bool already_added = false;
          for (const auto *existing : summary.synchronizations) {
            if (existing == call) {
              already_added = true;
              break;
            }
          }
          if (!already_added) {
            summary.synchronizations.push_back(call);
          }
        }

        for (const auto *call : callee_summary.atomics) {
          bool already_added = false;
          for (const auto *existing : summary.atomics) {
            if (existing == call) {
              already_added = true;
              break;
            }
          }
          if (!already_added) {
            summary.atomics.push_back(call);
          }
        }

        for (const auto *call : callee_summary.stream_ops) {
          if (!llvm::is_contained(summary.stream_ops, call)) {
            summary.stream_ops.push_back(call);
          }
        }

        for (const auto *call : callee_summary.event_ops) {
          if (!llvm::is_contained(summary.event_ops, call)) {
            summary.event_ops.push_back(call);
          }
        }

        for (const auto *call : callee_summary.texture_ops) {
          if (!llvm::is_contained(summary.texture_ops, call)) {
            summary.texture_ops.push_back(call);
          }
        }

        for (const auto *call : callee_summary.surface_ops) {
          if (!llvm::is_contained(summary.surface_ops, call)) {
            summary.surface_ops.push_back(call);
          }
        }

        if (callee_summary.recursive) {
          summary.recursive = true;
        }
      }

      for (const CUDAFunctionCallsite &callsite : summary.callsites) {
        auto callee_it = m_summaries.find(callsite.callee);
        if (callee_it == m_summaries.end()) {
          continue;
        }
        for (const auto &effect_pair : callee_it->second.effects) {
          CUDAEffectSummary &effect = summary.effects[effect_pair.first];
          for (const llvm::CallBase *call : effect_pair.second.may) {
            appendUnique(effect.may, call);
          }
          if (callsite.must_execute) {
            for (const llvm::CallBase *call : effect_pair.second.must) {
              appendUnique(effect.must, call);
            }
          }
        }
        for (const CUDAInstantiatedEffect &callee_effect :
             callee_it->second.instantiated_effects) {
          CUDAInstantiatedEffect instantiated = callee_effect;
          instantiated.callsite = callsite.callsite;
          instantiated.must_execute =
              callsite.must_execute && callee_effect.must_execute;
          for (auto &binding : instantiated.bindings) {
            binding.second =
                substituteValue(binding.second, callsite.arguments);
          }
          appendUnique(summary.instantiated_effects, std::move(instantiated));
        }
      }

      size_t new_effect_count = 0;
      for (const auto &effect : summary.effects) {
        new_effect_count += effect.second.may.size();
        new_effect_count += effect.second.must.size();
      }

      if (summary.kernel_launches.size() != orig_kernel_count ||
          summary.memory_transfers.size() != orig_transfer_count ||
          summary.synchronizations.size() != orig_sync_count ||
          summary.atomics.size() != orig_atomic_count ||
          summary.stream_ops.size() != orig_stream_count ||
          summary.event_ops.size() != orig_event_count ||
          summary.texture_ops.size() != orig_texture_count ||
          summary.surface_ops.size() != orig_surface_count ||
          new_effect_count != orig_effect_count ||
          summary.instantiated_effects.size() != orig_instantiated_count) {
        changed = true;
      }
    }
  }

  // O(V + E) cycle reachability detection using memoized 3-color DFS instead
  // of exponential all-simple-paths backtracking.
  // State: 0 = unvisited, 1 = on stack (gray), 2 = acyclic (black),
  //        3 = reaches a cycle.
  std::unordered_map<const llvm::Function *, uint8_t> color;
  color.reserve(m_summaries.size());
  std::function<bool(const llvm::Function *)> reaches_cycle =
      [&](const llvm::Function *fn) -> bool {
    auto col_it = color.find(fn);
    if (col_it != color.end()) {
      if (col_it->second == 1 || col_it->second == 3) {
        return true;
      }
      return false;
    }
    color[fn] = 1;
    bool has_cycle = false;
    auto it = m_summaries.find(fn);
    if (it != m_summaries.end()) {
      for (const llvm::Function *callee : it->second.callees) {
        if (!callee || callee->isDeclaration()) {
          continue;
        }
        if (reaches_cycle(callee)) {
          has_cycle = true;
        }
      }
    }
    color[fn] = has_cycle ? 3 : 2;
    return has_cycle;
  };

  for (auto &pair : m_summaries) {
    pair.second.recursive = reaches_cycle(pair.first);
  }

  for (auto &pair : m_summaries) {
    pair.second.reaches_fixed_point = true;
  }
}

const CUDAFunctionSummary *
CUDAFunctionSummaryAnalysis::getSummary(const llvm::Function *fn) const {
  auto it = m_summaries.find(fn);
  if (it != m_summaries.end()) {
    return &it->second;
  }
  return nullptr;
}

} // namespace concurrency::cuda
