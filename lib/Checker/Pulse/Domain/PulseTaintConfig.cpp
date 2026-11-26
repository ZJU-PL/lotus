#include "Checker/Pulse/Domain/PulseTaintConfig.h"

#include <algorithm>

#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/JSON.h>

namespace pulse {

namespace {

static ProcedureTarget parseTarget(llvm::StringRef target_str,
                                   ProcedureTarget fallback) {
  if (target_str == "ReturnValue")
    return ProcedureTarget::ReturnValue;
  if (target_str == "AllArguments")
    return ProcedureTarget::AllArguments;
  if (target_str == "ArgumentPositions")
    return ProcedureTarget::ArgumentPositions;
  if (target_str == "InstanceReference")
    return ProcedureTarget::InstanceReference;
  if (target_str == "AllArgumentsButPositions")
    return ProcedureTarget::AllArgumentsButPositions;
  return fallback;
}

static llvm::Expected<ProcedureUnit>
parseProcedureUnit(const llvm::json::Object &obj,
                   ProcedureTarget default_target,
                   const std::vector<TaintKind> &default_kinds) {
  const bool default_df_only =
      obj.getBoolean("data_flow_only").getValueOr(false);
  std::vector<TaintKind> kinds;
  if (const auto *arr = obj.getArray("kinds")) {
    for (const auto &v : *arr) {
      if (auto s = v.getAsString()) {
        kinds.emplace_back(s->str(), default_df_only);
      } else if (const auto *kobj = v.getAsObject()) {
        if (auto kn = kobj->getString("name")) {
          bool df = kobj->getBoolean("data_flow_only").getValueOr(default_df_only);
          kinds.emplace_back(kn->str(), df);
        }
      }
    }
  }
  if (kinds.empty()) {
    for (const auto &dk : default_kinds) {
      kinds.emplace_back(dk.getName(), dk.isDataFlowOnly() || default_df_only);
    }
  }

  ProcedureTarget target = default_target;
  if (auto t = obj.getString("target")) {
    target = parseTarget(*t, default_target);
  }

  std::vector<int> positions;
  if (const auto *pos_arr = obj.getArray("positions")) {
    for (const auto &v : *pos_arr) {
      if (auto n = v.getAsInteger()) {
        positions.push_back(static_cast<int>(*n));
      }
    }
  }

  std::optional<unsigned> field_idx;
  if (auto fi = obj.getInteger("field_index")) {
    if (*fi >= 0) {
      field_idx = static_cast<unsigned>(*fi);
    }
  }

  if (auto proc = obj.getString("procedure")) {
    ProcedureMatcher matcher(ProcedureMatcher::Type::ProcedureName,
                             proc->str());
    return ProcedureUnit(matcher, kinds, target, std::move(positions),
                         field_idx);
  }
  if (auto proc_re = obj.getString("procedure_regex")) {
    try {
      ProcedureMatcher matcher(ProcedureMatcher::Type::ProcedureNameRegex,
                               proc_re->str());
      return ProcedureUnit(matcher, kinds, target, std::move(positions),
                           field_idx);
    } catch (const std::regex_error &e) {
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "invalid procedure_regex '%s': %s",
                                     proc_re->str().c_str(), e.what());
    }
  }
  if (const auto *classes = obj.getArray("class_names")) {
    std::vector<std::string> cls_vec;
    std::vector<std::string> mth_vec;
    for (const auto &v : *classes) {
      if (auto s = v.getAsString())
        cls_vec.push_back(s->str());
    }
    if (const auto *methods = obj.getArray("method_names")) {
      for (const auto &v : *methods) {
        if (auto s = v.getAsString())
          mth_vec.push_back(s->str());
      }
    }
    ProcedureMatcher matcher =
        ProcedureMatcher::forClassAndMethods(std::move(cls_vec),
                                             std::move(mth_vec));
    return ProcedureUnit(matcher, kinds, target, std::move(positions),
                         field_idx);
  }

  return llvm::createStringError(
      llvm::inconvertibleErrorCode(),
      "taint unit requires 'procedure', 'procedure_regex', or 'class_names'");
}

} // namespace

