#include "Checker/KINT/Log.h"
#include "Checker/KINT/MKintAnalysisSupport.h"
#include "Checker/KINT/Options.h"

#include <llvm/ADT/StringSet.h>

using namespace llvm;

namespace kint {

uint64_t MKintPass::getStableValueId(const Value *v) {
  auto it = m_value_ids.find(v);
  if (it != m_value_ids.end())
    return it->second;
  const uint64_t id = ++m_next_value_id;
  m_value_ids[v] = id;
  return id;
}

void MKintPass::pushSymFrame() {
  m_sym_change_frames.push_back(m_sym_change_log.size());
}

void MKintPass::pushObjectFrame() {
  m_obj_change_frames.push_back(m_obj_change_log.size());
}

void MKintPass::popObjectFrame() {
  if (m_obj_change_frames.empty())
    return;
  const size_t frameStart = m_obj_change_frames.back();
  m_obj_change_frames.pop_back();

  while (m_obj_change_log.size() > frameStart) {
    ObjChange ch = std::move(m_obj_change_log.back());
    m_obj_change_log.pop_back();
    switch (ch.kind) {
    case ObjChangeKind::ObjBase:
      if (ch.hadOld)
        m_obj_base[ch.key] = std::move(ch.oldExpr);
      else
        m_obj_base.erase(ch.key);
      break;
    case ObjChangeKind::ObjSize:
      if (ch.hadOld)
        m_obj_size[ch.key] = std::move(ch.oldExpr);
      else
        m_obj_size.erase(ch.key);
      break;
    case ObjChangeKind::ObjListPush:
      if (!m_obj_list.empty())
        m_obj_list.pop_back();
      break;
    case ObjChangeKind::ObjMem:
      if (ch.hadOld)
        m_obj_mem[ch.key] = std::move(ch.oldExpr);
      else
        m_obj_mem.erase(ch.key);
      break;
    case ObjChangeKind::ObjAlias:
      if (ch.hadOld)
        m_obj_alias[ch.key] = ch.oldPtr;
      else
        m_obj_alias.erase(ch.key);
      break;
    case ObjChangeKind::IntAlias:
      if (ch.hadOld)
        m_int_alias[ch.key] = ch.oldPtr;
      else
        m_int_alias.erase(ch.key);
      break;
    case ObjChangeKind::PtrOffset:
      if (ch.hadOld)
        m_ptr_offset[ch.key] = std::move(ch.oldExpr);
      else
        m_ptr_offset.erase(ch.key);
      break;
    case ObjChangeKind::IntOffset:
      if (ch.hadOld)
        m_int_offset[ch.key] = std::move(ch.oldExpr);
      else
        m_int_offset.erase(ch.key);
      break;
    case ObjChangeKind::ObjFreed:
      if (!ch.hadOld)
        m_obj_freed.erase(ch.key);
      break;
    case ObjChangeKind::ObjSizeKnown:
      if (!ch.hadOld)
        m_obj_size_known.erase(ch.key);
      break;
    }
  }
}

void MKintPass::setObjBase(const Value *k, const std::optional<z3::expr> &v) {
  if (!m_obj_change_frames.empty()) {
    ObjChange ch;
    ch.kind = ObjChangeKind::ObjBase;
    ch.key = k;
    auto it = m_obj_base.find(k);
    ch.hadOld = (it != m_obj_base.end());
    if (ch.hadOld)
      ch.oldExpr = it->second;
    m_obj_change_log.push_back(std::move(ch));
  }
  m_obj_base[k] = v;
}

void MKintPass::setObjSize(const Value *k, const std::optional<z3::expr> &v) {
  if (!m_obj_change_frames.empty()) {
    ObjChange ch;
    ch.kind = ObjChangeKind::ObjSize;
    ch.key = k;
    auto it = m_obj_size.find(k);
    ch.hadOld = (it != m_obj_size.end());
    if (ch.hadOld)
      ch.oldExpr = it->second;
    m_obj_change_log.push_back(std::move(ch));
  }
  m_obj_size[k] = v;
}

void MKintPass::pushObjList(const Value *v) {
  if (!m_obj_change_frames.empty()) {
    ObjChange ch;
    ch.kind = ObjChangeKind::ObjListPush;
    ch.key = v;
    m_obj_change_log.push_back(std::move(ch));
  }
  m_obj_list.push_back(v);
}

void MKintPass::setObjMem(const Value *k, const std::optional<z3::expr> &v) {
  if (!m_obj_change_frames.empty()) {
    ObjChange ch;
    ch.kind = ObjChangeKind::ObjMem;
    ch.key = k;
    auto it = m_obj_mem.find(k);
    ch.hadOld = (it != m_obj_mem.end());
    if (ch.hadOld)
      ch.oldExpr = it->second;
    m_obj_change_log.push_back(std::move(ch));
  }
  m_obj_mem[k] = v;
}

void MKintPass::setObjAlias(const Value *k, const Value *v) {
  if (!m_obj_change_frames.empty()) {
    ObjChange ch;
    ch.kind = ObjChangeKind::ObjAlias;
    ch.key = k;
    auto it = m_obj_alias.find(k);
    ch.hadOld = (it != m_obj_alias.end());
    if (ch.hadOld)
      ch.oldPtr = it->second;
    m_obj_change_log.push_back(std::move(ch));
  }
  if (v)
    m_obj_alias[k] = v;
  else
    m_obj_alias.erase(k);
}

void MKintPass::setIntAlias(const Value *k, const Value *v) {
  if (!m_obj_change_frames.empty()) {
    ObjChange ch;
    ch.kind = ObjChangeKind::IntAlias;
    ch.key = k;
    auto it = m_int_alias.find(k);
    ch.hadOld = (it != m_int_alias.end());
    if (ch.hadOld)
      ch.oldPtr = it->second;
    m_obj_change_log.push_back(std::move(ch));
  }
  if (v)
    m_int_alias[k] = v;
  else
    m_int_alias.erase(k);
}

void MKintPass::setPtrOffset(const Value *k, const std::optional<z3::expr> &v) {
  if (!m_obj_change_frames.empty()) {
    ObjChange ch;
    ch.kind = ObjChangeKind::PtrOffset;
    ch.key = k;
    auto it = m_ptr_offset.find(k);
    ch.hadOld = (it != m_ptr_offset.end());
    if (ch.hadOld)
      ch.oldExpr = it->second;
    m_obj_change_log.push_back(std::move(ch));
  }
  if (v.has_value())
    m_ptr_offset[k] = v;
  else
    m_ptr_offset.erase(k);
}

void MKintPass::setIntOffset(const Value *k, const std::optional<z3::expr> &v) {
  if (!m_obj_change_frames.empty()) {
    ObjChange ch;
    ch.kind = ObjChangeKind::IntOffset;
    ch.key = k;
    auto it = m_int_offset.find(k);
    ch.hadOld = (it != m_int_offset.end());
    if (ch.hadOld)
      ch.oldExpr = it->second;
    m_obj_change_log.push_back(std::move(ch));
  }
  if (v.has_value())
    m_int_offset[k] = v;
  else
    m_int_offset.erase(k);
}

void MKintPass::markObjFreed(const Value *k) {
  const bool existed = m_obj_freed.contains(k);
  if (!existed && !m_obj_change_frames.empty()) {
    ObjChange ch;
    ch.kind = ObjChangeKind::ObjFreed;
    ch.key = k;
    ch.hadOld = false;
    m_obj_change_log.push_back(std::move(ch));
  }
  m_obj_freed.insert(k);
}

void MKintPass::markObjSizeKnown(const Value *k) {
  const bool existed = m_obj_size_known.contains(k);
  if (!existed && !m_obj_change_frames.empty()) {
    ObjChange ch;
    ch.kind = ObjChangeKind::ObjSizeKnown;
    ch.key = k;
    ch.hadOld = false;
    m_obj_change_log.push_back(std::move(ch));
  }
  m_obj_size_known.insert(k);
}

bool MKintPass::isKnownReadOnlyLibcCall(StringRef name) {
  static const llvm::StringSet<> kReadOnly = {
      // String / memory inspection
      "strlen",      "strnlen",     "strcmp",      "strncmp",
      "strcasecmp",  "strncasecmp", "memcmp",      "bcmp",
      "strchr",      "strrchr",     "strstr",      "memchr",
      "strspn",      "strcspn",     "strpbrk",
      // Character classification & numeric parsing
      "isalpha",     "isdigit",     "isalnum",     "isspace",
      "isupper",     "islower",     "isxdigit",    "isprint",
      "iscntrl",     "ispunct",     "toupper",     "tolower",
      "atoi",        "atol",        "atoll",       "abs",
      "labs",        "llabs",
      // Byte order & address helpers
      "htons",       "htonl",       "ntohs",       "ntohl",
      "inet_addr",   "inet_ntoa",   "strerror",    "strsignal",
      // Stdio / logging / output-only syscalls (do not mutate user stack/heap)
      "printf",      "fprintf",     "vprintf",     "vfprintf",
      "puts",        "putchar",     "fputs",       "fputc",
      "putc",        "fwrite",      "fflush",      "perror",
      "syslog",      "openlog",     "closelog",    "fseek",
      "fseeko",      "ftell",       "ftello",      "rewind",
      "ferror",      "feof",        "clearerr",    "fileno",
      // File/process/socket syscalls that take read-only pointers or scalars
      "open",        "open64",      "openat",      "creat",
      "creat64",     "fopen",       "fopen64",     "freopen",
      "fdopen",      "opendir",     "closedir",    "close",
      "fclose",      "fsync",       "fdatasync",   "lseek",
      "lseek64",     "access",      "faccessat",   "chdir",
      "fchdir",      "chroot",      "unlink",      "unlinkat",
      "remove",      "rename",      "renameat",    "mkdir",
      "mkdirat",     "rmdir",       "chmod",       "fchmod",
      "chown",       "fchown",      "lchown",      "link",
      "symlink",     "socket",      "bind",        "connect",
      "listen",      "shutdown",    "setsockopt",  "send",
      "sendto",      "write",       "pwrite",      "dup",
      "dup2",        "dup3",        "umask",       "setuid",
      "setgid",      "seteuid",     "setegid",     "setreuid",
      "setregid",    "setgroups",   "initgroups",  "setsid",
      "setpgid",     "fork",        "vfork",       "kill",
      "alarm",       "raise",       "sleep",       "usleep",
      "getpid",      "getppid",     "getuid",      "geteuid",
      "getgid",      "getegid",     "isatty",      "getenv",
      "setenv",      "unsetenv",    "putenv",      "system",
      "popen",       "pclose",      "execl",       "execv",
      "execve",      "execvp",
  };
  return kReadOnly.contains(name);
}

bool MKintPass::isKnownArgMemOnlyLibcCall(StringRef name) {
  static const llvm::StringSet<> kArgMemOnly = {
      "read",        "pread",       "recv",        "recvfrom",
      "fgets",       "fread",       "getline",     "getdelim",
      "stat",        "stat64",      "lstat",       "lstat64",
      "fstat",       "fstat64",     "accept",      "getsockname",
      "getpeername", "getsockopt",  "getcwd",      "readlink",
      "realpath",    "gettimeofday","clock_gettime","time",
      "localtime_r", "gmtime_r",    "ctime_r",     "strtol",
      "strtoul",     "strtoll",     "strtoull",    "strtod",
      "sprintf",     "snprintf",    "vsprintf",    "vsnprintf",
      "strcpy",      "strncpy",     "strcat",      "strncat",
      "stpcpy",      "stpncpy",     "sscanf",      "fscanf",
      "scanf",       "pipe",        "socketpair",  "wait",
      "waitpid",     "sigemptyset", "sigfillset",  "sigaddset",
      "sigdelset",   "sigprocmask", "sigaction",   "bzero",
  };
  return kArgMemOnly.contains(name);
}

bool MKintPass::isKnownNoReturnLibcCall(StringRef name) {
  static const llvm::StringSet<> kNoReturn = {
      "exit",         "_exit",         "_Exit",
      "abort",        "__assert_fail", "__assert_rtn",
      "err",          "errx",          "verr",
      "verrx",        "longjmp",       "siglongjmp",
      "__stack_chk_fail",
  };
  return kNoReturn.contains(name);
}

void MKintPass::popSymFrame() {
  if (m_sym_change_frames.empty())
    return;
  const size_t frameStart = m_sym_change_frames.back();
  m_sym_change_frames.pop_back();

  while (m_sym_change_log.size() > frameStart) {
    const SymChange ch = m_sym_change_log.back();
    m_sym_change_log.pop_back();
    if (!ch.key)
      continue;
    if (ch.hadOld) {
      m_v2sym[ch.key] = ch.oldValue;
    } else {
      m_v2sym.erase(ch.key);
    }
  }
}

void MKintPass::setSym(const Value *v, const z3::expr &e) {
  if (!m_sym_change_frames.empty()) {
    SymChange ch;
    ch.key = v;
    auto it = m_v2sym.find(v);
    ch.hadOld = (it != m_v2sym.end());
    if (ch.hadOld)
      ch.oldValue = it->second;
    m_sym_change_log.push_back(std::move(ch));
  }
  m_v2sym[v] = e;
}

void MKintPass::eraseSym(const Value *v) {
  auto it = m_v2sym.find(v);
  if (it == m_v2sym.end())
    return;
  if (!m_sym_change_frames.empty()) {
    SymChange ch;
    ch.key = v;
    ch.hadOld = true;
    ch.oldValue = it->second;
    m_sym_change_log.push_back(std::move(ch));
  }
  m_v2sym.erase(it);
}

void MKintPass::pushConstraintFrame() {
  m_constraint_frames.push_back(m_path_constraints.size());
}

void MKintPass::popConstraintFrame() {
  if (m_constraint_frames.empty())
    return;
  const size_t frameStart = m_constraint_frames.back();
  m_constraint_frames.pop_back();
  while (m_path_constraints.size() > frameStart) {
    m_path_constraints.pop_back();
  }
}

void MKintPass::addConstraint(const z3::expr &e) {
  m_solver.value().add(e);
  if (m_robust_reachability || m_building_summary) {
    m_path_constraints.push_back(e);
  }
}

const Value *MKintPass::resolveAliasedObject(const Value *obj) const {
  const Value *cur = obj;
  DenseSet<const Value *> seen;
  while (cur) {
    if (!seen.insert(cur).second)
      break;
    auto it = m_obj_alias.find(cur);
    if (it == m_obj_alias.end() || !it->second)
      break;
    cur = it->second;
  }
  return cur;
}

const Value *MKintPass::commonPointerObject(const Value *a,
                                            const Value *b) const {
  const Value *ra = a ? resolveAliasedObject(a) : nullptr;
  const Value *rb = b ? resolveAliasedObject(b) : nullptr;
  if (!ra || !rb || ra != rb)
    return nullptr;
  return ra;
}

const Value *MKintPass::getPointerOrigin(const Value *v) const {
  if (!v)
    return nullptr;
  if (v->getType()->isPointerTy())
    return getObjectForPtr(v);
  auto it = m_int_alias.find(v);
  if (it == m_int_alias.end() || !it->second)
    return nullptr;
  return resolveAliasedObject(it->second);
}

std::optional<z3::expr> MKintPass::getPointerOffset(const Value *v) const {
  if (!v)
    return std::nullopt;
  if (v->getType()->isPointerTy()) {
    auto it = m_ptr_offset.find(v);
    if (it != m_ptr_offset.end())
      return it->second;
    return std::nullopt;
  }
  auto it = m_int_offset.find(v);
  if (it != m_int_offset.end())
    return it->second;
  return std::nullopt;
}

const Value *MKintPass::getObjectForPtr(const Value *ptr) const {
  if (!ptr)
    return nullptr;
  if (m_obj_base.count(ptr)) {
    return resolveAliasedObject(ptr);
  }
  if (auto it = m_obj_alias.find(ptr); it != m_obj_alias.end() && it->second) {
    const Value *aliased = resolveAliasedObject(it->second);
    if (aliased && m_obj_base.count(aliased))
      return aliased;
  }
  const Value *stripped = ptr->stripPointerCasts();
  const Value *obj = llvm::getUnderlyingObject(stripped);
  obj = resolveAliasedObject(obj);
  if ((!obj || !m_obj_base.count(obj)) && stripped != ptr) {
    if (auto it = m_obj_alias.find(stripped);
        it != m_obj_alias.end() && it->second) {
      obj = resolveAliasedObject(it->second);
    }
  }
  if (obj && m_obj_base.count(obj))
    return obj;
  return nullptr;
}

z3::expr MKintPass::loadBytesFromMem(const z3::expr &mem,
                                     const z3::expr &offset, unsigned numBytes,
                                     bool littleEndian) const {
  auto &ctx = m_solver.value().ctx();
  if (numBytes == 0)
    return ctx.bv_val(0, 8);
  z3::expr result = z3::select(
      mem, offset + ctx.bv_val(littleEndian ? (numBytes - 1) : 0, m_ptr_bits));
  for (unsigned i = 1; i < numBytes; ++i) {
    const unsigned byteIndex = littleEndian ? (numBytes - 1 - i) : i;
    z3::expr b = z3::select(mem, offset + ctx.bv_val(byteIndex, m_ptr_bits));
    result = z3::concat(result, b);
  }
  return result;
}

z3::expr MKintPass::storeBytesToMem(const z3::expr &mem, const z3::expr &offset,
                                    const z3::expr &value, unsigned numBytes,
                                    bool littleEndian) const {
  auto &ctx = m_solver.value().ctx();
  if (numBytes == 0)
    return mem;
  const unsigned targetBits = numBytes * 8;
  z3::expr v = value;
  const unsigned vbw = v.get_sort().bv_size();
  if (vbw < targetBits)
    v = z3::zext(v, targetBits - vbw);
  else if (vbw > targetBits)
    v = v.extract(targetBits - 1, 0);

  z3::expr cur = mem;
  for (unsigned i = 0; i < numBytes; ++i) {
    const unsigned valueByteIndex = littleEndian ? i : (numBytes - 1 - i);
    const unsigned lo = valueByteIndex * 8;
    const unsigned hi = lo + 7;
    z3::expr byteVal = v.extract(hi, lo);
    cur = z3::store(cur, offset + ctx.bv_val(i, m_ptr_bits), byteVal);
  }
  return cur;
}

void MKintPass::havocObject(const Value *obj, const std::string &hint) {
  if (!obj)
    return;
  auto &ctx = m_solver.value().ctx();
  const auto id = g_obj_mem_id.fetch_add(1, std::memory_order_relaxed);
  const std::string name = "%objmem." + hint + "." + std::to_string(id);
  setObjMem(obj,
            ctx.constant(name.c_str(), ctx.array_sort(ctx.bv_sort(m_ptr_bits),
                                                      ctx.bv_sort(8))));
}

void MKintPass::havocObjectRange(const Value *obj, const z3::expr &offset,
                                 uint64_t numBytes, const std::string &hint) {
  if (!obj || numBytes == 0)
    return;
  constexpr uint64_t kMaxPreciseHavocBytes = 256;
  if (numBytes > kMaxPreciseHavocBytes || !m_obj_mem.count(obj) ||
      !m_obj_mem[obj].has_value()) {
    havocObject(obj, hint);
    return;
  }

  auto &ctx = m_solver.value().ctx();
  z3::expr curMem = m_obj_mem[obj].value();
  for (uint64_t i = 0; i < numBytes; ++i) {
    const auto id = g_obj_mem_id.fetch_add(1, std::memory_order_relaxed);
    const std::string name =
        "%objmem." + hint + "." + std::to_string(id) + ".b" + std::to_string(i);
    z3::expr fresh = ctx.bv_const(name.c_str(), 8);
    curMem = z3::store(curMem, offset + ctx.bv_val(i, m_ptr_bits), fresh);
  }
  setObjMem(obj, curMem);
}

static bool sameExpr(const z3::expr &a, const z3::expr &b) {
  if (&a.ctx() != &b.ctx())
    return false;
  return Z3_get_ast_id(a.ctx(), a) == Z3_get_ast_id(b.ctx(), b);
}

void MKintPass::invalidateObject(const Value *obj, const std::string &hint) {
  if (!obj)
    return;
  markObjFreed(obj);
  havocObject(obj, hint);
}

bool MKintPass::callMayModObject(llvm::CallBase *call, const Value *obj) const {
  if (!call || !obj || !m_aa)
    return true;
  const Value *resolved = resolveAliasedObject(obj);
  if (resolved && m_obj_freed.contains(resolved))
    return false;
  llvm::LocationSize size = llvm::LocationSize::afterPointer();
  if (resolved && m_obj_size.count(resolved)) {
    if (auto sizeExpr = m_obj_size.find(resolved)->second) {
      if (sizeExpr->is_numeral()) {
        uint64_t bytes = 0;
        if (Z3_get_numeral_uint64(sizeExpr->ctx(), *sizeExpr, &bytes)) {
          size = llvm::LocationSize::precise(bytes);
        }
      }
    }
  }
  llvm::MemoryLocation loc(resolved ? resolved : obj, size);
  auto modref = m_aa->getModRefInfo(call, loc);
  return llvm::isModSet(modref);
}

z3::expr MKintPass::buildPathConstraintConjunction() const {
  auto &ctx = m_solver.value().ctx();
  if (m_path_constraints.empty())
    return ctx.bool_val(true);
  z3::expr_vector pcs(ctx);
  for (const auto &c : m_path_constraints)
    pcs.push_back(c);
  return z3::mk_and(pcs);
}

void MKintPass::registerUniversal(const z3::expr &e) {
  if (!m_robust_reachability)
    return;
  Z3_ast key = e;
  if (m_universal_var_ids.insert(key).second) {
    m_universal_vars.push_back(e);
  }
}

void MKintPass::dumpEfConstraint(const Instruction *inst, interr type,
                                 const z3::expr &q) const {
  if (m_dump_ef_path.empty())
    return;
  std::ofstream out(m_dump_ef_path, std::ios::app);
  if (!out.is_open())
    return;
  out << "=== EF Constraint ===\n";
  if (inst && inst->getParent() && inst->getParent()->getParent()) {
    out << "Function: " << inst->getParent()->getParent()->getName().str()
        << "\n";
  }
  if (type != interr::NONE) {
    out << "Bug: " << bugTypeToString(type) << "\n";
  }
  if (inst) {
    std::string instStr;
    llvm::raw_string_ostream instOS(instStr);
    instOS << *inst;
    out << "Inst: " << instOS.str() << "\n";
  }
  out << q.to_string() << "\n";
  out << "=====================\n";
}

bool MKintPass::checkBugCondition(const Instruction *inst, interr type,
                                  const z3::expr &bugCond) {
  if (m_building_summary)
    return false;
  if (!m_robust_reachability) {
    m_solver.value().push();
    m_solver.value().add(bugCond);
    const bool sat = (m_solver.value().check() == z3::sat);
    m_solver.value().pop();
    return sat;
  }
  if (type != interr::NONE && !isRobustBugEnabled(type)) {
    return false;
  }

  auto &ctx = m_solver.value().ctx();
  z3::solver qsolver(ctx);
  z3::expr body = buildPathConstraintConjunction() && bugCond;
  z3::expr q = body;
  if (!m_universal_vars.empty()) {
    z3::expr_vector uvars(ctx);
    for (const auto &v : m_universal_vars)
      uvars.push_back(v);
    q = z3::forall(uvars, body);
    qsolver.add(q);
  } else {
    qsolver.add(body);
  }
  dumpEfConstraint(inst, type, q);
  return qsolver.check() == z3::sat;
}

bool MKintPass::isRobustBugEnabled(interr type) const {
  if (m_robust_bug_filter.empty())
    return true;
  return m_robust_bug_filter.count(type) > 0;
}

void MKintPass::parseRobustBugFilter(const std::string &csv) {
  m_robust_bug_filter.clear();
  if (csv.empty())
    return;
  size_t start = 0;
  while (start <= csv.size()) {
    size_t end = csv.find(',', start);
    if (end == std::string::npos)
      end = csv.size();
    auto token = csv.substr(start, end - start);
    auto trim = [](const std::string &s) {
      size_t b = s.find_first_not_of(" \t");
      size_t e = s.find_last_not_of(" \t");
      if (b == std::string::npos)
        return std::string();
      return s.substr(b, e - b + 1);
    };
    token = trim(token);
    if (token == "int-overflow") {
      m_robust_bug_filter.insert(interr::INT_OVERFLOW);
    } else if (token == "div-by-zero") {
      m_robust_bug_filter.insert(interr::DIV_BY_ZERO);
    } else if (token == "bad-shift") {
      m_robust_bug_filter.insert(interr::BAD_SHIFT);
    } else if (token == "array-oob") {
      m_robust_bug_filter.insert(interr::ARRAY_OOB);
    } else if (token == "dead-branch") {
      m_robust_bug_filter.insert(interr::DEAD_TRUE_BR);
      m_robust_bug_filter.insert(interr::DEAD_FALSE_BR);
    }
    start = end + 1;
  }
}

bool MKintPass::isLittleEndian() const {
  return m_dl ? m_dl->isLittleEndian() : true;
}
bool MKintPass::tryInitializeGlobalMemory(const GlobalVariable *gv) {
  if (!gv || !m_dl || !gv->hasDefinitiveInitializer())
    return false;
  if (!gv->isConstant() && !(m_in_main_entry && !m_has_global_ctors))
    return false;
  const Constant *init = gv->getInitializer();
  if (!init)
    return false;
  auto &ctx = m_solver.value().ctx();

  // O(1) exact zero-initialization for any size (including large BSS arrays).
  if (isa<ConstantAggregateZero>(init)) {
    setObjMem(gv, z3::const_array(ctx.bv_sort(m_ptr_bits), ctx.bv_val(0, 8)));
    return true;
  }

  constexpr uint64_t kMaxInitBytes = 256;
  const auto totalBytesOpt = getTypeAllocSizeSafe(m_dl, gv->getValueType());
  if (!totalBytesOpt.has_value() || *totalBytesOpt == 0 ||
      *totalBytesOpt > kMaxInitBytes)
    return false;

  const uint64_t totalBytes = *totalBytesOpt;
  const bool littleEndian = isLittleEndian();
  std::vector<std::optional<uint8_t>> byteMap(totalBytes, static_cast<uint8_t>(0));

  auto markUnknown = [&](uint64_t off, uint64_t len) {
    const uint64_t end = std::min(totalBytes, off + len);
    for (uint64_t i = off; i < end; ++i)
      byteMap[i] = std::nullopt;
  };

  auto fillConst = [&](const Constant *c, uint64_t offset,
                       const auto &self) -> void {
    if (!c)
      return;
    const auto szOpt = getTypeStoreSizeSafe(m_dl, c->getType());
    if (!szOpt.has_value())
      return;
    const uint64_t sz = *szOpt;
    if (offset + sz > totalBytes)
      return;

    if (isa<ConstantAggregateZero>(c) || isa<ConstantPointerNull>(c)) {
      return; // already zero-initialized
    }
    if (isa<UndefValue>(c)) {
      markUnknown(offset, sz);
      return;
    }
    if (const auto *ci = dyn_cast<ConstantInt>(c)) {
      const APInt &val = ci->getValue();
      const unsigned bits = val.getBitWidth();
      for (uint64_t i = 0; i < sz; ++i) {
        const uint64_t byteIdx = littleEndian ? i : (sz - 1 - i);
        const unsigned bitLo = static_cast<unsigned>(byteIdx * 8);
        uint8_t b = 0;
        if (bitLo < bits) {
          const unsigned take = std::min(8u, bits - bitLo);
          b = static_cast<uint8_t>(val.extractBits(take, bitLo).getZExtValue());
        }
        byteMap[offset + i] = b;
      }
      return;
    }
    if (const auto *cds = dyn_cast<ConstantDataSequential>(c)) {
      const StringRef raw = cds->getRawDataValues();
      const uint64_t n = std::min<uint64_t>(sz, raw.size());
      for (uint64_t i = 0; i < n; ++i) {
        byteMap[offset + i] = static_cast<uint8_t>(raw[i]);
      }
      return;
    }
    if (const auto *ca = dyn_cast<ConstantArray>(c)) {
      const auto elemSzOpt =
          getTypeAllocSizeSafe(m_dl, ca->getType()->getElementType());
      if (!elemSzOpt.has_value()) {
        markUnknown(offset, sz);
        return;
      }
      for (unsigned i = 0; i < ca->getNumOperands(); ++i) {
        self(ca->getOperand(i), offset + i * (*elemSzOpt), self);
      }
      return;
    }
    if (const auto *cs = dyn_cast<ConstantStruct>(c)) {
      auto *st = cs->getType();
      if (!st->isSized()) {
        markUnknown(offset, sz);
        return;
      }
      const auto *layout = m_dl->getStructLayout(st);
      for (unsigned i = 0; i < cs->getNumOperands(); ++i) {
        self(cs->getOperand(i), offset + layout->getElementOffset(i), self);
      }
      return;
    }
    // Unsupported constant element (e.g. float, global address relocation):
    // leave only this subrange unconstrained.
    markUnknown(offset, sz);
  };

  fillConst(init, 0, fillConst);

  z3::expr mem = z3::const_array(ctx.bv_sort(m_ptr_bits), ctx.bv_val(0, 8));
  for (uint64_t i = 0; i < totalBytes; ++i) {
    if (!byteMap[i].has_value()) {
      const auto id = g_obj_mem_id.fetch_add(1, std::memory_order_relaxed);
      const std::string name = "%ginit.unk." + std::to_string(id);
      mem = z3::store(mem, ctx.bv_val(i, m_ptr_bits),
                      ctx.bv_const(name.c_str(), 8));
    } else if (*byteMap[i] != 0) {
      mem = z3::store(mem, ctx.bv_val(i, m_ptr_bits),
                      ctx.bv_val(static_cast<unsigned>(*byteMap[i]), 8));
    }
  }
  setObjMem(gv, mem);
  return true;
}

bool MKintPass::ensureGlobalObject(const GlobalVariable *gv) {
  if (!gv)
    return false;
  if (m_obj_base.count(gv))
    return true;
  auto &ctx = m_solver.value().ctx();
  const uint64_t id = getStableValueId(gv);
  const std::string baseName =
      "global." + gv->getName().str() + "." + std::to_string(id);
  if (const auto bytes = getTypeAllocSizeSafe(m_dl, gv->getValueType())) {
    ensureObject(gv, baseName, ctx.bv_val(*bytes, m_ptr_bits),
                 /*sizeKnown=*/true, /*hasRealSize=*/true);
    tryInitializeGlobalMemory(gv);
  } else {
    auto sizeSym =
        ctx.bv_const(("global.size." + std::to_string(id)).c_str(), m_ptr_bits);
    ensureObject(gv, baseName, sizeSym, /*sizeKnown=*/false,
                 /*hasRealSize=*/false);
  }
  return true;
}

bool MKintPass::ensureAllocaObject(const AllocaInst *ai, BasicBlock *cur,
                                   BasicBlock *pred) {
  if (!ai)
    return false;
  if (m_obj_base.count(ai))
    return true;
  auto &ctx = m_solver.value().ctx();
  const uint64_t id = getStableValueId(ai);
  const std::string fnName =
      ai->getFunction() ? ai->getFunction()->getName().str() : "fn";
  const std::string baseName =
      "alloca." + fnName + "." + std::to_string(id);
  const auto elemBytesOpt = getTypeAllocSizeSafe(m_dl, ai->getAllocatedType());
  if (!elemBytesOpt.has_value()) {
    auto sizeSym =
        ctx.bv_const(("alloca.size." + std::to_string(id)).c_str(), m_ptr_bits);
    ensureObject(ai, baseName, sizeSym, /*sizeKnown=*/false,
                 /*hasRealSize=*/false);
    return true;
  }
  const uint64_t elemBytes = *elemBytesOpt;
  z3::expr sizeBytesExpr = ctx.bv_val(elemBytes, m_ptr_bits);
  bool known = true;
  bool hasRealSize = true;
  if (ai->isArrayAllocation()) {
    if (const auto count = getConstantU64(ai->getArraySize())) {
      uint64_t totalBytes = 0;
      if (!__builtin_mul_overflow(elemBytes, *count, &totalBytes)) {
        sizeBytesExpr = ctx.bv_val(totalBytes, m_ptr_bits);
      } else {
        auto countExpr = getIntExpr(ai->getArraySize(), cur, pred);
        const unsigned cbw = countExpr.get_sort().bv_size();
        if (cbw < m_ptr_bits)
          countExpr = z3::zext(countExpr, m_ptr_bits - cbw);
        else if (cbw > m_ptr_bits)
          countExpr = countExpr.extract(m_ptr_bits - 1, 0);
        sizeBytesExpr = countExpr * ctx.bv_val(elemBytes, m_ptr_bits);
        known = false;
      }
    } else if (ai->getArraySize()->getType()->isIntegerTy()) {
      auto countExpr = getIntExpr(ai->getArraySize(), cur, pred);
      const unsigned cbw = countExpr.get_sort().bv_size();
      if (cbw < m_ptr_bits)
        countExpr = z3::zext(countExpr, m_ptr_bits - cbw);
      else if (cbw > m_ptr_bits)
        countExpr = countExpr.extract(m_ptr_bits - 1, 0);
      sizeBytesExpr = countExpr * ctx.bv_val(elemBytes, m_ptr_bits);
      known = countExpr.is_numeral();
    } else {
      known = false;
      hasRealSize = false;
    }
  }
  ensureObject(ai, baseName, sizeBytesExpr, known, hasRealSize);
  return true;
}

void MKintPass::ensureObject(const Value *obj, const std::string &hintName,
                             const z3::expr &sizeBytes, bool sizeKnown,
                             bool hasRealSize) {
  if (m_obj_base.count(obj))
    return;

  auto &ctx = m_solver.value().ctx();
  const auto base = ctx.bv_const(hintName.c_str(), m_ptr_bits);

  setObjBase(obj, base);
  setObjSize(obj, sizeBytes);
  if (hasRealSize)
    markObjSizeKnown(obj);
  pushObjList(obj);
  havocObject(obj, hintName);

  // Basic well-formedness: keep base non-zero to avoid conflating with null.
  addConstraint(base != ctx.bv_val(0, m_ptr_bits));

  // Disjointness constraints against previously created objects.
  for (const auto *other : m_obj_list) {
    if (other == obj)
      continue;
    if (!m_obj_base.count(other) || !m_obj_size.count(other))
      continue;
    const auto otherBase = m_obj_base[other].value();
    const auto otherSize = m_obj_size[other].value();
    if (sizeKnown) {
      // Non-overlap: [base, base+size) does not overlap [otherBase,
      // otherBase+otherSize)
      const auto endThis = base + sizeBytes;
      const auto endOther = otherBase + otherSize;
      addConstraint(z3::ule(endThis, otherBase) || z3::ule(endOther, base));
    } else {
      // Unknown size: at least force distinct bases.
      addConstraint(base != otherBase);
    }
  }

  // Avoid modular wraparound when computing [base, base+size) for objects with
  // a modeled size.
  if (sizeKnown || hasRealSize) {
    addConstraint(z3::bvadd_no_overflow(base, sizeBytes, /*is_signed=*/false));
  }
}

bool MKintPass::maybeCheckOOB(const Instruction *at, const Value *ptrOperand,
                              uint64_t accessBytes, BasicBlock *cur,
                              BasicBlock *pred) {
  if (!CheckArrayOOB)
    return true;
  if (!at || !ptrOperand || !m_solver || !m_smt_mem)
    return true;
  if (accessBytes == 0)
    return true;

  const Value *stripped = ptrOperand->stripPointerCasts();
  const auto *gep = dyn_cast<GetElementPtrInst>(stripped);

  const Value *obj = getObjectForPtr(ptrOperand);
  if (!obj) {
    obj = llvm::getUnderlyingObject(stripped);
    if (const auto *gv = dyn_cast_or_null<GlobalVariable>(obj))
      ensureGlobalObject(gv);
    else if (const auto *ai = dyn_cast_or_null<AllocaInst>(obj))
      ensureAllocaObject(ai, cur, pred);
  }
  if (!obj)
    return true;
  // Precision guard (Incorrectness Logic): only report OOB against objects
  // whose size comes from an actual allocation/definition (alloca, sized
  // global, malloc/calloc/realloc), never against synthetic unconstrained
  // pointer-parameter sizes.
  if (!m_obj_size_known.contains(obj))
    return true;
  if (!m_obj_base.count(obj) || !m_obj_size.count(obj))
    return true;

  const auto baseOpt = m_obj_base[obj];
  const auto sizeOpt = m_obj_size[obj];
  if (!baseOpt.has_value() || !sizeOpt.has_value())
    return true;

  auto &solver = m_solver.value();
  auto &ctx = solver.ctx();
  const auto &base = baseOpt.value();
  const auto &size = sizeOpt.value();
  const auto addr = getPtrExpr(ptrOperand, cur, pred);
  const auto len = ctx.bv_val(accessBytes, m_ptr_bits);

  // In-bounds check for a byte range: addr >= base && addr + len <= base +
  // size, without wrapping.
  const z3::expr noWrap = z3::bvadd_no_overflow(addr, len, /*is_signed=*/false);
  const z3::expr inBounds =
      (z3::uge(addr, base) && z3::ule(addr + len, base + size) && noWrap);

  const Instruction *reportInst = gep ? static_cast<const Instruction *>(gep)
                                      : at;
  if ((!m_oob_insts.count(reportInst) || !m_dump_ef_path.empty()) &&
      checkBugCondition(reportInst, interr::ARRAY_OOB, !inBounds)) {
    m_oob_insts.insert(reportInst);
    if (gep)
      m_gep_oob.insert(const_cast<GetElementPtrInst *>(gep));
    if (m_bug_detection) {
      m_bug_detection->recordBug(reportInst, interr::ARRAY_OOB);
    }
  }

  // Constrain the remaining exploration to defined, in-bounds behaviors (LLVM
  // semantics for out-of-bounds are UB).
  addConstraint(inBounds);
  return solver.check() != z3::unsat;
}

bool MKintPass::addWellDefinedConstraints(BinaryOperator *op, BasicBlock *cur,
                                          BasicBlock *pred) {
  if (!op || !m_solver)
    return true;
  auto &solver = m_solver.value();
  auto &ctx = solver.ctx();

  const auto lhs = getIntExpr(op->getOperand(0), cur, pred);
  const auto rhs = getIntExpr(op->getOperand(1), cur, pred);
  const unsigned bw = lhs.get_sort().bv_size();

  bool added = false;
  const auto addAndMark = [&](const z3::expr &e) {
    addConstraint(e);
    added = true;
  };

  switch (op->getOpcode()) {
  case Instruction::UDiv:
  case Instruction::URem:
  case Instruction::SDiv:
  case Instruction::SRem:
    // Div/rem by zero is poison; keep exploring only defined paths.
    addAndMark(rhs != ctx.bv_val(0, bw));
    if (op->getOpcode() == Instruction::SDiv ||
        op->getOpcode() == Instruction::SRem) {
      // Signed division/remainder overflow (INT_MIN / -1) is UB in LLVM.
      addAndMark(z3::bvsdiv_no_overflow(lhs, rhs));
    }
    break;
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
    // Shift amount must be < bitwidth; otherwise poison.
    addAndMark(z3::ult(rhs, ctx.bv_val(bw, bw)));
    break;
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
    if (auto *ofop = dyn_cast<OverflowingBinaryOperator>(op)) {
      const bool nsw = ofop->hasNoSignedWrap();
      const bool nuw = ofop->hasNoUnsignedWrap();
      if (nuw) {
        if (op->getOpcode() == Instruction::Add) {
          addAndMark(z3::bvadd_no_overflow(lhs, rhs, /*is_signed=*/false));
        } else if (op->getOpcode() == Instruction::Sub) {
          addAndMark(z3::bvsub_no_underflow(lhs, rhs, /*is_signed=*/false));
        } else if (op->getOpcode() == Instruction::Mul) {
          addAndMark(z3::bvmul_no_overflow(lhs, rhs, /*is_signed=*/false));
        }
      }
      if (nsw) {
        if (op->getOpcode() == Instruction::Add) {
          addAndMark(z3::bvadd_no_overflow(lhs, rhs, /*is_signed=*/true));
          addAndMark(z3::bvadd_no_underflow(lhs, rhs));
        } else if (op->getOpcode() == Instruction::Sub) {
          addAndMark(z3::bvsub_no_underflow(lhs, rhs, /*is_signed=*/true));
          addAndMark(z3::bvsub_no_overflow(lhs, rhs));
        } else if (op->getOpcode() == Instruction::Mul) {
          addAndMark(z3::bvmul_no_overflow(lhs, rhs, /*is_signed=*/true));
          addAndMark(z3::bvmul_no_underflow(lhs, rhs));
        }
      }
    }
    break;
  default:
    break;
  }

  if (!added)
    return true;
  return solver.check() != z3::unsat;
}

z3::expr MKintPass::getValueExpr(const Value *v, BasicBlock *cur,
                                 BasicBlock *pred) {
  if (!v)
    return m_solver.value().ctx().bv_val(0, 1);
  if (v->getType()->isIntegerTy())
    return getIntExpr(v, cur, pred);
  if (v->getType()->isPointerTy())
    return getPtrExpr(v, cur, pred);
  // Unsupported sort: return a fresh 1-bit value to keep the solver going.
  const std::string name =
      "%unsupported." + std::to_string(getStableValueId(v));
  return m_solver.value().ctx().bv_const(name.c_str(), 1);
}

z3::expr MKintPass::getIntExpr(const Value *v, BasicBlock *cur,
                               BasicBlock *pred) {
  auto it = m_v2sym.find(v);
  if (it != m_v2sym.end() && it->second.has_value())
    return it->second.value();

  auto &ctx = m_solver.value().ctx();
  if (!v)
    return ctx.bv_val(0, 1);

  if (const auto *ci = dyn_cast<ConstantInt>(v)) {
    return bvValFromAPInt(ctx, ci->getValue());
  }

  if (const auto *fr = dyn_cast<FreezeInst>(v)) {
    if (fr->getType()->isIntegerTy()) {
      auto r = getIntExpr(fr->getOperand(0), cur, pred);
      setSym(v, r);
      return r;
    }
  }

  if (const auto *ev = dyn_cast<ExtractValueInst>(v)) {
    if (ev->getNumIndices() == 1 && ev->getType()->isIntegerTy()) {
      const unsigned idx = *ev->idx_begin();
      if (const auto *woi =
              dyn_cast<WithOverflowInst>(ev->getAggregateOperand())) {
        if (woi->arg_size() >= 2 &&
            woi->getArgOperand(0)->getType()->isIntegerTy()) {
          z3::expr res = ctx.bv_val(
              0, woi->getArgOperand(0)->getType()->getIntegerBitWidth());
          z3::expr ov = ctx.bool_val(false);
          if (computeWithOverflow(
                  woi, m_solver.value(),
                  [&](const llvm::Value *x) { return getIntExpr(x, cur, pred); },
                  res, ov)) {
            if (idx == 0) {
              setSym(v, res);
              return res;
            }
            if (idx == 1) {
              auto b = boolToBv1(ov);
              setSym(v, b);
              return b;
            }
          }
        }
      }
    }
  }

  if (const auto *pti = dyn_cast<PtrToIntOperator>(v)) {
    if (pti->getType()->isIntegerTy() &&
        pti->getOperand(0)->getType()->isPointerTy()) {
      auto p = getPtrExpr(pti->getOperand(0), cur, pred);
      const unsigned bw = pti->getType()->getIntegerBitWidth();
      if (bw < m_ptr_bits)
        p = p.extract(bw - 1, 0);
      else if (bw > m_ptr_bits)
        p = z3::zext(p, bw - m_ptr_bits);
      if (const Value *origin = getPointerOrigin(pti->getOperand(0)))
        setIntAlias(v, origin);
      if (auto off = getPointerOffset(pti->getOperand(0)))
        setIntOffset(v, off);
      setSym(v, p);
      return p;
    }
  }

  if (const auto *cast = dyn_cast<CastInst>(v)) {
    Value *operand = cast->getOperand(0);
    if (operand->getType()->isIntegerTy() && cast->getType()->isIntegerTy()) {
      auto in = getIntExpr(operand, cur, pred);
      const unsigned inBw = in.get_sort().bv_size();
      const unsigned outBw = cast->getType()->getIntegerBitWidth();
      z3::expr out = in;
      if (outBw < inBw)
        out = out.extract(outBw - 1, 0);
      else if (outBw > inBw) {
        if (isa<SExtInst>(cast))
          out = z3::sext(out, outBw - inBw);
        else
          out = z3::zext(out, outBw - inBw);
      }
      if (const Value *origin = getPointerOrigin(operand))
        setIntAlias(v, origin);
      if (auto off = getPointerOffset(operand))
        setIntOffset(v, off);
      setSym(v, out);
      return out;
    }
  }

  if (const auto *sel = dyn_cast<SelectInst>(v)) {
    if (sel->getType()->isIntegerTy() &&
        sel->getCondition()->getType()->isIntegerTy(1)) {
      auto c = getIntExpr(sel->getCondition(), cur, pred);
      auto t = getIntExpr(sel->getTrueValue(), cur, pred);
      auto f = getIntExpr(sel->getFalseValue(), cur, pred);
      z3::expr condBool = (c == ctx.bv_val(1, 1));
      z3::expr r = z3::ite(condBool, t, f);
      const Value *tOrigin = getPointerOrigin(sel->getTrueValue());
      const Value *fOrigin = getPointerOrigin(sel->getFalseValue());
      auto tOff = getPointerOffset(sel->getTrueValue());
      auto fOff = getPointerOffset(sel->getFalseValue());
      if (tOrigin && tOrigin == fOrigin) {
        setIntAlias(v, tOrigin);
        if (tOff && fOff && sameExpr(*tOff, *fOff))
          setIntOffset(v, tOff);
      }
      setSym(v, r);
      return r;
    }
  }

  if (const auto *bin = dyn_cast<BinaryOperator>(v)) {
    if (bin->getType()->isIntegerTy()) {
      const auto lhs = getIntExpr(bin->getOperand(0), cur, pred);
      const auto rhs = getIntExpr(bin->getOperand(1), cur, pred);
      z3::expr result = lhs;
      switch (bin->getOpcode()) {
      case Instruction::Add:
        result = lhs + rhs;
        break;
      case Instruction::Sub:
        result = lhs - rhs;
        break;
      case Instruction::Mul:
        result = lhs * rhs;
        break;
      case Instruction::UDiv:
        result = z3::udiv(lhs, rhs);
        break;
      case Instruction::SDiv:
        result = lhs / rhs;
        break;
      case Instruction::URem:
        result = z3::urem(lhs, rhs);
        break;
      case Instruction::SRem:
        result = z3::srem(lhs, rhs);
        break;
      case Instruction::Shl:
        result = z3::shl(lhs, rhs);
        break;
      case Instruction::LShr:
        result = z3::lshr(lhs, rhs);
        break;
      case Instruction::AShr:
        result = z3::ashr(lhs, rhs);
        break;
      case Instruction::And:
        result = lhs & rhs;
        break;
      case Instruction::Or:
        result = lhs | rhs;
        break;
      case Instruction::Xor:
        result = lhs ^ rhs;
        break;
      default:
        result = lhs;
        break;
      }
      const Value *lhsOrigin = getPointerOrigin(bin->getOperand(0));
      const Value *rhsOrigin = getPointerOrigin(bin->getOperand(1));
      if (lhsOrigin && (!rhsOrigin || lhsOrigin == rhsOrigin)) {
        setIntAlias(v, lhsOrigin);
        if (auto off = getPointerOffset(bin->getOperand(0)))
          setIntOffset(v, off);
      } else if (rhsOrigin && !lhsOrigin) {
        setIntAlias(v, rhsOrigin);
        if (auto off = getPointerOffset(bin->getOperand(1)))
          setIntOffset(v, off);
      }
      setSym(v, result);
      return result;
    }
  }

  if (const auto *icmp = dyn_cast<ICmpInst>(v)) {
    auto *lhs = icmp->getOperand(0);
    auto *rhs = icmp->getOperand(1);
    bool encoded = false;
    z3::expr condBool = ctx.bool_val(true);
    auto encodePredicate = [&](const z3::expr &l, const z3::expr &r) {
      switch (icmp->getPredicate()) {
      case ICmpInst::ICMP_EQ:
        condBool = (l == r);
        encoded = true;
        break;
      case ICmpInst::ICMP_NE:
        condBool = (l != r);
        encoded = true;
        break;
      case ICmpInst::ICMP_SGT:
        condBool = z3::sgt(l, r);
        encoded = true;
        break;
      case ICmpInst::ICMP_SGE:
        condBool = z3::sge(l, r);
        encoded = true;
        break;
      case ICmpInst::ICMP_SLT:
        condBool = z3::slt(l, r);
        encoded = true;
        break;
      case ICmpInst::ICMP_SLE:
        condBool = z3::sle(l, r);
        encoded = true;
        break;
      case ICmpInst::ICMP_UGT:
        condBool = z3::ugt(l, r);
        encoded = true;
        break;
      case ICmpInst::ICMP_UGE:
        condBool = z3::uge(l, r);
        encoded = true;
        break;
      case ICmpInst::ICMP_ULT:
        condBool = z3::ult(l, r);
        encoded = true;
        break;
      case ICmpInst::ICMP_ULE:
        condBool = z3::ule(l, r);
        encoded = true;
        break;
      default:
        break;
      }
    };
    if (lhs->getType()->isIntegerTy() && rhs->getType()->isIntegerTy()) {
      encodePredicate(getIntExpr(lhs, cur, pred), getIntExpr(rhs, cur, pred));
    } else if (lhs->getType()->isPointerTy() && rhs->getType()->isPointerTy()) {
      encodePredicate(getPtrExpr(lhs, cur, pred), getPtrExpr(rhs, cur, pred));
    }
    if (encoded) {
      auto bv = z3::ite(condBool, ctx.bv_val(1, 1), ctx.bv_val(0, 1));
      setSym(v, bv);
      return bv;
    }
    const std::string name = "%icmp." + std::to_string(getStableValueId(icmp));
    auto r = ctx.bv_const(name.c_str(), 1);
    setSym(v, r);
    return r;
  }

  if (const auto *phi = dyn_cast<PHINode>(v)) {
    const Value *commonOrigin = nullptr;
    bool seenOrigin = false;
    bool allHaveOrigin = true;
    for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
      const Value *incoming = phi->getIncomingValue(i);
      if (!incoming) {
        allHaveOrigin = false;
        break;
      }
      const Value *origin = getPointerOrigin(incoming);
      if (!origin) {
        allHaveOrigin = false;
        break;
      }
      if (!seenOrigin) {
        commonOrigin = origin;
        seenOrigin = true;
      } else if (commonOrigin != origin) {
        allHaveOrigin = false;
        break;
      }
    }
    if (allHaveOrigin && commonOrigin)
      setIntAlias(v, commonOrigin);
    if (allHaveOrigin) {
      std::optional<z3::expr> commonOffset;
      bool sawOffset = false;
      for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
        auto off = getPointerOffset(phi->getIncomingValue(i));
        if (!off) {
          sawOffset = false;
          break;
        }
        if (!sawOffset) {
          commonOffset = off;
          sawOffset = true;
          continue;
        }
        if (!sameExpr(*commonOffset, *off)) {
          sawOffset = false;
          break;
        }
      }
      if (sawOffset && commonOffset)
        setIntOffset(v, commonOffset);
    }
    // Ideally resolved on block entry. If not, keep it symbolic.
    const std::string name = "%phi." + std::to_string(getStableValueId(phi));
    const unsigned bw =
        phi->getType()->isIntegerTy() ? phi->getType()->getIntegerBitWidth() : 1;
    auto r = ctx.bv_const(name.c_str(), bw);
    setSym(v, r);
    return r;
  }

  if (const auto *call = dyn_cast<CallInst>(v)) {
    if (call->getType()->isIntegerTy()) {
      const unsigned bw = call->getType()->getIntegerBitWidth();
      if (const auto *intr = dyn_cast<IntrinsicInst>(call)) {
        switch (intr->getIntrinsicID()) {
        case Intrinsic::smin:
        case Intrinsic::smax:
        case Intrinsic::umin:
        case Intrinsic::umax:
          if (intr->arg_size() >= 2 &&
              intr->getArgOperand(0)->getType()->isIntegerTy(bw) &&
              intr->getArgOperand(1)->getType()->isIntegerTy(bw)) {
            auto a = getIntExpr(intr->getArgOperand(0), cur, pred);
            auto b = getIntExpr(intr->getArgOperand(1), cur, pred);
            z3::expr cond =
                intr->getIntrinsicID() == Intrinsic::smin   ? z3::slt(a, b)
                : intr->getIntrinsicID() == Intrinsic::smax ? z3::sgt(a, b)
                : intr->getIntrinsicID() == Intrinsic::umin ? z3::ult(a, b)
                                                            : z3::ugt(a, b);
            auto r = z3::ite(cond, a, b);
            setSym(v, r);
            return r;
          }
          break;
        case Intrinsic::abs:
          if (intr->arg_size() >= 1 &&
              intr->getArgOperand(0)->getType()->isIntegerTy(bw)) {
            auto a = getIntExpr(intr->getArgOperand(0), cur, pred);
            auto zero = ctx.bv_val(0, bw);
            auto r = z3::ite(z3::slt(a, zero), -a, a);
            setSym(v, r);
            return r;
          }
          break;
        case Intrinsic::uadd_sat:
          if (intr->arg_size() >= 2 &&
              intr->getArgOperand(0)->getType()->isIntegerTy(bw) &&
              intr->getArgOperand(1)->getType()->isIntegerTy(bw)) {
            auto a = getIntExpr(intr->getArgOperand(0), cur, pred);
            auto b = getIntExpr(intr->getArgOperand(1), cur, pred);
            auto maxU = ~ctx.bv_val(0, bw);
            auto r = z3::ite(z3::bvadd_no_overflow(a, b, false), a + b, maxU);
            setSym(v, r);
            return r;
          }
          break;
        case Intrinsic::usub_sat:
          if (intr->arg_size() >= 2 &&
              intr->getArgOperand(0)->getType()->isIntegerTy(bw) &&
              intr->getArgOperand(1)->getType()->isIntegerTy(bw)) {
            auto a = getIntExpr(intr->getArgOperand(0), cur, pred);
            auto b = getIntExpr(intr->getArgOperand(1), cur, pred);
            auto zero = ctx.bv_val(0, bw);
            auto r = z3::ite(z3::bvsub_no_underflow(a, b, false), a - b, zero);
            setSym(v, r);
            return r;
          }
          break;
        case Intrinsic::sadd_sat:
        case Intrinsic::ssub_sat:
          if (intr->arg_size() >= 2 &&
              intr->getArgOperand(0)->getType()->isIntegerTy(bw) &&
              intr->getArgOperand(1)->getType()->isIntegerTy(bw)) {
            auto a = getIntExpr(intr->getArgOperand(0), cur, pred);
            auto b = getIntExpr(intr->getArgOperand(1), cur, pred);
            auto smax = bvValFromAPInt(ctx, APInt::getSignedMaxValue(bw));
            auto smin = bvValFromAPInt(ctx, APInt::getSignedMinValue(bw));
            z3::expr r = a;
            if (intr->getIntrinsicID() == Intrinsic::sadd_sat) {
              r = z3::ite(!z3::bvadd_no_overflow(a, b, true), smax,
                          z3::ite(!z3::bvadd_no_underflow(a, b), smin, a + b));
            } else {
              r = z3::ite(!z3::bvsub_no_overflow(a, b), smax,
                          z3::ite(!z3::bvsub_no_underflow(a, b, true), smin,
                                  a - b));
            }
            setSym(v, r);
            return r;
          }
          break;
        case Intrinsic::bswap:
          if (bw >= 16 && (bw % 8) == 0 && intr->arg_size() >= 1 &&
              intr->getArgOperand(0)->getType()->isIntegerTy(bw)) {
            auto a = getIntExpr(intr->getArgOperand(0), cur, pred);
            const unsigned nbytes = bw / 8;
            z3::expr r = a.extract(7, 0);
            for (unsigned i = 1; i < nbytes; ++i) {
              r = z3::concat(r, a.extract((i + 1) * 8 - 1, i * 8));
            }
            setSym(v, r);
            return r;
          }
          break;
        case Intrinsic::fshl:
        case Intrinsic::fshr:
          if (intr->arg_size() >= 3 &&
              intr->getArgOperand(0)->getType()->isIntegerTy(bw) &&
              intr->getArgOperand(1)->getType()->isIntegerTy(bw) &&
              intr->getArgOperand(2)->getType()->isIntegerTy(bw)) {
            auto a = getIntExpr(intr->getArgOperand(0), cur, pred);
            auto b = getIntExpr(intr->getArgOperand(1), cur, pred);
            auto c = getIntExpr(intr->getArgOperand(2), cur, pred);
            auto shift = z3::urem(c, ctx.bv_val(bw, bw));
            auto wide = z3::concat(a, b);
            auto wideShift = z3::zext(shift, bw);
            z3::expr r =
                (intr->getIntrinsicID() == Intrinsic::fshl)
                    ? z3::shl(wide, wideShift).extract(2 * bw - 1, bw)
                    : z3::lshr(wide, wideShift).extract(bw - 1, 0);
            setSym(v, r);
            return r;
          }
          break;
        case Intrinsic::ctlz:
        case Intrinsic::cttz:
          if (bw <= 64 && intr->arg_size() >= 1 &&
              intr->getArgOperand(0)->getType()->isIntegerTy(bw)) {
            auto a = getIntExpr(intr->getArgOperand(0), cur, pred);
            z3::expr r = ctx.bv_val(bw, bw);
            const bool leading = (intr->getIntrinsicID() == Intrinsic::ctlz);
            for (int i = static_cast<int>(bw) - 1; i >= 0; --i) {
              const unsigned bitPos =
                  leading ? (bw - 1 - static_cast<unsigned>(i))
                          : static_cast<unsigned>(i);
              z3::expr bitSet = (a.extract(bitPos, bitPos) == ctx.bv_val(1, 1));
              r = z3::ite(bitSet, ctx.bv_val(static_cast<unsigned>(i), bw), r);
            }
            setSym(v, r);
            return r;
          }
          break;
        case Intrinsic::ctpop:
          if (bw <= 64 && intr->arg_size() >= 1 &&
              intr->getArgOperand(0)->getType()->isIntegerTy(bw)) {
            auto a = getIntExpr(intr->getArgOperand(0), cur, pred);
            z3::expr sum = ctx.bv_val(0, bw);
            for (unsigned i = 0; i < bw; ++i) {
              z3::expr bit = a.extract(i, i);
              sum = sum + (bw > 1 ? z3::zext(bit, bw - 1) : bit);
            }
            setSym(v, sum);
            return sum;
          }
          break;
        default:
          break;
        }
      }

      const std::string name = "%call." + std::to_string(getStableValueId(call)) +
                               "." + std::to_string(++m_fresh_sym_counter);
      auto r = ctx.bv_const(name.c_str(), bw);
      setSym(v, r);
      bool unknown_call = true;
      if (auto *callee = call->getCalledFunction()) {
        if (!callee->isDeclaration()) {
          unknown_call = false;
        }
      }
      if (unknown_call) {
        if (m_robust_reachability) {
          registerUniversal(r);
        }
      }
      return r;
    }
  }

  // Default: fresh int.
  const unsigned bw =
      v->getType()->isIntegerTy() ? v->getType()->getIntegerBitWidth() : 1;
  const std::string name = "%int." + std::to_string(getStableValueId(v)) + "." +
                           std::to_string(++m_fresh_sym_counter);
  auto r = ctx.bv_const(name.c_str(), bw);
  setSym(v, r);
  return r;
}

