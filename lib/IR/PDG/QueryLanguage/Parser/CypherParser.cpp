#include "IR/PDG/QueryLanguage/Parser/CypherParser.h"

#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"


#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <sstream>

namespace pdg {

static std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

static std::string unquoteStringToken(std::string token) {
  if (token.size() >= 2 && ((token.front() == '"' && token.back() == '"') ||
                            (token.front() == '\'' && token.back() == '\''))) {
    token = token.substr(1, token.size() - 2);
  }
  return token;
}

// ============================================================================
// CypherParser implementation
// ============================================================================

std::unique_ptr<CypherQuery> CypherParser::parse(const std::string &query) {
  clearError();
  std::string trimmedQuery = query;
  trim(trimmedQuery);

  if (trimmedQuery.empty()) {
    setError(CypherErrorCode::PARSE_ERROR, "Empty query", 1, 1);
    return nullptr;
  }

  size_t pos = 0;
  std::vector<std::string> tokens = tokenize(trimmedQuery);

  if (tokens.empty()) {
    setError(CypherErrorCode::PARSE_ERROR, "No valid tokens found", 1, 1);
    return nullptr;
  }

  activeParams_ = nullptr;
  return parseQuery(tokens, pos, CypherQueryParameters{});
}

std::unique_ptr<CypherQuery>
CypherParser::parse(const std::string &query,
                    const CypherQueryParameters &params) {
  clearError();
  std::string trimmedQuery = query;
  trim(trimmedQuery);

  if (trimmedQuery.empty()) {
    setError(CypherErrorCode::PARSE_ERROR, "Empty query", 1, 1);
    return nullptr;
  }

  size_t pos = 0;
  std::vector<std::string> tokens = tokenize(trimmedQuery);

  if (tokens.empty()) {
    setError(CypherErrorCode::PARSE_ERROR, "No valid tokens found", 1, 1);
    return nullptr;
  }

  activeParams_ = &params;
  return parseQuery(tokens, pos, params);
}

void CypherParser::trim(std::string &s) {
  if (s.empty())
    return;

  size_t start = 0;
  while (start < s.size() &&
         std::isspace(static_cast<unsigned char>(s[start]))) {
    start++;
  }

  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    end--;
  }

  s = s.substr(start, end - start);
}

bool CypherParser::isAlpha(char c) {
  return std::isalpha(static_cast<unsigned char>(c));
}

bool CypherParser::isDigit(char c) {
  return std::isdigit(static_cast<unsigned char>(c));
}

bool CypherParser::isAlphaNumeric(char c) {
  return isAlpha(c) || isDigit(c) || c == '_' || c == '$';
}

std::vector<std::string> CypherParser::tokenize(const std::string &s) {
  std::vector<std::string> tokens;
  std::string current;
  bool inString = false;
  char stringChar = '"';

  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];

    if (inString) {
      if (c == '\\' && i + 1 < s.size()) {
        current += c;
        current += s[++i];
      } else if (c == stringChar) {
        current += c;
        tokens.push_back(current);
        current.clear();
        inString = false;
      } else {
        current += c;
      }
      continue;
    }

    if (c == '"' || c == '\'') {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      current += c;
      stringChar = c;
      inString = true;
      continue;
    }

    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      continue;
    }

    if (c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' ||
        c == ',' || c == ';' || c == ':' || c == '-' || c == '>' || c == '<' ||
        c == '=' || c == '!' || c == '.' || c == '@' || c == '#' || c == '*') {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      if (c == '-' && i + 1 < s.size() && s[i + 1] == '>') {
        tokens.push_back("->");
        i++;
      } else if (c == '<' && i + 1 < s.size() && s[i + 1] == '-') {
        tokens.push_back("<-");
        i++;
      } else if (c == '<' && i + 1 < s.size() && s[i + 1] == '=') {
        tokens.push_back("<=");
        i++;
      } else if (c == '>' && i + 1 < s.size() && s[i + 1] == '=') {
        tokens.push_back(">=");
        i++;
      } else if (c == '!' && i + 1 < s.size() && s[i + 1] == '=') {
        tokens.push_back("!=");
        i++;
      } else if (c == '<' && i + 1 < s.size() && s[i + 1] == '>') {
        tokens.push_back("<>");
        i++;
      } else if (c == '=' && i + 1 < s.size() && s[i + 1] == '=') {
        tokens.push_back("==");
        i++;
      } else {
        std::string single(1, c);
        tokens.push_back(single);
      }
      continue;
    }

    current += c;
  }

  if (!current.empty()) {
    tokens.push_back(current);
  }

  return tokens;
}