bool ProcedureMatcher::matchesName(const std::string &func_name,
                                   bool is_intrinsic) const {
  if (func_name.empty()) {
    return false;
  }

  for (const auto &ex : exclude_names_) {
    if (func_name == ex) {
      return false;
    }
  }

  const std::string demangled = llvm::demangle(func_name);
  const std::string demangled_no_args =
      demangled.substr(0, demangled.find('('));

  for (const auto &ex_in : exclude_in_) {
    if (demangled_no_args.find(ex_in) != std::string::npos ||
        func_name.find(ex_in) != std::string::npos) {
      return false;
    }
  }

  switch (type_) {
  case Type::ProcedureName:
    return func_name == name_ || demangled_no_args == name_;

  case Type::ProcedureNameRegex:
    return std::regex_search(func_name, name_regex_) ||
           std::regex_search(demangled_no_args, name_regex_);

  case Type::BuiltinName:
    return is_intrinsic && func_name == name_;

  case Type::ClassNameRegex:
    return std::regex_search(demangled_no_args, name_regex_);

  case Type::ClassAndMethodNames: {
    bool class_match = class_names_.empty();
    for (const auto &cn : class_names_) {
      if (demangled_no_args.find(cn) != std::string::npos) {
        class_match = true;
        break;
      }
    }
    if (!class_match) {
      return false;
    }
    if (method_names_.empty()) {
      return true;
    }
    for (const auto &mn : method_names_) {
      if (demangled_no_args.find("::" + mn) != std::string::npos ||
          demangled_no_args == mn) {
        return true;
      }
    }
    return false;
  }
  }
  return false;
}

bool ProcedureMatcher::matches(const llvm::Function *func) const {
  if (!func)
    return false;
  return matchesName(func->getName().str(), func->isIntrinsic());
}

namespace {
static std::vector<const ProcedureUnit *> matchUnitsCached(
    const std::vector<ProcedureUnit> &units,
    std::unordered_map<std::string, std::vector<size_t>> &cache,
    const std::string &func_name, bool is_intrinsic) {
  std::vector<const ProcedureUnit *> out;
  if (units.empty() || func_name.empty()) {
    return out;
  }
  std::string key;
  key.reserve(func_name.size() + 2);
  key.push_back(is_intrinsic ? 'I' : 'F');
  key.push_back(':');
  key.append(func_name);

  auto it = cache.find(key);
  if (it == cache.end()) {
    std::vector<size_t> indices;
    for (size_t i = 0; i < units.size(); ++i) {
      if (units[i].matcher.matchesName(func_name, is_intrinsic)) {
        indices.push_back(i);
      }
    }
    it = cache.emplace(std::move(key), std::move(indices)).first;
  }
  out.reserve(it->second.size());
  for (size_t idx : it->second) {
    out.push_back(&units[idx]);
  }
  return out;
}
} // namespace

std::vector<const ProcedureUnit *>
TaintConfig::matchSources(const llvm::Function *func) const {
  if (!func)
    return {};
  return matchUnitsCached(source_procedures_, source_cache_,
                          func->getName().str(), func->isIntrinsic());
}

std::vector<const ProcedureUnit *>
TaintConfig::matchSourcesByName(const std::string &func_name) const {
  return matchUnitsCached(source_procedures_, source_cache_, func_name, false);
}

std::vector<const ProcedureUnit *>
TaintConfig::matchSourceBlocks(const llvm::Function *func) const {
  if (!func)
    return {};
  return matchUnitsCached(source_blocks_, source_block_cache_,
                          func->getName().str(), func->isIntrinsic());
}

std::vector<const ProcedureUnit *>
TaintConfig::matchSinks(const llvm::Function *func) const {
  if (!func)
    return {};
  return matchUnitsCached(sink_procedures_, sink_cache_, func->getName().str(),
                          func->isIntrinsic());
}

std::vector<const ProcedureUnit *>
TaintConfig::matchSinksByName(const std::string &func_name) const {
  return matchUnitsCached(sink_procedures_, sink_cache_, func_name, false);
}

