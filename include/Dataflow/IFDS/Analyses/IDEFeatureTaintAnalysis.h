#pragma once

#include "Dataflow/IFDS/Core/IFDSFramework.h"

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace llvm {
class GlobalVariable;
} // namespace llvm

namespace ifds {

/// The lattice value used by feature taint.  The mask member is retained for
/// compatibility with the original Lotus API; named_features is an unbounded
/// feature set for clients that need more than 64 features.
struct FeatureTaintValue {
  enum Kind { Bottom, Features, Top } kind;
  uint64_t mask;
  std::set<std::string> named_features;
  using FeatureSet = std::set<std::string>;

  FeatureTaintValue() : kind(Bottom), mask(0) {}
  FeatureTaintValue(Kind k, uint64_t m) : kind(k), mask(m) {}
  FeatureTaintValue(Kind k, uint64_t m, FeatureSet features)
      : kind(k), mask(m), named_features(std::move(features)) {}

  static FeatureTaintValue bottom() { return FeatureTaintValue(Bottom, 0); }
  static FeatureTaintValue top() { return FeatureTaintValue(Top, 0); }
  static FeatureTaintValue features(uint64_t m) {
    return FeatureTaintValue(Features, m);
  }
  static FeatureTaintValue features(FeatureSet features) {
    return FeatureTaintValue(Features, 0, std::move(features));
  }
  static FeatureTaintValue features(uint64_t m, FeatureSet features) {
    return FeatureTaintValue(Features, m, std::move(features));
  }

  bool empty() const {
    return kind == Bottom || (mask == 0 && named_features.empty());
  }
  bool has_feature(const std::string &feature) const {
    return named_features.count(feature) != 0;
  }
  bool operator==(const FeatureTaintValue &other) const {
    return kind == other.kind && mask == other.mask &&
           named_features == other.named_features;
  }
  bool operator!=(const FeatureTaintValue &other) const {
    return !(*this == other);
  }
};

/// Callback-driven feature generation, modelled after PhASAR's feature
/// generator but using Lotus's existing IDE value domain.  A generator can
/// return any iterable collection whose elements can be converted to strings;
/// the string names make the feature domain unbounded while keeping it easy to
/// inspect in clients and reports.
class FeatureTaintGenerator {
public:
  using InstOrGlobal =
      std::variant<const llvm::Instruction *, const llvm::GlobalVariable *>;
  using FeatureSet = FeatureTaintValue::FeatureSet;
  using IsSourceFn = std::function<bool(InstOrGlobal)>;
  using GenerateFeaturesFn = std::function<FeatureSet(InstOrGlobal)>;
  using GenerateTaintsFn = GenerateFeaturesFn;
  using PrinterFn = std::function<std::string(const FeatureTaintValue &)>;

  FeatureTaintGenerator();
  explicit FeatureTaintGenerator(GenerateFeaturesFn generator);

  template <typename FeatureGeneratorFn>
  static GenerateFeaturesFn
  createGenerateTaints(FeatureGeneratorFn &&generator) {
    return [generator = std::forward<FeatureGeneratorFn>(generator)](
               InstOrGlobal value) {
      return to_feature_set(std::invoke(generator, value));
    };
  }
  FeatureTaintGenerator(IsSourceFn source, GenerateFeaturesFn generator,
                        PrinterFn printer = {});

  template <typename SourceDetector, typename FeatureGeneratorFn>
  FeatureTaintGenerator(SourceDetector &&source, FeatureGeneratorFn &&generator)
      : FeatureTaintGenerator(
            IsSourceFn(std::forward<SourceDetector>(source)),
            GenerateFeaturesFn([generator = std::forward<FeatureGeneratorFn>(
                                    generator)](InstOrGlobal value) {
              return to_feature_set(std::invoke(generator, value));
            })) {}

  template <typename FeatureGeneratorFn,
            typename = std::enable_if_t<!std::is_same_v<
                std::decay_t<FeatureGeneratorFn>, FeatureTaintGenerator>>>
  explicit FeatureTaintGenerator(FeatureGeneratorFn &&generator)
      : FeatureTaintGenerator(GenerateFeaturesFn(
            [generator = std::forward<FeatureGeneratorFn>(generator)](
                InstOrGlobal value) {
              return to_feature_set(std::invoke(generator, value));
            })) {}

