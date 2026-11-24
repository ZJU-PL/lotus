#include "Mosaic/CHCParser.h"

#include "Mosaic/MosaicUtils.h"

#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

namespace lotus::mosaic {
namespace {

z3::expr materializeQuery(z3::fixedpoint &fixedpoint,
                          const z3::expr &parsed_query,
                          const std::string &source_name) {
  if (!utils::is_uninterpreted_predicate(parsed_query) ||
      parsed_query.num_args() != 0)
    throw std::runtime_error("query in CHC source '" + source_name +
                             "' must name a zero-argument relation");

  std::optional<z3::expr> query_rule;
  for (const z3::expr &rule : fixedpoint.rules()) {
    z3::expr head = get_clause_head(rule, /*is_query=*/false);
    if (!z3::eq(head.decl(), parsed_query.decl()))
      continue;
    if (query_rule.has_value())
      throw std::runtime_error("query relation in CHC source '" + source_name +
                               "' has more than one defining rule");
    query_rule = rule;
  }
  if (!query_rule.has_value())
    throw std::runtime_error("query relation in CHC source '" + source_name +
                             "' has no defining rule");

  z3::expr_vector variables = get_clause_vars(*query_rule);
  z3::expr_vector replacements(parsed_query.ctx());
  for (unsigned index = variables.size(); index > 0; --index)
    replacements.push_back(variables[index - 1]);

  z3::expr body =
      get_clause_body(*query_rule, /*is_query=*/false).substitute(replacements);
  if (!body.is_and())
    body = body && parsed_query.ctx().bool_val(true);

  // Mosaic's refinement loop expects query variables corresponding to the
  // queried predicate to appear first and in predicate-argument order. Z3 may
  // reorder the binders while parsing a rule, so recover the stable order from
  // the first body predicate and append auxiliary formula variables afterward.
  z3::expr_vector ordered_variables(parsed_query.ctx());
  for (const z3::expr &conjunct : utils::get_conjuncts(body)) {
    if (!utils::is_uninterpreted_predicate(conjunct))
      continue;
    for (const z3::expr &argument : utils::get_args(conjunct)) {
      if (argument.is_const())
        ordered_variables.push_back(argument);
    }
    break;
  }
  for (const z3::expr &variable : variables) {
    bool already_added = false;
    for (const z3::expr &ordered : ordered_variables)
      already_added |= z3::eq(variable, ordered);
    if (!already_added)
      ordered_variables.push_back(variable);
  }
  return z3::exists(ordered_variables, body);
}

void registerRelations(const z3::expr &expression, z3::fixedpoint &fixedpoint,
                       std::set<unsigned> &registered) {
  if (expression.is_quantifier()) {
    registerRelations(expression.body(), fixedpoint, registered);
    return;
  }
  if (!expression.is_app())
    return;
  if (expression.is_bool() && utils::is_uninterpreted_predicate(expression) &&
      registered.insert(expression.decl().id()).second) {
    z3::func_decl relation = expression.decl();
    fixedpoint.register_relation(relation);
  }
  for (const z3::expr &argument : utils::get_args(expression))
    registerRelations(argument, fixedpoint, registered);
}

void copyProgramRules(z3::fixedpoint &source, z3::fixedpoint &destination,
                      const z3::expr &parsed_query) {
  unsigned rule_number = 0;
  std::set<unsigned> registered;
  for (z3::expr rule : source.rules()) {
    z3::expr head = get_clause_head(rule, /*is_query=*/false);
    if (z3::eq(head.decl(), parsed_query.decl()))
      continue;

    registerRelations(rule, destination, registered);
    const std::string name =
        "__mosaic_input_rule_" + std::to_string(rule_number++);
    destination.add_rule(rule, rule.ctx().str_symbol(name.c_str()));
  }
}

} // namespace

z3::expr CHCParser::parseFile(z3::fixedpoint &fixedpoint,
                              const std::filesystem::path &path) const {
  std::ifstream stream(path);
  if (!stream)
    throw std::runtime_error("cannot open CHC file '" + path.string() + "'");

  std::ostringstream contents;
  contents << stream.rdbuf();
  return parseString(fixedpoint, contents.str(), path.string());
}

z3::expr CHCParser::parseString(z3::fixedpoint &fixedpoint, std::string input,
                                const std::string &source_name) const {
  try {
    z3::fixedpoint parsed_fixedpoint(m_context);
    z3::expr_vector queries = parsed_fixedpoint.from_string(input.c_str());
    if (queries.size() != 1)
      throw std::runtime_error("CHC source '" + source_name + "' defines " +
                               std::to_string(queries.size()) +
                               " queries; exactly one is required");
    z3::expr query =
        materializeQuery(parsed_fixedpoint, queries[0], source_name);
    copyProgramRules(parsed_fixedpoint, fixedpoint, queries[0]);
    return query;
  } catch (const z3::exception &error) {
    throw std::runtime_error("failed to parse CHC source '" + source_name +
                             "': " + error.msg());
  }
}

} // namespace lotus::mosaic
