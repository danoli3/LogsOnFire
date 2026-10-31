LogsOnFire 0.2.0 release candidate:

- Native C++ log ingestion, common diagnostic rules, repeated-error grouping,
  SQLite persistence, optional FTS5, and bounded JSON evidence queries.
- Python client, process capture, Bash and PowerShell launchers, and a local
  read-only HTTP API.
- Plain-text CLI/capture mode with `--output text` or `LOGSONFIRE_OUTPUT=text`,
  concise error/warning summaries, and explicit unknown/incomplete outcomes.
- A single semantic version source, maintained changelog, install guides, and a
  copyable AI-agent prompt.
- CMake installation, cross-platform build matrix, Linux packages, and generated
  Homebrew and WinGet manifests.
- A checksummed test/log tarball replayed by the unit-test workflow and every
  native package build, plus a tested project Homebrew tap formula.

This is an early heuristic analyzer. Confirm unknown outcomes with the actual
process status. Binary/compressed log containers require exporting to text.
See the README for supported inputs, resource bounds, and redaction limitations.
