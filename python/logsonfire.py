"""Standard-library Python client, command capture, and read-only local HTTP API.

Set LOGSONFIRE_BIN or pass binary= when the native executable is not on PATH.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit
from logsonfire_version import VERSION as __version__

SCHEMA_VERSION = 1
MAX_RESPONSE_BYTES = 32768


class LogError(RuntimeError):
    def __init__(self, message, payload=None):
        super().__init__(message)
        self.payload = payload


def parse_github_actions_url(url):
    """Accept only GitHub Actions listing, run, and job URLs."""
    try:
        parsed = urlsplit(url)
        port = parsed.port
    except (TypeError, ValueError) as exc:
        raise LogError("Invalid GitHub Actions URL") from exc
    if parsed.scheme != "https" or parsed.hostname != "github.com" or parsed.username or parsed.password or port:
        raise LogError("Expected an https://github.com Actions URL")
    parts = [part for part in parsed.path.split("/") if part]
    if len(parts) < 3 or parts[2] != "actions":
        raise LogError("URL must point to a GitHub repository Actions page, run, or job")
    owner, repository = parts[:2]
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", owner) or not re.fullmatch(r"[A-Za-z0-9_.-]+", repository):
        raise LogError("Invalid GitHub owner or repository in URL")
    if owner in (".", "..") or repository in (".", ".."):
        raise LogError("Invalid GitHub owner or repository in URL")
    safe_url = "https://github.com/" + owner + "/" + repository + "/" + "/".join(parts[2:])
    if len(parts) == 3:
        return {"repository": owner + "/" + repository, "kind": "page", "url": safe_url}
    if len(parts) == 5 and parts[3] == "runs" and parts[4].isdigit():
        return {"repository": owner + "/" + repository, "kind": "run", "run_id": parts[4], "url": safe_url}
    if (len(parts) == 7 and parts[3] == "runs" and parts[4].isdigit()
            and parts[5] == "job" and parts[6].isdigit()):
        return {"repository": owner + "/" + repository, "kind": "job", "run_id": parts[4],
                "job_id": parts[6], "url": safe_url}
    raise LogError("URL must point to /actions, /actions/runs/RUN_ID, or /actions/runs/RUN_ID/job/JOB_ID")


def _gh_result(args, timeout=60):
    executable = shutil.which("gh")
    if not executable:
        raise LogError("GitHub Actions links require the GitHub CLI (gh) on PATH")
    result = subprocess.run([executable, *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=timeout, check=False)
    if result.returncode:
        detail = result.stderr.decode("utf-8", "replace").strip()
        raise LogError("gh " + " ".join(args[:3]) + " failed" + (": " + detail[:1000] if detail else ""))
    return result.stdout


def fetch_github_log(url, directory):
    """Use gh with argument arrays (never a shell) and stream logs to a temp file."""
    target = parse_github_actions_url(url)
    selection = "direct_link"
    if target["kind"] == "page":
        try:
            rows = json.loads(_gh_result(["run", "list", "--repo", target["repository"], "--limit", "20",
                                         "--json", "databaseId,status,conclusion,createdAt,headBranch,name"]))
        except (ValueError, TypeError) as exc:
            raise LogError("gh returned invalid workflow-run data") from exc
        if not isinstance(rows, list):
            raise LogError("gh returned invalid workflow-run data")
        if not rows:
            raise LogError("No workflow runs were found for " + target["repository"])
        selected = next((row for row in rows if isinstance(row, dict) and row.get("conclusion") == "failure"), rows[0])
        if not isinstance(selected, dict):
            raise LogError("gh returned invalid workflow-run data")
        run_id = selected.get("databaseId")
        if not isinstance(run_id, (int, str)) or not str(run_id).isdigit():
            raise LogError("gh did not provide a valid workflow-run ID")
        target["run_id"] = str(run_id)
        selection = "most_recent_failed" if selected.get("conclusion") == "failure" else "most_recent"
        target["kind"] = "run"
        target["run"] = {key: selected.get(key) for key in
                          ("status", "conclusion", "createdAt", "headBranch", "name")}
    target["resolved_url"] = "https://github.com/" + target["repository"] + "/actions/runs/" + target["run_id"]
    if target.get("job_id"):
        target["resolved_url"] += "/job/" + target["job_id"]
    command = ["run", "view", target["run_id"], "--repo", target["repository"]]
    if target["kind"] == "job":
        command += ["--job", target["job_id"]]
    command += ["--log"]
    path = Path(directory) / "github-actions.log"
    with path.open("wb") as output:
        executable = shutil.which("gh")
        if not executable:
            raise LogError("GitHub Actions links require the GitHub CLI (gh) on PATH")
        try:
            result = subprocess.run([executable, *command], stdout=output, stderr=subprocess.PIPE,
                                    timeout=180, check=False)
        except subprocess.TimeoutExpired as exc:
            raise LogError("Timed out while downloading GitHub Actions logs with gh") from exc
    if result.returncode:
        detail = result.stderr.decode("utf-8", "replace").strip()
        raise LogError("gh " + " ".join(command[:3]) + " failed" + (": " + detail[:1000] if detail else ""))
    if path.stat().st_size == 0:
        raise LogError("gh returned an empty GitHub Actions log")
    target["selection"] = selection
    target["log_bytes"] = path.stat().st_size
    return target, path


def find_binary(binary=None):
    candidate = binary or os.environ.get("LOGSONFIRE_BIN") or shutil.which("logsonfire")
    if not candidate:
        root = Path(__file__).resolve().parent.parent
        for relative in ("build/logsonfire", "build/Release/logsonfire.exe", "bin/logsonfire.exe"):
            if (root / relative).is_file():
                candidate = str(root / relative)
                break
    if not candidate:
        raise LogError("Native logsonfire executable not found; build it or set LOGSONFIRE_BIN")
    return str(candidate)


class Client:
    """Return Python dicts; analysis failures are data, tool failures raise LogError."""

    def __init__(self, db=None, binary=None):
        self.binary = find_binary(binary)
        self.db = str(db or os.environ.get("LOGSONFIRE_DB", ".logsonfire/logs.db"))

    def query(self, command="summary", **options):
        args = [self.binary, "--db", self.db, "--output", "json", command]
        for name, value in options.items():
            if name == "output" and value != "json":
                raise LogError("Use Client.text() for plain-text output")
            if value is None or value is False:
                continue
            args.append("--" + name.replace("_", "-"))
            if value is not True:
                args.append(str(value))
        result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        try:
            payload = json.loads(result.stdout)
        except (ValueError, UnicodeError) as exc:
            raise LogError("Native executable did not return valid JSON") from exc
        if "error" in payload:
            raise LogError(payload["error"]["message"], payload)
        if result.returncode != 0 and command != "check":
            raise LogError("Native executable failed", payload)
        return payload

    def _read_file(self, command, path, *, name=None, exit_code=None, format="auto"):
        args = [self.binary, "--db", self.db, "--output", "json", command, str(path), "--format", format]
        if name is not None:
            args += ["--name", name]
        if exit_code is not None:
            args += ["--exit-code", str(exit_code)]
        result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            payload = json.loads(result.stdout)
        except (ValueError, UnicodeError) as exc:
            raise LogError("Native executable did not return valid JSON") from exc
        if result.returncode or "error" in payload:
            raise LogError(payload.get("error", {}).get("message", "Ingestion failed"), payload)
        return payload

    def ingest(self, path, *, name=None, exit_code=None, format="auto"):
        return self._read_file("ingest", path, name=name, exit_code=exit_code, format=format)

    def analyze(self, path, *, name=None, exit_code=None, format="auto"):
        """Read a dropped log and explain the most useful diagnostic evidence."""
        return self._read_file("analyze", path, name=name, exit_code=exit_code, format=format)

    def explain(self, source=None):
        return self.query("explain", source=source)

    def summary(self, source=None):
        return self.query("summary", source=source)

    def text(self, source=None):
        """Return a concise plain-text summary for people."""
        args = [self.binary,"--db",self.db,"--output","text","summary"]
        if source is not None:
            args += ["--source",str(source)]
        result = subprocess.run(args,capture_output=True,timeout=60)
        text = result.stdout.decode("utf-8")
        if result.returncode:
            raise LogError(text.strip().removeprefix("LogsOnFire error: "))
        return text

    def issues(self, source=None, **options):
        return self.query("issues", source=source, **options)

    def context(self, source=None, line=1, before=3, after=3):
        return self.query("context", source=source, line=line, before=before, after=after)

    def search(self, text, source=None, **options):
        return self.query("search", source=source, text=text, **options)

    def capture(self, command, *, name=None, quiet=False, raw_sink=None):
        """Stream merged stdout/stderr into native ingestion, returning (exit_code, report).

        The completion file is written before closing stdin, so the parser can
        finish a single atomic transaction with the real process exit status.
        Default capture creates no raw log file. raw_sink optionally observes
        original bytes for local accounting. Live output is relayed to stderr.
        """
        if not command:
            raise LogError("run requires a command after --")
        with tempfile.TemporaryDirectory(prefix="logsonfire-") as directory:
            exit_file = Path(directory) / "exit-code"
            ingest_args = [self.binary, "--db", self.db, "--output", "json", "ingest", "-", "--name",
                           name or Path(command[0]).name, "--exit-code-file", str(exit_file)]
            parser = subprocess.Popen(ingest_args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            child = None
            try:
                child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, shell=False)
                while True:
                    data = child.stdout.read1(65536)
                    if not data:
                        break
                    if raw_sink is not None:
                        raw_sink.write(data)
                    parser.stdin.write(data)
                    if not quiet:
                        sys.stderr.buffer.write(data)
                        sys.stderr.buffer.flush()
                code = child.wait()
                code = code if code >= 0 else 128 - code
                exit_file.write_text(str(code), encoding="ascii")
                parser.stdin.close()
                parser.stdin = None
                output, error = parser.communicate(timeout=60)
                payload = json.loads(output)
                if parser.returncode or "error" in payload:
                    raise LogError(payload.get("error", {}).get("message", "Ingestion failed"), payload)
                return code, payload
            finally:
                if child is not None:
                    if child.stdout:
                        child.stdout.close()
                    if child.poll() is None:
                        child.terminate()
                        try:
                            child.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            child.kill()
                            child.wait()
                if parser.poll() is None:
                    parser.kill()
                    parser.wait()
                for stream in (parser.stdin, parser.stdout, parser.stderr):
                    if stream:
                        stream.close()


API_SCHEMA = {
    "schema_version": SCHEMA_VERSION,
    "read_only": True,
    "max_response_bytes": MAX_RESPONSE_BYTES,
    "endpoints": {
        "/v1/health": {},
        "/v1/schema": {},
        "/v1/sources": {"limit": "1..100", "offset": "0..2147483647"},
        "/v1/summary": {"source": "positive source ID; latest if omitted"},
        "/v1/explain": {"source": "positive source ID; latest if omitted"},
        "/v1/issues": {"source": "ID", "severity": "error|warning", "limit": "1..100", "offset": "N"},
        "/v1/context": {"source": "ID", "line": "positive line number", "before": "0..49", "after": "0..49"},
        "/v1/search": {"source": "ID", "text": "literal substring, required", "limit": "1..100", "offset": "N"},
    },
    "workflow": ["GET /v1/explain for an assessment, or /v1/summary for status", "GET /v1/issues?source=ID if needed", "GET /v1/context?source=ID&line=FIRST_LINE"],
    "outcomes": {"passed": "Completion evidence with no detected errors", "failed": "Detected error or nonzero exit", "unknown": "Missing completion evidence or incomplete analysis"},
}


def make_handler(client):
    class Handler(BaseHTTPRequestHandler):
        server_version = "LogsOnFire/" + __version__

        def log_message(self, fmt, *args):
            # Avoid logging query text that could contain credentials.
            pass

        def send_json(self, status, payload):
            data = (json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
            if len(data) > MAX_RESPONSE_BYTES:
                status = 500
                data = b'{"error":{"code":"response_limit","message":"Response exceeds byte budget"}}\n'
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            try:
                # Prevent browser DNS rebinding against the unauthenticated local API.
                port = self.server.server_port
                if self.headers.get("Host", "") not in (f"127.0.0.1:{port}", f"localhost:{port}"):
                    return self.send_json(403, {"error": {"code": "invalid_host", "message": "Use localhost or 127.0.0.1"}})
                if self.headers.get("Origin"):
                    return self.send_json(403, {"error": {"code": "origin_denied", "message": "Cross-origin requests are disabled"}})
                if len(self.path) > 4096:
                    raise ValueError("Request URL exceeds 4096 characters")
                url = urlsplit(self.path)
                if url.path not in API_SCHEMA["endpoints"]:
                    return self.send_json(404, {"error": {"code": "not_found", "message": "See /v1/schema"}})
                params = parse_qs(url.query, keep_blank_values=True, max_num_fields=10)
                allowed = API_SCHEMA["endpoints"][url.path]
                if any(key not in allowed or len(values) != 1 for key, values in params.items()):
                    raise ValueError("Unknown or repeated query parameter")
                options = {key: values[0] for key, values in params.items()}
                if url.path == "/v1/health":
                    result = {"schema_version": 1, "status": "ok", "service": "logsonfire"}
                elif url.path == "/v1/schema":
                    result = API_SCHEMA
                else:
                    result = client.query(url.path.rsplit("/", 1)[-1], **options)
                self.send_json(200, result)
            except ValueError as exc:
                self.send_json(400, {"error": {"code": "invalid_request", "message": str(exc)}})
            except LogError as exc:
                self.send_json(400, exc.payload or {"error": {"code": "tool_error", "message": str(exc)}})
            except (OSError, subprocess.TimeoutExpired):
                self.send_json(503, {"error": {"code": "unavailable", "message": "Native query unavailable"}})

        def do_POST(self):
            self.send_json(405, {"error": {"code": "read_only", "message": "Ingest logs through the CLI or Python client"}})

    return Handler


def serve(client, port=8765):
    # A bounded worker count prevents an unbounded number of native query processes.
    import threading

    class Server(ThreadingHTTPServer):
        daemon_threads = True
        slots = threading.BoundedSemaphore(8)

        def process_request(self, request, client_address):
            if not self.slots.acquire(blocking=False):
                self.shutdown_request(request)
                return
            try:
                super().process_request(request, client_address)
            except Exception:
                self.slots.release()
                raise

        def process_request_thread(self, request, client_address):
            request.settimeout(15)
            try:
                super().process_request_thread(request, client_address)
            finally:
                self.slots.release()

    with Server(("127.0.0.1", port), make_handler(client)) as server:
        print(json.dumps({"url": f"http://127.0.0.1:{server.server_port}/v1/schema", "read_only": True}), file=sys.stderr, flush=True)
        server.serve_forever()


def analyze_github(client, url, output="json"):
    """Fetch one Actions log with gh, analyze it, and discard the downloaded file."""
    with tempfile.TemporaryDirectory(prefix="logsonfire-github-") as directory:
        target, path = fetch_github_log(url, directory)
        name = "GitHub Actions " + target["kind"] + " " + target["run_id"]
        if target.get("job_id"):
            name += " job " + target["job_id"]
        report = client.analyze(path, name=name, format="github")
        report["github_actions"] = target
        if output == "text":
            label = "GitHub Actions " + target["kind"] + " " + target["run_id"]
            if target.get("job_id"):
                label += " job " + target["job_id"]
            if target["selection"] != "direct_link":
                label += " (" + target["selection"].replace("_", " ") + ")"
            return report, label + "\n" + client.text(report["source_id"])
        return report, None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db")
    parser.add_argument("--binary")
    parser.add_argument("--output",choices=("json","text"),default=os.environ.get("LOGSONFIRE_OUTPUT","json"))
    sub = parser.add_subparsers(dest="command", required=True)
    server = sub.add_parser("serve", help="Start the read-only HTTP API on 127.0.0.1")
    server.add_argument("--port", type=int, default=8765)
    run = sub.add_parser("run", help="Capture a process; live output goes to stderr, JSON to stdout")
    run.add_argument("--name")
    run.add_argument("--quiet", action="store_true", help="Suppress live output (which can include secrets)")
    run.add_argument("--output",choices=("json","text"),default=argparse.SUPPRESS)
    run.add_argument("args", nargs=argparse.REMAINDER)
    github = sub.add_parser("github", help="Fetch and analyze a GitHub Actions page, run, or job URL with gh")
    github.add_argument("url", help="https://github.com/OWNER/REPO/actions[/runs/RUN_ID[/job/JOB_ID]]")
    github.add_argument("--output", choices=("json", "text"), default=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    try:
        if args.output not in ("json","text"):
            raise LogError("LOGSONFIRE_OUTPUT must be json or text")
        client = Client(args.db, args.binary)
        if args.command == "serve":
            serve(client, args.port)
            return 0
        if args.command == "github":
            report, text = analyze_github(client, args.url, args.output)
            if args.output == "text":
                sys.stdout.buffer.write(text.encode("utf-8"))
            else:
                print(json.dumps(report, ensure_ascii=False, separators=(",", ":")))
            return 0
        command = args.args[1:] if args.args[:1] == ["--"] else args.args
        code, payload = client.capture(command, name=args.name, quiet=args.quiet)
        if args.output=="text":
            sys.stdout.buffer.write(client.text(payload["source_id"]).encode("utf-8"))
        else:
            print(json.dumps(payload, ensure_ascii=False, separators=(",", ":")))
        return code
    except (LogError, OSError, ValueError, subprocess.TimeoutExpired) as exc:
        if args.output=="text":
            sys.stdout.buffer.write(("LogsOnFire error: "+str(exc)+"\n").encode("utf-8"))
        else:
            print(json.dumps({"schema_version": 1, "error": {"code": "tool_error", "message": str(exc)}}))
        return 2
    except KeyboardInterrupt:
        return 130


def exit_with_status(result):
    if os.name == "nt" and result > 0x7fffffff:
        # SystemExit converts through a signed C int. Preserve the full Windows
        # DWORD status when a captured process exits with an exception code.
        import ctypes
        sys.stdout.flush()
        sys.stderr.flush()
        exit_process = ctypes.windll.kernel32.ExitProcess
        exit_process.argtypes = [ctypes.c_uint32]
        exit_process(result)
    sys.exit(result)


if __name__ == "__main__":
    exit_with_status(main())
