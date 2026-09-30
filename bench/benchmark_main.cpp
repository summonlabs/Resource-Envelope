// Benchmark driver. Every number below is a completed operation: nothing is reported for
// submission, enqueue or admission latency. The durable measurements include the real durable
// path - fence write, segment append, flush, read-back verification and manifest publication -
// because that path is part of the guarantee being measured.
//
// Provenance is labelled per line: REAL means the operation ran against a real store directory
// on this host's filesystem; SYNTHETIC means the inputs were constructed by this driver and no
// physical facility hardware was involved, which is true of every line here.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "resource_envelope/canonical.hpp"
#include "resource_envelope/service.hpp"

using namespace resource_envelope;

namespace {

using Clock = std::chrono::steady_clock;

std::string plain_integer(std::uint64_t value) {
  std::string text = std::to_string(value);
  std::string grouped;
  int count = 0;
  for (auto iterator = text.rbegin(); iterator != text.rend(); ++iterator) {
    if (count == 3) {
      grouped.push_back('_');
      count = 0;
    }
    grouped.push_back(*iterator);
    ++count;
  }
  std::string out(grouped.rbegin(), grouped.rend());
  return out;
}

void report(const char* operation, const char* provenance, std::uint64_t iterations,
            std::chrono::nanoseconds elapsed) {
  const double seconds = std::chrono::duration<double>(elapsed).count();
  const double per_operation_ns = iterations == 0U ? 0.0 : (seconds * 1e9) / static_cast<double>(iterations);
  const double per_second = seconds <= 0.0 ? 0.0 : static_cast<double>(iterations) / seconds;
  std::printf("%-34s %-9s %12s ops %10.1f ns/op %14.0f ops/s" "\n",
              operation, provenance, plain_integer(iterations).c_str(), per_operation_ns, per_second);
  std::fflush(stdout);
}

Envelope benchmark_envelope(std::uint64_t watts) {
  Envelope envelope;
  envelope.id = "benchmark-envelope";
  envelope.revision = 1U;
  envelope.scope.kind = EnvelopeScopeKind::Tenant;
  envelope.scope.identity.id = "benchmark-tenant";
  envelope.scope.identity.generation = 1U;
  envelope.site_id = "site-one";
  envelope.provenance.authority = "facility-authority";
  envelope.provenance.actor = "operator";
  envelope.provenance.declared_at = 1767225600000000000LL;
  DimensionSpec power;
  power.kind = DimensionKind::PowerDrawWatts;
  power.hard_limit = watts * kNanounitsPerUnit;
  envelope.dimensions.push_back(power);
  DimensionSpec space;
  space.kind = DimensionKind::SpaceRackUnits;
  space.hard_limit = 40ULL * kNanounitsPerUnit;
  envelope.dimensions.push_back(space);
  return envelope;
}

EvaluationRequest benchmark_request(Nanounits watts) {
  EvaluationRequest request;
  request.scope.kind = EnvelopeScopeKind::Tenant;
  request.scope.identity.id = "benchmark-tenant";
  request.scope.identity.generation = 1U;
  request.at = 1767225600000000000LL;
  DimensionRequest dimension;
  dimension.kind = DimensionKind::PowerDrawWatts;
  dimension.quantity = watts;
  request.dimensions.push_back(dimension);
  return request;
}

class Scratch {
 public:
  Scratch() {
    const auto stamp = Clock::now().time_since_epoch().count();
    path_ = std::filesystem::current_path() / ("re-benchmark-" + std::to_string(stamp));
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  ~Scratch() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace

int main() {
  std::printf("Resource Envelope benchmark - single host, MSVC, SYNTHETIC inputs" "\n");
  std::printf("Every input is constructed by this driver. No physical facility hardware is" "\n");
  std::printf("involved, and no before/after or speedup claim is made." "\n\n");
  std::printf("%-34s %-9s %12s %17s %18s" "\n",
              "operation", "provenance", "iterations", "latency", "throughput");

  const Envelope envelope = benchmark_envelope(100ULL);
  const EvaluationRequest request = benchmark_request(40ULL * kNanounitsPerUnit);

  // Completed canonical encode.
  {
    constexpr std::uint64_t kIterations = 200000U;
    Bytes last;
    const Clock::time_point start = Clock::now();
    for (std::uint64_t index = 0; index < kIterations; ++index) last = canonical_envelope(envelope);
    const std::chrono::nanoseconds elapsed = Clock::now() - start;
    report("canonical encode envelope", "SYNTHETIC", kIterations, elapsed);
    std::printf("%-34s %-9s %12s bytes" "\n", "  encoded size", "-", std::to_string(last.size()).c_str());
  }

  // Completed canonical decode.
  {
    const Bytes encoded = canonical_envelope(envelope);
    constexpr std::uint64_t kIterations = 200000U;
    std::uint64_t accepted = 0U;
    const Clock::time_point start = Clock::now();
    for (std::uint64_t index = 0; index < kIterations; ++index) {
      if (decode_envelope(encoded).ok()) ++accepted;
    }
    const std::chrono::nanoseconds elapsed = Clock::now() - start;
    report("canonical decode envelope", "SYNTHETIC", kIterations, elapsed);
    if (accepted != kIterations) std::printf("  WARNING: %llu decodes were refused" "\n", static_cast<unsigned long long>(kIterations - accepted));
  }

  // Completed SHA-256 over the canonical form.
  {
    const Bytes encoded = canonical_envelope(envelope);
    constexpr std::uint64_t kIterations = 200000U;
    Digest last;
    const Clock::time_point start = Clock::now();
    for (std::uint64_t index = 0; index < kIterations; ++index) {
      last = Digest(sha256_domain(kDomainEnvelopeRecord, encoded));
    }
    const std::chrono::nanoseconds elapsed = Clock::now() - start;
    report("sha256 over canonical bytes", "SYNTHETIC", kIterations, elapsed);
    static_cast<void>(last);
  }

  // Completed pure evaluation.
  {
    constexpr std::uint64_t kIterations = 200000U;
    std::uint64_t completed = 0U;
    const Clock::time_point start = Clock::now();
    for (std::uint64_t index = 0; index < kIterations; ++index) {
      const EvaluationResult outcome = evaluate(envelope, request);
      if (outcome.decision_digest.known()) ++completed;
    }
    const std::chrono::nanoseconds elapsed = Clock::now() - start;
    report("evaluate (pure, no store)", "SYNTHETIC", kIterations, elapsed);
    if (completed != kIterations) std::printf("  WARNING: %llu evaluations produced no digest" "\n", static_cast<unsigned long long>(kIterations - completed));
  }

  // Completed durable declaration: the full commit protocol, including flush and read-back.
  {
    const Scratch scratch;
    StoreOptions options;
    options.mode = OpenMode::OpenOrCreate;
    const Result<std::unique_ptr<EnvelopeService>> service = EnvelopeService::open(scratch.path(), options);
    if (!service.ok()) {
      std::printf("durable declaration: store could not be opened (%s)" "\n", to_string(service.status().code()));
      return 1;
    }
    constexpr std::uint64_t kIterations = 200U;
    std::uint64_t completed = 0U;
    const Clock::time_point start = Clock::now();
    for (std::uint64_t index = 0; index < kIterations; ++index) {
      DeclareInput input;
      input.envelope = benchmark_envelope(100ULL + index);
      input.envelope.id = "benchmark-envelope-" + std::to_string(index);
      input.envelope.provenance.declared_at = 1767225600000000000LL + static_cast<Timestamp>(index);
      input.idempotency_key = "benchmark-declare-" + std::to_string(index);
      input.requested_at = 1767225600000000000LL + static_cast<Timestamp>(index);
      if (service.value()->declare(input).ok()) ++completed;
    }
    const std::chrono::nanoseconds elapsed = Clock::now() - start;
    report("durable declare (fence+flush+commit)", "REAL", completed, elapsed);
    if (completed != kIterations) std::printf("  WARNING: %llu declarations failed" "\n", static_cast<unsigned long long>(kIterations - completed));
  }

  std::printf("\nMethodology: steady_clock around a loop of completed operations, single host," "\n");
  std::printf("single process, default Release optimisation. Durable numbers include the fence" "\n");
  std::printf("write, the segment flush, the read-back verification and the manifest publication." "\n");
  return 0;
}
