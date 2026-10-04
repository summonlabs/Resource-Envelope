# Resource Envelope

Facility-level resource-envelope runtime.

## What this runtime is

A resource envelope is a declared, bounded statement of what a tenant, a service, or a
facility itself may consume from a facility across independent physical dimensions: space,
power, cooling, rack exposure, redundancy and operational counts. Resource Envelope owns the
definition, lifecycle and generation-bound evaluation of those declarations. It answers one
question:

> Given this envelope revision and this evidence, is this request permitted, refused, or
> impossible to determine - and exactly which dimension decided it?

One library and one command line tool are produced:

- `resource_envelope` - the core library, exported as `ResourceEnvelope::resource_envelope`.
- `resource-envelope` - the command line tool.

## Owned boundary

This repository owns exactly the following, and nothing else:

- Definition and lifecycle of resource envelopes: declaration, revision, retention, retirement.
- Scope binding to tenant, service and facility identities, and to the facility site an
  envelope addresses.
- Hard, reserved, alignment and threshold limits per physical dimension, with each dimension
  modelled independently.
- Generation-bound evaluation of a requested or observed allocation against an envelope, with
  deterministic residual and headroom arithmetic and explicit unknown states.
- Envelope revisioning, provenance, effective windows, bounded durable history, canonical
  content and record digests, and stale-authority fencing.
- A durable, integrity-checked store with an atomic commit point, single-writer exclusion,
  crash-consistent recovery and compaction.

### Explicit non-ownership

| Concern | Owning authority | How this runtime relates to it |
| --- | --- | --- |
| Actual capacity and measurements | Facility, Rack, Space, Power, Cooling Capacity | Consumes measurements and commitments as evidence. Never reads a meter, never owns a capacity record. |
| Placement decisions | Placement authority | Never decides placement. Reports the residual and the constraint that would be violated. |
| Admission decisions | Admission authority | Reports a determination. The admission decision is that authority's to make. |
| ASI scheduling and resource quotas | ASI | Not modelled here. |
| DFI bandwidth and path policy | DFI | Not modelled here. |
| Tenant identity lifecycle | Tenant Registry | Binds to an identifier, generation and digest. Never creates or mutates an identity. |
| Service class definition | Service Class Registry | Binds to a service class digest. Never defines what a class means. |

## Core semantic rules

These rules are implemented, not aspirational. Each is covered by at least one test.

1. **Observation is not authority.** A measurement is recorded and reported in `observed` and
   is never an input to admission arithmetic. A measurement never becomes a committed figure
   merely because it exists.
2. **Missing is not zero.** An unknown bound, an unknown committed figure, an unevidenced
   exclusive holder and an unevidenced operational spare each produce `Indeterminate`: never a
   silent zero, never a grant, never a denial.
3. **Requested state is not observed state.** A request is evaluated against declared
   constraints and recorded commitments. It does not become recorded usage by being evaluated;
   recording usage is a separate, explicit operation.
4. **Over-commitment is reported, not clamped.** A dimension whose committed usage exceeds the
   capacity available to it reports `over_committed` and `over_committed_by`, and its residual
   is reported as zero, because a negative residual would read as headroom.
5. **Stale authority is fenced, not inherited.** Every recorded decision names the envelope
   revision, envelope digest, control epoch and authority state digest it relied on, and
   verification reports exactly which of them changed.
6. **Expired means not authoritative.** An effective window is half-open:
   `[effective_from, effective_until)`. At `effective_until` the envelope has already stopped
   being authoritative.
7. **Idempotent replay is resolved before staleness.** A retried request carrying the same
   idempotency key and the same payload digest returns the recorded decision unchanged even
   when the authority has since advanced. A retry carrying a different payload under the same
   key is refused with `IdempotencyConflict`.
8. **Refusals are attributable.** Every refusal names a stage, a reason code and, when a
   dimension decided it, the blocking dimension. Secondary reasons are preserved.

## Dimensions

Each dimension is modelled independently. No conversion between dimensions exists anywhere in
the runtime.

| Dimension | Unit | Cardinality | Meaning of `limit` |
| --- | --- | --- | --- |
| `space-rack-units` | rack-unit | consumable | Rack units the scope may occupy. |
| `space-rack-slots` | slot | consumable | Slots the scope may occupy. |
| `power-draw-watts` | watt | consumable | Draw the scope may commit. |
| `power-feed-circuits` | circuit | exclusive | Capacity of the exclusive feed-class dimension. |
| `cooling-load-watts` | watt | consumable | Cooling load the scope may commit. |
| `cooling-energy-budget` | watt-hour | non-consumable | Budget that must be met. |
| `rack-exposure-class` | class-token | exclusive | Capacity of the exclusive exposure-class dimension. |
| `redundancy-level` | level | non-consumable | Minimum redundancy level that must be met. |
| `count-rack-positions` | position | consumable | Positions the scope may commit. |

### Cardinality semantics

- **Consumable** - `residual = max(0, limit - reserved - effective_committed)`. A request must
  not exceed the residual.
- **Exclusive** - at most one principal may hold the dimension and a holder consumes all of
  it. Compatibility classes are compared exactly: the runtime stores and compares them and
  never ranks, normalises or substitutes one for another.
- **Non-consumable** - a threshold that must be met. It yields no headroom and no residual is
  reported for it.

### Arithmetic

Every quantity is a count of `1e-9` base units in an unsigned 64-bit integer. All arithmetic
is exact; there is no floating point on any decision path.

- Declared magnitudes above `2^53` nanounits are refused at declaration rather than saturated.
- `checked_add` and `checked_sub` report overflow instead of wrapping, and saturate only at the
  true representable maximum.
- Every dimension reports `effective_committed`: the committed figure aligned upwards to the
  declared quantum. Upward alignment is the conservative direction, because it can only reduce
  the residual.
- A request whose quantity is not an exact multiple of the declared quantum is refused with
  `RejectedQuantization`, and the aligned quantity is reported in `quantized_quantity`. The
  runtime never rounds a request silently.

### Per-dimension report

| Field | Meaning |
| --- | --- |
| `committed` | The committed figure used, or `null` when unknown. |
| `effective_committed` | The committed figure after quantum alignment. |
| `residual` | Exactly determined remaining capacity, or `null` when not determinable. |
| `over_committed`, `over_committed_by` | Committed usage exceeds available capacity. |
| `headroom_after` | Residual less the aligned request; zero when the request exceeds it. |
| `observed`, `observation_present` | A measurement: reported, never consumed. |
| `shortfall` | How far a non-consumable threshold fell short. |

