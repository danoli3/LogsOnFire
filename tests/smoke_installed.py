"""Check installed layout, Bash/PowerShell endpoints, and a live HTTP request in CI."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import urllib.request

prefix = Path(sys.argv[1]).resolve()
skip_http = "--skip-http" in sys.argv[2:]
binary = prefix / "bin" / ("logsonfire.exe" if os.name == "nt" else "logsonfire")
with tempfile.TemporaryDirectory() as directory:
    db = Path(directory) / "logs.db"
    endpoint = prefix / "share/logsonfire/logsonfire.py"
    environment = {**os.environ, "LOGSONFIRE_BIN": str(binary), "LOGSONFIRE_DB": str(db), "PYTHON": sys.executable}
    if os.name == "nt":
        wrapper = ["pwsh", "-NoProfile", "-File", str(prefix / "bin/logsonfire-agent.ps1")]
    else:
        wrapper = [str(prefix / "bin/logsonfire-agent")]
    run = subprocess.run(wrapper + ["run", "--quiet", "--", sys.executable, "-c", "print('hello')"], env=environment, capture_output=True, timeout=30)
    assert run.returncode == 0, run.stderr
    assert json.loads(run.stdout)["status"] == "ok", run.stdout
    if not skip_http:
        server = subprocess.Popen([sys.executable, str(endpoint), "serve", "--port", "0"], env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            address = json.loads(server.stderr.readline())["url"].replace("/v1/schema", "/v1/summary")
            with urllib.request.urlopen(address, timeout=10) as response:
                assert json.load(response)["status"] == "ok"
        finally:
            server.terminate()
            server.communicate(timeout=10)
print("Installed executable and wrapper passed" + ("; HTTP skipped" if skip_http else "; HTTP API passed"))
