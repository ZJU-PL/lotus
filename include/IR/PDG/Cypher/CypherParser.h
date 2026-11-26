#pragma once

#include "IR/PDG/Cypher/CypherAST.h"

namespace pdg {

/**
 * @brief Cypher query parser with comprehensive error handling
 */
class CypherParser {
public:
  CypherParser() = default;

  std::unique_ptr<CypherQuery> parse(const std::string &query);
  std::unique_ptr<CypherQuery> parse(const std::string &query,
                                     const CypherQueryParameters &params);

  const CypherError &getLastError() const { return lastError_; }
  bool hasError() const { return lastError_.code != CypherErrorCode::SUCCESS; }

  // Utility methods
  static std::string escapeString(const std::string &s);
  static std::string unescapeString(const std::string &s);
  static bool isValidIdentifier(const std::string &s);
  static bool isKeyword(const std::string &s);

private:
  CypherError lastError_;
  const CypherQueryParameters *activeParams_ = nullptr;

  void setError(CypherErrorCode code, const std::string &message, int line = 0,
                int col = 0) {
    lastError_ = CypherError(code, message, line, col);
  }

  void clearError() { lastError_ = CypherError(); }

  // Parsing helpers
  void trim(std::string &s);
  bool isAlpha(char c);
  bool isDigit(char c);
  bool isAlphaNumeric(char c);

  // Tokenization
  std::vector<std::string> tokenize(const std::string &s);

  // Parsing functions
  std::unique_ptr<CypherQuery> parseQuery(std::vector<std::string> &tokens,
                                          size_t &pos,
                                          const CypherQueryParameters &params);
  std::unique_ptr<CypherPatternElement>
  parsePattern(std::vector<std::string> &tokens, size_t &pos);
  std::unique_ptr<CypherNodePattern>
  parseNodePattern(std::vector<std::string> &tokens, size_t &pos);
  std::unique_ptr<CypherRelationshipPattern>
  parseRelationshipPattern(std::vector<std::string> &tokens, size_t &pos);
  std::unique_ptr<CypherWhereClause>
  parseWhereClause(std::vector<std::string> &tokens, size_t &pos);
  std::unique_ptr<CypherWhereClause>
  parseBooleanExpression(std::vector<std::string> &tokens, size_t &pos);
  std::unique_ptr<CypherWhereClause>
  parseOrExpression(std::vector<std::string> &tokens, size_t &pos);
  std::unique_ptr<CypherWhereClause>
  parseAndExpression(std::vector<std::string> &tokens, size_t &pos);
  std::unique_ptr<CypherWhereClause>
  parseUnaryExpression(std::vector<std::string> &tokens, size_t &pos);
  std::unique_ptr<CypherWhereClause>
  parseComparison(std::vector<std::string> &tokens, size_t &pos);
  std::unique_ptr<CypherReturnItem>
  parseReturnItem(std::vector<std::string> &tokens, size_t &pos);
  std::unique_ptr<CypherOrderBy> parseOrderBy(std::vector<std::string> &tokens,
                                              size_t &pos);

  // Helper methods
  bool hasMore(const std::vector<std::string> &tokens, size_t pos);
  const std::string &peek(const std::vector<std::string> &tokens, size_t pos);
  const std::string &consume(std::vector<std::string> &tokens, size_t &pos);
  bool matchToken(const std::string &expected, std::vector<std::string> &tokens,
                  size_t &pos);

  // Parameter substitution
  std::string substituteParameter(const std::string &name,
                                  const CypherQueryParameters &params);
};

} // namespace pdg