std::unique_ptr<CypherQuery>
CypherParser::parseQuery(std::vector<std::string> &tokens, size_t &pos,
                         const CypherQueryParameters &params) {
  auto query = std::make_unique<CypherQuery>();

  while (hasMore(tokens, pos)) {
    std::string token = peek(tokens, pos);
    std::string upperToken = token;
    std::transform(upperToken.begin(), upperToken.end(), upperToken.begin(),
                   ::toupper);

    if (upperToken == "MATCH") {
      consume(tokens, pos);
      // Helper lambda to get uppercased peek
      auto peekUpper = [&tokens, &pos]() -> std::string {
        if (pos < tokens.size()) {
          std::string t = tokens[pos];
          std::transform(t.begin(), t.end(), t.begin(), ::toupper);
          return t;
        }
        return "";
      };
      while (hasMore(tokens, pos) && peekUpper() != "WHERE" &&
             peekUpper() != "RETURN" && peekUpper() != "WITH" &&
             peekUpper() != "MATCH" && peekUpper() != ";") {
        // Optional path binding syntax: MATCH p = (a)-[*]->(b) ...
        if (pos + 1 < tokens.size() && tokens[pos + 1] == "=") {
          consume(tokens, pos); // path variable (currently ignored)
          consume(tokens, pos); // '='
        }
        auto pattern = parsePattern(tokens, pos);
        if (!pattern) {
          return nullptr;
        }
        query->addPattern(std::move(pattern));
        // Handle comma-separated patterns
        if (hasMore(tokens, pos) && peek(tokens, pos) == ",") {
          consume(tokens, pos);
        }
      }
    } else if (upperToken == "WHERE") {
      consume(tokens, pos);
      auto whereClause = parseWhereClause(tokens, pos);
      if (!whereClause) {
        return nullptr;
      }
      query->andWhereClause(std::move(whereClause));
    } else if (upperToken == "RETURN") {
      consume(tokens, pos);
      // Helper lambda to get uppercased peek
      auto peekUpper = [&tokens, &pos]() -> std::string {
        if (pos < tokens.size()) {
          std::string t = tokens[pos];
          std::transform(t.begin(), t.end(), t.begin(), ::toupper);
          return t;
        }
        return "";
      };
      while (hasMore(tokens, pos) && peekUpper() != "ORDER" &&
             peekUpper() != "LIMIT" && peekUpper() != ";") {
        auto item = parseReturnItem(tokens, pos);
        if (!item) {
          return nullptr;
        }
        query->addReturnItem(std::move(item));
        if (hasMore(tokens, pos) && peek(tokens, pos) == ",") {
          consume(tokens, pos);
        }
      }
    } else if (upperToken == "ORDER") {
      consume(tokens, pos);
      if (hasMore(tokens, pos) &&
          (peek(tokens, pos) == "BY" || peek(tokens, pos) == "by")) {
        consume(tokens, pos);
        auto orderBy = parseOrderBy(tokens, pos);
        if (orderBy) {
          query->setOrderBy(std::move(orderBy));
        }
      } else {
        setError(CypherErrorCode::SYNTAX_ERROR, "Expected BY after ORDER", 0,
                 0);
        return nullptr;
      }
    } else if (upperToken == "LIMIT") {
      consume(tokens, pos);
      if (hasMore(tokens, pos)) {
        std::string limitStr = consume(tokens, pos);
        try {
          int limit = std::stoi(limitStr);
          query->setLimit(limit);
        } catch (...) {
          setError(CypherErrorCode::SYNTAX_ERROR,
                   "Invalid LIMIT value: " + limitStr, 0, 0);
          return nullptr;
        }
      } else {
        setError(CypherErrorCode::SYNTAX_ERROR, "Expected value after LIMIT", 0,
                 0);
        return nullptr;
      }
    } else if (upperToken == "WITH") {
      consume(tokens, pos);
      // Helper lambda to get uppercased peek
      auto peekUpper = [&tokens, &pos]() -> std::string {
        if (pos < tokens.size()) {
          std::string t = tokens[pos];
          std::transform(t.begin(), t.end(), t.begin(), ::toupper);
          return t;
        }
        return "";
      };
      while (hasMore(tokens, pos) && peekUpper() != "MATCH" &&
             peekUpper() != "RETURN" && peekUpper() != ";") {
        auto item = parseReturnItem(tokens, pos);
        if (item) {
          query->addWithItem(std::move(item));
        }
        if (hasMore(tokens, pos) && peek(tokens, pos) == ",") {
          consume(tokens, pos);
        }
      }
    } else if (token == ";") {
      break;
    } else {
      setError(CypherErrorCode::SYNTAX_ERROR, "Unexpected token: " + token, 0,
               0);
      return nullptr;
    }
  }

  if (query->getPatterns().empty()) {
    setError(CypherErrorCode::SYNTAX_ERROR, "MATCH clause is required", 0, 0);
    return nullptr;
  }

  return query;
}

