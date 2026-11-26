#include "SymbolicExecution/Models/MemoryAPI.h"

#include "SymbolicExecution/Support/AnalysisContext.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CommandLine.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_set>

using namespace llvm;

namespace SymbolicExecution {

namespace {

cl::opt<bool>
    KernelAnalysis("symex-analyze-kernel",
                   cl::desc("run the bof analysis for Linux kernel"),
                   cl::init(false), cl::ReallyHidden);

const AllocatorAPI LibcAllocators[] = {
    AllocatorAPI("malloc", {0}),
    AllocatorAPI("calloc", {0, 1}, true, -1, true),
    AllocatorAPI("realloc", {1}),
    AllocatorAPI("memalign", {1}),
    AllocatorAPI("aligned_alloc", {1}),
    AllocatorAPI("pvalloc", {0}),
    AllocatorAPI("valloc", {0}),
    AllocatorAPI("strndup", {1}),
    AllocatorAPI("_Znwj", {0}),
    AllocatorAPI("_ZnwjRKSt9nothrow_t", {0}),
    AllocatorAPI("_Znwm", {0}),
    AllocatorAPI("_ZnwmRKSt9nothrow_t", {0}),
    AllocatorAPI("_Znaj", {0}),
    AllocatorAPI("_ZnajRKSt9nothrow_t", {0}),
    AllocatorAPI("_Znam", {0}),
    AllocatorAPI("_ZnamRKSt9nothrow_t", {0}),
};

const AllocatorAPI KernelAllocators[] = {
    AllocatorAPI("kmalloc", {0}, false, 1),
    AllocatorAPI("kzalloc", {0}, false, 1, true),
    AllocatorAPI("devm_kmalloc", {1}, false, 2),
    AllocatorAPI("devm_kzalloc", {1}, false, 2, true),
    AllocatorAPI("kmalloc_array", {0, 1}, true, 2),
    AllocatorAPI("kcalloc", {0, 1}, true, 2, true),
    AllocatorAPI("krealloc", {1}, false, 2),
    AllocatorAPI("krealloc_array", {1, 2}, true, 3),
    AllocatorAPI("kmalloc_node", {0}, false, 1),
    AllocatorAPI("__kmalloc_node", {0}, false, 1),
    AllocatorAPI("kmalloc_array_node", {0, 1}, true, 2),
    AllocatorAPI("kcalloc_node", {0, 1}, true, 2, true),
    AllocatorAPI("kzalloc_node", {0}, false, 1, true),
    AllocatorAPI("kvmalloc", {0}, false, 1),
    AllocatorAPI("kvmalloc_node", {0}, false, 1),
    AllocatorAPI("kvzalloc_node", {0}, false, 1),
    AllocatorAPI("kvzalloc", {0}, false, 1),
    AllocatorAPI("kvmalloc_array", {0, 1}, true, 2),
    AllocatorAPI("kvcalloc", {0, 1}, true, 2, true),
    AllocatorAPI("kvrealloc", {2}, false, 3),
};

} // namespace

std::string AllocatorAPI::toString() const {
  std::string Desc = "alloc(size=";
  if (isArrayAlloc()) {
    Desc += "arg" + std::to_string(SizeArgs[0]);
    Desc += "*";
    Desc += "arg" + std::to_string(SizeArgs[1]);
  } else {
    Desc += "arg" + std::to_string(SizeArgs[0]);
  }

  if (ArrayAllocation) {
    Desc += ", array";
  }
  if (hasValidGFPFlag()) {
    Desc += ", gfp=arg" + std::to_string(GFPFlag);
  }
  if (Zeroed) {
    Desc += ", zeroed";
  }

  Desc += ")";
  return Desc;
}

const AllocatorAPI *AllocatorAPI::get(Function *F) {
  if (!F || !F->getReturnType()->isPointerTy()) {
    return nullptr;
  }

  std::string FunName = F->getName().str();
  std::vector<Argument *> Params;
  for (auto &V : F->args()) {
    Params.emplace_back(&V);
  }

  auto CheckIntIdx = [&Params](unsigned Idx) {
    return Idx < Params.size() && Params[Idx]->getType()->isIntegerTy();
  };

  const auto *APIBegin = KernelAnalysis ? std::begin(KernelAllocators)
                                        : std::begin(LibcAllocators);
  const auto *APIEnd =
      KernelAnalysis ? std::end(KernelAllocators) : std::end(LibcAllocators);

  for (const auto *Iter = APIBegin; Iter != APIEnd; ++Iter) {
    const auto &API = *Iter;
    const std::string &APIName = API.getName();
    bool Match = false;

    if (KernelAnalysis) {
      if (APIName == FunName || ("__" + APIName == FunName)) {
        Match = true;
      } else if (FunName.rfind(APIName, 0) == 0) {
        if (std::find_if(FunName.begin() + APIName.size(), FunName.end(),
                         [](unsigned char C) { return !std::isdigit(C); }) ==
            FunName.end()) {
          Match = true;
        }
      }
    } else {
      Match = (FunName.find(APIName) != std::string::npos);
    }

    if (Match) {
      for (auto Idx : API.getSizeArgs()) {
        if (!CheckIntIdx(static_cast<unsigned>(Idx))) {
          Match = false;
          break;
        }
      }

      if (Match && API.hasValidGFPFlag()) {
        Match = CheckIntIdx(static_cast<unsigned>(API.getGFPFlag()));
      }
    }

    if (Match) {
      return &API;
    }
  }

  return nullptr;
}

std::vector<int> getHeapAllocSizeArgs(const CallBase *CB) {
  const auto *API = AllocatorAPI::get(getDirectCallee(CB));
  return API ? API->getSizeArgs() : std::vector<int>{};
}

bool isFreeLikeFunction(const Function *F) {
  if (!F)
    return false;
  StringRef Name = F->getName();
  return Name == "free" || Name == "_ZdlPv" || Name == "_ZdaPv";
}

bool isKnownLib(const std::string &LibName) {
  static const std::unordered_set<std::string> Known = []() {
    std::unordered_set<std::string> Names = {
        "memset",   "snprintf",  "memcpy",     "memmove", "fread",    "fwrite",
        "memchr",   "strchr",    "strcpy",     "puts",    "strncpy",  "strncmp",
        "strstr",   "strcasestr", "strcmp",    "strtok",  "strlen",   "recv",
        "recvfrom", "read",      "readlink",   "pathconf", "strnlen", "strncat",
        "strcat",   "free",      "_ZdlPv",     "_ZdaPv"};
    for (const auto &API : LibcAllocators) {
      Names.insert(API.getName());
    }
    for (const auto &API : KernelAllocators) {
      Names.insert(API.getName());
    }
    return Names;
  }();

  static const std::array<std::string, 3> KnownPrefixes = {
      "llvm.memset", "llvm.memcpy", "llvm.memmove"};

  if (Known.count(LibName) != 0)
    return true;

  return llvm::any_of(KnownPrefixes, [&](const std::string &Prefix) {
    return LibName.rfind(Prefix, 0) == 0;
  });
}

} // namespace SymbolicExecution
