# Install and use the 0.2.0 release candidate

The first release candidate is `v0.2.0-rc.1`. Pre-releases are available from the
repository's [releases page](https://github.com/danoli3/LogsOnFire/releases), with
explicit versioned download URLs. Stable `0.2.0` is a later release.

## Homebrew: macOS or Linux

The release workflow publishes the tested formula into this repository's
`Formula` directory. Add it as a custom tap:

```sh
brew tap danoli3/logsonfire https://github.com/danoli3/LogsOnFire.git
brew install danoli3/logsonfire/logsonfire
brew test danoli3/logsonfire/logsonfire
logsonfire --output text --version
```

Use the installed commands directly, from any working directory:

```sh
logsonfire analyze build.log --output text
logsonfire-agent run --quiet --output text -- make -j8
logsonfire-agent serve --port 8765
```

The tap follows its published formula. A future `brew update` / `brew upgrade
danoli3/logsonfire/logsonfire` installs the next version published there. This is
a project tap, not a claim of inclusion in Homebrew core.

## Ubuntu/Debian: local DEB with apt-get

Choose `x64` for an amd64 machine or `arm64` for an ARM64 machine:

```sh
curl -fLO https://github.com/danoli3/LogsOnFire/releases/download/v0.2.0-rc.1/logsonfire-0.2.0-rc.1-linux-x64.deb
sudo apt-get install ./logsonfire-0.2.0-rc.1-linux-x64.deb
logsonfire --output text --version
logsonfire analyze build.log --output text
```

The `./` tells apt-get to install the downloaded local package and resolve its
dependencies. There is no hosted LogsOnFire APT repository yet, so bare
`apt-get install logsonfire` is not configured by this guide. CI packages are built
on Ubuntu 24.04; older glibc systems should build from source instead.

## Archives, RPM, and Windows

Download the archive for your platform/architecture, extract the full tree, and
add its `bin` directory to PATH. Python 3.9+ is needed for `logsonfire-agent`, HTTP
and savings accounting, while the native executable runs independently. On Windows,
use `logsonfire-agent.ps1`. Linux RPM packages are also attached to releases.

Published assets include SHA-256 checksums, source, a Homebrew formula, and a
separate test/log archive. See [test bundle usage](TEST_BUNDLE.md),
[building from source](../BUILDING.md), and [the usage guide](../README.md).