z3::expr MKintPass::gepOffsetBytes(const GEPOperator *gep, BasicBlock *cur,
                                   BasicBlock *pred) {
  auto &ctx = m_solver.value().ctx();
  if (!gep || !m_dl)
    return ctx.bv_val(0, m_ptr_bits);

  // Fast path: all-constant GEP (use pointer address-space index bitwidth).
  const unsigned idxBits =
      m_dl->getIndexSizeInBits(gep->getPointerAddressSpace());
  if (idxBits > 0) {
    APInt constOff(idxBits, 0);
    if (gep->accumulateConstantOffset(*m_dl, constOff)) {
      return bvValFromAPInt(ctx, constOff.sextOrTrunc(m_ptr_bits));
    }
  }

  z3::expr off = ctx.bv_val(0, m_ptr_bits);
  Type *ty = gep->getSourceElementType();
  unsigned idxNo = 0;
  for (auto idxIt = gep->idx_begin(); idxIt != gep->idx_end();
       ++idxIt, ++idxNo) {
    Value *idxV = idxIt->get();
    if (!idxV)
      continue;

    if (idxNo > 0) {
      if (auto *st = dyn_cast<StructType>(ty)) {
        auto *ci = dyn_cast<ConstantInt>(idxV);
        if (!ci || ci->getValue().getActiveBits() > 32 || !st->isSized()) {
          const std::string name =
              "%gep.structidx." + std::to_string(getStableValueId(gep));
          return ctx.bv_const(name.c_str(), m_ptr_bits);
        }
        const unsigned field = static_cast<unsigned>(ci->getZExtValue());
        if (field >= st->getNumElements()) {
          const std::string name =
              "%gep.structidx." + std::to_string(getStableValueId(gep));
          return ctx.bv_const(name.c_str(), m_ptr_bits);
        }
        const auto *layout = m_dl->getStructLayout(st);
        off = off + ctx.bv_val(layout->getElementOffset(field), m_ptr_bits);
        ty = st->getElementType(field);
        continue;
      }

      if (auto *at = dyn_cast<ArrayType>(ty)) {
        ty = at->getElementType();
      } else if (auto *vt = dyn_cast<FixedVectorType>(ty)) {
        ty = vt->getElementType();
      } else {
        const std::string name =
            "%gep.unsized." + std::to_string(getStableValueId(gep));
        return ctx.bv_const(name.c_str(), m_ptr_bits);
      }
    }

    const auto elemBytesOpt = getTypeAllocSizeSafe(m_dl, ty);
    if (!elemBytesOpt.has_value() || !idxV->getType()->isIntegerTy()) {
      const std::string name =
          "%gep.unsized." + std::to_string(getStableValueId(gep));
      return ctx.bv_const(name.c_str(), m_ptr_bits);
    }

    z3::expr idx = getIntExpr(idxV, cur, pred);
    const unsigned ibw = idx.get_sort().bv_size();
    if (ibw < m_ptr_bits)
      idx = z3::sext(idx, m_ptr_bits - ibw);
    else if (ibw > m_ptr_bits)
      idx = idx.extract(m_ptr_bits - 1, 0);
    off = off + (idx * ctx.bv_val(*elemBytesOpt, m_ptr_bits));
  }

  return off;
}

