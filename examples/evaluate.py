#!/usr/bin/env python3
"""Compare real log byte sizes with native summaries and explanations.

This measures bytes, not model tokens. Each run uses a fresh temporary database.
"""
import argparse
import json
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python"))
from logsonfire import Client


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--binary")
    args = parser.parse_args()
    results = []
    with tempfile.TemporaryDirectory(prefix="logsonfire-evaluation-") as directory:
        client = Client(binary=args.binary, db=Path(directory) / "logs.db")
        for log in args.logs:
            report = client.analyze(log)
            summary = client.summary(report["source_id"])
            input_bytes = log.stat().st_size
            explanation_bytes = len(json.dumps(report, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
            summary_bytes = len(json.dumps(summary, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
            results.append({
                "log": log.name,
                "input_bytes": input_bytes,
                "input_lines": report["lines"],
                "summary_bytes": summary_bytes,
                "explanation_bytes": explanation_bytes,
                "explanation_byte_reduction_percent": round(100 * (1 - explanation_bytes / input_bytes), 1) if input_bytes else None,
                "report": report,
            })
    print(json.dumps({"measurement": "UTF-8 bytes; not model tokens", "results": results}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
