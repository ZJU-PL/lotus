

#ifndef THREADINFO_H
#define THREADINFO_H

#include <list>
#include <unordered_map>

#include "llvm/IR/Function.h"
#include "llvm/IR/Value.h"

namespace llvm {
class CallBase;
}

enum class TFGEdgeType {
  CALL,
  FORK,
  JOIN,
  NOTIFY,
  WAIT,
  LOCK,
  UNLOCK,
  FLOW,
  OTHERS
};

template <typename E>
constexpr typename std::underlying_type<E>::type
TFG_To_underlying(E e) noexcept {
  return static_cast<typename std::underlying_type<E>::type>(e);
}

class ThreadInfo {
public:
  static size_t generateTID(const llvm::CallBase *cb);
  static const llvm::CallBase *queryCSByTID(size_t TID);
  static size_t queryTIDByCS(const llvm::CallBase *CB);
  static void setPPThreadCreateInfo(llvm::Function *, size_t);
  static bool isPPThreadCreate(llvm::Function *);
  static bool isPPThreadFun(llvm::Function *, TFGEdgeType);
  static size_t getPPThreadCreateArg(llvm::Function *);
  static size_t getPPThreadFunArg(llvm::Function *);
  static std::string getCBThreadFunName(TFGEdgeType);
  static bool isValidCallInst(llvm::Value *);
  // for debug.
  static void dumpTrace(std::list<const llvm::CallBase *> &trace);

  // static TFGEdgeType getCallType(Function* func);

private:
  static size_t curTID;
  // init as 1 because 0 is reserved as parent set.
  static std::unordered_map<llvm::Function *, size_t> PPThreadCreateInfo;
  static std::unordered_map<size_t, const llvm::CallBase *> TIDMap;
  static std::unordered_map<const llvm::CallBase *, size_t> ReversedTIDMap;
};

#endif // THREADINFO_H