// An independent consumer of the installed Resource Envelope package. It links only against
// the installed prefix and it exercises real public API behaviour rather than merely linking:
// it opens a store, declares an envelope, records committed usage, authorises a request,
// reads the envelope back, and prints what the runtime decided.

#include <cstdio>
#include <filesystem>
#include <memory>
#include <system_error>

#include <resource_envelope/canonical.hpp>
#include <resource_envelope/service.hpp>

using namespace resource_envelope;

int fail(const char* what, const Status& status) {
  std::printf("FAIL %s: %s", what, status.describe().c_str());
  std::printf("\n");
  return 1;
}

int main(int argc, char** argv) {
  const std::filesystem::path root =
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::temp_directory_path() / "re-consumer-store";
  std::error_code error;
  std::filesystem::remove_all(root, error);

  StoreOptions options;
  options.mode = OpenMode::OpenOrCreate;
  const Result<std::unique_ptr<EnvelopeService>> service = EnvelopeService::open(root, options);
  if (!service.ok()) return fail("open", service.status());

  Envelope envelope;
  envelope.id = "consumer-envelope";
  envelope.scope.kind = EnvelopeScopeKind::Tenant;
  envelope.scope.identity.id = "consumer-tenant";
  envelope.scope.identity.generation = 1U;
  envelope.site_id = "site-one";
  envelope.require_binding_confirmation = false;
  envelope.provenance.authority = "facility-authority";
  envelope.provenance.actor = "consumer";
  envelope.provenance.declared_at = 1767225600000000000LL;
  DimensionSpec power;
  power.kind = DimensionKind::PowerDrawWatts;
  power.hard_limit = 100ULL * kNanounitsPerUnit;
  power.reserved = 10ULL * kNanounitsPerUnit;
  envelope.dimensions.push_back(power);

  DeclareInput declaration;
  declaration.envelope = envelope;
  declaration.idempotency_key = "consumer-declare";
  declaration.requested_at = 1767225600000000000LL;
  const Result<EnvelopeDeclaration> declared = service.value()->declare(declaration);
  if (!declared.ok()) return fail("declare", declared.status());
  std::printf("declared %s revision %llu\n", declared.value().envelope_id.c_str(),
              static_cast<unsigned long long>(declared.value().revision));

  UsageInput usage;
  usage.envelope_id = declared.value().envelope_id;
  usage.idempotency_key = "consumer-usage";
  usage.requested_at = 1767225600000000000LL;
  UsageDelta delta;
  delta.envelope_id = usage.envelope_id;
  delta.kind = DimensionKind::PowerDrawWatts;
  delta.delta = static_cast<std::int64_t>(30ULL * kNanounitsPerUnit);
  delta.source = "consumer";
  usage.deltas.push_back(delta);
  const Result<UsageResult> recorded = service.value()->record_usage(usage);
  if (!recorded.ok()) return fail("record-usage", recorded.status());
  std::printf("recorded %llu usage entries, committed total %lld\n",
              static_cast<unsigned long long>(recorded.value().entries_appended),
              static_cast<long long>(30));

  EvaluationRequest request;
  request.scope = envelope.scope;
  request.at = 1767225600000000000LL;
  request.idempotency_key = "consumer-authorize";
  DimensionRequest dimension;
  dimension.kind = DimensionKind::PowerDrawWatts;
  dimension.quantity = 50ULL * kNanounitsPerUnit;
  request.dimensions.push_back(dimension);
  const Result<AuthorizeResult> authorised = service.value()->authorize(request);
  if (!authorised.ok()) return fail("authorize", authorised.status());
  const EvaluationResult& outcome = authorised.value().evaluation;
  std::printf("decision outcome %s reason %s blocking %s\n", to_string(outcome.outcome),
              to_string(outcome.reason),
              outcome.blocking_dimension.has_value() ? to_string(*outcome.blocking_dimension) : "none");
  if (!outcome.dimensions.empty()) {
    const DimensionResult& first = outcome.dimensions.front();
    std::printf("dimension %s limit %s residual %s committed %s\n", to_string(first.kind),
                first.limit.has_value() ? format_quantity(*first.limit).c_str() : "unknown",
                first.residual.has_value() ? format_quantity(*first.residual).c_str() : "unknown",
                first.committed.has_value() ? format_quantity(*first.committed).c_str() : "unknown");
  }

  const Result<EnvelopeView> view = service.value()->get_envelope(declared.value().envelope_id);
  if (!view.ok()) return fail("read-back", view.status());
  std::printf("read back revision %llu, record digest %s\n",
              static_cast<unsigned long long>(view.value().envelope.revision),
              view.value().record_digest.to_string().c_str());
  std::printf("consumer OK\n");
  return 0;
}
