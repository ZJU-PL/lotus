#include "IR/PDG/Analyses/ApiAnalysis.h"
#include "IR/PDG/Analyses/ArithmeticAnalysis.h"
#include "IR/PDG/Analyses/BoundsAnalysis.h"
#include "IR/PDG/Analyses/CallContractAnalysis.h"
#include "IR/PDG/Analyses/LifetimeAnalysis.h"
#include "IR/PDG/Analyses/ProtocolAnalysis.h"
#include "IR/PDG/Analyses/StateAnalysis.h"
#include "IR/PDG/Rules/RuleEngine.h"

#include <map>

namespace pdg {

const std::vector<RuleDescriptor> &RuleEngine::catalog() {
  static const std::vector<RuleDescriptor> rules = [] {
    std::vector<RuleDescriptor> rules = {
        {"cpp/dangerous-function-overflow",
         "error",
         "Security/CWE/CWE-676/DangerousFunctionOverflow.ql",
         "Direct global/std gets calls with one declared parameter; no "
         "unresolved indirect calls.",
         {}},
        {"cpp/potentially-dangerous-function",
         "warning",
         "Security/CWE/CWE-676/PotentiallyDangerousFunction.ql",
         "Direct global gmtime/localtime/ctime/asctime calls; no unresolved "
         "indirect calls.",
         {}},
        {"cpp/power-of-10/use-of-jmp",
         "warning",
         "Power of 10/Rule 1/UseOfJmp.ql",
         "Direct calls with base name setjmp/longjmp/sigsetjmp/siglongjmp in "
         "any "
         "namespace; lowered builtins and unresolved indirect calls "
         "excluded.",
         {}},
        {"cpp/bad-strncpy-size",
         "warning",
         "Likely Bugs/Memory Management/StrncpyFlippedArgs.ql",
         "Subset: fixed object and constant bound, or strlen/wcslen(source) "
         "with "
         "constant offsets >= 0; requires SSA pointer values. No GVN or sizeof "
         "provenance. Same-block load equivalence checks intervening "
         "clobbers.",
         {}},
        {"cpp/return-stack-allocated-memory",
         "warning",
         "Likely Bugs/Memory Management/ReturnStackAllocatedMemory.ql",
         "Subset: pointer returns whose SSA origins are all local allocas or "
         "modeled stack-returning APIs (including GEP/cast/PHI/select). No "
         "memory loads or interprocedural flow; use mem2reg. Functions named "
         "with stack/sp excluded.",
         {}},
        {"cpp/power-of-10/use-of-recursion",
         "warning",
         "Power of 10/Rule 1/UseOfRecursion.ql",
         "Direct call-graph cycles, including mutual recursion; unresolved "
         "indirect calls excluded.",
         {}},
        {"cpp/jpl-c/recursion",
         "warning",
         "JPL_C/LOC-2/Rule 04/Recursion.ql",
         "Direct call-graph cycles, including mutual recursion; unresolved "
         "indirect calls excluded.",
         {}},
        {"cpp/wrong-number-format-arguments",
         "error",
         "Likely Bugs/Format/WrongNumberOfFormatArguments.ql",
         "Known printf-family APIs with constant ASCII/narrow or wide "
         "literals; "
         "positional and star arguments supported. Custom formatting "
         "attributes "
         "and source syntax errors unavailable.",
         {}},
        {"cpp/too-many-format-arguments",
         "recommendation",
         "Likely Bugs/Format/TooManyFormatArguments.ql",
         "Known printf-family APIs and parsed literals; unknown directives and "
         "va_list calls excluded.",
         {}},
        {"cpp/memory-unsafe-function-scan",
         "warning",
         "experimental/Security/CWE/CWE-120/MemoryUnsafeFunctionScan.ql",
         "Scanf-family constant formats with unsuppressed unbounded %s/%ls. "
         "Escaped percent and bounded fields excluded.",
         {}},
        {"cpp/incorrectly-checked-scanf",
         "warning",
         "Critical/IncorrectCheckScanf.ql",
         "SSA scanf results used by branches testing only equality/inequality "
         "with zero; positive, EOF and signed range checks suppress. Kernel "
         "detection needs debug macros.",
         {}},
        {"cpp/new-free-mismatch",
         "warning",
         "Critical/NewFreeMismatch.ql",
         "SSA/GEP/cast origins from modeled allocation APIs reaching an "
         "incompatible release in the same function; all origins must "
         "establish "
         "the mismatch. No escaping member memory or cross-function flow.",
         {}},
        {"cpp/new-array-delete-mismatch",
         "warning",
         "Critical/NewArrayDeleteMismatch.ql",
         "Direct modeled operator new[] origins reaching operator delete "
         "through "
         "SSA values; placement/nothrow wrappers excluded.",
         {}},
        {"cpp/new-delete-array-mismatch",
         "warning",
         "Critical/NewDeleteArrayMismatch.ql",
         "Direct modeled operator new origins reaching operator delete[] "
         "through "
         "SSA values; placement/nothrow wrappers excluded.",
         {}},
        {"cpp/incomplete-parity-check",
         "warning",
         "Likely Bugs/Arithmetic/BadCheckOdd.ql",
         "Signed remainder by 2 compared with 1 in retained LLVM instructions; "
         "source literal/macro distinctions unavailable.",
         {}},
        {"cpp/equality-on-floats",
         "recommendation",
         "Likely Bugs/Arithmetic/FloatComparison.ql",
         "Retained floating equality/inequality between nonconstants that are "
         "not equivalent; configuration-test-file exclusions unavailable.",
         {}},
        {"cpp/lossy-pointer-cast",
         "warning",
         "Likely Bugs/Conversion/LossyPointerCast.ql",
         "Retained pointer-to-integer instructions narrower than the target "
         "pointer width, excluding booleans and bitmask uses. Source macro "
         "exclusions unavailable.",
         {}},
        {"cpp/unsafe-strncat",
         "warning",
         "Likely Bugs/Memory Management/SuspiciousCallToStrncat.ql",
         "Subset: known destination capacity minus strlen(destination), "
         "missing "
         "the extra byte for the terminator. sizeof provenance branch "
         "unavailable.",
         {}},
        {"cpp/open-call-with-mode-argument",
         "error",
         "Security/CWE/CWE-732/OpenCallMissingModeArgument.ql",
         "Direct open/openat/_open/_wopen with constant flags and missing "
         "mode; "
         "O_CREAT/O_TMPFILE obtained from unique debug macros, never host "
         "constants.",
         {}},
        {"cpp/world-writable-file-creation",
         "warning",
         "Security/CWE/CWE-732/DoNotCreateWorldWritable.ql",
         "chmod/creat/fopen/open families with known mode and same-block "
         "constant umask. Cross-block umask, wide fopen modes and "
         "configuration "
         "exclusions unavailable.",
         {}},
        {"cpp/insecure-generation-of-filename",
         "warning",
         "experimental/Security/CWE/CWE-377/InsecureTemporaryFile.ql",
         "tmpnam-family calls without a CFG-connected "
         "mktemp/mkstemp/mkstemps/mkdtemp call in the same function.",
         {}},
        {"cpp/alloca-in-loop",
         "warning",
         "Likely Bugs/Memory Management/AllocaInLoop.ql",
         "LLVM alloca instructions in natural loops not proved small by SCEV; "
         "loops with stackrestore excluded to avoid scoped VLAs. Microsoft "
         "declaration provenance unavailable.",
         {}},
        {"cpp/curl-disabled-ssl",
         "warning",
         "experimental/Security/CWE/CWE-295/CurlSSL.ql",
         "curl_easy_setopt disabling numeric "
         "CURLOPT_SSL_VERIFYHOST(81)/VERIFYPEER(64); source enum-name "
         "provenance "
         "unavailable.",
         {}},
        {"cpp/signed-overflow-check",
         "warning",
         "Likely Bugs/Arithmetic/SignedOverflowCheck.ql",
         "Retained comparisons of an nsw addition that may overflow positively "
         "to an equivalent operand; SCEV suppresses bounded additions. nsw "
         "excludes wrapping "
         "additions; source macro exclusions unavailable.",
         {}},
        {"cpp/dead-code-function",
         "warning",
         "Critical/DeadCodeFunction.ql",
         "Defined local-linkage nonconstructor/nonoperator functions with no "
         "uses; source private methods/templates unavailable and "
         "compiler-discarded functions absent.",
         {}},
        {"cpp/return-value-ignored",
         "recommendation",
         "Critical/ReturnValueIgnored.ql",
         "Subset: unused fgets return values (CodeQL default mandatory-check "
         "API). Statistical, explicit-void and macro/source-context handling "
         "unavailable.",
         {}},
        {"cpp/uncontrolled-process-operation",
         "warning",
         "Security/CWE/CWE-114/UncontrolledProcessOperation.ql",
         "PDG user-input content flow into modeled process/library-loading "
         "arguments; matched parameter/return and memory updates. Unknown "
         "unknown aliases/indirect calls/unmodeled C++ objects excluded.",
         {}},
        {"cpp/command-line-injection",
         "error",
         "Security/CWE/CWE-078/ExecTainted.ql",
         "PDG flow-state requires a noninitial %s/%S conversion or strcat "
         "source "
         "before a shell-command sink; transparent wrappers supported. Direct "
         "whole-command input uses uncontrolled-process-operation.",
         {}},
        {"cpp/sql-injection",
         "error",
         "Security/CWE/CWE-089/SqlTainted.ql",
         "PDG input-content flow into modeled SQL arguments and transparent "
         "wrappers; modeled SQL escaping is a domain-specific barrier. C++ "
         "database objects and arbitrary barrier models excluded.",
         {}},
        {"cpp/path-injection",
         "warning",
         "Security/CWE/CWE-022/TaintedPath.ql",
         "PDG input-content flow into modeled file-path arguments and "
         "transparent wrappers. Source-AST variable upper-bound checks, "
         "fstream "
         "objects and unknown aliases excluded.",
         {}},
        {"cpp/tainted-format-string",
         "warning",
         "Security/CWE/CWE-134/UncontrolledFormatString.ql",
         "PDG user-input content flow into formatting API roles and "
         "transparent "
         "wrappers. Numeric/content facts are separate; custom format "
         "attributes "
         "and unmodeled C++ string APIs excluded.",
         {}},
        {"cpp/non-constant-format",
         "recommendation",
         "Likely Bugs/Format/NonConstantFormat.ql",
         "PDG format-content provenance from input, uncalled pointer "
         "parameters "
         "and unknown external pointer outputs. Constant strings/uninitialized "
         "allocations are not sources; no array-index taint propagation.",
         {}},
        {"cpp/uncontrolled-allocation-size",
         "warning",
         "Security/CWE/CWE-190/TaintedAllocationSize.ql",
         "PDG scalar input flow into modeled heap allocation sizes; dominating "
         "upper-bound/equality guards and range-reducing operations suppress "
         "candidates. Signed upper guards also require nonnegativity. "
         "Source-AST "
         "variable checks, stack allocation expressions and indirect/custom "
         "allocators remain partial.",
         {}},
        {"cpp/wordexp-injection",
         "error",
         "experimental/Security/CWE/CWE-078/WordexpTainted.ql",
         "PDG-native user-content flow to POSIX wordexp input, including "
         "direct call/return summaries and transparent wrappers. WRDE_NOCMD "
         "in constant or OR-composed flags suppresses command substitution. "
         "Unknown aliases, indirect calls and unmodeled C++ object APIs remain "
         "partial.",
         {}}};
    auto append = [&](const auto &catalog) {
      for (const auto &rule : catalog)
        rules.push_back(
            {rule.id, rule.severity, rule.codeql_query, rule.coverage, {}});
    };
    append(BoundsAnalysis::catalog());
    append(LifetimeAnalysis::catalog());
    append(StateAnalysis::catalog());
    append(ArithmeticAnalysis::catalog());
    append(ApiAnalysis::catalog());
    append(CallContractAnalysis::catalog());
    append(ProtocolAnalysis::catalog());
    for (auto &rule : rules)
      rule.coverage +=
          " Shared callee resolution includes immutable function-pointer "
          "globals, unchanged local spills and agreeing select/phi targets; "
          "ambiguous or clobbered targets remain unresolved.";
    static const std::map<std::string, std::vector<unsigned>> cwe_tags = {
        {"cpp/alloca-in-loop", {770}},
        {"cpp/arithmetic-with-extreme-values", {190, 191}},
        {"cpp/bad-addition-overflow-check", {190, 192}},
        {"cpp/bad-strncpy-size", {119, 251, 676}},
        {"cpp/badly-bounded-write", {120, 787, 805}},
        {"cpp/certificate-not-checked", {295}},
        {"cpp/certificate-result-conflation", {295}},
        {"cpp/command-line-injection", {78, 88}},
        {"cpp/conditionally-uninitialized-variable", {457}},
        {"cpp/curl-disabled-ssl", {295}},
        {"cpp/dangerous-function-overflow", {242, 676}},
        {"cpp/dangerous-use-of-ssl-shutdown", {670}},
        {"cpp/dead-code-function", {561}},
        {"cpp/deref-null-result", {476}},
        {"cpp/descriptor-may-not-be-closed", {775}},
        {"cpp/descriptor-never-closed", {775}},
        {"cpp/divide-by-zero-using-return-value", {369}},
        {"cpp/double-free", {415}},
        {"cpp/double-release", {666, 675}},
        {"cpp/external-entity-expansion", {611}},
        {"cpp/file-may-not-be-closed", {775}},
        {"cpp/file-never-closed", {775}},
        {"cpp/if-statement-addition-overflow", {190}},
        {"cpp/improper-check-return-value-scanf", {754, 908}},
        {"cpp/improper-null-termination", {170, 665}},
        {"cpp/inconsistent-null-check", {476}},
        {"cpp/inconsistent-nullness-testing", {476}},
        {"cpp/incorrectly-checked-scanf", {253}},
        {"cpp/insecure-generation-of-filename", {377}},
        {"cpp/insufficient-key-size", {326}},
        {"cpp/integer-multiplication-cast-to-long", {190, 192, 197, 681}},
        {"cpp/integer-overflow-tainted", {190, 197, 681}},
        {"cpp/invalid-pointer-deref", {119, 125, 193, 787}},
        {"cpp/late-check-of-function-argument", {20}},
        {"cpp/memory-leak-on-failed-call-to-realloc", {401}},
        {"cpp/memory-may-not-be-freed", {401}},
        {"cpp/memory-never-freed", {401}},
        {"cpp/memory-unsafe-function-scan", {120}},
        {"cpp/missing-check-scanf", {252, 253}},
        {"cpp/missing-null-test", {476}},
        {"cpp/multiplication-overflow-in-alloc", {128, 190}},
        {"cpp/new-free-mismatch", {762}},
        {"cpp/no-space-for-terminator", {120, 122, 131}},
        {"cpp/non-constant-format", {134}},
        {"cpp/not-initialised", {457}},
        {"cpp/open-call-with-mode-argument", {732}},
        {"cpp/overflow-buffer", {119, 121, 122, 126}},
        {"cpp/overflow-calculated", {120, 131}},
        {"cpp/overflow-destination", {119, 131}},
        {"cpp/overflowing-snprintf", {190, 253}},
        {"cpp/overrunning-write", {120, 787, 805}},
        {"cpp/overrunning-write-with-float", {120, 787, 805}},
        {"cpp/path-injection", {22, 23, 36, 73}},
        {"cpp/potential-buffer-overflow", {676}},
        {"cpp/potentially-dangerous-function", {676}},
        {"cpp/return-stack-allocated-memory", {825}},
        {"cpp/return-value-ignored", {252}},
        {"cpp/signed-overflow-check", {128, 190}},
        {"cpp/sql-injection", {89}},
        {"cpp/static-buffer-overflow", {119, 131}},
        {"cpp/tainted-arithmetic", {190, 191}},
        {"cpp/tainted-format-string", {134}},
        {"cpp/too-few-arguments", {234, 685}},
        {"cpp/twice-locked", {764, 833}},
        {"cpp/unbounded-write", {120, 787, 805}},
        {"cpp/unclear-array-index-validation", {129}},
        {"cpp/uncontrolled-allocation-size", {190, 789}},
        {"cpp/uncontrolled-arithmetic", {190, 191}},
        {"cpp/uncontrolled-process-operation", {73, 78, 114}},
        {"cpp/uninitialized-local", {457, 665}},
        {"cpp/unreleased-lock", {764, 833}},
        {"cpp/unsafe-dacl-security-descriptor", {732}},
        {"cpp/unsafe-strncat", {119, 251, 676, 788}},
        {"cpp/unsigned-difference-expression-compared-zero", {191}},
        {"cpp/unterminated-variadic-call", {121}},
        {"cpp/use-after-free", {416}},
        {"cpp/user-controlled-null-termination-tainted", {170}},
        {"cpp/very-likely-overrunning-write", {120, 787, 805}},
        {"cpp/weak-cryptographic-algorithm", {327}},
        {"cpp/wordexp-injection", {78}},
        {"cpp/world-writable-file-creation", {732}},
        {"cpp/wrong-number-format-arguments", {234, 685}},
        {"cpp/wrong-type-format-argument", {686}},
        {"cpp/wrong-use-of-the-umask", {200, 264, 266, 560, 687}},
        {"lotus/shared-global-race", {362}},
        {"lotus/lock-order-cycle", {833}},
        {"lotus/filesystem-toctou", {367}},
    };
    for (auto &rule : rules) {
      auto it = cwe_tags.find(rule.id);
      if (it != cwe_tags.end())
        rule.cwes = it->second;
    }
    return rules;
  }();

  return rules;
}

} // namespace pdg
