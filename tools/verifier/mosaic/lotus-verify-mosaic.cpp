#include "Mosaic/CHCParser.h"
#include "Mosaic/MosaicFixedpoint.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <z3++.h>

namespace {

enum ExitCode {
  EXIT_SAT = 0,
  EXIT_UNSAT = 1,
  EXIT_UNKNOWN = 2,
  EXIT_ERROR = 3,
};

enum class Backend { Mosaic, Z3 };

struct Options {
  std::filesystem::path input;
  std::filesystem::path output;
  Backend backend = Backend::Mosaic;
  bool force_preprocess = false;
  bool quiet = false;
};

z3::params getFixedpointParams(z3::context &context) {
  z3::params params(context);
  params.set("engine", "spacer");
  params.set("fp.xform.slice", false);
  params.set("fp.xform.inline_linear", false);
  params.set("fp.xform.inline_eager", false);
  return params;
}

void printHelp() {
  lotus::mosaic::OUT()
      << "Usage:\n"
         "  lotus-verify-mosaic [options] <input.smt2>\n"
         "Options:\n"
         "  --backend mosaic|z3  Select the solver backend (default: mosaic)\n"
         "  --force-preprocess   Force Mosaic integer-to-BV preprocessing\n"
         "  --debug              Enable Mosaic diagnostics\n"
         "  --quiet              Suppress the result line\n"
         "  --output <file>      Redirect tool output\n"
         "  --help               Show this help\n\n"
         "The input must be a standard Horn SMT-LIB2 file with exactly one\n"
         "zero-argument query relation and one rule defining that relation.\n\n"
         "Exit codes: 0=SAT, 1=UNSAT, 2=UNKNOWN, 3=error\n";
}

Options parseOptions(int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const char *argument = argv[index];
    if (std::strcmp(argument, "--help") == 0) {
      printHelp();
      std::exit(EXIT_SAT);
    }
    if (std::strcmp(argument, "--backend") == 0 && index + 1 < argc) {
      const std::string backend = argv[++index];
      if (backend == "mosaic")
        options.backend = Backend::Mosaic;
      else if (backend == "z3")
        options.backend = Backend::Z3;
      else
        throw std::invalid_argument("--backend expects 'mosaic' or 'z3'");
    } else if (std::strcmp(argument, "--output") == 0 && index + 1 < argc) {
      options.output = argv[++index];
    } else if (std::strcmp(argument, "--force-preprocess") == 0) {
      options.force_preprocess = true;
    } else if (std::strcmp(argument, "--debug") == 0) {
      lotus::mosaic::set_mtfp_debug(true);
    } else if (std::strcmp(argument, "--quiet") == 0) {
      options.quiet = true;
    } else if (argument[0] == '-') {
      throw std::invalid_argument("unknown or incomplete option '" +
                                  std::string(argument) + "'");
    } else if (options.input.empty()) {
      options.input = argument;
    } else {
      throw std::invalid_argument("multiple input files were specified");
    }
  }

  if (options.input.empty())
    throw std::invalid_argument("missing input file");
  if (options.backend == Backend::Z3 && options.force_preprocess)
    throw std::invalid_argument(
        "--force-preprocess is only valid with the Mosaic backend");
  return options;
}

std::pair<std::string, ExitCode> resultAndCode(z3::check_result result) {
  if (result == z3::sat)
    return {"SAT", EXIT_SAT};
  if (result == z3::unsat)
    return {"UNSAT", EXIT_UNSAT};
  return {"UNKNOWN", EXIT_UNKNOWN};
}

int run(int argc, char **argv) {
  const Options options = parseOptions(argc, argv);

  std::ofstream output_stream;
  if (!options.output.empty()) {
    output_stream.open(options.output);
    if (!output_stream)
      throw std::runtime_error("cannot open output file '" +
                               options.output.string() + "'");
    lotus::mosaic::set_output_stream(output_stream);
    lotus::mosaic::set_error_stream(output_stream);
  }

  z3::context context;
  z3::fixedpoint fixedpoint(context);
  fixedpoint.set(getFixedpointParams(context));
  z3::expr query =
      lotus::mosaic::CHCParser(context).parseFile(fixedpoint, options.input);

  z3::check_result result = z3::unknown;
  if (options.backend == Backend::Mosaic) {
    lotus::mosaic::MosaicFixedpoint mosaic(context, options.force_preprocess);
    mosaic.from_solver(fixedpoint);
    result = mosaic.query(query);
  } else {
    result = fixedpoint.query(query);
  }

  auto [result_text, exit_code] = resultAndCode(result);
  if (!options.quiet)
    lotus::mosaic::OUT() << result_text << " - (Exit code: " << exit_code
                         << ")\n";
  return exit_code;
}

} // namespace

int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &error) {
    lotus::mosaic::ERR() << "error: " << error.what() << '\n';
    return EXIT_ERROR;
  }
}
