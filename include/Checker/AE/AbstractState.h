/** @file AbstractState.h @brief Abstract state representation for abstract
 * execution bug detection. */
//===- AbstractState.h ----Abstract State--------------------------//
//
// Migrated from SVF's AE engine to Lotus.
//
// This file defines the AbstractState class, which represents the abstract
// execution state during program analysis. The state maps program variables
// and memory locations to abstract values (intervals or addresses).
//
// Key concepts:
// - Abstract values: Either intervals (e.g., [0, 100]) or address sets
// - Variable mapping: Maps SSA values to their abstract values
// - Memory mapping: Maps heap/stack objects to their abstract values
// - Field sensitivity: Tracks individual struct fields via GEP offsets
//
// Based on the paper:
// "Precise Sparse Abstract Execution via Cross-Domain Interaction"
// Xiao Cheng, Jiawei Wang, Yulei Sui. ICSE 2024.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Checker/AE/AbstractValue.h"

#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>

namespace lotus {
namespace analysis {

// Forward declaration (SVFIRWrapper only needs pointer)
class SVFIRWrapper;
class AbstractInterpretation;

/// Maximum field limit for GEP offset calculations to prevent explosion
/// in field-sensitive analysis. Fields beyond this limit are merged.
__attribute__((unused)) static constexpr uint32_t MaxFieldLimit = 10000;

class AbstractState;

/// Storage for the variable-to-value map of an `AbstractState`.
///
/// Two backings are supported, selected per analysis run:
///
///  - **Local**: the table owns its map and a state copy deep-copies it. This
///  is
///    the classic dense layout.
///  - **Def-site**: the table points at one run-wide map owned by
///    `AbstractStateStore`. A value is defined by exactly one SSA instruction,
///    so its abstract value is the same at every program point dominated by
///    that definition; storing it once per *value* instead of once per *node*
///    is what SVF calls the semi-sparse layout
///    (`svf/include/AE/Svfexe/SparseAbstractInterpretation.h`).
///
/// The facade keeps the same member-function surface as
/// `std::unordered_map<uint32_t, AbstractValue>`, so no call site in the
/// analysis had to change.
class VarTable {
public:
  using Map = std::unordered_map<uint32_t, AbstractValue>;
  using iterator = Map::iterator;
  using const_iterator = Map::const_iterator;

  VarTable() : local_(std::make_unique<Map>()) {}
  explicit VarTable(const Map &init) : local_(std::make_unique<Map>(init)) {}

  VarTable(const VarTable &other) {
    if (other.shared_)
      shared_ = other.shared_;
    else
      local_ = std::make_unique<Map>(*other.local_);
  }

  VarTable(VarTable &&other) noexcept
      : local_(std::move(other.local_)), shared_(other.shared_) {
    other.shared_ = nullptr;
  }

  VarTable &operator=(const VarTable &other) {
    if (this == &other)
      return *this;
    shared_ = nullptr;
    local_.reset();
    if (other.shared_)
      shared_ = other.shared_;
    else
      local_ = std::make_unique<Map>(*other.local_);
    return *this;
  }

  VarTable &operator=(VarTable &&other) noexcept {
    if (this == &other)
      return *this;
    local_ = std::move(other.local_);
    shared_ = other.shared_;
    other.shared_ = nullptr;
    return *this;
  }

  /// True when the table is backed by the run-wide def-site map.
  bool isDefSite() const { return shared_ != nullptr; }

  /// Backs this table with @p shared. The local map is dropped: writes now land
  /// in the shared table.
  void bindDefSite(Map *shared) {
    shared_ = shared;
    local_.reset();
  }

  /// Copies the entries of the shared table into a private map and unbinds.
  ///
  /// Used before widening/narrowing, which must not mutate the shared table
  /// in place (`AbstractState::widening` documents why).
  void detachToLocal() {
    if (!shared_)
      return;
    local_ = std::make_unique<Map>(*shared_);
    shared_ = nullptr;
  }

