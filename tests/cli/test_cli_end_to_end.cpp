// End-to-end validation of the command line tool against a real store directory.
//
// The library suites call the API directly, so the one thing they cannot cover is the command line
// surface: which options a command accepts, what shape each command's --dimension takes, the unit
// a quantity is expressed in, and the JSON the tool prints. This suite runs the built tool as a
// child process and asserts the observable answers, which is what makes the hand validation
// recorded in the README reproducible rather than remembered.
//
// Like the recovery suite, this is Windows-specific: it uses _popen and the platform's quoting.

#include "support/testing.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace {

#ifndef RE_CLI_PATH
#error "RE_CLI_PATH must name the command line tool built from this repository"
#endif

// A scratch store directory inside the current test working directory, removed on scope exit even
// when a check fails.
class ScratchStore {
 public:
  explicit ScratchStore(const std::string& name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::current_path() / (name + "-" + std::to_string(stamp));
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  ~ScratchStore() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  ScratchStore(const ScratchStore&) = delete;
  ScratchStore& operator=(const ScratchStore&) = delete;
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::string quote(const std::string& text) { return std::string("\"") + text + "\""; }

// Runs the tool with the supplied argument line and returns everything it printed, standard error
// included, so a failure message shows what the tool actually said.
// The tool's path arrives as a compile definition. The build system quotes it when the path
// contains a space, so any surrounding quotes are removed here and exactly one pair is added by
// the caller: one quoting rule rather than two.
std::string tool_path() {
  std::string path = RE_CLI_PATH;
  if (path.size() >= 2U && path.front() == '"' && path.back() == '"') {
    path = path.substr(1U, path.size() - 2U);
  }
  return path;
}

std::string run(const std::string& arguments) {
  // cmd.exe strips the outer quotes of a /s /c command line, which is what keeps a program path
  // containing a space one token no matter how many quoted arguments follow it.
  const std::string command = "cmd /d /s /c \"\"" + tool_path() + "\" " + arguments + "\" 2>&1";
  FILE* pipe = _popen(command.c_str(), "r");
  // A tool that cannot be started is reported as an empty answer, which fails the first check of
  // every case rather than silently passing.
  if (pipe == nullptr) return std::string();
  std::string output;
  char buffer[512];
  while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr) {
    output += buffer;
  }
  _pclose(pipe);
  while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
  return output;
}

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

// Reads a string field out of the tool's JSON output. The tool prints canonical JSON, so a field
// is exactly "name":"value"; this is deliberately not a parser, because a suite that parsed the
// output with a shared encoder could not notice the encoder changing the output.
std::string string_field(const std::string& json, const std::string& name) {
  const std::string needle = std::string("\"") + name + "\":\"";
  const std::size_t start = json.find(needle);
  if (start == std::string::npos) return std::string();
  const std::size_t begin = start + needle.size();
  const std::size_t end = json.find('"', begin);
  if (end == std::string::npos) return std::string();
  return json.substr(begin, end - begin);
}

const char* const kIdentityDigest =
    "a1b2c3d4e5f60718293a4b5c6d7e8f90112233445566778899aabbccddeeff00";
const char* const kAt = "2026-01-01T00:00:00Z";

}  // namespace

