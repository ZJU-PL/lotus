#include "Alias/InclusionBased/FlowSensitive/ValueFlow/ValueFlowPTA.h"

#include "Alias/Infrastructure/Spec/AliasSpecManager.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Casting.h>

namespace lotus::alias {
using namespace llvm;
using vfg::AccessID;
using vfg::BlockID;
using vfg::CallSiteID;
using vfg::FunctionID;
using vfg::NodeID;

namespace {

const Function *directCallee(const CallBase &call) {
  const Value *value = call.getCalledOperand()->stripPointerCasts();
  if (const auto *fn = dyn_cast<Function>(value))
    return fn;
  if (!isa<GlobalAlias>(value))
    return nullptr;
  SmallPtrSet<const Value *, 8> seen;
  while (const auto *alias = dyn_cast<GlobalAlias>(value)) {
    if (!seen.insert(value).second)
      return nullptr;
    value = alias->getAliasee()->stripPointerCasts();
  }
  return dyn_cast<Function>(value);
}

bool isAnnotationFunction(const Function *function) {
  if (!function || !function->isDeclaration())
    return false;
  const StringRef name = function->getName();
  return name == "__aser_alias__" || name == "__aser_no_alias__" ||
         name == "MAYALIAS" || name == "NOALIAS" || name == "MUSTALIAS" ||
         name == "PARTIALALIAS" || name == "EXPECTEDFAIL_MAYALIAS" ||
         name == "EXPECTEDFAIL_NOALIAS" || name == "svf_assert" ||
         name == "__VERIFIER_assert";
}

std::optional<AllocatorInfo> builtinAllocatorInfo(const Function *function) {
  if (!function || !function->isDeclaration())
    return std::nullopt;
  const StringRef name = function->getName();
  if (name == "malloc" || name == "aligned_alloc" || name == "valloc" ||
      name == "pvalloc" || name == "memalign" || name == "strdup" ||
      name == "strndup" || name == "fopen" || name == "fopen64" ||
      name == "fdopen" || name == "tmpfile" || name == "tmpfile64" ||
      name == "opendir" || name == "popen" || name == "mmap" ||
      name == "mmap64" || name == "__cxa_allocate_exception" ||
      name == "_Znwm" || name == "_Znam" || name == "_Znwj" ||
      name == "_Znaj" || name == "_ZnwmRKSt9nothrow_t" ||
      name == "_ZnamRKSt9nothrow_t" || name == "_ZnwjRKSt9nothrow_t" ||
      name == "_ZnajRKSt9nothrow_t" || name == "_ZnwmSt11align_val_t" ||
      name == "_ZnamSt11align_val_t" || name == "_ZnwjSt11align_val_t" ||
      name == "_ZnajSt11align_val_t" ||
      name == "_ZnwmSt11align_val_tRKSt9nothrow_t" ||
      name == "_ZnamSt11align_val_tRKSt9nothrow_t" ||
      name == "_ZnwjSt11align_val_tRKSt9nothrow_t" ||
      name == "_ZnajSt11align_val_tRKSt9nothrow_t") {
    AllocatorInfo info;
    info.functionName = name.str();
    info.returnsPointer = true;
    info.initializesToZero = false;
    return info;
  }
  if (name == "calloc") {
    AllocatorInfo info;
    info.functionName = name.str();
    info.returnsPointer = true;
    info.initializesToZero = true;
    return info;
  }
  if (name == "realloc" || name == "reallocf" || name == "reallocarray" ||
      name == "mremap") {
    AllocatorInfo info;
    info.functionName = name.str();
    info.returnsPointer = true;
    info.initializesToZero = false;
    return info;
  }
  if (name == "posix_memalign" || name == "vasprintf" || name == "asprintf" ||
      name == "getline" || name == "getdelim" || name == "__getdelim") {
    AllocatorInfo info;
    info.functionName = name.str();
    info.returnsPointer = false;
    info.ptrOutArgIndex = 0;
    info.initializesToZero = false;
    return info;
  }
  return std::nullopt;
}

bool isReallocatorName(StringRef name) {
  return name == "realloc" || name == "reallocf" || name == "reallocarray" ||
         name == "mremap";
}

bool builtinDeallocator(const Function *function) {
  if (!function || !function->isDeclaration())
    return false;
  const StringRef name = function->getName();
  return name == "free" || name == "cfree" || name == "__cxa_free_exception" ||
         name == "_ZdlPv" || name == "_ZdaPv" || name == "_ZdlPvm" ||
         name == "_ZdaPvm" || name == "_ZdlPvj" || name == "_ZdaPvj" ||
         name == "_ZdlPvSt11align_val_t" || name == "_ZdaPvSt11align_val_t" ||
         name == "_ZdlPvmSt11align_val_t" || name == "_ZdaPvmSt11align_val_t" ||
         name == "_ZdlPvRKSt9nothrow_t" || name == "_ZdaPvRKSt9nothrow_t";
}

bool builtinExitFunction(const Function *function) {
  if (!function || !function->isDeclaration())
    return false;
  const StringRef name = function->getName();
  return name == "exit" || name == "_exit" || name == "_Exit" ||
         name == "abort" || name == "err" || name == "errx" ||
         name == "__assert_fail" || name == "__assert_rtn" ||
         name == "__cxa_pure_virtual" || name == "_Unwind_Resume" ||
         name == "__cxa_rethrow" || name == "_ZSt9terminatev" ||
         name == "_ZSt17__throw_bad_allocv" ||
         name == "_ZSt28__throw_bad_array_new_lengthv" ||
         name == "_ZSt19__throw_logic_errorPKc" ||
         name == "_ZSt20__throw_length_errorPKc" ||
         name == "_ZSt20__throw_out_of_rangePKc" ||
         name == "_ZSt24__throw_out_of_range_fmtPKcz" ||
         name == "_ZSt25__throw_bad_function_callv" ||
         name == "_ZSt21__throw_bad_exceptionv" ||
         name == "_ZSt16__throw_bad_castv" ||
         name == "_ZSt18__throw_bad_typeidv" ||
         name == "_ZSt22__throw_overflow_errorPKc" ||
         name == "_ZSt21__throw_runtime_errorPKc" ||
         name == "_ZSt20__throw_system_errori" ||
         name == "_ZSt24__throw_invalid_argumentPKc";
}

bool isCppCharStringMethod(StringRef name) {
  return name.startswith("_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE") ||
         name.startswith("_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE") ||
         name.startswith("_ZNSt12basic_stringIcSt11char_traitsIcESaIcEE") ||
         name.startswith("_ZNKSt12basic_stringIcSt11char_traitsIcESaIcEE") ||
         name.startswith("_ZNSs") || name.startswith("_ZNKSs");
}

bool isCppCharStreamNoCallbackMethod(StringRef name) {
  if (name.contains("register_callback") || name.contains("imbue"))
    return false;
  return name.startswith("_ZNSols") || name.startswith("_ZNSirs") ||
         name.startswith("_ZStls") || name.startswith("_ZStrs") ||
         name.startswith("_ZSt7getlineIc") ||
         name.startswith("_ZNSt8ios_base") ||
         name.startswith("_ZNKSt8ios_base") ||
         name.startswith("_ZNSt9basic_iosIc") ||
         name.startswith("_ZNKSt9basic_iosIc") ||
         name.startswith("_ZNSi") || name.startswith("_ZNKSi") ||
         name.startswith("_ZNSo") || name.startswith("_ZNKSo") ||
         name.startswith("_ZNSd") || name.startswith("_ZNKSd") ||
         name.startswith("_ZNSt13basic_istreamIc") ||
         name.startswith("_ZNSt13basic_ostreamIc") ||
         name.startswith("_ZNSt14basic_iostreamIc") ||
         name.startswith("_ZNSt14basic_ifstreamIc") ||
         name.startswith("_ZNKSt14basic_ifstreamIc") ||
         name.startswith("_ZNSt14basic_ofstreamIc") ||
         name.startswith("_ZNKSt14basic_ofstreamIc") ||
         name.startswith("_ZNSt13basic_fstreamIc") ||
         name.startswith("_ZNKSt13basic_fstreamIc") ||
         name.startswith("_ZNSt7__cxx1118basic_stringstreamIc") ||
         name.startswith("_ZNSt7__cxx1119basic_istringstreamIc") ||
         name.startswith("_ZNSt7__cxx1119basic_ostringstreamIc") ||
         name.startswith("_ZNSt7__cxx1115basic_stringbufIc") ||
         name.startswith("_ZNSt13basic_filebufIc") ||
         name.startswith("_ZNSt15basic_streambufIc");
}

bool builtinNoPointerEffect(const Function *function) {
  if (!function || !function->isDeclaration())
    return false;
  const StringRef name = function->getName();
  if (name.startswith("_ZNSaIcE") || name.startswith("_ZNKSaIcE") ||
      name.startswith("_ZNSt6chrono") || name.startswith("_ZNKSt6chrono") ||
      name.startswith("_ZNKSt8__detail20_Prime_rehash_policy") ||
      name.startswith("_ZNSt8__detail20_Prime_rehash_policy") ||
      name.startswith("_ZSt11_Hash_bytes")) {
    return true;
  }
  static const StringSet<> noEffectNames = {
      // C stdio / formatting / fortified _chk variants
      "printf", "fprintf", "vprintf", "vfprintf", "sprintf", "snprintf",
      "vsprintf", "vsnprintf", "sscanf", "__isoc99_sscanf", "fscanf",
      "__isoc99_fscanf", "scanf", "__isoc99_scanf", "__printf_chk",
      "__fprintf_chk", "__sprintf_chk", "__snprintf_chk", "__vsprintf_chk",
      "__vsnprintf_chk", "__syslog_chk", "__read_chk", "__recv_chk",
      "__fdelt_chk", "puts", "fputs", "putchar", "putc", "fputc", "fgetc",
      "getc", "getchar", "ungetc", "perror", "fflush", "fclose", "pclose",
      "fread", "fwrite", "fseek", "fseeko", "fseeko64", "ftell", "ftello",
      "ftello64", "fgetpos", "fsetpos", "rewind", "feof", "ferror", "clearerr",
      "fileno", "setvbuf", "setbuf", "remove", "rename", "renameat",
      // C string comparison / classification / numeric conversion (no out-ptr)
      "bcmp", "strlen", "strnlen", "wcslen", "strcmp", "strncmp", "wcscmp",
      "wcsncmp", "memcmp", "wmemcmp", "strcasecmp", "strncasecmp", "strcoll",
      "strxfrm", "strcspn", "strspn", "atoi", "atol", "atoll", "atof",
      "strftime", "mktime", "clock", "time", "difftime", "rand", "srand",
      "random", "srandom", "abs", "labs", "llabs",
      // C math
      "fabs", "fabsf", "sqrt", "sqrtf", "sin", "sinf", "cos", "cosf", "tan",
      "tanf", "asin", "acos", "atan", "atan2", "sinh", "cosh", "tanh", "exp",
      "expf", "log", "logf", "log2", "log2f", "log10", "log10f", "pow", "powf",
      "floor", "floorf", "ceil", "ceilf", "round", "roundf", "trunc", "truncf",
      "fmod", "fmodf", "frexp", "ldexp", "modf", "hypot",
      // POSIX / Linux syscalls (only primitive data, no user callbacks or
      // pointer-field writes)
      "open", "open64", "openat", "creat", "close", "read", "write", "pread",
      "pread64", "pwrite", "pwrite64", "readv", "writev", "lseek", "lseek64",
      "sendfile", "splice", "stat", "stat64", "fstat", "fstat64", "lstat", "lstat64", "fstatat",
      "__xstat", "__fxstat", "__lxstat", "__xstat64", "__fxstat64",
      "__lxstat64", "access", "faccessat", "chmod", "fchmod", "chown", "fchown",
      "lchown", "chdir", "fchdir", "chroot", "mkdir", "mkdirat", "rmdir",
      "unlink", "unlinkat", "link", "symlink", "readlink", "readlinkat",
      "truncate", "truncate64", "ftruncate", "ftruncate64", "fsync",
      "fdatasync", "sync", "flock", "lockf", "fcntl", "fcntl64", "ioctl", "dup",
      "dup2", "dup3", "pipe", "pipe2", "poll", "ppoll", "select", "pselect",
      "epoll_create", "epoll_create1", "epoll_ctl", "epoll_wait", "kqueue",
      "kevent", "fork", "vfork", "execv", "execve", "execvp", "execl", "execlp",
      "wait", "waitpid", "wait3", "wait4", "kill", "raise", "alarm", "pause",
      "sleep", "usleep", "nanosleep", "clock_gettime", "clock_getres",
      "gettimeofday", "sched_yield", "getpid", "getppid", "getpgid", "getpgrp",
      "setpgid", "setsid", "getuid", "geteuid", "setuid", "seteuid", "setreuid",
      "setresuid", "getgid", "getegid", "setgid", "setegid", "setregid",
      "setresgid", "getgroups", "setgroups", "initgroups", "getdtablesize",
      "getpagesize", "sysconf", "pathconf", "fpathconf", "getrlimit",
      "getrlimit64", "setrlimit", "setrlimit64", "getrusage", "nice",
      "setpriority", "getpriority", "uname", "gethostname", "sethostname",
      "daemon", "isatty", "tcgetattr", "tcsetattr", "umask", "utime", "utimes",
      "futimes", "utimensat", "futimens", "mkstemp", "mkstemp64", "mkdtemp",
      "closedir", "dirfd", "openlog", "closelog", "syslog", "vsyslog", "munmap",
      "mprotect", "madvise", "msync", "mlock", "munlock", "socket",
      "socketpair", "bind", "connect", "listen", "accept", "accept4",
      "shutdown", "getsockname", "getpeername", "getsockopt", "setsockopt",
      "send", "sendto", "sendmsg", "recv", "recvfrom", "recvmsg", "getnameinfo",
      "freeaddrinfo", "inet_aton", "inet_ntop", "inet_pton", "sigemptyset",
      "sigfillset", "sigaddset", "sigdelset", "sigismember", "sigprocmask",
      "sigsuspend", "sigpending", "endusershell", "setusershell", "endutxent",
      "cap_init", "cap_free", "cap_get_proc", "cap_set_proc", "cap_set_flag",
      "cap_clear", "cap_clear_flag", "setjmp", "_setjmp", "sigsetjmp",
      "longjmp", "_longjmp", "siglongjmp", "flockfile", "funlockfile",
      "__uflow", "system", "tolower", "toupper", "fnmatch", "setenv",
      "unsetenv", "tzset", "killpg", "prctl", "closefrom", "tcflush",
      "tcgetpgrp", "cfgetispeed", "cfgetospeed", "cfmakeraw", "cfsetispeed",
      "cfsetospeed", "wctomb", "mbtowc", "wcwidth", "malloc_trim", "regcomp",
      "regexec", "regfree", "globfree", "forkpty", "del_curterm", "setupterm",
      "tigetflag", "tigetnum", "getloadavg", "sched_getaffinity",
      "__sched_cpucount", "posix_spawn", "posix_spawnp",
      "posix_spawn_file_actions_init", "posix_spawn_file_actions_destroy",
      "posix_spawn_file_actions_addclose", "posix_spawn_file_actions_adddup2",
      "posix_spawn_file_actions_addopen", "posix_spawnattr_init",
      "posix_spawnattr_destroy", "posix_spawnattr_setflags",
      "posix_spawnattr_setsigmask", "__b64_ntop", "__b64_pton",
      "__ctype_get_mb_cur_max",
      // pthreads synchronization (non-callback)
      "pthread_mutex_lock", "pthread_mutex_unlock", "pthread_mutex_trylock",
      "pthread_mutex_init", "pthread_mutex_destroy", "pthread_mutexattr_init",
      "pthread_mutexattr_destroy", "pthread_mutexattr_settype",
      "pthread_rwlock_rdlock", "pthread_rwlock_wrlock", "pthread_rwlock_unlock",
      "pthread_rwlock_init", "pthread_rwlock_destroy", "pthread_cond_wait",
      "pthread_cond_timedwait", "pthread_cond_signal", "pthread_cond_broadcast",
      "pthread_cond_init", "pthread_cond_destroy", "pthread_self",
      "pthread_equal",
      // C++ runtime bookkeeping (non-callback, non-pointer-clobbering)
      "__cxa_guard_acquire", "__cxa_guard_release", "__cxa_guard_abort",
      "__cxa_begin_catch", "__cxa_end_catch", "__cxa_pure_virtual",
      "__cxa_deleted_virtual"};
  return noEffectNames.contains(name);
}

bool builtinStaticReturnFunction(const Function *function) {
  if (!function || !function->isDeclaration())
    return false;
  const StringRef name = function->getName();
  static const StringSet<> staticReturnNames = {
      "__ctype_b_loc", "__ctype_tolower_loc", "__ctype_toupper_loc",
      "__errno_location", "__error", "__h_errno_location", "gai_strerror",
      "hstrerror", "strerror", "strsignal", "crypt", "inet_ntoa", "asctime",
      "ctime", "gmtime", "localtime", "localeconv", "setlocale", "nl_langinfo",
      "getenv", "secure_getenv", "ttyname", "getpwnam", "getpwuid", "getgrnam",
      "getgrgid", "getspnam", "getusershell", "gethostbyname", "gethostbyaddr",
      "getservbyname", "getprotobyname", "readdir", "readdir64", "tigetstr",
      "tparm"};
  return staticReturnNames.contains(name);
}

bool scalarType(const Type *type) {
  return type->isSingleValueType() && !type->isVectorTy();
}

bool hasDeferredHandlerDecl(const Module &module) {
  for (const Function &fn : module) {
    if (!fn.isDeclaration())
      continue;
    const StringRef name = fn.getName();
    if (name == "atexit" || name == "on_exit" || name == "__cxa_atexit" ||
        name == "signal" || name == "__sysv_signal" || name == "sigset" ||
        name == "sigaction")
      return true;
  }
  return false;
}

// An allocation site inside a CFG cycle may represent multiple live objects.
bool blockInCycle(const BasicBlock &origin) {
  if (pred_empty(&origin))
    return false;
  SmallPtrSet<const BasicBlock *, 32> seen;
  SmallVector<const BasicBlock *, 32> work;
  for (const BasicBlock *successor : successors(&origin))
    work.push_back(successor);
  while (!work.empty()) {
    const BasicBlock *block = work.pop_back_val();
    if (block == &origin)
      return true;
    if (!seen.insert(block).second)
      continue;
    for (const BasicBlock *successor : successors(block))
      work.push_back(successor);
  }
  return false;
}

struct LifecycleEntry {
  int32_t priority = 65535;
  const Function *function = nullptr;
};

std::vector<LifecycleEntry> parseLifecycleList(const Module &module,
                                               const char *globalName,
                                               bool ascending) {
  std::vector<LifecycleEntry> entries;
  const GlobalVariable *list = module.getNamedGlobal(globalName);
  if (!list || !list->hasInitializer())
    return entries;
  const auto *arr = dyn_cast<ConstantArray>(list->getInitializer());
  if (!arr)
    return entries;
  for (const Use &op : arr->operands()) {
    const auto *st = dyn_cast<ConstantStruct>(op.get());
    if (!st || st->getNumOperands() < 2)
      continue;
    const auto *prio = dyn_cast<ConstantInt>(st->getOperand(0));
    const Value *fnVal = st->getOperand(1)->stripPointerCasts();
    const auto *fn = dyn_cast<Function>(fnVal);
    if (!fn || fn->isDeclaration())
      continue;
    entries.push_back(
        {prio ? static_cast<int32_t>(prio->getSExtValue()) : 65535, fn});
  }
  std::stable_sort(entries.begin(), entries.end(),
                   [ascending](const LifecycleEntry &a,
                               const LifecycleEntry &b) {
                     return ascending ? (a.priority < b.priority)
                                      : (a.priority > b.priority);
                   });
  return entries;
}

} // namespace

class ValueFlowPTA::Impl {
public:
  Impl(const Module &module, Config config)
      : module(module), config(std::move(config)) {}

