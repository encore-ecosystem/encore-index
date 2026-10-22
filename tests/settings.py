#!/usr/bin/env python3
"""Check setting aliases against the compiled server, with disposable storage."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *_args):
        return None


def main():
    binary = str(Path(sys.argv[1]).resolve())
    project = Path(__file__).resolve().parents[1]
    clean = {key: value for key, value in os.environ.items()
             if not key.startswith(("ENCORE_INDEX_", "ENCORE_REGISTRY_", "ENCORE_GITHUB_"))}
    with tempfile.TemporaryDirectory(prefix="encore-index-settings-") as temporary:
        root = Path(temporary)
        for label, primary, legacy, chosen in (
            ("primary", "primary", "legacy", "primary"),
            ("empty-primary", "", "legacy", "legacy"),
            ("legacy", None, "legacy", "legacy"),
            ("defaults", None, None, "data"),
        ):
            cwd = root / label
            cwd.mkdir()
            env = dict(clean)
            if primary is not None:
                env["ENCORE_INDEX_DATA"] = str(cwd / primary) if primary else ""
            if legacy is not None:
                env["ENCORE_REGISTRY_DATA"] = str(cwd / legacy)
            result = subprocess.run([binary, "init"], cwd=cwd, env=env,
                                    capture_output=True, timeout=10)
            assert result.returncode == 0, label
            assert (cwd / chosen / "registry.sqlite3").is_file(), label
            if chosen != "legacy":
                assert not (cwd / "legacy").exists(), label

        for label, extra, expected_client in (
            ("primary", {"ENCORE_GITHUB_CLIENT_ID": "old-client",
                         "ENCORE_GITHUB_CLIENT_SECRET": "old-secret",
                         "ENCORE_INDEX_GITHUB_CLIENT_ID": "new-client",
                         "ENCORE_INDEX_GITHUB_CLIENT_SECRET": "new-secret"}, "new-client"),
            ("empty-primary", {"ENCORE_INDEX_GITHUB_CLIENT_ID": "",
                               "ENCORE_INDEX_GITHUB_CLIENT_SECRET": "",
                               "ENCORE_GITHUB_CLIENT_ID": "old-client",
                               "ENCORE_GITHUB_CLIENT_SECRET": "old-secret"}, "old-client"),
            ("invalid-primary", {"ENCORE_INDEX_GITHUB_CLIENT_ID": "new-client",
                                 "ENCORE_INDEX_GITHUB_CLIENT_SECRET": "x" * 257,
                                 "ENCORE_GITHUB_CLIENT_ID": "old-client",
                                 "ENCORE_GITHUB_CLIENT_SECRET": "old-secret"}, None),
        ):
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0))
                port = probe.getsockname()[1]
            env = {**clean, **extra, "ENCORE_INDEX_PORT": str(port),
                   "ENCORE_REGISTRY_PORT": "0", "ENCORE_INDEX_DATA": str(root / ("http-" + label)),
                   "ENCORE_INDEX_CSS": str(project / "assets/index.css"),
                   "ENCORE_REGISTRY_CSS": str(root / "missing.css")}
            process = subprocess.Popen([binary, "serve"], cwd=project, env=env,
                                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            base = f"http://127.0.0.1:{port}"
            try:
                deadline = time.monotonic() + 10
                while True:
                    assert process.poll() is None, process.stderr.read().decode()
                    try:
                        with urllib.request.urlopen(base + "/index/config.json", timeout=.3) as response:
                            config = json.load(response)
                        break
                    except OSError:
                        assert time.monotonic() < deadline, "startup timed out"
                        time.sleep(.03)
                assert ("auth" in config) == (expected_client is not None), label
                for path in ("/assets/index.css", "/assets/registry.css"):
                    with urllib.request.urlopen(base + path, timeout=2) as response:
                        assert response.read() == (project / "assets/index.css").read_bytes()
                if expected_client:
                    opener = urllib.request.build_opener(NoRedirect())
                    try:
                        opener.open(base + "/auth/github/start", timeout=2)
                        raise AssertionError("expected OAuth redirect")
                    except urllib.error.HTTPError as response:
                        assert response.code == 303
                        query = urllib.parse.parse_qs(urllib.parse.urlsplit(response.headers["Location"]).query)
                        assert query["client_id"] == [expected_client]
                        assert query["redirect_uri"] == [base + "/auth/github/callback"]
            finally:
                process.terminate()
                try:
                    process.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate()

        for field, value, message in (
            ("PORT", "0", "ENCORE_INDEX_PORT"),
            ("CSS", str(root / "absent.css"), "ENCORE_INDEX_CSS"),
        ):
            env = {**clean, "ENCORE_INDEX_DATA": str(root / ("invalid-" + field)),
                   "ENCORE_REGISTRY_PORT": "8137", "ENCORE_REGISTRY_CSS": str(project / "assets/index.css"),
                   "ENCORE_INDEX_" + field: value}
            result = subprocess.run([binary, "serve"], cwd=project, env=env,
                                    capture_output=True, text=True, timeout=5)
            assert result.returncode != 0 and message in result.stderr, field
    print("Settings passed: primary/legacy/default, OAuth aliases, derived URL, CSS routes, invalid primary values")


if __name__ == "__main__":
    main()
