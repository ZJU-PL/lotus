#include "SymbolicExecution/Models/TaintModel.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include "Utils/Formats/cJSON.h"

#include <initializer_list>

using namespace llvm;

namespace SymbolicExecution {

namespace {

void warnInvalidSpec(const Twine &Message) {
  llvm::errs() << "Warning: failed to load bof taint spec: " << Message << "\n";
}

const cJSON *getObjectItemByAliases(const cJSON *Object,
                                    std::initializer_list<const char *> Keys) {
  if (!Object || !cJSON_IsObject(Object)) {
    return nullptr;
  }

  for (const char *Key : Keys) {
    if (const cJSON *Item = cJSON_GetObjectItemCaseSensitive(Object, Key)) {
      return Item;
    }
  }
  return nullptr;
}

std::string getFunctionName(const cJSON *Item) {
  if (!Item || !cJSON_IsObject(Item)) {
    return "";
  }

  if (const cJSON *Name =
          getObjectItemByAliases(Item, {"function", "name", "callee"})) {
    if (cJSON_IsString(Name) && Name->valuestring) {
      return Name->valuestring;
    }
  }

  return "";
}

bool appendIntSpec(const cJSON *Item, std::vector<int> &Out,
                   const Twine &Context) {
  if (!Item) {
    warnInvalidSpec(Context + ": missing integer list");
    return false;
  }

  if (cJSON_IsNumber(Item)) {
    Out.push_back(Item->valueint);
    return true;
  }

  if (!cJSON_IsArray(Item)) {
    warnInvalidSpec(Context + ": expected integer or integer array");
    return false;
  }

  cJSON *Elem = nullptr;
  cJSON_ArrayForEach(Elem, Item) {
    if (!cJSON_IsNumber(Elem)) {
      warnInvalidSpec(Context + ": array contains a non-integer entry");
      return false;
    }
    Out.push_back(Elem->valueint);
  }

  return true;
}

void mergeFunctionSet(const cJSON *Section, std::set<std::string> &Target,
                      const Twine &Context) {
  if (!Section) {
    return;
  }

  if (cJSON_IsArray(Section)) {
    cJSON *Entry = nullptr;
    cJSON_ArrayForEach(Entry, Section) {
      if (cJSON_IsString(Entry) && Entry->valuestring) {
        Target.insert(Entry->valuestring);
        continue;
      }

      std::string Name = getFunctionName(Entry);
      if (!Name.empty()) {
        Target.insert(std::move(Name));
        continue;
      }

      warnInvalidSpec(Context + ": expected function name string or object");
    }
    return;
  }

  if (cJSON_IsObject(Section)) {
    cJSON *Entry = nullptr;
    cJSON_ArrayForEach(Entry, Section) {
      if (!Entry->string) {
        continue;
      }

      if (cJSON_IsFalse(Entry) || cJSON_IsNull(Entry)) {
        continue;
      }

      Target.insert(Entry->string);
    }
    return;
  }

  warnInvalidSpec(Context + ": expected array or object");
}

void mergeFunctionIntMap(
    const cJSON *Section,
    std::unordered_map<std::string, std::vector<int>> &Target,
    const Twine &Context) {
  if (!Section) {
    return;
  }

  if (cJSON_IsObject(Section)) {
    cJSON *Entry = nullptr;
    cJSON_ArrayForEach(Entry, Section) {
      if (!Entry->string) {
        continue;
      }

      std::vector<int> Values;
      if (!appendIntSpec(Entry, Values,
                         Context + " for function '" + Entry->string + "'")) {
        continue;
      }
      Target[Entry->string] = std::move(Values);
    }
    return;
  }

  if (cJSON_IsArray(Section)) {
    cJSON *Entry = nullptr;
    cJSON_ArrayForEach(Entry, Section) {
      std::string Name = getFunctionName(Entry);
      if (Name.empty()) {
        warnInvalidSpec(Context + ": missing function name");
        continue;
      }

      const cJSON *Args =
          getObjectItemByAliases(Entry, {"args", "indices", "arguments"});
      std::vector<int> Values;
      if (!appendIntSpec(Args, Values,
                         Context + " for function '" + Name + "'")) {
        continue;
      }
      Target[Name] = std::move(Values);
    }
    return;
  }

  warnInvalidSpec(Context + ": expected array or object");
}

void mergeTransferSection(const cJSON *Section,
                          std::multimap<std::string, std::vector<int>> &Target,
                          const Twine &Context) {
  auto AddTransfer = [&](const std::string &Name, const cJSON *Spec) {
    if (!Spec) {
      warnInvalidSpec(Context + " for function '" + Name +
                      "': missing transfer spec");
      return;
    }

    if (cJSON_IsObject(Spec)) {
      const cJSON *Src =
          getObjectItemByAliases(Spec, {"src", "source", "from"});
      const cJSON *Dsts =
          getObjectItemByAliases(Spec, {"dst", "dests", "destinations", "to"});
      if (!Src || !Dsts) {
        warnInvalidSpec(Context + " for function '" + Name +
                        "': missing src/dst fields");
        return;
      }

      std::vector<int> Mapping;
      if (!appendIntSpec(Src, Mapping,
                         Context + " for function '" + Name + "' source") ||
          Mapping.size() != 1) {
        warnInvalidSpec(Context + " for function '" + Name +
                        "': source must be a single integer");
        return;
      }

      std::vector<int> DstsVec;
      if (!appendIntSpec(Dsts, DstsVec,
                         Context + " for function '" + Name +
                             "' destinations")) {
        return;
      }
      Mapping.insert(Mapping.end(), DstsVec.begin(), DstsVec.end());
      if (Mapping.size() < 2) {
        warnInvalidSpec(Context + " for function '" + Name +
                        "': transfer needs source and destination");
        return;
      }
      Target.emplace(Name, std::move(Mapping));
      return;
    }

    if (cJSON_IsArray(Spec)) {
      cJSON *Elem = cJSON_GetArrayItem(Spec, 0);
      if (Elem && cJSON_IsArray(Elem)) {
        cJSON *MappingSpec = nullptr;
        cJSON_ArrayForEach(MappingSpec, Spec) {
          std::vector<int> Mapping;
          if (!appendIntSpec(MappingSpec, Mapping,
                             Context + " for function '" + Name + "'")) {
            continue;
          }
          if (Mapping.size() < 2) {
            warnInvalidSpec(Context + " for function '" + Name +
                            "': transfer needs source and destination");
            continue;
          }
          Target.emplace(Name, std::move(Mapping));
        }
        return;
      }

      std::vector<int> Mapping;
      if (!appendIntSpec(Spec, Mapping,
                         Context + " for function '" + Name + "'")) {
        return;
      }
      if (Mapping.size() < 2) {
        warnInvalidSpec(Context + " for function '" + Name +
                        "': transfer needs source and destination");
        return;
      }
      Target.emplace(Name, std::move(Mapping));
      return;
    }

    warnInvalidSpec(Context + " for function '" + Name +
                    "': expected object or array");
  };

  if (!Section) {
    return;
  }

  if (cJSON_IsObject(Section)) {
    cJSON *Entry = nullptr;
    cJSON_ArrayForEach(Entry, Section) {
      if (!Entry->string) {
        continue;
      }
      AddTransfer(Entry->string, Entry);
    }
    return;
  }

  if (cJSON_IsArray(Section)) {
    cJSON *Entry = nullptr;
    cJSON_ArrayForEach(Entry, Section) {
      std::string Name = getFunctionName(Entry);
      if (Name.empty()) {
        warnInvalidSpec(Context + ": missing function name");
        continue;
      }

      const cJSON *Mappings = getObjectItemByAliases(
          Entry, {"mappings", "mapping", "transfers", "transfer", "rules"});
      AddTransfer(Name, Mappings ? Mappings : Entry);
    }
    return;
  }

  warnInvalidSpec(Context + ": expected array or object");
}

bool loadExternalTaintSpec(
    const std::string &Path, std::set<std::string> &RetAsSourceFunctions,
    std::unordered_map<std::string, std::vector<int>> &ArgAsSourceFunctions,
    std::multimap<std::string, std::vector<int>> &DataTransferFunctions,
    std::unordered_map<std::string, std::vector<int>> &SinkFunctions) {
  auto BufferOrErr = llvm::MemoryBuffer::getFile(Path);
  if (!BufferOrErr) {
    warnInvalidSpec("cannot open '" + Path +
                    "': " + BufferOrErr.getError().message());
    return false;
  }

  std::string Json = BufferOrErr.get()->getBuffer().str();
  cJSON *Root = cJSON_Parse(Json.c_str());
  if (!Root) {
    const char *ErrorPtr = cJSON_GetErrorPtr();
    warnInvalidSpec("invalid JSON in '" + Path + "'" +
                    (ErrorPtr ? (Twine(" near: ") + ErrorPtr) : Twine("")));
    return false;
  }

  if (!cJSON_IsObject(Root)) {
    warnInvalidSpec("top-level value in '" + Path + "' must be an object");
    cJSON_Delete(Root);
    return false;
  }

  mergeFunctionSet(
      getObjectItemByAliases(Root, {"ret_as_source", "return_as_source",
                                    "ret_sources", "return_sources"}),
      RetAsSourceFunctions, "return-source section");
  mergeFunctionIntMap(
      getObjectItemByAliases(Root,
                             {"arg_as_source", "arg_sources", "source_args"}),
      ArgAsSourceFunctions, "argument-source section");
  mergeFunctionIntMap(getObjectItemByAliases(Root, {"sinks", "sink_functions"}),
                      SinkFunctions, "sink section");
  mergeTransferSection(
      getObjectItemByAliases(
          Root, {"transfers", "transfer_functions", "propagations"}),
      DataTransferFunctions, "transfer section");

  if (const cJSON *Sources = getObjectItemByAliases(Root, {"sources"})) {
    if (cJSON_IsObject(Sources)) {
      mergeFunctionSet(
          getObjectItemByAliases(
              Sources, {"ret", "return", "ret_as_source", "return_as_source"}),
          RetAsSourceFunctions, "sources.return section");
      mergeFunctionIntMap(
          getObjectItemByAliases(Sources,
                                 {"arg", "args", "arg_as_source", "arguments"}),
          ArgAsSourceFunctions, "sources.argument section");

      cJSON *Entry = nullptr;
      cJSON_ArrayForEach(Entry, Sources) {
        if (!Entry->string || !cJSON_IsObject(Entry)) {
          continue;
        }

        if (const cJSON *Ret =
                getObjectItemByAliases(Entry, {"ret", "return", "retval"})) {
          if (cJSON_IsTrue(Ret)) {
            RetAsSourceFunctions.insert(Entry->string);
          }
        }

        if (const cJSON *Args = getObjectItemByAliases(
                Entry, {"args", "indices", "arguments"})) {
          std::vector<int> Values;
          if (appendIntSpec(Args, Values,
                            Twine("sources section for function '") +
                                Entry->string + "'")) {
            ArgAsSourceFunctions[Entry->string] = std::move(Values);
          }
        }
      }
    } else if (cJSON_IsArray(Sources)) {
      cJSON *Entry = nullptr;
      cJSON_ArrayForEach(Entry, Sources) {
        std::string Name = getFunctionName(Entry);
        if (Name.empty()) {
          warnInvalidSpec("sources section: missing function name");
          continue;
        }

        if (const cJSON *Ret =
                getObjectItemByAliases(Entry, {"ret", "return", "retval"})) {
          if (cJSON_IsTrue(Ret)) {
            RetAsSourceFunctions.insert(Name);
          }
        }

        if (const cJSON *Args = getObjectItemByAliases(
                Entry, {"args", "indices", "arguments"})) {
          std::vector<int> Values;
          if (appendIntSpec(Args, Values,
                            Twine("sources section for function '") + Name +
                                "'")) {
            ArgAsSourceFunctions[Name] = std::move(Values);
          }
        }
      }
    } else {
      warnInvalidSpec("sources section: expected array or object");
    }
  }

  cJSON_Delete(Root);
  return true;
}

} // namespace

static cl::opt<std::string>
    bof_taint_spec("bof-taint-spec",
                   cl::desc("The path to the taint specification used for "
                            "checking buffer overflow."),
                   cl::Optional, cl::init(""));

TaintModel::TaintModel() {
  // Hard-coded specs
  RetAsSourceFunctions = {
      // Character Input Functions
      "fgetc",
      "getc",
      "getchar",
      "fgetc_unlocked",
      "getc_unlocked",
      "getchar_unlocked",
      "fgetwc",
      "getwc",
      "getwchar",
      "fgetwc_unlocked",
      "getwc_unlocked",
      "getwchar_unlocked",
      "getw",
      "_IO_getc",

      // Line Input Functions
      "fgets",
      "gets",
      "fgets_unlocked",
      "fgetws",
      "fgetws_unlocked",
      "fgetln",
      "fgetline",

      // Working Directory Functions
      "getcwd",
      "getwd",
      "get_current_dir_name",
      "g_get_current_dir",

      // Password Functions
      "getpass",

      // Environment Variables Functions
      "getenv",
      "wgetenv",
      "curl_getenv",
      "g_getenv",
      "g_get_home_dir",
      "g_get_tmp_dir",
      "_wgetenv",
  };

  ArgAsSourceFunctions = {
      // Line Input Functions
      {"fgets", {0}},
      {"gets", {0}},
      {"fgets_unlocked", {0}},
      {"fgetws", {0}},
      {"fgetws_unlocked", {0}},
      {"getline", {0}},
      {"getdelim", {0}},
      {"__getdelim", {0}},

      // Formatted Input Functions
      {"__isoc99_scanf", {-1}},
      {"scanf", {-1}},
      {"fscanf", {-2}},
      {"vscanf", {1}},
      {"vfscanf", {2}},
      {"wscanf", {-1}},
      {"fwscanf", {-2}},

      // Primitive Input Functions
      {"read", {1}},
      {"pread", {1}},
      {"pread64", {1}},
      {"readv", {1}},
      {"preadv", {1}},
      {"fread", {0}},
      {"fread", {0}},
      {"fread_unlocked", {0}},
      {"aio_read", {0}},

      // Working Directory Functions
      {"getcwd", {0}},
      {"getwd", {0}},

      // Symbolc Links Functions
      {"readlink", {1}},

      // Network Input Functions
      {"recvfrom", {1}},
      {"recv", {1}},
      {"recvmsg", {1}},
  };

  SinkFunctions = {
      // Format String Vulnerabilities
      {"printf", {0}},
      {"sprintf", {1}},
      {"fprintf", {0, 1}},
      {"snprintf", {1, 2}},
      {"asprintf", {1}},
      {"dprintf", {0, 1}},
      {"vprintf", {0}},
      {"vsprintf", {1}},
      {"vfprintf", {0, 1}},
      {"vsnprintf", {1, 2}},
      {"vasprintf", {1}},
      {"wprintf", {0}},
      {"swprintf", {1}},
      {"fwprintf", {0, 1}},
      {"snwprintf", {1, 2}},
      {"vwprintf", {0}},
      {"vswprintf", {1}},
      {"vfwprintf", {0, 1}},
      {"obstack_printf", {0}},
      {"obstack_vprintf", {0}},
      {"setproctitle", {0}},
      {"syslog", {1}},
      {"vsyslog", {1}},

      // Command Injection Vulnerabilities
      {"execl", {0, -1}},
      {"execlp", {0, -1}},
      {"execle", {0, -1}},
      {"execlpe", {0, -1}},
      {"wexecl", {0, -1}},
      {"wexeclp", {0, -1}},
      {"wexecle", {0, -1}},
      {"wexeclpe", {0, -1}},

      {"execv", {0, 1}},
      {"execve", {0, 1, 2}},
      {"execvp", {0, 1}},
      {"execvpe", {0, 1, 2}},
      {"fexecve", {0, 1, 2}},
      {"wexecv", {0, 1}},
      {"wexecve", {0, 1, 2}},
      {"wexecvp", {0, 1}},
      {"wexecvpe", {0, 1, 2}},

      {"system", {0}},
      {"popen", {0}},
      {"_wsystem", {0}},
      {"wpopen", {0}},

      {"_spawnl", {-1}},
      {"_spawnle", {-1}},
      {"_spawnlp", {-1}},
      {"_spawnlpe", {-1}},
      {"_wspawnl", {-1}},
      {"_wspawnle", {-1}},
      {"_wspawnlp", {-1}},
      {"_wspawnlpe", {-1}},

      {"_spawnv", {1, 2}},
      {"_spawnve", {1, 2, 3}},
      {"_spawnvp", {1, 2}},
      {"_spawnvpe", {1, 2, 3}},
      {"_wspawnv", {1, 2}},
      {"_wspawnve", {1, 2, 3}},
      {"_wspawnvp", {1, 2}},
      {"_wspawnvpe", {1, 2, 3}},

      {"WinExe", {0}},
      {"ShellExecute", {0}},
      {"ShellExecuteA", {0}},
      {"ShellExecuteW", {0}},
      {"ShellExecuteEx", {0}},
      {"ShellExecuteExA", {0}},
      {"ShellExecuteExW", {0}},

      // String Manipulation Vulnerabilities
      {"strcpy", {1}},
      {"stpcpy", {1}},
      {"strcat", {1}},
      {"wcscpy", {1}},
      {"wcpcpy", {1}},
      {"wcscat", {1}},
      {"strccpy", {1}},
      {"strcadd", {1}},
      {"strecpy", {1}},
      {"streadd", {1}},

      {"strlcat", {1, 2}},
      {"strlcpy", {1, 2}},
      {"strncpy", {1, 2}},
      {"stpncpy", {1, 2}},
      {"strncat", {1, 2}},
      {"wcsncpy", {1, 2}},
      {"wcsncat", {1, 2}},
      {"wcpncpy", {1, 2}},
      {"strxfrm", {1, 2}},

      // Memory Allocation Vulnerabilities

      // Memory Copy Vulnerabilities
      {"memcpy", {1, 2}},
      {"mempcpy", {1, 2}},
      {"memmove", {1, 2}},
      {"memset", {1, 2}},
      {"memccpy", {1, 2, 3}},
      {"wmemcpy", {1, 2}},
      {"wmempcpy", {1, 2}},
      {"wmemmove", {1, 2}},
      {"wmemset", {1, 2}},
      {"bcopy", {0, 2}},
      {"CopyMemory", {1, 2}},
      {"MoveMemory", {1, 2}},
      // -> llvm intrinsics

      // Configuration Vulnerabilities
      {"SetComputerName", {0}},
      {"SetComputeNameA", {0}},
      {"SetComputeNameW", {0}},
      {"sethostid", {0}},

      // Path Traversal Vulnerabilities
      {"open", {0}},
      {"fopen", {0}},
      {"fdopen", {0}},
      {"freopen", {0}},
      {"wopen", {0}},
      {"CreateFile", {0}},
      {"CreateFileA", {0}},
      {"CreateFileW", {0}},
      {"remove", {0}},
      {"rename", {0}},
      {"_ZNSt14basic_ifstreamIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode",
       {0}}, // ifstream's open
      {"_ZNSt14basic_ofstreamIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode",
       {0}}, // ofstream's open

      // Process Control Vulnerabilities
      {"dlopen", {0}},
      {"LoadLibrary", {0}},
      {"LoadLibraryW", {0}},
      {"LoadLibraryA", {0}},
      {"LoadLibraryEx", {0}},
      {"LoadLibraryExA", {0}},
      {"LoadLibraryExW", {0}},
      {"AfxLoadLibrary", {0}},

      // Search Path (Env) Vulnerabilties
      {"putenv", {0}},
      {"wputenv", {0}},
      {"setenv", {0, 1, 2}},

      // Data Leak Vulnerabilities
      {"write", {0, -1}},
      {"fwrite", {0, -1}},
      {"pwrite", {0, -1}},
      {"writev", {0, -1}},
      {"puts", {0}},
      {"fputs", {0, 1}},
      {"send", {0, -1}},
      {"sendto", {0, -1}},
      {"sendmsg", {0, -1}},
  };

  if (!bof_taint_spec.empty()) {
    loadExternalTaintSpec(bof_taint_spec, RetAsSourceFunctions,
                          ArgAsSourceFunctions, DataTransferFunctions,
                          SinkFunctions);
  }
}

void TaintModel::getTransferDstVect(const CallBase *CS, Value *Arg,
                                    std::vector<Value *> &DstVect) const {
  if (!CS) {
    return;
  }

  Function *Callee = CS->getCalledFunction();
  if (!Callee) {
    return;
  }

  std::string callee_name = Callee->getName().str();
  auto IterPair = DataTransferFunctions.equal_range(callee_name);
  for (auto It = IterPair.first; It != IterPair.second; ++It) {
    auto *Vec = &It->second;
    if (Vec->size() < 2) {
      continue;
    }

    unsigned SrcIdx = (unsigned)Vec->at(0);
    if (SrcIdx >= CS->arg_size()) {
      continue;
    }

    if (CS->getArgOperand(SrcIdx) != Arg) {
      continue;
    }

    bool Match = true;
    for (unsigned I = 1; I < Vec->size(); ++I) {
      int DstIdx = Vec->at(I);
      if (DstIdx >= 0 && (unsigned)DstIdx >= CS->arg_size()) {
        Match = false;
        break;
      }
    }

    if (!Match) {
      continue;
    }

    for (unsigned I = 1; I < Vec->size(); ++I) {
      int DstIdx = Vec->at(I);
      if (DstIdx == -1) { // ret
        DstVect.push_back(const_cast<CallBase *>(CS));
      } else if ((unsigned)DstIdx < CS->arg_size()) {
        DstVect.push_back(CS->getArgOperand((unsigned)DstIdx));
      }
    }
  }

  std::set<Value *> Tmp(DstVect.begin(), DstVect.end());
  DstVect.clear();
  DstVect.insert(DstVect.end(), Tmp.begin(), Tmp.end());
}

bool TaintModel::isFunctionRetAsSource(const Function *func) const {
  if (!func)
    return false;

  return RetAsSourceFunctions.count(func->getName().str()) != 0;
}

bool TaintModel::isFunctionArgAsSource(const Function *func) const {
  if (!func)
    return false;

  return ArgAsSourceFunctions.count(func->getName().str()) != 0;
}

const std::vector<int> *
TaintModel::getTaintSourceArguments(Function *func) const {
  if (!func)
    return nullptr;

  auto It = ArgAsSourceFunctions.find(func->getName().str());
  if (It != ArgAsSourceFunctions.end()) {
    return &It->second;
  }
  return nullptr;
}

bool TaintModel::isSinkFunction(const Function *Func) const {
  if (!Func)
    return false;
  return SinkFunctions.count(Func->getName().str()) != 0;
}

const std::vector<int> *
TaintModel::getSinkArguments(const Function *Func) const {
  if (!Func)
    return nullptr;
  auto It = SinkFunctions.find(Func->getName().str());
  return It != SinkFunctions.end() ? &It->second : nullptr;
}

std::set<Var>
TaintModel::getTaintedVars(Instruction *I,
                           lotus::gvfg::GuardedValueFlowGraph *Graph) const {
  std::set<Var> Result;
  if (!I || !Graph) {
    return Result;
  }

  auto *CB = dyn_cast<CallBase>(I);
  if (!CB) {
    return Result;
  }

  Function *Callee = CB->getCalledFunction();
  if (!Callee) {
    return Result;
  }

  auto AddValue = [&](Value *V) {
    if (!V) {
      return;
    }
    V = V->stripPointerCasts();
    if (auto *Node = Graph->findNode(V)) {
      Result.insert(Var(Node));
    }
  };

  if (!CB->getType()->isVoidTy() && isFunctionRetAsSource(Callee)) {
    AddValue(CB);
  }

  const std::vector<int> *SourceArgs = getTaintSourceArguments(Callee);
  if (!SourceArgs) {
    return Result;
  }

  for (int ArgIdx : *SourceArgs) {
    if (ArgIdx >= 0) {
      if (static_cast<unsigned>(ArgIdx) < CB->arg_size()) {
        AddValue(CB->getArgOperand(static_cast<unsigned>(ArgIdx)));
      }
      continue;
    }

    auto FirstVariadicIdx = static_cast<unsigned>(-ArgIdx);
    for (unsigned Idx = FirstVariadicIdx; Idx < CB->arg_size(); ++Idx) {
      AddValue(CB->getArgOperand(Idx));
    }
  }

  return Result;
}

} // namespace SymbolicExecution