  /// Copies only @p ids out of the shared table into a private map and
  /// unbinds. Entries not listed are dropped from the local copy.
  void detachToLocal(const std::unordered_set<uint32_t> &ids) {
    if (!shared_)
      return;
    local_ = std::make_unique<Map>();
    for (uint32_t id : ids) {
      const auto it = shared_->find(id);
      if (it != shared_->end())
        (*local_)[id] = it->second;
    }
    shared_ = nullptr;
  }

  /// Writes this table's entries into @p target.
  void scatterTo(Map &target) const {
    for (const auto &entry : *this)
      target[entry.first] = entry.second;
  }

  const Map &map() const { return shared_ ? *shared_ : *local_; }
  Map &map() { return shared_ ? *shared_ : *local_; }

  AbstractValue &operator[](uint32_t key) { return map()[key]; }
  const AbstractValue &at(uint32_t key) const { return map().at(key); }
  AbstractValue &at(uint32_t key) { return map().at(key); }

  iterator find(uint32_t key) { return map().find(key); }
  const_iterator find(uint32_t key) const { return map().find(key); }
  size_t count(uint32_t key) const { return map().count(key); }
  bool empty() const { return map().empty(); }
  size_t size() const { return map().size(); }
  void clear() { map().clear(); }
  iterator begin() { return map().begin(); }
  iterator end() { return map().end(); }
  const_iterator begin() const { return map().begin(); }
  const_iterator end() const { return map().end(); }

  template <typename... Args> auto emplace(Args &&...args) {
    return map().emplace(std::forward<Args>(args)...);
  }

private:
  std::unique_ptr<Map> local_;
  Map *shared_ = nullptr;
};

/// AbstractState represents the abstract execution state at a program point.
///
/// The state consists of two main mappings:
/// 1. Variable-to-value map (_varToAbsVal): Maps SSA value IDs to abstract
/// values
/// 2. Address-to-value map (_addrToAbsVal): Maps memory object IDs to abstract
/// values
///
/// Abstract values can be:
/// - Intervals: Numeric ranges like [0, 100] or [-∞, +∞]
/// - Addresses: Sets of memory object IDs that a pointer may point to
///
/// Example:
///   int x = 5;           // _varToAbsVal[x_id] = IntervalValue(5, 5)
///   int *p = &x;         // _varToAbsVal[p_id] = AddressValue({x_obj_id})
///   int arr[10];         // _addrToAbsVal[arr_obj_id] = IntervalValue::top()
///   free(p);             // _freedAddrs.insert(x_obj_id)
///
class AbstractState {
public:
  /// Map from variable ID to abstract value
  using VarToAbsValMap = std::unordered_map<uint32_t, AbstractValue>;

  /// Map from memory address/object ID to abstract value
  using AddrToAbsValMap = VarToAbsValMap;

  /// Special sentinel IDs for null and black-hole pointers
  static constexpr uint32_t NullPtr = 0; ///< Null pointer constant (ID 0)
  static constexpr uint32_t BlkPtr =
      2; ///< Black-hole pointer (absorbs all unknown pointers)

  /// Outcome of the strong/weak-update decision for a memory store.
  ///
  /// Mirrors SVF's store semantics: a store through a pointer only *replaces*
  /// the target object's value when that is justified (`Strong`); otherwise it
  /// is joined into what the object already holds (`Weak`).
  enum class StoreKind { Strong, Weak };

  /// Set of freed memory object IDs (for use-after-free detection)
  std::unordered_set<uint32_t> _freedAddrs;
  std::unordered_set<uint32_t> _pendingFreedAddrs;
  std::unordered_set<uint32_t> _heapObjs;

  /// Objects whose fields were merged into the base because a variant
  /// (non-constant) GEP reached them. Metadata only: copied with the state and
  /// deliberately not part of the lattice order, like `_heapObjs`.
  std::unordered_set<uint32_t> _fieldInsensitiveObjs;

  /// Maps SSA value IDs to their abstract values (intervals or addresses).
  ///
  /// A `VarTable`, not a raw map: in the def-site layout the entries live in a
  /// single run-wide table instead of being duplicated per instruction.
  VarTable _varToAbsVal;

  /// Maps memory object IDs to their abstract values (heap/stack contents)
  AddrToAbsValMap _addrToAbsVal;

