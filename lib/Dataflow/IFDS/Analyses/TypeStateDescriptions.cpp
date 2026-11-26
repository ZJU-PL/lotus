// Adapted from PhASAR (88366fc69). Copyright (c) 2018, 2020 Philipp
// Schubert, Fabian Schiebel and others. MIT; see
// third-party/phasar/LICENSE.txt.
#include "Dataflow/IFDS/Analyses/TypeStateDescriptions.h"

#include <stdexcept>
#include <utility>

namespace ifds {
namespace {
std::function<bool(const llvm::CallBase *)>
requirePrerequisite(std::function<bool(const llvm::CallBase *)> query) {
  if (!query)
    throw std::invalid_argument("typestate prerequisite query cannot be empty");
  return query;
}
} // namespace

TableTypeStateDescription::TableTypeStateDescription(
    std::string type, int initial, int error, std::vector<Operation> operations,
    Prerequisite prerequisite, bool factory_only)
    : m_type(std::move(type)), m_initial(initial), m_error(error),
      m_prerequisite(std::move(prerequisite)), m_factory_only(factory_only) {
  for (auto &op : operations) {
    auto name = op.name;
    m_operations.emplace(std::move(name), std::move(op));
  }
}
bool TableTypeStateDescription::is_factory_function(
    llvm::StringRef name) const {
  auto it = m_operations.find(name.str());
  return it != m_operations.end() && it->second.factory;
}
bool TableTypeStateDescription::is_consuming_function(
    llvm::StringRef name) const {
  auto it = m_operations.find(name.str());
  return it != m_operations.end() && !it->second.factory;
}
std::set<int> TableTypeStateDescription::get_consumer_param_indices(
    llvm::StringRef name) const {
  auto it = m_operations.find(name.str());
  return it != m_operations.end() && !it->second.factory ? it->second.parameters
                                                         : std::set<int>{};
}
std::set<int> TableTypeStateDescription::get_factory_param_indices(
    llvm::StringRef name) const {
  auto it = m_operations.find(name.str());
  return it != m_operations.end() && it->second.factory ? it->second.parameters
                                                        : std::set<int>{};
}
int TableTypeStateDescription::get_next_state(
    llvm::StringRef name, int state, const llvm::CallBase *call) const {
  auto it = m_operations.find(name.str());
  if (it == m_operations.end())
    return state; // unrelated calls do not change this protocol
  if (state < 0 || static_cast<size_t>(state) >= it->second.transitions.size())
    return m_error;
  if (m_prerequisite && (!m_factory_only || it->second.factory) &&
      (!call || !m_prerequisite(call)))
    return m_error;
  return it->second.transitions[state];
}

CSTDFILEIOTypeStateDescription::CSTDFILEIOTypeStateDescription()
    : TableTypeStateDescription(
          "_IO_FILE", 0, 3,
          {
              {"fopen", true, {-1}, {1, 1, 1, 3, 1}},
              {"fdopen", true, {-1}, {1, 1, 1, 3, 1}},
              {"fclose", false, {0}, {3, 2, 3, 3, 4}},
              {"fread", false, {3}, {3, 1, 3, 3, 4}},
              {"fwrite", false, {3}, {3, 1, 3, 3, 4}},
              {"fgetc", false, {0}, {3, 1, 3, 3, 4}},
              {"fgetwc", false, {0}, {3, 1, 3, 3, 4}},
              {"fgets", false, {2}, {3, 1, 3, 3, 4}},
              {"getc", false, {0}, {3, 1, 3, 3, 4}},
              {"getwc", false, {0}, {3, 1, 3, 3, 4}},
              {"_IO_getc", false, {0}, {3, 1, 3, 3, 4}},
              {"ungetc", false, {1}, {3, 1, 3, 3, 4}},
              {"ungetwc", false, {1}, {3, 1, 3, 3, 4}},
              {"fputc", false, {1}, {3, 1, 3, 3, 4}},
              {"fputwc", false, {1}, {3, 1, 3, 3, 4}},
              {"fputs", false, {1}, {3, 1, 3, 3, 4}},
              {"putc", false, {1}, {3, 1, 3, 3, 4}},
              {"putwc", false, {1}, {3, 1, 3, 3, 4}},
              {"_IO_putc", false, {1}, {3, 1, 3, 3, 4}},
              {"fprintf", false, {0}, {3, 1, 3, 3, 4}},
              {"fwprintf", false, {0}, {3, 1, 3, 3, 4}},
              {"vfprintf", false, {0}, {3, 1, 3, 3, 4}},
              {"vfwprintf", false, {0}, {3, 1, 3, 3, 4}},
              {"__isoc99_fscanf", false, {0}, {3, 1, 3, 3, 4}},
              {"fscanf", false, {0}, {3, 1, 3, 3, 4}},
              {"fwscanf", false, {0}, {3, 1, 3, 3, 4}},
              {"vfscanf", false, {0}, {3, 1, 3, 3, 4}},
              {"vfwscanf", false, {0}, {3, 1, 3, 3, 4}},
              {"fflush", false, {0}, {3, 1, 3, 3, 4}},
              {"fseek", false, {0}, {3, 1, 3, 3, 4}},
              {"ftell", false, {0}, {3, 1, 3, 3, 4}},
              {"rewind", false, {0}, {3, 1, 3, 3, 4}},
              {"fgetpos", false, {0}, {3, 1, 3, 3, 4}},
              {"fsetpos", false, {0}, {3, 1, 3, 3, 4}},
              {"fileno", false, {0}, {3, 1, 3, 3, 4}},
          }) {}

OpenSSLEVPKDFDescription::OpenSSLEVPKDFDescription()
    : TableTypeStateDescription("evp_kdf_st", 0, 2,
                                {
                                    {"EVP_KDF_fetch", true, {-1}, {1, 2, 2, 1}},
                                    {"EVP_KDF_free", false, {0}, {2, 0, 2, 3}},
                                }) {}

OpenSSLEVPKDFCTXDescription::OpenSSLEVPKDFCTXDescription(
    std::function<bool(const llvm::CallBase *)> kdf_is_fetched)
    : TableTypeStateDescription(
          "evp_kdf_ctx_st", 5, 4,
          {
              {"EVP_KDF_CTX_new", true, {-1}, {1, 1, 1, 1, 4, 1}},
              {"EVP_KDF_CTX_set_params", false, {0}, {4, 2, 2, 2, 4, 4}},
              {"EVP_KDF_derive", false, {0}, {4, 4, 3, 3, 4, 4}},
              {"EVP_KDF_CTX_free", false, {0}, {4, 5, 5, 5, 4, 4}},
          },
          requirePrerequisite(std::move(kdf_is_fetched)), true) {}

OpenSSLSecureMemoryDescription::OpenSSLSecureMemoryDescription()
    : TableTypeStateDescription(
          "", 0, 3,
          {
              {"CRYPTO_malloc", true, {-1}, {4, 4, 4, 3, 4}},
              {"CRYPTO_zalloc", true, {-1}, {1, 1, 1, 3, 1}},
              {"CRYPTO_free", false, {0}, {3, 2, 3, 3, 3}},
              {"OPENSSL_cleanse", false, {0}, {0, 1, 3, 3, 1}},
          }) {}

OpenSSLSecureHeapDescription::OpenSSLSecureHeapDescription(
    std::function<bool(const llvm::CallBase *)> heap_is_initialized)
    : TableTypeStateDescription(
          "", 1, 5,
          {
              {"CRYPTO_secure_malloc", true, {-1}, {2, 2, 2, 2, 2, 2}},
              {"CRYPTO_secure_zalloc", true, {-1}, {3, 3, 3, 3, 3, 3}},
              {"CRYPTO_secure_free", false, {0}, {5, 5, 5, 4, 5, 5}},
              {"CRYPTO_secure_clear_free", false, {0}, {5, 5, 4, 4, 5, 5}},
          },
          requirePrerequisite(std::move(heap_is_initialized))) {}

} // namespace ifds