  const Module &module;
  Config config;
  Statistics stats;
  bool analyzed = false;
  const AliasSpecManager *specManager = nullptr;
  std::unique_ptr<vfg::Program> program;
  std::unique_ptr<vfg::Solver> solver;
  NodeID emptyValueNode = 0;
  ObjectID atexitSlot = 0;
  ObjectID onExitSlot = 0;
  ObjectID onExitArgSlot = 0;
  ObjectID cxaAtexitSlot = 0;
  ObjectID cxaAtexitArgSlot = 0;
  ObjectID signalSlot = 0;
  FunctionID atexitDispatchFn = 0;
  FunctionID externalCallbackFn = 0;
  bool externalCallbackBuilt = false;
  FunctionID unknownExternalCalleeFns[4] = {0, 0, 0, 0};

  DenseMap<const Value *, NodeID> values;
  DenseMap<const Value *, NodeID> aggregateNodes;
  DenseMap<const StoreInst *, NodeID> storeNodes;
  std::vector<const Value *> nodeValues;
  DenseMap<const Value *, ObjectID> objects;
  std::vector<const Value *> objectValues;
  mutable std::vector<PointedToBySet> inverse;
  mutable bool inverseBuilt = false;
  DenseMap<const BasicBlock *, BlockID> blockIds;
  DenseMap<const AtomicCmpXchgInst *, NodeID> atomicResults;
  DenseMap<const Function *, ObjectID> staticReturnObjects;
  DenseMap<const CallBase *, ObjectID> callAllocObjects;
  DenseMap<const Type *, bool> typeHasPtrCache;
  DenseMap<const Value *, bool> ptrToIntCache;
  DenseSet<const Function *> addressTakenSet;

  // Compute address-taken defined functions directly from global initializers,
  // aliases, ifuncs, and instruction operands without calling `Value::uses()`,
  // so modules loaded with lazy metadata (`ShouldLazyLoadMetadata=true`) work
  // without forcing global metadata materialization.
  void collectAddressTakenFunctions() {
    addressTakenSet.clear();
    DenseSet<const Constant *> visitedConsts;
    auto markConst = [&](const Constant *root) {
      if (!root)
        return;
      SmallVector<const Constant *, 32> work;
      if (visitedConsts.insert(root).second)
        work.push_back(root);
      while (!work.empty()) {
        const Constant *c = work.pop_back_val();
        if (const auto *fn = dyn_cast<Function>(c)) {
          if (!fn->isDeclaration())
            addressTakenSet.insert(fn);
          continue;
        }
        if (isa<BlockAddress>(c))
          continue;
        for (const Use &op : c->operands()) {
          if (const auto *sub = dyn_cast<Constant>(op.get())) {
            if (visitedConsts.insert(sub).second)
              work.push_back(sub);
          }
        }
      }
    };
    for (const GlobalVariable &gv : module.globals()) {
      if (gv.hasInitializer())
        markConst(gv.getInitializer());
    }
    for (const GlobalAlias &ga : module.aliases()) {
      if (const Constant *aliasee = ga.getAliasee())
        markConst(aliasee);
    }
    for (const GlobalIFunc &gif : module.ifuncs()) {
      if (const Constant *resolver = gif.getResolver())
        markConst(resolver);
    }
    for (const Function &fn : module) {
      if (fn.isDeclaration())
        continue;
      if (fn.hasPersonalityFn())
        markConst(fn.getPersonalityFn());
      if (fn.hasPrefixData())
        markConst(fn.getPrefixData());
      if (fn.hasPrologueData())
        markConst(fn.getPrologueData());
      for (const BasicBlock &bb : fn) {
        for (const Instruction &inst : bb) {
          if (isa<DbgInfoIntrinsic>(&inst))
            continue;
          const auto *cb = dyn_cast<CallBase>(&inst);
          for (const Use &u : inst.operands()) {
            if (cb && cb->isCallee(&u)) {
              const Value *v = u.get();
              if (isa<Function>(v))
                continue;
              if (const auto *ce = dyn_cast<ConstantExpr>(v)) {
                if (ce->isCast() && isa<Function>(ce->getOperand(0)))
                  continue;
              }
            }
            if (const auto *c = dyn_cast<Constant>(u.get()))
              markConst(c);
          }
        }
      }
    }
  }

  bool isFunctionAddressTaken(const Function &fn) const {
    return addressTakenSet.contains(&fn);
  }

  struct ExtDeclSpecCache {
    bool initialized = false;
    bool isDealloc = false;
    bool isExit = false;
    bool isNoEffect = false;
    std::optional<AllocatorInfo> allocInfo;
    bool specEffectsLoaded = false;
    std::vector<CopyInfo> copyEffects;
    std::vector<ReturnAliasInfo> returnAliases;
  };
  mutable DenseMap<const Function *, ExtDeclSpecCache> extDeclCache;

  const ExtDeclSpecCache &getExtDeclSpec(const Function *fn) const {
    auto &entry = extDeclCache[fn];
    if (!entry.initialized) {
      entry.initialized = true;
      if (fn && fn->isDeclaration()) {
        if (specManager) {
          if (auto info = specManager->getAllocatorInfo(fn))
            entry.allocInfo = std::move(info);
        }
        if (!entry.allocInfo)
          entry.allocInfo = builtinAllocatorInfo(fn);
        entry.isDealloc = (specManager && specManager->isDeallocator(fn)) ||
                          builtinDeallocator(fn);
        entry.isExit = (specManager && specManager->isExitFunction(fn)) ||
                       builtinExitFunction(fn);
        entry.isNoEffect = isAnnotationFunction(fn) ||
                           (specManager && specManager->isNoEffect(fn)) ||
                           builtinNoPointerEffect(fn);
      }
    }
    return entry;
  }

  const ExtDeclSpecCache &getExtDeclSpecWithEffects(const Function *fn) const {
    auto &entry = const_cast<ExtDeclSpecCache &>(getExtDeclSpec(fn));
    if (!entry.specEffectsLoaded) {
      entry.specEffectsLoaded = true;
      if (specManager && fn && fn->isDeclaration()) {
        entry.copyEffects = specManager->getCopyEffects(fn);
        entry.returnAliases = specManager->getReturnAliasInfo(fn);
      }
    }
    return entry;
  }

  bool typeContainsPointer(const Type *type) {
    if (!type)
      return false;
    if (type->isPointerTy())
      return true;
    if (type->isIntegerTy() || type->isFloatingPointTy() || type->isVoidTy() ||
        type->isLabelTy() || type->isMetadataTy())
      return false;
    auto it = typeHasPtrCache.find(type);
    if (it != typeHasPtrCache.end())
      return it->second;
    bool result = false;
    if (const auto *arr = dyn_cast<ArrayType>(type)) {
      result = typeContainsPointer(arr->getElementType());
    } else if (const auto *vec = dyn_cast<VectorType>(type)) {
      result = typeContainsPointer(vec->getElementType());
    } else if (const auto *st = dyn_cast<StructType>(type)) {
      for (const Type *elem : st->elements()) {
        if (typeContainsPointer(elem)) {
          result = true;
          break;
        }
      }
    }
    typeHasPtrCache[type] = result;
    return result;
  }

