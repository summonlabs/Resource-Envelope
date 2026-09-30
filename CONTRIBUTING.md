# Contributing to Resource Envelope

Resource Envelope is a C++20 library and toolset for representing, validating, and exchanging resource
envelopes: the identity, generation, digest, and lifetime metadata of resources that move between
processes and machines.

This document covers how to build, test, and contribute to the project, and the terms under which
contributions are accepted. Open an issue before starting anything substantial.

## Ways to contribute

- Bug reports with a minimal reproduction: compiler, platform, exact command, observed result.
- Documentation fixes, including unclear or outdated build instructions.
- Tests that pin down untested behaviour or reproduce a fixed bug, and implementation changes that
  fix a defect or implement an accepted issue.

## Building

Requirements: CMake 3.25+, a C++20 compiler (MSVC 19.30+, GCC 13+, or Clang 16+), Ninja or the Visual
Studio generator, and a standard library with `<bit>`, `<span>`, `<string_view>`, and `<=>`.

Everything is configured through CMake presets; no step should require hand-editing generated files.

```sh
cmake --preset dev            # configure
cmake --build --preset dev    # build
ctest --preset dev            # run the full test suite
```

`release` presets build optimised with the same warning policy; by hand, use
`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_STANDARD=20`.

### Warning and conformance policy

Every translation unit must compile cleanly under the strictest settings the compiler offers, and
warnings are errors in every build type, release and CI included.

- MSVC: `/W4 /WX /permissive- /std:c++20 /Zc:__cplusplus /Zc:preprocessor`
- GCC and Clang: `-Wall -Wextra -Wpedantic -Werror -std=c++20`

Do not silence a diagnostic with a local pragma unless the warning is genuinely wrong for that code;
never add `-Wno-*` or `/wd` flags to make a change pass, and never weaken the warning level.

## Testing

```sh
ctest --preset dev --output-on-failure
```

- Run the full suite locally before opening a pull request; CI repeats it on MSVC, GCC, and Clang.
- Tests must be deterministic: no sleeps, no wall-clock assumptions, no network access, no reliance on
  unordered iteration order, no dependence on the current working directory.
- Test data lives in the repository and is versioned with the tests; nothing is fetched at test time.
- A failing test is never deleted, skipped, or weakened to make a branch green: fix the behaviour, or
  explain in the pull request why the expectation itself was wrong. Keep new tests next to the code
  they cover and follow the existing naming and discovery rules.

## Code quality expectations

### No placeholders

Do not commit `TODO`, `FIXME`, `XXX`, or "not implemented" markers, stubs that silently succeed, or
dead code kept "for later". Documented limitations are welcome; unfinished implementation is not.

### Deterministic behaviour

Given identical inputs, the library must produce byte-identical observable results on every run, on
every supported platform, and in every configuration. Avoid uninitialised reads, dependencies on
unspecified evaluation order, address- or pointer-dependent output, locale-dependent formatting, and
unordered iteration wherever order is observable.

### Strong typing for identities, generations, and digests

Identities, generations, digests, and similar values are distinct types, not raw integers or strings.
Use scoped enums, strong typedefs, or dedicated class types to make invalid states unrepresentable; a
generation is not an index, and a digest is not a string.

### Canonical serialization for digested data

Anything hashed, signed, or compared across processes or platforms must be serialized canonically
first: one documented byte order, explicit field order, no struct padding or compiler-layout
dependence, no locale- or implementation-defined text encoding, and never the raw memory of a struct.
When a serialized format changes, update its specification and tests in the same pull request and say
so in the commit message.

### No telemetry

The project performs no telemetry, analytics, crash reporting, or background network activity. It must
not phone home or require an account. Do not add code that collects or transmits usage data.

### Dependencies

Prefer the standard library. Do not add a third-party runtime dependency without prior discussion in an
issue, and never add one that a consumer of the library would be forced to link. Build-time tools are
acceptable when widely available and version-pinned; record any approved dependency and its license.

## Proof obligations

Every change that alters observable behaviour must carry its proof in the same pull request:

- Unit tests for the new or changed behaviour, including boundary values and error paths.
- Property tests for anything with an algebraic or round-trip contract: encode/decode,
  serialize/parse, normalise/idempotence.
- Adversarial tests for input validation: truncated, oversized, contradictory, or maliciously crafted
  inputs must fail cleanly and never crash, leak, or invoke undefined behaviour.
- A regression test for every bug fix, failing before the fix and passing after it.

State what you ran and what you observed in the pull request. "It compiles" and "it looks correct" are
not proof; if a claim cannot be tested automatically, explain how you verified it.

## Commit messages

Commit messages are public and permanent. Write them for a reader with no access to the discussion
that produced the change.

- Imperative subject, 72 characters or fewer, no trailing period: `Fix generation wraparound in
  envelope validation`. Capitalise it; no conventional-commit prefixes, emoji, or release notes.
- Add a body when the reason is not obvious from the subject: explain why, not what. Reference issues
  or pull requests where that helps a future reader. Keep messages neutral and factual.
- No attribution trailers of any kind: no `Co-authored-by:`, no `Generated-by:`, no tool signatures.
  The commit is authored by the person who submits it.
- Do not rewrite published history; keep commits focused, and separate refactors from behaviour changes.

## Contribution terms

- Contributions are accepted under the Apache License, Version 2.0, the project's license. See
  `LICENSE` and `NOTICE`.
- No separate contributor license agreement (CLA) and no copyright assignment is required; you keep
  the copyright to your contribution.
- By submitting a pull request you confirm that you have the right to license the contribution under
  Apache-2.0, that you wrote it or otherwise have permission to submit it, and that it does not
  knowingly include code from an incompatible source.

## Reporting security issues

Do not report security-relevant issues in public issues, pull requests, or discussions. Use GitHub
private vulnerability reporting on the repository (Security tab, "Report a vulnerability"); if that is
unavailable, contact the maintainers privately before writing anything in public.

Include the affected version, a reproduction, and the impact. We will acknowledge the report and credit
you in the fix unless you prefer otherwise; please allow reasonable time before public disclosure.

## Review

Pull requests are reviewed for correctness, determinism, evidence, and fit with the project's direction.
Expect questions about edge cases and about the proof behind claims.