  /// Pointer to SVFIR wrapper for querying pointer analysis results
  SVFIRWrapper *svfir_ = nullptr;

  /// Default constructor - creates an empty abstract state
  AbstractState() {}

  /// Construct from existing variable and address maps
  AbstractState(VarToAbsValMap &_varToValMap, AddrToAbsValMap &_locToValMap)
      : _varToAbsVal(_varToValMap), _addrToAbsVal(_locToValMap) {}

  /// Copy constructor - performs deep copy of all state components
  AbstractState(const AbstractState &rhs)
      : _freedAddrs(rhs._freedAddrs),
        _pendingFreedAddrs(rhs._pendingFreedAddrs), _heapObjs(rhs._heapObjs),
        _fieldInsensitiveObjs(rhs._fieldInsensitiveObjs),
        _varToAbsVal(rhs._varToAbsVal), _addrToAbsVal(rhs._addrToAbsVal),
        svfir_(rhs.svfir_), _objToSize(rhs._objToSize),
        _gepObjOffsetFromBase(rhs._gepObjOffsetFromBase),
        pathLocalVarTable_(rhs.pathLocalVarTable_) {}

  virtual ~AbstractState() = default;

  /// Copy assignment operator
  AbstractState &operator=(const AbstractState &rhs) {
    if (rhs != *this) {
      _varToAbsVal = rhs._varToAbsVal;
      _addrToAbsVal = rhs._addrToAbsVal;
      _freedAddrs = rhs._freedAddrs;
      _pendingFreedAddrs = rhs._pendingFreedAddrs;
      _heapObjs = rhs._heapObjs;
      _fieldInsensitiveObjs = rhs._fieldInsensitiveObjs;
      _objToSize = rhs._objToSize;
      _gepObjOffsetFromBase = rhs._gepObjOffsetFromBase;
      svfir_ = rhs.svfir_;
      pathLocalVarTable_ = rhs.pathLocalVarTable_;
    }
    return *this;
  }

  /// Move constructor - transfers ownership of state components
  AbstractState(AbstractState &&rhs)
      : _freedAddrs(std::move(rhs._freedAddrs)),
        _pendingFreedAddrs(std::move(rhs._pendingFreedAddrs)),
        _heapObjs(std::move(rhs._heapObjs)),
        _varToAbsVal(std::move(rhs._varToAbsVal)),
        _addrToAbsVal(std::move(rhs._addrToAbsVal)), svfir_(rhs.svfir_),
        _objToSize(std::move(rhs._objToSize)),
        _gepObjOffsetFromBase(std::move(rhs._gepObjOffsetFromBase)),
        pathLocalVarTable_(rhs.pathLocalVarTable_) {}

  /// Move assignment operator
  AbstractState &operator=(AbstractState &&rhs) {
    if (&rhs != this) {
      _varToAbsVal = std::move(rhs._varToAbsVal);
      _addrToAbsVal = std::move(rhs._addrToAbsVal);
      _freedAddrs = std::move(rhs._freedAddrs);
      _pendingFreedAddrs = std::move(rhs._pendingFreedAddrs);
      _heapObjs = std::move(rhs._heapObjs);
      _objToSize = std::move(rhs._objToSize);
      _gepObjOffsetFromBase = std::move(rhs._gepObjOffsetFromBase);
      svfir_ = rhs.svfir_;
      pathLocalVarTable_ = rhs.pathLocalVarTable_;
    }
    return *this;
  }

  /// Access abstract value for a variable (mutable). In the def-site layout a
  /// missing entry receives the type-directed fallback defined by
  /// AbstractState.cpp instead of being exposed as bottom.
  AbstractValue &operator[](uint32_t varId);

  /// Access abstract value for a variable (const)
  const AbstractValue &operator[](uint32_t varId) const {
    return _varToAbsVal.at(varId);
  }

  /// Check if a variable ID maps to an address value (pointer)
  /// @param id Variable ID to check
  /// @return true if the variable holds an address set, false otherwise
  bool inVarToAddrsTable(uint32_t id) const {
    if (_varToAbsVal.find(id) != _varToAbsVal.end()) {
      if (_varToAbsVal.at(id).isAddr()) {
        return true;
      }
    }
    return false;
  }