RE_TEST(the_command_line_tool_reports_the_arithmetic_it_was_given) {
  // A quantity on the command line is a quantity in its dimension's own unit everywhere it
  // appears: a declared limit, a requested quantity, an observed value and a usage delta. This
  // case fails if any of them is read as a raw nanounit count, which is how a usage record once
  // became a hundred-millionth of what the operator typed.
  const ScratchStore scratch("re-cli-e2e");
  const std::string store = quote(scratch.path().string());

  const std::string declared = run("declare --store " + store + " --id tenant-a-envelope" +
                                   " --scope tenant --identity tenant-a --identity-generation 3" +
                                   " --identity-digest " + kIdentityDigest +
                                   " --dimension power-draw-watts:100:10" +
                                   " --idempotency-key declare-1 --at " + kAt);
  RE_CHECK(contains(declared, "\"ok\":true"));
  RE_CHECK(contains(declared, "\"operation\":\"declare\""));
  RE_CHECK(contains(declared, "\"revision\":1"));

  const std::string shown = run("show --store " + store + " --id tenant-a-envelope");
  RE_CHECK(contains(shown, "\"hard_limit\":\"100\""));
  RE_CHECK(contains(shown, "\"reserved\":\"10\""));

  const std::string usage = run("record-usage --store " + store +
                                " --id tenant-a-envelope"
                                " --dimension power-draw-watts:30:principal-a"
                                " --idempotency-key usage-1 --at " + std::string(kAt));
  RE_CHECK(contains(usage, "\"entries_appended\":1"));
  const std::string committed = run("committed --store " + store + " --id tenant-a-envelope");
  // 30 W is thirty thousand million nanounits. A raw 30 here is the defect this pins.
  RE_CHECK(contains(committed, "\"value\":30000000000"));
  RE_CHECK(contains(committed, "\"formatted\":\"30\""));

  // A retry is a replay, and a release is subtracted.
  const std::string replay = run("record-usage --store " + store +
                                 " --id tenant-a-envelope"
                                 " --dimension power-draw-watts:30:principal-a"
                                 " --idempotency-key usage-1 --at " + std::string(kAt));
  RE_CHECK(contains(replay, "\"entries_appended\":0"));
  RE_CHECK(contains(replay, "\"entries_replayed\":1"));
  const std::string release = run("record-usage --store " + store +
                                  " --id tenant-a-envelope"
                                  " --dimension power-draw-watts:-5:principal-a"
                                  " --idempotency-key usage-2 --at " + std::string(kAt));
  RE_CHECK(contains(release, "\"entries_appended\":1"));
  const std::string after_release = run("committed --store " + store + " --id tenant-a-envelope");
  RE_CHECK(contains(after_release, "\"value\":25000000000"));

  // An observation is reported and never consumed: the residual is 100 - 10 - 25 = 65, and a
  // request of 50 W fits inside it with 15 W of headroom.
  const std::string observation = run("record-observation --store " + store +
                                      " --id tenant-a-envelope"
                                      " --dimension power-draw-watts:90:principal-a"
                                      " --idempotency-key observation-1 --at " + std::string(kAt));
  RE_CHECK(contains(observation, "\"stored\":1"));
  const std::string observation_replay = run("record-observation --store " + store +
                                             " --id tenant-a-envelope"
                                             " --dimension power-draw-watts:90:principal-a"
                                             " --idempotency-key observation-1 --at " +
                                             std::string(kAt));
  RE_CHECK(contains(observation_replay, "\"stored\":0"));
  RE_CHECK(contains(observation_replay, "\"replayed\":1"));

  const std::string evaluated = run("evaluate --store " + store +
                                    " --scope tenant --identity tenant-a --identity-generation 3"
                                    " --identity-digest " + kIdentityDigest +
                                    " --dimension power-draw-watts:50 --at " + kAt);
  RE_CHECK(contains(evaluated, "\"outcome\":\"Granted\""));
  RE_CHECK(contains(evaluated, "\"residual\":\"65\""));
  RE_CHECK(contains(evaluated, "\"committed\":\"25\""));
  RE_CHECK(contains(evaluated, "\"headroom_after\":\"15\""));

  // A binding that is not confirmed is refused at the generation stage, and the refusal names it.
  const std::string unbound = run("evaluate --store " + store +
                                  " --scope tenant --identity tenant-a --identity-generation 3"
                                  " --dimension power-draw-watts:50 --at " + kAt);
  RE_CHECK(contains(unbound, "\"outcome\":\"Denied\""));
  RE_CHECK(contains(unbound, "\"reason_name\":\"IdentityBindingMismatch\""));
  RE_CHECK(contains(unbound, "\"stage\":\"GenerationBinding\""));
}

