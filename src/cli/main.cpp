#include "cli.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "json.hpp"
#include "options.hpp"

#include "resource_envelope/canonical.hpp"
#include "resource_envelope/service.hpp"
#include "resource_envelope/text.hpp"
#include "resource_envelope/time.hpp"

namespace resource_envelope {
namespace cli {
namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitFailure = 1;

// ---------------------------------------------------------------------------
// Field parsing
// ---------------------------------------------------------------------------
Result<std::uint64_t> parse_unsigned(const std::string& text, const char* what) {
  if (text.empty() || text.size() > 18U) {
    return Status(StatusCode::OutOfRange, std::string("the ") + what + " is not a decimal integer");
  }
  std::uint64_t value = 0U;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return Status(StatusCode::OutOfRange, std::string("the ") + what + " is not a decimal integer");
    }
    value = value * 10U + static_cast<std::uint64_t>(character - '0');
  }
  return value;
}

// A signed canonical decimal quantity: an optional sign in front of the same form a declared
// limit and a requested quantity take. A usage delta is a quantity in its dimension's own unit,
// exactly as every other quantity on the command line is, and it is signed because a commitment
// can be released as well as taken.
Result<std::int64_t> parse_signed_quantity(const std::string& text, const char* what) {
  if (text.empty()) return Status(StatusCode::OutOfRange, std::string("the ") + what + " is empty");
  const bool negative = text[0] == '-';
  const std::string magnitude = (negative || text[0] == '+') ? text.substr(1U) : text;
  Nanounits quantity = 0U;
  if (magnitude.empty() || !parse_quantity(magnitude, quantity)) {
    return Status(StatusCode::InvalidArgument,
                  std::string("the ") + what + " is not a canonical decimal quantity");
  }
  if (quantity > static_cast<Nanounits>(std::numeric_limits<std::int64_t>::max())) {
    return Status(StatusCode::OutOfRange, std::string("the ") + what + " exceeds the representable range");
  }
  const std::int64_t value = static_cast<std::int64_t>(quantity);
  return negative ? -value : value;
}

Result<Digest> parse_digest(const std::string& text, const char* what) {
  if (text.empty() || text == "unknown") return Digest::unknown();
  Digest::Value value{};
  if (!from_hex(text, value)) {
    return Status(StatusCode::InvalidDigest,
                  std::string("the ") + what + " is not 64 lowercase hexadecimal characters");
  }
  return Digest(value);
}

Result<Timestamp> parse_time(const std::string& text, const char* what) {
  if (text.empty()) return Status(StatusCode::InvalidArgument, std::string("the ") + what + " is required");
  Timestamp value = 0;
  if (!parse_timestamp(text, value)) {
    return Status(StatusCode::InvalidArgument,
                  std::string("the ") + what + " is not an RFC 3339 UTC timestamp");
  }
  return value;
}

Result<DimensionKind> parse_kind(const std::string& text) {
  DimensionKind kind = DimensionKind::CountRackPositions;
  if (!dimension_kind_from_string(text, kind)) {
    return Status(StatusCode::InvalidEnumValue, std::string("unknown dimension kind: ") + text);
  }
  return kind;
}

std::vector<std::string> split_colons(const std::string& text) {
  std::vector<std::string> fields;
  std::string current;
  for (const char character : text) {
    if (character == ':') {
      fields.push_back(trim(current));
      current.clear();
      continue;
    }
    current.push_back(character);
  }
  fields.push_back(trim(current));
  return fields;
}

// --dimension for a declaration:
//   <kind>:<hard-limit|unknown>[:<reserved>[:<quantum>[:<class>[:<indexing>]]]]
Result<DimensionSpec> parse_spec(const std::string& text) {
  const std::vector<std::string> fields = split_colons(text);
  if (fields.empty() || fields[0].empty()) {
    return Status(StatusCode::InvalidArgument, "a dimension specification needs a dimension kind");
  }
  const Result<DimensionKind> kind = parse_kind(fields[0]);
  if (!kind.ok()) return kind.status();
  DimensionSpec spec;
  spec.kind = kind.value();
  if (fields.size() == 1U) {
    return Status(StatusCode::InvalidArgument,
                  "a declared dimension needs a limit; write unknown to declare the bound as unknown");
  }
  if (fields[1].empty()) {
    return Status(StatusCode::InvalidArgument, "the declared limit is empty; write unknown instead");
  }
  if (fields[1] != "unknown") {
    Nanounits limit = 0U;
    if (!parse_quantity(fields[1], limit)) {
      return Status(StatusCode::InvalidArgument, "the declared limit is not a canonical decimal quantity");
    }
    spec.hard_limit = limit;
  }
  if (fields.size() >= 3U && !fields[2].empty()) {
    Nanounits reserved = 0U;
    if (!parse_quantity(fields[2], reserved)) {
      return Status(StatusCode::InvalidArgument,
                    "the declared reservation is not a canonical decimal quantity");
    }
    spec.reserved = reserved;
  }
  if (fields.size() >= 4U && !fields[3].empty()) {
    Nanounits quantum = 0U;
    if (!parse_quantity(fields[3], quantum)) {
      return Status(StatusCode::InvalidArgument, "the declared alignment is not a canonical decimal quantity");
    }
    spec.quantum = quantum;
  }
  if (fields.size() >= 5U) spec.compatibility_class = fields[4];
  if (fields.size() >= 6U && !fields[5].empty()) {
    if (fields[5] == "per-principal") {
      spec.indexing = DimensionIndexing::PerPrincipal;
    } else if (fields[5] == "aggregate") {
      spec.indexing = DimensionIndexing::Aggregate;
    } else {
      return Status(StatusCode::InvalidEnumValue,
                    "the declared indexing is neither aggregate nor per-principal");
    }
  }
  return spec;
}

