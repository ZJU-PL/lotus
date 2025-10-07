
#ifndef THREADAPI_H
#define THREADAPI_H

#include "llvm/ADT/StringMap.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

/*
 * ThreadAPI class contains interfaces for pthread programs
 */

namespace llvm {
class CallBase;
class Value;
class Instruction;
}

using namespace llvm;

typedef unsigned u32_t;

class ThreadAPI {

public:
  enum TD_TYPE {
    TD_DUMMY = 0,       /// dummy type
    TD_FORK,            /// create a new thread
    TD_JOIN,            /// wait for a thread to join
    TD_DETACH,          /// detach a thread directly instead wait for it to join
    TD_ACQUIRE,         /// acquire a lock
    TD_TRY_ACQUIRE,     /// try to acquire a lock
    TD_RELEASE,         /// release a lock
    TD_EXIT,            /// exit/kill a thread
    TD_CANCEL,          /// cancel a thread by another
    TD_COND_WAIT,       /// wait a condition
    TD_COND_SIGNAL,     /// signal a condition
    TD_COND_BROADCAST,  /// broadcast a condition
    TD_MUTEX_INI,       /// initial a mutex variable
    TD_MUTEX_DESTROY,   /// initial a mutex variable
    TD_CONDVAR_INI,     /// initial a mutex variable
    TD_CONDVAR_DESTROY, /// initial a mutex variable
    TD_BAR_INIT,        /// Barrier init
    TD_BAR_WAIT,        /// Barrier wait
    HARE_PAR_FOR
  };

  typedef llvm::StringMap<TD_TYPE> TDAPIMap;

private:
  /// API map, from a string to threadAPI type
  TDAPIMap tdAPIMap;

  /// Constructor
  ThreadAPI() { init(); }

  /// Initialize the map
  void init();

  /// Static reference
  static ThreadAPI *tdAPI;

  /// Get the function type if it is a threadAPI function
  inline TD_TYPE getType(const Function *F) const {
    if (F) {
      TDAPIMap::const_iterator it = tdAPIMap.find(F->getName().str());
      if (it != tdAPIMap.end())
        return it->second;
    }
    return TD_DUMMY;
  }

public:
  /// Return a static reference
  static ThreadAPI *getThreadAPI() {
    if (tdAPI == NULL) {
      tdAPI = new ThreadAPI();
    }
    return tdAPI;
  }

  /// Return the callee/callsite/func
  //@{
  const llvm::Function *getCallee(const llvm::Instruction *inst) const;

  const llvm::Function *getCallee(const llvm::CallBase *cb) const;

  const llvm::CallBase *getLLVMCallSite(const llvm::Instruction *inst) const;
  //@}

  /// Return true if this call create a new thread
  //@{
  inline bool isTDFork(const Instruction *inst) const {
    return getType(getCallee(inst)) == TD_FORK;
  }
  inline bool isTDFork(const CallBase *cb) const {
    return getType(getCallee(cb)) == TD_FORK;
  }
  //@}

  /// Return true if this call proceeds a hare_parallel_for
  //@{
  inline bool isHareParFor(const Instruction *inst) const {
    return getType(getCallee(inst)) == HARE_PAR_FOR;
  }
  inline bool isHareParFor(const CallBase *cb) const {
    return isHareParFor(dyn_cast<Instruction>(cb));
  }
  //@}

  /// Return arguments/attributes of pthread_create / hare_parallel_for
  //@{
  /// Return the first argument of the call,
  /// Note that, it is the pthread_t pointer
  inline const Value *getForkedThread(const Instruction *inst) const {
    assert(isTDFork(inst) && "not a thread fork function!");
    const CallBase *cb = getLLVMCallSite(inst);
    return cb->getArgOperand(0);
  }
  inline const Value *getForkedThread(const CallBase *cb) const {
    return getForkedThread(dyn_cast<Instruction>(cb));
  }

  /// Return the third argument of the call,
  /// Note that, it could be function type or a void* pointer
  inline const Value *getForkedFun(const Instruction *inst) const {
    assert(isTDFork(inst) && "not a thread fork function!");
    const CallBase *cb = getLLVMCallSite(inst);
    return cb->getArgOperand(2)->stripPointerCasts();
  }
  inline const Value *getForkedFun(const CallBase *cb) const {
    return getForkedFun(dyn_cast<Instruction>(cb));
  }

  /// Return the forth argument of the call,
  /// Note that, it is the sole argument of start routine ( a void* pointer )
  inline const Value *getActualParmAtForkSite(const Instruction *inst) const {
    assert(isTDFork(inst) && "not a thread fork function!");
    const CallBase *cb = getLLVMCallSite(inst);
    return cb->getArgOperand(3);
  }
  inline const Value *getActualParmAtForkSite(const CallBase *cb) const {
    return getActualParmAtForkSite(dyn_cast<Instruction>(cb));
  }
  //@}