  bool containsPtrToIntImpl(const Value *value,
                            SmallPtrSetImpl<const Value *> &visited) {
    if (!value)
      return false;
    auto it = ptrToIntCache.find(value);
    if (it != ptrToIntCache.end())
      return it->second;
    if (!visited.insert(value).second)
      return false;
    bool found = false;
    if (isa<PtrToIntInst>(value)) {
      found = true;
    } else if (const auto *ce = dyn_cast<ConstantExpr>(value)) {
      if (ce->getOpcode() == Instruction::PtrToInt) {
        found = true;
      } else {
        for (const Use &op : ce->operands()) {
          if (containsPtrToIntImpl(op.get(), visited)) {
            found = true;
            break;
          }
        }
      }
    } else if (const auto *inst = dyn_cast<Instruction>(value)) {
      if (isa<CastInst>(inst) || isa<FreezeInst>(inst) ||
          isa<ExtractValueInst>(inst) || isa<ExtractElementInst>(inst)) {
        found = containsPtrToIntImpl(inst->getOperand(0), visited);
      } else if (const auto *bin = dyn_cast<BinaryOperator>(inst)) {
        found = containsPtrToIntImpl(bin->getOperand(0), visited) ||
                containsPtrToIntImpl(bin->getOperand(1), visited);
      } else if (const auto *phi = dyn_cast<PHINode>(inst)) {
        for (const Use &inc : phi->incoming_values()) {
          if (containsPtrToIntImpl(inc.get(), visited)) {
            found = true;
            break;
          }
        }
      } else if (const auto *sel = dyn_cast<SelectInst>(inst)) {
        found = containsPtrToIntImpl(sel->getTrueValue(), visited) ||
                containsPtrToIntImpl(sel->getFalseValue(), visited);
      } else if (const auto *ins = dyn_cast<InsertValueInst>(inst)) {
        found =
            containsPtrToIntImpl(ins->getAggregateOperand(), visited) ||
            containsPtrToIntImpl(ins->getInsertedValueOperand(), visited);
      } else if (const auto *ins = dyn_cast<InsertElementInst>(inst)) {
        found = containsPtrToIntImpl(ins->getOperand(0), visited) ||
                containsPtrToIntImpl(ins->getOperand(1), visited);
      } else if (const auto *shuf = dyn_cast<ShuffleVectorInst>(inst)) {
        found = containsPtrToIntImpl(shuf->getOperand(0), visited) ||
                containsPtrToIntImpl(shuf->getOperand(1), visited);
      }
    }
    if (found)
      ptrToIntCache[value] = true;
    return found;
  }

  bool containsPtrToInt(const Value *value) {
    if (!value)
      return false;
    auto it = ptrToIntCache.find(value);
    if (it != ptrToIntCache.end())
      return it->second;
    SmallPtrSet<const Value *, 16> visited;
    if (containsPtrToIntImpl(value, visited))
      return true;
    for (const Value *u : visited)
      ptrToIntCache[u] = false;
    return false;
  }

  struct FunctionInfo {
    FunctionID id = 0;
    BlockID entry = 0;
    BlockID exit = 0;
    NodeID returned = 0;
    std::vector<NodeID> formalParams;
    std::vector<std::pair<unsigned, ObjectID>> byValParams;
  };
  DenseMap<const Function *, FunctionInfo> functions;
  std::vector<const Function *> functionById;

  struct ExternalCallRecord {
    const CallBase *instruction = nullptr;
    BlockID block = 0;
    bool conservative = false;
  };
  std::vector<ExternalCallRecord> externalCalls;

  struct IndirectCallRecord {
    const CallBase *instruction = nullptr;
    BlockID block = 0;
    CallSiteID site = 0;
    NodeID calledPointer = 0;
    std::set<const Function *> modeledExternalTargets;
    bool modeledUnknownExternal = false;
  };
  std::vector<IndirectCallRecord> indirectCalls;

  std::vector<const Function *> entryPoints;

  std::optional<AllocatorInfo> getAllocatorInfo(const Function *fn) const {
    return getExtDeclSpec(fn).allocInfo;
  }

  bool isDeallocator(const Function *fn) const {
    return getExtDeclSpec(fn).isDealloc;
  }

  bool isExitFunction(const Function *fn) const {
    return getExtDeclSpec(fn).isExit;
  }

  bool isNoEffectFunction(const Function *fn) const {
    return getExtDeclSpec(fn).isNoEffect;
  }

  bool mayReturn(const CallBase &call, const Function *callee = nullptr) const {
    if (callee && isExitFunction(callee) && !isa<InvokeInst>(call))
      return false;
    return !call.doesNotReturn() || isa<InvokeInst>(call);
  }

  void recordNodeValue(NodeID id, const Value *value) {
    if (id >= nodeValues.size())
      nodeValues.resize(id + 1, nullptr);
    if (!nodeValues[id])
      nodeValues[id] = value;
  }

  ObjectID makeObject(const Value *value, bool singleton, bool memory) {
    const ObjectID object = program->addObject(
        singleton, memory,
        value && value->hasName() ? value->getName().str() : "");
    if (value) {
      objects[value] = object;
      recordNodeValue(program->address(object), value);
    }
    objectValues.resize(program->objectCount() + 1, nullptr);
    objectValues[object] = value;
    return object;
  }

  NodeID node(const Value *value) {
    if (!value || !value->getType()->isPointerTy())
      return program->unknownValue();
    auto found = values.find(value);
    if (found != values.end())
      return found->second;
    if (isa<ConstantPointerNull>(value)) {
      values[value] = program->nullValue();
      return program->nullValue();
    }
    const NodeID result = program->addNode();
    values[value] = result; // Install before following recursive constants.
    recordNodeValue(result, value);
    if (const auto *alias = dyn_cast<GlobalAlias>(value)) {
      program->addCopy(node(alias->getAliasee()), result);
    } else if (const auto *expression = dyn_cast<ConstantExpr>(value)) {
      if ((expression->isCast() ||
           expression->getOpcode() == Instruction::GetElementPtr) &&
          expression->getOperand(0)->getType()->isPointerTy()) {
        program->addCopy(node(expression->getOperand(0)), result);
      } else if (expression->getOpcode() == Instruction::Select &&
                 expression->getNumOperands() >= 3) {
        program->addCopy(node(expression->getOperand(1)), result);
        program->addCopy(node(expression->getOperand(2)), result);
      } else {
        program->addCopy(program->unknownValue(), result);
      }
    } else if (isa<UndefValue>(value) || isa<PoisonValue>(value)) {
      // Undef/poison pointer constants carry no concrete address.
    } else if (isa<Constant>(value) ||
               (!isa<Instruction>(value) && !isa<Argument>(value))) {
      program->addCopy(program->unknownValue(), result);
    }
    return result;
  }

  FunctionID getOrCreateExternalCallbackDispatcher() {
    if (externalCallbackBuilt)
      return externalCallbackFn;
    externalCallbackBuilt = true;
    std::vector<const Function *> addrTaken;
    for (const Function &fn : module) {
      if (!fn.isDeclaration() && isFunctionAddressTaken(fn))
        addrTaken.push_back(&fn);
    }
    if (addrTaken.empty())
      return 0;
    externalCallbackFn = program->addFunction("<__vfg_external_callback>");
    const BlockID blk = program->addBlock(externalCallbackFn);
    program->setFunctionEntry(externalCallbackFn, blk);
    program->setFunctionExit(externalCallbackFn, blk);
    for (const Function *fn : addrTaken) {
      const auto &cbInfo = functions.find(fn)->second;
      for (const auto &[argIdx, byValObj] : cbInfo.byValParams) {
        (void)argIdx;
        program->addStore(blk, program->address(byValObj),
                          program->unknownValue(), /*allowStrong=*/false);
      }
    }
    const CallSiteID disp = program->addCallSite(blk, 0, 0, {}, true, false);
    program->markCallExternal(disp, true);
    for (const Function *fn : addrTaken)
      program->addExternalCallback(disp, functions.find(fn)->second.id);
    return externalCallbackFn;
  }

  FunctionID getOrCreateUnknownExternalCallee(bool readOnly, bool noCallback) {
    const unsigned idx = (readOnly ? 1U : 0U) | (noCallback ? 2U : 0U);
    if (unknownExternalCalleeFns[idx])
      return unknownExternalCalleeFns[idx];
    const FunctionID fn = program->addFunction("<__vfg_unknown_callee>");
    const BlockID blk = program->addBlock(fn);
    program->setFunctionEntry(fn, blk);
    program->setFunctionExit(fn, blk);
    program->setFunctionReturn(fn, program->unknownValue());
    if (!readOnly) {
      program->addStore(blk, program->unknownValue(), program->unknownValue(),
                        /*allowStrong=*/false);
    }
    if (!noCallback) {
      if (const FunctionID cbDispatcher =
              getOrCreateExternalCallbackDispatcher()) {
        const CallSiteID cbSite = program->addCallSite(blk);
        program->markCallExternal(cbSite, true);
        program->addExternalCallback(cbSite, cbDispatcher);
      }
    }
    unknownExternalCalleeFns[idx] = fn;
    return fn;
  }

  ObjectID getOrCreateStaticReturnObject(const Function *callee) {
    auto it = staticReturnObjects.find(callee);
    if (it != staticReturnObjects.end())
      return it->second;
    const ObjectID obj = program->addObject(
        /*singleton=*/false, /*memory=*/true,
        "<static:" + callee->getName().str() + ">");
    objectValues.resize(program->objectCount() + 1, nullptr);
    objectValues[obj] = nullptr;
    staticReturnObjects[callee] = obj;
    return obj;
  }

  ObjectID getOrCreateCallAllocObject(const CallBase &call,
                                      const AllocatorInfo &info) {
    auto it = callAllocObjects.find(&call);
    if (it != callAllocObjects.end())
      return it->second;
    const ObjectID obj = makeObject(&call, /*singleton=*/false, /*memory=*/true);
    program->setObjectGlobal(obj, false);
    if (info.initializesToZero)
      program->setInitializer(obj, program->nullValue());
    else
      program->setEmptyInitializer(obj);
    callAllocObjects[&call] = obj;
    return obj;
  }

  NodeID aggregateNode(const Value *val) {
    if (!val)
      return emptyValueNode;
    if (val->getType()->isPointerTy())
      return node(val);
    if (isa<UndefValue>(val) || isa<PoisonValue>(val))
      return emptyValueNode;
    if (!typeContainsPointer(val->getType())) {
      return containsPtrToInt(val) ? program->unknownValue() : emptyValueNode;
    }
    auto found = aggregateNodes.find(val);
    if (found != aggregateNodes.end())
      return found->second;
    if (isa<ConstantAggregateZero>(val)) {
      aggregateNodes[val] = program->nullValue();
      return program->nullValue();
    }
    const NodeID result = program->addNode();
    aggregateNodes[val] = result; // Install before following recursive consts.
    if (const auto *c = dyn_cast<Constant>(val)) {
      if (c->getType()->isAggregateType() || c->getType()->isVectorTy()) {
        for (const Use &op : c->operands()) {
          const Value *elem = op.get();
          if (elem->getType()->isPointerTy() ||
              typeContainsPointer(elem->getType()) || containsPtrToInt(elem)) {
            const NodeID elemNode = aggregateNode(elem);
            if (elemNode != emptyValueNode)
              program->addCopy(elemNode, result);
          }
        }
      } else {
        program->addCopy(program->unknownValue(), result);
      }
    } else if (!isa<Instruction>(val) && !isa<Argument>(val)) {
      program->addCopy(program->unknownValue(), result);
    }
    return result;
  }

  NodeID carrierNode(const Value *val) {
    if (!val)
      return 0;
    if (val->getType()->isPointerTy())
      return node(val);
    if (typeContainsPointer(val->getType()))
      return aggregateNode(val);
    return 0;
  }

  // Collect pointer-valued fields from a global constant initializer.
  // Unlike the legacy adapter, non-pointer scalar fields (e.g., i32 0 in
  // `{ i32, i8* }`) do NOT inject `unknown` in pointer-only mode.
  // Lazily allocates `destination` on first emitted edge to avoid creating
  // unused VFG nodes for thousands of non-pointer global constants.
  void collectInitializer(const Constant *constant, NodeID &destination,
                          bool &addedAny) {
    if (!constant)
      return;
    auto emitEdge = [&](NodeID src) {
      if (!destination)
        destination = program->addNode();
      program->addCopy(src, destination);
      addedAny = true;
    };
    if (constant->getType()->isPointerTy()) {
      if (isa<UndefValue>(constant) || isa<PoisonValue>(constant))
        return;
      emitEdge(node(constant));
    } else if (isa<ConstantAggregateZero>(constant)) {
      if (typeContainsPointer(constant->getType()))
        emitEdge(program->nullValue());
    } else if (isa<UndefValue>(constant) || isa<PoisonValue>(constant)) {
      if (!config.pointerOnlyMemory)
        emitEdge(program->unknownValue());
    } else if (constant->getType()->isAggregateType() ||
               constant->getType()->isVectorTy()) {
      for (const Use &operand : constant->operands())
        collectInitializer(dyn_cast<Constant>(operand.get()), destination,
                           addedAny);
      if (!config.pointerOnlyMemory && constant->getNumOperands() == 0)
        emitEdge(program->unknownValue());
    } else if (const auto *ce = dyn_cast<ConstantExpr>(constant)) {
      if (containsPtrToInt(ce))
        emitEdge(program->unknownValue());
      else if (!config.pointerOnlyMemory)
        emitEdge(program->unknownValue());
    } else if (!config.pointerOnlyMemory) {
      emitEdge(program->unknownValue());
    }
  }