## Outcomes and refusal precedence

A determination is one of three values. `Denied` is a definite refusal. `Indeterminate` is the
absence of a determination because a required input is unknown. The two are never conflated,
because conflating them would let missing data read as a decision.

Refusal stages are ordered and fixed. When several failures coexist the earliest stage decides
the primary reason and every later failure is preserved in `secondary`.

| Ordinal | Stage | Examples |
| --- | --- | --- |
| 1 | `RequestValidation` | malformed identifier, duplicate dimension, unparseable timestamp |
| 2 | `EnvelopePresence` | no envelope exists for the scope |
| 3 | `ScopeMatch` | the resolved envelope has a different scope |
| 4 | `LineageIntegrity` | the envelope names itself as its own source |
| 5 | `EnvelopeComposition` | conjunctive composition requested without a facility envelope |
| 6 | `EnvelopeCurrent` | the revision under evaluation is not current |
| 7 | `GenerationBinding` | identity generation or digest does not match the binding |
| 8 | `EffectiveWindow` | the window has not started, or has ended |
| 9 | `RequestedRevision` | expected revision or digest does not match |
| 10 | `ContextBinding` | service class or policy digest does not match |
| 11 | `ProviderAvailability` | an authority could not be consulted |
| 12 | `ReplayResolution` | idempotency key reused with a different payload |
| 13 | `Arithmetic` | a computed value is not representable |
| 14 | `DimensionConstraint` | a dimension refused, or could not be determined |

Reason codes are stable integers, printed by the CLI and persisted inside decision records.
They are never renumbered; new codes are appended. `resource-envelope schema` prints the
current complete list.

## Authority, generations and fencing

Eight independent values can invalidate a prior determination, and changing any one of them is
detectable from the recorded decision alone:

| Value | Changes when |
| --- | --- |
| Envelope revision | any constraint is revised |
| Envelope record digest | any constraint, binding, window or provenance changes |
| Envelope content digest | any constraint or binding changes (excludes provenance text) |
| Identity generation | the owning identity authority advances an identity |
| Identity digest | the bound identity record changes |
| Service class digest | the referenced service class revision changes |
| Policy digest | the authorising policy revision changes |
| Store control epoch | any successful mutation is published |

A new authority incarnation is fenced by the epoch: every successful commit advances the store
control epoch and writes it to the epoch fence before the manifest that relies on it is
published. A decision recorded under an older epoch is reported `ControlEpochFenced`, which is
distinct from `EnvelopeRevisionAdvanced`, so an operator can tell a retirement from an ordinary
revision.

Binding confirmation is explicit. An envelope with `require_binding_confirmation` set refuses
evaluation unless the caller supplies the identity digest it is bound to; an absent
confirmation is never read as agreement, and a supplied digest that differs from the binding is
refused at the same stage, before any arithmetic runs. An operator that has not yet wired an
identity authority declares the envelope with `--require-identity-binding false`, which waives the
requirement for an absent snapshot and nothing else: a wrong digest is still compared and refused.
Verification re-checks the same binding, so a caller that reports a different generation **or** a
different digest for the identity a determination was bound to is told `IdentityBindingChanged`.

Nothing in the runtime caches authority between calls. The service loads state, decides and
publishes inside one call, so a restart has no live authority to preserve and no stale
authority can survive inside a process.

## Lifecycle

An envelope identifier names a lineage; a revision is one immutable member of it.

1. **Declared** - revision 1 is created. A repeated declaration with the same idempotency key
   and payload digest is resolved as a replay and returns the original revision without
   advancing authority. A declaration that differs from an existing identifier is refused.
2. **Revised** - the current revision advances by exactly one and carries a fresh digest. A
   revision is a member of the same lineage, so it carries no `supersedes` pointer: that pointer
   names another envelope, and a pointer naming the envelope that makes it is a self-reference and
   is refused at evaluation. An expected current revision or digest that does not match is refused
   with `StaleRevision`.
3. **Retained** - superseded revisions are retained so a decision recorded against one stays
   explainable. Retention is bounded by `kMaxRetainedRevisionsPerEnvelope` (64); a decision
   naming a revision that retention has dropped is reported `EnvelopeMissing` and is never
   silently re-pointed at a newer revision.
4. **Retired** - a retirement removes the envelope from authority. A retired envelope is never
   evaluated, never revised and never re-declared.

## Persistence and recovery

A store is a directory of fixed, store-owned names. No caller-supplied string ever reaches the
filesystem, so path traversal, alternate-data-stream syntax, reserved device names and
reparse-point tricks cannot arise in the durable layer.

```text
<store-root>/
  manifest.renv      the atomic commit point
  epoch.fence        monotonic control epoch, written before each publication
  store.lock         the operating-system lock file
  segments/
    segment-<20-digit epoch>.renv
```

### Frame and segment format

A segment is a 32-byte header followed by length-prefixed frames. A frame is an 88-byte header
followed by its payload:

```text
segment header (32 bytes)
  magic "RENVSEG1" | u16 format | u16 kind | u32 reserved(0) | u64 epoch | u64 baseline
frame header (88 bytes)
  magic "RENVFRM1" | u16 format | u16 kind | u16 flags(0) | u16 reserved(0)
  u32 payload_length | u32 payload_crc32 | u64 epoch | u64 sequence | 32-byte SHA-256
```

Order of verification on every read: magic and format, reserved fields zero, declared length
within bounds and within the file, CRC-32 over the payload, then the SHA-256 payload digest, and
only then the structured decode. A frame is rejected before it is parsed.

### Commit protocol

A commit is four stages and the manifest is the only atomic step:

1. Allocate the epoch and write it to `epoch.fence` (write, flush, read back, verify). The fence
   is therefore always at least as new as any published manifest.
2. Append every frame to the delta segment at a computed offset.
3. Flush the segment and read every frame back through the same handle, verifying header,
   length, CRC-32 and SHA-256 digest against the bytes that were written.
4. Publish the new manifest by writing a temporary file, flushing it, reading it back,
   verifying it, and replacing `manifest.renv` atomically.

No reader can observe a half-published commit: the frames a manifest names are durable and
verified before the manifest names them. A crash at any point leaves either the old manifest or
the new one, and frames written past the committed sequence are unreferenced and removed on
the next open.

