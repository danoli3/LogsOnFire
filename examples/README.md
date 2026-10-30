# Drop in a GitHub Actions log and ask “what's wrong?”

From the repository root, try the included sample:

```sh
build/logsonfire analyze examples/github-actions-failed.log --human
```

For your own downloaded GitHub Actions text log:

```sh
logsonfire analyze /path/to/job-log.txt --human
```

The sample contains a missing SQLite header followed by generic build failures.
LogsOnFire reads the file, saves the evidence, and responds along these lines:

```text
The build or run failed. A required header, module, or package could not be found.
Source: github-actions-failed.log (ID 1, 13 lines)
4 error lines, 0 warning lines, 4 distinct issues.

Start here (log line 8, dependency.missing):
  .../src/main.cpp:1:10: fatal error: sqlite3.h: No such file or directory
  Check: Check that the dependency is installed in the failing job and that its include or module search paths are configured.
  Check: For sqlite3.h on Debian/Ubuntu, check the libsqlite3-dev package; other platforms need their SQLite development package.
```

The actual output also includes a small evidence window and a command for deeper
inspection. Specific diagnostics are ranked ahead of generic failure markers;
the ranking is a starting point for investigation, not proof of a root cause.

## The same interaction through an AI agent

1. The user attaches a log and asks “what's wrong?”
2. The external agent passes the attached file's path to `logsonfire analyze PATH`.
3. LogsOnFire returns compact JSON containing `answer`, `outcome`, up to three
   `findings`, `suggested_checks`, and a small `context` window. The agent does
   not need to load the raw file into its prompt.
4. The agent communicates those findings to the user. It can request more
   context by source ID and line number if the evidence needs clarification.

LogsOnFire performs this analysis locally with deterministic rules. It does not
make an LLM call. An external agent can phrase the result conversationally using
the compact report as evidence.

```sh
# Structured response for tools: omit --human
build/logsonfire analyze examples/github-actions-failed.log

# Revisit the saved result without reading the file again
build/logsonfire explain --source 1 --human

# Or obtain the explanation through the read-only local API
curl 'http://127.0.0.1:8765/v1/explain?source=1'
```

Python integration:

```python
from logsonfire import Client

logs = Client(binary="build/logsonfire")
report = logs.analyze("examples/github-actions-failed.log")
print(report["answer"])
for finding in report["findings"]:
    print(f"Log line {finding['first_line']}: {finding['message']}")
    for check in finding["suggested_checks"]:
        print(check)
```

The fixture is synthetic and safe to share. It illustrates a real class of CI
failure without containing credentials or relying on a live GitHub run.

## Evaluate crash analysis

`ci-linux-crash.log` is a real public CI failure from this project's first Linux
run. `macos-crash.crash` is a synthetic Apple crash report. Compare their raw sizes
with the analyzer's reports:

```sh
python3 examples/evaluate.py --binary build/logsonfire \
  examples/ci-linux-crash.log examples/macos-crash.crash
```

See [the evaluation report](../docs/EVALUATION.md) for what helped Codex, what it
could not determine, and why very small inputs can produce larger JSON reports.
