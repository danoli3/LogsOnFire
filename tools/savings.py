#!/usr/bin/env python3
"""Run the local savings ledger from a source checkout."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python"))
from logsonfire_savings import main
from logsonfire import exit_with_status

if __name__ == "__main__":
    exit_with_status(main())
