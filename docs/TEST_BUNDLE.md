# Released regression tests and sample logs

Every release includes `logsonfire-VERSION-tests.tar.gz`:

```sh
curl -fLO https://github.com/danoli3/LogsOnFire/releases/download/v0.2.0-rc.2/logsonfire-0.2.0-rc.2-tests.tar.gz
tar -xzf logsonfire-0.2.0-rc.2-tests.tar.gz
python3 logsonfire-tests-0.2.0-rc.2/run-tests.py --binary "$(command -v logsonfire)"
```

On Windows, pass the full path of `logsonfire.exe` to the same Python runner.
Use the matching native release version. The bundle verifies file checksums and
replays ingestion/query, savings, format, GitHub URL handling, plain-text, version,
and packaging tests.

Samples include GitHub Actions, Xcode success/failure, Android logcat, Studio,
VS Code, Make, CMake optional/required dependency cases, Gradle/Kotlin, warnings,
pytest, sanitizers, and Apple crash text. Synthetic samples and the public CI log
fixture are distinguished in the included example documentation.

The Unit tests workflow creates and replays this archive. All six native platform
jobs download and replay that exact uploaded archive before packaging. A manifest
inside the archive records the version and SHA-256 of each test, fixture and helper.

To create a bundle locally:

```sh
python3 tools/package_tests.py --output dist/logsonfire-0.2.0-rc.2-tests.tar.gz
```
