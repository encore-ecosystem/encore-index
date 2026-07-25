#!/usr/bin/env python3
"""Validate sparse metadata, ownership, and append-only package history."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import tarfile
import tempfile
import tomllib
import urllib.request
from pathlib import Path

NAME = re.compile(r"^[a-z0-9_-]{2,}$")
SEMVER = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
COMMIT = re.compile(r"^[0-9a-f]{40}$")
GITHUB = re.compile(r"^https://github\.com/([^/]+/[^/]+?)(?:\.git)?$")


def fail(message: str) -> None:
    raise ValueError(message)


def load(path: Path) -> dict:
    try:
        return json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        fail(f"{path}: invalid JSON: {error}")


def base_document(base: str, path: Path) -> dict | None:
    result = subprocess.run(
        ["git", "show", f"{base}:{path.as_posix()}"],
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        return None
    return json.loads(result.stdout)


def immutable_view(version: dict) -> dict:
    return {key: value for key, value in version.items() if key != "yanked"}


def verify_archive(name: str, repository: str, version: dict, extract: Path | None) -> None:
    archive = version.get("archive", "")
    checksum = version.get("checksum", "")
    if not archive.startswith("https://github.com/") or "/releases/download/" not in archive:
        fail(f"{name}@{version.get('version')}: archive must be an immutable GitHub release URL")
    if not SHA256.fullmatch(checksum):
        fail(f"{name}@{version.get('version')}: invalid SHA-256")
    with tempfile.TemporaryDirectory() as directory:
        destination = Path(directory) / "package.tar.gz"
        with urllib.request.urlopen(archive, timeout=60) as response:
            destination.write_bytes(response.read())
        if hashlib.sha256(destination.read_bytes()).hexdigest() != checksum:
            fail(f"{name}@{version.get('version')}: archive checksum mismatch")
        with tarfile.open(destination, "r:gz") as package:
            members = package.getmembers()
            paths = {member.name.removeprefix("./") for member in members}
            if "encore.toml" not in paths:
                fail(f"{name}@{version.get('version')}: archive has no root encore.toml")
            for member in members:
                path = Path(member.name.removeprefix("./"))
                if member.issym() or member.islnk() or member.isdev() or path.is_absolute() or ".." in path.parts:
                    fail(f"{name}@{version.get('version')}: unsafe archive member {member.name}")
            manifest_member = next(
                member for member in members if member.name.removeprefix("./") == "encore.toml"
            )
            manifest = tomllib.loads(package.extractfile(manifest_member).read().decode())
            project = manifest.get("project", {})
            expected = {
                "name": name,
                "version": version.get("version"),
                "repository": repository,
                "encore": version.get("encore"),
            }
            for field, value in expected.items():
                if project.get(field) != value:
                    fail(
                        f"{name}@{version.get('version')}: manifest {field} "
                        f"does not match index metadata"
                    )
            if extract is not None:
                package_root = extract / f"{name}-{version['version']}"
                package_root.mkdir(parents=True, exist_ok=False)
                package.extractall(package_root)


def validate(path: Path, base: str | None, download: bool, extract: Path | None) -> None:
    document = load(path)
    name = document.get("name", "")
    if not NAME.fullmatch(name) or path.as_posix() != f"{name[:2]}/{name}.json":
        fail(f"{path}: sparse path/name mismatch")
    versions = document.get("versions")
    if not isinstance(versions, list) or not versions:
        fail(f"{path}: versions must be a non-empty array")
    by_version: dict[str, dict] = {}
    for item in versions:
        version = item.get("version", "")
        if not SEMVER.fullmatch(version) or version in by_version:
            fail(f"{path}: invalid or duplicate version {version!r}")
        if not isinstance(item.get("yanked"), bool):
            fail(f"{name}@{version}: yanked must be boolean")
        by_version[version] = item

    old = base_document(base, path) if base else None
    old_versions = {item["version"]: item for item in old.get("versions", [])} if old else {}
    if old:
        if document.get("name") != old.get("name"):
            fail(f"{path}: package name is immutable")
        if old.get("repository") and document.get("repository") != old.get("repository"):
            fail(f"{path}: repository ownership is immutable")
        for version, item in old_versions.items():
            current = by_version.get(version)
            if current is None:
                fail(f"{path}: published version {version} was removed")
            if immutable_view(current) != immutable_view(item):
                fail(f"{name}@{version}: immutable metadata changed")
            if item.get("yanked") is True and current.get("yanked") is False:
                fail(f"{name}@{version}: a yank cannot be reverted")

    repository = document.get("repository", "")
    new_versions = (
        [item for version, item in by_version.items() if version not in old_versions]
        if base
        else []
    )
    if new_versions:
        owner = GITHUB.fullmatch(repository)
        if not owner:
            fail(f"{path}: new versions require GitHub repository ownership")
        for item in new_versions:
            version = item["version"]
            if not item.get("encore"):
                fail(f"{name}@{version}: missing compiler requirement")
            if not COMMIT.fullmatch(item.get("commit", "")):
                fail(f"{name}@{version}: invalid source commit")
            if not isinstance(item.get("subdir"), str):
                fail(f"{name}@{version}: invalid source subdir")
            expected_prefix = f"https://github.com/{owner.group(1)}/releases/download/{name}-v{version}/"
            if not item.get("archive", "").startswith(expected_prefix):
                fail(f"{name}@{version}: archive does not belong to package owner/tag")
            if download:
                verify_archive(name, repository, item, extract)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", help="base Git revision used for append-only checks")
    parser.add_argument("--download", action="store_true", help="download and inspect archives")
    parser.add_argument(
        "--extract-new",
        type=Path,
        help="safely extract each new package archive for build verification",
    )
    args = parser.parse_args()
    if args.extract_new is not None and not args.base:
        fail("--extract-new requires --base")
    if args.extract_new is not None:
        args.extract_new.mkdir(parents=True, exist_ok=True)
    paths = sorted(Path(".").glob("*/*.json"))
    if not paths:
        fail("index contains no package metadata")
    for path in paths:
        validate(path, args.base, args.download, args.extract_new)
    print(f"validated {len(paths)} package metadata files")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
