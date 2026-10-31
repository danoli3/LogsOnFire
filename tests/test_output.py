"""Plain CLI preferences must not change the machine-readable SDK/API contract."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

BINARY=str(Path(sys.argv.pop(1)).resolve())
ROOT=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(ROOT/"python"))
from logsonfire import Client, __version__


class OutputTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.db=str(Path(self.temp.name)/"logs.db")

    def run_cli(self,*args,data=None,env=None,code=0):
        result=subprocess.run([BINARY,"--db",self.db,*args],input=data,capture_output=True,env=env)
        self.assertEqual(result.returncode,code,result.stdout)
        return result.stdout.decode("utf-8")

    def test_success_and_32_warnings(self):
        self.assertEqual(self.run_cli("analyze","-","--output","text",data=b"** BUILD SUCCEEDED **\n"),"No errors in Logs\n")
        log=("warning: optional setting\n"*32+"** BUILD SUCCEEDED **\n").encode()
        self.assertEqual(self.run_cli("ingest","-","--output","text",data=log),"No errors, 32 warnings\n")
        self.assertEqual(self.run_cli("check","--output","text"),"No errors, 32 warnings\n")

    def test_actionable_error_and_nonzero_exit(self):
        result=self.run_cli("analyze","-","--output","text",data=b"FAILED: main.o\nmain.cpp:1: fatal error: sqlite3.h: No such file or directory\n")
        self.assertTrue(result.startswith("Error in logs is: main.cpp"),result)
        self.assertIn("2 errors",result)
        self.run_cli("check","--output","text",code=1)
        result=self.run_cli("ingest","-","--exit-code","7","--output","text",data=b"stopped\n")
        self.assertIn("process exited with code 7",result)

    def test_unknown_and_incomplete_are_disclosed(self):
        result=self.run_cli("analyze","-","--output","text",data=b"still working\n")
        self.assertIn("Outcome unknown",result)
        self.run_cli("check","--output","text",code=2)
        result=self.run_cli("analyze","-","--output","text",data=("x"*70000+"\n** BUILD SUCCEEDED **\n").encode())
        self.assertIn("truncated",result)

    def test_persistent_setting_json_override_and_plain_errors(self):
        env={**os.environ,"LOGSONFIRE_OUTPUT":"text"}
        self.assertEqual(self.run_cli("analyze","-",data=b"** BUILD SUCCEEDED **\n",env=env),"No errors in Logs\n")
        report=json.loads(self.run_cli("summary","--output","json",env=env))
        self.assertEqual(report["outcome"],"passed")
        result=self.run_cli("analyze","missing.log",env=env,code=2)
        self.assertTrue(result.startswith("LogsOnFire error:"),result)

    def test_text_queries_and_version(self):
        self.run_cli("ingest","-",data=b"error: broken\n")
        for args in (("sources",),("issues",),("context","--line","1"),("search","--text","broken"),("schema",)):
            result=self.run_cli(*args,"--output","text")
            self.assertFalse(result.startswith("{"),result)
        self.assertEqual(self.run_cli("--version","--output","text"),"logsonfire "+__version__+"\n")

    def test_sdk_and_capture_keep_json_under_text_environment(self):
        with patch.dict(os.environ,{"LOGSONFIRE_OUTPUT":"text"}):
            client=Client(binary=BINARY,db=self.db)
            code,report=client.capture([sys.executable,"-c","print('hello')"],quiet=True)
            self.assertEqual((code,report["outcome"]),(0,"passed"))
            self.assertEqual(client.summary()["outcome"],"passed")
            self.assertEqual(client.text(),"No errors in Logs\n")
        result=subprocess.run([sys.executable,str(ROOT/"python/logsonfire.py"),"--binary",BINARY,"--db",self.db,
            "run","--quiet","--output","text","--",sys.executable,"-c","raise SystemExit(7)"],capture_output=True)
        self.assertEqual(result.returncode,7,result.stdout)
        self.assertIn(b"process exited with code 7",result.stdout)


if __name__=="__main__":
    unittest.main()
