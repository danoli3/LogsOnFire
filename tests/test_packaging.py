import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("release_manifests", ROOT / "tools/release_manifests.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class PackagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.artifacts = Path(self.temp.name) / "artifacts"
        self.artifacts.mkdir()
        self.output = Path(self.temp.name) / "output"
        self.source = self.artifacts / "logsonfire-0.1.0-source.tar.gz"
        self.source.write_bytes(b"fixture source bytes")
        for arch in ("x64", "arm64"):
            with zipfile.ZipFile(self.artifacts / f"logsonfire-0.1.0-windows-{arch}.zip", "w") as archive:
                archive.writestr("bin/logsonfire.exe", b"fixture executable " + arch.encode())

    def generate(self, **overrides):
        args = dict(repository="example/LogsOnFire", version="0.1.0", artifacts=self.artifacts,
                    output=self.output, package_id="Example.LogsOnFire", publisher="Example publisher")
        args.update(overrides)
        release.generate(**args)

    def test_manifests_use_real_hashes_paths_and_repository(self):
        self.generate()
        formula = (self.output / "homebrew/logsonfire.rb").read_text()
        self.assertIn(hashlib.sha256(self.source.read_bytes()).hexdigest(), formula)
        self.assertIn("https://github.com/example/LogsOnFire/releases/download/v0.1.0/", formula)
        directory = self.output / "winget/e/Example/LogsOnFire/0.1.0"
        self.assertEqual(len(list(directory.glob("*.yaml"))), 3)
        installer = (directory / "Example.LogsOnFire.installer.yaml").read_text()
        self.assertIn("RelativeFilePath: bin/logsonfire.exe", installer)
        for arch in ("x64", "arm64"):
            path = self.artifacts / f"logsonfire-0.1.0-windows-{arch}.zip"
            self.assertIn(hashlib.sha256(path.read_bytes()).hexdigest().upper(), installer)
        self.assertEqual(len((self.output / "SHA256SUMS").read_text().splitlines()), 3)

    def test_refuses_missing_or_wrong_layout_archives(self):
        archive = self.artifacts / "logsonfire-0.1.0-windows-x64.zip"
        archive.unlink()
        with self.assertRaisesRegex(ValueError, "Missing Windows"):
            self.generate()
        with zipfile.ZipFile(archive, "w") as package:
            package.writestr("wrong/logsonfire.exe", b"wrong path")
        with self.assertRaisesRegex(ValueError, "missing bin/logsonfire.exe"):
            self.generate()

    def test_rejects_invalid_release_identity(self):
        for values in ({"repository": "bad\nrepository"}, {"version": "../../v1"}, {"package_id": "NoDot"}):
            with self.subTest(values=values), self.assertRaises(ValueError):
                self.generate(**values)


if __name__ == "__main__":
    unittest.main()
