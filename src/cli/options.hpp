#ifndef RESOURCE_ENVELOPE_CLI_OPTIONS_HPP
#define RESOURCE_ENVELOPE_CLI_OPTIONS_HPP

#include <string>
#include <vector>

#include "resource_envelope/result.hpp"

namespace resource_envelope {
namespace cli {

// Parsed command line. Options are held as ordered pairs so that a repeated option
// keeps its declared order, which is what makes a dimension list reproducible.
struct Options {
  std::vector<std::string> positional;
  std::vector<std::pair<std::string, std::string>> values;
  std::vector<std::string> dimensions;
  std::vector<std::string> precedence;

  [[nodiscard]] bool has(const std::string& key) const {
    for (const auto& pair : values) {
      if (pair.first == key) return true;
    }
    return false;
  }
  [[nodiscard]] std::string get(const std::string& key, const std::string& fallback = std::string()) const {
    for (auto iterator = values.rbegin(); iterator != values.rend(); ++iterator) {
      if (iterator->first == key) return iterator->second;
    }
    return fallback;
  }
};

// Refuses an unknown option or a missing value instead of silently ignoring it, so a
// mistyped command can never evaluate a different request than the one intended.
RESOURCE_ENVELOPE_API Result<Options> parse_options(const std::vector<std::string>& arguments);

// The exact usage text. It is printed for --help and for any usage failure.
RESOURCE_ENVELOPE_API const char* usage_text();

}  // namespace cli
}  // namespace resource_envelope

#endif  // RESOURCE_ENVELOPE_CLI_OPTIONS_HPP