RE_TEST(the_command_line_tool_records_and_verifies_a_decision) {
  const ScratchStore scratch("re-cli-decision");
  const std::string store = quote(scratch.path().string());
  RE_CHECK(contains(run("declare --store " + store + " --id tenant-a-envelope" +
                        " --scope tenant --identity tenant-a --identity-generation 3" +
                        " --identity-digest " + kIdentityDigest +
                        " --dimension power-draw-watts:100:10" +
                        " --idempotency-key declare-1 --at " + kAt),
                    "\"ok\":true"));
  RE_CHECK(contains(run("record-usage --store " + store + " --id tenant-a-envelope" +
                        " --dimension power-draw-watts:30:principal-a" +
                        " --idempotency-key usage-1 --at " + kAt),
                    "\"entries_appended\":1"));

  const std::string authorized = run("authorize --store " + store +
                                     " --scope tenant --identity tenant-a --identity-generation 3" +
                                     " --identity-digest " + kIdentityDigest +
                                     " --dimension power-draw-watts:50" +
                                     " --idempotency-key authorize-1 --at " + kAt);
  RE_CHECK(contains(authorized, "\"operation\":\"authorize\""));
  RE_CHECK(contains(authorized, "\"outcome\":\"Granted\""));
  RE_CHECK(contains(authorized, "\"identity_checked\":true"));
  RE_CHECK(contains(authorized, "\"identity_generation\":3"));
  const std::string decision_id = string_field(authorized, "decision_id");
  RE_CHECK(!decision_id.empty());

  // The recorded decision is readable, it reports the digest that identifies it, and verification
  // of it succeeds while nothing has moved.
  const std::string decision = run("decision --store " + store + " --decision " + decision_id);
  RE_CHECK(contains(decision, "\"decision_id\":\"" + decision_id + "\""));
  RE_CHECK(contains(decision, "\"outcome\":\"Granted\""));
  const std::string digest = string_field(decision, "decision_digest");
  RE_CHECK(!digest.empty());
  RE_CHECK(digest != "unknown");
  const std::string verified = run("verify --store " + store + " --decision " + decision_id);
  RE_CHECK(contains(verified, "\"valid\":true"));
  RE_CHECK(contains(verified, "\"authority_state\":\"Valid\""));

  // The durable history names the same decisions, with the same digests.
  const std::string history = run("history --store " + store + " --id tenant-a-envelope");
  RE_CHECK(contains(history, "\"kind\":\"decision\""));
  RE_CHECK(contains(history, digest));

  // Compaction, a retained revision and a retirement all keep the tool's answers truthful.
  const std::string compacted = run("compact --store " + store);
  RE_CHECK(contains(compacted, "\"compaction_count\":1"));
  const std::string usage_replay = run("record-usage --store " + store +
                                       " --id tenant-a-envelope" +
                                       " --dimension power-draw-watts:30:principal-a" +
                                       " --idempotency-key usage-1 --at " + kAt);
  RE_CHECK(contains(usage_replay, "\"entries_appended\":0"));
  RE_CHECK(contains(usage_replay, "\"entries_replayed\":1"));
  const std::string retained = run("show-revision --store " + store +
                                   " --id tenant-a-envelope --revision 1");
  RE_CHECK(contains(retained, "\"revision\":1"));
  RE_CHECK(contains(retained, "\"current\":true"));

  const std::string statistics = run("statistics --store " + store);
  RE_CHECK(contains(statistics, "\"envelope_count\":1"));
  RE_CHECK(contains(statistics, "\"decision_count\":1"));
  RE_CHECK(contains(statistics, "\"usage_entry_count\":1"));
  RE_CHECK(contains(statistics, "\"observation_count\":0"));

  RE_CHECK(contains(run("retire --store " + store + " --id tenant-a-envelope" +
                        " --reason decommissioned --request-id 3 --at " + kAt),
                    "\"superseded_revision\":1"));
  // A retried authorisation is answered from its record after the retirement, and a new request
  // for the retired scope finds no authority.
  const std::string replay = run("authorize --store " + store +
                                 " --scope tenant --identity tenant-a --identity-generation 3" +
                                 " --identity-digest " + kIdentityDigest +
                                 " --dimension power-draw-watts:50" +
                                 " --idempotency-key authorize-1 --at " + kAt);
  RE_CHECK(contains(replay, "\"replayed\":true"));
  RE_CHECK(contains(replay, "\"outcome\":\"Granted\""));
  const std::string after_retirement = run("evaluate --store " + store +
                                           " --scope tenant --identity tenant-a" +
                                           " --identity-generation 3" +
                                           " --identity-digest " + kIdentityDigest +
                                           " --dimension power-draw-watts:50 --at " + kAt);
  RE_CHECK(contains(after_retirement, "\"outcome\":\"Denied\""));
  RE_CHECK(contains(after_retirement, "\"reason_name\":\"EnvelopeNotFound\""));
  RE_CHECK(contains(run("list --store " + store), "\"count\":0"));
}

RE_TEST(the_command_line_tool_refuses_a_malformed_quantity_and_prints_its_schema) {
  const ScratchStore scratch("re-cli-schema");
  const std::string store = quote(scratch.path().string());
  RE_CHECK(contains(run("declare --store " + store + " --id tenant-a-envelope" +
                        " --scope tenant --identity tenant-a --identity-generation 3" +
                        " --require-identity-binding false" +
                        " --dimension power-draw-watts:100:10" +
                        " --idempotency-key declare-1 --at " + kAt),
                    "\"ok\":true"));
  // A quantity the runtime cannot read is refused, and it is refused before anything is written.
  const std::string refused = run("record-usage --store " + store + " --id tenant-a-envelope" +
                                  " --dimension power-draw-watts:1e9" +
                                  " --idempotency-key usage-1 --at " + kAt);
  RE_CHECK(contains(refused, "\"ok\":false"));
  RE_CHECK(contains(refused, "not a canonical decimal quantity"));
  RE_CHECK(contains(run("committed --store " + store + " --id tenant-a-envelope"), "\"count\":0"));
  // The schema documents the vocabulary the tool accepts and prints.
  const std::string schema = run("schema");
  RE_CHECK(contains(schema, "envelope-content"));
  RE_CHECK(contains(schema, "store-state"));
  RE_CHECK(contains(schema, "power-draw-watts"));
  RE_CHECK(contains(schema, "IdentityBindingMismatch"));
  // An unknown option is a usage error rather than something silently ignored.
  const std::string unknown = run("statistics --store " + store + " --nonsense 1");
  RE_CHECK(contains(unknown, "unrecognised option"));
}