  /// Get the task function (i.e., the 5th parameter) of the hare_parallel_for
  /// call
  //@{
  inline const Value *
  getTaskFuncAtHareParForSite(const Instruction *inst) const {
    assert(isHareParFor(inst) && "not a hare_parallel_for function!");
    const CallBase *cb = getLLVMCallSite(inst);
    return cb->getArgOperand(4)->stripPointerCasts();
  }

  inline const Value *getTaskFuncAtHareParForSite(const CallBase *cb) const {
    return getTaskFuncAtHareParForSite(dyn_cast<Instruction>(cb));
  }
  //@}

  /// Get the task data (i.e., the 6th parameter) of the hare_parallel_for call
  //@{
  inline const Value *
  getTaskDataAtHareParForSite(const Instruction *inst) const {
    assert(isHareParFor(inst) && "not a hare_parallel_for function!");
    const CallBase *cb = getLLVMCallSite(inst);
    return cb->getArgOperand(5);
  }
  inline const Value *getTaskDataAtHareParForSite(const CallBase *cb) const {
    return getTaskDataAtHareParForSite(dyn_cast<Instruction>(cb));
  }
  //@}

  /// Return true if this call wait for a worker thread
  //@{
  inline bool isTDJoin(const Instruction *inst) const {
    return getType(getCallee(inst)) == TD_JOIN;
  }
  inline bool isTDJoin(const CallBase *cb) const {
    return getType(getCallee(cb)) == TD_JOIN;
  }
  //@}

  /// Return arguments/attributes of pthread_join
  //@{
  /// Return the first argument of the call,
  /// Note that, it is the pthread_t pointer
  inline const Value *getJoinedThread(const Instruction *inst) const {
    assert(isTDJoin(inst) && "not a thread join function!");
    const CallBase *cb = getLLVMCallSite(inst);
    Value *join = cb->getArgOperand(0);
    if (llvm::isa<LoadInst>(join))
      return llvm::cast<LoadInst>(join)->getPointerOperand();
    else if (llvm::isa<Argument>(join))
      return join;
    assert(
        false &&
        "the value of the first argument at join is not a load instruction?");
    return NULL;
  }
  inline const Value *getJoinedThread(const CallBase *cb) const {
    return getJoinedThread(dyn_cast<Instruction>(cb));
  }
  /// Return the send argument of the call,
  /// Note that, it is the pthread_t pointer
  inline const Value *getRetParmAtJoinedSite(const Instruction *inst) const {
    assert(isTDJoin(inst) && "not a thread join function!");
    const CallBase *cb = getLLVMCallSite(inst);
    return cb->getArgOperand(1);
  }
  inline const Value *getRetParmAtJoinedSite(const CallBase *cb) const {
    return getRetParmAtJoinedSite(dyn_cast<Instruction>(cb));
  }
  //@}

  /// Return true if this call exits/terminate a thread
  //@{
  inline bool isTDExit(const Instruction *inst) const {
    return getType(getCallee(inst)) == TD_EXIT;
  }

  inline bool isTDExit(const CallBase *cb) const {
    return getType(getCallee(cb)) == TD_EXIT;
  }
  //@}

  /// Return true if this call acquire a lock
  //@{
  inline bool isTDAcquire(const Instruction *inst) const {
    return getType(getCallee(inst)) == TD_ACQUIRE;
  }

  inline bool isTDAcquire(const CallBase *cb) const {
    return getType(getCallee(cb)) == TD_ACQUIRE;
  }
  //@}

  /// Return true if this call release a lock
  //@{
  inline bool isTDRelease(const Instruction *inst) const {
    return getType(getCallee(inst)) == TD_RELEASE;
  }

  inline bool isTDRelease(const CallBase *cb) const {
    return getType(getCallee(cb)) == TD_RELEASE;
  }
  //@}

  /// Return lock value
  //@{
  /// First argument of pthread_mutex_lock/pthread_mutex_unlock
  inline const Value *getLockVal(const Instruction *inst) const {
    assert((isTDAcquire(inst) || isTDRelease(inst)) &&
           "not a lock acquire or release function");
    const CallBase *cb = getLLVMCallSite(inst);
    return cb->getArgOperand(0);
  }
  inline const Value *getLockVal(const CallBase *cb) const {
    return getLockVal(dyn_cast<Instruction>(cb));
  }
  //@}

  /// Return true if this call waits for a barrier
  //@{
  inline bool isTDBarWait(const Instruction *inst) const {
    return getType(getCallee(inst)) == TD_BAR_WAIT;
  }

  inline bool isTDBarWait(const CallBase *cb) const {
    return getType(getCallee(cb)) == TD_BAR_WAIT;
  }
  //@}

  void performAPIStat(Module *m);
  void statInit(llvm::StringMap<u32_t> &tdAPIStatMap);
};

#endif // THREADAPI_H