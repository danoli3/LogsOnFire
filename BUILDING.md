# Building and installing LogsOnFire

Requires CMake 3.20+, a C++17 compiler, and SQLite 3.24+ development headers/libraries.
Tests, command capture, HTTP, and savings accounting require Python 3.9+.
The native CLI runs independently of Python.

## Get the source

```sh
git clone https://github.com/danoli3/LogsOnFire.git
cd LogsOnFire
```

## macOS

```sh
brew install cmake sqlite python
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
build/logsonfire --version
```

## Linux

On Debian/Ubuntu:

```sh
sudo apt-get install cmake g++ libsqlite3-dev python3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
build/logsonfire --version
```

Other distributions need the equivalent compiler, CMake, SQLite development
package, and Python; the CMake commands are the same.

## Windows

Use Visual Studio's C++ build tools and [vcpkg](https://github.com/microsoft/vcpkg).
In a developer PowerShell, adjusting the vcpkg toolchain location as needed:

```powershell
vcpkg install sqlite3:x64-windows-static
cmake -S . -B build -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
build/Release/logsonfire.exe --version
```

For native ARM64 Windows builds, use `arm64-windows-static` and `-A ARM64`.
The default MSVC configuration links the C++ runtime statically.

## Install

macOS/Linux, to a prefix you own:

```sh
cmake --install build --config Release --prefix "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
logsonfire --version
```

Windows, to a staging directory:

```powershell
cmake --install build --config Release --prefix "$PWD/stage"
$env:PATH = "$PWD/stage/bin;$env:PATH"
logsonfire --version
```

Installation places the executable and launchers in `bin`, and Python modules in
`share/logsonfire`. Keep that layout intact when moving an installation. Use the
Bash launcher on macOS/Linux and `logsonfire-agent.ps1` on Windows. Set
`LOGSONFIRE_BIN` to select a native executable and `PYTHON` to select the launcher's
Python interpreter. A native-only build can disable tests with `-DBUILD_TESTING=OFF`.

## Development checks

After the first build, track log payloads replaced during further builds/tests:

```sh
python3 tools/savings.py run -- cmake --build build --config Release --parallel
python3 tools/savings.py run -- ctest --test-dir build -C Release --output-on-failure
python3 tools/savings.py totals
```

See [savings accounting](docs/SAVINGS.md) for counter accuracy and the local ledger.
The integration suites cover diagnostic rules, format adapters, status decisions,
storage/queries, capture, and savings accounting. CI tests macOS, Linux, and
Windows on x64 and arm64, including installed wrappers and a live HTTP request.

## Packages and releases

CI produces macOS/Linux archives, Windows ZIPs, and Linux DEB/RPM packages.
Homebrew/WinGet manifest generation is prepared; registry publication is separate.
See [distribution and release preparation](docs/DISTRIBUTION.md) for artifact
layout, runtime requirements, packaging commands, and release workflows.

Return to [README.md](README.md) for supported inputs and usage.
