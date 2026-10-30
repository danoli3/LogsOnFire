"""Local accounting for log text replaced by LogsOnFire responses.

Reference-tokenizer measurements are not provider billing or Codex telemetry.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import sqlite3
import sys
import tempfile
import uuid

from logsonfire import Client, LogError, exit_with_status

MAX_EXACT_BYTES = 8 * 1024 * 1024


class Counter:
    def __init__(self, name="estimate"):
        self.encoder = None
        if name == "estimate":
            self.name = "estimate:utf8_bytes_div_4"
        else:
            if name not in ("o200k_base", "cl100k_base"):
                raise ValueError("Unknown reference encoding")
            try:
                import tiktoken
            except ImportError as exc:
                raise ValueError("Install optional tiktoken, or use --counter estimate") from exc
            self.encoder = tiktoken.get_encoding(name)
            self.name = "tiktoken:" + name

    def count(self, data):
        if self.encoder is None:
            return math.ceil(len(data) / 4)
        if len(data) > MAX_EXACT_BYTES:
            raise ValueError("Reference counting is capped at 8 MiB; use --counter estimate for larger logs")
        return len(self.encoder.encode(data.decode("utf-8", errors="replace"), disallowed_special=()))

    def measure(self, stream):
        digest = hashlib.sha256()
        total = 0
        chunks = []
        stream.seek(0)
        while True:
            chunk = stream.read(65536)
            if not chunk:
                break
            digest.update(chunk)
            total += len(chunk)
            if self.encoder:
                if total > MAX_EXACT_BYTES:
                    raise ValueError("Reference counting is capped at 8 MiB; use --counter estimate for larger logs")
                chunks.append(chunk)
        tokens = self.count(b"".join(chunks)) if self.encoder else math.ceil(total / 4)
        return {"sha256": digest.hexdigest(), "bytes": total, "tokens": tokens, "counter": self.name}


def utc_now():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


class Ledger:
    def __init__(self, path=".logsonfire/savings.sqlite3"):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(self.path, timeout=30)
        if os.name != "nt":
            self.path.chmod(0o600)
        self.db.execute("PRAGMA foreign_keys=ON")
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS sessions(
                id TEXT PRIMARY KEY, created_at TEXT NOT NULL,
                kind TEXT NOT NULL CHECK(kind IN ('usage','benchmark')),
                label TEXT NOT NULL, raw_sha256 TEXT NOT NULL,
                raw_bytes INTEGER NOT NULL, raw_tokens INTEGER NOT NULL,
                counter TEXT NOT NULL, logs_db TEXT NOT NULL, source_id INTEGER NOT NULL,
                UNIQUE(kind,raw_sha256,counter)
            );
            CREATE TABLE IF NOT EXISTS deliveries(
                id TEXT PRIMARY KEY, session_id TEXT NOT NULL REFERENCES sessions(id),
                created_at TEXT NOT NULL, operation TEXT NOT NULL,
                bytes INTEGER NOT NULL, tokens INTEGER NOT NULL, payload_sha256 TEXT NOT NULL
            );
            CREATE INDEX IF NOT EXISTS delivery_session ON deliveries(session_id);
        """)

    def close(self):
        self.db.close()

    def start(self, measurement, label, source_id, logs_db, kind="usage"):
        if kind not in ("usage", "benchmark"):
            raise ValueError("kind must be usage or benchmark")
        session = uuid.uuid4().hex
        with self.db:
            self.db.execute("""INSERT INTO sessions VALUES(?,?,?,?,?,?,?,?,?,?)
                ON CONFLICT(kind,raw_sha256,counter) DO UPDATE
                SET logs_db=excluded.logs_db,source_id=excluded.source_id""",
                (session, utc_now(), kind, label, measurement["sha256"], measurement["bytes"],
                 measurement["tokens"], measurement["counter"], str(Path(logs_db).resolve()), source_id))
            row = self.db.execute("SELECT id FROM sessions WHERE kind=? AND raw_sha256=? AND counter=?",
                                  (kind, measurement["sha256"], measurement["counter"])).fetchone()
        return row[0]

    def session(self, session):
        self.db.row_factory = sqlite3.Row
        row = self.db.execute("SELECT * FROM sessions WHERE id=?", (session,)).fetchone()
        if row is None:
            raise ValueError("Savings session not found")
        return dict(row)

    def charge(self, session, payload, counter, operation, event_id=None):
        row = self.session(session)
        if counter.name != row["counter"]:
            raise ValueError("Follow-up counter must match the session's counter")
        values = (event_id or uuid.uuid4().hex, session, utc_now(), operation, len(payload),
                  counter.count(payload), hashlib.sha256(payload).hexdigest())
        with self.db:
            existing = self.db.execute("SELECT session_id,payload_sha256 FROM deliveries WHERE id=?", (values[0],)).fetchone()
            if existing:
                if tuple(existing) != (session,values[-1]):
                    raise ValueError("Event ID was already used for another delivery")
                return False
            self.db.execute("INSERT INTO deliveries VALUES(?,?,?,?,?,?,?)", values)
        self.write_summary()
        return True

    def write_summary(self):
        stats = self.totals()
        lines = ["# Local LogsOnFire savings", "", "Updated: " + utc_now(), "",
                 "Log payload accounting only; not Codex billing or observed provider token usage.", "",
                 "| Kind | Counter | Logs | Replies | Baseline tokens | Delivered tokens | Net saved tokens |",
                 "| --- | --- | ---: | ---: | ---: | ---: | ---: |"]
        for row in stats["groups"]:
            lines.append("| {kind} | {counter} | {sessions} | {deliveries} | {baseline_tokens} | {delivered_tokens} | {net_saved_tokens:+d} |".format(**row))
        lines += ["", "`usage` means a declared replacement read. Benchmarks are separate.",
                  "Each raw log is credited once per counter; all follow-up reports are charged.",
                  "Negative values mean reports cost more tokens than the raw baseline.",
                  "`estimate:utf8_bytes_div_4` is a rough estimate. `tiktoken:*` counts a named reference encoding.",
                  "Tool protocol overhead, other prompts, reasoning, caching and billing are excluded.", ""]
        target = self.path.parent / "SAVINGS.md"
        with tempfile.NamedTemporaryFile(mode="w",encoding="utf-8",dir=target.parent,delete=False) as stream:
            temporary = Path(stream.name)
            stream.write("\n".join(lines))
        try:
            os.replace(temporary,target)
        finally:
            temporary.unlink(missing_ok=True)

    def totals(self):
        rows = self.db.execute("""SELECT s.kind,s.counter,count(*),sum(s.raw_bytes),sum(s.raw_tokens),
                    sum(d.n),sum(d.bytes),sum(d.tokens) FROM sessions s JOIN
                    (SELECT session_id,count(*) n,sum(bytes) bytes,sum(tokens) tokens
                     FROM deliveries GROUP BY session_id) d ON d.session_id=s.id
                    GROUP BY s.kind,s.counter ORDER BY s.kind DESC,s.counter""").fetchall()
        groups = []
        for kind, counter, sessions, raw_bytes, raw_tokens, events, delivered_bytes, delivered_tokens in rows:
            groups.append({"kind": kind, "counter": counter, "sessions": sessions, "deliveries": events,
                           "baseline_tokens": raw_tokens, "delivered_tokens": delivered_tokens,
                           "net_saved_tokens": raw_tokens-delivered_tokens,
                           "baseline_bytes": raw_bytes, "delivered_bytes": delivered_bytes,
                           "net_saved_bytes": raw_bytes-delivered_bytes})
        return {"schema_version": 1, "ledger": str(self.path), "groups": groups,
                "scope": "Log payloads only; excludes model/tool protocol overhead, reasoning, caching and billing.",
                "usage": "Declared replacement reads; each raw log credited once per counter; follow-ups charged.",
                "benchmark": "Retrospective/previously-read inputs; excluded from usage savings."}


