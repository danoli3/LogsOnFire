#!/usr/bin/env python3
"""Verify and replay the released tests/logs against a chosen native executable."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import os


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary",type=Path,required=True)
    args=parser.parse_args()
    binary=args.binary.resolve()
    root=Path(__file__).resolve().parent
    if not (root/"manifest.json").is_file():
        root=root.parent
    manifest=json.loads((root/"manifest.json").read_text())
    for name,expected in manifest["files"].items():
        path=(root/name).resolve()
        path.relative_to(root.resolve())
        actual=hashlib.sha256(path.read_bytes()).hexdigest()
        if actual!=expected:
            raise ValueError("Fixture/test checksum mismatch: "+name)
    suites=("test_system.py","test_packaging.py","test_savings.py","test_formats.py","test_output.py","test_version.py","test_github.py")
    for suite in suites:
        command=[sys.executable,str(root/"tests"/suite)]
        if suite!="test_packaging.py": command.append(str(binary))
        subprocess.run(command,cwd=root,check=True,env={**os.environ,"LOGSONFIRE_OUTPUT":"json"})
    print("All released regression tests and fixture checks passed")


if __name__=="__main__":
    main()