z3::expr MKintPass::getPtrExpr(const Value *v, BasicBlock *cur,
                               BasicBlock *pred) {
  auto it = m_v2sym.find(v);
  if (it != m_v2sym.end() && it->second.has_value())
    return it->second.value();

  auto &ctx = m_solver.value().ctx();
  if (!v)
    return ctx.bv_val(0, m_ptr_bits);

  if (isa<ConstantPointerNull>(v)) {
    return ctx.bv_val(0, m_ptr_bits);
  }

  if (const auto *gv = dyn_cast<GlobalVariable>(v)) {
    ensureGlobalObject(gv);
    setPtrOffset(v, ctx.bv_val(0, m_ptr_bits));
    setSym(v, m_obj_base[gv].value());
    return m_obj_base[gv].value();
  }

  if (const auto *fr = dyn_cast<FreezeInst>(v)) {
    if (fr->getType()->isPointerTy()) {
      auto r = getPtrExpr(fr->getOperand(0), cur, pred);
      setSym(v, r);
      return r;
    }
  }

  if (const auto *ai = dyn_cast<AllocaInst>(v)) {
    ensureAllocaObject(ai, cur, pred);
    setPtrOffset(v, ctx.bv_val(0, m_ptr_bits));
    setSym(v, m_obj_base[ai].value());
    return m_obj_base[ai].value();
  }

  if (const auto *arg = dyn_cast<Argument>(v)) {
    if (arg->getType()->isPointerTy()) {
      const std::string name = (arg->getParent()->getName() + ".argptr" +
                                std::to_string(arg->getArgNo()))
                                   .str();
      auto r = ctx.bv_const(name.c_str(), m_ptr_bits);
      setPtrOffset(v, ctx.bv_val(0, m_ptr_bits));
      setSym(v, r);
      return r;
    }
  }

  if (const auto *gep = dyn_cast<GEPOperator>(v)) {
    if (gep->getType()->isPointerTy() &&
        gep->getPointerOperand()->getType()->isPointerTy()) {
      auto base = getPtrExpr(gep->getPointerOperand(), cur, pred);
      auto off = gepOffsetBytes(gep, cur, pred);
      auto r = base + off;
      if (auto baseOff = getPointerOffset(gep->getPointerOperand()))
        setPtrOffset(v, baseOff.value() + off);
      else
        setPtrOffset(v, off);
      if (const Value *root = getPointerOrigin(gep->getPointerOperand()))
        setObjAlias(v, root);
      setSym(v, r);
      return r;
    }
  }

  if (const auto *bc = dyn_cast<BitCastOperator>(v)) {
    if (bc->getType()->isPointerTy() &&
        bc->getOperand(0)->getType()->isPointerTy()) {
      auto r = getPtrExpr(bc->getOperand(0), cur, pred);
      if (auto off = getPointerOffset(bc->getOperand(0)))
        setPtrOffset(v, off);
      if (const Value *root = getPointerOrigin(bc->getOperand(0)))
        setObjAlias(v, root);
      setSym(v, r);
      return r;
    }
  }

  if (const auto *asc = dyn_cast<AddrSpaceCastOperator>(v)) {
    if (asc->getType()->isPointerTy() &&
        asc->getOperand(0)->getType()->isPointerTy()) {
      auto r = getPtrExpr(asc->getOperand(0), cur, pred);
      if (auto off = getPointerOffset(asc->getOperand(0)))
        setPtrOffset(v, off);
      if (const Value *root = getPointerOrigin(asc->getOperand(0)))
        setObjAlias(v, root);
      setSym(v, r);
      return r;
    }
  }

  if (const auto *op = dyn_cast<Operator>(v)) {
    if (op->getOpcode() == Instruction::IntToPtr &&
        op->getType()->isPointerTy() &&
        op->getOperand(0)->getType()->isIntegerTy()) {
      auto i = getIntExpr(op->getOperand(0), cur, pred);
      const unsigned ibw = i.get_sort().bv_size();
      if (ibw < m_ptr_bits)
        i = z3::zext(i, m_ptr_bits - ibw);
      else if (ibw > m_ptr_bits)
        i = i.extract(m_ptr_bits - 1, 0);
      if (const Value *origin = getPointerOrigin(op->getOperand(0)))
        setObjAlias(v, origin);
      if (auto off = getPointerOffset(op->getOperand(0)))
        setPtrOffset(v, off);
      setSym(v, i);
      return i;
    }
  }

  if (const auto *sel = dyn_cast<SelectInst>(v)) {
    if (sel->getType()->isPointerTy() &&
        sel->getCondition()->getType()->isIntegerTy(1)) {
      auto c = getIntExpr(sel->getCondition(), cur, pred);
      auto t = getPtrExpr(sel->getTrueValue(), cur, pred);
      auto f = getPtrExpr(sel->getFalseValue(), cur, pred);
      z3::expr condBool = (c == ctx.bv_val(1, 1));
      auto r = z3::ite(condBool, t, f);
      if (const Value *root =
              commonPointerObject(sel->getTrueValue(), sel->getFalseValue())) {
        setObjAlias(v, root);
      }
      auto tOff = getPointerOffset(sel->getTrueValue());
      auto fOff = getPointerOffset(sel->getFalseValue());
      if (tOff && fOff && sameExpr(*tOff, *fOff))
        setPtrOffset(v, tOff);
      setSym(v, r);
      return r;
    }
  }

  if (const auto *phi = dyn_cast<PHINode>(v)) {
    const Value *commonRoot = nullptr;
    bool seenPtr = false;
    for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
      const Value *incoming = phi->getIncomingValue(i);
      if (!incoming || !incoming->getType()->isPointerTy())
        continue;
      const Value *incomingRoot = getPointerOrigin(incoming);
      if (!incomingRoot) {
        commonRoot = nullptr;
        seenPtr = false;
        break;
      }
      if (!seenPtr) {
        commonRoot = incomingRoot;
        seenPtr = true;
      } else if (commonRoot != incomingRoot) {
        commonRoot = nullptr;
        break;
      }
    }
    if (commonRoot)
      setObjAlias(v, commonRoot);
    std::optional<z3::expr> commonOffset;
    bool seenOffset = false;
    for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
      const Value *incoming = phi->getIncomingValue(i);
      if (!incoming || !incoming->getType()->isPointerTy()) {
        seenOffset = false;
        break;
      }
      auto off = getPointerOffset(incoming);
      if (!off) {
        seenOffset = false;
        break;
      }
      if (!seenOffset) {
        commonOffset = off;
        seenOffset = true;
        continue;
      }
      if (!sameExpr(*commonOffset, *off)) {
        seenOffset = false;
        break;
      }
    }
    if (seenOffset && commonOffset)
      setPtrOffset(v, commonOffset);
    const std::string name =
        "%phi.ptr." + std::to_string(getStableValueId(phi));
    auto r = ctx.bv_const(name.c_str(), m_ptr_bits);
    setSym(v, r);
    return r;
  }

  if (const auto *call = dyn_cast<CallInst>(v)) {
    if (call->getType()->isPointerTy()) {
      const std::string name =
          "%call.ptr." + std::to_string(getStableValueId(call)) + "." +
          std::to_string(++m_fresh_sym_counter);
      auto r = ctx.bv_const(name.c_str(), m_ptr_bits);
      setSym(v, r);
      return r;
    }
  }

  const std::string name = "%ptr." + std::to_string(getStableValueId(v)) + "." +
                           std::to_string(++m_fresh_sym_counter);
  auto r = ctx.bv_const(name.c_str(), m_ptr_bits);
  setSym(v, r);
  return r;
}

} // namespace kint
