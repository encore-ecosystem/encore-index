#!/usr/bin/env python3
"""Exercise the compiled Encore/Blaze process, never a substitute HTTP server."""
import hashlib
import io
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import tarfile
import time
import urllib.error
import urllib.request


PORT = int(os.environ.get("ENCORE_INDEX_PORT", "8137"))
BASE = f"http://127.0.0.1:{PORT}"

def main():
    binary = str(Path(sys.argv[1]).resolve())
    # Refuse to test an unrelated process already listening on the preview port.
    with socket.socket() as probe:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        probe.bind(("127.0.0.1", PORT))
    with tempfile.TemporaryDirectory(prefix="encore-index-http-") as temp:
        env = {**os.environ, "ENCORE_INDEX_DATA": temp,
               "ENCORE_INDEX_PUBLIC_URL": BASE}
        env.pop("ENCORE_INDEX_URL", None)

        def admin(*args, success=True):
            run = subprocess.run([binary, *args], env=env, capture_output=True,
                                 text=True, timeout=10)
            assert (run.returncode == 0) == success, run.stdout + run.stderr
            return run.stdout.strip()

        admin("init")
        admin("user-add", "alice")
        token = admin("token-create", "alice")
        assert token.startswith("enr_") and len(token) == 68
        admin("reserve", "std", "alice")
        admin("reserve", "std", "alice", success=False)
        admin("reserve", "probe", "alice")
        archive = Path(temp) / "probe.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            for name, content in {
                "encore.toml": '[project]\nname = "probe"\nversion = "1.0.0"\ndependencies = []\n',
                "src/lib.enq": 'pub fn value() -> u32 { ret 42_u32 }\n',
            }.items():
                data = content.encode()
                entry = tarfile.TarInfo(name)
                entry.size = len(data)
                tar.addfile(entry, io.BytesIO(data))
        with sqlite3.connect(Path(temp) / "registry.sqlite3") as db:
            digest = db.execute("SELECT digest FROM tokens").fetchone()[0]
            assert digest == hashlib.sha256(token.encode()).hexdigest()
            metadata = {"version": "1.0.0", "archive": "https://example.org/std.tar.gz",
                        "checksum": "0" * 64, "encore": ">=0.0.0", "yanked": False}
            # Deliberately local fixture, not a publication API bypass in production.
            db.execute("INSERT INTO versions(package,version,release,checksum,size,manifest,metadata) VALUES(?,?,?,?,?,?,?)",
                       ("std", "1.0.0", "1.0.0-neumann", "0" * 64, 0, "", json.dumps(metadata)))
            db.execute("UPDATE packages SET description=? WHERE name='std'", ('<script>alert("x")</script>',))
            probe_meta = {**metadata, "archive": archive.as_uri(),
                          "checksum": hashlib.sha256(archive.read_bytes()).hexdigest()}
            db.execute("INSERT INTO versions(package,version,release,checksum,size,manifest,metadata) VALUES(?,?,?,?,?,?,?)",
                       ("probe", "1.0.0", "1.0.0-neumann", probe_meta["checksum"], archive.stat().st_size,
                        "", json.dumps(probe_meta)))
        admin("token-revoke", "1")
        with sqlite3.connect(Path(temp) / "registry.sqlite3") as db:
            assert db.execute("SELECT revoked FROM tokens").fetchone()[0] == 1
        server = subprocess.Popen([binary, "serve"], env=env,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            for _ in range(100):
                assert server.poll() is None, server.communicate()
                try:
                    with urllib.request.urlopen((BASE + "/healthz"), timeout=.2) as response:
                        assert json.load(response) == {"status": "ok", "publication": True}
                    break
                except (OSError, urllib.error.URLError):
                    time.sleep(.02)
            else:
                raise AssertionError("Blaze startup timeout")
            admin("init", success=False)
            for query in ("STD", "ALERT"):
                with urllib.request.urlopen((BASE + "/api/v1/packages?q=") + query, timeout=2) as response:
                    found = json.load(response)
                    assert [p["name"] for p in found] == ["std"]
                    assert found[0]["latest"]["version"] == "1.0.0"
            with sqlite3.connect(Path(temp) / "registry.sqlite3") as db:
                db.execute("UPDATE versions SET yanked=1 WHERE package='std'")
            with urllib.request.urlopen((BASE + "/?q=std"), timeout=2) as response:
                assert "All versions yanked" in response.read().decode()
            with sqlite3.connect(Path(temp) / "registry.sqlite3") as db:
                db.execute("UPDATE versions SET yanked=0 WHERE package='std'")
            with urllib.request.urlopen((BASE + "/"), timeout=2) as response:
                page = response.read().decode()
                assert response.headers.get_content_type() == "text/html"
                assert "&lt;script&gt;" in page and "<script>" not in page
                assert response.headers["Content-Security-Policy"]
                assert "style-src 'self'" in response.headers["Content-Security-Policy"]
                assert "unsafe-inline" not in response.headers["Content-Security-Policy"]
                assert '<link rel="stylesheet" href="/assets/index.css">' in page
                assert page.count('<script') == 1
                assert '<script defer src="/assets/copy.js"></script>' in page
                assert 'style=' not in page
            with urllib.request.urlopen((BASE + "/assets/index.css"), timeout=2) as response:
                assert response.headers.get_content_type() == "text/css"
                assert response.headers["Cache-Control"] == "no-cache"
                assert response.headers["X-Content-Type-Options"] == "nosniff"
                assert b"prefers-color-scheme: dark" in response.read()
            for query, expected in (("no+such+package", "no such package"), ("%22%3E%3Cscript%3E", "&quot;&gt;&lt;script&gt;")):
                with urllib.request.urlopen((BASE + "/?q=") + query, timeout=2) as response:
                    page = response.read().decode()
                    assert 'value="' + expected + '"' in page
                    assert "No packages found." in page and "<script>" not in page
            with urllib.request.urlopen((BASE + "/packages/std"), timeout=2) as response:
                assert "1.0.0" in response.read().decode()
            for _ in range(20):
                with urllib.request.urlopen((BASE + "/api/v1/packages"), timeout=2) as response:
                    assert response.headers.get_content_type() == "application/json"
                    assert [p["name"] for p in json.load(response)] == ["probe", "std"]
                with urllib.request.urlopen((BASE + "/index/st/std.json"), timeout=2) as response:
                    result = json.load(response)
                    assert result["name"] == "std" and result["versions"][0] == metadata
            for path in ("/index/xx/std.json", "/index/no/none.json", "/api/v1/publish"):
                try:
                    urllib.request.urlopen(BASE + path, timeout=2)
                    raise AssertionError("unexpected route success")
                except urllib.error.HTTPError as error:
                    assert error.code == 404
            with sqlite3.connect(Path(temp) / "registry.sqlite3") as db:
                for number in range(101):
                    name = f"page_{number:03d}"
                    db.execute("INSERT INTO packages(name,description) VALUES(?,?)", (name, "Long description " * 50))
                    db.execute("INSERT INTO versions(package,version,release,checksum,size,manifest,metadata) VALUES(?,?,?,?,?,?,?)",
                               (name, "1.0.0", "1.0.0-neumann", "0" * 64, 0, "", json.dumps(metadata)))
            with urllib.request.urlopen((BASE + "/?q=page_"), timeout=2) as response:
                page = response.read().decode()
                assert page.count('class="package-row"') == 100
                assert 'href="/?q=page_&amp;after=page_099"' in page
            with urllib.request.urlopen((BASE + "/?q=page_&after=page_099"), timeout=2) as response:
                page = response.read().decode()
                assert page.count('class="package-row"') == 1
                assert 'value="page_"' in page and "Next page" not in page
            if len(sys.argv) > 2:
                compiler = str(Path(sys.argv[2]).resolve())
                app = Path(temp) / "consumer"
                (app / "src").mkdir(parents=True)
                (app / "encore.toml").write_text('[project]\nname = "consumer"\nversion = "0.0.0"\ndependencies = []\n'
                                                f'[registry]\nindex = "{BASE}/index"\n')
                (app / "src/main.enq").write_text('import probe::value\nfn main() -> u32 { ret if value() == 42_u32 { 0_u32 } else { 1_u32 } }\n')
                client_env = {**env, "ENCORE_REGISTRY_CACHE": str(Path(temp) / "client-cache")}
                for args in (("add", "probe"), ("run",)):
                    run = subprocess.run([compiler, *args], cwd=app, env=client_env,
                                         capture_output=True, text=True, timeout=90)
                    assert run.returncode == 0, run.stdout + run.stderr
                assert f'registry = "{BASE}/index"' in (app / "encore.lock").read_text()
        finally:
            server.terminate()
            try:
                server.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.communicate()
        admin("init")
        missing_css = subprocess.run([binary, "serve"], env={**env, "ENCORE_INDEX_CSS": str(Path(temp) / "missing.css")}, capture_output=True, text=True, timeout=5)
        assert missing_css.returncode != 0 and "Cannot load registry CSS" in missing_css.stderr
    print("Blaze/SQLite integration: bootstrap, tokens, metadata, catalogue and restart passed")


if __name__ == "__main__":
    main()
