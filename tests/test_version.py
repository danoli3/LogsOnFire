import importlib.util
from pathlib import Path
import subprocess
import sys
import unittest

BINARY=str(Path(sys.argv.pop(1)).resolve())
ROOT=Path(__file__).resolve().parent.parent
spec=importlib.util.spec_from_file_location("project_version",ROOT/"tools/version.py")
version=importlib.util.module_from_spec(spec)
spec.loader.exec_module(version)


class VersionTests(unittest.TestCase):
    def test_semver_and_changelog_consistency(self):
        value=version.read_version(ROOT)
        result=subprocess.run([sys.executable,str(ROOT/"tools/version.py"),"--check","--expected","v"+value],capture_output=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn(f"## [{value}]",(ROOT/"changelog.md").read_text())
        import json
        native=json.loads(subprocess.check_output([BINARY,"--output","json","--version"]))
        self.assertEqual(native["version"],value)

    def test_semver_identifiers(self):
        for value in ("0.1.0","0.2.0-rc.1","0.2.0-rc.2","1.2.3+build.5"):
            self.assertIsNotNone(version.SEMVER.fullmatch(value))
        for value in ("0.02.0","1.2","1.2.3-01","../0.2.0","1.2.3-"):
            self.assertIsNone(version.SEMVER.fullmatch(value))


if __name__=="__main__":
    unittest.main()