  void collectObjectsAndFunctions() {
    emptyValueNode = program->addNode();

    for (const GlobalVariable &global : module.globals()) {
      const auto object =
          makeObject(&global, scalarType(global.getValueType()), true);
      values[&global] = program->address(object);
    }

    for (const Function &function : module) {
      const auto object = makeObject(&function, false, false);
      values[&function] = program->address(object);
      if (function.isDeclaration())
        continue;

      FunctionInfo info;
      info.id = program->addFunction(function.getName().str());
      info.entry = program->addBlock(info.id);
      info.exit = program->addBlock(info.id);
      program->setFunctionEntry(info.id, info.entry);
      program->setFunctionExit(info.id, info.exit);
      program->setFunctionObject(object, info.id);
      program->setAddressTakenFunction(info.id, isFunctionAddressTaken(function));
      if (function.getReturnType()->isPointerTy() ||
          typeContainsPointer(function.getReturnType())) {
        info.returned = program->addNode();
        program->setFunctionReturn(info.id, info.returned);
      }

      unsigned argIdx = 0;
      for (const Argument &argument : function.args()) {
        if (argument.getType()->isPointerTy()) {
          if (argument.hasByValAttr()) {
            const auto argObj = makeObject(&argument, false, true);
            program->setEmptyInitializer(argObj);
            program->setObjectOwner(argObj, info.id);
            values[&argument] = program->address(argObj);
            // Copy from the caller's pointer argument once at callee entry so
            // both direct and dynamic indirect calls handle `byval` in a single
            // solver pass without per-callsite duplication.
            const NodeID srcPtrNode = program->addNode();
            info.formalParams.push_back(srcPtrNode);
            const NodeID copyTmp = program->addNode();
            program->addLoad(info.entry, srcPtrNode, copyTmp);
            program->addStore(info.entry, program->address(argObj), copyTmp,
                              /*allowStrong=*/false);
            info.byValParams.emplace_back(argIdx, argObj);
          } else {
            const NodeID paramNode = node(&argument);
            info.formalParams.push_back(paramNode);
          }
        } else if (typeContainsPointer(argument.getType())) {
          const NodeID paramNode = aggregateNode(&argument);
          info.formalParams.push_back(paramNode);
        } else {
          info.formalParams.push_back(0);
        }
        ++argIdx;
      }
      program->setFunctionParams(info.id, info.formalParams);

      for (const BasicBlock &block : function)
        blockIds[&block] = program->addBlock(info.id);
      program->addControlEdge(info.entry,
                              blockIds.lookup(&function.getEntryBlock()));

      if (info.id >= functionById.size())
        functionById.resize(info.id + 1, nullptr);
      functionById[info.id] = &function;
      functions[&function] = std::move(info);
    }

    for (const Function &function : module) {
      if (function.isDeclaration())
        continue;
      const FunctionID fnId = functions.find(&function)->second.id;
      for (const BasicBlock &block : function) {
        bool cycleChecked = false;
        bool cyclic = false;
        for (const Instruction &instruction : block) {
          if (const auto *alloca = dyn_cast<AllocaInst>(&instruction)) {
            if (!cycleChecked) {
              cyclic = blockInCycle(block);
              cycleChecked = true;
            }
            const auto *count = dyn_cast<ConstantInt>(alloca->getArraySize());
            const bool unique = scalarType(alloca->getAllocatedType()) &&
                                count && count->isOne() && !cyclic;
            const auto object = makeObject(alloca, unique, true);
            program->setEmptyInitializer(object);
            program->setObjectOwner(object, fnId);
            values[alloca] = program->address(object);
          } else if (const auto *call = dyn_cast<CallBase>(&instruction)) {
            const Function *callee = directCallee(*call);
            if (auto allocInfo = getAllocatorInfo(callee)) {
              const ObjectID object = getOrCreateCallAllocObject(*call, *allocInfo);
              if (allocInfo->returnsPointer && call->getType()->isPointerTy()) {
                const NodeID result = node(call);
                program->addCopy(program->address(object), result);
                program->addCopy(program->nullValue(), result);
              }
            }
          }
          if (instruction.getType()->isPointerTy())
            node(&instruction);
          else if (typeContainsPointer(instruction.getType()))
            aggregateNode(&instruction);
          if (const auto *cmp = dyn_cast<AtomicCmpXchgInst>(&instruction))
            atomicResults[cmp] = program->addNode();
        }
      }
    }

    for (const GlobalAlias &alias : module.aliases())
      node(&alias);
  }

  void setupEntryPointsAndLifecycle() {
    std::vector<LifecycleEntry> ctors;
    std::vector<LifecycleEntry> dtors;
    for (const char *name : {"llvm.global_ctors", "llvm.global_dtors"}) {
      if (const GlobalVariable *list = module.getNamedGlobal(name)) {
        const auto *type = dyn_cast<ArrayType>(list->getValueType());
        if (!type || type->getNumElements() != 0) {
          if (!config.lowerGlobalLifecycle) {
            throw std::invalid_argument(
                "ValueFlowPTA: lower global constructors/destructors to "
                "explicit entry-point calls and remove their lifecycle lists "
                "first");
          }
        }
      }
    }
    if (config.lowerGlobalLifecycle) {
      ctors = parseLifecycleList(module, "llvm.global_ctors",
                                 /*ascending=*/true);
      dtors = parseLifecycleList(module, "llvm.global_dtors",
                                 /*ascending=*/false);
    }
    stats.globalCtors = ctors.size();
    stats.globalDtors = dtors.size();

    entryPoints = config.entryPoints;
    if (entryPoints.empty()) {
      const Function *main = module.getFunction("main");
      if (main && !main->isDeclaration()) {
        entryPoints.push_back(main);
      } else {
        for (const Function &function : module)
          if (!function.isDeclaration() && !function.hasLocalLinkage())
            entryPoints.push_back(&function);
        if (entryPoints.empty())
          for (const Function &function : module)
            if (!function.isDeclaration())
              entryPoints.push_back(&function);
      }
    }
    for (const Function *function : entryPoints) {
      if (!function || function->getParent() != &module ||
          function->isDeclaration()) {
        throw std::invalid_argument(
            "ValueFlowPTA: entry point is not a module definition");
      }
    }

    // Seed entry-point arguments (external inputs) with unknown.
    for (const Function *function : entryPoints) {
      const auto &info = functions.find(function)->second;
      for (const Argument &argument : function->args()) {
        if (argument.getType()->isPointerTy()) {
          if (!argument.hasByValAttr())
            program->addCopy(program->unknownValue(), node(&argument));
        } else if (typeContainsPointer(argument.getType())) {
          program->addCopy(program->unknownValue(), aggregateNode(&argument));
        }
      }
      for (const auto &[argIdx, byValObj] : info.byValParams) {
        (void)argIdx;
        program->addStore(info.entry, program->address(byValObj),
                          program->unknownValue(), /*allowStrong=*/false);
      }
    }

    const bool hasDeferredHandlers = hasDeferredHandlerDecl(module);
    if (hasDeferredHandlers) {
      atexitSlot = program->addObject(/*singleton=*/false, /*memory=*/true,
                                      "<__vfg_atexit>");
      onExitSlot = program->addObject(/*singleton=*/false, /*memory=*/true,
                                      "<__vfg_on_exit>");
      onExitArgSlot = program->addObject(/*singleton=*/false, /*memory=*/true,
                                         "<__vfg_on_exit_arg>");
      cxaAtexitSlot = program->addObject(/*singleton=*/false, /*memory=*/true,
                                         "<__vfg_cxa_atexit>");
      cxaAtexitArgSlot = program->addObject(/*singleton=*/false, /*memory=*/true,
                                            "<__vfg_cxa_atexit_arg>");
      signalSlot = program->addObject(/*singleton=*/false, /*memory=*/true,
                                      "<__vfg_signal>");
      program->setEmptyInitializer(atexitSlot);
      program->setEmptyInitializer(onExitSlot);
      program->setEmptyInitializer(onExitArgSlot);
      program->setEmptyInitializer(cxaAtexitSlot);
      program->setEmptyInitializer(cxaAtexitArgSlot);
      program->setEmptyInitializer(signalSlot);
      objectValues.resize(program->objectCount() + 1, nullptr);
    }

    if (ctors.empty() && dtors.empty() && !hasDeferredHandlers &&
        entryPoints.size() <= 1) {
      for (const Function *function : entryPoints)
        program->addEntryFunction(functions.find(function)->second.id);
      return;
    }

    // Synthesize a startup/teardown driver. For single-entry modules (e.g.,
    // main), run straight-line: ctors -> main -> deferred handlers -> dtors.
    // For multi-entry library modules, wrap entry points in a non-deterministic
    // loop so global state written by any entry point flows soundly to every
    // other entry point in any call order while preserving intra-function flow
    // sensitivity.
    const FunctionID startup = program->addFunction("<__vfg_startup>");
    const BlockID ctorBlock = program->addBlock(startup);
    program->setFunctionEntry(startup, ctorBlock);
    for (const auto &entry : ctors) {
      auto it = functions.find(entry.function);
      if (it == functions.end())
        continue;
      const CallSiteID cs = program->addCallSite(ctorBlock);
      program->addCallTarget(cs, it->second.id);
    }

    BlockID postEntryBlock = ctorBlock;
    if (entryPoints.size() <= 1) {
      for (const Function *entry : entryPoints) {
        const CallSiteID cs = program->addCallSite(ctorBlock);
        program->addCallTarget(cs, functions.find(entry)->second.id);
      }
    } else {
      const BlockID loopHeader = program->addBlock(startup);
      postEntryBlock = program->addBlock(startup);
      program->addControlEdge(ctorBlock, loopHeader);
      for (const Function *entry : entryPoints) {
        const BlockID callBlock = program->addBlock(startup);
        const CallSiteID cs = program->addCallSite(callBlock);
        program->addCallTarget(cs, functions.find(entry)->second.id);
        program->addControlEdge(loopHeader, callBlock);
        program->addControlEdge(callBlock, loopHeader);
      }
      program->addControlEdge(loopHeader, postEntryBlock);
    }

    BlockID dtorBlock = postEntryBlock;
    if (hasDeferredHandlers) {
      // Dedicated exit-handler dispatcher function so both normal return from
      // entry points and explicit exit() calls invoke registered atexit/on_exit
      // handlers in a non-deterministic loop (modeling multiple handlers
      // interacting in LIFO order).
      atexitDispatchFn = program->addFunction("<__vfg_atexit_dispatch>");
      const BlockID dispLoop = program->addBlock(atexitDispatchFn);
      const BlockID dispExit = program->addBlock(atexitDispatchFn);
      program->setFunctionEntry(atexitDispatchFn, dispLoop);
      program->setFunctionExit(atexitDispatchFn, dispExit);

      const BlockID atexitBlock = program->addBlock(atexitDispatchFn);
      const NodeID atexitFn = program->addNode();
      program->addLoad(atexitBlock, program->address(atexitSlot), atexitFn);
      const CallSiteID atexitCs = program->addCallSite(atexitBlock, atexitFn);
      program->markCallExternal(atexitCs, true);
      program->addControlEdge(dispLoop, atexitBlock);
      program->addControlEdge(atexitBlock, dispLoop);

      const BlockID onExitBlock = program->addBlock(atexitDispatchFn);
      const NodeID onExitFn = program->addNode();
      const NodeID onExitArg = program->addNode();
      program->addLoad(onExitBlock, program->address(onExitSlot), onExitFn);
      program->addLoad(onExitBlock, program->address(onExitArgSlot), onExitArg);
      const CallSiteID onExitCs =
          program->addCallSite(onExitBlock, onExitFn, 0, {0, onExitArg});
      program->markCallExternal(onExitCs, true);
      program->addControlEdge(dispLoop, onExitBlock);
      program->addControlEdge(onExitBlock, dispLoop);

      const BlockID cxaBlock = program->addBlock(atexitDispatchFn);
      const NodeID cxaFn = program->addNode();
      const NodeID cxaArg = program->addNode();
      program->addLoad(cxaBlock, program->address(cxaAtexitSlot), cxaFn);
      program->addLoad(cxaBlock, program->address(cxaAtexitArgSlot), cxaArg);
      const CallSiteID cxaCs =
          program->addCallSite(cxaBlock, cxaFn, 0, {cxaArg});
      program->markCallExternal(cxaCs, true);
      program->addControlEdge(dispLoop, cxaBlock);
      program->addControlEdge(cxaBlock, dispLoop);
      program->addControlEdge(dispLoop, dispExit);

      // Post-entry teardown loop invoking registered atexit/on_exit and signal
      // handlers after entry-point execution and before global destructors.
      const BlockID teardownLoop = program->addBlock(startup);
      dtorBlock = program->addBlock(startup);
      program->addControlEdge(postEntryBlock, teardownLoop);

      const BlockID dispatchCallBlock = program->addBlock(startup);
      const CallSiteID dispCs = program->addCallSite(dispatchCallBlock);
      program->addCallTarget(dispCs, atexitDispatchFn);
      program->addControlEdge(teardownLoop, dispatchCallBlock);
      program->addControlEdge(dispatchCallBlock, teardownLoop);

      const BlockID signalBlock = program->addBlock(startup);
      const NodeID signalFn = program->addNode();
      program->addLoad(signalBlock, program->address(signalSlot), signalFn);
      const CallSiteID signalCs = program->addCallSite(signalBlock, signalFn);
      program->markCallExternal(signalCs, true);
      program->addControlEdge(teardownLoop, signalBlock);
      program->addControlEdge(signalBlock, teardownLoop);

      program->addControlEdge(teardownLoop, dtorBlock);
    }

    for (const auto &entry : dtors) {
      auto it = functions.find(entry.function);
      if (it == functions.end())
        continue;
      const CallSiteID cs = program->addCallSite(dtorBlock);
      program->addCallTarget(cs, it->second.id);
    }
    program->setFunctionExit(startup, dtorBlock);
    program->addEntryFunction(startup);
  }

