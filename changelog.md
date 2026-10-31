# Changelog

Notable changes are recorded here using [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
`VERSION` is authoritative. A version entry describes source changes; publication
is established by its GitHub tag/release, not by this file alone.

## [Unreleased]

## [0.2.0-rc.1] - 2026-10-31

### Added

- Concise plain-text output selected by `--output text` or `LOGSONFIRE_OUTPUT=text`,
  including warning counts, actionable errors, and unknown/incomplete outcomes.
- A version source file, version/changelog consistency checks, and versioning
  guidance for contributors and future agents.
- Linux pre-release packages, a generated Homebrew formula, and a compressed
  bundle of regression tests and sample logs replayed by CI.
- Guides for installed Homebrew and Linux DEB usage.
- Copyable AI-settings prompt with linked usage/install/API guides, plus separate
  build and unit-test workflow badges in the README.

### Fixed

- Python clients and HTTP queries explicitly request JSON, remaining structured
  when a human-facing CLI preference selects plain text.

## [0.1.0] - 2026-10-31

Initial development baseline; no published release is implied.

### Added

- Streaming C++/SQLite log ingestion, repeated diagnostic grouping, bounded JSON
  queries, explanations, text context, search, and heuristic secret redaction.
- GitHub Actions text, compiler/build tools, Xcode, Android logcat/Studio,
  VS Code, exception, sanitizer, and crash-indicator coverage.
- Python client, command capture, Bash/PowerShell launchers, local read-only HTTP
  API, and local savings accounting with separate benchmarks and signed estimates.
- Native build/test/package CI for macOS, Windows, and Linux on x64 and arm64.
- Usage/support matrices and a separate BUILDING.md guide.

### Fixed

- Long-line Linux regex crashes, zero-failure CTest false positives, Windows SDK
  macro collisions, UTF-8 excerpt boundaries, full Windows crash exit codes,
  and Windows output-byte accounting.
