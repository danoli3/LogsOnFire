import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "python"))
from logsonfire_savings import Counter, Ledger


class SavingsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        self.ledger = Ledger(self.path / "savings.sqlite3")
        self.addCleanup(self.ledger.close)
        self.counter = Counter()

    def start(self, raw=b"log data" * 100, kind="usage"):
        measured = self.counter.measure(io.BytesIO(raw))
        return self.ledger.start(measured,"test",1,self.path/"logs.db",kind)

    def test_baseline_counted_once_followups_charged(self):
        session = self.start()
        self.ledger.charge(session,b"first report",self.counter,"summary")
        self.assertEqual(self.start(),session)
        self.ledger.charge(session,b"followup",self.counter,"context")
        row = self.ledger.totals()["groups"][0]
        self.assertEqual((row["sessions"],row["deliveries"]),(1,2))
        self.assertEqual(row["baseline_tokens"],200)
        self.assertEqual(row["net_saved_tokens"],195)

    def test_benchmarks_separate_and_negative_savings_kept(self):
        session = self.start(b"tiny",kind="benchmark")
        self.ledger.charge(session,b"large report"*10,self.counter,"explain")
        usage = self.start(b"tiny",kind="usage")
        self.ledger.charge(usage,b"a",self.counter,"explain")
        groups = {r["kind"]:r for r in self.ledger.totals()["groups"]}
        self.assertLess(groups["benchmark"]["net_saved_tokens"],0)
        self.assertEqual(groups["usage"]["net_saved_tokens"],0)

    def test_idempotent_events_and_unfinished_sessions(self):
        session = self.start()
        self.assertEqual(self.ledger.totals()["groups"],[])
        self.assertTrue(self.ledger.charge(session,b"response",self.counter,"summary","event1"))
        self.assertFalse(self.ledger.charge(session,b"response",self.counter,"summary","event1"))
        with self.assertRaises(ValueError):
            self.ledger.charge(session,b"different",self.counter,"summary","event1")
        self.assertEqual(self.ledger.totals()["groups"][0]["deliveries"],1)

    def test_counts_bytes_including_unicode_and_no_raw_content_stored(self):
        raw = "password=private-value 火".encode()
        session = self.start(raw)
        row = self.ledger.session(session)
        self.assertEqual(row["raw_bytes"],len(raw))
        self.assertEqual(row["raw_sha256"],hashlib.sha256(raw).hexdigest())
        self.assertNotIn("private-value","\n".join(self.ledger.db.iterdump()))

    def cli(self,*args):
        return subprocess.run([sys.executable,str(ROOT/"tools/savings.py"),"--binary",BINARY,
                               "--ledger",str(self.path/"cli.sqlite3"),"--db",str(self.path/"logs.db"),*args],capture_output=True)

    def test_cli_capture_preserves_code_and_charges_actual_output(self):
        run = self.cli("run","--",sys.executable,"-c","print('progress ' * 1000); raise SystemExit(7)")
        self.assertEqual(run.returncode,7,run.stdout)
        payload = json.loads(run.stdout)
        self.assertEqual(payload["report"]["exit_code"],7)
        stats = json.loads(self.cli("totals").stdout)["groups"][0]
        self.assertEqual(stats["delivered_bytes"],len(run.stdout))
        self.assertEqual(stats["delivered_tokens"],(len(run.stdout)+3)//4)
        query = self.cli("query",payload["savings_session"],"summary")
        self.assertEqual(query.returncode,0,query.stdout)
        stats = json.loads(self.cli("totals").stdout)["groups"][0]
        self.assertEqual(stats["delivered_bytes"],len(run.stdout)+len(query.stdout))
        self.assertEqual(stats["sessions"],1)

    @unittest.skipUnless(os.name=="nt","Windows DWORD exit status")
    def test_cli_preserves_windows_crash_status(self):
        run = self.cli("run","--",sys.executable,"-c","import ctypes; ctypes.windll.kernel32.ExitProcess(0xC0000005)")
        self.assertEqual(run.returncode,3221225477)
        self.assertEqual(json.loads(run.stdout)["report"]["exit_code"],run.returncode)


if __name__ == "__main__":
    unittest.main()