std::unique_ptr<CypherPatternElement>
CypherParser::parsePattern(std::vector<std::string> &tokens, size_t &pos) {
  if (!hasMore(tokens, pos)) {
    setError(CypherErrorCode::SYNTAX_ERROR, "Expected pattern", 0, 0);
    return nullptr;
  }

  auto pattern =
      std::make_unique<CypherPatternElement>(parseNodePattern(tokens, pos));

  if (!pattern->getStartNode()) {
    return nullptr;
  }

  auto cloneNodePattern = [](const CypherNodePattern *nodePat)
      -> std::unique_ptr<CypherNodePattern> {
    if (!nodePat)
      return nullptr;
    auto copy = std::make_unique<CypherNodePattern>(nodePat->getVariable(),
                                                    nodePat->getLabel());
    for (const auto &kv : nodePat->getProperties()) {
      copy->addProperty(kv.first, kv.second);
    }
    return copy;
  };

  CypherPatternElement *current = pattern.get();
  while (hasMore(tokens, pos)) {
    const std::string token = peek(tokens, pos);
    if (token != "[" && token != "-" && token != "<-") {
      break;
    }

    auto rel = parseRelationshipPattern(tokens, pos);
    if (!rel) {
      return nullptr;
    }
    current->setRelationship(std::move(rel));

    if (!hasMore(tokens, pos) || peek(tokens, pos) != "(") {
      setError(CypherErrorCode::SYNTAX_ERROR,
               "Expected '(' for end node pattern", 0, 0);
      return nullptr;
    }
    auto endNode = parseNodePattern(tokens, pos);
    if (!endNode) {
      return nullptr;
    }
    current->setEndNode(std::move(endNode));

    if (!hasMore(tokens, pos)) {
      break;
    }
    const std::string nextToken = peek(tokens, pos);
    if (nextToken == "[" || nextToken == "-" || nextToken == "<-") {
      auto nextStart = cloneNodePattern(current->getEndNode());
      if (!nextStart) {
        return nullptr;
      }
      current = current->addNextElement(
          std::make_unique<CypherPatternElement>(std::move(nextStart)));
    } else {
      break;
    }
  }

  return pattern;
}

std::unique_ptr<CypherNodePattern>
CypherParser::parseNodePattern(std::vector<std::string> &tokens, size_t &pos) {
  if (!hasMore(tokens, pos)) {
    setError(CypherErrorCode::SYNTAX_ERROR, "Expected '(' for node pattern", 0,
             0);
    return nullptr;
  }

  if (peek(tokens, pos) != "(") {
    setError(CypherErrorCode::SYNTAX_ERROR,
             "Expected '(' but found: " + peek(tokens, pos), 0, 0);
    return nullptr;
  }
  consume(tokens, pos);

  std::string variable;
  std::string label;
  std::unordered_map<std::string, std::string> properties;

  if (hasMore(tokens, pos)) {
    std::string next = peek(tokens, pos);
    if (next != ":" && next != ")" && next != "{") {
      variable = consume(tokens, pos);
      if (variable.empty()) {
        return nullptr;
      }
    }
  }

  if (hasMore(tokens, pos) && peek(tokens, pos) == ":") {
    consume(tokens, pos);
    if (hasMore(tokens, pos)) {
      label = consume(tokens, pos);
      if (label.empty()) {
        setError(CypherErrorCode::SYNTAX_ERROR, "Expected label after ':'", 0,
                 0);
        return nullptr;
      }
    }
  }

  if (hasMore(tokens, pos) && peek(tokens, pos) == "{") {
    consume(tokens, pos);
    while (hasMore(tokens, pos) && peek(tokens, pos) != "}") {
      std::string key = consume(tokens, pos);
      std::string value;
      if (hasMore(tokens, pos) && peek(tokens, pos) == ":") {
        consume(tokens, pos);
        value = consume(tokens, pos);
        if (!value.empty() && value.front() == '$') {
          value = substituteParameter(value.substr(1),
                                      activeParams_ ? *activeParams_
                                                    : CypherQueryParameters{});
        }
        value = unquoteStringToken(std::move(value));
        properties[key] = value;
      }
      if (hasMore(tokens, pos) && peek(tokens, pos) == ",") {
        consume(tokens, pos);
      }
    }
    if (hasMore(tokens, pos) && peek(tokens, pos) == "}") {
      consume(tokens, pos);
    }
  }

  if (!hasMore(tokens, pos) || peek(tokens, pos) != ")") {
    setError(CypherErrorCode::SYNTAX_ERROR,
             "Expected ')' to close node pattern", 0, 0);
    return nullptr;
  }
  consume(tokens, pos);

  auto pat = std::make_unique<CypherNodePattern>(variable, label);
  for (const auto &kv : properties) {
    pat->addProperty(kv.first, kv.second);
  }
  return pat;
}

