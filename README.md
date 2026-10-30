# LogsOnFire

Read a large log once. Ask for a small report, then query the evidence you need.

LogsOnFire is a local C++17 log analyzer with SQLite storage, a JSON CLI, a Python
client, Bash and PowerShell launchers, and a read-only HTTP API. It detects common
GitHub Actions annotations, GCC/Clang/MSVC diagnostics, CMake/Make/Ninja failures,
Xcode results, test failures, Python tracebacks, runtime errors, sanitizer reports,
and Apple crash indicators.

This is an initial implementation. Detectors are heuristics. A report describes
the evidence found; it cannot guarantee that a build or program is correct.

## Drop in a log: “what's wrong?”

```sh
build/logsonfire analyze examples/github-actions-failed.log --human
```

LogsOnFire reads and saves the log, explains the strongest diagnostics, suggests
what to check, and shows a small evidence window. In the included example, it
surfaces the missing `sqlite3.h` header before the generic CI failure messages.
Omit `--human` for compact JSON an AI agent can consume directly.

Use `logsonfire analyze YOUR_LOG.txt` for your own text log, or `logsonfire explain
--source ID` to revisit saved evidence. See the [complete user/agent example](examples/README.md).

## Build

Requires CMake 3.20+, a C++17 compiler, and SQLite 3.24+ development headers/libraries.
Python 3.9+ is required for tests, command capture, and HTTP; the native CLI runs
independently.

```sh
# macOS dependencies
brew install cmake sqlite python

# Debian / Ubuntu dependencies (alternative to the command above)
sudo apt-get install cmake g++ libsqlite3-dev python3

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure

# Optional: install to a prefix you own
cmake --install build --config Release --prefix "$HOME/.local"
```

On Windows, use Visual Studio's C++ build tools and [vcpkg](https://github.com/microsoft/vcpkg):

```powershell
vcpkg install sqlite3:x64-windows-static
cmake -S . -B build -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix "$PWD/stage"
```

Use `arm64-windows-static` and `-A ARM64` on an ARM64 Windows machine. CI targets
macOS, Linux, and Windows on x64 and arm64. See [distribution instructions](docs/DISTRIBUTION.md)
for package formats and release preparation. Homebrew and WinGet publication has
not happened yet.

## Analyze existing logs

Examples use `build/logsonfire`; after installation, use `logsonfire`.

```sh
build/logsonfire ingest build.log
build/logsonfire summary
build/logsonfire issues --source 1 --severity error --limit 5
build/logsonfire context --source 1 --line 42 --before 3 --after 6
build/logsonfire search --source 1 --text "undefined reference"
build/logsonfire sources --limit 10

# Supply a known process outcome when available
build/logsonfire ingest xcode.log --exit-code 65 --name "iOS build"

# stdin works with any log producer, including the GitHub CLI
gh run view RUN_ID --log-failed | build/logsonfire ingest - --name "CI failure"
```

The final pipeline command above analyzes the text it receives. Use `set -o pipefail`
when the producer's failure should also fail your shell pipeline. For capturing a
command's actual exit status automatically, use `logsonfire-agent run` below.

Each ingestion creates a new source, even for repeated input. Queries default to
the latest committed source; pass `--source ID` when coordinating concurrent
agents. The original log is never modified. Cleaned, redacted text and original
physical line numbers are persisted. The default database is `.logsonfire/logs.db`
in the working directory. Override it with `--db PATH` or `LOGSONFIRE_DB`.

A summary looks like this (timestamps and storage details omitted here):

```json
{
  "schema_version": 1,
  "source_id": 1,
  "status": "error",
  "outcome": "failed",
  "reason": "Error evidence detected",
  "errors": 17,
  "warnings": 0,
  "issue_groups": 1,
  "top_issues": [{
    "id": 1,
    "severity": "error",
    "rule": "linker.error",
    "message": "ld: undefined reference to foo",
    "message_truncated": false,
    "first_line": 42,
    "last_line": 91,
    "count": 17
  }]
}
```

`errors` and `warnings` count matching lines. `issue_groups` counts deduplicated
diagnostics. Timestamps and hexadecimal addresses are normalized for grouping;
source locations and other numbers are preserved. Findings are grouped within a
source. Each line receives one primary classification.

## Capture commands

```sh
# From the source checkout; discovers build/logsonfire automatically
scripts/logsonfire-agent run -- make -j8
scripts/logsonfire-agent run --name "Xcode build" -- xcodebuild -project App.xcodeproj

# For agents: suppress live output and read only the JSON report
scripts/logsonfire-agent run --quiet -- python3 -m unittest

# Shell syntax must be explicitly passed to a shell
scripts/logsonfire-agent run -- bash -c 'make clean && make -j8'
```

The wrapper streams merged stdout/stderr to the C++ parser. It writes no temporary
raw log. Live output goes to **stderr**; one JSON report goes to **stdout**. The
wrapper returns the captured process's exit code. If capturing or saving fails,
it returns a tool error instead. On POSIX, a signal maps to `128 + signal`.
Windows exit codes above 255 are clamped to 255.

