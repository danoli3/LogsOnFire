# Does LogsOnFire help Codex diagnose a crash?

Yes for finding and communicating relevant evidence. It still needs an agent or
developer to investigate the underlying code.

This evaluation used a real failed LogsOnFire CI job and a small synthetic Apple
crash report. The CI case was a retrospective check: the failure had already been
inspected before running this evaluation.

## Reproduce

```sh
python3 examples/evaluate.py --binary build/logsonfire \
  examples/ci-linux-crash.log examples/macos-crash.crash

build/logsonfire analyze examples/ci-linux-crash.log --human
build/logsonfire analyze examples/macos-crash.crash --human
```

The real log was captured from the failed Linux x64 test job in
[the first CI run](https://github.com/danoli3/LogsOnFire/actions/runs/41139432197/job/124349806800).
The Apple report is explicitly synthetic and contains no user's crash data.

## Observed sizes

These are UTF-8 byte counts of the files and compact JSON responses, using the
included evaluation script. They are not tokenizer measurements.

| Input | Raw log | Status summary | Explanation with evidence |
| --- | ---: | ---: | ---: |
| Real Linux CI failure, 46 lines | 5,396 bytes | 1,686 bytes | 2,632 bytes |
| Synthetic Apple crash, 16 lines | 802 bytes | 1,159 bytes | 2,397 bytes |

The CI explanation was 51.2% smaller than the original log. The tiny crash report
grew because structured fields, interpretations, and evidence windows add overhead.
This tool does not make every short log cheaper to read.

## What helped in the real failure

The explanation selected `AssertionError: -11 != 0` as the primary diagnostic and
included `test_long_diagnostic_and_credential_lines` among its findings. Its
context showed the assertion checking `result.returncode`. That gave Codex the
specific failing test and the evidence needed to interpret `-11` as a subprocess
SIGSEGV on POSIX.

LogsOnFire itself reported an assertion failure. It did not infer a native stack
trace or prove the cause of the segmentation fault. Source investigation led to
the fix: bound regex matching windows and replace credential regex replacements
with direct scanning. Both Linux architectures passed the subsequent CI run's
test and packaging jobs.

The evaluation also caught a misleading detector match against an echoed Python
string containing `"error: "`. That match was removed, and a regression test now
keeps echoed error text from displacing the actual assertion diagnostic.

## What helped in the Apple fixture

The report surfaced `EXC_BAD_ACCESS (SIGSEGV)`, the termination evidence, and the
crashed thread. An initial context window stopped before the stack frames, so
`explain` now adds `crash_context`: up to eight lines at a nearby crashed-thread or
stack-trace header. The fixture's `parse_record`, `load_document`, and `main`
frames are therefore available in the first response.

This identifies where to investigate. It does not establish why the invalid
memory access happened. Four matching error lines in the fixture describe one
crash; they are not four separate crashes.

## Practical assessment

For Codex, the useful parts are a consistent outcome, prioritized evidence,
source/line references, reusable storage, and targeted follow-up queries. These
reduce navigation and repeated reading. Remaining work includes incident-level
grouping, more precise diagnostic categories, smaller response modes, broader
format coverage, and measurements on larger real logs with actual tokenizers.