std::unique_ptr<CypherRelationshipPattern>
CypherParser::parseRelationshipPattern(std::vector<std::string> &tokens,
                                       size_t &pos) {
  bool arrowAtStart = false; // arrowhead pointing to start node
  bool arrowAtEnd = false;   // arrowhead pointing to end node

  // Handle leading direction marker: "<-" or "-"
  if (hasMore(tokens, pos)) {
    const std::string token = peek(tokens, pos);
    if (token == "<-") {
      arrowAtStart = true;
      consume(tokens, pos);
    } else if (token == "-") {
      consume(tokens, pos);
    }
  }

  auto consumeTrailingDirection = [&]() {
    if (!hasMore(tokens, pos))
      return;
    const std::string next = peek(tokens, pos);
    if (next == "<-") {
      consume(tokens, pos);
      arrowAtStart = true;
      return;
    }
    if (next == "->") {
      consume(tokens, pos);
      arrowAtEnd = true;
      return;
    }
    if (next == "-") {
      consume(tokens, pos);
      if (hasMore(tokens, pos) && peek(tokens, pos) == ">") {
        consume(tokens, pos);
        arrowAtEnd = true;
      }
      return;
    }
  };

  auto inferDirection = [&]() -> CypherRelationshipPattern::Direction {
    if (arrowAtStart && arrowAtEnd)
      return CypherRelationshipPattern::Direction::BOTH;
    if (arrowAtStart)
      return CypherRelationshipPattern::Direction::IN;
    if (arrowAtEnd)
      return CypherRelationshipPattern::Direction::OUT;
    // Undirected (-[]-) defaults to BOTH for traversal convenience.
    return CypherRelationshipPattern::Direction::BOTH;
  };

  auto parseVarLength = [&]() -> std::pair<int, int> {
    int minHops = 1;
    int maxHops = -1; // -1 means unbounded (executor will cap)
    bool haveExplicitMin = false;

    if (!hasMore(tokens, pos))
      return {minHops, maxHops};

    // "*N" or "*min..max" (both optional)
    const std::string &t0 = peek(tokens, pos);
    if (!t0.empty() && std::isdigit(static_cast<unsigned char>(t0[0]))) {
      std::string minStr = consume(tokens, pos);
      try {
        minHops = std::stoi(minStr);
        haveExplicitMin = true;
      } catch (...) {
        minHops = 1;
      }
    }

    if (hasMore(tokens, pos) && peek(tokens, pos) == ".") {
      consume(tokens, pos); // '.'
      if (hasMore(tokens, pos) && peek(tokens, pos) == ".") {
        consume(tokens, pos); // '.'
        if (hasMore(tokens, pos)) {
          const std::string &t1 = peek(tokens, pos);
          if (!t1.empty() && std::isdigit(static_cast<unsigned char>(t1[0]))) {
            std::string maxStr = consume(tokens, pos);
            try {
              maxHops = std::stoi(maxStr);
            } catch (...) {
              maxHops = -1;
            }
          } else {
            maxHops = -1;
          }
        }
      }
    } else if (haveExplicitMin) {
      // Exact length "*N".
      maxHops = minHops;
    }

    return {std::max(1, minHops), maxHops};
  };

  // Check for relationship pattern in brackets
  if (hasMore(tokens, pos) && peek(tokens, pos) == "[") {
    consume(tokens, pos); // consume '['

    std::string variable;
    std::string type;
    int minHops = 1;
    int maxHops = 1;
    std::unordered_map<std::string, std::string> properties;

    // Pure variable-length pattern: "[*]" / "[*1..3]" (no var / no type).
    if (hasMore(tokens, pos) && peek(tokens, pos) == "*") {
      consume(tokens, pos); // '*'
      const auto range = parseVarLength();
      minHops = range.first;
      maxHops = range.second;

      if (!hasMore(tokens, pos) || peek(tokens, pos) != "]") {
        setError(CypherErrorCode::SYNTAX_ERROR,
                 "Expected ']' to close relationship pattern", 0, 0);
        return nullptr;
      }
      consume(tokens, pos); // ']'
      consumeTrailingDirection();

      auto rel =
          std::make_unique<CypherRelationshipPattern>("", "", inferDirection());
      rel->setMinHops(minHops);
      rel->setMaxHops(maxHops);
      return rel;
    }

    if (hasMore(tokens, pos)) {
      std::string next = peek(tokens, pos);
      if (next != ":" && next != "]") {
        variable = consume(tokens, pos);
      }
    }

    if (hasMore(tokens, pos) && peek(tokens, pos) == ":") {
      consume(tokens, pos);
      if (hasMore(tokens, pos) && peek(tokens, pos) != "]") {
        type = consume(tokens, pos);
      }
    }

    // Optional variable-length hop bounds: "[:T*1..3]" or "[*1..3]"
    if (hasMore(tokens, pos) && peek(tokens, pos) == "*") {
      consume(tokens, pos); // '*'
      const auto range = parseVarLength();
      minHops = range.first;
      maxHops = range.second;
    }

    // Handle properties in braces
    if (hasMore(tokens, pos) && peek(tokens, pos) == "{") {
      consume(tokens, pos);
      while (hasMore(tokens, pos) && peek(tokens, pos) != "}") {
        std::string key = consume(tokens, pos);
        if (hasMore(tokens, pos) && peek(tokens, pos) == ":") {
          consume(tokens, pos);
          std::string value = consume(tokens, pos);
          if (!value.empty() && value.front() == '$') {
            value = substituteParameter(
                value.substr(1),
                activeParams_ ? *activeParams_ : CypherQueryParameters{});
          }
          value = unquoteStringToken(std::move(value));
          properties[key] = value;
        }
        if (hasMore(tokens, pos) && peek(tokens, pos) == ",") {
          consume(tokens, pos);
        }
      }
      if (hasMore(tokens, pos) && peek(tokens, pos) == "}") {
        consume(tokens, pos);
      }
    }

    if (!hasMore(tokens, pos) || peek(tokens, pos) != "]") {
      setError(CypherErrorCode::SYNTAX_ERROR,
               "Expected ']' to close relationship pattern", 0, 0);
      return nullptr;
    }
    consume(tokens, pos); // consume ']'

    // Handle trailing direction
    consumeTrailingDirection();

    auto rel = std::make_unique<CypherRelationshipPattern>(variable, type,
                                                           inferDirection());
    rel->setMinHops(minHops);
    rel->setMaxHops(maxHops);
    for (const auto &kv : properties) {
      rel->addProperty(kv.first, kv.second);
    }
    return rel;
  }

  // No bracket pattern, just direction
  consumeTrailingDirection();
  return std::make_unique<CypherRelationshipPattern>("", "", inferDirection());
}

