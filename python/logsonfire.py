"""Standard-library Python client, command capture, and read-only local HTTP API.

Set LOGSONFIRE_BIN or pass binary= when the native executable is not on PATH.
"""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

SCHEMA_VERSION = 1
MAX_RESPONSE_BYTES = 32768


class LogError(RuntimeError):
    def __init__(self, message, payload=None):
        super().__init__(message)
        self.payload = payload


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
        args = [self.binary, "--db", self.db, command]
        for name, value in options.items():
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

    def ingest(self, path, *, name=None, exit_code=None, format="auto"):
        args = [self.binary, "--db", self.db, "ingest", str(path), "--format", format]
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

    def summary(self, source=None):
        return self.query("summary", source=source)

    def issues(self, source=None, **options):
        return self.query("issues", source=source, **options)

    def context(self, source=None, line=1, before=3, after=3):
        return self.query("context", source=source, line=line, before=before, after=after)

    def search(self, text, source=None, **options):
        return self.query("search", source=source, text=text, **options)

    def capture(self, command, *, name=None, quiet=False):
        """Stream merged stdout/stderr into native ingestion, returning (exit_code, report).

        The completion file is written before closing stdin, so the parser can
        finish a single atomic transaction with the real process exit status.
        No unredacted log file is created. Child output is relayed to stderr.
        """
        if not command:
            raise LogError("run requires a command after --")
        with tempfile.TemporaryDirectory(prefix="logsonfire-") as directory:
            exit_file = Path(directory) / "exit-code"
            ingest_args = [self.binary, "--db", self.db, "ingest", "-", "--name",
                           name or Path(command[0]).name, "--exit-code-file", str(exit_file)]
            parser = subprocess.Popen(ingest_args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            child = None
            try:
                child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, shell=False)
                while True:
                    data = child.stdout.read1(65536)
                    if not data:
                        break
                    parser.stdin.write(data)
                    if not quiet:
                        sys.stderr.buffer.write(data)
                        sys.stderr.buffer.flush()
                code = child.wait()
                code = min(255, code if code >= 0 else 128 - code)
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
        "/v1/issues": {"source": "ID", "severity": "error|warning", "limit": "1..100", "offset": "N"},
        "/v1/context": {"source": "ID", "line": "positive line number", "before": "0..49", "after": "0..49"},
        "/v1/search": {"source": "ID", "text": "literal substring, required", "limit": "1..100", "offset": "N"},
    },
    "workflow": ["GET /v1/summary", "GET /v1/issues?source=ID if needed", "GET /v1/context?source=ID&line=FIRST_LINE"],
    "outcomes": {"passed": "Completion evidence with no detected errors", "failed": "Detected error or nonzero exit", "unknown": "Missing completion evidence or incomplete analysis"},
}


def make_handler(client):
    class Handler(BaseHTTPRequestHandler):
        server_version = "LogsOnFire/0.1"

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


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db")
    parser.add_argument("--binary")
    sub = parser.add_subparsers(dest="command", required=True)
    server = sub.add_parser("serve", help="Start the read-only HTTP API on 127.0.0.1")
    server.add_argument("--port", type=int, default=8765)
    run = sub.add_parser("run", help="Capture a process; live output goes to stderr, JSON to stdout")
    run.add_argument("--name")
    run.add_argument("--quiet", action="store_true", help="Suppress live output (which can include secrets)")
    run.add_argument("args", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    try:
        client = Client(args.db, args.binary)
        if args.command == "serve":
            serve(client, args.port)
            return 0
        command = args.args[1:] if args.args[:1] == ["--"] else args.args
        code, payload = client.capture(command, name=args.name, quiet=args.quiet)
        print(json.dumps(payload, ensure_ascii=False, separators=(",", ":")))
        return code
    except (LogError, OSError, ValueError, subprocess.TimeoutExpired) as exc:
        print(json.dumps({"schema_version": 1, "error": {"code": "tool_error", "message": str(exc)}}))
        return 2
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
