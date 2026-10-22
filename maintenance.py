#!/usr/bin/env python3
"""Offline checked snapshots and API-based import. Never executes package code."""
import argparse
import contextlib
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import sqlite3
import tarfile
import tomllib
import urllib.parse
import urllib.request

LIMIT = 4 * 1024 * 1024


@contextlib.contextmanager
def locked(directory):
    if not directory.is_dir() or directory.is_symlink():
        raise ValueError("source must be an existing ordinary directory")
    fd = os.open(directory / "service.lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield
    finally:
        os.close(fd)


def digest_file(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def snapshot(source, destination, restoring=False):
    source, destination = source.absolute(), destination.absolute()
    if destination.exists() or destination.is_symlink():
        raise ValueError("destination must not exist; existing installations are never overwritten")
    if source == destination or source in destination.parents:
        raise ValueError("snapshot destination cannot be inside source")
    with locked(source):
        if (source / "INCOMPLETE").exists():
            raise ValueError("source is an incomplete snapshot")
        db_path = source / "registry.sqlite3"
        if db_path.is_symlink() or not db_path.is_file():
            raise ValueError("missing or unsafe database")
        with sqlite3.connect(db_path.as_uri() + "?mode=ro", uri=True) as db:
            if db.execute("PRAGMA integrity_check").fetchone() != ("ok",):
                raise ValueError("database integrity failure")
            if db.execute("PRAGMA user_version").fetchone() not in ((1,), (2,), (3,)):
                raise ValueError("unsupported schema version")
            archives = dict(db.execute("SELECT checksum,size FROM versions"))
            for checksum, size in archives.items():
                if not re.fullmatch(r"[0-9a-f]{64}", checksum):
                    raise ValueError("invalid stored checksum")
                archive = source / "archives" / (checksum + ".tar.gz")
                if archive.is_symlink() or not archive.is_file() or archive.stat().st_size != size:
                    raise ValueError("missing/unsafe archive: " + checksum)
                if digest_file(archive) != checksum:
                    raise ValueError("archive checksum mismatch: " + checksum)
            if restoring:
                marker = json.loads((source / "SNAPSHOT.json").read_text())
                if marker.get("schema") != 1 or marker.get("archives") != archives:
                    raise ValueError("snapshot manifest mismatch")
                if marker.get("database_sha256") != digest_file(db_path):
                    raise ValueError("snapshot database checksum mismatch")
            destination.mkdir(mode=0o700)
            # A failure leaves an explicitly incomplete directory for inspection,
            # never a seemingly valid snapshot or a removed user directory.
            (destination / "INCOMPLETE").touch(mode=0o600)
            (destination / "archives").mkdir(mode=0o700)
            target_db = destination / "registry.sqlite3"
            target_db.touch(mode=0o600)
            with sqlite3.connect(target_db) as target:
                db.backup(target)
                target.execute("PRAGMA journal_mode=DELETE")
            for checksum in archives:
                filename = checksum + ".tar.gz"
                target = destination / "archives" / filename
                shutil.copyfile(source / "archives" / filename, target, follow_symlinks=False)
                target.chmod(0o600)
                if digest_file(target) != checksum:
                    raise ValueError("copy checksum mismatch")
                with target.open("rb") as stream:
                    os.fsync(stream.fileno())
            marker = {"schema": 1, "archives": archives, "database_sha256": digest_file(target_db)}
            (destination / "SNAPSHOT.json").write_text(json.dumps(marker, sort_keys=True) + "\n")
            (destination / "INCOMPLETE").unlink()
            for path in (target_db, destination / "SNAPSHOT.json", destination / "archives", destination):
                fd = os.open(path, os.O_RDONLY)
                try:
                    os.fsync(fd)
                finally:
                    os.close(fd)
    print(f"{'Restored' if restoring else 'Backed up'} {len(archives)} archives into {destination}")


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise ValueError("redirect refused: credentials must never change origin")


class ArchiveRedirect(urllib.request.HTTPRedirectHandler):
    """Public release assets may redirect to HTTPS object storage, never HTTP."""
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        parsed = urllib.parse.urlsplit(newurl)
        if parsed.scheme != "https" or parsed.username or parsed.password:
            raise ValueError("archive redirect must use HTTPS without credentials")
        if req.has_header("Authorization") or req.has_header("Cookie"):
            raise ValueError("archive downloads must not carry credentials")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def import_index(index, url, packages, archive_cache=None):
    parsed = urllib.parse.urlsplit(url)
    if parsed.username or parsed.password or parsed.query or parsed.fragment or parsed.path not in ("", "/"):
        raise ValueError("registry URL must be an origin without credentials")
    if parsed.scheme != "https" and not (parsed.scheme == "http" and parsed.hostname in ("localhost", "127.0.0.1", "::1")):
        raise ValueError("registry requires HTTPS or loopback HTTP")
    token = os.environ.get("ENCORE_INDEX_TOKEN") or os.environ.get("ENCORE_REGISTRY_TOKEN", "")
    if not re.fullmatch(r"enr_[0-9a-f]{64}", token):
        raise ValueError("set ENCORE_INDEX_TOKEN")
    opener = urllib.request.build_opener(NoRedirect)
    # Separate unauthenticated transport: GitHub release assets redirect to its
    # object store. The registry token is attached only by request() below.
    archives = urllib.request.build_opener(ArchiveRedirect)

    def request(path, method, body):
        req = urllib.request.Request(url.rstrip("/") + path, data=body, method=method,
                                     headers={"Authorization": "Bearer " + token})
        with opener.open(req, timeout=90) as response:
            return json.load(response)

    count = 0
    for metadata_path in sorted(index.glob("*/*.json")):
        metadata = json.loads(metadata_path.read_text())
        name = metadata.get("name", "")
        if not re.fullmatch(r"[a-z0-9_-]{2,64}", name) or (packages and name not in packages):
            continue
        for version in metadata["versions"]:
            checksum = version["checksum"]
            if not re.fullmatch(r"[0-9a-f]{64}", checksum):
                raise ValueError("invalid archive checksum: " + name)
            archive_url = version["archive"]
            if urllib.parse.urlsplit(archive_url).scheme not in ("https", "file"):
                raise ValueError("import archive must use HTTPS or local file URL")
            archive_origin = urllib.parse.urlsplit(archive_url)
            if archive_origin.username or archive_origin.password:
                raise ValueError("archive URL must not contain credentials")
            cached = sorted(archive_cache.glob("*-" + checksum + ".tar.gz")) if archive_cache else []
            source = cached[0].open("rb") if cached else archives.open(archive_url, timeout=20)
            with source as response:
                archive = response.read(LIMIT + 1)
            if len(archive) > LIMIT or hashlib.sha256(archive).hexdigest() != version["checksum"]:
                raise ValueError("archive size or checksum mismatch: " + name)
            with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as tar:
                member = tar.getmember("encore.toml")
                if not member.isfile() or member.size > 262144:
                    raise ValueError("missing or oversized manifest")
                project = tomllib.loads(tar.extractfile(member).read().decode())["project"]
                if project["name"] != name or project["version"] != version["version"]:
                    raise ValueError("index identity does not match archive")
            ident = request("/api/v1/uploads", "POST", b"")["upload"]
            for offset in range(0, len(archive), 65536):
                request(f"/api/v1/uploads/{ident}/{offset}", "PUT", archive[offset:offset + 65536])
            finish = {"checksum": version["checksum"], "nametag": version.get("nametag", "neumann")}
            request(f"/api/v1/uploads/{ident}/finish", "POST", json.dumps(finish).encode())
            if version.get("yanked"):
                request(f"/api/v1/packages/{name}/{version['version']}/yank", "POST", b"")
            count += 1
            print(f"Imported {name} {version['version']}", flush=True)
    print(f"Imported {count} immutable versions through validation API")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("backup", "restore"):
        command = commands.add_parser(name)
        command.add_argument("source", type=Path)
        command.add_argument("destination", type=Path)
    command = commands.add_parser("import")
    command.add_argument("index", type=Path)
    command.add_argument("--url", required=True)
    command.add_argument("--package", action="append", default=[])
    command.add_argument("--archive-cache", type=Path, help="Read matching, checksum-verified local Encore archives before downloading")
    args = parser.parse_args()
    if args.command == "import":
        import_index(args.index, args.url, args.package, args.archive_cache)
    else:
        snapshot(args.source, args.destination, args.command == "restore")


if __name__ == "__main__":
    main()