std::unique_ptr<CypherWhereClause>
CypherParser::parseWhereClause(std::vector<std::string> &tokens, size_t &pos) {
  return parseBooleanExpression(tokens, pos);
}

std::unique_ptr<CypherWhereClause>
CypherParser::parseBooleanExpression(std::vector<std::string> &tokens,
                                     size_t &pos) {
  return parseOrExpression(tokens, pos);
}

std::unique_ptr<CypherWhereClause>
CypherParser::parseOrExpression(std::vector<std::string> &tokens, size_t &pos) {
  auto left = parseAndExpression(tokens, pos);
  if (!left)
    return nullptr;

  while (hasMore(tokens, pos)) {
    std::string op = peek(tokens, pos);
    std::string upperOp = op;
    std::transform(upperOp.begin(), upperOp.end(), upperOp.begin(), ::toupper);
    if (upperOp != "OR")
      break;
    consume(tokens, pos);

    auto right = parseAndExpression(tokens, pos);
    if (!right)
      return nullptr;
    left = CypherWhereClause::makeOr(std::move(left), std::move(right));
  }

  return left;
}

std::unique_ptr<CypherWhereClause>
CypherParser::parseAndExpression(std::vector<std::string> &tokens,
                                 size_t &pos) {
  auto left = parseUnaryExpression(tokens, pos);
  if (!left)
    return nullptr;

  while (hasMore(tokens, pos)) {
    std::string op = peek(tokens, pos);
    std::string upperOp = op;
    std::transform(upperOp.begin(), upperOp.end(), upperOp.begin(), ::toupper);
    if (upperOp != "AND")
      break;
    consume(tokens, pos);

    auto right = parseUnaryExpression(tokens, pos);
    if (!right)
      return nullptr;
    left = CypherWhereClause::makeAnd(std::move(left), std::move(right));
  }

  return left;
}

