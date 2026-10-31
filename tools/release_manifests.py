#!/usr/bin/env python3
"""Generate Homebrew and WinGet manifests using hashes of real release artifacts."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import zipfile
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
from version import SEMVER


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def quoted(value):
    # JSON strings are also valid YAML scalar strings for these fields.
    return json.dumps(value, ensure_ascii=True)


def write_homebrew(repository,version,source,output,source_url=None):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("repository must be OWNER/REPO")
    if not SEMVER.fullmatch(version):
        raise ValueError("version must be valid SemVer")
    base = f"https://github.com/{repository}"
    if not source.is_file():
        raise ValueError(f"Missing source archive: {source}")
    url=source_url or f"{base}/releases/download/v{version}/{source.name}"
    formula = f'''class Logsonfire < Formula
  desc "Local log analysis and compact evidence for agents"
  homepage "{base}"
  url {quoted(url)}
  version "{version}"
  sha256 "{sha256(source)}"
  license "MIT"

  depends_on "cmake" => :build
  depends_on "python"
  depends_on "sqlite"

  def install
    system "cmake", "-S", ".", "-B", "build", *std_cmake_args, "-DBUILD_TESTING=OFF"
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  test do
    (testpath/"ok.log").write("** BUILD SUCCEEDED **\\n")
    assert_match '\"status\":\"ok\"', shell_output("#{{bin}}/logsonfire --output json --db #{{testpath}}/logs.db ingest #{{testpath}}/ok.log")
  end
end
'''
    formula_dir = output / "homebrew"
    formula_dir.mkdir(parents=True, exist_ok=True)
    (formula_dir / "logsonfire.rb").write_text(formula, encoding="utf-8")


def generate(repository, version, artifacts, output, package_id, publisher):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("repository must be OWNER/REPO")
    if not SEMVER.fullmatch(version):
        raise ValueError("version must be valid SemVer")
    if not re.fullmatch(r"[A-Za-z0-9]+(?:\.[A-Za-z0-9]+)+", package_id):
        raise ValueError("package ID must contain dot-separated alphanumeric components")
    base=f"https://github.com/{repository}"
    downloads=f"{base}/releases/download/v{version}"
    source=artifacts/f"logsonfire-{version}-source.tar.gz"
    if not source.is_file():
        raise ValueError(f"Missing source archive: {source}")
    installers=[]
    for arch in ("x64","arm64"):
        archive=artifacts/f"logsonfire-{version}-windows-{arch}.zip"
        if not archive.is_file():
            raise ValueError(f"Missing Windows archive: {archive}")
        with zipfile.ZipFile(archive) as package:
            if "bin/logsonfire.exe" not in package.namelist():
                raise ValueError(f"{archive}: missing bin/logsonfire.exe")
        installers.append((arch,archive.name,sha256(archive).upper()))
    write_homebrew(repository,version,source,output)
    winget = output / "winget" / package_id.split(".")[0][0].lower() / Path(*package_id.split(".")) / version
    winget.mkdir(parents=True, exist_ok=True)
    common = f"PackageIdentifier: {package_id}\nPackageVersion: {quoted(version)}\n"
    installer = common + "InstallerType: zip\nNestedInstallerType: portable\nNestedInstallerFiles:\n- RelativeFilePath: bin/logsonfire.exe\n  PortableCommandAlias: logsonfire\nInstallers:\n"
    for arch, name, checksum in installers:
        installer += f"- Architecture: {arch}\n  InstallerUrl: {downloads}/{name}\n  InstallerSha256: {checksum}\n"
    installer += "ManifestType: installer\nManifestVersion: 1.10.0\n"
    (winget / f"{package_id}.installer.yaml").write_text(installer, encoding="utf-8")
    locale = common + f'''PackageLocale: en-US
Publisher: {quoted(publisher)}
PublisherUrl: {base}
PackageName: LogsOnFire
PackageUrl: {base}
License: MIT
LicenseUrl: {base}/blob/v{version}/LICENSE
ShortDescription: Local log analysis and compact evidence for agents
Moniker: logsonfire
Tags:
- logs
- developer-tools
- ai
ManifestType: defaultLocale
ManifestVersion: 1.10.0
'''
    (winget / f"{package_id}.locale.en-US.yaml").write_text(locale, encoding="utf-8")
    (winget / f"{package_id}.yaml").write_text(common + "DefaultLocale: en-US\nManifestType: version\nManifestVersion: 1.10.0\n", encoding="utf-8")
    packages = sorted(p for p in artifacts.iterdir() if p.is_file() and p.suffix in (".gz", ".zip", ".deb", ".rpm"))
    (output / "SHA256SUMS").write_text("".join(f"{sha256(p)}  {p.name}\n" for p in packages), encoding="ascii")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--package-id", default="LogsOnFire.LogsOnFire")
    parser.add_argument("--publisher", default="LogsOnFire contributors")
    parser.add_argument("--homebrew-only",action="store_true")
    parser.add_argument("--source-url",help="Override the source URL for an isolated local Homebrew smoke test")
    args = parser.parse_args()
    if args.homebrew_only:
        write_homebrew(args.repository,args.version,args.artifacts/f"logsonfire-{args.version}-source.tar.gz",args.output,args.source_url)
    else:
        generate(args.repository, args.version, args.artifacts, args.output, args.package_id, args.publisher)


if __name__ == "__main__":
    main()
