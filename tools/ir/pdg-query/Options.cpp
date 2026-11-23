#include "Options.h"

#include "llvm/Support/CommandLine.h"

using namespace llvm;

namespace lotus::pdg_query {
namespace {

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::init("-"),
                                          cl::value_desc("filename"));

static cl::opt<std::string>
    QueryString("query", cl::desc("Execute a single Cypher query"),
                cl::value_desc("cypher_query"));
static cl::alias QueryAlias("q", cl::aliasopt(QueryString));

static cl::opt<std::string>
    QueryFile("query-file", cl::desc("Execute Cypher queries from file"),
              cl::value_desc("filename"));
static cl::alias QueryFileAlias("f", cl::aliasopt(QueryFile));

static cl::opt<bool> Interactive("interactive",
                                 cl::desc("Run in interactive mode"));
static cl::alias InteractiveAlias("i", cl::aliasopt(Interactive));

static cl::opt<bool> Verbose("verbose", cl::desc("Enable verbose output"));
static cl::alias VerboseAlias("v", cl::aliasopt(Verbose));

static cl::opt<bool> Explain("explain", cl::desc("Show query execution plan"));
static cl::alias ExplainAlias("e", cl::aliasopt(Explain));

static cl::opt<bool>
    BuildPDG("build-pdg",
             cl::desc("Build full PDG (adds data/control/param edges)"),
             cl::init(true));

static cl::opt<int>
    ResultLimit("limit",
                cl::desc("Maximum number of results to return (default: 100)"),
                cl::init(100));

static cl::opt<int> UnboundedMaxHops(
    "max-unbounded-hops",
    cl::desc("Default cap for unbounded traversals (e.g. *..), default: 5"),
    cl::init(5));

static cl::opt<std::string> Format("format",
                                   cl::desc("Output format: text, json, dot"),
                                   cl::init("text"));

static cl::list<std::string> QueryParams(
    "param",
    cl::desc("Query parameter key=value (repeatable); referenced as $key"),
    cl::ZeroOrMore, cl::value_desc("key=value"));

static cl::opt<std::string>
    PropertyFile("property-file",
                 cl::desc("Resolve criteria from a Symbiotic-style .prp file"),
                 cl::value_desc("filename"), cl::init(""));

static cl::opt<std::string>
    SliceDirection("direction",
                   cl::desc("Property slice direction: backward|forward"),
                   cl::init("backward"));

static cl::opt<bool> DumpSlice("dump-slice",
                               cl::desc("Dump selected property slice nodes"),
                               cl::init(false));

static cl::opt<std::string>
    AnalysisName("analysis",
                 cl::desc("Run PDG analysis: slice-forward, slice-backward, "
                          "chop, shortest-path, reaching-defs, live, dead, "
                          "control-region, controllers, diff, summary, "
                          "impact, resource-flow, rules"),
                 cl::init(""));

static cl::list<std::string> RuleIds(
    "rule",
    cl::desc("Select a semantic rule by CodeQL ID (repeatable; default: all)"),
    cl::ZeroOrMore);

static cl::list<unsigned>
    CweIds("cwe",
           cl::desc("Select rules tagged with a CWE number (repeatable or "
                    "comma-separated)"),
           cl::ZeroOrMore, cl::CommaSeparated);

static cl::opt<bool> ListCwes(
    "list-cwes",
    cl::desc("List implemented CWE-to-rule mappings; not full CWE coverage"),
    cl::init(false));

static cl::opt<bool>
    ListRules("list-rules",
              cl::desc("List supported semantic rules and coverage, then exit"),
              cl::init(false));

static cl::opt<unsigned long long>
    TaintStepLimit("taint-step-limit",
                   cl::desc("PDG taint step budget for rules (0: unbounded)"),
                   cl::init(200000));

static cl::opt<unsigned long long> LifetimeStateLimit(
    "lifetime-state-limit",
    cl::desc(
        "Lifetime exploration budget per object (default: 4096, 0: unbounded)"),
    cl::init(4096));

static cl::opt<unsigned long long> ProtocolStateLimit(
    "protocol-state-limit",
    cl::desc("Maximum protocol states per worker/check/cycle search (0 disables budget)"),
    cl::init(4096));

static cl::opt<std::string>
    CriteriaQuery("criteria-query",
                  cl::desc("Cypher query selecting analysis criteria"),
                  cl::init(""));

static cl::opt<std::string>
    TargetQuery("target-query",
                cl::desc("Cypher query selecting analysis targets"),
                cl::init(""));

static cl::opt<std::string>
    BaselineQuery("baseline-query",
                  cl::desc("Cypher query selecting baseline criteria for "
                           "changed-only impact"),
                  cl::init(""));

static cl::opt<std::string>
    ScopeFunction("scope-function",
                  cl::desc("Restrict analysis scope to one LLVM function"),
                  cl::init(""));

static cl::opt<std::string>
    ScopeQuery("scope-query", cl::desc("Cypher query selecting analysis scope"),
               cl::init(""));

static cl::opt<std::string>
    EdgePreset("edge-preset",
               cl::desc("Edge preset: all, data, control, parameter, "
                        "interprocedural, value-flow, transform-legality"),
               cl::init("all"));

static cl::opt<bool>
    ContextSensitive("context-sensitive",
                     cl::desc("Use call/return matching during traversal"),
                     cl::init(false));

static cl::opt<bool> Thin("thin", cl::desc("Use thin slicing semantics"),
                          cl::init(false));

static cl::opt<std::string>
    SummaryKindFlag("summary-kind",
                    cl::desc("Summary bucket: all, input-to-return, "
                             "input-to-global-write, input-to-callsite, "
                             "global-readers, global-writers, "
                             "control-predicates, reachable-calls, "
                             "resource-kinds"),
                    cl::init("all"));

static cl::opt<std::string> ResourceKindFlag(
    "resource-kind",
    cl::desc("Resource family: all, heap, file, fd, dir, lock"),
    cl::init("all"));

static cl::opt<bool> ShowVersion("show-version",
                                 cl::desc("Show version information"));

static cl::opt<bool>
    Schema("schema",
           cl::desc("Print PDG schema (node labels, edge types, properties, "
                    "edge presets) as JSON and exit"),
           cl::init(false));

} // namespace