std::unique_ptr<CypherWhereClause>
CypherParser::parseUnaryExpression(std::vector<std::string> &tokens,
                                   size_t &pos) {
  if (!hasMore(tokens, pos)) {
    setError(CypherErrorCode::SYNTAX_ERROR,
             "Expected expression in WHERE clause", 0, 0);
    return nullptr;
  }

  std::string token = peek(tokens, pos);
  std::string upperToken = token;
  std::transform(upperToken.begin(), upperToken.end(), upperToken.begin(),
                 ::toupper);

  if (upperToken == "NOT") {
    consume(tokens, pos);
    auto expr = parseUnaryExpression(tokens, pos);
    if (!expr)
      return nullptr;
    return CypherWhereClause::makeNot(std::move(expr));
  }

  if (upperToken == "EXISTS") {
    consume(tokens, pos);
    if (!hasMore(tokens, pos) || peek(tokens, pos) != "(") {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected '(' after EXISTS", 0,
               0);
      return nullptr;
    }
    consume(tokens, pos); // '('
    if (!hasMore(tokens, pos)) {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected operand in EXISTS", 0,
               0);
      return nullptr;
    }

    std::string variable = consume(tokens, pos);
    std::string property;
    if (hasMore(tokens, pos) && peek(tokens, pos) == ".") {
      consume(tokens, pos);
      if (!hasMore(tokens, pos)) {
        setError(CypherErrorCode::SYNTAX_ERROR,
                 "Expected property after '.' in EXISTS", 0, 0);
        return nullptr;
      }
      property = consume(tokens, pos);
    }

    if (!hasMore(tokens, pos) || peek(tokens, pos) != ")") {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected ')' after EXISTS", 0,
               0);
      return nullptr;
    }
    consume(tokens, pos); // ')'
    return CypherWhereClause::makeExists(variable, property);
  }

  if (token == "(") {
    consume(tokens, pos);
    auto expr = parseOrExpression(tokens, pos);
    if (!expr)
      return nullptr;
    if (!hasMore(tokens, pos) || peek(tokens, pos) != ")") {
      setError(CypherErrorCode::SYNTAX_ERROR,
               "Expected ')' to close parenthesized WHERE expression", 0, 0);
      return nullptr;
    }
    consume(tokens, pos);
    return expr;
  }

  return parseComparison(tokens, pos);
}