// --dimension for a request: <kind>:<quantity>[:<principals>[:<class>[:<principal>]]]
Result<DimensionRequest> parse_request_dimension(const std::string& text) {
  const std::vector<std::string> fields = split_colons(text);
  if (fields.empty() || fields[0].empty()) {
    return Status(StatusCode::InvalidArgument, "a requested dimension needs a dimension kind");
  }
  const Result<DimensionKind> kind = parse_kind(fields[0]);
  if (!kind.ok()) return kind.status();
  DimensionRequest request;
  request.kind = kind.value();
  if (fields.size() >= 2U && !fields[1].empty()) {
    Nanounits quantity = 0U;
    if (!parse_quantity(fields[1], quantity)) {
      return Status(StatusCode::InvalidArgument,
                    "the requested quantity is not a canonical decimal quantity");
    }
    request.quantity = quantity;
  }
  if (fields.size() >= 3U && !fields[2].empty()) {
    const Result<std::uint64_t> principals = parse_unsigned(fields[2], "requested principal count");
    if (!principals.ok()) return principals.status();
    request.principals = static_cast<std::uint32_t>(principals.value());
  }
  if (fields.size() >= 4U) request.compatibility_class = fields[3];
  if (fields.size() >= 5U) request.principal = fields[4];
  return request;
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------
void write_outcome(JsonWriter& writer, Outcome outcome) {
  writer.string_field("outcome", to_string(outcome));
}

void write_failure(StatusCode code, const std::string& detail) {
  JsonWriter writer(std::cout);
  writer.begin_object();
  writer.bool_field("ok", false);
  writer.integer_field("code", static_cast<std::uint64_t>(code));
  writer.string_field("code_name", to_string(code));
  writer.string_field("detail", detail);
  writer.end_object();
  std::cout << '\n';
}

void write_dimension(JsonWriter& writer, const DimensionResult& item) {
  writer.begin_object();
  writer.string_field("dimension", to_string(item.kind));
  writer.string_field("unit", to_string(unit_of(item.kind)));
  const Cardinality cardinality = cardinality_of(item.kind);
  writer.string_field("cardinality", cardinality == Cardinality::Consumable
                                            ? "consumable"
                                            : (cardinality == Cardinality::Exclusive ? "exclusive"
                                                                                     : "non-consumable"));
  writer.string_field("outcome", to_string(item.outcome));
  writer.quantity_field("limit", item.limit);
  writer.string_field("reserved", format_quantity(item.reserved));
  writer.quantity_field("committed", item.committed);
  writer.quantity_field("effective_committed", item.effective_committed);
  writer.quantity_field("residual", item.residual);
  writer.bool_field("over_committed", item.over_committed);
  writer.string_field("over_committed_by", format_quantity(item.over_committed_by));
  writer.string_field("requested", format_quantity(item.requested));
  writer.integer_field("principals", item.principals);
  writer.quantity_field("headroom_after", item.headroom_after);
  writer.string_field("quantum", format_quantity(item.quantum));
  writer.quantity_field("quantized_quantity", item.quantized_quantity);
  writer.bool_field("observation_present", item.observation_present);
  writer.quantity_field("observed", item.observed);
  writer.string_field("required_class", item.required_class);
  writer.string_field("held_class", item.held_class);
  writer.integer_field("holders", item.holders);
  writer.quantity_field("required_threshold", item.required_threshold);
  writer.quantity_field("declared_threshold", item.declared_threshold);
  writer.quantity_field("shortfall", item.shortfall);
  writer.integer_field("desired_level", item.desired_level);
  writer.integer_field("declared_level", item.declared_level);
  if (item.operational_spare.has_value()) {
    writer.bool_field("operational_spare", *item.operational_spare);
  } else {
    writer.null_field("operational_spare");
  }
  writer.field_array_start("secondary");
  for (const StatusCode code : item.secondary) {
    writer.integer_element(static_cast<std::uint64_t>(code));
  }
  writer.end_array();
  writer.end_object();
}

void write_evaluation(JsonWriter& writer, const EvaluationResult& result) {
  write_outcome(writer, result.outcome);
  writer.integer_field("reason", static_cast<std::uint64_t>(result.reason));
  writer.string_field("reason_name", to_string(result.reason));
  writer.string_field("stage", to_string(result.stage));
  if (result.blocking_dimension.has_value()) {
    writer.string_field("blocking_dimension", to_string(*result.blocking_dimension));
  } else {
    writer.null_field("blocking_dimension");
  }
  writer.digest_field("decision_digest", result.decision_digest);
  writer.integer_field("decision_sequence", result.decision_sequence);
  writer.string_field("envelope_id", result.envelope_id);
  writer.integer_field("envelope_revision", result.envelope_revision);
  writer.digest_field("envelope_digest", result.envelope_digest);
  writer.integer_field("control_epoch", result.control_epoch);
  writer.digest_field("request_digest", result.request_digest);
  writer.digest_field("evidence_digest", result.evidence_digest);
  writer.field_array_start("dimensions");
  for (const DimensionResult& item : result.dimensions) write_dimension(writer, item);
  writer.end_array();
  writer.field_array_start("secondary");
  for (const StatusCode code : result.secondary) {
    writer.integer_element(static_cast<std::uint64_t>(code));
  }
  writer.end_array();
}

void write_envelope(JsonWriter& writer, const EnvelopeView& view) {
  const Envelope& envelope = view.envelope;
  writer.string_field("envelope_id", envelope.id);
  writer.integer_field("revision", envelope.revision);
  writer.string_field("kind", to_string(envelope.kind));
  writer.string_field("scope", to_string(envelope.scope.kind));
  writer.string_field("identity", envelope.scope.identity.id);
  writer.integer_field("identity_generation", envelope.scope.identity.generation);
  writer.string_field("site", envelope.site_id);
  writer.digest_field("identity_digest", envelope.identity_digest);
  writer.digest_field("service_class_digest", envelope.service_class_digest);
  writer.digest_field("policy_digest", envelope.policy_digest);
  if (envelope.window.effective_from.has_value()) {
    writer.string_field("effective_from", format_timestamp(*envelope.window.effective_from));
  } else {
    writer.null_field("effective_from");
  }
  if (envelope.window.effective_until.has_value()) {
    writer.string_field("effective_until", format_timestamp(*envelope.window.effective_until));
  } else {
    writer.null_field("effective_until");
  }
  writer.bool_field("requires_binding_confirmation", envelope.require_binding_confirmation);
  writer.string_field("merge_policy", to_string(envelope.merge_policy));
  writer.bool_field("current", view.current);
  writer.bool_field("retired", view.tombstoned);
  writer.integer_field("stored_sequence", view.stored_sequence);
  writer.digest_field("record_digest", view.record_digest);
  writer.digest_field("content_digest", view.content_digest);
  writer.string_field("authority", envelope.provenance.authority);
  writer.string_field("author", envelope.provenance.actor);
  writer.string_field("reason", envelope.provenance.reason);
  writer.string_field("declared_at", format_timestamp(envelope.provenance.declared_at));
  writer.field_array_start("precedence");
  for (const DimensionKind kind : envelope.precedence) writer.string_element(to_string(kind));
  writer.end_array();
  writer.field_array_start("lineage");
  for (const EnvelopeRef& reference : envelope.merged_from) {
    writer.begin_object();
    writer.string_field("envelope_id", reference.envelope_id);
    writer.integer_field("revision", reference.revision);
    writer.digest_field("digest", reference.digest);
    writer.end_object();
  }
  writer.end_array();
  writer.field_array_start("dimensions");
  for (const DimensionSpec& spec : envelope.dimensions) {
    writer.begin_object();
    writer.string_field("dimension", to_string(spec.kind));
    writer.string_field("unit", to_string(unit_of(spec.kind)));
    writer.quantity_field("hard_limit", spec.hard_limit);
    writer.string_field("reserved", format_quantity(spec.reserved));
    writer.string_field("quantum", format_quantity(spec.quantum));
    writer.string_field("compatibility_class", spec.compatibility_class);
    writer.string_field("indexing", to_string(spec.indexing));
    writer.end_object();
  }
  writer.end_array();
}

void write_record(JsonWriter& writer, const DecisionRecord& record) {
  writer.string_field("decision_id", record.decision_id);
  writer.string_field("idempotency_key", record.idempotency_key);
  write_outcome(writer, record.outcome);
  writer.integer_field("reason", static_cast<std::uint64_t>(record.reason));
  writer.string_field("reason_name", to_string(record.reason));
  writer.string_field("stage", to_string(record.stage));
  if (record.blocking_dimension.has_value()) {
    writer.string_field("blocking_dimension", to_string(*record.blocking_dimension));
  } else {
    writer.null_field("blocking_dimension");
  }
  writer.string_field("envelope_id", record.envelope_id);
  writer.integer_field("envelope_revision", record.envelope_revision);
  writer.digest_field("envelope_digest", record.envelope_digest);
  writer.integer_field("control_epoch", record.control_epoch);
  writer.digest_field("authority_state_digest", record.authority_state_digest);
  writer.string_field("scope", to_string(record.scope.kind));
  writer.string_field("identity", record.identity.id);
  writer.integer_field("identity_generation", record.identity.generation);
  writer.bool_field("identity_checked", record.identity_checked);
  writer.digest_field("identity_digest", record.identity_digest);
  writer.digest_field("service_class_digest", record.service_class_digest);
  writer.digest_field("policy_digest", record.policy_digest);
  writer.string_field("evaluated_at", format_timestamp(record.evaluated_at));
  writer.integer_field("sequence", record.sequence);
  writer.digest_field("request_digest", record.request_digest);
  writer.digest_field("evidence_digest", record.evidence_digest);
  writer.digest_field("grant_digest", record.grant_digest);
  writer.digest_field("decision_digest", record.decision_digest);
  writer.field_array_start("dimensions");
  for (const DimensionResult& item : record.dimensions) write_dimension(writer, item);
  writer.end_array();
  writer.field_array_start("secondary");
  for (const StatusCode code : record.secondary) {
    writer.integer_element(static_cast<std::uint64_t>(code));
  }
  writer.end_array();
}

// ---------------------------------------------------------------------------
// Request construction
// ---------------------------------------------------------------------------
Result<EnvelopeScope> parse_scope(const Options& options) {
  EnvelopeScope scope;
  const std::string kind = options.get("scope");
  if (!envelope_scope_kind_from_string(kind, scope.kind)) {
    return Status(StatusCode::InvalidArgument, "--scope must be tenant, service or facility");
  }
  scope.identity.id = options.get("identity");
  if (scope.identity.id.empty()) {
    return Status(StatusCode::InvalidArgument, "--identity is required");
  }
  const Result<std::uint64_t> generation =
      parse_unsigned(options.get("identity-generation", "0"), "identity generation");
  if (!generation.ok()) return generation.status();
  scope.identity.generation = generation.value();
  return scope;
}

Result<Envelope> build_envelope(const Options& options) {
  Envelope envelope;
  envelope.id = options.get("id");
  if (envelope.id.empty()) {
    return Status(StatusCode::InvalidArgument, "--id is required for a declaration or a revision");
  }
  const Result<EnvelopeScope> scope = parse_scope(options);
  if (!scope.ok()) return scope.status();
  envelope.scope = scope.value();
  envelope.site_id = options.get("site");
  envelope.provenance.authority = options.get("authority");
  envelope.provenance.actor = options.get("actor");
  envelope.provenance.reason = options.get("reason");

  const std::string identity_digest = options.get("identity-digest");
  if (!identity_digest.empty()) {
    const Result<Digest> digest = parse_digest(identity_digest, "identity digest");
    if (!digest.ok()) return digest.status();
    envelope.identity_digest = digest.value();
  }
  const std::string policy_digest = options.get("policy-digest");
  if (!policy_digest.empty()) {
    const Result<Digest> digest = parse_digest(policy_digest, "policy digest");
    if (!digest.ok()) return digest.status();
    envelope.policy_digest = digest.value();
  }
  const std::string service_digest = options.get("service-class-digest");
  if (!service_digest.empty()) {
    const Result<Digest> digest = parse_digest(service_digest, "service class digest");
    if (!digest.ok()) return digest.status();
    envelope.service_class_digest = digest.value();
  }

  if (options.has("effective-from")) {
    const Result<Timestamp> from = parse_time(options.get("effective-from"), "window start");
    if (!from.ok()) return from.status();
    envelope.window.effective_from = from.value();
  }
  if (options.has("effective-until")) {
    const Result<Timestamp> until = parse_time(options.get("effective-until"), "window end");
    if (!until.ok()) return until.status();
    envelope.window.effective_until = until.value();
  }
  if (options.has("require-identity-binding")) {
    const std::string value = options.get("require-identity-binding");
    if (value == "false") {
      envelope.require_binding_confirmation = false;
    } else if (value == "true") {
      envelope.require_binding_confirmation = true;
    } else {
      return Status(StatusCode::InvalidArgument, "--require-identity-binding must be true or false");
    }
  }
  if (options.has("merge-policy")) {
    if (!merge_policy_from_string(options.get("merge-policy"), envelope.merge_policy)) {
      return Status(StatusCode::InvalidArgument, "--merge-policy must be strict or tightest-wins");
    }
  }
  for (const std::string& entry : options.precedence) {
    for (const std::string& token : split_commas(entry)) {
      const Result<DimensionKind> kind = parse_kind(token);
      if (!kind.ok()) return kind.status();
      envelope.precedence.push_back(kind.value());
    }
  }
  for (const std::string& text : options.dimensions) {
    const Result<DimensionSpec> spec = parse_spec(text);
    if (!spec.ok()) return spec.status();
    envelope.dimensions.push_back(spec.value());
  }
  return envelope;
}

Result<Timestamp> parse_at(const Options& options) {
  if (!options.has("at")) {
    return Status(StatusCode::InvalidArgument,
                  "--at is required: an authoritative decision always names its clock reading");
  }
  return parse_time(options.get("at"), "evaluation timestamp");
}

Result<EvaluationRequest> build_request(const Options& options) {
  EvaluationRequest request;
  const Result<EnvelopeScope> scope = parse_scope(options);
  if (!scope.ok()) return scope.status();
  request.scope = scope.value();
  request.idempotency_key = options.get("idempotency-key");
  const Result<Timestamp> at = parse_at(options);
  if (!at.ok()) return at.status();
  request.at = at.value();

  if (options.has("expected-revision")) {
    const Result<std::uint64_t> revision =
        parse_unsigned(options.get("expected-revision"), "expected revision");
    if (!revision.ok()) return revision.status();
    request.expected_envelope_revision = revision.value();
  }
  if (options.has("expected-digest")) {
    const Result<Digest> digest = parse_digest(options.get("expected-digest"), "expected digest");
    if (!digest.ok()) return digest.status();
    request.expected_envelope_digest = digest.value();
  }

  if (options.has("identity-digest")) {
    request.identity.identity = request.scope.identity;
    const Result<Digest> digest = parse_digest(options.get("identity-digest"), "identity digest");
    if (!digest.ok()) return digest.status();
    request.identity.digest = digest.value();
  }
  if (options.has("service-class-state")) {
    const std::string state = options.get("service-class-state");
    if (state == "provided") {
      request.service_class.state = ResolutionState::Provided;
    } else if (state == "not-current") {
      request.service_class.state = ResolutionState::NotCurrent;
    } else if (state == "unavailable") {
      request.service_class.state = ResolutionState::Unavailable;
    } else {
      return Status(StatusCode::InvalidArgument,
                    "--service-class-state must be provided, not-current or unavailable");
    }
  }
  if (options.has("service-class-digest")) {
    const Result<Digest> digest =
        parse_digest(options.get("service-class-digest"), "service class digest");
    if (!digest.ok()) return digest.status();
    request.service_class.digest = digest.value();
    if (!options.has("service-class-state")) request.service_class.state = ResolutionState::Provided;
  }
  if (options.has("policy-state")) {
    const std::string state = options.get("policy-state");
    if (state == "provided") {
      request.policy.state = ResolutionState::Provided;
    } else if (state == "not-current") {
      request.policy.state = ResolutionState::NotCurrent;
    } else if (state == "unavailable") {
      request.policy.state = ResolutionState::Unavailable;
    } else {
      return Status(StatusCode::InvalidArgument,
                    "--policy-state must be provided, not-current or unavailable");
    }
  }
  if (options.has("policy-digest")) {
    const Result<Digest> digest = parse_digest(options.get("policy-digest"), "policy digest");
    if (!digest.ok()) return digest.status();
    request.policy.digest = digest.value();
    if (!options.has("policy-state")) request.policy.state = ResolutionState::Provided;
  }

  for (const std::string& token : split_commas(options.get("accepted-classes"))) {
    request.accepted_classes.push_back(token);
  }
  for (const std::string& text : options.dimensions) {
    const Result<DimensionRequest> dimension = parse_request_dimension(text);
    if (!dimension.ok()) return dimension.status();
    request.dimensions.push_back(dimension.value());
  }

  const std::string composition = options.get("composition");
  if (!composition.empty()) {
    if (!facility_composition_from_string(composition, request.facility_composition)) {
      return Status(StatusCode::InvalidArgument, "--composition must be principal-only or conjunctive");
    }
  }
  request.facility_envelope_id = options.get("facility");
  return request;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
Result<std::filesystem::path> store_path(const Options& options) {
  const std::string path = options.get("store");
  if (path.empty()) return Status(StatusCode::InvalidArgument, "--store is required");
  return std::filesystem::path(path);
}

// A read path opens an existing store read-only: reading must never create state, and a
// missing store is reported rather than materialised. A write path creates the store when
// the root holds none, which keeps the first declaration a single command.
StoreOptions store_options(bool read_only) {
  StoreOptions options;
  options.mode = read_only ? OpenMode::ReadOnly : OpenMode::OpenOrCreate;
  return options;
}

int command_declare(const Options& options, bool revise) {
  const Result<std::filesystem::path> path = store_path(options);
  if (!path.ok()) { write_failure(path.status().code(), path.status().detail()); return kExitUsage; }
  Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(path.value(), store_options(false));
  if (!service.ok()) { write_failure(service.status().code(), service.status().detail()); return kExitFailure; }
  const Result<Envelope> envelope = build_envelope(options);
  if (!envelope.ok()) { write_failure(envelope.status().code(), envelope.status().detail()); return kExitUsage; }
  const Result<Timestamp> at = parse_time(options.get("at"), "request timestamp");
  if (!at.ok()) { write_failure(at.status().code(), at.status().detail()); return kExitUsage; }

  if (!revise) {
    DeclareInput input;
    input.envelope = envelope.value();
    input.idempotency_key = options.get("idempotency-key");
    input.requested_at = at.value();
    const Result<EnvelopeDeclaration> declared = service.value()->declare(input);
    if (!declared.ok()) { write_failure(declared.status().code(), declared.status().detail()); return kExitFailure; }
    JsonWriter writer(std::cout);
    writer.begin_object();
    writer.bool_field("ok", true);
    writer.string_field("operation", "declare");
    writer.bool_field("replayed", declared.value().replayed);
    writer.string_field("envelope_id", declared.value().envelope_id);
    writer.integer_field("revision", declared.value().revision);
    writer.integer_field("control_epoch", declared.value().control_epoch);
    writer.digest_field("record_digest", declared.value().record_digest);
    writer.digest_field("content_digest", declared.value().content_digest);
    writer.end_object();
    std::cout << '\n';
    return kExitOk;
  }

  ReviseInput input;
  input.envelope = envelope.value();
  input.idempotency_key = options.get("idempotency-key");
  input.requested_at = at.value();
  if (options.has("expected-revision")) {
    const Result<std::uint64_t> revision =
        parse_unsigned(options.get("expected-revision"), "expected revision");
    if (!revision.ok()) { write_failure(revision.status().code(), revision.status().detail()); return kExitUsage; }
    input.expected_current_revision = revision.value();
  }
  if (options.has("expected-digest")) {
    const Result<Digest> digest = parse_digest(options.get("expected-digest"), "expected digest");
    if (!digest.ok()) { write_failure(digest.status().code(), digest.status().detail()); return kExitUsage; }
    input.expected_current_digest = digest.value();
  }
  const Result<EnvelopeRevision> revised = service.value()->revise(input);
  if (!revised.ok()) { write_failure(revised.status().code(), revised.status().detail()); return kExitFailure; }
  JsonWriter writer(std::cout);
  writer.begin_object();
  writer.bool_field("ok", true);
  writer.string_field("operation", "revise");
  writer.bool_field("replayed", revised.value().replayed);
  writer.string_field("envelope_id", revised.value().envelope_id);
  writer.integer_field("previous_revision", revised.value().previous_revision);
  writer.integer_field("revision", revised.value().revision);
  writer.integer_field("control_epoch", revised.value().control_epoch);
  writer.digest_field("record_digest", revised.value().record_digest);
  writer.digest_field("content_digest", revised.value().content_digest);
  writer.end_object();
  std::cout << '\n';
  return kExitOk;
}

int command_show(const Options& options, bool explicit_revision) {
  const Result<std::filesystem::path> path = store_path(options);
  if (!path.ok()) { write_failure(path.status().code(), path.status().detail()); return kExitUsage; }
  Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(path.value(), store_options(true));
  if (!service.ok()) { write_failure(service.status().code(), service.status().detail()); return kExitFailure; }
  Result<EnvelopeView> view = Result<EnvelopeView>(Status(StatusCode::InvalidArgument, "unset"));
  if (explicit_revision) {
    const Result<std::uint64_t> revision = parse_unsigned(options.get("revision"), "revision");
    if (!revision.ok()) { write_failure(revision.status().code(), revision.status().detail()); return kExitUsage; }
    view = service.value()->get_envelope_revision(options.get("id"), revision.value());
  } else {
    view = service.value()->get_envelope(options.get("id"));
  }
  if (!view.ok()) { write_failure(view.status().code(), view.status().detail()); return kExitFailure; }
  JsonWriter writer(std::cout);
  writer.begin_object();
  writer.bool_field("ok", true);
  write_envelope(writer, view.value());
  writer.end_object();
  std::cout << '\n';
  return kExitOk;
}

int command_list(const Options& options) {
  const Result<std::filesystem::path> path = store_path(options);
  if (!path.ok()) { write_failure(path.status().code(), path.status().detail()); return kExitUsage; }
  Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(path.value(), store_options(true));
  if (!service.ok()) { write_failure(service.status().code(), service.status().detail()); return kExitFailure; }
  const Result<std::vector<EnvelopeView>> views = service.value()->list_envelopes();
  if (!views.ok()) { write_failure(views.status().code(), views.status().detail()); return kExitFailure; }
  JsonWriter writer(std::cout);
  writer.begin_object();
  writer.bool_field("ok", true);
  writer.integer_field("count", views.value().size());
  writer.field_array_start("envelopes");
  for (const EnvelopeView& view : views.value()) {
    writer.object_element([&view](JsonWriter& inner) { write_envelope(inner, view); });
  }
  writer.end_array();
  writer.end_object();
  std::cout << '\n';
  return kExitOk;
}

int command_evaluate(const Options& options, bool authorize) {
  const Result<std::filesystem::path> path = store_path(options);
  if (!path.ok()) { write_failure(path.status().code(), path.status().detail()); return kExitUsage; }
  Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(path.value(), store_options(!authorize));
  if (!service.ok()) { write_failure(service.status().code(), service.status().detail()); return kExitFailure; }
  const Result<EvaluationRequest> request = build_request(options);
  if (!request.ok()) { write_failure(request.status().code(), request.status().detail()); return kExitUsage; }

  JsonWriter writer(std::cout);
  if (authorize) {
    const Result<AuthorizeResult> outcome = service.value()->authorize(request.value());
    if (!outcome.ok()) { write_failure(outcome.status().code(), outcome.status().detail()); return kExitFailure; }
    writer.begin_object();
    writer.bool_field("ok", true);
    writer.string_field("operation", "authorize");
    writer.bool_field("replayed", outcome.value().replayed);
    write_evaluation(writer, outcome.value().evaluation);
    writer.field_object_start("record");
    write_record(writer, outcome.value().record);
    writer.end_object();
    writer.end_object();
  } else {
    const Result<EvaluationResult> outcome = service.value()->evaluate(request.value());
    if (!outcome.ok()) { write_failure(outcome.status().code(), outcome.status().detail()); return kExitFailure; }
    writer.begin_object();
    writer.bool_field("ok", true);
    writer.string_field("operation", "evaluate");
    writer.bool_field("recorded", false);
    write_evaluation(writer, outcome.value());
    writer.end_object();
  }
  std::cout << '\n';
  return kExitOk;
}

int command_verify(const Options& options) {
  const Result<std::filesystem::path> path = store_path(options);
  if (!path.ok()) { write_failure(path.status().code(), path.status().detail()); return kExitUsage; }
  Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(path.value(), store_options(true));
  if (!service.ok()) { write_failure(service.status().code(), service.status().detail()); return kExitFailure; }
  VerifyInput input;
  if (options.has("decision")) input.decision_id = options.get("decision");
  if (options.has("expected-digest")) {
    const Result<Digest> digest = parse_digest(options.get("expected-digest"), "expected digest");
    if (!digest.ok()) { write_failure(digest.status().code(), digest.status().detail()); return kExitUsage; }
    input.expected_envelope_digest = digest.value();
  }
  if (!input.decision_id.has_value() && !input.expected_envelope_digest.has_value()) {
    write_failure(StatusCode::InvalidArgument, "--decision or --expected-digest is required");
    return kExitUsage;
  }
  if (options.has("at")) {
    const Result<Timestamp> at = parse_time(options.get("at"), "verification timestamp");
    if (!at.ok()) { write_failure(at.status().code(), at.status().detail()); return kExitUsage; }
    input.at = at.value();
  }
  const Result<VerifyResult> verdict = service.value()->verify_authority(input);
  if (!verdict.ok()) { write_failure(verdict.status().code(), verdict.status().detail()); return kExitFailure; }
  JsonWriter writer(std::cout);
  writer.begin_object();
  writer.bool_field("ok", true);
  writer.bool_field("valid", verdict.value().verdict.valid());
  writer.string_field("authority_state", to_string(verdict.value().verdict.state));
  writer.string_field("detail", verdict.value().verdict.detail);
  writer.string_field("envelope_id", verdict.value().verdict.envelope_id);
  writer.integer_field("granted_revision", verdict.value().verdict.granted_revision);
  writer.integer_field("current_revision", verdict.value().verdict.current_revision);
  writer.integer_field("granted_control_epoch", verdict.value().verdict.granted_control_epoch);
  writer.integer_field("current_control_epoch", verdict.value().verdict.current_control_epoch);
  writer.digest_field("decision_digest", verdict.value().record.decision_digest);
  writer.end_object();
  std::cout << '\n';
  return kExitOk;
}

int command_decision(const Options& options, bool listing) {
  const Result<std::filesystem::path> path = store_path(options);
  if (!path.ok()) { write_failure(path.status().code(), path.status().detail()); return kExitUsage; }
  Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(path.value(), store_options(true));
  if (!service.ok()) { write_failure(service.status().code(), service.status().detail()); return kExitFailure; }
  JsonWriter writer(std::cout);
  if (!listing) {
    const Result<DecisionRecord> record = service.value()->get_decision(options.get("decision"));
    if (!record.ok()) { write_failure(record.status().code(), record.status().detail()); return kExitFailure; }
    writer.begin_object();
    writer.bool_field("ok", true);
    write_record(writer, record.value());
    writer.end_object();
    std::cout << '\n';
    return kExitOk;
  }
  const Result<std::uint64_t> limit = parse_unsigned(options.get("limit", "20"), "decision limit");
  if (!limit.ok()) { write_failure(limit.status().code(), limit.status().detail()); return kExitUsage; }
  const Result<std::vector<DecisionRecord>> records =
      service.value()->list_decisions(options.get("id"), limit.value());
  if (!records.ok()) { write_failure(records.status().code(), records.status().detail()); return kExitFailure; }
  writer.begin_object();
  writer.bool_field("ok", true);
  writer.integer_field("count", records.value().size());
  writer.field_array_start("decisions");
  for (const DecisionRecord& record : records.value()) {
    writer.object_element([&record](JsonWriter& inner) { write_record(inner, record); });
  }
  writer.end_array();
  writer.end_object();
  std::cout << '\n';
  return kExitOk;
}

int command_record_usage(const Options& options) {
  const Result<std::filesystem::path> path = store_path(options);
  if (!path.ok()) { write_failure(path.status().code(), path.status().detail()); return kExitUsage; }
  Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(path.value(), store_options(false));
  if (!service.ok()) { write_failure(service.status().code(), service.status().detail()); return kExitFailure; }
  const Result<Timestamp> at = parse_time(options.get("at"), "request timestamp");
  if (!at.ok()) { write_failure(at.status().code(), at.status().detail()); return kExitUsage; }

  UsageInput input;
  input.envelope_id = options.get("id");
  if (input.envelope_id.empty()) { write_failure(StatusCode::InvalidArgument, "--id is required"); return kExitUsage; }
  input.idempotency_key = options.get("idempotency-key");
  input.requested_at = at.value();
  if (options.has("expected-revision")) {
    const Result<std::uint64_t> revision =
        parse_unsigned(options.get("expected-revision"), "expected revision");
    if (!revision.ok()) { write_failure(revision.status().code(), revision.status().detail()); return kExitUsage; }
    input.expected_envelope_revision = revision.value();
  }
  for (const std::string& text : options.dimensions) {
    const std::vector<std::string> fields = split_colons(text);
    if (fields.empty()) { write_failure(StatusCode::InvalidArgument, "a usage entry needs a dimension kind"); return kExitUsage; }
    const Result<DimensionKind> kind = parse_kind(fields[0]);
    if (!kind.ok()) { write_failure(kind.status().code(), kind.status().detail()); return kExitUsage; }
    UsageDelta delta;
    delta.envelope_id = input.envelope_id;
    delta.kind = kind.value();
    if (fields.size() >= 2U && !fields[1].empty()) {
      const Result<std::int64_t> amount = parse_signed_quantity(fields[1], "usage delta");
      if (!amount.ok()) { write_failure(amount.status().code(), amount.status().detail()); return kExitUsage; }
      delta.delta = amount.value();
    }
    if (fields.size() >= 3U) delta.principal = fields[2];
    if (fields.size() >= 4U) delta.compatibility_class = fields[3];
    delta.source = options.get("source");
    if (fields.size() >= 2U && fields[1] == "observed") {
      write_failure(StatusCode::InvalidArgument,
                    "an observation is not a commitment; record it with record-observation");
      return kExitUsage;
    }
    input.deltas.push_back(std::move(delta));
  }
  if (input.deltas.empty()) { write_failure(StatusCode::InvalidArgument, "at least one --dimension is required"); return kExitUsage; }
  const Result<UsageResult> recorded = service.value()->record_usage(input);
  if (!recorded.ok()) { write_failure(recorded.status().code(), recorded.status().detail()); return kExitFailure; }
  JsonWriter writer(std::cout);
  writer.begin_object();
  writer.bool_field("ok", true);
  writer.string_field("operation", "record-usage");
  writer.string_field("envelope_id", recorded.value().envelope_id);
  writer.integer_field("envelope_revision", recorded.value().envelope_revision);
  writer.integer_field("entries_appended", recorded.value().entries_appended);
  writer.integer_field("entries_replayed", recorded.value().entries_replayed);
  writer.integer_field("first_sequence", recorded.value().first_sequence);
  writer.integer_field("last_sequence", recorded.value().last_sequence);
  writer.integer_field("control_epoch", recorded.value().control_epoch);
  writer.digest_field("entries_digest", recorded.value().entries_digest);
  writer.end_object();
  std::cout << '\n';
  return kExitOk;
}

int command_record_observation(const Options& options) {
  const Result<std::filesystem::path> path = store_path(options);
  if (!path.ok()) { write_failure(path.status().code(), path.status().detail()); return kExitUsage; }
  Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(path.value(), store_options(false));
  if (!service.ok()) { write_failure(service.status().code(), service.status().detail()); return kExitFailure; }
  const Result<Timestamp> at = parse_time(options.get("at"), "observation timestamp");
  if (!at.ok()) { write_failure(at.status().code(), at.status().detail()); return kExitUsage; }
  ObservationInput input;
  input.envelope_id = options.get("id");
  if (input.envelope_id.empty()) { write_failure(StatusCode::InvalidArgument, "--id is required"); return kExitUsage; }
  input.idempotency_key = options.get("idempotency-key");
  input.requested_at = at.value();
  for (const std::string& text : options.dimensions) {
    const std::vector<std::string> fields = split_colons(text);
    if (fields.empty()) { write_failure(StatusCode::InvalidArgument, "an observation needs a dimension kind"); return kExitUsage; }
    const Result<DimensionKind> kind = parse_kind(fields[0]);
    if (!kind.ok()) { write_failure(kind.status().code(), kind.status().detail()); return kExitUsage; }
    ObservationEntry entry;
    entry.envelope_id = input.envelope_id;
    entry.kind = kind.value();
    if (fields.size() >= 2U && !fields[1].empty() && fields[1] != "unknown") {
      Nanounits value = 0U;
      if (!parse_quantity(fields[1], value)) {
        write_failure(StatusCode::InvalidArgument, "an observed value is not a canonical decimal quantity");
        return kExitUsage;
      }
      entry.status = MeasureStatus::Measured;
      entry.value = value;
    } else {
      entry.status = MeasureStatus::Unknown;
    }
    if (fields.size() >= 3U) entry.principal = fields[2];
    entry.observed_at = at.value();
    entry.source = options.get("source");
    input.entries.push_back(std::move(entry));
  }
  if (input.entries.empty()) { write_failure(StatusCode::InvalidArgument, "at least one --dimension is required"); return kExitUsage; }
  const Result<ObservationResult> recorded = service.value()->record_observation(input);
  if (!recorded.ok()) { write_failure(recorded.status().code(), recorded.status().detail()); return kExitFailure; }
  JsonWriter writer(std::cout);
  writer.begin_object();
  writer.bool_field("ok", true);
  writer.string_field("operation", "record-observation");
  writer.string_field("envelope_id", recorded.value().envelope_id);
  writer.integer_field("stored", recorded.value().stored);
  writer.integer_field("replayed", recorded.value().replayed);
  writer.integer_field("last_sequence", recorded.value().last_sequence);
  writer.digest_field("entries_digest", recorded.value().entries_digest);
  writer.end_object();
  std::cout << '\n';
  return kExitOk;
}

int command_committed(const Options& options) {
  const Result<std::filesystem::path> path = store_path(options);
  if (!path.ok()) { write_failure(path.status().code(), path.status().detail()); return kExitUsage; }
  Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(path.value(), store_options(true));
  if (!service.ok()) { write_failure(service.status().code(), service.status().detail()); return kExitFailure; }
  const Result<CommittedMap> committed = service.value()->committed_usage(options.get("id"));
  if (!committed.ok()) { write_failure(committed.status().code(), committed.status().detail()); return kExitFailure; }
  JsonWriter writer(std::cout);
  writer.begin_object();
  writer.bool_field("ok", true);
  writer.string_field("envelope_id", options.get("id"));
  writer.integer_field("count", committed.value().size());
  writer.field_array_start("committed");
  for (const auto& pair : committed.value()) {
    writer.object_element([&pair](JsonWriter& inner) {
      inner.string_field("dimension", to_string(pair.first.kind));
      inner.string_field("principal", pair.first.principal);
      inner.string_field("compatibility_class", pair.first.compatibility_class);
      inner.signed_field("value", pair.second.value);
      inner.string_field("formatted", format_quantity(static_cast<Nanounits>(pair.second.value < 0
                                                                         ? -pair.second.value
                                                                         : pair.second.value)));
      inner.string_field("observed_at", format_timestamp(pair.second.observed_at));
      inner.string_field("source", pair.second.source);
    });
  }
  writer.end_array();
  writer.end_object();
  std::cout << '\n';
  return kExitOk;
}

int command_observations(const Options& options) {
  const Result<std::filesystem::path> path = store_path(options);
  if (!path.ok()) { write_failure(path.status().code(), path.status().detail()); return kExitUsage; }
  Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(path.value(), store_options(true));
  if (!service.ok()) { write_failure(service.status().code(), service.status().detail()); return kExitFailure; }
  const Result<std::vector<ObservationEntry>> entries = service.value()->observations(options.get("id"));
  if (!entries.ok()) { write_failure(entries.status().code(), entries.status().detail()); return kExitFailure; }
  JsonWriter writer(std::cout);
  writer.begin_object();
  writer.bool_field("ok", true);
  writer.string_field("envelope_id", options.get("id"));
  writer.integer_field("count", entries.value().size());
  writer.field_array_start("observations");
  for (const ObservationEntry& entry : entries.value()) {
    writer.object_element([&entry](JsonWriter& inner) {
      inner.string_field("entry_id", entry.entry_id);
      inner.string_field("dimension", to_string(entry.kind));
      inner.string_field("status", entry.status == MeasureStatus::Measured ? "measured" : "unknown");
      if (entry.status == MeasureStatus::Measured) {
        inner.string_field("value", format_quantity(entry.value));
      } else {
        inner.null_field("value");
      }
      inner.string_field("principal", entry.principal);
      inner.string_field("observed_at", format_timestamp(entry.observed_at));
      inner.string_field("source", entry.source);
      inner.integer_field("sequence", entry.sequence);
    });
  }
  writer.end_array();
  writer.end_object();
  std::cout << '\n';
  return kExitOk;
}

int command_history(const Options& options) {
  const Result<std::filesystem::path> path = store_path(options);
  if (!path.ok()) { write_failure(path.status().code(), path.status().detail()); return kExitUsage; }
  Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(path.value(), store_options(true));
  if (!service.ok()) { write_failure(service.status().code(), service.status().detail()); return kExitFailure; }
  const Result<std::vector<HistoryEntry>> entries = service.value()->history(options.get("id"));
  if (!entries.ok()) { write_failure(entries.status().code(), entries.status().detail()); return kExitFailure; }
  JsonWriter writer(std::cout);
  writer.begin_object();
  writer.bool_field("ok", true);
  writer.string_field("envelope_id", options.get("id"));
  writer.integer_field("count", entries.value().size());
  writer.field_array_start("history");
  for (const HistoryEntry& entry : entries.value()) {
    writer.object_element([&entry](JsonWriter& inner) {
      inner.string_field("kind", entry.kind);
      inner.integer_field("sequence", entry.sequence);
      inner.string_field("envelope_id", entry.envelope_id);
      inner.integer_field("revision", entry.revision);
      inner.string_field("detail", entry.detail);
      inner.digest_field("digest", entry.digest);
      inner.string_field("at", format_timestamp(entry.at));
    });
  }
  writer.end_array();
  writer.end_object();
  std::cout << '\n';
  return kExitOk;
}

int command_retire(const Options& options) {
  const Result<std::filesystem::path> path = store_path(options);
  if (!path.ok()) { write_failure(path.status().code(), path.status().detail()); return kExitUsage; }
  Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(path.value(), store_options(false));
  if (!service.ok()) { write_failure(service.status().code(), service.status().detail()); return kExitFailure; }
  const Result<Timestamp> at = parse_time(options.get("at"), "request timestamp");
  if (!at.ok()) { write_failure(at.status().code(), at.status().detail()); return kExitUsage; }
  const Result<std::uint64_t> request_id = parse_unsigned(options.get("request-id", "1"), "request identifier");
  if (!request_id.ok()) { write_failure(request_id.status().code(), request_id.status().detail()); return kExitUsage; }
  TombstoneInput input;
  input.envelope_id = options.get("id");
  input.reason = options.get("reason");
  input.requested_at = at.value();
  input.request_id = request_id.value();
  if (options.has("expected-revision")) {
    const Result<std::uint64_t> revision =
        parse_unsigned(options.get("expected-revision"), "expected revision");
    if (!revision.ok()) { write_failure(revision.status().code(), revision.status().detail()); return kExitUsage; }
    input.expected_current_revision = revision.value();
  }
  const Result<TombstoneResult> retired = service.value()->tombstone(input);
  if (!retired.ok()) { write_failure(retired.status().code(), retired.status().detail()); return kExitFailure; }
  JsonWriter writer(std::cout);
  writer.begin_object();
  writer.bool_field("ok", true);
  writer.string_field("operation", "retire");
  writer.bool_field("replayed", retired.value().replayed);
  writer.string_field("envelope_id", retired.value().envelope_id);
  writer.integer_field("superseded_revision", retired.value().superseded_revision);
  writer.string_field("reason", retired.value().reason);
  writer.digest_field("last_record_digest", retired.value().last_record_digest);
  writer.end_object();
  std::cout << '\n';
  return kExitOk;
}

int command_statistics(const Options& options) {
  const Result<std::filesystem::path> path = store_path(options);
  if (!path.ok()) { write_failure(path.status().code(), path.status().detail()); return kExitUsage; }
  Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(path.value(), store_options(true));
  if (!service.ok()) { write_failure(service.status().code(), service.status().detail()); return kExitFailure; }
  const Result<StoreStatistics> stats = service.value()->statistics();
  if (!stats.ok()) { write_failure(stats.status().code(), stats.status().detail()); return kExitFailure; }
  JsonWriter writer(std::cout);
  writer.begin_object();
  writer.bool_field("ok", true);
  writer.integer_field("control_epoch", stats.value().epoch);
  writer.integer_field("state_epoch", stats.value().state_epoch);
  writer.integer_field("state_sequence", stats.value().state_sequence);
  writer.integer_field("frames_committed", stats.value().frames_committed);
  writer.integer_field("envelope_count", stats.value().envelope_count);
  writer.integer_field("current_revision_count", stats.value().current_revision_count);
  writer.integer_field("retained_revision_count", stats.value().retained_revision_count);
  writer.integer_field("retired_count", stats.value().tombstoned_count);
  writer.integer_field("decision_count", stats.value().decision_count);
  writer.integer_field("granted_count", stats.value().granted_count);
  writer.integer_field("denied_count", stats.value().denied_count);
  writer.integer_field("indeterminate_count", stats.value().indeterminate_count);
  writer.integer_field("usage_entry_count", stats.value().usage_entry_count);
  writer.integer_field("observation_count", stats.value().observation_count);
  writer.integer_field("committed_key_count", stats.value().committed_key_count);
  writer.integer_field("compaction_count", stats.value().compaction_count);
  writer.integer_field("store_bytes", stats.value().store_bytes);
  writer.digest_field("state_digest", stats.value().state_digest);
  writer.digest_field("chain_digest", stats.value().chain_digest);
  writer.end_object();
  std::cout << '\n';
  return kExitOk;
}

int command_compact(const Options& options) {
  const Result<std::filesystem::path> path = store_path(options);
  if (!path.ok()) { write_failure(path.status().code(), path.status().detail()); return kExitUsage; }
  Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(path.value(), store_options(false));
  if (!service.ok()) { write_failure(service.status().code(), service.status().detail()); return kExitFailure; }
  const Status status = service.value()->compact();
  if (!status.ok()) { write_failure(status.code(), status.detail()); return kExitFailure; }
  const Result<StoreStatistics> stats = service.value()->statistics();
  if (!stats.ok()) { write_failure(stats.status().code(), stats.status().detail()); return kExitFailure; }
  JsonWriter writer(std::cout);
  writer.begin_object();
  writer.bool_field("ok", true);
  writer.string_field("operation", "compact");
  writer.integer_field("control_epoch", stats.value().epoch);
  writer.integer_field("compaction_count", stats.value().compaction_count);
  writer.integer_field("frames_committed", stats.value().frames_committed);
  writer.digest_field("state_digest", stats.value().state_digest);
  writer.end_object();
  std::cout << '\n';
  return kExitOk;
}

// Reports the exact wire contract, so a downstream consumer can confirm the format
// version, the canonical field order and the digest domains without reading headers.
int command_schema() {
  JsonWriter writer(std::cout);
  writer.begin_object();
  writer.bool_field("ok", true);
  writer.string_field("format", "resource-envelope-canonical");
  writer.integer_field("canonical_format_version", kCanonicalFormatVersion);
  writer.integer_field("store_format_version", kStoreFormatVersion);
  writer.integer_field("store_layout_version", kStoreLayoutVersion);
  writer.string_field("byte_order", "little-endian");
  writer.field_array_start("dimensions");
  const DimensionKind kinds[] = {
      DimensionKind::SpaceRackUnits,     DimensionKind::SpaceRackSlots,
      DimensionKind::PowerDrawWatts,     DimensionKind::PowerFeedCircuits,
      DimensionKind::CoolingLoadWatts,   DimensionKind::CoolingEnergyBudget,
      DimensionKind::RackExposureClass,  DimensionKind::RedundancyLevel,
      DimensionKind::CountRackPositions,
  };
  for (const DimensionKind kind : kinds) {
    const Cardinality cardinality = cardinality_of(kind);
    writer.object_element([kind, cardinality](JsonWriter& inner) {
      inner.string_field("dimension", to_string(kind));
      inner.integer_field("code", static_cast<std::uint8_t>(kind));
      inner.string_field("unit", to_string(unit_of(kind)));
      inner.string_field("cardinality", cardinality == Cardinality::Consumable
                                                ? "consumable"
                                                : (cardinality == Cardinality::Exclusive ? "exclusive"
                                                                                         : "non-consumable"));
    });
  }
  writer.end_array();
  writer.field_array_start("reason_codes");
  for (std::uint32_t code = 0U; code <= static_cast<std::uint32_t>(StatusCode::UnsupportedOperation); ++code) {
    const StatusCode value = static_cast<StatusCode>(code);
    const char* name = to_string(value);
    const std::string text(name);
    if (text == "UnknownStatusCode") continue;
    writer.object_element([code, &text](JsonWriter& inner) {
      inner.integer_field("code", code);
      inner.string_field("name", text);
    });
  }
  writer.end_array();
  writer.field_array_start("digest_domains");
  const char* domains[] = {kDomainEnvelopeContent, kDomainEnvelopeRecord, kDomainRequest, kDomainEvidence,
                           kDomainDecision,        kDomainState,          kDomainFrame,   kDomainIdempotency,
                           kDomainGrant};
  for (const char* domain : domains) {
    writer.object_element([domain](JsonWriter& inner) {
      inner.string_field("domain", domain);
      inner.string_field("digest", "sha256");
    });
  }
  writer.end_array();
  writer.end_object();
  std::cout << '\n';
  return kExitOk;
}

int run_command(const std::vector<std::string>& arguments) {
  if (arguments.empty()) {
    std::cout << usage_text();
    return kExitUsage;
  }
  const std::string command = arguments[0];
  if (command == "help") {
    std::cout << usage_text();
    return kExitOk;
  }
  const Result<Options> parsed = parse_options(std::vector<std::string>(arguments.begin() + 1, arguments.end()));
  if (!parsed.ok()) { write_failure(parsed.status().code(), parsed.status().detail()); std::cout << usage_text(); return kExitUsage; }
  const Options& options = parsed.value();
  if (options.has("help")) {
    std::cout << usage_text();
    return kExitOk;
  }
  if (command == "schema") return command_schema();
  if (command == "declare") return command_declare(options, false);
  if (command == "revise") return command_declare(options, true);
  if (command == "show") return command_show(options, false);
  if (command == "show-revision") return command_show(options, true);
  if (command == "list") return command_list(options);
  if (command == "evaluate") return command_evaluate(options, false);
  if (command == "authorize") return command_evaluate(options, true);
  if (command == "verify") return command_verify(options);
  if (command == "decision") return command_decision(options, false);
  if (command == "decisions") return command_decision(options, true);
  if (command == "record-usage") return command_record_usage(options);
  if (command == "record-observation") return command_record_observation(options);
  if (command == "committed") return command_committed(options);
  if (command == "observations") return command_observations(options);
  if (command == "history") return command_history(options);
  if (command == "retire") return command_retire(options);
  if (command == "statistics") return command_statistics(options);
  if (command == "compact") return command_compact(options);
  write_failure(StatusCode::InvalidArgument, std::string("unknown command: ") + command);
  std::cout << usage_text();
  return kExitUsage;
}

}  // namespace

int run(const std::vector<std::string>& arguments) {
  try {
    return run_command(arguments);
  } catch (const std::exception& error) {
    write_failure(StatusCode::UnsupportedOperation, std::string("unhandled failure: ") + error.what());
    return kExitFailure;
  }
}

}  // namespace cli
}  // namespace resource_envelope

int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
  for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index] == nullptr ? "" : argv[index]);
  // Standard output is used for exactly one canonical JSON object, so it is streamed
  // without buffering surprises and never interleaved with diagnostics.
  std::ios::sync_with_stdio(true);
  return resource_envelope::cli::run(arguments);
}
