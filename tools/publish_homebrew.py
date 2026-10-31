#!/usr/bin/env python3
"""Publish the tested release formula into this repository's Formula directory."""
import argparse
import base64
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository",required=True)
    parser.add_argument("--version",required=True)
    parser.add_argument("--formula",type=Path,required=True)
    args=parser.parse_args()
    endpoint=f"repos/{args.repository}/contents/Formula/logsonfire.rb"
    result=subprocess.run(["gh","api",endpoint+"?ref=main"],capture_output=True)
    payload={"message":"Update Homebrew formula for "+args.version,"branch":"main",
             "content":base64.b64encode(args.formula.read_bytes()).decode("ascii")}
    if result.returncode==0:
        current=json.loads(result.stdout)
        if base64.b64decode(current["content"])==args.formula.read_bytes():
            print("Homebrew formula already current")
            return
        payload["sha"]=current["sha"]
    elif b"404" not in result.stderr:
        raise RuntimeError(result.stderr.decode(errors="replace"))
    with tempfile.NamedTemporaryFile(mode="w",encoding="utf-8",delete=False) as stream:
        json.dump(payload,stream)
        filename=Path(stream.name)
    try:
        subprocess.run(["gh","api","--method","PUT",endpoint,"--input",str(filename),"--jq",".commit.html_url"],check=True)
    finally:
        filename.unlink()


if __name__=="__main__":
    main()
