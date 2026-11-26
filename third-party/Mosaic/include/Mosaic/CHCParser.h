#pragma once

#include <filesystem>
#include <string>

#include <z3++.h>

namespace lotus::mosaic {

class CHCParser {
public:
  explicit CHCParser(z3::context &context) : m_context(context) {}

  z3::expr parseFile(z3::fixedpoint &fixedpoint,
                     const std::filesystem::path &path) const;
  z3::expr parseString(z3::fixedpoint &fixedpoint, std::string input,
                       const std::string &source_name = "<string>") const;

private:
  z3::context &m_context;
};

} // namespace lotus::mosaic