  bool handleIntrinsic(const CallBase &call, BlockID block) {
    const auto *instruction = dyn_cast<IntrinsicInst>(&call);
    if (!instruction)
      return false;
    if (isa<DbgInfoIntrinsic>(instruction))
      return true;
    switch (instruction->getIntrinsicID()) {
    case Intrinsic::lifetime_start:
    case Intrinsic::lifetime_end:
    case Intrinsic::assume:
    case Intrinsic::invariant_start:
    case Intrinsic::invariant_end:
    case Intrinsic::vaend:
    case Intrinsic::trap:
    case Intrinsic::stackrestore:
    case Intrinsic::stacksave:
      if (call.getType()->isPointerTy())
        program->addCopy(program->unknownValue(), node(&call));
      return true;
    default:
      break;
    }

    const DataLayout &dl = module.getDataLayout();
    const uint64_t ptrSize = dl.getPointerSize() ? dl.getPointerSize() : 8;

    if (const auto *memSet = dyn_cast<MemSetInst>(instruction)) {
      const auto *length = dyn_cast<ConstantInt>(memSet->getLength());
      if (length && length->isZero())
        return true;
      const auto *byteVal = dyn_cast<ConstantInt>(memSet->getValue());
      const bool fullZero =
          byteVal && byteVal->isZero() &&
          (!length || length->getZExtValue() >= ptrSize);
      program->addStore(block, node(memSet->getDest()),
                        fullZero ? program->nullValue()
                                 : program->unknownValue(),
                        /*allowStrong=*/false);
      return true;
    }
    if (const auto *memTransfer = dyn_cast<MemTransferInst>(instruction)) {
      const auto *length = dyn_cast<ConstantInt>(memTransfer->getLength());
      if (length && length->isZero())
        return true;
      if (length && length->getZExtValue() < ptrSize) {
        // Partial-byte overwrite can synthesize an arbitrary pointer value.
        program->addStore(block, node(memTransfer->getDest()),
                          program->unknownValue(), /*allowStrong=*/false);
      } else {
        // Model pointer transfer *src -> *dest without injecting unknown.
        const NodeID tmp = program->addNode();
        program->addLoad(block, node(memTransfer->getSource()), tmp);
        program->addStore(block, node(memTransfer->getDest()), tmp,
                          /*allowStrong=*/false);
      }
      return true;
    }

    if (call.getType()->isPointerTy())
      program->addCopy(program->unknownValue(), node(&call));
    else if (typeContainsPointer(call.getType()))
      program->addCopy(program->unknownValue(), aggregateNode(&call));
    if (!call.doesNotAccessMemory() && !call.onlyReadsMemory()) {
      program->addStore(block, program->unknownValue(), program->unknownValue(),
                        /*allowStrong=*/false);
      for (const Use &arg : call.args()) {
        if (const NodeID c = carrierNode(arg.get()))
          program->addExternalEscape(block, c);
      }
    }
    return true;
  }

