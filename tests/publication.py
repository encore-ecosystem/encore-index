#!/usr/bin/env python3
"""End-to-end publication through the real Blaze server and Encore CLI."""
import concurrent.futures
import hashlib
import io
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request

PORT = int(os.environ.get("ENCORE_INDEX_PORT", "8137"))
BASE = f"http://127.0.0.1:{PORT}"


def archive(name="sample", version="1.0.0", extra=None, manifest_extra=""):
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode="w:gz") as tar:
        for path, text in {
            "encore.toml": f'[project]\nname = "{name}"\nversion = "{version}"\nencore = ">=0.0.0"\ndependencies = []\n{manifest_extra}',
            "src/lib.enq": "pub fn value() -> u32 { ret 42_u32 }\n",
        }.items():
            data = text.encode()
            member = tarfile.TarInfo(path)
            member.size = len(data)
            tar.addfile(member, io.BytesIO(data))
        if extra is not None:
            tar.addfile(extra)
    return out.getvalue()


def request(path, method="GET", data=None, token=None, expected=200):
    headers = {"Authorization": "Bearer " + token} if token else {}
    req = urllib.request.Request(BASE + path, data=data, headers=headers, method=method)
    try:
        response = urllib.request.urlopen(req, timeout=10)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        body = response.read()
        assert response.status == expected, (path, response.status, body)
        return body