Options parseOptions(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv, "PDG Query Tool\n");
  Options config;
  config.inputFilename = InputFilename.getValue();
  config.queryString = QueryString.getValue();
  config.queryFile = QueryFile.getValue();
  config.interactive = Interactive.getValue();
  config.verbose = Verbose.getValue();
  config.explain = Explain.getValue();
  config.buildPDG = BuildPDG.getValue();
  config.resultLimit = ResultLimit.getValue();
  config.unboundedMaxHops = UnboundedMaxHops.getValue();
  config.format = Format.getValue();
  config.queryParams.assign(QueryParams.begin(), QueryParams.end());
  config.propertyFile = PropertyFile.getValue();
  config.sliceDirection = SliceDirection.getValue();
  config.dumpSlice = DumpSlice.getValue();
  config.analysisName = AnalysisName.getValue();
  config.ruleIds.assign(RuleIds.begin(), RuleIds.end());
  config.cweIds.assign(CweIds.begin(), CweIds.end());
  config.listCwes = ListCwes.getValue();
  config.listRules = ListRules.getValue();
  config.taintStepLimit = TaintStepLimit.getValue();
  config.lifetimeStateLimit = LifetimeStateLimit.getValue();
  config.protocolStateLimit = ProtocolStateLimit.getValue();
  config.criteriaQuery = CriteriaQuery.getValue();
  config.targetQuery = TargetQuery.getValue();
  config.baselineQuery = BaselineQuery.getValue();
  config.scopeFunction = ScopeFunction.getValue();
  config.scopeQuery = ScopeQuery.getValue();
  config.edgePreset = EdgePreset.getValue();
  config.contextSensitive = ContextSensitive.getValue();
  config.thin = Thin.getValue();
  config.summaryKind = SummaryKindFlag.getValue();
  config.resourceKind = ResourceKindFlag.getValue();
  config.showVersion = ShowVersion.getValue();
  config.schema = Schema.getValue();
  return config;
}

} // namespace lotus::pdg_query