std::unique_ptr<CypherWhereClause>
CypherParser::parseComparison(std::vector<std::string> &tokens, size_t &pos) {
  if (!hasMore(tokens, pos)) {
    setError(CypherErrorCode::SYNTAX_ERROR, "Expected comparison expression", 0,
             0);
    return nullptr;
  }

  std::string variable = consume(tokens, pos);
  std::string property;

  if (hasMore(tokens, pos) && peek(tokens, pos) == ".") {
    consume(tokens, pos);
    if (hasMore(tokens, pos)) {
      property = consume(tokens, pos);
    }
  }

  if (!hasMore(tokens, pos)) {
    return CypherWhereClause::makeExists(variable, property);
  }

  std::string op = consume(tokens, pos);
  std::string upperOp = op;
  std::transform(upperOp.begin(), upperOp.end(), upperOp.begin(), ::toupper);

  // Handle "IN [..]" / "IN [.., ..]" list membership.
  if (upperOp == "IN") {
    if (!hasMore(tokens, pos) || peek(tokens, pos) != "[") {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected '[' after IN", 0, 0);
      return nullptr;
    }
    consume(tokens, pos); // '['

    std::vector<std::string> values;
    while (hasMore(tokens, pos) && peek(tokens, pos) != "]") {
      std::string v = consume(tokens, pos);
      if (!v.empty() && v.front() == '$') {
        v = substituteParameter(v.substr(1), activeParams_
                                                 ? *activeParams_
                                                 : CypherQueryParameters{});
      }
      v = unquoteStringToken(std::move(v));
      values.push_back(std::move(v));

      if (hasMore(tokens, pos) && peek(tokens, pos) == ",") {
        consume(tokens, pos);
      }
    }

    if (!hasMore(tokens, pos) || peek(tokens, pos) != "]") {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected ']' to close IN list",
               0, 0);
      return nullptr;
    }
    consume(tokens, pos); // ']'

    return CypherWhereClause::makeInList(variable, property, std::move(values));
  }

  // Handle "IS NULL" / "IS NOT NULL"
  if (upperOp == "IS") {
    if (!hasMore(tokens, pos)) {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected NULL after IS", 0, 0);
      return nullptr;
    }
    std::string t1 = consume(tokens, pos);
    std::string upperT1 = t1;
    std::transform(upperT1.begin(), upperT1.end(), upperT1.begin(), ::toupper);

    if (upperT1 == "NOT") {
      if (!hasMore(tokens, pos)) {
        setError(CypherErrorCode::SYNTAX_ERROR, "Expected NULL after IS NOT", 0,
                 0);
        return nullptr;
      }
      std::string t2 = consume(tokens, pos);
      std::string upperT2 = t2;
      std::transform(upperT2.begin(), upperT2.end(), upperT2.begin(),
                     ::toupper);
      if (upperT2 != "NULL") {
        setError(CypherErrorCode::SYNTAX_ERROR, "Expected NULL after IS NOT", 0,
                 0);
        return nullptr;
      }
      return CypherWhereClause::makeComparison(
          variable, property, CypherComparisonOp::IS_NOT_NULL, "");
    }

    if (upperT1 != "NULL") {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected NULL after IS", 0, 0);
      return nullptr;
    }
    return CypherWhereClause::makeComparison(variable, property,
                                             CypherComparisonOp::IS_NULL, "");
  }

  // Handle "STARTS WITH" / "ENDS WITH"
  if (upperOp == "STARTS" || upperOp == "ENDS") {
    if (!hasMore(tokens, pos)) {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected WITH after " + op, 0,
               0);
      return nullptr;
    }
    std::string maybeWith = consume(tokens, pos);
    std::string upperWith = maybeWith;
    std::transform(upperWith.begin(), upperWith.end(), upperWith.begin(),
                   ::toupper);
    if (upperWith != "WITH") {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected WITH after " + op, 0,
               0);
      return nullptr;
    }

    std::string value;
    if (hasMore(tokens, pos)) {
      value = consume(tokens, pos);
      if (!value.empty() && value.front() == '$') {
        value = substituteParameter(value.substr(1),
                                    activeParams_ ? *activeParams_
                                                  : CypherQueryParameters{});
      }
      value = unquoteStringToken(std::move(value));
    }

    return CypherWhereClause::makeComparison(
        variable, property,
        (upperOp == "STARTS") ? CypherComparisonOp::STARTS_WITH
                              : CypherComparisonOp::ENDS_WITH,
        value);
  }

  std::string value;
  std::string rightProperty;
  if (hasMore(tokens, pos)) {
    value = consume(tokens, pos);
    if (!value.empty() &&
        (std::isalpha(static_cast<unsigned char>(value.front())) ||
         value.front() == '_') &&
        hasMore(tokens, pos) && peek(tokens, pos) == ".") {
      consume(tokens, pos);
      if (!hasMore(tokens, pos)) {
        setError(CypherErrorCode::SYNTAX_ERROR,
                 "Expected property after RHS '.'", 0, 0);
        return nullptr;
      }
      rightProperty = consume(tokens, pos);
    }
    if (!value.empty() && value.front() == '$') {
      value = substituteParameter(value.substr(1),
                                  activeParams_ ? *activeParams_
                                                : CypherQueryParameters{});
    }
    value = unquoteStringToken(std::move(value));
  }

  CypherComparisonOp comparisonOp = CypherComparisonOp::EQUALS;

  if (upperOp == "=" || upperOp == "==") {
    comparisonOp = CypherComparisonOp::EQUALS;
  } else if (upperOp == "!=" || upperOp == "<>") {
    comparisonOp = CypherComparisonOp::NOT_EQUALS;
  } else if (upperOp == "<") {
    comparisonOp = CypherComparisonOp::LESS_THAN;
  } else if (upperOp == "<=") {
    comparisonOp = CypherComparisonOp::LESS_THAN_OR_EQUAL;
  } else if (upperOp == ">") {
    comparisonOp = CypherComparisonOp::GREATER_THAN;
  } else if (upperOp == ">=") {
    comparisonOp = CypherComparisonOp::GREATER_THAN_OR_EQUAL;
  } else if (upperOp == "CONTAINS") {
    comparisonOp = CypherComparisonOp::CONTAINS;
  }

  if (!rightProperty.empty())
    return CypherWhereClause::makePropertyComparison(
        variable, property, comparisonOp, value, rightProperty);
  return CypherWhereClause::makeComparison(variable, property, comparisonOp,
                                           value);
}

