#include "Checker/GSAF/API/Models.h"

#include "Checker/GSAF/Support/Options.h"
#include "DefaultModels.h"

#include <algorithm>

#include <llvm/IR/GlobalAlias.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace lotus::gsaf {

char GSAFModels::ID = 0;
static RegisterPass<GSAFModels>
    registration("gsaf-models", "GSAF annotation profile", false, true);
GSAFModels::GSAFModels() : ModulePass(ID) {}

const FunctionSpec *GSAFModels::builtinSpec(Function *function) {
  static APISpec profile = [] {
    APISpec spec;
    std::string error;
    if (!spec.loadJSONString(defaults::API, error))
      report_fatal_error(Twine("Invalid built-in profile: ") + error);
    return spec;
  }();
  if (!function)
    return nullptr;
  StringRef name = function->getName();
  if (function->isIntrinsic()) {
    size_t first = name.find('.');
    size_t second = name.find('.', first + 1);
    name = name.take_front(second);
  }
  return profile.get(name.str());
}

bool GSAFModels::runOnModule(llvm::Module &) {
  API = APISpec{};
  Taint = TaintConfig{};
  SourceArguments.clear();
  SinkArguments.clear();
  std::string error;
  if (!API.loadJSONString(defaults::API, error) ||
      !Taint.loadJSONString(defaults::Taint, error, false))
    report_fatal_error(Twine("Invalid built-in GSAF model: ") + error);
  for (const auto *path :
       {&GSAFOptions::MemorySpecPath, &GSAFOptions::IOSpecPath,
        &GSAFOptions::BufferSpecPath})
    if (!path->empty() && !API.loadJSONFile(*path, error))
      errs() << "GSAF model: " << *path << ": " << error << '\n';
  if (!GSAFOptions::TaintSpecPath.empty() &&
      !Taint.loadJSONFile(GSAFOptions::TaintSpecPath, error))
    errs() << "GSAF model: " << GSAFOptions::TaintSpecPath << ": " << error
           << '\n';

  for (const auto &name : Taint.argument_source_functions)
    SourceArguments[name];
  for (const auto &name : Taint.sinks)
    SinkArguments[name];
  for (const auto &entry : Taint.function_specs) {
    auto append = [](const std::vector<::TaintSpec> &models,
                     std::vector<int> &out) {
      for (const auto &model : models) {
        if (model.location == ::TaintSpec::ARG)
          out.push_back(model.arg_index);
        else if (model.location == ::TaintSpec::AFTER_ARG)
          out.push_back(-model.arg_index);
      }
    };
    if (SourceArguments.count(entry.first))
      append(entry.second.source_specs, SourceArguments[entry.first]);
    if (SinkArguments.count(entry.first))
      append(entry.second.sink_specs, SinkArguments[entry.first]);
  }
  return false;
}

const FunctionSpec *GSAFModels::spec(const Function *function) const {
  return function ? API.get(function->getName().str()) : nullptr;
}
Function *GSAFModels::callee(Value *value) {
  auto *call = dyn_cast_or_null<CallBase>(value);
  return call ? call->getCalledFunction() : nullptr;
}

bool GSAFModels::is_malloc_function(Function *f) const {
  auto *s = spec(f);
  return s && s->isMallocLike;
}
bool GSAFModels::is_new_function(Function *f) const {
  auto *s = spec(f);
  return s && s->isNewLike;
}
bool GSAFModels::is_free_function(Function *f) const {
  auto *s = spec(f);
  return s && s->isFreeLike;
}
bool GSAFModels::is_delete_function(Function *f) const {
  auto *s = spec(f);
  return s && s->isDeleteLike;
}
bool GSAFModels::isAllocFunc(Function *f) const {
  auto *s = spec(f);
  return s && s->isAllocator;
}
bool GSAFModels::isFreeFunc(Function *f) const {
  auto *s = spec(f);
  return s && s->isDeallocator;
}
bool GSAFModels::is_pure_lib_function(Function *f) const {
  return isPureLib(f);
}
bool GSAFModels::isPureLib(Function *f) const {
  auto *s = spec(f);
  return !f || (s && s->isIgnored);
}
bool GSAFModels::isHeapAllocSite(Value *v) const {
  return isAllocFunc(callee(v));
}
bool GSAFModels::isHeapFreeSite(Value *v) const {
  return isFreeFunc(callee(v));
}
bool GSAFModels::isHeapReallocSite(Value *v) const {
  auto *s = spec(callee(v));
  return s && s->isReallocator;
}
bool GSAFModels::isStackAllocSite(Value *v) const {
  if (isa_and_nonnull<AllocaInst>(v))
    return true;
  auto *f = callee(v);
  return f && f->getName().startswith("llvm.frameallocate");
}
bool GSAFModels::isGlobalMemory(Value *v) const {
  if (auto *alias = dyn_cast_or_null<GlobalAlias>(v))
    v = const_cast<GlobalObject *>(alias->getAliaseeObject());
  return isa_and_nonnull<GlobalVariable>(v);
}
bool GSAFModels::isConcreteMemory(Value *v) const {
  return isStackAllocSite(v) || isGlobalMemory(v) || isHeapAllocSite(v);
}
std::vector<int> GSAFModels::getHeapAllocSize(Value *v) const {
  auto *s = spec(callee(v));
  return s ? s->allocationSizeArguments : std::vector<int>{};
}

