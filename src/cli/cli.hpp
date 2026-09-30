#ifndef RESOURCE_ENVELOPE_CLI_HPP
#define RESOURCE_ENVELOPE_CLI_HPP

#include <string>
#include <vector>

#include "resource_envelope/export.hpp"

namespace resource_envelope {
namespace cli {

// Runs one command line and returns the process exit code. Every failure is reported
// on standard output as a single canonical JSON object, so the tool composes in a
// pipeline without a caller having to parse human prose.
RESOURCE_ENVELOPE_API int run(const std::vector<std::string>& arguments);

}  // namespace cli
}  // namespace resource_envelope

#endif  // RESOURCE_ENVELOPE_CLI_HPP
