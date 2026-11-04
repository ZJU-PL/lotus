#include "Annotation/Taint/TaintConfigParser.h"

#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <algorithm>
#include <limits>

using namespace llvm;

bool TaintConfig::loadJSONFile(const std::string &path, std::string &error) {
  auto buffer = MemoryBuffer::getFile(path);
  if (!buffer) {
    error = buffer.getError().message();
    return false;
  }
  return loadJSONString((*buffer)->getBuffer().str(), error);
}

bool TaintConfig::loadJSONString(const std::string &content, std::string &error,
                                bool sortArgumentIndices) {
  auto parsed = json::parse(content);
  if (!parsed) {
    error = toString(parsed.takeError());
    return false;
  }
  auto *root = parsed->getAsObject();
  if (!root) {
    error = "Taint contract must be a JSON object";
    return false;
  }
  if (auto *names = root->getArray("taint-return-as-source")) {
    for (const auto &entry : *names) {
      auto name = entry.getAsString();
      if (!name)
        continue;
      sources.insert(name->str());
      TaintSpec model;
      model.location = TaintSpec::RET;
      function_specs[name->str()].source_specs.push_back(model);
    }
  }
  for (StringRef key : {StringRef("taint-argument-as-source"),
                       StringRef("taint-sink")}) {
    auto *functions = root->getObject(key);
    if (!functions)
      continue;
    bool source = key == "taint-argument-as-source";
    for (auto &entry : *functions) {
      auto *array = entry.second.getAsArray();
      if (!array)
        continue;
      auto name = entry.first.str();
      (source ? sources : sinks).insert(name);
      if (source)
        argument_source_functions.insert(name);
      auto &record = function_specs[name];
      auto &models = source ? record.source_specs : record.sink_specs;
      std::vector<int> indices;
      for (const auto &value : *array)
        if (auto index = value.getAsInteger())
          if (*index > std::numeric_limits<int>::min() &&
              *index <= std::numeric_limits<int>::max())
            indices.push_back(static_cast<int>(*index));
      if (sortArgumentIndices) {
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
      }
      for (int index : indices) {
        TaintSpec model;
        model.location = index < 0 ? TaintSpec::AFTER_ARG : TaintSpec::ARG;
        model.arg_index = index < 0 ? -index : index;
        models.push_back(model);
      }
    }
  }
  if (auto *rows = root->getArray("taint-exception")) {
    for (const auto &row : *rows) {
      auto *entry = row.getAsArray();
      if (!entry || entry->size() != 4 || !(*entry)[0].getAsString() ||
          !(*entry)[1].getAsInteger() || !(*entry)[2].getAsString() ||
          !(*entry)[3].getAsInteger())
        continue;
      exceptions[{(*entry)[0].getAsString()->str(),
                  static_cast<int>(*(*entry)[1].getAsInteger())}]
          .insert({(*entry)[2].getAsString()->str(),
                   static_cast<int>(*(*entry)[3].getAsInteger())});
    }
  }
  error.clear();
  return true;
}