def emit_report(ledger, session, report, counter, operation, event_id=None):
    # Charge the exact compact UTF-8 text printed, including this receipt and newline.
    output = json.dumps({"report": report, "savings_session": session}, ensure_ascii=False, separators=(",", ":")) + "\n"
    ledger.charge(session, output.encode("utf-8"), counter, operation, event_id)
    sys.stdout.write(output)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ledger", default=os.environ.get("LOGSONFIRE_SAVINGS_DB", ".logsonfire/savings.sqlite3"))
    parser.add_argument("--db", default=".logsonfire/development.db")
    parser.add_argument("--binary")
    parser.add_argument("--counter", choices=("estimate", "o200k_base", "cl100k_base"), default="estimate")
    sub = parser.add_subparsers(dest="operation", required=True)
    sub.add_parser("totals")
    for operation in ("analyze", "run"):
        command = sub.add_parser(operation)
        command.add_argument("--kind", choices=("usage", "benchmark"), default="usage")
        command.add_argument("--name")
        if operation == "analyze":
            command.add_argument("file", type=Path)
            command.add_argument("--format", default="auto")
        else:
            command.add_argument("args", nargs=argparse.REMAINDER)
    query = sub.add_parser("query")
    query.add_argument("session")
    query.add_argument("command", choices=("summary", "explain", "issues", "context", "search"))
    for name in ("line", "before", "after", "limit", "offset"):
        query.add_argument("--"+name, type=int)
    query.add_argument("--text")
    query.add_argument("--severity", choices=("error", "warning"))
    args = parser.parse_args(argv)
    ledger = None
    try:
        ledger = Ledger(args.ledger)
        if args.operation == "totals":
            print(json.dumps(ledger.totals(), separators=(",", ":")))
            return 0
        if args.operation == "query":
            row = ledger.session(args.session)
            counter = Counter(row["counter"].split(":", 1)[1] if row["counter"].startswith("tiktoken:") else "estimate")
            client = Client(binary=args.binary, db=row["logs_db"])
            options = {name: getattr(args,name) for name in ("line","before","after","limit","offset","text","severity")}
            report = client.query(args.command, source=row["source_id"], **options)
            emit_report(ledger,args.session,report,counter,args.command)
            return 0
        counter = Counter(args.counter)
        client = Client(binary=args.binary, db=args.db)
        code = 0
        if args.operation == "run":
            command = args.args[1:] if args.args[:1] == ["--"] else args.args
            # Private temporary spool exists only for counting; removed on exit.
            with tempfile.TemporaryFile() as raw:
                code, report = client.capture(command,name=args.name,quiet=True,raw_sink=raw)
                measurement = counter.measure(raw)
            label = args.name or (Path(command[0]).name if command else "command")
        else:
            with args.file.open("rb") as stream:
                measurement = counter.measure(stream)
            report = client.analyze(args.file,name=args.name,format=args.format)
            label = args.name or args.file.name
        session = ledger.start(measurement,label,report["source_id"],client.db,args.kind)
        if args.operation == "run" and report["outcome"] == "passed" and report["warnings"] == 0:
            report = {key:report[key] for key in ("schema_version","source_id","status","outcome","exit_code")}
        emit_report(ledger,session,report,counter,args.operation)
        return code
    except (LogError, OSError, ValueError, sqlite3.Error) as exc:
        print(json.dumps({"error": {"code": "savings_error", "message": str(exc)}}))
        return 2
    finally:
        if ledger:
            ledger.close()


if __name__ == "__main__":
    exit_with_status(main())