  /// Check if a variable ID maps to an interval value (numeric)
  /// @param id Variable ID to check
  /// @return true if the variable holds an interval, false otherwise
  bool inVarToValTable(uint32_t id) const {
    if (_varToAbsVal.find(id) != _varToAbsVal.end()) {
      if (_varToAbsVal.at(id).isInterval()) {
        return true;
      }
    }
    return false;
  }

  /// Check if a memory address maps to an address value (pointer stored in
  /// memory)
  /// @param id Memory object ID to check
  /// @return true if the memory location holds an address set, false otherwise
  bool inAddrToAddrsTable(uint32_t id) const {
    if (_addrToAbsVal.find(id) != _addrToAbsVal.end()) {
      if (_addrToAbsVal.at(id).isAddr()) {
        return true;
      }
    }
    return false;
  }

  /// Check if a memory address maps to an interval value (numeric stored in
  /// memory)
  /// @param id Memory object ID to check
  /// @return true if the memory location holds an interval, false otherwise
  bool inAddrToValTable(uint32_t id) const {
    if (_addrToAbsVal.find(id) != _addrToAbsVal.end()) {
      if (_addrToAbsVal.at(id).isInterval()) {
        return true;
      }
    }
    return false;
  }

  /// Get the variable-to-value mapping (read-only)
  const VarToAbsValMap &getVarToVal() const { return _varToAbsVal.map(); }

  /// The variable table facade (layout-aware).
  const VarTable &varTable() const { return _varToAbsVal; }
  VarTable &varTable() { return _varToAbsVal; }

  /// Rebinds the variable table to the run-wide def-site map.
  void bindDefSiteVarTable(VarToAbsValMap *shared) {
    _varToAbsVal.bindDefSite(shared);
    pathLocalVarTable_ = false;
  }

  bool usesDefSiteVarTable() const { return _varToAbsVal.isDefSite(); }
  bool usesPathLocalVarTable() const { return pathLocalVarTable_; }

  /// Detaches from the shared table, optionally keeping only @p ids.
  /// Used by widening/narrowing, which must not mutate the shared table.
  void materializeVarTable(const std::unordered_set<uint32_t> *only = nullptr) {
    if (only)
      _varToAbsVal.detachToLocal(*only);
    else
      _varToAbsVal.detachToLocal();
    pathLocalVarTable_ = true;
  }

  /// Get the address-to-value mapping (read-only)
  const AddrToAbsValMap &getLocToVal() const { return _addrToAbsVal; }

  /// Widening operator: over-approximates the join to ensure termination
  /// Used in fixpoint iteration to accelerate convergence by extrapolating
  /// trends. Example: [0,10] ⊔ [0,20] with widening → [0,+∞]
  AbstractState widening(const AbstractState &other);

  /// Narrowing operator: refines over-approximations from widening
  /// Used after widening reaches fixpoint to improve precision.
  /// Example: [0,+∞] ⊓ [0,100] with narrowing → [0,100]
  AbstractState narrowing(const AbstractState &other);

  /// Join this state with another (in-place union/least upper bound)
  /// Merges two abstract states by taking the join of all abstract values.
  void joinWith(const AbstractState &other);

  /// Meet this state with another (in-place intersection/greatest lower bound)
  /// Refines this state by taking the meet of all abstract values.
  void meetWith(const AbstractState &other);

  /// Mark a memory address as freed (for use-after-free detection)
  /// @param addr Memory object ID to mark as freed
  void addToFreedAddrs(uint32_t addr) {
    if (AddressValue::isVirtualMemAddress(addr)) {
      _freedAddrs.insert(AddressValue::getInternalID(addr));
    } else {
      _freedAddrs.insert(addr);
    }
  }

  void addToPendingFreedAddrs(uint32_t addr) {
    if (AddressValue::isVirtualMemAddress(addr)) {
      _pendingFreedAddrs.insert(AddressValue::getInternalID(addr));
    } else {
      _pendingFreedAddrs.insert(addr);
    }
  }