def main():
    binary = str(Path(sys.argv[1]).resolve())
    compiler = str(Path(sys.argv[2]).resolve()) if len(sys.argv) > 2 else None
    with socket.socket() as probe:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        probe.bind(("127.0.0.1", PORT))
    with tempfile.TemporaryDirectory(prefix="registry-publication-") as scratch:
        root = Path(scratch)
        env = {**os.environ, "ENCORE_INDEX_DATA": str(root / "data"), "ENCORE_INDEX_PUBLIC_URL": BASE}
        env.pop("ENCORE_INDEX_URL", None)

        def admin(*args):
            return subprocess.check_output([binary, *args], env=env, text=True).strip()

        admin("init")
        for user in ("alice", "bob", "invalid"):
            admin("user-add", user)
        alice, bob, invalid = (admin("token-create", user) for user in ("alice", "bob", "invalid"))

        def start():
            process = subprocess.Popen([binary, "serve"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            for _ in range(100):
                assert process.poll() is None, process.communicate()
                try:
                    request("/healthz")
                    return process
                except OSError:
                    time.sleep(.02)
            raise AssertionError("startup timed out")

        def stop(process):
            process.terminate()
            try:
                process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()

        def upload(data, token=alice, expected=201, repeat_chunk=False):
            ident = json.loads(request("/api/v1/uploads", "POST", b"", token, 201))["upload"]
            for offset in range(0, len(data), 65536):
                chunk = data[offset:offset + 65536]
                request(f"/api/v1/uploads/{ident}/{offset}", "PUT", chunk, token)
                if repeat_chunk:
                    request(f"/api/v1/uploads/{ident}/{offset}", "PUT", chunk, token)
            finish = json.dumps({"checksum": hashlib.sha256(data).hexdigest(), "nametag": "neumann"}).encode()
            result = request(f"/api/v1/uploads/{ident}/finish", "POST", finish, token, expected)
            return ident, finish, result

        server = start()
        try:
            maintenance = Path(__file__).resolve().parents[1] / "maintenance.py"
            blocked_backup = subprocess.run([sys.executable, str(maintenance), "backup", env["ENCORE_INDEX_DATA"], str(root / "busy-backup")], capture_output=True)
            assert blocked_backup.returncode != 0 and not (root / "busy-backup").exists()
            request("/api/v1/uploads", "POST", b"", expected=401)
            request("/api/v1/uploads", "POST", b"x" * 65537, expected=413)
            request("/api/v1/uploads", "POST", b"", "enr_" + "0" * 64, 401)
            config = json.loads(request("/index/config.json"))
            assert config["api"] == "/api/v1"
            request("/api/v1/packages/missing", expected=404)
            data = archive()
            ident, finish, result = upload(data, repeat_chunk=True)
            assert json.loads(result)["published"]
            request(f"/api/v1/uploads/{ident}/finish", "POST", finish, alice)
            upload(data, expected=200)
            upload(archive(manifest_extra='description = "changed"\n'), expected=409)
            upload(archive(version="1.0.1"), token=bob, expected=403)
            api_package = json.loads(request("/api/v1/packages/sample"))
            assert api_package == json.loads(request("/index/sa/sample.json"))
            metadata = api_package["versions"][0]
            assert metadata["checksum"] == hashlib.sha256(data).hexdigest()
            assert request(metadata["archive"].removeprefix(BASE)) == data
            assert b"sample" in request("/api/v1/packages?q=sam")
            assert json.loads(request("/api/v1/packages?q=missing")) == []
            assert json.loads(request("/api/v1/packages?after=sample")) == []
            request("/api/v1/packages/sample/1.0.0/yank", "POST", b"", bob, 403)
            request("/api/v1/packages/sample/1.0.0/yank", "POST", b"", alice)
            assert json.loads(request("/index/sa/sample.json"))["versions"][0]["yanked"]
            assert request(metadata["archive"].removeprefix(BASE)) == data
            request("/api/v1/packages/sample/1.0.0/unyank", "POST", b"", alice)
            assert not json.loads(request("/index/sa/sample.json"))["versions"][0]["yanked"]
            bad_link = tarfile.TarInfo("src/escape")
            bad_link.type, bad_link.linkname = tarfile.SYMTYPE, "/etc/passwd"
            rejected_id, _, _ = upload(archive(extra=bad_link), token=invalid, expected=400)
            upload(archive(extra=tarfile.TarInfo("../escape")), token=invalid, expected=400)
            upload(archive(manifest_extra='name = "duplicate"\n'), token=invalid, expected=400)
            upload(b"not a gzip archive", token=invalid, expected=400)
            request("/api/v1/uploads", "POST", b"", invalid, 429)
            request(f"/api/v1/uploads/{rejected_id}", "DELETE", b"", bob, 404)
            request(f"/api/v1/uploads/{rejected_id}", "DELETE", b"", invalid)
            cancelled_id = json.loads(request("/api/v1/uploads", "POST", b"", invalid, 201))["upload"]
            request(f"/api/v1/uploads/{cancelled_id}", "DELETE", b"", invalid)
            with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
                list(pool.map(lambda _: request("/index/sa/sample.json"), range(100)))
            index = root / "old-index"
            (index / "im").mkdir(parents=True)
            imported = archive(name="imported")
            imported_archive = root / "imported.tar.gz"
            imported_archive.write_bytes(imported)
            (index / "im/imported.json").write_text(json.dumps({"name": "imported", "versions": [{"version": "1.0.0", "archive": imported_archive.as_uri(), "checksum": hashlib.sha256(imported).hexdigest(), "yanked": True}]}))
            subprocess.run([sys.executable, str(maintenance), "import", str(index), "--url", BASE], env={**env, "ENCORE_INDEX_TOKEN": alice, "ENCORE_REGISTRY_TOKEN": "invalid-legacy-token"}, check=True, capture_output=True)
            assert json.loads(request("/index/im/imported.json"))["versions"][0]["yanked"]
            # Fail after the immutable file has been linked but before the
            # SQLite transaction commits. No version may become visible.
            with sqlite3.connect(root / "data/registry.sqlite3") as db:
                db.execute("CREATE TRIGGER fail_publish BEFORE INSERT ON versions WHEN NEW.package='recover' BEGIN SELECT RAISE(ABORT,'injected failure'); END")
            recovery_data = archive(name="recover")
            old_id, old_finish, _ = upload(recovery_data, expected=503)
            request("/index/re/recover.json", expected=404)
            with sqlite3.connect(root / "data/registry.sqlite3") as db:
                db.execute("DROP TRIGGER fail_publish")
            server.kill()
            server.communicate(timeout=5)
            server = start()
            request(f"/api/v1/uploads/{old_id}/finish", "POST", old_finish, alice, 404)
            upload(recovery_data)
            assert json.loads(request("/index/re/recover.json"))["versions"][0]["checksum"] == hashlib.sha256(recovery_data).hexdigest()
            if compiler:
                package = root / "publisher with spaces"
                (package / "src").mkdir(parents=True)
                (package / "encore.toml").write_text('[project]\nname = "cliprobe"\nversion = "1.0.0"\nencore = ">=0.0.0"\ndependencies = []\n[registry]\nindex = "' + BASE + '/index"\n')
                (package / "src/lib.enq").write_text("pub fn value() -> u32 { ret 42_u32 }\n")
                subprocess.run(["git", "init", "-q"], cwd=package, check=True)
                subprocess.run(["git", "add", "encore.toml", "src"], cwd=package, check=True)
                client_env = {**env, "ENCORE_REGISTRY_TOKEN": alice, "ENCORE_REGISTRY_TOKEN_INDEX": BASE + "/index", "ENCORE_REGISTRY_CACHE": str(root / "cache")}
                for args in (("publish", "--dry-run"), ("publish",), ("publish",)):
                    run = subprocess.run([compiler, *args], cwd=package, env=client_env, text=True, capture_output=True, timeout=120)
                    assert run.returncode == 0, run.stdout + run.stderr
                    assert alice not in run.stdout + run.stderr
                    if "--dry-run" in args:
                        request("/index/cl/cliprobe.json", expected=404)
                # A rejected version must release staging; repeated mistakes
                # must not exhaust the four-upload quota.
                (package / "src/lib.enq").write_text("pub fn value() -> u32 { ret 43_u32 }\n")
                for _ in range(5):
                    rejected = subprocess.run([compiler, "publish"], cwd=package, env=client_env, text=True, capture_output=True, timeout=120)
                    assert rejected.returncode != 0 and "HTTP 409" in rejected.stderr + rejected.stdout, rejected.stdout + rejected.stderr
                    assert alice not in rejected.stdout + rejected.stderr
                (package / "src/lib.enq").write_text("pub fn value() -> u32 { ret 42_u32 }\n")
                consumer = root / "consumer"
                (consumer / "src").mkdir(parents=True)
                (consumer / "encore.toml").write_text('[project]\nname = "consumer"\nversion = "0.0.0"\ndependencies = []\n[registry]\nindex = "' + BASE + '/index"\n')
                (consumer / "src/main.enq").write_text('import cliprobe::value\nfn main() -> u32 { ret if value() == 42_u32 { 0_u32 } else { 1_u32 } }\n')
                for args in (("add", "cliprobe"), ("run",)):
                    run = subprocess.run([compiler, *args], cwd=consumer, env=client_env, text=True, capture_output=True, timeout=120)
                    assert run.returncode == 0, run.stdout + run.stderr
                assert 'registry = "' + BASE + '/index"' in (consumer / "encore.lock").read_text()
        finally:
            stop(server)
        if compiler:
            offline = subprocess.run([compiler, "run"], cwd=consumer, env=client_env, text=True, capture_output=True, timeout=120)
            assert offline.returncode == 0, offline.stdout + offline.stderr
        with sqlite3.connect(root / "data/registry.sqlite3") as db:
            assert db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
            assert db.execute("SELECT count(*) FROM versions WHERE package='sample'").fetchone()[0] == 1
            assert db.execute("SELECT count(*) FROM audit WHERE action='publish' AND package='sample'").fetchone()[0] == 1
        admin("token-revoke", "1")
        subprocess.run([sys.executable, str(maintenance), "backup", env["ENCORE_INDEX_DATA"], str(root / "snapshot")], check=True, capture_output=True)
        subprocess.run([sys.executable, str(maintenance), "restore", str(root / "snapshot"), str(root / "restored")], check=True, capture_output=True)
        env["ENCORE_INDEX_DATA"] = str(root / "restored")
        server = start()
        try:
            request("/index/sa/sample.json")
            request("/api/v1/uploads", "POST", b"", alice, 401)
        finally:
            stop(server)
        saved_archive = next((root / "snapshot/archives").glob("*.tar.gz"))
        saved_archive.write_bytes(b"corrupted")
        corrupted = subprocess.run([sys.executable, str(maintenance), "restore", str(root / "snapshot"), str(root / "bad-restore")], capture_output=True)
        assert corrupted.returncode != 0 and not (root / "bad-restore").exists()
    print("Publication passed: immutability, retries, auth, ownership, unsafe archives, quotas, yank, concurrency, import, backup/restore" + (", CLI" if compiler else ""))


if __name__ == "__main__":
    main()
