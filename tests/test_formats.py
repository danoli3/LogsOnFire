"""Format adapters exercised through the actual native CLI."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())
ROOT = Path(__file__).resolve().parent.parent


class FormatTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.db = str(Path(self.temp.name)/"logs.db")

    def cli(self,*args,data=None):
        result = subprocess.run([BINARY,"--db",self.db,*args],input=data,capture_output=True)
        self.assertEqual(result.returncode,0,result.stdout)
        self.assertLessEqual(len(result.stdout),32768)
        return json.loads(result.stdout)

    def sample(self,name):
        return self.cli("analyze",str(ROOT/"examples/formats"/name))

    def test_android_crash_cause_and_process_scoping(self):
        report = self.sample("android-logcat.log")
        self.assertEqual(report["outcome"],"failed")
        first = report["findings"][0]
        self.assertEqual(first["rule"],"exception.cause")
        self.assertIn("Required account",first["message"])
        self.assertEqual(first["origin"]["pid"],"4012")
        self.assertEqual(first["origin"]["tag"],"AndroidRuntime")
        self.assertTrue(any("MainActivity.kt:42" in row["text"] for row in report["crash_context"]))
        self.assertFalse(any("OtherService" in row["text"] for row in report["crash_context"]))
        summary = self.cli("summary")
        self.assertEqual(summary["format"],"logcat")
        self.assertEqual(summary["errors"],4)
        self.assertEqual(summary["warnings"],1)

    def test_logcat_priorities_brief_time_and_studio_export(self):
        cases = [
            ("E/Example( 123): network unavailable","error"),
            ("10-31 12:00:00.100 E/Example( 123): network unavailable","error"),
            ("W/Example: low memory","warning"),
            ("2026-10-31 12:00:00.100 123-456 Example com.example E network unavailable","error"),
        ]
        for line,severity in cases:
            with self.subTest(line=line):
                report = self.cli("analyze","-",data=(line+"\n").encode())
                self.assertEqual(report["findings"][0]["severity"],severity)
        report = self.cli("analyze","-",data=b"I/Example( 123): handled Error: retrying\nI/Example( 123): BUILD SUCCESSFUL\n")
        self.assertEqual(report["outcome"],"unknown")
        self.assertEqual(report["errors"],0)

    def test_logcat_repeats_ignore_timestamps_but_keep_processes(self):
        self.cli("ingest","-",data=b"10-31 12:00:00.000 123 456 E Example: unavailable\n10-31 12:00:01.000 123 456 E Example: unavailable\n10-31 12:00:02.000 999 456 E Example: unavailable\n")
        items = self.cli("issues")["items"]
        self.assertEqual(len(items),2)
        self.assertEqual(items[0]["count"],2)
        self.assertEqual(items[1]["count"],1)

    def test_studio_exception_chain(self):
        report = self.sample("android-studio.log")
        self.assertEqual(self.cli("summary")["format"],"android-studio")
        self.assertEqual(report["findings"][0]["rule"],"exception.cause")
        self.assertIn("UnknownHostException",report["findings"][0]["message"])
        items = self.cli("issues")["items"]
        self.assertTrue(any(item.get("origin",{}).get("tag")=="#com.android.tools.idea.gradle" for item in items))

    def test_vscode_extension(self):
        report = self.sample("vscode.log")
        self.assertEqual(self.cli("summary")["format"],"vscode")
        self.assertEqual(report["findings"][0]["rule"],"vscode.extension")
        self.assertIn("example.language-tools",report["answer"]+report["findings"][0]["message"])
        self.assertEqual(report["findings"][0]["origin"]["priority"],"error")

    def test_build_formats(self):
        for filename,format,rule in [("xcode.log","xcode","xcode.signing"),("make.log","make","make.target"),
                                     ("cmake.log","cmake","dependency.missing"),("gradle.log","gradle","compiler.kotlin")]:
            with self.subTest(filename=filename):
                report = self.sample(filename)
                self.assertEqual(report["outcome"],"failed")
                self.assertEqual(self.cli("summary")["format"],format)
                self.assertEqual(report["findings"][0]["rule"],rule)

    def test_optional_cmake_probe_is_not_failure_or_build_success(self):
        report = self.cli("analyze","-",data=b"-- Performing Test OPTIONAL_FEATURE - Failed\n-- Could NOT find OptionalThing (missing: OptionalThing_DIR)\n-- Configuring done\n-- Generating done\n")
        self.assertEqual(report["errors"],0)
        self.assertEqual(report["outcome"],"unknown")

    def test_released_success_warning_and_crash_samples(self):
        for name,outcome in (("clean-build.log","passed"),("xcode-success.log","passed"),
                             ("warnings-only.log","passed"),("android-logcat-info.log","unknown"),
                             ("cmake-optional.log","unknown"),("pytest-failed.log","failed"),
                             ("asan-crash.log","failed")):
            with self.subTest(name=name):
                self.assertEqual(self.sample(name)["outcome"],outcome)


if __name__ == "__main__":
    unittest.main()