  // Model an external call (either direct or resolved indirectly).
  // Returns true if handled via a known spec (non-conservative), false if
  // treated conservatively as an opaque external call.
  bool modelExternalCall(BlockID block, const CallBase &call,
                         const Function *callee, CallSiteID existingSite = 0) {
    // Any indirect callsite reaching an external target (even readonly/no-op)
    // must be marked external so defined sibling targets cannot emit an unsound
    // strong callOutStore when the external target executes instead.
    if (existingSite)
      program->markCallExternal(existingSite, true);

    auto emitLoad = [&](NodeID ptr, NodeID res) {
      if (existingSite)
        program->addLoadAfterCall(existingSite, ptr, res);
      else
        program->addLoad(block, ptr, res);
    };
    auto emitStore = [&](NodeID ptr, NodeID val, bool allowStrong) {
      if (existingSite)
        program->addStoreAfterCall(existingSite, ptr, val, allowStrong);
      else
        program->addStore(block, ptr, val, allowStrong);
    };
    auto emitExternalEscape = [&](NodeID val) {
      if (!val || val == emptyValueNode)
        return;
      if (existingSite)
        program->addExternalEscapeAfterCall(existingSite, val);
      else
        program->addExternalEscape(block, val);
    };
    auto emitHandlerCall = [&](NodeID handlerPtr,
                               std::vector<NodeID> handlerArgs) {
      const CallSiteID cbCs =
          existingSite
              ? program->addCallSiteAfterCall(
                    existingSite, handlerPtr, 0, std::move(handlerArgs),
                    /*mayReturn=*/true, /*isInvoke=*/false)
              : program->addCallSite(block, handlerPtr, 0,
                                     std::move(handlerArgs),
                                     /*mayReturn=*/true, /*isInvoke=*/false);
      program->markCallExternal(cbCs, true);
    };

    const DataLayout &dl = module.getDataLayout();
    const uint64_t ptrSize = dl.getPointerSize() ? dl.getPointerSize() : 8;

    if (callee) {
      if (isAnnotationFunction(callee) || isDeallocator(callee))
        return true;
      if (isExitFunction(callee)) {
        // Normal exit() runs registered atexit/on_exit handlers before
        // terminating; _exit/_Exit/abort do not.
        if (callee->getName() == "exit" && atexitDispatchFn) {
          const CallSiteID dispCs =
              existingSite
                  ? program->addCallSiteAfterCall(
                        existingSite, 0, 0, {}, /*mayReturn=*/false,
                        /*isInvoke=*/false)
                  : program->addCallSite(block, 0, 0, {}, /*mayReturn=*/false,
                                         /*isInvoke=*/false);
          program->addCallTarget(dispCs, atexitDispatchFn);
        }
        return true;
      }

      // Explicitly validate byte-value and partial-length semantics for C
      // memory library functions (matching handleIntrinsic).
      const StringRef name = callee->getName();
      if ((name == "memset" || name == "__memset_chk") && call.arg_size() >= 3 &&
          call.getArgOperand(0)->getType()->isPointerTy()) {
        const NodeID dst = node(call.getArgOperand(0));
        if (call.getType()->isPointerTy())
          program->addCopy(dst, node(&call));
        const auto *length = dyn_cast<ConstantInt>(call.getArgOperand(2));
        if (!length || !length->isZero()) {
          const auto *byteVal = dyn_cast<ConstantInt>(call.getArgOperand(1));
          const bool fullZero =
              byteVal && byteVal->isZero() &&
              (!length || length->getZExtValue() >= ptrSize);
          emitStore(dst,
                    fullZero ? program->nullValue() : program->unknownValue(),
                    /*allowStrong=*/false);
        }
        return true;
      }
      if ((name == "memcpy" || name == "memmove" || name == "__memcpy_chk" ||
           name == "__memmove_chk" || name == "bcopy") &&
          call.arg_size() >= 3) {
        const unsigned dstIdx = (name == "bcopy") ? 1u : 0u;
        const unsigned srcIdx = (name == "bcopy") ? 0u : 1u;
        if (call.getArgOperand(dstIdx)->getType()->isPointerTy() &&
            call.getArgOperand(srcIdx)->getType()->isPointerTy()) {
          const NodeID dst = node(call.getArgOperand(dstIdx));
          const NodeID src = node(call.getArgOperand(srcIdx));
          if (call.getType()->isPointerTy())
            program->addCopy(dst, node(&call));
          const auto *length = dyn_cast<ConstantInt>(call.getArgOperand(2));
          if (!length || !length->isZero()) {
            if (length && length->getZExtValue() < ptrSize) {
              emitStore(dst, program->unknownValue(), /*allowStrong=*/false);
            } else {
              const NodeID tmp = program->addNode();
              emitLoad(src, tmp);
              emitStore(dst, tmp, /*allowStrong=*/false);
            }
          }
          return true;
        }
      }
      if ((name == "__strcpy_chk" || name == "__strncpy_chk" ||
           name == "__strcat_chk" || name == "__strncat_chk" ||
           name == "stpcpy" || name == "stpncpy") &&
          call.arg_size() >= 2 &&
          call.getArgOperand(0)->getType()->isPointerTy()) {
        if (call.getType()->isPointerTy())
          program->addCopy(node(call.getArgOperand(0)), node(&call));
        return true;
      }
      if ((name == "strpbrk" || name == "strchr" || name == "strrchr" ||
           name == "strstr" || name == "strcasestr" || name == "memchr" ||
           name == "memrchr" || name == "rawmemchr" || name == "memmem" ||
           name == "dirname" || name == "__xpg_basename") &&
          call.arg_size() >= 1 &&
          call.getArgOperand(0)->getType()->isPointerTy()) {
        if (call.getType()->isPointerTy()) {
          program->addCopy(node(call.getArgOperand(0)), node(&call));
          program->addCopy(program->nullValue(), node(&call));
        }
        return true;
      }
      if (name == "strsep" && call.arg_size() >= 1 &&
          call.getArgOperand(0)->getType()->isPointerTy()) {
        const NodeID stringp = node(call.getArgOperand(0));
        const NodeID tmp = program->addNode();
        emitLoad(stringp, tmp);
        emitStore(stringp, tmp, /*allowStrong=*/false);
        emitStore(stringp, program->nullValue(), /*allowStrong=*/false);
        if (call.getType()->isPointerTy()) {
          program->addCopy(tmp, node(&call));
          program->addCopy(program->nullValue(), node(&call));
        }
        return true;
      }
      if (name == "pthread_create" && call.arg_size() >= 4 &&
          call.getArgOperand(2)->getType()->isPointerTy()) {
        emitHandlerCall(node(call.getArgOperand(2)),
                        {carrierNode(call.getArgOperand(3))});
        return true;
      }
      if (name == "pthread_join") {
        if (call.arg_size() >= 2 &&
            call.getArgOperand(1)->getType()->isPointerTy() &&
            !isa<ConstantPointerNull>(call.getArgOperand(1))) {
          emitStore(node(call.getArgOperand(1)), program->unknownValue(),
                    /*allowStrong=*/false);
        }
        return true;
      }
      if ((name == "getcwd" || name == "__getcwd_chk") && call.arg_size() >= 1 &&
          call.getArgOperand(0)->getType()->isPointerTy()) {
        if (call.getType()->isPointerTy()) {
          const NodeID res = node(&call);
          program->addCopy(node(call.getArgOperand(0)), res);
          AllocatorInfo info;
          info.functionName = name.str();
          info.returnsPointer = true;
          const ObjectID obj = getOrCreateCallAllocObject(call, info);
          program->addCopy(program->address(obj), res);
          program->addCopy(program->nullValue(), res);
        }
        return true;
      }
      if ((name == "realpath" || name == "__realpath_chk") &&
          call.arg_size() >= 2 &&
          call.getArgOperand(1)->getType()->isPointerTy()) {
        if (call.getType()->isPointerTy()) {
          const NodeID res = node(&call);
          program->addCopy(node(call.getArgOperand(1)), res);
          AllocatorInfo info;
          info.functionName = name.str();
          info.returnsPointer = true;
          const ObjectID obj = getOrCreateCallAllocObject(call, info);
          program->addCopy(program->address(obj), res);
          program->addCopy(program->nullValue(), res);
        }
        return true;
      }
      if ((name == "freopen" || name == "freopen64") && call.arg_size() >= 3 &&
          call.getArgOperand(2)->getType()->isPointerTy()) {
        if (call.getType()->isPointerTy()) {
          const NodeID res = node(&call);
          program->addCopy(node(call.getArgOperand(2)), res);
          AllocatorInfo info;
          info.functionName = name.str();
          info.returnsPointer = true;
          const ObjectID obj = getOrCreateCallAllocObject(call, info);
          program->addCopy(program->address(obj), res);
          program->addCopy(program->nullValue(), res);
        }
        return true;
      }
      if (name == "__cxa_atexit" && call.arg_size() >= 2 &&
          call.getArgOperand(0)->getType()->isPointerTy()) {
        const NodeID fnNode = node(call.getArgOperand(0));
        const NodeID argCarrier = carrierNode(call.getArgOperand(1));
        if (cxaAtexitSlot) {
          emitStore(program->address(cxaAtexitSlot), fnNode,
                    /*allowStrong=*/false);
          if (argCarrier)
            emitStore(program->address(cxaAtexitArgSlot), argCarrier,
                      /*allowStrong=*/false);
        } else {
          emitHandlerCall(fnNode, {argCarrier});
        }
        return true;
      }
      if (name == "__cxa_throw") {
        if (call.arg_size() >= 1)
          emitExternalEscape(carrierNode(call.getArgOperand(0)));
        if (call.arg_size() >= 3 &&
            call.getArgOperand(2)->getType()->isPointerTy() &&
            !isa<ConstantPointerNull>(call.getArgOperand(2))) {
          emitHandlerCall(node(call.getArgOperand(2)),
                          {carrierNode(call.getArgOperand(0))});
        }
        return true;
      }
      if (name ==
              "_ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_" &&
          call.arg_size() >= 4 &&
          call.getArgOperand(1)->getType()->isPointerTy() &&
          call.getArgOperand(2)->getType()->isPointerTy() &&
          call.getArgOperand(3)->getType()->isPointerTy()) {
        const NodeID x = node(call.getArgOperand(1));
        const NodeID pNode = node(call.getArgOperand(2));
        const NodeID hdr = node(call.getArgOperand(3));
        const NodeID tmp = program->addNode();
        program->addCopy(x, tmp);
        program->addCopy(pNode, tmp);
        program->addCopy(hdr, tmp);
        emitStore(x, tmp, /*allowStrong=*/false);
        emitStore(pNode, tmp, /*allowStrong=*/false);
        emitStore(hdr, tmp, /*allowStrong=*/false);
        return true;
      }
      if (name ==
              "_ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_" &&
          call.arg_size() >= 2 &&
          call.getArgOperand(0)->getType()->isPointerTy() &&
          call.getArgOperand(1)->getType()->isPointerTy()) {
        const NodeID z = node(call.getArgOperand(0));
        const NodeID hdr = node(call.getArgOperand(1));
        const NodeID tmp = program->addNode();
        program->addCopy(z, tmp);
        program->addCopy(hdr, tmp);
        emitLoad(z, tmp);
        emitLoad(hdr, tmp);
        emitStore(z, tmp, /*allowStrong=*/false);
        emitStore(hdr, tmp, /*allowStrong=*/false);
        if (call.getType()->isPointerTy())
          program->addCopy(z, node(&call));
        return true;
      }
      if ((name == "_ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base" ||
           name == "_ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base" ||
           name == "_ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base" ||
           name == "_ZSt18_Rb_tree_decrementPKSt18_Rb_tree_node_base") &&
          call.arg_size() >= 1 &&
          call.getArgOperand(0)->getType()->isPointerTy()) {
        if (call.getType()->isPointerTy()) {
          const NodeID res = node(&call);
          program->addCopy(node(call.getArgOperand(0)), res);
          emitLoad(res, res);
        }
        return true;
      }
      if (isCppCharStringMethod(name)) {
        const bool isConst = name.startswith("_ZNK");
        const NodeID self =
            (call.arg_size() >= 1 &&
             call.getArgOperand(0)->getType()->isPointerTy())
                ? node(call.getArgOperand(0))
                : 0;
        if (!isConst && self) {
          AllocatorInfo info;
          info.functionName = "std::string";
          info.returnsPointer = true;
          const ObjectID buf = getOrCreateCallAllocObject(call, info);
          emitStore(self, program->address(buf), /*allowStrong=*/false);
          if (name.contains("swap") && call.arg_size() >= 2 &&
              call.getArgOperand(1)->getType()->isPointerTy()) {
            const NodeID other = node(call.getArgOperand(1));
            const NodeID t1 = program->addNode();
            const NodeID t2 = program->addNode();
            emitLoad(self, t1);
            emitLoad(other, t2);
            emitStore(self, t2, /*allowStrong=*/false);
            emitStore(other, t1, /*allowStrong=*/false);
          }
        }
        if (call.getType()->isPointerTy()) {
          const NodeID res = node(&call);
          if (self) {
            program->addCopy(self, res);
            if (!call.getArgOperand(0)->getType()->isPointerTy() ||
                call.getType() != call.getArgOperand(0)->getType()) {
              emitLoad(self, res);
            }
          } else {
            program->addCopy(program->unknownValue(), res);
          }
        }
        return true;
      }
      if (isCppCharStreamNoCallbackMethod(name)) {
        if (call.getType()->isPointerTy()) {
          if (call.arg_size() >= 1 &&
              call.getArgOperand(0)->getType()->isPointerTy()) {
            program->addCopy(node(call.getArgOperand(0)), node(&call));
          } else {
            program->addCopy(program->unknownValue(), node(&call));
          }
        }
        return true;
      }

      if ((name == "bzero" || name == "explicit_bzero") &&
          call.arg_size() >= 2 &&
          call.getArgOperand(0)->getType()->isPointerTy()) {
        const NodeID dst = node(call.getArgOperand(0));
        const auto *length = dyn_cast<ConstantInt>(call.getArgOperand(1));
        if (!length || !length->isZero()) {
          const bool fullZero = !length || length->getZExtValue() >= ptrSize;
          emitStore(dst,
                    fullZero ? program->nullValue() : program->unknownValue(),
                    /*allowStrong=*/false);
        }
        return true;
      }
      if ((name == "qsort" || name == "qsort_r") && call.arg_size() >= 4 &&
          call.getArgOperand(0)->getType()->isPointerTy() &&
          call.getArgOperand(3)->getType()->isPointerTy()) {
        const NodeID base = node(call.getArgOperand(0));
        const NodeID compar = node(call.getArgOperand(3));
        std::vector<NodeID> cbArgs{base, base};
        if (name == "qsort_r" && call.arg_size() >= 5)
          cbArgs.push_back(carrierNode(call.getArgOperand(4)));
        emitHandlerCall(compar, std::move(cbArgs));
        // Swapping elements inside the array preserves pointer contents.
        const NodeID tmp = program->addNode();
        emitLoad(base, tmp);
        emitStore(base, tmp, /*allowStrong=*/false);
        return true;
      }
      if (name == "bsearch" && call.arg_size() >= 5 &&
          call.getArgOperand(0)->getType()->isPointerTy() &&
          call.getArgOperand(1)->getType()->isPointerTy() &&
          call.getArgOperand(4)->getType()->isPointerTy()) {
        const NodeID key = node(call.getArgOperand(0));
        const NodeID base = node(call.getArgOperand(1));
        const NodeID compar = node(call.getArgOperand(4));
        emitHandlerCall(compar, {key, base});
        if (call.getType()->isPointerTy()) {
          program->addCopy(base, node(&call));
          program->addCopy(program->nullValue(), node(&call));
        }
        return true;
      }
      if ((name == "strtoll" || name == "strtoull" || name == "strtold" ||
           name == "strtoimax" || name == "strtoumax") &&
          call.arg_size() >= 2 &&
          call.getArgOperand(0)->getType()->isPointerTy() &&
          call.getArgOperand(1)->getType()->isPointerTy()) {
        emitStore(node(call.getArgOperand(1)), node(call.getArgOperand(0)),
                  /*allowStrong=*/false);
        return true;
      }
      if (name == "strtok_r" && call.arg_size() >= 3 &&
          call.getArgOperand(0)->getType()->isPointerTy() &&
          call.getArgOperand(2)->getType()->isPointerTy()) {
        const NodeID str = node(call.getArgOperand(0));
        const NodeID saveptr = node(call.getArgOperand(2));
        emitStore(saveptr, str, /*allowStrong=*/false);
        if (call.getType()->isPointerTy()) {
          const NodeID res = node(&call);
          program->addCopy(str, res);
          emitLoad(saveptr, res);
          program->addCopy(program->nullValue(), res);
        }
        return true;
      }
      if (name == "getaddrinfo" && call.arg_size() >= 4 &&
          call.getArgOperand(3)->getType()->isPointerTy()) {
        AllocatorInfo info;
        info.functionName = "getaddrinfo";
        info.returnsPointer = false;
        info.ptrOutArgIndex = 3;
        const ObjectID obj = getOrCreateCallAllocObject(call, info);
        emitStore(node(call.getArgOperand(3)), program->address(obj),
                  /*allowStrong=*/false);
        emitStore(program->address(obj), program->address(obj),
                  /*allowStrong=*/false);
        return true;
      }
      if ((name == "localtime_r" || name == "gmtime_r" || name == "ctime_r" ||
           name == "asctime_r") &&
          call.arg_size() >= 2 &&
          call.getArgOperand(1)->getType()->isPointerTy()) {
        if (call.getType()->isPointerTy()) {
          program->addCopy(node(call.getArgOperand(1)), node(&call));
          program->addCopy(program->nullValue(), node(&call));
        }
        return true;
      }
      if (builtinStaticReturnFunction(callee)) {
        if (call.getType()->isPointerTy()) {
          const ObjectID staticObj = getOrCreateStaticReturnObject(callee);
          program->addCopy(program->address(staticObj), node(&call));
          program->addCopy(program->nullValue(), node(&call));
        }
        return true;
      }

      // Explicitly model callback-registering C standard/POSIX functions
      // before AliasSpecManager (ptr.spec marks atexit/sigaction as IGNORE and
      // signal as COPY-only, which would otherwise omit handler invocation).
      // atexit/on_exit handlers are deferred to exit() and the post-entry
      // teardown loop; signal/sigaction handlers may fire immediately after
      // registration AND later (modeled via both immediate call and teardown
      // loop).
      if (name == "atexit" && call.arg_size() >= 1 &&
          call.getArgOperand(0)->getType()->isPointerTy()) {
        if (atexitSlot) {
          emitStore(program->address(atexitSlot), node(call.getArgOperand(0)),
                    /*allowStrong=*/false);
        } else {
          emitHandlerCall(node(call.getArgOperand(0)), {});
        }
        return true;
      }
      if (name == "on_exit" && call.arg_size() >= 2 &&
          call.getArgOperand(0)->getType()->isPointerTy()) {
        const Value *arg1 = call.getArgOperand(1);
        if (onExitSlot) {
          emitStore(program->address(onExitSlot), node(call.getArgOperand(0)),
                    /*allowStrong=*/false);
          if (const NodeID argCarrier = carrierNode(arg1)) {
            emitStore(program->address(onExitArgSlot), argCarrier,
                      /*allowStrong=*/false);
          }
        } else {
          emitHandlerCall(node(call.getArgOperand(0)),
                          {0, carrierNode(arg1)});
        }
        return true;
      }
      if ((name == "signal" || name == "__sysv_signal" || name == "sigset") &&
          call.arg_size() >= 2 &&
          call.getArgOperand(1)->getType()->isPointerTy()) {
        const NodeID handler = node(call.getArgOperand(1));
        if (call.getType()->isPointerTy()) {
          program->addCopy(handler, node(&call));
          program->addCopy(program->unknownValue(), node(&call));
        }
        if (signalSlot)
          emitStore(program->address(signalSlot), handler,
                    /*allowStrong=*/false);
        emitHandlerCall(handler, {});
        return true;
      }
      if (name == "sigaction" && call.arg_size() >= 2 &&
          call.getArgOperand(1)->getType()->isPointerTy()) {
        const NodeID handler = program->addNode();
        emitLoad(node(call.getArgOperand(1)), handler);
        if (signalSlot)
          emitStore(program->address(signalSlot), handler,
                    /*allowStrong=*/false);
        emitHandlerCall(handler, {});
        if (call.arg_size() >= 3 &&
            call.getArgOperand(2)->getType()->isPointerTy()) {
          emitStore(node(call.getArgOperand(2)), program->unknownValue(),
                    /*allowStrong=*/false);
        }
        return true;
      }

      if (auto allocInfo = getAllocatorInfo(callee)) {
        const ObjectID obj = getOrCreateCallAllocObject(call, *allocInfo);
        if (allocInfo->returnsPointer && call.getType()->isPointerTy()) {
          const NodeID result = node(&call);
          program->addCopy(program->address(obj), result);
          program->addCopy(program->nullValue(), result);
        } else {
          // Covers both explicit OUT-arg allocators (posix_memalign) and
          // specs marked ALLOC that return int/ssize_t and write *arg0
          // (asprintf, getline, getdelim).
          const int outIdx =
              allocInfo->ptrOutArgIndex >= 0 ? allocInfo->ptrOutArgIndex : 0;
          if (static_cast<unsigned>(outIdx) < call.arg_size()) {
            const Value *outArg = call.getArgOperand(outIdx);
            if (outArg->getType()->isPointerTy()) {
              emitStore(node(outArg), program->address(obj),
                        /*allowStrong=*/false);
            }
          }
        }
        if (isReallocatorName(callee->getName()) && call.arg_size() > 0 &&
            call.getArgOperand(0)->getType()->isPointerTy()) {
          const NodeID oldPtr = node(call.getArgOperand(0));
          if (call.getType()->isPointerTy())
            program->addCopy(oldPtr, node(&call));
          // realloc copies contents of *oldPtr into the new object.
          const NodeID copied = program->addNode();
          emitLoad(oldPtr, copied);
          emitStore(program->address(obj), copied, /*allowStrong=*/false);
        }
        return true;
      }

      // Check AliasSpecManager copy / return-alias specs (cached per callee).
      bool modeledBySpec = false;
      if (specManager) {
        const auto &extSpec = getExtDeclSpecWithEffects(callee);
        for (const auto &copy : extSpec.copyEffects) {
          modeledBySpec = true;
          if (copy.returnsAlias && call.getType()->isPointerTy()) {
            const int idx =
                copy.retArgIndex >= 0 ? copy.retArgIndex : copy.dstArgIndex;
            if (idx >= 0 && static_cast<unsigned>(idx) < call.arg_size() &&
                call.getArgOperand(idx)->getType()->isPointerTy()) {
              program->addCopy(node(call.getArgOperand(idx)), node(&call));
            }
          }
          if (copy.dstArgIndex >= 0 &&
              static_cast<unsigned>(copy.dstArgIndex) < call.arg_size() &&
              call.getArgOperand(copy.dstArgIndex)->getType()->isPointerTy()) {
            const NodeID dst = node(call.getArgOperand(copy.dstArgIndex));
            if (copy.srcArgIndex >= 0 &&
                static_cast<unsigned>(copy.srcArgIndex) < call.arg_size()) {
              const Value *srcVal = call.getArgOperand(copy.srcArgIndex);
              if (srcVal->getType()->isPointerTy()) {
                const NodeID src = node(srcVal);
                if (copy.srcIsRegion) {
                  const NodeID tmp = program->addNode();
                  emitLoad(src, tmp);
                  emitStore(dst, tmp, /*allowStrong=*/false);
                } else {
                  // Covers both `ArgN R ArgM V` and `ArgN D ArgM V`
                  // (e.g., strtol/strtod writing Arg0 into *Arg1).
                  emitStore(dst, src, /*allowStrong=*/false);
                }
              }
            } else if (copy.srcArgIndex < 0) {
              // srcArgIndex < 0 represents NULL or STATIC in AliasSpecManager.
              emitStore(dst, program->unknownValue(), /*allowStrong=*/false);
            }
          }
        }
        const auto &retAliases = extSpec.returnAliases;
        if (!retAliases.empty()) {
          modeledBySpec = true;
          if (call.getType()->isPointerTy()) {
            const NodeID res = node(&call);
            for (const auto &ret : retAliases) {
              if (ret.isNull)
                program->addCopy(program->nullValue(), res);
              if (ret.isStatic) {
                const ObjectID staticObj =
                    getOrCreateStaticReturnObject(callee);
                program->addCopy(program->address(staticObj), res);
              }
              if (ret.argIndex >= 0 &&
                  static_cast<unsigned>(ret.argIndex) < call.arg_size() &&
                  call.getArgOperand(ret.argIndex)->getType()->isPointerTy()) {
                const NodeID arg = node(call.getArgOperand(ret.argIndex));
                if (ret.isRegion) {
                  emitLoad(arg, res);
                } else {
                  program->addCopy(arg, res);
                }
              }
            }
          }
        }
        if (modeledBySpec)
          return true;
      }

      if (isNoEffectFunction(callee)) {
        if (call.getType()->isPointerTy())
          program->addCopy(program->unknownValue(), node(&call));
        else if (typeContainsPointer(call.getType()))
          program->addCopy(program->unknownValue(), aggregateNode(&call));
        return true;
      }
    }

    // Opaque external call: conservative modeling.
    const bool readOnly =
        call.doesNotAccessMemory() || call.onlyReadsMemory() ||
        (callee &&
         (callee->doesNotAccessMemory() || callee->onlyReadsMemory()));
    const bool noCallback =
        call.hasFnAttr(Attribute::NoCallback) ||
        (callee && callee->hasFnAttribute(Attribute::NoCallback));
    if (call.getType()->isPointerTy())
      program->addCopy(program->unknownValue(), node(&call));
    else if (typeContainsPointer(call.getType()))
      program->addCopy(program->unknownValue(), aggregateNode(&call));
    if (!readOnly) {
      emitStore(program->unknownValue(), program->unknownValue(),
                /*allowStrong=*/false);
      // Any pointer or pointer-containing aggregate passed to an opaque
      // external function may be saved in external/global state and later
      // accessed via `unknown`.
      for (const Use &arg : call.args())
        emitExternalEscape(carrierNode(arg.get()));
    }
    if (!noCallback) {
      // Route opaque external callbacks through a single shared dispatcher
      // function (`O(|Calls| + |AddrTaken|)` edges instead of quadratic
      // `O(|Calls| * |AddrTaken|)`), ensuring callbacks receive `unknown`
      // arguments and `byval` seeds without blowing up the call graph.
      if (const FunctionID cbDispatcher =
              getOrCreateExternalCallbackDispatcher()) {
        const CallSiteID cbSite =
            existingSite
                ? program->addCallSiteAfterCall(existingSite, 0, 0, {},
                                                mayReturn(call, callee),
                                                isa<InvokeInst>(call))
                : program->addCallSite(block, 0, 0, {}, mayReturn(call, callee),
                                       isa<InvokeInst>(call));
        program->markCallExternal(cbSite, true);
        program->addExternalCallback(cbSite, cbDispatcher);
      }
    }
    return false;
  }

