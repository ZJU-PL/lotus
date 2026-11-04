#include "Annotation/APISpec.h"

#include <algorithm>
#include <limits>
#include <set>

#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>

using namespace llvm;

namespace lotus {

bool APISpec::loadJSONFile(const std::string &path, std::string &error) {
  auto buffer = MemoryBuffer::getFile(path);
  if (!buffer) {
    error = buffer.getError().message();
    return false;
  }
  return loadJSONString((*buffer)->getBuffer(), error);
}

bool APISpec::loadJSONString(StringRef content, std::string &error) {
  auto parsed = json::parse(content);
  if (!parsed) {
    error = toString(parsed.takeError());
    return false;
  }
  auto *root = parsed->getAsObject();
  if (!root) {
    error = "API contract must be a JSON object";
    return false;
  }
  auto function = [this](StringRef name) -> FunctionSpec & {
    auto &spec = nameToSpec[name.str()];
    spec.functionName = name.str();
    return spec;
  };
  auto appendNames = [](const json::Value &value,
                        std::vector<std::string> &out) {
    if (auto *array = value.getAsArray())
      for (const auto &item : *array)
        if (auto name = item.getAsString())
          out.push_back(name->str());
  };
  if (auto *functions = root->getObject("functions")) {
    for (auto &entry : *functions) {
      auto *record = entry.second.getAsObject();
      if (!record)
        continue;
      auto &spec = function(entry.first.str());
      auto flag = [&](StringRef key, bool &destination) {
        if (auto value = record->getBoolean(key))
          destination = *value;
      };
      flag("malloc", spec.isMallocLike);
      flag("new", spec.isNewLike);
      flag("free", spec.isFreeLike);
      flag("delete", spec.isDeleteLike);
      flag("reallocate", spec.isReallocator);
      flag("ignore", spec.isIgnored);
      flag("resource-acquire", spec.acquiresResource);
      flag("resource-release", spec.releasesResource);
      flag("may-return-negative", spec.mayReturnNegative);
      if (auto nullable = record->getBoolean("may-return-null"))
        spec.mayReturnNull = *nullable;
      if (auto *arguments = record->getArray("dereferenced-arguments")) {
        for (const auto &argument : *arguments)
          if (auto index = argument.getAsInteger())
            if (*index >= 0 && *index <= std::numeric_limits<unsigned>::max())
              spec.dereferencedArguments.push_back(
                  static_cast<unsigned>(*index));
        std::sort(spec.dereferencedArguments.begin(),
                  spec.dereferencedArguments.end());
        spec.dereferencedArguments.erase(
            std::unique(spec.dereferencedArguments.begin(),
                        spec.dereferencedArguments.end()),
            spec.dereferencedArguments.end());
      }
      spec.isAllocator |= spec.isMallocLike || spec.isNewLike;
      spec.isDeallocator |= spec.isFreeLike || spec.isDeleteLike;
      if (auto pattern = record->getInteger("buffer-access-pattern"))
        spec.bufferAccessPattern = static_cast<int>(*pattern);
      if (auto *sizes = record->getArray("size-arguments")) {
        for (const auto &size : *sizes)
          if (auto index = size.getAsInteger())
            spec.allocationSizeArguments.push_back(static_cast<int>(*index));
      }
      if (auto *value = record->get("deallocators"))
        appendNames(*value, spec.matchingDeallocators);
      if (auto *value = record->get("pointer-resource-releases"))
        appendNames(*value, spec.pointerResourceReleases);
      if (auto *value = record->get("integer-resource-releases"))
        appendNames(*value, spec.integerResourceReleases);
    }
  }
  // Accept the source engine's configuration overlays using the same store.
  for (auto &category : *root) {
    std::string key_storage = category.first.str();
    StringRef key(key_storage);
    if (auto *names = category.second.getAsArray()) {
      if (key == "mem-alloc" || key == "mem-free" || key == "file-open" ||
          key == "file-close") {
        for (const auto &item : *names) {
          auto name = item.getAsString();
          if (!name)
            continue;
          auto &spec = function(*name);
          if (key == "mem-alloc")
            spec.isAllocator = spec.isMallocLike = true;
          if (key == "mem-free")
            spec.isDeallocator = spec.isFreeLike = true;
          if (key == "file-open")
            spec.acquiresResource = true;
          if (key == "file-close")
            spec.releasesResource = true;
        }
      } else if (key == "mem-alloc-free" || key == "file-open-close") {
        for (const auto &item : *names) {
          auto *pair = item.getAsArray();
          if (!pair || pair->size() != 2 || !(*pair)[0].getAsString() ||
              !(*pair)[1].getAsString())
            continue;
          auto &spec = function(*(*pair)[0].getAsString());
          auto &releases = key == "mem-alloc-free"
                               ? spec.matchingDeallocators
                               : spec.pointerResourceReleases;
          releases.push_back((*pair)[1].getAsString()->str());
        }
      }
    } else if (key == "bufferoverflow-argument-as-sink") {
      if (auto *patterns = category.second.getAsObject())
        for (auto &entry : *patterns) {
          auto *array = entry.second.getAsArray();
          std::set<int> values;
          if (array)
            for (const auto &item : *array)
              if (auto number = item.getAsInteger())
                values.insert(static_cast<int>(*number));
          auto &spec = function(entry.first.str());
          if (spec.bufferAccessPattern < 0 && !values.empty())
            spec.bufferAccessPattern = *values.begin();
        }
    }
  }
  error.clear();
  return true;
}

} // namespace lotus
