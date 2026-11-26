// Adapted from PhASAR (88366fc69). Copyright (c) 2018, 2020 Philipp
// Schubert, Fabian Schiebel and others. MIT; see
// third-party/phasar/LICENSE.txt.
#pragma once

#include "Dataflow/IFDS/Analyses/IDETypeState.h"

namespace ifds {

/// Table-driven API protocols. State IDs are protocol-local; special lattice
/// values are represented by TypeStateValue, not by entries in these tables.
class TableTypeStateDescription : public TypeStateDescriptionBase {
public:
  bool is_factory_function(llvm::StringRef name) const override;
  bool is_consuming_function(llvm::StringRef name) const override;
  std::string get_type_name_of_interest() const override { return m_type; }
  std::set<int> get_consumer_param_indices(llvm::StringRef name) const override;
  std::set<int> get_factory_param_indices(llvm::StringRef name) const override;
  StateId uninitialized_state() const override { return m_initial; }
  StateId error_state() const override { return m_error; }
  StateId get_next_state(llvm::StringRef name, StateId state,
                         const llvm::CallBase *call) const override;

protected:
  struct Operation {
    std::string name;
    bool factory;
    std::set<int> parameters; // -1 denotes a returned object
    std::vector<int> transitions;
  };
  using Prerequisite = std::function<bool(const llvm::CallBase *)>;
  TableTypeStateDescription(std::string type, int initial, int error,
                            std::vector<Operation> operations,
                            Prerequisite prerequisite = {},
                            bool factory_only = false);

private:
  std::string m_type;
  int m_initial, m_error;
  std::unordered_map<std::string, Operation> m_operations;
  Prerequisite m_prerequisite;
  bool m_factory_only;
};

class CSTDFILEIOTypeStateDescription final : public TableTypeStateDescription {
public:
  enum State {
    Uninitialized = 0,
    Opened = 1,
    Closed = 2,
    Error = 3,
    Initial = 4
  };
  CSTDFILEIOTypeStateDescription();
};

class OpenSSLEVPKDFDescription final : public TableTypeStateDescription {
public:
  enum State { Uninitialized = 0, Fetched = 1, Error = 2, Initial = 3 };
  OpenSSLEVPKDFDescription();
};

class OpenSSLEVPKDFCTXDescription final : public TableTypeStateDescription {
public:
  enum State {
    Uninitialized = 5,
    Attached = 1,
    ParametersSet = 2,
    Derived = 3,
    Error = 4,
    Initial = 0
  };
  /// Required prior KDF-analysis query, evaluated at EVP_KDF_CTX_new.
  explicit OpenSSLEVPKDFCTXDescription(
      std::function<bool(const llvm::CallBase *)> kdf_is_fetched);
};

class OpenSSLSecureMemoryDescription final : public TableTypeStateDescription {
public:
  enum State { Initial = 0, Zeroed = 1, Freed = 2, Error = 3, Allocated = 4 };
  OpenSSLSecureMemoryDescription();
};

class OpenSSLSecureHeapDescription final : public TableTypeStateDescription {
public:
  enum State {
    Initial = 0,
    Uninitialized = 1,
    Allocated = 2,
    Zeroed = 3,
    Freed = 4,
    Error = 5
  };
  /// Required prior-analysis query. It is evaluated at each API call.
  explicit OpenSSLSecureHeapDescription(
      std::function<bool(const llvm::CallBase *)> heap_is_initialized);
};

} // namespace ifds