### Recovery

Open performs, in order: fence verification, manifest load, segment scan, replay, digest
verification and tail reconciliation.

- The manifest names exactly one authoritative generation: one optional snapshot segment and
  one optional delta segment, replayed in sequence order.
- If the recovered state digest does not equal the digest the manifest records, the store is
  refused with `StoreIntegrityMismatch`. It is never repaired and never started empty.
- If the manifest does not match the chain digest it records, the store is refused.
- If the manifest epoch is ahead of the epoch fence, the store is refused with
  `StoreRolledBack`, which detects a store directory rewound to an older manifest.
- A fence with no manifest is refused: there is nothing to recover and the directory is not
  empty.
- Bytes beyond the committed sequence are unpublished and are truncated away.
- One segment that fails its checksum or digest fails the whole open. Recovery never skips a
  frame and never merges generations.

Rollback detection is bounded by the stated threat model: it defeats accidental rewind and
partial copies, not an adversary who can rewrite the fence, the manifest and the segments
together.

### Durable format version

`kStoreLayoutVersion` is 2. Layout 2 records the journal sequence a snapshot covers, so the delta
frames of a compacted generation are counted exactly, and carries accepted usage claims in the
canonical state, so an idempotent usage retry is recognised after a compaction. A store written
under layout 1 is refused with `UnsupportedVersion`: the two layouts cannot be told apart by
inspection, and a store that cannot be read exactly is refused rather than interpreted. The frame
and segment wire format (`kStoreFormatVersion`) is unchanged, and no caller-visible API changed
shape.

### Compaction

Compaction writes the live state into a fresh snapshot segment, flushes it, reads it back,
verifies it, publishes a manifest that names it, and only then removes superseded segments. A
snapshot segment contains exactly one frame whose payload is the canonical effective state.

The manifest records the journal sequence the snapshot covers, not merely the number of frames it
holds: a snapshot folds an arbitrary number of frames into one, so the delta frames of a generation
are the sequences above that covered sequence. A generation whose snapshot covers everything
committed has no delta, and the next commit allocates a segment above every generation the manifest
names rather than reusing the snapshot's own name. Compaction therefore changes nothing a reader
can observe: the state digest, the journal sequence and the control counters are identical before
and after it, including for a store that holds no records at all. A segment that no manifest
references is never authority: it is the residue of a commit that died before it published, and it
is removed when the store is next opened for writing, before anything can append to it.

## Concurrency model

The runtime is a single-writer, multi-reader library over one store directory. It creates no
threads of its own and holds no in-process mutex. Every guarantee below is enforced by the
operating system:

- **Writer exclusion.** At most one process holds the writer lock, a real OS advisory lock on
  `store.lock`. It is released by the kernel when the process dies, so a crash cannot leave a
  store permanently locked.
- **Lock discipline.** A writer loads its own state *after* taking the exclusive lock. There is
  no read-to-write upgrade path, and no function acquires the lock twice. The only lock held
  across application code is the store file lock.
- **No callback under a library lock.** The library owns no lock other than the file lock, so
  no event sink, log sink or user callback can be invoked while an internal lock is held.
- **Publication ordering.** The manifest is replaced after the frames it names are durable and
  verified, so a concurrent reader sees one generation or the other, never a mixture.

### Deliberate design decisions in this area

- The journal sequence is a single monotonic series across snapshot and delta frames, so the
  committed frame count equals the highest committed sequence. A gap is detected as
  `StoreCorrupt` rather than tolerated.
- Committed usage is persisted as an append-only fold over deltas, so replaying a segment
  reproduces the exact committed totals and a replayed delta cannot double-count.
- State staging and durability are separate: a staged record is validated and applied to the
  in-memory state before anything is written, so a transaction the store would refuse cannot
  reach the disk and a mid-transaction failure abandons the whole critical section.

## Canonical encoding and digests

Every digested or persisted representation is produced by one canonical encoder:

- little-endian, every field width explicit, every variable-size field length-prefixed;
- collections with set semantics are emitted in a canonical order, so declaration order cannot
  change a digest;
- absent values are encoded with an explicit presence byte, so an absent value can never be
  confused with a present zero or an empty string;
- decoders are strict: unknown enum values, non-zero reserved fields, over-declared lengths,
  truncation and trailing bytes are all refused.

SHA-256 is computed over the canonical encoding under a domain tag. Nine domains are defined:
`envelope-content`, `envelope-record`, `evaluation-request`, `evidence-set`, `decision-record`,
`store-state`, `journal-frame`, `idempotency` and `grant`. The tag is length-prefixed and hashed
first, so digests for different purposes can never be confused even over identical bytes.
`resource-envelope schema` prints the current list.

### What each digest covers

| Digest | Covers | Excludes |
| --- | --- | --- |
| Envelope content digest | identity, scope, site, bindings, window, merge policy, precedence, every dimension limit | provenance text, supersedes pointer, lineage |
| Envelope record digest | everything in the content digest, plus provenance, supersedes and lineage | - |
| Request digest | scope, idempotency key, timestamp, expected revision and digest, binding resolutions, accepted classes, requested dimensions, composition | evidence set |
| Evidence digest | the evidence set, ordered canonically | - |
| Decision digest | the whole decision record with its own digest field cleared | that field |
| State digest | envelopes, decisions, observations, commitments, retirements, accepted usage claims, counters | the journal |
| Frame digest | the exact frame payload bytes | - |

## Error and refusal semantics

The library does not throw for expected failure. Failures are returned as a `Status` carrying a
stable `StatusCode` and a machine-readable `detail` string. Detail strings never contain an
absolute path, a host name or a process identifier, so a persisted record carries no host
layout. The command line tool prints one canonical JSON object and distinguishes a *refused
operation* (exit 0, with an `outcome` field) from a *failed operation* (exit 1).

## Command line

```text
resource-envelope <command> --store <directory> [options]
```

| Command | Effect |
| --- | --- |
| `declare` | Declare envelope revision 1. |
| `revise` | Declare the next revision of an existing envelope. |
| `show`, `show-revision` | Print the current or one retained revision. |
| `list` | Print every current envelope. |
| `evaluate` | Evaluate a request without recording a decision. |
| `authorize` | Evaluate a request and record the decision. |
| `verify` | Report whether a recorded decision is still authoritative. |
| `decision`, `decisions` | Print one recorded decision, or recent ones. |
| `record-usage` | Append committed usage deltas. |
| `record-observation` | Record observed measurements. |
| `committed`, `observations` | Print recorded commitments or observations. |
| `history` | Print the durable history of one envelope. |
| `retire` | Retire an envelope so it stops being authoritative. |
| `statistics` | Print store statistics, including the state and chain digests. |
| `compact` | Rewrite the store into a fresh snapshot. |
| `schema` | Print the wire format, dimension table, reason codes and digest domains. |

