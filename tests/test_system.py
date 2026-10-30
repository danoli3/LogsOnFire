"""End-to-end contracts against the actual native binary; no mock parser."""
import concurrent.futures
import io
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "python"))
from logsonfire import Client, make_handler  # noqa: E402


class SystemTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.db = str(Path(self.temp.name) / "logs.db")
        self.client = Client(db=self.db, binary=BINARY)

    def cli(self, *args, data=None, code=0):
        result = subprocess.run([BINARY, "--db", self.db, *args], input=data,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(result.returncode, code, result.stdout.decode(errors="replace"))
        self.assertLessEqual(len(result.stdout), 32768)
        return json.loads(result.stdout)

    def ingest(self, text, *args):
        if isinstance(text, str):
            text = text.encode()
        return self.cli("ingest", "-", *args, data=text)

    def test_github_duplicates_and_context(self):
        report = self.ingest("setup\n2026-10-31T01:02:03Z ##[error]undefined reference to foo\n"
                             "2026-10-31T01:02:04Z ##[error]undefined reference to foo\nfinished\n")
        self.assertEqual(report["outcome"], "failed")
        self.assertEqual(report["format"], "github")
        self.assertEqual(report["errors"], 2)
        self.assertEqual(report["issue_groups"], 1)
        issue = self.client.issues()["items"][0]
        self.assertEqual((issue["first_line"], issue["last_line"], issue["count"]), (2, 3, 2))
        self.assertEqual(len(self.client.context(line=2)["items"]), 4)
        self.cli("check", code=1)

    def test_drop_github_log_and_ask_whats_wrong(self):
        report = self.client.analyze(ROOT / "examples/github-actions-failed.log")
        self.assertEqual(report["outcome"], "failed")
        self.assertEqual(report["analysis"], "rule_based")
        self.assertIn("could not be found", report["answer"])
        primary = report["findings"][0]
        self.assertEqual(primary["rule"], "dependency.missing")
        self.assertIn("sqlite3.h", primary["message"])
        self.assertTrue(any("libsqlite3-dev" in check for check in primary["suggested_checks"]))
        self.assertTrue(any(row["line"] == primary["first_line"] for row in report["context"]))
        self.assertEqual(self.client.explain(report["source_id"])["answer"], report["answer"])
        result = subprocess.run([BINARY, "--db", self.db, "explain", "--human"], capture_output=True)
        self.assertEqual(result.returncode, 0)
        self.assertIn("Start here (log line 8, dependency.missing)", result.stdout.decode())
        self.assertIn("sqlite3.h", result.stdout.decode())

    def test_explain_unknown_failure_without_diagnostic_and_passed(self):
        self.ingest("still compiling\n")
        report = self.client.explain()
        self.assertEqual(report["outcome"], "unknown")
        self.assertTrue(report["suggested_checks"])
        self.ingest("stopped\n", "--exit-code", "9")
        report = self.client.explain()
        self.assertEqual((report["outcome"], report["exit_code"]), ("failed", 9))
        self.assertEqual(report["findings"], [])
        self.assertTrue(report["suggested_checks"])
        self.ingest("** BUILD SUCCEEDED **\n")
        report = self.client.explain()
        self.assertEqual(report["outcome"], "passed")
        self.assertEqual(report["findings"], [])
        self.assertIn("successfully", report["answer"])

    def test_explain_prioritizes_error_before_warning(self):
        self.ingest("warning: unused variable\nFAILED: app.o\nmain.cpp:2:3: error: syntax error\n")
        self.assertEqual(self.client.explain()["findings"][0]["rule"], "compiler.error")
        self.cli("summary", "--human", code=2)

    def test_real_ci_crash_log_retains_assertion_and_failing_test(self):
        report = self.client.analyze(ROOT / "examples/ci-linux-crash.log")
        self.assertEqual(report["outcome"], "failed")
        self.assertIn("AssertionError: -11", report["findings"][0]["message"])
        self.assertTrue(any("test_long_diagnostic_and_credential_lines" in item["message"] for item in report["findings"]))
        self.assertTrue(any("result.returncode" in row["text"] for row in report["context"]))

    def test_apple_crash_report_surfaces_crashed_thread_frames(self):
        report = self.client.analyze(ROOT / "examples/macos-crash.crash")
        self.assertEqual(report["outcome"], "failed")
        self.assertEqual(report["findings"][0]["rule"], "crash.signal")
        self.assertIn("SIGSEGV", report["findings"][0]["message"])
        self.assertTrue(any("parse_record" in row["text"] for row in report["crash_context"]))
        self.assertTrue(any("Thread 0 Crashed" in row["text"] for row in report["crash_context"]))

    def test_analyze_stdin_is_bounded(self):
        report = self.cli("analyze", "-", data=("error: " + '"' * 60000 + "\n").encode())
        self.assertEqual(report["outcome"], "failed")
        self.assertTrue(report["findings"][0]["message_truncated"])
        self.assertTrue(report["context"][0]["truncated"])

    def test_xcode_success_with_warning(self):
        report = self.ingest("xcodebuild -project app.xcodeproj\n/a.cpp:4:2: warning: unused variable\n** BUILD SUCCEEDED **\n")
        self.assertEqual((report["format"], report["status"], report["outcome"]), ("xcode", "warning", "passed"))
        self.assertEqual(report["warnings"], 1)
        self.cli("check")

    def test_unknown_is_not_healthy(self):
        report = self.ingest("compiling source\n0 errors, 0 warnings\n")
        self.assertEqual(report["outcome"], "unknown")
        self.assertEqual(report["errors"], 0)
        self.cli("check", code=2)

    def test_echoed_error_text_and_progress_ok_are_not_results(self):
        report = self.ingest('report = self.ingest("error: " + "x" * 65000)\nOK starting next build\n')
        self.assertEqual(report["errors"], 0)
        self.assertEqual(report["outcome"], "unknown")
        self.assertEqual(self.ingest("OK (skipped=1)\n")["outcome"], "passed")

    def test_success_requires_absence_of_errors(self):
        report = self.ingest("fatal error: missing file\n** BUILD SUCCEEDED **\n", "--exit-code", "0")
        self.assertEqual(report["outcome"], "failed")

    def test_exit_code_alone_proves_failure(self):
        report = self.ingest("nothing helpful\n", "--exit-code", "7")
        self.assertEqual(report["outcome"], "failed")
        self.assertEqual(report["errors"], 0)
        self.assertEqual(report["exit_code"], 7)

    def test_windows_exception_status_preserved_in_reports(self):
        report = self.ingest("process stopped\n", "--exit-code", "3221225477")
        self.assertEqual(report["exit_code"], 3221225477)
        self.assertEqual(self.client.explain()["exit_code"], 3221225477)
        self.cli("ingest", "-", "--exit-code", "4294967296", data=b"", code=2)

    @unittest.skipUnless(os.name == "nt", "Windows native process status")
    def test_capture_windows_exception_status(self):
        code, report = self.client.capture([sys.executable, "-c", "import ctypes; ctypes.windll.kernel32.ExitProcess(0xC0000005)"], quiet=True)
        self.assertEqual(code, 3221225477)
        self.assertEqual(report["exit_code"], code)
        self.assertEqual(report["outcome"], "failed")
        run = subprocess.run([sys.executable, str(ROOT / "python/logsonfire.py"), "--binary", BINARY, "--db", self.db,
                              "run", "--quiet", "--", sys.executable, "-c",
                              "import ctypes; ctypes.windll.kernel32.ExitProcess(0xC0000005)"], capture_output=True)
        self.assertEqual(run.returncode, code)
        self.assertEqual(json.loads(run.stdout)["exit_code"], code)

    def test_exit_zero(self):
        self.assertEqual(self.ingest("", "--exit-code", "0")["status"], "ok")

    def test_rule_families(self):
        cases = [
            ("main.cpp:3:9: error: use of undeclared identifier", "compiler.error"),
            ("main.cpp(10): error C2143: syntax error", "compiler.error"),
            ("ld: undefined reference to `foo`", "linker.error"),
            ("FAILED: CMakeFiles/foo.o", "build.failed"),
            ("make[2]: *** [all] Error 2", "build.failed"),
            ("** TEST FAILED **", "build.failed"),
            ("2 failed, 10 passed in 1.3s", "test.failed"),
            ("FAIL: test_thing", "test.failed"),
            ("Traceback (most recent call last):", "exception.unhandled"),
            ("ValueError: invalid input", "log.error"),
            ("panic: index out of range", "exception.unhandled"),
            ("Segmentation fault (core dumped)", "crash.signal"),
            ("Exception Type: EXC_BAD_ACCESS (SIGSEGV)", "crash.signal"),
            ('{"exception":{"type":"EXC_CRASH"}}', "crash.signal"),
            ('{"termination":{"code":9}}', "crash.apple"),
            ('{"level":"error","message":"network unavailable"}', "log.error"),
        ]
        for text, rule in cases:
            with self.subTest(text=text):
                report = self.ingest(text)
                self.assertEqual(report["outcome"], "failed")
                self.assertEqual(report["top_issues"][0]["rule"], rule)

    def test_ansi_invalid_utf8_and_no_final_newline(self):
        report = self.ingest(b"\x1b[31merror: broken\x1b[0m \xff")
        self.assertEqual(report["lines"], 1)
        text = self.client.context()["items"][0]["text"]
        self.assertNotIn("\x1b", text)
        self.assertIn("\ufffd", text)

    def test_redacts_storage_and_fts_not_just_report(self):
        secret = "ghp_abcdefghijklmnop12345678"
        report = self.ingest(f'error: token={secret}\nAuthorization: Bearer abc.def.123\n'
                             'password="spaces inside secret"\nhttps://user:pass@example.com\n'
                             '-----BEGIN RSA PRIVATE KEY-----\nsuperprivate\n-----END RSA PRIVATE KEY-----\n')
        self.assertGreaterEqual(report["redacted_lines"], 6)
        self.assertEqual(self.client.search(secret)["items"], [])
        connection = sqlite3.connect(self.db)
        self.addCleanup(connection.close)
        dump = "\n".join(connection.iterdump())
        for value in (secret, "spaces inside secret", "abc.def.123", "superprivate", "user:pass"):
            self.assertNotIn(value, dump)

    def test_explicit_redaction_opt_out(self):
        report = self.ingest("token=keepme", "--no-redact")
        self.assertFalse(report["redaction_enabled"])
        self.assertIn("keepme", self.client.context()["items"][0]["text"])

    def test_oversized_line_cannot_claim_success(self):
        report = self.ingest("a" * 200000 + "\n** BUILD SUCCEEDED **\n")
        self.assertEqual(report["truncated_lines"], 1)
        self.assertEqual(report["outcome"], "unknown")
        self.assertTrue(self.client.context()["items"][0]["truncated"])

    def test_long_diagnostic_and_credential_lines(self):
        report = self.ingest("error: " + "x" * 65000 + "\ntoken=" + "y" * 65000 + "\n")
        self.assertEqual(report["outcome"], "failed")
        self.assertTrue(report["top_issues"][0]["message_truncated"])
        self.assertEqual(self.client.context(line=2, before=0, after=0)["items"][0]["text"], "token=[REDACTED]")

    def test_long_lines_keep_late_errors_and_redact_full_secret_values(self):
        report = self.ingest("prefix " + "x" * 5000 + " error: late diagnostic\n" +
                             "padding " + "x" * 1010 + " token=" + "s" * 12000 + "\n" +
                             "error:" + " " * 20000 + "message\n")
        self.assertEqual(report["errors"], 2)
        self.assertIn("late diagnostic", report["top_issues"][0]["message"])
        self.assertEqual(self.client.search("ssssssss")["items"], [])
        self.assertTrue(self.client.search("REDACTED")["items"])

    def test_redaction_quoted_values_and_multiple_secrets(self):
        self.ingest('password="escaped \\\" private words" token=secondsecret Authorization: Basic dXNlcjpwYXNz\n'
                    '{"apiKey":"thirdvalue", "client_secret":"fourthvalue"}\n')
        for value in ("private words", "secondsecret", "dXNlcjpwYXNz", "thirdvalue", "fourthvalue"):
            self.assertEqual(self.client.search(value)["items"], [], value)

    def test_response_budget_and_pagination(self):
        report = self.ingest("".join(f"error: issue{i} " + "x" * 2000 + "\n" for i in range(100)))
        self.assertEqual(len(report["top_issues"]), 5)
        first = self.cli("issues", "--limit", "100")
        self.assertIsNotNone(first["next_offset"])
        second = self.cli("issues", "--limit", "100", "--offset", str(first["next_offset"]))
        self.assertNotEqual(first["items"][0]["id"], second["items"][0]["id"])
        self.cli("context", "--line", "50", "--before", "49", "--after", "49")

    def test_literal_search_and_pagination(self):
        self.ingest("one 100%\ntwo UNDER_score\nthree 100%\n")
        result = self.client.search("100%", limit=1)
        self.assertEqual(result["items"][0]["line"], 1)
        self.assertEqual(result["next_offset"], 1)
        self.assertEqual(self.client.search("100%", offset=1)["items"][0]["line"], 3)
        self.assertEqual(self.client.search("UNDER_")["items"][0]["line"], 2)
        self.assertEqual(self.client.search("' OR 1=1 --")["items"], [])

    def test_fts_when_available(self):
        self.ingest("error: missing dependency\n** BUILD SUCCEEDED **\n")
        connection = sqlite3.connect(self.db)
        has_fts = connection.execute("SELECT count(*) FROM sqlite_master WHERE name='line_search'").fetchone()[0]
        connection.close()
        if has_fts:
            self.assertEqual(len(self.client.search("missing AND dependency", fts=True)["items"]), 1)
            self.cli("search", "--fts", "--text", '"', code=2)

    def test_multiple_sources_and_filter(self):
        first = self.ingest("warning: deprecated\nerror: missing\n")
        second = self.ingest("** BUILD SUCCEEDED **\n")
        self.assertEqual(self.client.summary()["source_id"], second["source_id"])
        self.assertEqual(self.client.summary(first["source_id"])["outcome"], "failed")
        self.assertEqual(len(self.client.issues(first["source_id"], severity="warning")["items"]), 1)
        self.assertEqual(len(self.client.query("sources")["items"]), 2)

    def test_bad_inputs_and_missing_source(self):
        self.cli("summary", code=2)
        self.cli("ingest", "missing.log", code=2)
        self.cli("issues", "--source", "0", code=2)
        self.cli("context", "--line", "1x", code=2)
        self.cli("search", "--limit", "101", code=2)
        self.ingest("hi\n")
        self.cli("summary", "--source", "999", code=2)
        self.cli("search", code=2)

    def test_failed_ingestion_rolls_back(self):
        self.ingest("first\n")
        self.cli("ingest", "-", "--exit-code-file", str(Path(self.temp.name) / "absent"), data=b"error: temporary\n", code=2)
        self.assertEqual(len(self.client.query("sources")["items"]), 1)

    def test_capture_preserves_exit_code_and_both_streams(self):
        code, report = self.client.capture([sys.executable, "-c", "import sys; print('hello'); print('error: failed', file=sys.stderr); sys.exit(7)"], quiet=True)
        self.assertEqual(code, 7)
        self.assertEqual((report["exit_code"], report["outcome"], report["lines"]), (7, "failed", 2))

    def test_capture_streams_large_output_and_zero_exit(self):
        code, report = self.client.capture([sys.executable, "-c", "for i in range(10000): print('progress', i)"], quiet=True)
        self.assertEqual(code, 0)
        self.assertEqual((report["status"], report["lines"]), ("ok", 10000))

    def test_missing_capture_command_does_not_commit(self):
        with self.assertRaises(OSError):
            self.client.capture([str(Path(self.temp.name) / "not-a-command")], quiet=True)
        self.assertEqual(self.client.query("sources")["items"], [])

    def test_concurrent_ingestion_and_queries(self):
        def task(i):
            return self.client.ingest(self.fixture, name=f"source {i}", exit_code=0)
        self.fixture = Path(self.temp.name) / "concurrent.log"
        self.fixture.write_text("progress\n" * 100, encoding="utf-8")
        self.client.ingest(self.fixture)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            reports = list(pool.map(task, range(6)))
        self.assertEqual(len({r["source_id"] for r in reports}), 6)
        self.assertEqual(len(self.client.query("sources")["items"]), 7)

    def test_reader_sees_committed_source_during_streaming_ingestion(self):
        committed = self.ingest("** BUILD SUCCEEDED **\n")
        writer = subprocess.Popen([BINARY, "--db", self.db, "ingest", "-", "--exit-code", "0"],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            writer.stdin.write(b"still running\n")
            writer.stdin.flush()
            deadline = time.monotonic() + 5
            locked = False
            while time.monotonic() < deadline:
                connection = sqlite3.connect(self.db, timeout=0)
                try:
                    connection.execute("BEGIN IMMEDIATE")
                    connection.rollback()
                except sqlite3.OperationalError:
                    locked = True
                finally:
                    connection.close()
                if locked:
                    break
                time.sleep(0.01)
            self.assertTrue(locked, "streaming ingestion should hold an uncommitted write transaction")
            result = subprocess.run([BINARY, "--db", self.db, "summary"], capture_output=True, timeout=3)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(json.loads(result.stdout)["source_id"], committed["source_id"])
            writer.stdin.close()
            writer.stdin = None
            output, _ = writer.communicate(timeout=5)
            self.assertEqual(json.loads(output)["lines"], 1)
        finally:
            if writer.poll() is None:
                writer.kill()
                writer.wait()
            for stream in (writer.stdin, writer.stdout, writer.stderr):
                if stream:
                    stream.close()

    def test_http_api(self):
        self.ingest("error: missing header\n")
        # Exercise the real HTTP parser/handler without requiring a socket bind.
        class Socket:
            def __init__(self, request):
                self.input = io.BytesIO(request)
                self.output = bytearray()
            def makefile(self, *args):
                return self.input
            def sendall(self, data):
                self.output.extend(data)
        def request(path, headers=None, method="GET"):
            fields = {"Host": "127.0.0.1:8765", **(headers or {})}
            raw = method + " " + path + " HTTP/1.0\r\n" + "".join(f"{k}: {v}\r\n" for k, v in fields.items()) + "\r\n"
            connection = Socket(raw.encode())
            make_handler(self.client)(connection, ("127.0.0.1", 12345), SimpleNamespace(server_port=8765))
            head, body = bytes(connection.output).split(b"\r\n\r\n", 1)
            return int(head.split(b" ", 2)[1]), json.loads(body)
        status, payload = request("/v1/summary")
        self.assertEqual((status, payload["outcome"]), (200, "failed"))
        status, payload = request("/v1/schema")
        self.assertEqual(status, 200)
        self.assertTrue(payload["read_only"])
        status, payload = request("/v1/explain")
        self.assertEqual((status, payload["outcome"]), (200, "failed"))
        self.assertTrue(payload["answer"])
        for path, headers, expected in [
            ("/v1/missing", {}, 404),
            ("/v1/search?text=x&db=elsewhere", {}, 400),
            ("/v1/summary?source=1&source=2", {}, 400),
            ("/v1/summary", {"Host": "evil.example"}, 403),
            ("/v1/summary", {"Origin": "https://evil.example"}, 403),
        ]:
            with self.subTest(path=path, headers=headers):
                self.assertEqual(request(path, headers)[0], expected)
        self.assertEqual(request("/v1/summary", method="POST")[0], 405)

    def test_source_file_and_unicode(self):
        log = Path(self.temp.name) / "büild.log"
        log.write_text("error: café 火\n", encoding="utf-8")
        report = self.client.ingest(log)
        self.assertEqual(report["name"], log.name)
        self.assertIn("火", self.client.context()["items"][0]["text"])


if __name__ == "__main__":
    unittest.main()
