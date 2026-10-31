#!/usr/bin/env python3
"""Read/check the project's single authoritative SemVer value."""
import argparse
from pathlib import Path
import re

IDENTIFIER = r"(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)"
SEMVER = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-("+IDENTIFIER+r"(?:\."+IDENTIFIER+r")*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?")


def read_version(root):
    value = (Path(root)/"VERSION").read_text(encoding="ascii").strip()
    if not SEMVER.fullmatch(value):
        raise ValueError("VERSION must contain one valid semantic version")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root",type=Path,default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--check",action="store_true")
    parser.add_argument("--expected",help="Expected version or v-prefixed release tag")
    args = parser.parse_args()
    value = read_version(args.root)
    if args.expected and value != args.expected.removeprefix("v"):
        parser.error("Tag differs from VERSION")
    if args.check and f"## [{value}]" not in (args.root/"changelog.md").read_text(encoding="utf-8"):
        parser.error("changelog.md needs a section matching VERSION")
    print(value)


if __name__ == "__main__":
    main()