`--at <rfc3339>` is required for every operation that produces an authoritative decision. The
runtime never reads the system clock on a decision path, so a decision is reproducible from its
recorded inputs.

`--dimension` takes a different shape per command: an envelope declaration uses
`kind:limit[:reserved[:quantum[:class[:indexing]]]]`, a request uses
`kind:quantity[:principals[:class[:principal]]]`, and a usage or observation record uses
`kind:value[:principal[:class]]`.

### Worked example

```console
$ resource-envelope declare --store /var/lib/re --id tenant-a-envelope \
      --scope tenant --identity tenant-a --identity-generation 3 \
      --require-identity-binding false \
      --dimension power-draw-watts:100:10 \
      --idempotency-key declare-1 --at 2026-01-01T00:00:00Z
{"ok":true,"operation":"declare","replayed":false,"envelope_id":"tenant-a-envelope",
 "revision":1,"control_epoch":1,"record_digest":"...","content_digest":"..."}

$ resource-envelope record-usage --store /var/lib/re --id tenant-a-envelope \
      --dimension power-draw-watts:30:principal-a \
      --idempotency-key usage-1 --at 2026-01-01T00:00:01Z
{"ok":true,"operation":"record-usage","envelope_id":"tenant-a-envelope",
 "envelope_revision":1,"entries_appended":1,"entries_replayed":0,...}

$ resource-envelope evaluate --store /var/lib/re --scope tenant --identity tenant-a \
      --dimension power-draw-watts:50 --at 2026-01-01T00:00:02Z
{"ok":true,"operation":"evaluate","recorded":false,"outcome":"...",...}
```

The first two commands above were executed on this host and produced exactly these shapes. The
third is shown without an outcome because of known gap 1 below.

## Library integration

```cpp
#include <resource_envelope/service.hpp>

using namespace resource_envelope;

StoreOptions options;
options.mode = OpenMode::OpenOrCreate;
auto service = EnvelopeService::open(store_root, options);
if (!service.ok()) { /* refused: service.status() */ }

DeclareInput input;
input.envelope = envelope;           // id, scope, bindings, dimensions, provenance
input.idempotency_key = "declare-1";
input.requested_at = now;            // the caller's clock reading, never the system clock
const Result<EnvelopeDeclaration> declared = service.value()->declare(input);

EvaluationRequest request;           // scope, at, dimensions, evidence, bindings
const Result<AuthorizeResult> authorised = service.value()->authorize(request);
```

`evaluate(envelope, request)` and `evaluate_with_context(context, request)` are pure entry
points that touch no durable state, so an owner with its own storage can reuse the exact
semantics.

## Build, install and consume