  bool is_source(InstOrGlobal value) const { return m_source(value); }
  // PhASAR-compatible spelling retained for clients migrating generators.
  bool isSource(InstOrGlobal value) const { return is_source(value); }
  FeatureTaintValue generate(InstOrGlobal value) const {
    return FeatureTaintValue::features(m_generator(value));
  }
  FeatureTaintValue getGeneratedTaintsAt(InstOrGlobal value) const {
    return generate(value);
  }
  std::string print(const FeatureTaintValue &value) const {
    return m_printer ? m_printer(value) : default_print(value);
  }
  std::string toString(const FeatureTaintValue &value) const {
    return print(value);
  }

private:
  template <typename T> static std::string feature_name(const T &value) {
    using U = std::decay_t<T>;
    if constexpr (std::is_same_v<U, std::string>) {
      return value;
    } else if constexpr (std::is_same_v<U, const char *> ||
                         std::is_same_v<U, char *>) {
      return std::string(value);
    } else if constexpr (std::is_arithmetic_v<U>) {
      return std::to_string(value);
    } else if constexpr (std::is_enum_v<U>) {
      using Underlying = std::underlying_type_t<U>;
      return std::to_string(static_cast<Underlying>(value));
    } else {
      return std::string(value);
    }
  }

  template <typename T> static FeatureSet to_feature_set(const T &value) {
    if constexpr (std::is_same_v<std::decay_t<T>, FeatureTaintValue>) {
      return value.named_features;
    } else if constexpr (std::is_same_v<std::decay_t<T>, std::string> ||
                         std::is_arithmetic_v<std::decay_t<T>> ||
                         std::is_enum_v<std::decay_t<T>>) {
      return {feature_name(value)};
    } else {
      FeatureSet result;
      for (const auto &feature : value) {
        result.insert(feature_name(feature));
      }
      return result;
    }
  }

  static std::string default_print(const FeatureTaintValue &value);

  IsSourceFn m_source;
  GenerateFeaturesFn m_generator;
  PrinterFn m_printer;
};

class IDEFeatureTaintAnalysis
    : public DefaultNoAliasIDEProblem<const llvm::Value *, FeatureTaintValue> {
public:
  using Fact = const llvm::Value *;
  using Value = FeatureTaintValue;

  IDEFeatureTaintAnalysis();
  explicit IDEFeatureTaintAnalysis(FeatureTaintGenerator generator);

  template <typename SourceDetector, typename FeatureGeneratorFn>
  IDEFeatureTaintAnalysis(SourceDetector &&source,
                          FeatureGeneratorFn &&generator)
      : IDEFeatureTaintAnalysis(FeatureTaintGenerator(
            std::forward<SourceDetector>(source),
            std::forward<FeatureGeneratorFn>(generator))) {}

  Fact zero_fact() const override { return nullptr; }
  FactSet normal_flow(const llvm::Instruction *stmt,
                      const llvm::Instruction *succ, const Fact &fact) override;
  FactSet call_flow(const llvm::CallBase *call, const llvm::Function *callee,
                    const Fact &fact) override;
  FactSet return_flow(const llvm::CallBase *call,
                      const llvm::Instruction *exit_inst,
                      const llvm::Instruction *return_site,
                      const llvm::Function *callee, const Fact &exit_fact,
                      const Fact &call_fact) override;
  FactSet call_to_return_flow(const llvm::CallBase *call,
                              const llvm::Instruction *return_site,
                              llvm::ArrayRef<const llvm::Function *> callees,
                              const Fact &fact) override;
  FactSet initial_facts(const llvm::Function *main) override;
  IDEInitialSeeds initial_ide_seeds(const llvm::Module &module) override;

  Value top_value() const override { return Value::top(); }
  Value bottom_value() const override { return Value::bottom(); }
  Value join(const Value &v1, const Value &v2) const override;

  EdgeFunction normal_edge_function(const llvm::Instruction *stmt,
                                    const llvm::Instruction *succ,
                                    const Fact &src_fact,
                                    const Fact &tgt_fact) override;
  EdgeFunction call_edge_function(const llvm::CallBase *call,
                                  const llvm::Function *callee,
                                  const Fact &src_fact,
                                  const Fact &tgt_fact) override;
  EdgeFunction return_edge_function(const llvm::CallBase *call,
                                    const llvm::Function *callee,
                                    const llvm::Instruction *exit_inst,
                                    const llvm::Instruction *return_site,
                                    const Fact &exit_fact,
                                    const Fact &ret_fact) override;
  EdgeFunction call_to_return_edge_function(
      const llvm::CallBase *call, const llvm::Instruction *return_site,
      llvm::ArrayRef<const llvm::Function *> callees, const Fact &src_fact,
      const Fact &tgt_fact) override;

  FactSet summary_flow(const llvm::CallBase *call, const llvm::Function *callee,
                       const Fact &fact) override;
  EdgeFunction summary_edge_function(const llvm::CallBase *call,
                                     const llvm::Function *callee,
                                     const llvm::Instruction *return_site,
                                     const Fact &src_fact,
                                     const Fact &tgt_fact) override;

  const FeatureTaintGenerator *feature_generator() const {
    return m_has_feature_generator ? &m_feature_generator : nullptr;
  }

private:
  std::unordered_map<std::string, uint64_t> m_source_feature_bits;
  std::unordered_map<std::string, uint64_t> m_sanitizer_clears;
  FeatureTaintGenerator m_feature_generator;
  bool m_has_feature_generator = false;
};

} // namespace ifds