  void buildInstructions() {
    for (const GlobalVariable &global : module.globals()) {
      if (!global.hasInitializer())
        continue;
      const ObjectID obj = objects.lookup(&global);
      if (global.isConstant() && global.hasDefinitiveInitializer())
        program->setObjectReadOnly(obj, true);
      NodeID initial = 0;
      bool addedAny = false;
      collectInitializer(global.getInitializer(), initial, addedAny);
      if (addedAny)
        program->setInitializer(obj, initial);
      else
        program->setEmptyInitializer(obj);
    }

    for (const Function &function : module) {
      if (function.isDeclaration())
        continue;
      const auto &fnInfo = functions.find(&function)->second;

      for (const BasicBlock &block : function) {
        const BlockID current = blockIds.lookup(&block);
        for (const Instruction &instruction : block) {
          if (const auto *alloca = dyn_cast<AllocaInst>(&instruction)) {
            const ObjectID obj = objects.lookup(alloca);
            if (program->object(obj).singleton) {
              // Strong reset for a singleton stack slot on activation without
              // injecting `unknown`.
              program->addStore(current, node(alloca), emptyValueNode,
                                /*allowStrong=*/true);
            } else if (!config.pointerOnlyMemory) {
              program->addStore(current, node(alloca), program->unknownValue(),
                                /*allowStrong=*/false);
            }
            continue;
          }

          if (const auto *load = dyn_cast<LoadInst>(&instruction)) {
            if (load->getType()->isPointerTy()) {
              program->addLoad(current, node(load->getPointerOperand()),
                               node(load));
            } else if (typeContainsPointer(load->getType())) {
              program->addLoad(current, node(load->getPointerOperand()),
                               aggregateNode(load));
            }
          } else if (const auto *store = dyn_cast<StoreInst>(&instruction)) {
            const Value *val = store->getValueOperand();
            const NodeID ptrNode = node(store->getPointerOperand());
            if (val->getType()->isPointerTy()) {
              const AccessID id = program->addStore(current, ptrNode, node(val),
                                                    /*allowStrong=*/true);
              const NodeID storedNode = program->access(id).value;
              storeNodes[store] = storedNode;
              recordNodeValue(storedNode, store);
            } else if (!config.pointerOnlyMemory) {
              const AccessID id = program->addStore(
                  current, ptrNode, program->unknownValue(), false);
              const NodeID storedNode = program->access(id).value;
              storeNodes[store] = storedNode;
              recordNodeValue(storedNode, store);
            } else if (typeContainsPointer(val->getType())) {
              if (!isa<UndefValue>(val) && !isa<PoisonValue>(val)) {
                const NodeID aggSrc = aggregateNode(val);
                if (aggSrc != emptyValueNode) {
                  const AccessID id =
                      program->addStore(current, ptrNode, aggSrc,
                                        /*allowStrong=*/false);
                  const NodeID storedNode = program->access(id).value;
                  storeNodes[store] = storedNode;
                  recordNodeValue(storedNode, store);
                }
              }
            } else if (containsPtrToInt(val)) {
              const AccessID id = program->addStore(
                  current, ptrNode, program->unknownValue(), false);
              const NodeID storedNode = program->access(id).value;
              storeNodes[store] = storedNode;
              recordNodeValue(storedNode, store);
            }
          } else if (const auto *phi = dyn_cast<PHINode>(&instruction)) {
            if (phi->getType()->isPointerTy()) {
              for (const Use &incoming : phi->incoming_values())
                program->addCopy(node(incoming.get()), node(phi));
            } else if (typeContainsPointer(phi->getType())) {
              const NodeID dst = aggregateNode(phi);
              for (const Use &incoming : phi->incoming_values()) {
                const NodeID src = aggregateNode(incoming.get());
                if (src != emptyValueNode)
                  program->addCopy(src, dst);
              }
            }
          } else if (const auto *select = dyn_cast<SelectInst>(&instruction)) {
            if (select->getType()->isPointerTy()) {
              program->addCopy(node(select->getTrueValue()), node(select));
              program->addCopy(node(select->getFalseValue()), node(select));
            } else if (typeContainsPointer(select->getType())) {
              const NodeID dst = aggregateNode(select);
              const NodeID t = aggregateNode(select->getTrueValue());
              const NodeID f = aggregateNode(select->getFalseValue());
              if (t != emptyValueNode)
                program->addCopy(t, dst);
              if (f != emptyValueNode)
                program->addCopy(f, dst);
            }
          } else if (const auto *gep =
                         dyn_cast<GetElementPtrInst>(&instruction)) {
            if (gep->getType()->isPointerTy()) {
              program->addCopy(node(gep->getPointerOperand()), node(gep));
            } else if (typeContainsPointer(gep->getType())) {
              const NodeID src = aggregateNode(gep->getPointerOperand());
              if (src != emptyValueNode)
                program->addCopy(src, aggregateNode(gep));
            }
          } else if (const auto *cast = dyn_cast<CastInst>(&instruction)) {
            const Value *op0 = cast->getOperand(0);
            if (isa<PtrToIntInst>(cast)) {
              if (const NodeID c = carrierNode(op0))
                program->addExternalEscape(current, c);
            } else if (isa<BitCastInst>(cast) &&
                       !cast->getType()->isPointerTy() &&
                       !typeContainsPointer(cast->getType()) &&
                       typeContainsPointer(op0->getType())) {
              if (const NodeID c = carrierNode(op0))
                program->addExternalEscape(current, c);
            }
            if (cast->getType()->isPointerTy()) {
              program->addCopy(node(op0), node(cast));
            } else if (typeContainsPointer(cast->getType())) {
              const NodeID dst = aggregateNode(cast);
              if (op0->getType()->isPointerTy() ||
                  typeContainsPointer(op0->getType())) {
                const NodeID src = aggregateNode(op0);
                if (src != emptyValueNode)
                  program->addCopy(src, dst);
              } else {
                program->addCopy(program->unknownValue(), dst);
              }
            }
          } else if (const auto *freeze = dyn_cast<FreezeInst>(&instruction)) {
            if (freeze->getType()->isPointerTy()) {
              program->addCopy(node(freeze->getOperand(0)), node(freeze));
            } else if (typeContainsPointer(freeze->getType())) {
              const NodeID src = aggregateNode(freeze->getOperand(0));
              if (src != emptyValueNode)
                program->addCopy(src, aggregateNode(freeze));
            }
          } else if (const auto *ins =
                         dyn_cast<InsertValueInst>(&instruction)) {
            if (typeContainsPointer(ins->getType())) {
              const NodeID dst = aggregateNode(ins);
              const Value *agg = ins->getAggregateOperand();
              if (!isa<UndefValue>(agg) && !isa<PoisonValue>(agg)) {
                const NodeID src = aggregateNode(agg);
                if (src != emptyValueNode)
                  program->addCopy(src, dst);
              }
              const Value *elem = ins->getInsertedValueOperand();
              if (elem->getType()->isPointerTy() ||
                  typeContainsPointer(elem->getType()) ||
                  containsPtrToInt(elem)) {
                const NodeID src = aggregateNode(elem);
                if (src != emptyValueNode)
                  program->addCopy(src, dst);
              }
            }
          } else if (const auto *ins =
                         dyn_cast<InsertElementInst>(&instruction)) {
            if (typeContainsPointer(ins->getType())) {
              const NodeID dst = aggregateNode(ins);
              const Value *vec = ins->getOperand(0);
              if (!isa<UndefValue>(vec) && !isa<PoisonValue>(vec)) {
                const NodeID src = aggregateNode(vec);
                if (src != emptyValueNode)
                  program->addCopy(src, dst);
              }
              const Value *elem = ins->getOperand(1);
              if (elem->getType()->isPointerTy() ||
                  typeContainsPointer(elem->getType()) ||
                  containsPtrToInt(elem)) {
                const NodeID src = aggregateNode(elem);
                if (src != emptyValueNode)
                  program->addCopy(src, dst);
              }
            }
          } else if (const auto *shuf =
                         dyn_cast<ShuffleVectorInst>(&instruction)) {
            if (typeContainsPointer(shuf->getType())) {
              const NodeID dst = aggregateNode(shuf);
              for (unsigned i = 0; i < 2; ++i) {
                const Value *op = shuf->getOperand(i);
                if (!isa<UndefValue>(op) && !isa<PoisonValue>(op)) {
                  const NodeID src = aggregateNode(op);
                  if (src != emptyValueNode)
                    program->addCopy(src, dst);
                }
              }
            }
          } else if (const auto *rmw = dyn_cast<AtomicRMWInst>(&instruction)) {
            if (rmw->getType()->isPointerTy()) {
              program->addLoad(current, node(rmw->getPointerOperand()),
                               node(rmw));
              program->addStore(current, node(rmw->getPointerOperand()),
                                node(rmw->getValOperand()), false);
            } else if (!config.pointerOnlyMemory ||
                       containsPtrToInt(rmw->getValOperand())) {
              program->addStore(current, node(rmw->getPointerOperand()),
                                program->unknownValue(), false);
            }
          } else if (const auto *cmp =
                         dyn_cast<AtomicCmpXchgInst>(&instruction)) {
            const NodeID cmpRes = atomicResults.lookup(cmp);
            if (cmp->getNewValOperand()->getType()->isPointerTy()) {
              program->addLoad(current, node(cmp->getPointerOperand()), cmpRes);
              program->addStore(current, node(cmp->getPointerOperand()),
                                node(cmp->getNewValOperand()), false);
              if (typeContainsPointer(cmp->getType()))
                program->addCopy(cmpRes, aggregateNode(cmp));
            } else if (!config.pointerOnlyMemory ||
                       containsPtrToInt(cmp->getNewValOperand())) {
              program->addLoad(current, node(cmp->getPointerOperand()), cmpRes);
              program->addStore(current, node(cmp->getPointerOperand()),
                                program->unknownValue(), false);
            }
          } else if (const auto *extract =
                         dyn_cast<ExtractValueInst>(&instruction)) {
            if (extract->getType()->isPointerTy()) {
              const auto *cmp =
                  dyn_cast<AtomicCmpXchgInst>(extract->getAggregateOperand());
              if (cmp && extract->getNumIndices() == 1 &&
                  *extract->idx_begin() == 0) {
                program->addCopy(atomicResults.lookup(cmp), node(extract));
              } else {
                const NodeID src = aggregateNode(extract->getAggregateOperand());
                if (src != emptyValueNode)
                  program->addCopy(src, node(extract));
              }
            } else if (typeContainsPointer(extract->getType())) {
              const NodeID src = aggregateNode(extract->getAggregateOperand());
              if (src != emptyValueNode)
                program->addCopy(src, aggregateNode(extract));
            }
          } else if (const auto *extractElem =
                         dyn_cast<ExtractElementInst>(&instruction)) {
            if (extractElem->getType()->isPointerTy()) {
              const NodeID src = aggregateNode(extractElem->getVectorOperand());
              if (src != emptyValueNode)
                program->addCopy(src, node(extractElem));
            } else if (typeContainsPointer(extractElem->getType())) {
              const NodeID src = aggregateNode(extractElem->getVectorOperand());
              if (src != emptyValueNode)
                program->addCopy(src, aggregateNode(extractElem));
            }
          } else if (const auto *call = dyn_cast<CallBase>(&instruction)) {
            if (handleIntrinsic(*call, current))
              continue;
            if (call->isInlineAsm()) {
              if (call->getType()->isPointerTy())
                program->addCopy(program->unknownValue(), node(call));
              else if (typeContainsPointer(call->getType()))
                program->addCopy(program->unknownValue(), aggregateNode(call));
              if (!call->doesNotAccessMemory() && !call->onlyReadsMemory()) {
                program->addStore(current, program->unknownValue(),
                                  program->unknownValue(),
                                  /*allowStrong=*/false);
                for (const Use &arg : call->args()) {
                  if (const NodeID c = carrierNode(arg.get()))
                    program->addExternalEscape(current, c);
                }
              }
              continue;
            }
            const Function *callee = directCallee(*call);
            if (callee && !callee->isDeclaration()) {
              std::vector<NodeID> actualArgs;
              actualArgs.reserve(call->arg_size());
              for (unsigned i = 0; i < call->arg_size(); ++i) {
                const NodeID c = carrierNode(call->getArgOperand(i));
                actualArgs.push_back(c);
                // Variadic pointer/carrier arguments may be extracted via
                // va_arg (modeled as unknown) in the callee or a forwarded
                // va_list helper.
                if (callee->isVarArg() && i >= callee->arg_size() && c)
                  program->addExternalEscape(current, c);
              }
              const NodeID resNode = carrierNode(call);
              const CallSiteID cs = program->addCallSite(
                  current, 0, resNode, std::move(actualArgs),
                  mayReturn(*call, callee), isa<InvokeInst>(*call));
              program->addCallTarget(cs, functions.find(callee)->second.id);
            } else if (callee && callee->isDeclaration()) {
              const bool specModeled = modelExternalCall(current, *call, callee);
              externalCalls.push_back({call, current, !specModeled});
            } else {
              // Indirect call: resolved dynamically inside vfg::Solver.
              std::vector<NodeID> actualArgs;
              actualArgs.reserve(call->arg_size());
              const FunctionType *fnTy = call->getFunctionType();
              for (unsigned i = 0; i < call->arg_size(); ++i) {
                const NodeID c = carrierNode(call->getArgOperand(i));
                actualArgs.push_back(c);
                if (fnTy && fnTy->isVarArg() && i >= fnTy->getNumParams() && c)
                  program->addExternalEscape(current, c);
              }
              const NodeID resNode = carrierNode(call);
              const NodeID calleePtr = node(call->getCalledOperand());
              const CallSiteID cs = program->addCallSite(
                  current, calleePtr, resNode, std::move(actualArgs),
                  mayReturn(*call), isa<InvokeInst>(*call));
              const bool readOnly =
                  call->doesNotAccessMemory() || call->onlyReadsMemory();
              const bool noCallback = call->hasFnAttr(Attribute::NoCallback);
              program->setUnknownExternalTarget(
                  cs, getOrCreateUnknownExternalCallee(readOnly, noCallback),
                  /*escapeArgs=*/!readOnly);
              indirectCalls.push_back(
                  {call, current, cs, calleePtr, {}, false});
            }
          } else if (const auto *ret = dyn_cast<ReturnInst>(&instruction)) {
            const Value *value = ret->getReturnValue();
            if (fnInfo.returned && value) {
              if (const NodeID c = carrierNode(value))
                program->addCopy(c, fnInfo.returned);
            }
          } else {
            if (instruction.getType()->isPointerTy())
              program->addCopy(program->unknownValue(), node(&instruction));
            else if (typeContainsPointer(instruction.getType()))
              program->addCopy(program->unknownValue(),
                               aggregateNode(&instruction));
            if (instruction.mayWriteToMemory())
              program->addStore(current, program->unknownValue(),
                                program->unknownValue(), false);
          }
        }

        const Instruction *terminator = block.getTerminator();
        if (succ_empty(&block)) {
          if (!isa<UnreachableInst>(terminator))
            program->addControlEdge(current, fnInfo.exit);
        } else {
          for (const BasicBlock *successor : successors(&block))
            program->addControlEdge(current, blockIds.lookup(successor));
        }
      }
    }
  }

