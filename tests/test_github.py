"""GitHub Actions URL validation and gh-backed log fetching."""
import json
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch

BINARY = str(Path(sys.argv.pop(1)).resolve())
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "python"))
from logsonfire import Client, LogError, analyze_github, fetch_github_log, main, parse_github_actions_url  # noqa: E402


class GithubTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.db = str(Path(self.temp.name) / "logs.db")
        self.fixture = (ROOT / "examples/github-actions-failed.log").read_bytes()

    def test_parse_run_and_job_links(self):
        run = parse_github_actions_url("https://github.com/danoli3/LogsOnFire/actions/runs/123?check_suite_focus=true")
        self.assertEqual((run["repository"], run["kind"], run["run_id"]), ("danoli3/LogsOnFire", "run", "123"))
        self.assertNotIn("check_suite_focus", run["url"])
        job = parse_github_actions_url("https://github.com/danoli3/LogsOnFire/actions/runs/123/job/456")
        self.assertEqual((job["kind"], job["run_id"], job["job_id"]), ("job", "123", "456"))

    def test_rejects_non_github_and_malformed_urls(self):
        for url in ("http://github.com/a/b/actions/runs/1", "https://example.com/a/b/actions/runs/1",
                    "https://user@github.com/a/b/actions/runs/1", "https://github.com/a/b/actions/runs/x",
                    "https://github.com/a/b/actions/runs/1/../../settings", "https://github.com/../b/actions"):
            with self.subTest(url=url), self.assertRaises(LogError):
                parse_github_actions_url(url)

    @patch("logsonfire.shutil.which", return_value="/usr/bin/gh")
    @patch("logsonfire.subprocess.run")
    def test_fetches_run_log_without_a_shell(self, run, which):
        def complete(command, *, stdout, stderr, timeout, check):
            stdout.write(self.fixture)
            return subprocess.CompletedProcess(command, 0, stderr=b"")

        run.side_effect = complete
        with tempfile.TemporaryDirectory() as directory:
            target, path = fetch_github_log("https://github.com/danoli3/LogsOnFire/actions/runs/123", directory)
            self.assertEqual(path.read_bytes(), self.fixture)
        self.assertEqual(target["run_id"], "123")
        self.assertEqual(target["log_bytes"], len(self.fixture))
        args = run.call_args.args[0]
        self.assertEqual(args, ["/usr/bin/gh", "run", "view", "123", "--repo", "danoli3/LogsOnFire", "--log"])
        self.assertFalse(run.call_args.kwargs.get("shell", False))

    @patch("logsonfire.shutil.which", return_value="/usr/bin/gh")
    @patch("logsonfire.subprocess.run")
    def test_fetches_only_the_job_linked_by_job_url(self, run, which):
        def complete(command, *, stdout, stderr, timeout, check):
            stdout.write(self.fixture)
            return subprocess.CompletedProcess(command, 0, stderr=b"")

        run.side_effect = complete
        with tempfile.TemporaryDirectory() as directory:
            target, _ = fetch_github_log("https://github.com/danoli3/LogsOnFire/actions/runs/123/job/456", directory)
        self.assertEqual(target["kind"], "job")
        self.assertEqual(run.call_args.args[0][-3:], ["--job", "456", "--log"])

    @patch("logsonfire.shutil.which", return_value="/usr/bin/gh")
    @patch("logsonfire.subprocess.run")
    def test_actions_page_selects_the_latest_failure(self, run, which):
        listing = [{"databaseId": 9, "status": "completed", "conclusion": "success"},
                   {"databaseId": 8, "status": "completed", "conclusion": "failure"}]

        def complete(command, **kwargs):
            if "--json" in command:
                return subprocess.CompletedProcess(command, 0, stdout=json.dumps(listing).encode(), stderr=b"")
            kwargs["stdout"].write(self.fixture)
            return subprocess.CompletedProcess(command, 0, stderr=b"")

        run.side_effect = complete
        with tempfile.TemporaryDirectory() as directory:
            target, _ = fetch_github_log("https://github.com/danoli3/LogsOnFire/actions", directory)
        self.assertEqual((target["run_id"], target["selection"]), ("8", "most_recent_failed"))
        self.assertEqual(run.call_count, 2)

    @patch("logsonfire.shutil.which", return_value=None)
    def test_requires_gh(self, which):
        with tempfile.TemporaryDirectory() as directory, self.assertRaisesRegex(LogError, "require the GitHub CLI"):
            fetch_github_log("https://github.com/danoli3/LogsOnFire/actions/runs/123", directory)

    def test_analysis_uses_native_pipeline_and_attaches_target(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "actions.log"
            path.write_bytes(self.fixture)
            target = {"repository": "danoli3/LogsOnFire", "kind": "job", "run_id": "123", "job_id": "456",
                      "url": "https://github.com/danoli3/LogsOnFire/actions/runs/123/job/456",
                      "selection": "direct_link", "log_bytes": len(self.fixture)}
            with patch("logsonfire.fetch_github_log", return_value=(target, path)):
                client = Client(db=self.db, binary=BINARY)
                report, _ = analyze_github(client, target["url"])
        self.assertEqual(report["outcome"], "failed")
        self.assertEqual(client.summary(report["source_id"])["format"], "github")
        self.assertEqual(report["github_actions"]["job_id"], "456")

    def test_agent_command_returns_normal_json_analysis(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "actions.log"
            path.write_bytes(self.fixture)
            target = {"repository": "danoli3/LogsOnFire", "kind": "run", "run_id": "123", "url":
                      "https://github.com/danoli3/LogsOnFire/actions/runs/123", "resolved_url":
                      "https://github.com/danoli3/LogsOnFire/actions/runs/123", "selection": "direct_link",
                      "log_bytes": len(self.fixture)}
            output = io.StringIO()
            with patch("logsonfire.fetch_github_log", return_value=(target, path)), redirect_stdout(output):
                code = main(["--db", self.db, "--binary", BINARY, "github", target["url"], "--output", "json"])
        report = json.loads(output.getvalue())
        self.assertEqual(code, 0)
        self.assertEqual(report["github_actions"]["resolved_url"], target["resolved_url"])
        self.assertEqual(report["outcome"], "failed")


if __name__ == "__main__":
    unittest.main()