std::vector<const ProcedureUnit *>
TaintConfig::matchSanitizers(const llvm::Function *func) const {
  if (!func)
    return {};
  return matchUnitsCached(sanitizer_procedures_, sanitizer_cache_,
                          func->getName().str(), func->isIntrinsic());
}

std::vector<const ProcedureUnit *>
TaintConfig::matchSanitizersByName(const std::string &func_name) const {
  return matchUnitsCached(sanitizer_procedures_, sanitizer_cache_, func_name,
                          false);
}

std::vector<const ProcedureUnit *>
TaintConfig::matchPropagators(const llvm::Function *func) const {
  if (!func)
    return {};
  return matchUnitsCached(propagator_procedures_, propagator_cache_,
                          func->getName().str(), func->isIntrinsic());
}

std::vector<const ProcedureUnit *>
TaintConfig::matchPropagatorsByName(const std::string &func_name) const {
  return matchUnitsCached(propagator_procedures_, propagator_cache_, func_name,
                          false);
}

std::vector<TaintKind> TaintConfig::isSource(const llvm::Function *func) const {
  std::vector<TaintKind> kinds;
  for (const ProcedureUnit *unit : matchSources(func)) {
    kinds.insert(kinds.end(), unit->kinds.begin(), unit->kinds.end());
  }
  return kinds;
}

bool TaintConfig::isSink(const llvm::Function *func,
                         std::vector<TaintKind> &sink_kinds) const {
  sink_kinds.clear();
  auto matched = matchSinks(func);
  for (const ProcedureUnit *unit : matched) {
    sink_kinds.insert(sink_kinds.end(), unit->kinds.begin(), unit->kinds.end());
  }
  return !matched.empty();
}

std::vector<TaintKind>
TaintConfig::isSanitizer(const llvm::Function *func) const {
  std::vector<TaintKind> kinds;
  for (const ProcedureUnit *unit : matchSanitizers(func)) {
    kinds.insert(kinds.end(), unit->kinds.begin(), unit->kinds.end());
  }
  return kinds;
}

bool TaintConfig::isPropagator(const llvm::Function *func) const {
  return !matchPropagators(func).empty();
}

const std::vector<SinkPolicy> &
TaintConfig::getSinkPolicies(const TaintKind &sink_kind) const {
  auto it = sink_policies_.find(sink_kind);
  if (it != sink_policies_.end()) {
    return it->second;
  }
  static const std::vector<SinkPolicy> empty;
  return empty;
}

std::vector<TaintKind>
TaintConfig::getAllocationSourceKinds(const std::string &class_name) const {
  auto it = allocation_sources_.find(class_name);
  if (it != allocation_sources_.end()) {
    return it->second;
  }
  return {};
}

std::vector<TaintKind>
TaintConfig::matchAllocationSourceForName(const std::string &name) const {
  if (name.empty() || allocation_sources_.empty())
    return {};
  auto exact = allocation_sources_.find(name);
  if (exact != allocation_sources_.end()) {
    return exact->second;
  }
  const std::string demangled = llvm::demangle(name);
  const std::string demangled_no_args =
      demangled.substr(0, demangled.find('('));
  for (const auto &kv : allocation_sources_) {
    const std::string &cls = kv.first;
    if (cls.empty())
      continue;
    if (name == "struct." + cls || name == "class." + cls ||
        name.find(cls) != std::string::npos ||
        demangled_no_args.find(cls) != std::string::npos) {
      return kv.second;
    }
  }
  return {};
}

TaintConfig *TaintConfig::default_config_ = nullptr;

