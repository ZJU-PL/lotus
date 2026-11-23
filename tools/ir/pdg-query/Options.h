#pragma once

#include <string>
#include <vector>

namespace lotus::pdg_query {

/// Parsed tool configuration; analysis and output do not access LLVM CLI
/// globals.
struct Options {
  std::string inputFilename = "-";
  std::string queryString;
  std::string queryFile;
  bool interactive = false;
  bool verbose = false;
  bool explain = false;
  bool buildPDG = true;
  int resultLimit = 100;
  int unboundedMaxHops = 5;
  std::string format = "text";
  std::vector<std::string> queryParams;
  std::string propertyFile;
  std::string sliceDirection = "backward";
  bool dumpSlice = false;
  std::string analysisName;
  std::vector<std::string> ruleIds;
  std::vector<unsigned> cweIds;
  bool listCwes = false;
  bool listRules = false;
  unsigned long long taintStepLimit = 200000;
  unsigned long long lifetimeStateLimit = 4096;
  unsigned long long protocolStateLimit = 4096;
  std::string criteriaQuery;
  std::string targetQuery;
  std::string baselineQuery;
  std::string scopeFunction;
  std::string scopeQuery;
  std::string edgePreset = "all";
  bool contextSensitive = false;
  bool thin = false;
  std::string summaryKind = "all";
  std::string resourceKind = "all";
  bool showVersion = false;
  bool schema = false;
};

Options parseOptions(int argc, char **argv);

} // namespace lotus::pdg_query
