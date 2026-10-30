# Local savings accounting

```sh
python3 tools/savings.py run -- cmake --build build --parallel
python3 tools/savings.py run -- ctest --test-dir build --output-on-failure
python3 tools/savings.py totals
```

Also available through `logsonfire-agent savings ...` and the PowerShell wrapper.
The local ledger is `.logsonfire/savings.sqlite3`; a readable running report is
`.logsonfire/SAVINGS.md`. Both stay out of Git. Captured development evidence is
stored in `.logsonfire/development.db` by default.

Successful runs without warnings print a compact verdict. Failed runs and warning
reports retain diagnostics. The captured command's exit code is preserved.

## What is counted

- `usage` records a declared replacement read: software processed the log and a
  report was printed for the agent instead of its raw contents.
- Each raw SHA-256 is credited once per counter. Re-analyzing identical content
  reuses that baseline; every subsequent report is charged.
- `net_saved_tokens = baseline_tokens - delivered_tokens`. Negative savings
  remain visible. Sessions without recorded deliveries do not contribute.
- The charged response is the exact compact UTF-8 JSON printed, including its
  `savings_session` receipt and newline.
- Counters and benchmark totals remain separate.

This is payload accounting, not Codex telemetry or provider billing. It excludes
tool protocol framing, unrelated prompts/tool calls, reasoning, cache effects,
and accounting-summary reads. It assumes declared reports replace raw-log reads;
it cannot independently observe an agent's context. Previously-read logs are
benchmarks, not avoided reads.

## Follow-ups and benchmarks

Use the returned session ID for subsequent queries:

```sh
python3 tools/savings.py query SESSION_ID explain
python3 tools/savings.py query SESSION_ID context --line 42 --after 6
python3 tools/savings.py analyze --kind benchmark examples/ci-linux-crash.log
```

Benchmark totals are excluded from usage savings. Re-running a benchmark charges
another response against its single baseline; use a different `--ledger PATH`
for independent experiments.

## Accuracy

Default `estimate:utf8_bytes_div_4` uses `ceil(UTF8_bytes / 4)`. This is a rough
estimate and can be inaccurate for Unicode text, paths, and repetitive output.

Optional named reference encodings use [tiktoken](https://github.com/openai/tiktoken):

```sh
python3 -m pip install tiktoken
python3 tools/savings.py --counter o200k_base run -- cmake --build build
```

`cl100k_base` is also supported. Counts are exact for the named encoding applied
to decoded log/report text; the runtime model's tokenizer and billing remain
unknown. First use may download the vocabulary. Reference counting is capped at
8 MiB per input to bound memory; use estimates for larger inputs. Follow-up
queries reuse their session's counter.

## Privacy

The ledger stores hashes, counts, labels, timestamps, and source references, not
raw log bodies or report bodies. Native storage retains cleaned/redacted evidence.
Accounting-enabled `run` privately spools raw output while counting, removes the
temporary file afterward, and does not echo it to the agent. POSIX ledger files
have owner-only permissions; Windows uses the parent directory's ACLs.
`--ledger PATH` or `LOGSONFIRE_SAVINGS_DB` relocates the ledger.