  void run() {
    analyzed = false;
    solver.reset();
    program = std::make_unique<vfg::Program>();
    stats = {};
    emptyValueNode = 0;
    atexitSlot = 0;
    onExitSlot = 0;
    onExitArgSlot = 0;
    cxaAtexitSlot = 0;
    cxaAtexitArgSlot = 0;
    signalSlot = 0;
    atexitDispatchFn = 0;
    externalCallbackFn = 0;
    externalCallbackBuilt = false;
    for (FunctionID &fn : unknownExternalCalleeFns)
      fn = 0;
    values.clear();
    aggregateNodes.clear();
    storeNodes.clear();
    nodeValues.clear();
    objects.clear();
    objectValues.assign(3, nullptr);
    inverse.clear();
    inverseBuilt = false;
    blockIds.clear();
    atomicResults.clear();
    staticReturnObjects.clear();
    callAllocObjects.clear();
    typeHasPtrCache.clear();
    ptrToIntCache.clear();
    extDeclCache.clear();
    functions.clear();
    functionById.clear();
    externalCalls.clear();
    indirectCalls.clear();
    entryPoints.clear();

    if (config.useExternalSpecs) {
      static thread_local const AliasSpecManager sharedSpecs;
      specManager = &sharedSpecs;
    } else {
      specManager = nullptr;
    }

    collectAddressTakenFunctions();
    collectObjectsAndFunctions();
    setupEntryPointsAndLifecycle();
    buildInstructions();

    vfg::Solver::Config solverConfig;
    solverConfig.enableStrongUpdates = config.enableStrongUpdates;

    // Precompute bitsets of function objects so post-solve indirect-call scans
    // skip non-function objects in O(|function chunks|) instead of O(|pts|).
    vfg::PointsToSet extDeclFuncObjects;
    vfg::PointsToSet definedFuncObjects;
    for (ObjectID obj = 1; obj < objectValues.size(); ++obj) {
      const auto *fn = dyn_cast_or_null<Function>(objectValues[obj]);
      if (!fn)
        continue;
      if (fn->isDeclaration())
        extDeclFuncObjects.insertBit(obj);
      else
        definedFuncObjects.insertBit(obj);
    }

    // Defined indirect targets (including `byval` parameters) and unknown
    // external targets resolve dynamically inside a single Solver::analyze()
    // pass. Only loop again if a reachable indirect call discovers a concrete
    // external declaration with pointer-visible effects not already subsumed.
    while (true) {
      solver = std::make_unique<vfg::Solver>(*program, solverConfig);
      solver->analyze();
      ++stats.callGraphIterations;

      struct PendingExternal {
        std::size_t idx;
        const Function *callee; // nullptr means unknown external
      };
      std::vector<PendingExternal> newExternals;

      for (std::size_t i = 0; i < indirectCalls.size(); ++i) {
        auto &rec = indirectCalls[i];
        if (!solver->isBlockReachable(rec.block))
          continue;

        const auto &calleePts = solver->pointsTo(rec.calledPointer);
        const bool hasUnknown = calleePts.count(program->unknownObject()) != 0;
        if (hasUnknown)
          rec.modeledUnknownExternal = true;
        const bool callReadOnly = rec.instruction->doesNotAccessMemory() ||
                                  rec.instruction->onlyReadsMemory();
        const bool callNoCallback =
            rec.instruction->hasFnAttr(Attribute::NoCallback);
        const bool fullUnknownActive =
            hasUnknown && !callReadOnly && !callNoCallback;
        calleePts.forEachIn(extDeclFuncObjects, [&](ObjectID obj) {
          const auto *callee = cast<Function>(objectValues[obj]);
          if (rec.modeledExternalTargets.count(callee))
            return;
          if (fullUnknownActive) {
            const StringRef nm = callee->getName();
            const bool registersDeferred =
                nm == "atexit" || nm == "on_exit" || nm == "__cxa_atexit" ||
                nm == "signal" || nm == "__sysv_signal" || nm == "sigset" ||
                nm == "sigaction" || nm == "pthread_create" ||
                nm == "thrd_create";
            const bool needsConcreteAlloc =
                getAllocatorInfo(callee).has_value() &&
                rec.instruction->getType()->isPointerTy();
            const bool needsStaticRet =
                builtinStaticReturnFunction(callee) &&
                rec.instruction->getType()->isPointerTy();
            if (!needsConcreteAlloc && !needsStaticRet && !registersDeferred) {
              rec.modeledExternalTargets.insert(callee);
              return;
            }
          } else if (isNoEffectFunction(callee) &&
                     !rec.instruction->getType()->isPointerTy() &&
                     !typeContainsPointer(rec.instruction->getType())) {
            rec.modeledExternalTargets.insert(callee);
            externalCalls.push_back(
                {rec.instruction, rec.block, /*conservative=*/false});
            return;
          }
          newExternals.push_back({i, callee});
        });
      }

      if (newExternals.empty())
        break;

      solver.reset();
      for (const auto &ext : newExternals) {
        auto &rec = indirectCalls[ext.idx];
        if (ext.callee)
          rec.modeledExternalTargets.insert(ext.callee);
        else
          rec.modeledUnknownExternal = true;
        const CallBase *inst = rec.instruction;
        const BlockID blk = rec.block;
        const CallSiteID site = rec.site;
        const bool specModeled =
            modelExternalCall(blk, *inst, ext.callee, site);
        externalCalls.push_back({inst, blk, !specModeled});
      }
    }

    // Collect final statistics from the converged solver.
    stats.solver = solver->statistics();
    stats.resolvedIndirectTargets = 0;
    stats.conservativeExternalCalls = 0;
    stats.specModeledCalls = 0;
    for (const auto &rec : indirectCalls) {
      if (!solver->isBlockReachable(rec.block))
        continue;
      if (rec.modeledUnknownExternal) {
        solver->pointsTo(rec.calledPointer)
            .forEachIn(definedFuncObjects, [&](ObjectID) {
              ++stats.resolvedIndirectTargets;
            });
      } else {
        for (FunctionID fnId : solver->resolvedCallTargets(rec.site)) {
          if (fnId < functionById.size() && functionById[fnId])
            ++stats.resolvedIndirectTargets;
        }
      }
      stats.resolvedIndirectTargets += rec.modeledExternalTargets.size();
      if (rec.modeledUnknownExternal)
        ++stats.conservativeExternalCalls;
    }
    for (const auto &rec : externalCalls) {
      if (!solver->isBlockReachable(rec.block))
        continue;
      if (rec.conservative)
        ++stats.conservativeExternalCalls;
      else
        ++stats.specModeledCalls;
    }

    // Inverse index over tracked LLVM Values is populated lazily on
    // getPointedToBy() queries to avoid gigabytes of unused set allocations.
    inverse.clear();
    inverseBuilt = false;

    stats.functions = program->functionCount();
    stats.callSites = program->callSiteCount();
    stats.objects = program->objectCount();
    stats.valueNodes = program->nodeCount();
    stats.controlBlocks = program->blockCount();
    analyzed = true;
  }

  void ensureInverse() const {
    if (inverseBuilt)
      return;
    inverseBuilt = true;
    inverse.assign(program->objectCount() + 1, {});
    for (const auto &entry : values)
      for (ObjectID object : solver->pointsTo(entry.second))
        inverse[object].insert(entry.first);
  }

  void requireAnalyzed() const {
    if (!analyzed)
      throw std::logic_error("ValueFlowPTA: analyze() must precede queries");
  }
};

ValueFlowPTA::ValueFlowPTA(const Module &module)
    : ValueFlowPTA(module, Config{}) {}
ValueFlowPTA::ValueFlowPTA(const Module &module, Config config)
    : impl_(std::make_unique<Impl>(module, std::move(config))) {}
ValueFlowPTA::~ValueFlowPTA() = default;

void ValueFlowPTA::analyze() { impl_->run(); }

const ValueFlowPTA::PointsToSet &
ValueFlowPTA::getPointsTo(const Value *value) const {
  impl_->requireAnalyzed();
  static const PointsToSet empty;
  const auto it = impl_->values.find(value);
  return it == impl_->values.end() ? empty
                                   : impl_->solver->pointsTo(it->second);
}

const ValueFlowPTA::PointedToBySet &
ValueFlowPTA::getPointedToBy(ObjectID object) const {
  impl_->requireAnalyzed();
  impl_->ensureInverse();
  if (!object || object >= impl_->inverse.size())
    throw std::out_of_range("ValueFlowPTA: invalid object");
  return impl_->inverse[object];
}

ValueFlowPTA::PointedToByStoreSet
ValueFlowPTA::getPointedToByStores(ObjectID object) const {
  impl_->requireAnalyzed();
  PointedToByStoreSet result;
  for (NodeID id : impl_->solver->pointedToBy(object)) {
    if (id < impl_->nodeValues.size()) {
      if (const auto *store = dyn_cast_or_null<StoreInst>(impl_->nodeValues[id]))
        result.insert(store);
    }
  }
  return result;
}

bool ValueFlowPTA::mayAlias(const Value *left, const Value *right) const {
  impl_->requireAnalyzed();
  auto a = impl_->values.find(left), b = impl_->values.find(right);
  if (a == impl_->values.end() || b == impl_->values.end())
    return true;
  return impl_->solver->mayAlias(a->second, b->second);
}

ValueFlowPTA::ObjectID
ValueFlowPTA::getObjectId(const Value *allocation) const {
  impl_->requireAnalyzed();
  auto it = impl_->objects.find(allocation);
  return it == impl_->objects.end() ? 0 : it->second;
}

const Value *ValueFlowPTA::getObjectValue(ObjectID object) const {
  impl_->requireAnalyzed();
  return object < impl_->objectValues.size() ? impl_->objectValues[object]
                                             : nullptr;
}

ValueFlowPTA::ObjectID ValueFlowPTA::getNullObjectId() const {
  impl_->requireAnalyzed();
  return impl_->program->nullObject();
}

ValueFlowPTA::ObjectID ValueFlowPTA::getUnknownObjectId() const {
  impl_->requireAnalyzed();
  return impl_->program->unknownObject();
}

const ValueFlowPTA::Statistics &ValueFlowPTA::getStatistics() const {
  impl_->requireAnalyzed();
  return impl_->stats;
}

const vfg::Program &ValueFlowPTA::getProgram() const {
  impl_->requireAnalyzed();
  return *impl_->program;
}

const vfg::Solver &ValueFlowPTA::getSolver() const {
  impl_->requireAnalyzed();
  return *impl_->solver;
}

ValueFlowPTA::NodeID ValueFlowPTA::getValueNode(const Value *value) const {
  impl_->requireAnalyzed();
  auto it = impl_->values.find(value);
  return it == impl_->values.end() ? 0 : it->second;
}

ValueFlowPTA::NodeID ValueFlowPTA::getStoreNode(const StoreInst *store) const {
  impl_->requireAnalyzed();
  auto it = impl_->storeNodes.find(store);
  return it == impl_->storeNodes.end() ? 0 : it->second;
}

const Value *ValueFlowPTA::getNodeValue(NodeID node) const {
  impl_->requireAnalyzed();
  return node < impl_->nodeValues.size() ? impl_->nodeValues[node] : nullptr;
}

vfg::NodeSet ValueFlowPTA::getVFGSuccessors(NodeID node) const {
  impl_->requireAnalyzed();
  return impl_->solver->vfgSuccessors(node);
}

vfg::NodeSet ValueFlowPTA::getVFGIndirectSuccessors(NodeID node) const {
  impl_->requireAnalyzed();
  return impl_->solver->vfgIndirectSuccessors(node);
}

} // namespace lotus::alias