  void commitPendingFrees() {
    _freedAddrs.insert(_pendingFreedAddrs.begin(), _pendingFreedAddrs.end());
    _pendingFreedAddrs.clear();
  }

  void clearPendingFrees() { _pendingFreedAddrs.clear(); }

  void addHeapObject(uint32_t objId) { _heapObjs.insert(objId); }

  bool isHeapObject(uint32_t objId) const {
    return _heapObjs.find(objId) != _heapObjs.end();
  }

  /// Records that @p objId had its fields merged into the base object because a
  /// variant (non-constant) GEP reached it.
  void markFieldInsensitiveObject(uint32_t objId) {
    _fieldInsensitiveObjs.insert(objId);
  }

  bool isFieldInsensitiveObject(uint32_t objId) const {
    return _fieldInsensitiveObjs.find(objId) != _fieldInsensitiveObjs.end();
  }

  /// Check if a memory address has been freed
  /// @param addr Memory object ID to check
  /// @return true if the address was previously freed, false otherwise
  bool isFreedMem(uint32_t addr) const {
    uint32_t objId = AddressValue::isVirtualMemAddress(addr)
                         ? AddressValue::getInternalID(addr)
                         : addr;
    return _freedAddrs.find(objId) != _freedAddrs.end();
  }

  /// Decides whether a store through a pointer with address set @p targets may
  /// *replace* the pointed-to object rather than be joined into it.
  ///
  /// Ports the guard from SVF's `AbstractInterpretation::isStrongUpdate`
  /// (`svf/lib/AE/Svfexe/AbstractStateManager.cpp`): only a singleton,
  /// non-summary object may be strongly updated. Heap objects, objects whose
  /// fields were merged by a variant GEP, and oversized aggregates are all
  /// summaries and must be joined. The checks that need call-graph and type
  /// information (recursive-function locals, array bases) live in
  /// `AbstractInterpretation::isStrongUpdateForStore`.
  bool allowsStrongUpdate(const AddressValue &targets) const {
    if (targets.size() != 1)
      return false;
    const uint32_t addr = *targets.begin();
    if (!AddressValue::isVirtualMemAddress(addr))
      return false;
    if (isNullMem(addr) || isInvalidMem(addr))
      return false;
    const uint32_t objId = AddressValue::getInternalID(addr);
    if (objId == NullPtr || objId == BlkPtr)
      return false;
    if (isHeapObject(objId))
      return false;
    if (isFieldInsensitiveObject(objId))
      return false;
    const uint32_t size = getObjSize(objId);
    if (size > MaxFieldLimit)
      return false;
    return true;
  }

  /// Store an abstract value to a memory address
  /// @param addr Virtual memory address (encoded object ID)
  /// @param val Abstract value to store
  /// Skips stores to null memory or freed memory (for safety)
  void store(uint32_t addr, const AbstractValue &val) {
    if (!AddressValue::isVirtualMemAddress(addr))
      return;
    uint32_t objId = getIDFromAddr(addr);
    if (isNullMem(addr))
      return;
    // Check for freed memory before storing
    if (isFreedMem(addr))
      return;
    _addrToAbsVal[objId] = val;
  }

  /// Join @p val into the value already held at @p addr (weak update).
  /// Skips non-virtual, null and freed memory, like `store`.
  void weakStore(uint32_t addr, const AbstractValue &val) {
    if (!AddressValue::isVirtualMemAddress(addr))
      return;
    if (isNullMem(addr) || isFreedMem(addr))
      return;
    _addrToAbsVal[getIDFromAddr(addr)].join_with(val);
  }

  /// Load an abstract value from a memory address
  /// @param addr Virtual memory address (encoded object ID)
  /// @return Reference to the abstract value at that address
  AbstractValue &load(uint32_t addr) {
    assert(AddressValue::isVirtualMemAddress(addr) && "not virtual address?");
    uint32_t objId = getIDFromAddr(addr);
    return _addrToAbsVal[objId];
  }

  /// Print the abstract state to stderr (for debugging)
  void printAbstractState() const;

  /// Convert state to string representation
  std::string toString() const { return ""; }

  /// Check if this state equals another state
  bool equals(const AbstractState &other) const;

  /// Compute hash of this state
  uint32_t hash() const;