int GSAFModels::getAllMemoryAllocs(std::vector<std::string> &names) const {
  for (bool cpp : {false, true}) {
    std::vector<std::string> group;
    for (const auto &entry : API.all())
      if (cpp ? entry.second.isNewLike : entry.second.isMallocLike)
        group.push_back(entry.first);
    std::sort(group.begin(), group.end());
    names.insert(names.end(), group.begin(), group.end());
  }
  return names.size();
}
int GSAFModels::getAllMemoryFrees(std::vector<std::string> &names) const {
  for (bool cpp : {false, true}) {
    std::vector<std::string> group;
    for (const auto &entry : API.all())
      if (cpp ? entry.second.isDeleteLike : entry.second.isFreeLike)
        group.push_back(entry.first);
    std::sort(group.begin(), group.end());
    names.insert(names.end(), group.begin(), group.end());
  }
  return names.size();
}
int GSAFModels::getMatchedFreesForMalloc(
    const std::string &name, std::vector<std::string> &frees) const {
  if (auto *s = API.get(name))
    frees.insert(frees.end(), s->matchingDeallocators.begin(),
                 s->matchingDeallocators.end());
  return frees.size();
}
int GSAFModels::getAllMallocFreePairs(
    std::map<std::string, std::set<std::string>> &pairs) const {
  for (const auto &entry : API.all())
    if (!entry.second.matchingDeallocators.empty())
      pairs[entry.first].insert(entry.second.matchingDeallocators.begin(),
                                entry.second.matchingDeallocators.end());
  return pairs.size();
}
bool GSAFModels::isFileOpenSite(Value *v) const {
  auto *s = spec(callee(v));
  return s && s->acquiresResource;
}
bool GSAFModels::isFileCloseSite(Value *v) const {
  auto *s = spec(callee(v));
  return s && s->releasesResource;
}
int GSAFModels::getAllFilePtrOpenClosePairs(
    std::map<std::string, std::set<std::string>> &pairs) const {
  for (const auto &entry : API.all())
    if (!entry.second.pointerResourceReleases.empty())
      pairs[entry.first].insert(entry.second.pointerResourceReleases.begin(),
                                entry.second.pointerResourceReleases.end());
  return pairs.size();
}
bool GSAFModels::isBufferAccessFunc(Function *f) const {
  return getBufferAccessPattern(f) >= 0;
}
int GSAFModels::getBufferAccessPattern(Function *f) const {
  auto *s = spec(f);
  return s ? s->bufferAccessPattern : -1;
}
bool GSAFModels::isPotentialSrcFunction(Function *f) const {
  auto *s = spec(f);
  return s && s->mayReturnNegative;
}

bool GSAFModels::isFunctionRetAsSource(const Function *f) const {
  auto *s = f ? Taint.get_function_config(f->getName().str()) : nullptr;
  return s && std::any_of(s->source_specs.begin(), s->source_specs.end(),
                          [](const ::TaintSpec &model) {
                            return model.location == ::TaintSpec::RET;
                          });
}
bool GSAFModels::isFunctionArgAsSource(const Function *f) const {
  return f && SourceArguments.count(f->getName().str());
}
bool GSAFModels::isFunctionAsSink(const Function *f) const {
  return f && Taint.is_sink(f->getName().str());
}
const std::vector<int> *GSAFModels::getTaintSourceArguments(Function *f) const {
  if (!f)
    return nullptr;
  auto found = SourceArguments.find(f->getName().str());
  return found == SourceArguments.end() ? nullptr : &found->second;
}
const std::vector<int> *GSAFModels::getTaintSinkArguments(Function *f) const {
  if (!f)
    return nullptr;
  auto found = SinkArguments.find(f->getName().str());
  return found == SinkArguments.end() ? nullptr : &found->second;
}
bool GSAFModels::isException(const std::pair<Function *, int> &source,
                             const std::pair<Function *, int> &sink) const {
  if (!source.first || !sink.first)
    return false;
  auto found =
      Taint.exceptions.find({source.first->getName().str(), source.second});
  return found != Taint.exceptions.end() &&
         found->second.count({sink.first->getName().str(), sink.second});
}

} // namespace lotus::gsaf