void TaintConfig::initializeDefault() {
  if (default_config_)
    return;

  default_config_ = new TaintConfig();

  // --- Unified default taint sources (with precise ProcedureTarget) ---
  default_config_->addSourceProcedure(ProcedureUnit(
      ProcedureMatcher(ProcedureMatcher::Type::ProcedureName, "read"),
      {TaintKind::UserInput()}, ProcedureTarget::ArgumentPositions, {1}));
  default_config_->addSourceProcedure(ProcedureUnit(
      ProcedureMatcher(ProcedureMatcher::Type::ProcedureNameRegex,
                       "^(recv|recvfrom|recvmsg)$"),
      {TaintKind::Network()}, ProcedureTarget::ArgumentPositions, {1}));
  default_config_->addSourceProcedure(ProcedureUnit(
      ProcedureMatcher(ProcedureMatcher::Type::ProcedureNameRegex,
                       "^(gets|fgets|fread)$"),
      {TaintKind::UserInput()}, ProcedureTarget::ArgumentPositions, {0}));
  default_config_->addSourceProcedure(ProcedureUnit(
      ProcedureMatcher(ProcedureMatcher::Type::ProcedureName, "getcwd"),
      {TaintKind::FileSystem()}, ProcedureTarget::ArgumentPositions, {0}));
  default_config_->addSourceProcedure(ProcedureUnit(
      ProcedureMatcher(ProcedureMatcher::Type::ProcedureName, "getenv"),
      {TaintKind::Environment()}, ProcedureTarget::ReturnValue));
  default_config_->addSourceProcedure(ProcedureUnit(
      ProcedureMatcher(ProcedureMatcher::Type::ProcedureName, "scanf"),
      {TaintKind::UserInput()}, ProcedureTarget::AllArgumentsButPositions,
      {0}));
  default_config_->addSourceProcedure(ProcedureUnit(
      ProcedureMatcher(ProcedureMatcher::Type::ProcedureName, "fscanf"),
      {TaintKind::UserInput()}, ProcedureTarget::AllArgumentsButPositions,
      {0, 1}));

  // --- Unified default taint sinks ---
  default_config_->addSinkProcedure(ProcedureUnit(
      ProcedureMatcher(ProcedureMatcher::Type::ProcedureNameRegex,
                       "^(system|exec|execl|execlp|execle|execv|execvp|execve|"
                       "popen|eval)$"),
      {TaintKind::UserInput()}, ProcedureTarget::AllArguments));
  default_config_->addSinkProcedure(ProcedureUnit(
      ProcedureMatcher(ProcedureMatcher::Type::ProcedureNameRegex,
                       "^(printf|fprintf|sprintf|snprintf)$"),
      {TaintKind::UserInput()}, ProcedureTarget::AllArguments));

  // --- Unified default taint sanitizers ---
  default_config_->addSanitizerProcedure(ProcedureUnit(
      ProcedureMatcher(ProcedureMatcher::Type::ProcedureNameRegex,
                       "^(strlen|strcmp|strncmp|atoi|atol|atoll|strtol|"
                       "strtoul)$"),
      {TaintKind::Unknown()}, ProcedureTarget::AllArguments));

  // --- Unified default taint propagators ---
  default_config_->addPropagatorProcedure(ProcedureUnit(
      ProcedureMatcher(ProcedureMatcher::Type::ProcedureNameRegex,
                       "^(strcpy|strncpy|strcat|strncat|memcpy|memmove|strdup|"
                       "sprintf|snprintf)$"),
      {TaintKind::UserInput()}, ProcedureTarget::ReturnValue));

  // --- Default sink policies ---
  SinkPolicy command_injection(1, "Command injection / tainted sink");
  command_injection.source_kinds = {TaintKind::UserInput(),
                                    TaintKind::Network(),
                                    TaintKind::Environment(),
                                    TaintKind::FileSystem(),
                                    TaintKind::Sensitive()};
  command_injection.sanitizer_kinds = {TaintKind::Unknown()};
  default_config_->addSinkPolicy(TaintKind::UserInput(), command_injection);
}

const TaintConfig &TaintConfig::getDefault() {
  if (!default_config_) {
    initializeDefault();
  }
  return *default_config_;
}