  static bool eqVarToValMap(const VarToAbsValMap &lhs,
                            const VarToAbsValMap &rhs) {
    if (lhs.size() != rhs.size())
      return false;
    for (const auto &item : lhs) {
      auto it = rhs.find(item.first);
      if (it == rhs.end())
        return false;
      if (!item.second.equals(it->second))
        return false;
    }
    return true;
  }

  static bool geqVarToValMap(const VarToAbsValMap &lhs,
                             const VarToAbsValMap &rhs) {
    if (rhs.empty())
      return true;
    for (const auto &item : rhs) {
      auto it = lhs.find(item.first);
      if (it == lhs.end())
        return false;
      if (!it->second.getInterval().contain(item.second.getInterval()))
        return false;
    }
    return true;
  }

  static bool geqFreedAddrs(const std::unordered_set<uint32_t> &lhs,
                            const std::unordered_set<uint32_t> &rhs) {
    for (uint32_t addr : rhs) {
      if (lhs.find(addr) == lhs.end())
        return false;
    }
    return true;
  }

  static bool geqObjSizeMap(const std::unordered_map<uint32_t, uint32_t> &lhs,
                            const std::unordered_map<uint32_t, uint32_t> &rhs) {
    if (lhs.size() != rhs.size())
      return false;
    for (const auto &item : rhs) {
      auto it = lhs.find(item.first);
      if (it == lhs.end())
        return false;
      // Treat object size as exact metadata for lattice-order checks.
      if (it->second != item.second)
        return false;
    }
    return true;
  }

  bool operator==(const AbstractState &rhs) const {
    return eqVarToValMap(_varToAbsVal.map(), rhs.getVarToVal()) &&
           eqVarToValMap(_addrToAbsVal, rhs.getLocToVal()) &&
           _freedAddrs == rhs._freedAddrs && _objToSize == rhs._objToSize;
  }

  bool operator!=(const AbstractState &rhs) const { return !(*this == rhs); }

  bool operator<(const AbstractState &rhs) const { return !(*this >= rhs); }

  bool operator>=(const AbstractState &rhs) const {
    return geqVarToValMap(_varToAbsVal.map(), rhs.getVarToVal()) &&
           geqVarToValMap(_addrToAbsVal, rhs.getLocToVal()) &&
           geqFreedAddrs(_freedAddrs, rhs._freedAddrs) &&
           geqObjSizeMap(_objToSize, rhs._objToSize);
  }

  void clear() {
    _addrToAbsVal.clear();
    _varToAbsVal.clear();
    _freedAddrs.clear();
    _gepObjOffsetFromBase.clear();
  }

  AbstractState bottom() const {
    AbstractState inv = *this;
    inv.materializeVarTable();
    for (auto &item : inv._varToAbsVal) {
      if (item.second.isInterval())
        item.second.getInterval().set_to_bottom();
    }
    return inv;
  }

  AbstractState top() const {
    AbstractState inv = *this;
    inv.materializeVarTable();
    for (auto &item : inv._varToAbsVal) {
      if (item.second.isInterval())
        item.second.getInterval().set_to_top();
    }
    return inv;
  }

  static inline bool isNullMem(uint32_t addr) { return addr == NullMemAddr; }
  static inline bool isInvalidMem(uint32_t addr) {
    return addr == InvalidMemAddr;
  }

  static inline uint32_t getVirtualMemAddress(uint32_t idx) {
    return AddressValue::getVirtualMemAddress(idx);
  }

  static inline bool isVirtualMemAddress(uint32_t val) {
    return AddressValue::isVirtualMemAddress(val);
  }

  inline uint32_t getIDFromAddr(uint32_t addr) const {
    uint32_t objId = AddressValue::getInternalID(addr);
    return _freedAddrs.count(objId)
               ? AddressValue::getInternalID(InvalidMemAddr)
               : objId;
  }