Requires CMake 3.20 or later and a C++20 compiler. Validated with MSVC 19.44 and CMake 4.3.2.

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
cmake --install build/release --prefix /opt/resource-envelope
```

Downstream consumption uses the exported target:

```cmake
find_package(ResourceEnvelope 1.0 CONFIG REQUIRED)
target_link_libraries(my_consumer PRIVATE ResourceEnvelope::resource_envelope)
```

| Option | Default | Effect |
| --- | --- | --- |
| `RESOURCE_ENVELOPE_BUILD_TESTS` | `ON` | Build the test suite. |
| `RESOURCE_ENVELOPE_BUILD_BENCHMARKS` | `ON` | Build the benchmark driver. |
| `RESOURCE_ENVELOPE_WARNINGS_AS_ERRORS` | `ON` | Treat first-party warnings as errors. |
| `RESOURCE_ENVELOPE_ENABLE_SANITIZERS` | `OFF` | Enable Address/Undefined sanitizers when the toolchain genuinely supports them. |

The strict warning policy is applied `PRIVATE`, so no warning, hardening or sanitizer flag is
propagated to a consumer of the installed package.

## Validation performed

All results below were produced on this host. Nothing is extrapolated.

| Validation | Result |
| --- | --- |
| Debug build, MSVC `/W4 /WX /permissive- /std:c++20 /GR- /guard:cf` | clean, zero warnings |
| Release build, same policy | clean, zero warnings |
| Debug test suite | 12 of 12 suites pass, 0 failures, 70 870 checks |
| Release test suite | 12 of 12 suites pass, 0 failures, 70 870 checks |
| Randomized state machine | 480 steps over two seeds against live stores, 65 205 checks, 0 failures; the same seed reproduces the same state digest in a different store |
| Identity binding | 206 checks over tenant, service and facility scopes: a matching generation and digest succeeds, a stale generation, a stale digest, a foreign identity or an absent confirmation fails with `GenerationBinding`, and a restart or a replay cannot bypass the binding |
| Command line tool | declaration and replay of a declaration, usage recording including a release, observation recording and replay, evaluation, authorisation, verification, history, compaction, a retained revision, retirement and statistics, verified by hand against a real store directory and then pinned by `cli.test_cli_end_to_end`, which drives the built tool as a child process over that same surface |
| Decoder corruption sweeps | every truncation prefix and every single-byte flip of the envelope, decision-record, usage-delta and journal-entry encodings |
| Crash recovery | a real process killed at each of the four commit-stage boundaries, then reopened; a partial publication never becomes visible; a process killed inside the very first commit of a new store leaves a store the next commit can use |
| Cross-process exclusion | a second writer refused while another process holds the store; a killed holder does not leave the store locked |
| Property validation | more than 300 generated cases per property from a fixed seed, over canonical encoding, digest invariance, evaluation monotonicity and replay exactness, 4 578 checks |
| Durable format | store layout version 2; a store written under layout 1 is refused with `UnsupportedVersion` rather than misinterpreted |
| Downstream consumer | configured, built and executed against the installed prefix only, through `find_package` |
| Fresh clone | the committed revision cloned into an empty directory, configured, built and tested from a clean tree |
| Sanitizers | **not available.** The validated toolchain is MSVC, which provides no AddressSanitizer for this configuration; `RESOURCE_ENVELOPE_ENABLE_SANITIZERS=ON` reports that fact rather than fabricating a claim. The strongest available alternative is used: every non-Release configuration compiles with `_ITERATOR_DEBUG_LEVEL=2` and MSVC runtime checks. |

### Test suites

| Suite | Covers |
| --- | --- |
| `core.test_quantity` | canonical decimal parse and format, exact round trips, overflow and underflow, saturation, quantization, the one documented non-exact conversion |
| `core.test_digest` | SHA-256 against published FIPS 180-4 known-answer vectors, streaming equivalence, hexadecimal round trips, domain separation, unknown versus zero digest |
| `core.test_text` | UTF-8 validation (overlong, surrogate, out-of-range, truncated, embedded NUL), identifier defects, Windows reserved device names, splitting and trimming |
| `core.test_time` | RFC 3339 parse and format round trips, malformed and out-of-range timestamps, numeric offsets normalised to UTC, leap years, half-open window boundaries, saturating addition |
| `eval.test_evaluate` | reserved capacity in the residual, unknown versus zero committed usage, over-commitment reporting, undeclared dimensions, unknown bounds, quantization refusal, expiry and window boundaries, scope mismatch, revision fencing, redundancy without an operational spare, exclusive dimension without an evidenced holder, refusal precedence, digest determinism, envelope encoding round trips and corruption, merge dominance and ambiguity |
| `integration.test_service_store` | durable round trip with a stable digest across reopen, idempotent declaration and authorisation replay, idempotency conflict, revision advance and fencing, authority verification states, usage folding and replay without double counting, compaction, retirement, truncation detection, read-only refusal, hostile identifiers, and the focused regression cases for the defects listed below: a revised envelope that still evaluates, compaction followed by continued mutation, compacting an empty store, usage replay across compaction, revision and retirement, observation replay, every operation answered from its record after a retirement, a commitment released below zero, an ambiguous scope, and the digest of a recorded decision |
| `adversarial.test_hostile_inputs` | every decoder fed every truncation prefix and every single-byte flip, impossible manifest combinations, absurd declared sizes, hostile identifiers, generated segment names, the published CRC-32 check value |
| `property.test_canonical_invariants` | canonical encoding is total, injective and a fixed point after one pass; content and record digests do not depend on dimension or lineage declaration order; evaluation is total, deterministic and monotone in the requested quantity; tightening a limit never turns a refusal into a grant; declarations and authorisations replay bit for bit and append nothing |
| `recovery.test_crash_recovery` | a process killed at the fence, frame, verification and publication boundaries; an interrupted publication is never visible; a process killed inside the first commit of a new store leaves a store whose next commit is admitted, whose orphaned segment is never read as authority and whose subsequent reopen agrees; a second writer is refused across processes; a reader is never admitted alongside a writer; a killed holder does not leave the store locked |
| `machine.test_state_machine` | a seeded randomized sequence of declarations, revisions, retirements, usage records, observations, authorisations, verifications, compactions, restarts and reads against live stores, with ten core invariants checked after every step against a reference model: envelope lifecycle and revision monotonicity, idempotent replay before staleness, generation and control-epoch fencing, exact residual arithmetic, unknown never becoming zero or permitted, retire/revise/reopen semantics, durable restart equivalence, compaction followed by continued mutation, canonical digest stability and declaration-order independence, and no authority duplication |
| `cli.test_cli_end_to_end` | the built command line tool driven as a child process against a real store directory: the units every quantity is expressed in (a declared limit, a requested quantity, an observed value and a signed usage delta), usage and observation replay, the exact residual and headroom the tool reports, an unconfirmed binding refused at the generation stage, a recorded decision read back and verified with the digest that identifies it, a usage replay after a compaction, a retained revision printed by `show-revision`, retirement, and a malformed quantity refused before anything is written |
| `binding.test_identity_binding` | the identity-binding path for tenant, service and facility scopes: matching generation and digest confirmed, stale generation, stale digest, foreign identity, absent confirmation and digest-without-identifier each refused with the designed stage and reason, a waived binding waived and nothing else, service-class and policy digests checked the same way, the binding recorded in the decision, verification reporting a changed generation or digest, and a restart or replay that cannot bypass the binding |


### Proof classes

Every proof class the repository standard requires is present and was executed.

| Proof class | Where it lives | What it establishes |
| --- | --- | --- |
| Unit | `tests/core` | Quantity arithmetic, digests, text and time handling against published vectors and boundary values. |
| Evaluation semantics | `tests/eval` | Residual and headroom arithmetic, unknown versus zero, refusal precedence, windows, generations, exclusive and non-consumable cardinality. |
| Integration | `tests/integration` | Durable round trips, replay, revision fencing, usage folding, compaction, retirement, truncation detection. |
| Adversarial | `tests/adversarial` | Every decoder against every truncation prefix and every single-byte flip; impossible manifests; hostile identifiers and paths. |
| Property | `tests/property` | More than 300 generated cases per property from a fixed seed and a reported ordinal: canonical encoding is total and injective, digests are declaration-order independent, evaluation is total, deterministic and monotone, tightening a limit never grants more, and replay is exact. |
| Randomized state machine | `tests/machine` | A long randomized sequence of real service calls against live stores, with the core invariants checked after every step against a reference model. The run is a pure function of its seed, the seed is printed, and a failure reports the step and the steps that led to it, so it is reproducible exactly with `--seed` and `--steps`. |
| Identity binding | `tests/binding` | The generation and digest binding of a tenant, service or facility identity, checked at evaluation, at determination and at verification, and shown to survive neither a restart nor a replay by accident. |
| Crash recovery | `tests/recovery` | A real process is killed at each of the four commit-stage boundaries and the store is then reopened. |
| Cross-process exclusion | `tests/recovery` | A second writer is refused while another process holds the store, a reader is never admitted alongside a writer, and a killed holder does not leave the store locked. |
| End to end | `examples/consumer` | An installed-package consumer declares, records usage, authorises and reads back. |
| Command line end to end | `tests/cli` | The built command line tool is run as a child process and its printed answers are asserted, so the option surface, the units and the output shape are validated rather than remembered. |
| Benchmarks | `bench` | Completed-operation latency and throughput, with provenance labelled per line. |

#### Crash recovery detail

The interrupted commits are produced by re-executing the recovery suite under a documented
directive, `RESOURCE_ENVELOPE_ABORT_AT`, that terminates the process at a named stage boundary.
The directive never changes what is written or what is decided; it only ends the process, which is
what makes the interruption a real process death at a real boundary rather than an approximation.
It is a test and fault-injection surface and nothing else: it is inert unless it is explicitly set
in the environment, it is read only at the four commit-stage boundaries of a write, it is never
consulted on a read or decision path, and its only effect is to end the process with its own
status. A build that never sets it behaves exactly as a build that does not contain it.
The suite then asserts that the store either reproduces the last published generation exactly or
refuses to open, that a subsequent commit succeeds, and that an interrupted publication never
makes a partial generation visible. The child process is expected to exit with the directive's own
status, so the suite also proves the interruption happened where it was asked to.

#### Randomized state machine detail

The suite runs three machines. The first drives 200 steps from seed `0x5EED5EED1234ABCD`
(6 840 227 781 351 353 293), the second 120 steps from seed `0x0F1E2D3C4B5A6978`
(1 089 357 896 855 742 840), and the third runs 80 steps twice from the second seed over two
separate stores and asserts that both produce the same state digest, which is what makes a failure
a fact about the seed rather than about one interleaving. Every random choice comes from that one
generator, so a reported failure is reproduced exactly:

```sh
re_test_machine_test_state_machine --seed 6840227781351353293 --steps 200
```

A failure prints the seed, the step number, the steps that led to it, and the command that
reproduces it. No step is time-dependent: every timestamp is derived from the step number, so the
sequence is a pure function of the seed.

#### Cross-process exclusion detail

Writer exclusion is enforced by an operating-system range lock on `store.lock`, and the measured
behaviour of the platform is reported rather than assumed: on Windows a shared range lock cannot
coexist with an exclusive one, so a reader is refused while a writer holds the store. That is the
stricter of the two behaviours and the suite asserts it, because the property that matters is that
a reader is never admitted alongside a writer that could change what it is reading. A process
killed while holding the lock does not leave the store locked.

### Benchmarks

Measured with `resource-envelope-benchmark` in a Release build: single host, single process,
default optimisation. Every number is a completed operation; nothing is reported for submission
or enqueue latency. `REAL` means the operation ran against a real store directory on this host's
filesystem and includes the fence write, the segment flush, the read-back verification and the
manifest publication. `SYNTHETIC` means the inputs were constructed by the driver; no physical
facility hardware was involved and no before/after or speedup claim is made.

| Operation | Provenance | Iterations | Latency | Throughput |
| --- | --- | --- | --- | --- |
| Canonical encode envelope | SYNTHETIC | 200 000 | 403.5 ns/op | 2 478 401 ops/s |
| Canonical decode envelope | SYNTHETIC | 200 000 | 263.9 ns/op | 3 790 032 ops/s |
| SHA-256 over canonical bytes | SYNTHETIC | 200 000 | 640.5 ns/op | 1 561 221 ops/s |
| Evaluate (pure, no store) | SYNTHETIC | 200 000 | 4 667.8 ns/op | 214 233 ops/s |
| Durable declare (fence + flush + commit) | REAL | 200 | 13 207 637 ns/op | 76 ops/s |

The encoded envelope is 202 bytes. The durable figure is dominated by the two flushes and the
directory publication that the durability guarantee requires. It is reported as measured and is
not comparable to an in-memory operation.

### Downstream consumption

A standalone consumer at `examples/consumer` was configured, built and executed against the
installed prefix only, using `find_package(ResourceEnvelope 1.0 CONFIG REQUIRED)`. It declares an
envelope, records 30 W of committed usage against a 100 W limit with 10 W reserved, authorises a
50 W request, and reads the envelope back. Its output on this host:

```text
declared consumer-envelope revision 1
recorded 1 usage entries, committed total 30
decision outcome Granted reason Ok blocking none
dimension power-draw-watts limit 100 residual 60 committed 30
read back revision 1, record digest e3c90f9dca2fa00f34997ba431888eca1617edf357da44ef392f0971b95b3f3a
consumer OK
```

The residual is exactly the documented arithmetic: 100 minus 10 reserved minus 30 committed is 60.

## Hardening defects found and fixed

Each of the following was found by the tests, the command line tool or the downstream consumer
and fixed at the root cause.

1. **`ends_with` compared a substring against itself.** `std::string_view::compare` was called
   with an extra length argument after the position, which made every suffix comparison trivially
   true. This affected segment-file-name recognition and dimension text handling. Fixed to the
   two-argument form.
2. **A quantity near the declared bound was rejected by an unsigned underflow.** The range check
   subtracted the fractional part from the bound before comparing, which wrapped when the whole
   part was exactly at the bound. The comparison was reordered so that no subtraction can
   underflow.
3. **The whole-part guard truncated the declared bound.** The guard divided the bound by the
   nanounit scale, so the effective limit became the truncated quotient. The guard now bounds the
   multiplication instead, and the declared bound is applied to the scaled value.
4. **The durable state digest folded in the journal.** The documented contract is that the state
   digest covers effective values only. Including the journal made the digest depend on how a
   state was reached, so two states a reader cannot tell apart compared unequal and every
   authorisation recorded a digest that changed on replay.
5. **A satisfied request was reported `Indeterminate` with reason `InvalidArgument`, and a
   satisfied dimension was reported `NotRequested`.** Two halves of one defect: the verdict fell
   back to default-initialised values whenever no dimension produced an issue, and a per-dimension
   report kept its default outcome unless the dimension refused. A request that every dimension
   satisfied was therefore returned as an indeterminate argument error whose granted dimension
   claimed the caller had asked nothing about it. Found by the downstream consumer proof, which
   printed `Indeterminate reason InvalidArgument` alongside a residual correctly computed as 60 of
   an available 90. The verdict is now derived from the collected issues as a whole - no issue
   means every dimension was satisfied, and the result is `Granted` with reason `Ok` - and a
   dimension's outcome is set as soon as the envelope is found to declare it.
6. **Revision staging invalidated a live pointer.** `revise` held a pointer into the retained
   revision vector and then appended to that vector, which reallocated it; the observed effect was
   a revision result reporting a garbage previous revision. The previous revision and its digest
   are now copied before anything is staged.
7. **The chain digest depended on itself.** The manifest digest was computed over a manifest that
   already contained the digest being computed, so manifest verification could never succeed. The
   field is cleared before hashing.
8. **Retirements were not durable across a restart.** A snapshot does not carry the journal, so
   retirements vanished on reopen and a retired envelope could be retired again under a different
   reason. Committed retirements are now carried in the state, encoded canonically, and consulted
   before the live journal.
9. **Command line defects.** `--id` and `--dimension` were rejected as unknown options; the write
    path required a store to exist while the read path attempted to create one, so the first
    declaration could not create a store; and `record-usage` parsed the wrong option.
10. **Replay resolution compared a derived value.** The idempotency claim digest included the
    revision number, which the store derives from the authority at the time of the attempt. A
    retried request could therefore never match the record it had created, and a lost response was
    not retryable. The claim now covers exactly what the caller stated, and a revision is located
    by the idempotency key that produced it, which is recorded beside it.
11. **A retirement was refused instead of replayed.** Retiring an envelope clears its current
    revision, and the guards that refuse an already-retired envelope ran before replay resolution,
    so a retried retirement reported a missing envelope. Replay resolution now runs first, before
    every guard that would refuse the retry.
12. **Compaction corrupted the store it had just written.** Three faults met here. The commit path
    derived the delta segment's name from the commit epoch, which after a compaction is the
    snapshot segment's own name, so the first write after a compaction truncated the snapshot the
    manifest still named. The pre-flight truncation derived the same name independently and
    shortened the snapshot as well. And the truncation guard then refused the very store it had
    corrupted with a self-contradictory integrity error. Segment allocation is now decided once,
    above every generation the manifest names; the truncation helper only ever touches the delta it
    was given; and a generation whose frames all belong to its snapshot is reported as having no
    delta rather than as an inconsistency.
13. **The snapshot decoder read its own length prefix as a version.** A snapshot frame wraps the
    canonical state in a length-prefixed blob followed by the snapshot's metadata, and the decoder
    started at the blob length. Every snapshot the store wrote was refused by the store itself.
    The metadata is now decoded and cross-checked against both the state and the manifest.
14. **A reader excluded every other reader.** A read-only handle took an exclusive lock and then
    released it, re-acquiring on demand from the read path. The store is documented as single
    writer and multi reader; a read-only handle now takes a shared lock at open and keeps it. The
    measured behaviour of the platform is reported in the cross-process exclusion detail above
    rather than assumed.

The following were found by the randomized state machine and the identity-binding proof added in
the pass that closed the last two proof gaps. Each is fixed at the root cause and pinned by a
focused case; the numbers refer to the suite that covers the fix.

15. **A revision refused itself.** `revise` wrote the envelope's own identifier into the
    `supersedes` pointer. That pointer is a lineage claim about *another* envelope, and evaluation
    refuses a claim that names the envelope making it, so every envelope that had ever been revised
    was unevaluable: `revised` envelopes answered `Denied / InvalidArgument` at
    `LineageIntegrity`. The lifecycle and evaluation suites both revised without evaluating
    afterwards, which is exactly the interleaving the randomized state machine generated on its
    first run. `revise` no longer derives a lineage pointer; a caller's own lineage statement is
    preserved, as on a declaration. (`integration.test_service_store`)
16. **A compaction changed the state of an empty store.** Compaction forced the journal sequence to
    one, so a store holding no records committed to a different state after a rewrite than before
    it, and the manifest it wrote declared a snapshot longer than its committed sequence, so the
    store it had just published could not be read back at all. A compaction now leaves the sequence
    exactly as it found it. (`integration.test_service_store`)
17. **A compaction wrote delta frames into its own snapshot segment.** The manifest counted the
    frames a snapshot *holds* where it needed the sequence a snapshot *covers*, so after folding
    several frames into one frame the generation appeared to have a delta. Segment allocation then
    derived the delta's name from the snapshot's frame count and named the snapshot segment itself,
    and the next commit appended to it: the manifest named one segment as both its snapshot and its
    delta, and the store refused it afterwards with "a snapshot segment holds a non-snapshot
    frame". The delta frame count is now the committed sequence minus the covered sequence, the
    allocation is always above every generation the manifest names, a generation with no delta is
    never truncated, and a gap in the journal sequence is detected rather than tolerated.
    (`integration.test_service_store`, `machine.test_state_machine`)
18. **An unreferenced segment was appended to.** A process killed inside the very first commit of a
    new store leaves a segment that no manifest names. The next commit opened it and appended after
    the unpublished frames, so the frames the manifest counted began at the wrong offset and the
    store then refused its own state for ever with `StoreIntegrityMismatch`. Unreferenced segments
    are now removed when a writable store is opened, which is what `remove_orphan_segments`
    always claimed to do. (`recovery.test_crash_recovery`)
19. **Committed usage was double counted after a compaction.** A claim accepted by a usage record
    lived only in the journal, and a snapshot does not carry the journal. A retried record after a
    compaction was therefore appended a second time and the commitment silently doubled - the one
    thing the append-only fold exists to prevent. Accepted usage claims are now part of the
    effective state, beside the aggregate they contributed to, exactly as revision keys are.
    (`integration.test_service_store`, `machine.test_state_machine`)
20. **A usage retry became a conflict after a revision.** The claim digest covered the revision the
    store derived at the time of the attempt, so a lost response followed by a revision turned the
    retry into `IdempotencyConflict` instead of the replay a retry is. The claim now covers what
    the caller stated and nothing the store derived, which is the same lesson the declaration path
    had already learned. (`integration.test_service_store`)
21. **An observation retry was never a replay.** The claim digest covered the journal sequence and
    the entry identifier that the store assigns, so it could never match anything a caller could
    send: a retried observation was recorded again and reported `replayed: 0`. The claim now
    covers the observation the caller stated. (`integration.test_service_store`)
22. **Replay resolution ran after retirement instead of before it.** Declarations, revisions, usage
    records, observations and authorisations resolved a retry *after* the guards that refuse a
    retired envelope, so a caller retrying a request whose response it never received was refused,
    or - for an authorisation - had its request evaluated again against an authority that no longer
    existed. Replay resolution now runs first for every operation, as it already did for
    retirements. (`integration.test_service_store`, `binding.test_identity_binding`)
23. **A decision read back carried no digest.** The decision digest is excluded from the record's
    canonical payload because it is a function of the record, and nothing restored it on decode, so
    `get_decision`, `list_decisions`, `find_decision_by_key` and the history view reported an
    absent digest where the determination itself had reported a value. The decoder now restores it.
    (`integration.test_service_store`)
24. **A release below zero made its own store unloadable.** The evaluator refuses to interpret a
    commitment that aggregates below zero, and the snapshot decoder refused to *load* one, so
    recording a release larger than the commitment - which the API accepts - produced a store that
    could not be reopened. Persistence now records the value the runtime accepted; the evaluator's
    refusal to read it as capacity is unchanged. (`integration.test_service_store`)
25. **Verification ignored the digest it was given.** `verify_authority` compared the identity
    generation and ignored the identity digest, so a caller reporting that the bound identity record
    had changed was told the determination was still valid - although a changed digest invalidates a
    determination exactly as a changed generation does. Both are compared now.
    (`binding.test_identity_binding`)
26. **A usage amount was read as a raw nanounit count.** `record-usage --dimension
    power-draw-watts:30:principal-a` recorded thirty *nanounits*, a hundred-millionth of the 30 W
    the operator wrote, while a declared limit, a requested quantity and an observed value on the
    same command line are quantities in the dimension's own unit. Every commitment recorded through
    the tool was therefore wrong by nine orders of magnitude, and the residual arithmetic computed
    from it was wrong with it; the documented worked example is what made it visible. A usage
    amount is now a signed quantity in its dimension's own unit, like every other quantity the tool
    accepts, and the suite that drives the tool asserts the committed total in nanounits.
    (`cli.test_cli_end_to_end`)
27. **`show-revision` could not name a revision.** The command reads `--revision`, and
    `--revision` was not a recognised option, so the command printed its usage text instead of a
    retained revision. The option is registered and documented. (`cli.test_cli_end_to_end`)
28. **An ambiguous scope was reported as an absent one.** When two current envelopes claimed one
    scope, the resolution failure was discarded and replaced with the generic "no envelope" a
    context-only evaluation records, so the operator was told to create what already existed and the
    `DuplicateIdentity` reason was unreachable from the service. The resolution failure is now the
    reason, and the determination still names no authority and writes no record.
    (`integration.test_service_store`, `machine.test_state_machine`)

## Known gaps and unvalidated behaviour

This section is deliberately explicit. None of the following is covered by a passing test, and
nothing here should be treated as validated.

Both gaps recorded in the previous revision of this section are closed. The confirmed
identity-binding path is covered directly by `tests/binding` for tenant, service and facility
scopes - a matching generation and digest is confirmed, a stale generation, a stale digest, a
foreign identity and an absent confirmation each fail with `GenerationBinding`, and neither a
restart nor a replay bypasses the binding - and `tests/machine` drives a long randomized sequence
of interleaved declarations, revisions, retirements, usage records, observations, authorisations,
verifications, compactions and restarts against live stores, checks ten core invariants after every
step against a reference model, and reprints the seed and the steps that led to any failure. That
suite found six defects on its first runs; they are listed above. What follows is what remains
genuinely unvalidated or deliberately bounded.

1. **The POSIX implementation is unexecuted.** The file and lock layers have a POSIX branch written
   against `pwrite`, `pread`, `fsync`, `flock` and `rename`. It has never been compiled or run on
   any platform, and this repository is validated on Windows only.
2. **Crash points are stage boundaries, not arbitrary byte boundaries.** The recovery suite kills a
   process at each of the four commit stages. It does not corrupt a byte inside a frame and then
   assert recovery, because that case is covered by the adversarial decoder sweeps on the frame
   codecs rather than through a live store.
3. **`RESOURCE_ENVELOPE_ABORT_AT` is a fault-injection surface.** It is documented, it is read only
   at the four commit-stage boundaries of a write, its only effect is to end the process with its
   own status, it is never consulted on a read or decision path, and it is inert unless it is
   explicitly set. A deployment that considers any such surface unacceptable can compile it out,
   and the recovery suite would then be unable to produce its interrupted commits.
4. **A composed facility envelope's identity binding is not separately confirmable.** A request
   carries one identity snapshot, for the scope it addresses. A facility envelope named as a
   composition source is consulted at its current revision: its dimension constraints are
   evaluated and it must be current, but its own identity digest is not a second confirmation
   input, because the request shape has nowhere to carry one. This is a boundary of the request
   shape rather than an unchecked value - the revision consulted is the one the store holds, and
   any change to it advances the control epoch and fences every earlier determination - but an
   operator that needs the facility binding confirmed must address the facility envelope directly.
   `tests/binding` asserts the boundary rather than leaving it implicit.
5. **A retired envelope reports no view.** `get_envelope` answers `EnvelopeNotCurrent` for a
   retired envelope - the identifier is known, so it is distinct from `EnvelopeNotFound`, but the
   envelope has no current revision - and `list_envelopes` lists current revisions only. The
   `tombstoned` flag of `EnvelopeView` is therefore never set by either path; a tombstone is read
   through `history`. This is reported rather than changed, because the two statuses already
   distinguish "never existed" from "retired" and no consumer of either path relies on a view of a
   retired envelope.
6. **Compaction is validated at two generations and beyond, not at a chosen scale.** The compaction
   cases cover a store holding several generations, retained revisions, decisions, usage entries
   and a retirement, and the randomized suite compacts repeatedly between mutations, but no case
   compacts a store holding the maximum permitted number of records.
## Unsupported and unvalidated platforms

- **Windows is the only validated platform.** Both configurations were built and tested with
  MSVC 19.44 on Windows. The POSIX branch of the file and lock layers is implemented against
  `pwrite`, `pread`, `fsync`, `flock` and `rename`, and has never been compiled or executed.
- **No physical facility hardware was involved.** Every quantity in every test is declared or
  recorded by the test; no meter, feed or rack was read. No claim is made about physical
  hardware behaviour.
- **Sanitizers are unavailable** on the validated toolchain, as stated above.
- **Cross-platform durability differences are not measured.** `flush_directory` flushes the
  containing directory on POSIX and is a documented no-op on Windows, where durability rests on
  `FlushFileBuffers` plus a write-through rename.

## Threat model

The store is a single-host, single-writer directory. The integrity checks - CRC-32, SHA-256,
the epoch fence, the chain digest and the state digest - protect against corruption, truncation,
torn writes, partial copies, interrupted commits and accidental rewind. They do not defend
against an adversary who can rewrite every file in the directory, because a SHA-256 digest
without a secret is not an authenticator. A deployment that needs that property must place the
store on media the threat model excludes, or supply authentication outside this repository.

No telemetry is transmitted by any part of this runtime, in any build configuration.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.