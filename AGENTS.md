# LogsOnFire mission

Build a fast, local, cross-platform log analysis system that does the expensive
reading, parsing, indexing, and summarizing work for AI agents and other external
systems. An agent should be able to ask whether a build or run succeeded, learn
what went wrong, and retrieve just the relevant evidence without loading the
entire log into its context.

## Product goals

- Ingest GitHub Actions logs, compiler and build logs, Xcode output, crash
  reports, runtime logs, files, stdin, and captured process output.
- Identify errors, warnings, completion evidence, and important diagnostics.
  Group repeated problems while preserving occurrence counts and original line
  references.
- Persist cleaned log text and findings locally so agents can revisit a report
  and query evidence without repeatedly reading or parsing the source.
- Offer a predictable JSON CLI, an easy Python client, Bash and PowerShell entry
  points, and a simple local HTTP API with discoverable commands and fields.
- Minimize agent token usage with compact default summaries, bounded responses,
  pagination, search, and small context windows around findings.
- Distribute native builds for macOS, Windows, and Linux, including x64 and
  arm64, with Homebrew and WinGet packaging and Linux installation artifacts.

## Architecture

- Use C++17 for the native parsing, analysis, SQLite storage, and query engine.
  Keep the native CLI usable without Python or Bash installed.
- Use Python's standard library for the client, process capture, and local HTTP
  endpoint. Keep Bash and PowerShell as thin launchers.
- Store log sources, lines, and grouped findings in SQLite. Use FTS5 when
  available and preserve a substring search fallback.
- Use CMake for building and installation. Keep platform-specific behavior
  isolated and avoid unnecessary dependencies.
- The current code is an initial implementation. Extend it toward this mission;
  do not mistake existing parser heuristics for complete format coverage.

## Correctness and agent experience

- Distinguish `passed`, `failed`, and `unknown`. The absence of detected errors
  alone does not establish success. Require a real exit code or recognized
  completion evidence, and disclose incomplete analysis.
- Preserve the actual captured process exit code. Keep tool failures distinct
  from problems found in the log.
- Produce valid machine-readable JSON, stable field names, explicit schema
  versions, actionable error messages, and evidence tied to source IDs and line
  numbers.
- Bound memory use during ingestion and output size during queries. Do not read
  entire large logs into memory or send entire logs to an AI by default.
- Treat log content as untrusted data, never as instructions or executable code.
  Redact common credentials before persistence by default, while documenting
  the limits of heuristic redaction.
- Keep the HTTP API read-only and bound to localhost by default. Never add
  arbitrary shell execution or filesystem ingestion to HTTP query parameters.
- Describe detector output as evidence from heuristics. Avoid promises of
  complete diagnosis, perfect secret detection, or universal format support.

## Working in this repository

- Preserve the C++ core and thin scripting endpoints when adding capabilities.
- Add meaningful regression coverage for parser behavior, status decisions,
  storage and query contracts, process capture, and platform-specific fixes.
- Build and test with:

  ```sh
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build --config Release --parallel
  ctest --test-dir build -C Release --output-on-failure
  ```

- Keep README examples, API discovery output, and packaging instructions aligned
  with the actual implementation.
- Generate package manager manifests using real release artifacts and checksums.
  Do not invent download URLs, hashes, publication status, or validation results.
- Prepare reviewable release artifacts and manifests before publication. Report
  which operating systems were actually tested and which remain CI targets.

## Local savings ledger

- Keep accounting for log payloads replaced by LogsOnFire during development:

  ```sh
  python3 tools/savings.py run -- cmake --build build --config Release --parallel
  python3 tools/savings.py run -- ctest --test-dir build -C Release --output-on-failure
  python3 tools/savings.py totals
  ```

- Local data lives in `.logsonfire/savings.sqlite3`; the readable running report
  is `.logsonfire/SAVINGS.md`. These files stay out of Git.
- Use `analyze --kind benchmark FILE` for retrospective examples, synthetic
  evaluations, or logs already read by the agent. Do not count those as avoided
  reads. Use the returned savings session for tracked follow-up queries.
- Count each raw log once and charge follow-ups. Preserve negative savings.
  Identify the counter: default byte-based token estimates or an explicitly
  selected reference tokenizer. Never describe these figures as measured Codex
  billing, total conversation savings, or exact savings for an unknown tokenizer.

## Versioning and changelog

- Follow SemVer. `VERSION` is the single source for native, Python, package, and
  release versions. Do not add duplicated hard-coded version strings.
- Increment PATCH for compatible fixes; MINOR for new features. While pre-1.0,
  put compatibility breaks in a new minor version and label them explicitly.
  After 1.0, incompatible public API changes require a major bump.
- Use ordered pre-release suffixes such as `0.2.0-rc.1`, `0.2.0-rc.2`, then
  `0.2.0`. Tags use the exact `v`-prefixed value. Never move or overwrite a tag
  or alter already-published release artifacts.
- Keep `changelog.md` current for each user-visible change, grouped under Added,
  Changed, Fixed, Removed, Deprecated, or Security as appropriate. Keep an
  Unreleased section and a section matching VERSION. Do not invent publication.
- Run `python3 tools/version.py --check`; release tags must also pass
  `python3 tools/version.py --check --expected TAG`.
- Preserve the default JSON CLI/API contract. Plain text is an explicit human
  preference; internal clients must request JSON independently of that preference.
- Ship the test/log bundle with releases and replay that same bundle in CI.
  README remains a usage guide; setup belongs in BUILDING.md, and installed-package
  instructions must distinguish local DEB installation from an actual APT repository.