llvm::Expected<TaintConfig>
TaintConfig::loadFromJson(llvm::StringRef json_text, bool extend_default) {
  auto parsed = llvm::json::parse(json_text);
  if (!parsed) {
    return parsed.takeError();
  }
  const auto *root = parsed->getAsObject();
  if (!root) {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "taint config JSON must be an object");
  }

  TaintConfig cfg = extend_default ? getDefault() : TaintConfig();

  if (const auto *sources = root->getArray("sources")) {
    for (const auto &v : *sources) {
      const auto *obj = v.getAsObject();
      if (!obj)
        continue;
      auto unit = parseProcedureUnit(*obj, ProcedureTarget::ReturnValue,
                                     {TaintKind::UserInput()});
      if (!unit)
        return unit.takeError();
      cfg.addSourceProcedure(*unit);
    }
  }
  if (const auto *sinks = root->getArray("sinks")) {
    for (const auto &v : *sinks) {
      const auto *obj = v.getAsObject();
      if (!obj)
        continue;
      auto unit = parseProcedureUnit(*obj, ProcedureTarget::AllArguments,
                                     {TaintKind::UserInput()});
      if (!unit)
        return unit.takeError();
      cfg.addSinkProcedure(*unit);
    }
  }
  if (const auto *sanitizers = root->getArray("sanitizers")) {
    for (const auto &v : *sanitizers) {
      const auto *obj = v.getAsObject();
      if (!obj)
        continue;
      auto unit = parseProcedureUnit(*obj, ProcedureTarget::AllArguments,
                                     {TaintKind::Unknown()});
      if (!unit)
        return unit.takeError();
      cfg.addSanitizerProcedure(*unit);
    }
  }
  if (const auto *props = root->getArray("propagators")) {
    for (const auto &v : *props) {
      const auto *obj = v.getAsObject();
      if (!obj)
        continue;
      auto unit = parseProcedureUnit(*obj, ProcedureTarget::ReturnValue,
                                     {TaintKind::UserInput()});
      if (!unit)
        return unit.takeError();
      cfg.addPropagatorProcedure(*unit);
    }
  }
  if (const auto *alloc_srcs = root->getArray("allocation_sources")) {
    for (const auto &v : *alloc_srcs) {
      const auto *obj = v.getAsObject();
      if (!obj)
        continue;
      auto cls = obj->getString("class_name");
      if (!cls || cls->empty())
        continue;
      const bool df_only = obj->getBoolean("data_flow_only").getValueOr(false);
      std::vector<TaintKind> kinds;
      if (const auto *karr = obj->getArray("kinds")) {
        for (const auto &kv : *karr) {
          if (auto s = kv.getAsString()) {
            kinds.emplace_back(s->str(), df_only);
          }
        }
      }
      if (kinds.empty()) {
        kinds.emplace_back(TaintKind::UserInput().getName(), df_only);
      }
      cfg.addAllocationSource(cls->str(), kinds);
    }
  }
  if (const auto *src_blocks = root->getArray("source_blocks")) {
    for (const auto &v : *src_blocks) {
      const auto *obj = v.getAsObject();
      if (!obj)
        continue;
      auto unit = parseProcedureUnit(*obj, ProcedureTarget::ReturnValue,
                                     {TaintKind::UserInput()});
      if (!unit)
        return unit.takeError();
      cfg.addSourceBlock(*unit);
    }
  }
  if (const auto *policies = root->getArray("policies")) {
    int next_id = 100;
    for (const auto &v : *policies) {
      const auto *obj = v.getAsObject();
      if (!obj)
        continue;
      std::string sink_kind_name =
          obj->getString("sink_kind").getValueOr("UserInput").str();
      std::string desc =
          obj->getString("description").getValueOr("Custom taint policy").str();
      SinkPolicy pol(next_id++, desc);
      if (const auto *srcs = obj->getArray("source_kinds")) {
        for (const auto &sv : *srcs) {
          if (auto s = sv.getAsString())
            pol.source_kinds.emplace_back(s->str());
        }
      }
      if (const auto *sans = obj->getArray("sanitizer_kinds")) {
        for (const auto &sv : *sans) {
          if (auto s = sv.getAsString())
            pol.sanitizer_kinds.emplace_back(s->str());
        }
      }
      cfg.addSinkPolicy(TaintKind(sink_kind_name), pol);
    }
  }

  return cfg;
}

} // namespace pulse