On Windows, use `scripts/logsonfire-agent.ps1 run --quiet -- COMMAND ...` or run
`python python/logsonfire.py run --quiet -- COMMAND ...`. After installation,
the wrappers live in `bin`. Use `LOGSONFIRE_BIN` to choose a specific native binary
and `PYTHON` to select the wrapper's Python interpreter.

## Python client

Add the checkout's `python` directory (or the installed `share/logsonfire`
directory) to your Python import path:

```python
from logsonfire import Client

logs = Client(binary="build/logsonfire", db=".logsonfire/logs.db")
report = logs.ingest("build.log", name="Linux build", exit_code=1)
report = logs.summary(report["source_id"])
if report["outcome"] == "failed":
    for issue in logs.issues(report["source_id"], limit=3)["items"]:
        print(logs.context(report["source_id"], line=issue["first_line"]))

code, report = logs.capture(["cmake", "--build", "build"], quiet=True)
```

Queries return dictionaries. Analysis failures are report data; operational
failures raise `LogError`. `Client.query()` also exposes native commands directly.

## HTTP API

```sh
scripts/logsonfire-agent serve --port 8765
curl 'http://127.0.0.1:8765/v1/summary'
curl 'http://127.0.0.1:8765/v1/issues?source=1&limit=3'
curl 'http://127.0.0.1:8765/v1/context?source=1&line=42&after=6'
curl --get 'http://127.0.0.1:8765/v1/search' --data-urlencode 'text=undefined reference'
curl 'http://127.0.0.1:8765/v1/schema'
```

The API binds to `127.0.0.1` and exposes read-only queries against the configured
database. Use `logsonfire-agent --db PATH serve` to select the database. Ingestion
and command execution happen through CLI/Python. The HTTP service does not accept
file paths, shell commands, or arbitrary database overrides. No CORS access is
enabled. This API is intended for trusted programs on the same machine.

See [the API contract](docs/API.md) and the discoverable `/v1/schema` endpoint.

## Status and exit codes

| Outcome | Meaning | `check` exit |
| --- | --- | --- |
| `passed` | Exit code 0 or a recognized success marker, with no detected errors and no oversized lines | 0 |
| `failed` | An error pattern was detected or a supplied/captured exit code is nonzero | 1 |
| `unknown` | Completion evidence is missing or oversized lines prevented complete analysis | 2 |

`status` is `ok`, `warning`, `error`, or `unknown`. Warnings can accompany either
a passed or unknown outcome, so agents should branch on **`outcome`**. A successful
process may still produce an error finding and a `failed` report.

`ingest` and ordinary query commands return 0 when the operation succeeds, even
when the report contains errors. Invalid requests and tool failures return 2 with
an `error` object. Use `check` when CI should fail based on the analysis.
`logsonfire-agent run` returns the process exit code, independently of findings.

## Resource use and privacy

- Ingestion streams input and commits each source atomically. A source becomes
  visible after ingestion completes. WAL permits readers to query older committed
  sources during an ingestion; concurrent writers serialize, with a 30-second
  busy timeout.
- Physical lines are capped at 64 KiB. Excess bytes are consumed and counted;
  `truncated_lines` discloses the loss. Invalid UTF-8 is replaced and ANSI/control
  sequences are removed. This is cleaned evidence, not a byte-for-byte archive.
- Summaries include at most five issues. Other lists default to ten items and
  accept at most 100. JSON responses stay within 32 KiB. Long messages and context
  lines are clipped with explicit truncation flags. Follow `next_offset` or
  `next_line` when more data is needed.
- SQLite FTS5 indexes the persisted text when available. `search --text` is a
  literal substring search (ASCII case-insensitive). `--fts` opts into SQLite's
  token-based FTS query syntax. FTS5 availability depends on the SQLite build.
- Common assigned secrets, bearer/basic authorization values, GitHub tokens,
  AWS access key IDs, URL credentials, and PEM private keys are redacted before
  storage. This is heuristic redaction, not a guarantee that all sensitive data
  is removed. Review logs before sharing. `--no-redact` explicitly disables it.
- Live command output is relayed unchanged to stderr unless `--quiet` is used.
  The database is local; LogsOnFire makes no model calls and sends no telemetry.
  POSIX-created files use private permissions. Windows uses the directory's ACLs.
- Storage grows with input. There is no automatic retention policy yet. Stop
  active writers before moving or deleting a database; use SQLite's backup tools
  for a live database, including its WAL.

## Scope and extension

Textual `.crash` files and recognizable Apple `.ips` indicators are supported as
text. This version does not decode `.xcresult`, `.xcactivitylog`, minidumps,
compressed archives, GitHub ZIP downloads, or complete Apple JSON structures.
Export/decompress these formats to text first. Large JSON lines may be truncated.
All detectors run regardless of `--format`; that flag labels the source rather
than selecting an exclusive parser.

Rules currently live in `src/main.cpp`; add representative regression cases in
`tests/test_system.py` when extending them. Structured format adapters, configurable
rules, root-cause linking, retention, and additional integrations are useful next
steps. The project mission is recorded in [AGENTS.md](AGENTS.md).
