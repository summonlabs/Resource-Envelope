#include "options.hpp"

#include "resource_envelope/status.hpp"

namespace resource_envelope {
namespace cli {
namespace {

const std::vector<std::string>& known_options() {
  static const std::vector<std::string> kKnown = {
      "store",
      "id",
      "dimension",
      "precedence",
      "scope",
      "identity",
      "identity-generation",
      "identity-digest",
      "policy-digest",
      "service-class-digest",
      "service-class-state",
      "policy-state",
      "site",
      "authority",
      "actor",
      "reason",
      "idempotency-key",
      "at",
      "effective-from",
      "effective-until",
      "merge-policy",
      "precedence",
      "class",
      "accepted-classes",
      "value",
      "delta",
      "observed-at",
      "facility",
      "composition",
      "expected-revision",
      "expected-digest",
      "decision",
      "limit",
      "principal",
      "principal-generation",
      "source",
      "request-id",
      "require-identity-binding",
      "limit",
      "json",
      "help",
  };
  return kKnown;
}

bool is_known(const std::string& name) {
  for (const std::string& candidate : known_options()) {
    if (candidate == name) return true;
  }
  return false;
}

const char* kUsage =
    "resource-envelope <command> --store <directory> [options]\n"
    "\n"
    "Commands:\n"
    "  declare              declare envelope revision 1\n"
    "  revise               declare the next revision of an existing envelope\n"
    "  show                 print the current revision of one envelope\n"
    "  show-revision        print one retained revision of an envelope\n"
    "  list                 print every current envelope\n"
    "  evaluate             evaluate a request without recording a decision\n"
    "  authorize            evaluate a request and record the decision\n"
    "  verify               report whether a recorded decision is still authoritative\n"
    "  decision             print one recorded decision\n"
    "  decisions            print recorded decisions, newest first\n"
    "  record-usage         append committed usage deltas\n"
    "  record-observation   record observed measurements\n"
    "  committed            print committed usage totals\n"
    "  observations         print recorded observations\n"
    "  history              print the durable history of one envelope\n"
    "  retire               retire an envelope so it stops being authoritative\n"
    "  statistics           print store statistics\n"
    "  compact              rewrite the store into a fresh snapshot\n"
    "  schema               print the canonical wire format and digest domains\n"
    "\n"
    "Common options:\n"
    "  --store <dir>                store root directory (required)\n"
    "  --at <rfc3339>               authoritative clock reading for the operation\n"
    "  --idempotency-key <key>      replay key for a mutating operation\n"
    "  --json                       print the canonical JSON form (default)\n"
    "\n"
    "Envelope options:\n"
    "  --scope <tenant|service|facility>\n"
    "  --identity <id>              bound identity identifier\n"
    "  --identity-generation <n>    bound identity generation\n"
    "  --identity-digest <hex>      digest of the bound identity record\n"
    "  --policy-digest <hex>        digest of the authorising policy revision\n"
    "  --service-class-digest <hex> digest of the referenced service class revision\n"
    "  --site <id>                  facility site scope token\n"
    "  --require-identity-binding <true|false>\n"
    "                               require the caller to confirm the bound identity\n"
    "  --authority <token>          declaring authority\n"
    "  --actor <token>              declaring actor\n"
    "  --reason <text>              declaration reason\n"
    "  --id <id>                    envelope identifier (declare and revise)\n"
    "  --effective-from <rfc3339>   window start\n"
    "  --effective-until <rfc3339>  window end\n"
    "  --precedence <list>          comma-separated dimension precedence\n"
    "  --merge-policy <policy>      strict or tightest-wins\n"
    "\n"
    "Dimension options (repeatable):\n"
    "  --dimension <kind>:<hard-limit>[:<reserved>[:<quantum>[:<class>[:<indexing>]]]]\n"
    "  --dimension <kind>            for a request: <kind>:<quantity>[:<principals>[:<class>]]\n"
    "\n"
    "Request options:\n"
    "  --class <token>              requested compatibility class\n"
    "  --accepted-classes <list>    comma-separated acceptable classes\n"
    "  --facility <id>              facility envelope for conjunctive composition\n"
    "  --composition <mode>         principal-only or conjunctive\n"
    "  --expected-revision <n>      fence the evaluation to one revision\n"
    "  --expected-digest <hex>      fence the evaluation to one digest\n"
    "  --decision <id>              decision identifier\n"
    "  --principal <token>          principal for usage and observations\n"
    "\n"
    "Exit codes: 0 success, 1 refused or failed operation, 2 usage error.\n"
    "A refused evaluation is a successful run: it exits 0 and reports its outcome,\n"
    "reason and blocking dimension in the JSON object.\n";

}  // namespace

const char* usage_text() { return kUsage; }

Result<Options> parse_options(const std::vector<std::string>& arguments) {
  Options options;
  std::size_t index = 0;
  while (index < arguments.size()) {
    const std::string& token = arguments[index];
    if (token.size() >= 2U && token[0] == '-' && token[1] == '-') {
      const std::string body = token.substr(2U);
      const std::size_t equals = body.find('=');
      if (equals != std::string::npos) {
        const std::string name = body.substr(0, equals);
        if (!is_known(name)) {
          return Status(StatusCode::InvalidArgument, std::string("unrecognised option --") + name);
        }
        options.values.emplace_back(name, body.substr(equals + 1U));
        ++index;
        continue;
      }
      if (!is_known(body)) {
        return Status(StatusCode::InvalidArgument, std::string("unrecognised option --") + body);
      }
      if (body == "json" || body == "help") {
        options.values.emplace_back(body, "1");
        ++index;
        continue;
      }
      if (index + 1U >= arguments.size()) {
        return Status(StatusCode::InvalidArgument, std::string("--") + body + " requires a value");
      }
      const std::string value = arguments[index + 1U];
      if (body == "dimension") {
        options.dimensions.push_back(value);
      } else if (body == "precedence") {
        options.precedence.push_back(value);
      } else {
        options.values.emplace_back(body, value);
      }
      index += 2U;
      continue;
    }
    options.positional.push_back(token);
    ++index;
  }
  return options;
}

}  // namespace cli
}  // namespace resource_envelope
