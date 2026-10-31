"""Version data is installed alongside this module, or read from the source root."""
from pathlib import Path

def read_version():
    directory = Path(__file__).resolve().parent
    for candidate in (directory/"VERSION",directory.parent/"VERSION"):
        if candidate.is_file():
            return candidate.read_text(encoding="ascii").strip()
    return "unknown"

VERSION = read_version()
