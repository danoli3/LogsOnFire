Initial LogsOnFire release:

- Native C++ log ingestion, common diagnostic rules, repeated-error grouping,
  SQLite persistence, optional FTS5, and bounded JSON evidence queries.
- Python client, process capture, Bash and PowerShell launchers, and a local
  read-only HTTP API.
- CMake installation, cross-platform build matrix, Linux packages, and generated
  Homebrew and WinGet manifests.

This is an early heuristic analyzer. Confirm unknown outcomes with the actual
process status. Binary/compressed log containers require exporting to text.
See the README for supported inputs, resource bounds, and redaction limitations.
