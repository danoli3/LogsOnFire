# Distribution and release preparation

The repository builds native packages, a Homebrew tap formula, and the test/log
archive. WinGet community submission is separate. The release repository is
[danoli3/LogsOnFire](https://github.com/danoli3/LogsOnFire).

## Build targets

GitHub Actions builds and tests native x64 and arm64 variants of macOS, Linux,
and Windows. Every job runs the native/Python integration suite and checks the
installed executable, platform wrapper, and a live HTTP request before packaging.

| Platform | Artifact | Runtime |
| --- | --- | --- |
| macOS | `.tar.gz`, Homebrew source formula | System SQLite; Python for scripting endpoints |
| Windows | `.zip`, WinGet portable ZIP manifests | SQLite and MSVC runtime linked statically; optional Python 3.9+ |
| Linux | `.tar.gz`, `.deb`, `.rpm` | System SQLite/libstdc++; optional Python 3.9+ |

Linux artifacts are built on Ubuntu 24.04 and require a compatible glibc
(normally 2.39+) and the corresponding shared libraries. They are not universal
Linux binaries. Build from source on older distributions or musl-based systems.
macOS archives inherit the runner's deployment target; Homebrew source builds
target the host environment. macOS archives are not notarized.

## Local packaging

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cpack --config build/CPackConfig.cmake -C Release -G TGZ -B dist
```

Use `-G ZIP` on Windows. On Linux, `-G DEB` requires `dpkg-shlibdeps`, and `-G RPM`
requires `rpmbuild`. The release workflow installs these dependencies and names
artifacts `logsonfire-VERSION-PLATFORM-ARCH.EXT`.

Archives contain `bin/logsonfire[.exe]`, launchers in `bin`, and the Python module
in `share/logsonfire`. Extract the entire archive so relative wrapper paths keep
working. Add its `bin` to PATH. On Linux, use the distribution's package manager
to install `.deb` or `.rpm` files with dependencies.

## Prepare a release

1. Put the project in its intended GitHub repository. Set its `WINGET_PACKAGE_ID`
   Actions variable if the identifier should differ from `LogsOnFire.LogsOnFire`.
   Select an identifier appropriate for the actual publisher.
2. Update `VERSION`, `changelog.md`, and release notes, and commit all
   intended release files. Run tests.
3. Run `python3 tools/version.py --check`. Push a matching `vMAJOR.MINOR.PATCH`
   tag, optionally with a SemVer pre-release suffix such as `v0.2.0-rc.2`.
4. The workflow verifies the tag, builds/tests all six variants and Homebrew,
   replays the fixture archive, and publishes the assets. Suffixed versions are
   marked as GitHub pre-releases. Published tags/assets must remain immutable.
5. The generated formula is committed into this repository's `Formula` directory
   for the custom tap. WinGet community submission remains separate.

Release manifests are generated from actual artifact bytes, with real SHA-256
checksums and URLs derived from the chosen repository and tag. To generate locally:

```sh
python3 tools/release_manifests.py \
  --repository YOUR_OWNER/YOUR_REPO \
  --version 0.2.0-rc.2 \
  --artifacts dist \
  --output dist/manifests \
  --package-id YourPublisher.LogsOnFire \
  --publisher 'Your Publisher'
```

The generator requires `logsonfire-0.2.0-rc.2-source.tar.gz` and both Windows ZIPs,
with `bin/logsonfire.exe` in each. The workflow creates the source archive with
`git archive`; it includes only committed files. `SHA256SUMS` covers release
packages and source. The release workflow also hashes the final manifests archive
and standalone formula. Debian pre-release package versions use `~rc.2`, while
RPM uses a `0.rc.2` release value so candidates sort before stable packages.

## Homebrew

The generated `homebrew/logsonfire.rb` builds from the release source archive using
CMake and depends on SQLite and Python. CI tests it against a local source archive
before publication. The release workflow writes `Formula/logsonfire.rb` on main.
Users install the project's custom tap with:

```sh
brew tap danoli3/logsonfire https://github.com/danoli3/LogsOnFire.git
brew install danoli3/logsonfire/logsonfire
brew test danoli3/logsonfire/logsonfire
```

Validate with `brew style`, `brew audit`, installation, and `brew test` before
announcing the tap. See Homebrew's [Formula Cookbook](https://docs.brew.sh/Formula-Cookbook).
This project does not imply acceptance into Homebrew core.

## WinGet

The generated `winget/` tree contains version, default locale, and installer
manifests for x64 and arm64 portable ZIPs using schema 1.10.0. Its nested executable
path is `bin/logsonfire.exe`, with the command alias `logsonfire`. Python is optional
and is not pulled in by the native package.

On Windows, validate the version directory with `winget validate --manifest PATH`
and test installation with `winget install --manifest PATH` (local manifests may
need enabling). Once the release assets are publicly downloadable, submit the
manifest directory to [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs).
Acceptance and publication are separate steps. Microsoft's
[manifest documentation](https://learn.microsoft.com/windows/package-manager/package/manifest)
and [installer schema](https://github.com/microsoft/winget-pkgs/blob/master/doc/manifest/schema/1.10.0/installer.md)
describe the required fields.

WinGet exposes the native executable alias. To use the optional Python endpoints,
locate the installed package's `share/logsonfire/logsonfire.py`, or use the full
release archive and its PowerShell launcher. The Python interpreter must be
installed separately.
