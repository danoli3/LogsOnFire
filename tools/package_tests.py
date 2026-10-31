#!/usr/bin/env python3
"""Create the checksummed test/log archive that CI replays and releases ship."""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import tarfile
from version import read_version


def build(root,output):
    root=Path(root).resolve()
    version=read_version(root)
    entries={}
    for directory in ("tests","examples","python","tools","scripts"):
        for path in (root/directory).rglob("*"):
            if path.is_file() and not path.is_symlink() and "__pycache__" not in path.parts and path.suffix!=".pyc":
                entries[path.relative_to(root).as_posix()]=path.read_bytes()
    for filename in ("VERSION","CMakeLists.txt","LICENSE","changelog.md"):
        entries[filename]=(root/filename).read_bytes()
    entries["run-tests.py"]=(root/"tools/run_test_bundle.py").read_bytes()
    entries["README.md"]=b"# LogsOnFire test/log bundle\n\nRun: python3 run-tests.py --binary /path/to/logsonfire\n\nThe runner verifies manifest.json and replays every regression suite.\nAll synthetic examples are labeled in the source documentation.\n"
    manifest={"schema_version":1,"version":version,"files":{name:hashlib.sha256(data).hexdigest() for name,data in sorted(entries.items())}}
    entries["manifest.json"]=(json.dumps(manifest,sort_keys=True,indent=2)+"\n").encode()
    output=Path(output)
    output.parent.mkdir(parents=True,exist_ok=True)
    with output.open("wb") as stream, gzip.GzipFile(filename="",mode="wb",fileobj=stream,mtime=0) as compressed, tarfile.open(fileobj=compressed,mode="w") as archive:
        for name,data in sorted(entries.items()):
            item=tarfile.TarInfo(f"logsonfire-tests-{version}/{name}")
            item.size=len(data)
            item.mode=0o755 if name=="run-tests.py" or name=="scripts/logsonfire-agent" else 0o644
            archive.addfile(item,io.BytesIO(data))
    return output


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root",type=Path,default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args()
    print(build(args.root,args.output))


if __name__=="__main__":
    main()