  AddressValue getGepObjAddrs(uint32_t pointer, IntervalValue offset);
  AbstractValue loadValue(uint32_t varId);
  /// Store @p valId through the pointer @p varId.
  ///
  /// @param kind `StoreKind::Strong` replaces the target object's value,
  ///             `StoreKind::Weak` joins into it. Callers decide with
  ///             `AbstractInterpretation::isStrongUpdateForStore`.
  void storeValue(uint32_t varId, uint32_t valId,
                  StoreKind kind = StoreKind::Weak);

  // GEP offset computation
  IntervalValue getByteOffset(const llvm::GetElementPtrInst *gep);
  IntervalValue getElementIndex(const llvm::GetElementPtrInst *gep);
  uint32_t getAllocaInstByteSize(const llvm::AllocaInst *alloca);
  uint32_t getAllocaInstByteSize(const llvm::AllocaInst *alloca,
                                 const AbstractState &as);

  AddressValue getGepObjAddrs(uint32_t pointer, IntervalValue offset,
                              const llvm::GetElementPtrInst *gep);
  uint32_t getGepFieldSize(llvm::Type *srcType, int64_t offset,
                           const llvm::DataLayout &dl);

  // Object initialization
  void initObjVar(const llvm::Value *objVar);

  // Pointer to SVFIRWrapper for PTA-based queries
  void setSVFIRWrapper(SVFIRWrapper *wrapper) { svfir_ = wrapper; }
  SVFIRWrapper *getSVFIRWrapper() const { return svfir_; }

  // Type queries using PTA
  const llvm::Type *getPointeeElement(uint32_t id);

  // Get object size using PTA
  uint32_t getObjectSize(const llvm::Value *obj) const;

  // Get points-to set for a pointer using PTA
  // Returns vector of objects (as void* to avoid template in header)
  void getPointsToSet(const llvm::Value *ptr,
                      std::vector<void *> &result) const;

  // Object size tracking (public for AE use)
  void setObjSize(uint32_t objId, uint32_t size) { _objToSize[objId] = size; }
  bool hasKnownObjSize(uint32_t objId) const {
    return _objToSize.find(objId) != _objToSize.end();
  }
  uint32_t getObjSize(uint32_t objId) const {
    auto it = _objToSize.find(objId);
    return it != _objToSize.end() ? it->second : 0;
  }

  void recordGepObjOffsetFromBase(uint32_t objId, const IntervalValue &offset);

  bool getGepObjOffsetFromBase(uint32_t objId, IntervalValue &offset) const;

private:
  // Object size tracking (storage)
  std::unordered_map<uint32_t, uint32_t> _objToSize;

  // GEP object offset tracking (similar to SVF's GepObjVar)
  // Maps GEP instruction pointer -> offset from base object
  std::unordered_map<const llvm::GetElementPtrInst *, IntervalValue>
      _gepObjOffsetFromBase;
  /// Def-site snapshots carrying path-specific refinements stay local so the
  /// shared SSA table is not polluted by a sibling edge.
  bool pathLocalVarTable_ = false;
  void setGepObjOffsetFromBase(const llvm::GetElementPtrInst *gep,
                               const IntervalValue &offset) {
    _gepObjOffsetFromBase[gep] = offset;
  }
  bool hasGepObjOffsetFromBase(const llvm::GetElementPtrInst *gep) const {
    return _gepObjOffsetFromBase.find(gep) != _gepObjOffsetFromBase.end();
  }
  IntervalValue
  getGepObjOffsetFromBase(const llvm::GetElementPtrInst *gep) const {
    auto it = _gepObjOffsetFromBase.find(gep);
    if (it != _gepObjOffsetFromBase.end()) {
      return it->second;
    }
    return IntervalValue(0, 0);
  }

  // Get the run-wide field-sensitive GEP object ID for (base, offset). SVF
  // models these as SVFIR objects, so all control-flow paths must agree on the
  // same identity for a given base/offset pair.
  uint32_t getGepFieldObjId(uint32_t baseObjId, int64_t offset);

  AbstractState sliceState(std::set<uint32_t> &sl) const {
    AbstractState inv;
    for (uint32_t id : sl) {
      if (_varToAbsVal.find(id) != _varToAbsVal.end()) {
        inv._varToAbsVal[id] = _varToAbsVal.at(id);
      }
    }
    return inv;
  }
};

} // namespace analysis
} // namespace lotus