std::unique_ptr<CypherReturnItem>
CypherParser::parseReturnItem(std::vector<std::string> &tokens, size_t &pos) {
  if (!hasMore(tokens, pos)) {
    setError(CypherErrorCode::SYNTAX_ERROR, "Expected return item", 0, 0);
    return nullptr;
  }

  std::string token = consume(tokens, pos);
  std::string alias;

  std::string upperToken = token;
  std::transform(upperToken.begin(), upperToken.end(), upperToken.begin(),
                 ::toupper);

  if (upperToken == "COUNT") {
    if (!hasMore(tokens, pos) || peek(tokens, pos) != "(") {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected '(' after COUNT", 0, 0);
      return nullptr;
    }
    consume(tokens, pos); // '('

    bool distinct = false;
    if (hasMore(tokens, pos)) {
      std::string maybeDistinct = peek(tokens, pos);
      std::string upperDistinct = maybeDistinct;
      std::transform(upperDistinct.begin(), upperDistinct.end(),
                     upperDistinct.begin(), ::toupper);
      if (upperDistinct == "DISTINCT") {
        distinct = true;
        consume(tokens, pos);
      }
    }

    if (!hasMore(tokens, pos)) {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected COUNT argument", 0, 0);
      return nullptr;
    }

    std::string arg = consume(tokens, pos);
    if (hasMore(tokens, pos) && peek(tokens, pos) == ".") {
      consume(tokens, pos); // '.'
      if (!hasMore(tokens, pos)) {
        setError(CypherErrorCode::SYNTAX_ERROR,
                 "Expected property after '.' in COUNT argument", 0, 0);
        return nullptr;
      }
      arg += ".";
      arg += consume(tokens, pos);
    }

    if (!hasMore(tokens, pos) || peek(tokens, pos) != ")") {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected ')' after COUNT", 0, 0);
      return nullptr;
    }
    consume(tokens, pos); // ')'

    if (hasMore(tokens, pos) &&
        (peek(tokens, pos) == "AS" || peek(tokens, pos) == "as")) {
      consume(tokens, pos);
      if (hasMore(tokens, pos)) {
        alias = consume(tokens, pos);
      }
    }

    return CypherReturnItem::makeCount(arg, distinct, alias);
  }

  // Handle property access (e.g., n.id -> consume . and property)
  std::string variable = token;
  if (hasMore(tokens, pos) && peek(tokens, pos) == ".") {
    consume(tokens, pos); // consume '.'
    if (hasMore(tokens, pos)) {
      std::string prop = consume(tokens, pos);
      variable += "." + prop; // Combine into single variable
    }
  }

  if (hasMore(tokens, pos) &&
      (peek(tokens, pos) == "AS" || peek(tokens, pos) == "as")) {
    consume(tokens, pos);
    if (hasMore(tokens, pos)) {
      alias = consume(tokens, pos);
    }
  }

  return std::make_unique<CypherReturnItem>(variable, alias);
}

std::unique_ptr<CypherOrderBy>
CypherParser::parseOrderBy(std::vector<std::string> &tokens, size_t &pos) {
  if (!hasMore(tokens, pos)) {
    setError(CypherErrorCode::SYNTAX_ERROR, "Expected variable for ORDER BY", 0,
             0);
    return nullptr;
  }

  std::string variable = consume(tokens, pos);
  std::string property;

  // Handle property access (e.g., n.id -> consume . and property)
  if (hasMore(tokens, pos) && peek(tokens, pos) == ".") {
    consume(tokens, pos); // consume '.'
    if (hasMore(tokens, pos)) {
      property = consume(tokens, pos); // consume property name
    }
  }

  CypherOrderBy::Direction dir = CypherOrderBy::Direction::ASC;

  if (hasMore(tokens, pos)) {
    std::string next = peek(tokens, pos);
    std::string upperNext = next;
    std::transform(upperNext.begin(), upperNext.end(), upperNext.begin(),
                   ::toupper);
    if (upperNext == "DESC") {
      dir = CypherOrderBy::Direction::DESC;
      consume(tokens, pos);
    } else if (upperNext == "ASC") {
      consume(tokens, pos);
    }
  }

  return std::make_unique<CypherOrderBy>(variable, property, dir);
}

bool CypherParser::hasMore(const std::vector<std::string> &tokens, size_t pos) {
  return pos < tokens.size();
}

const std::string &CypherParser::peek(const std::vector<std::string> &tokens,
                                      size_t pos) {
  static const std::string empty = "";
  if (pos < tokens.size()) {
    return tokens[pos];
  }
  return empty;
}

const std::string &CypherParser::consume(std::vector<std::string> &tokens,
                                         size_t &pos) {
  static const std::string empty = "";
  if (pos < tokens.size()) {
    return tokens[pos++];
  }
  return empty;
}

std::string
CypherParser::substituteParameter(const std::string &name,
                                  const CypherQueryParameters &params) {
  auto it = params.find(name);
  if (it == params.end()) {
    setError(CypherErrorCode::INVALID_PARAMETER, "Unknown parameter: $" + name,
             0, 0);
    lastError_.suggestion =
        "Pass it via --param " + name + "=<value> (repeatable).";
    return "";
  }
  return it->second;
}

} // namespace pdg
