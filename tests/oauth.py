#!/usr/bin/env python3
"""Real Blaze + Encore HTTP client against a disposable HTTPS OAuth provider.

The provider override is compiled into a disposable test binary only.
"""
import base64
import hashlib
import html
import http.cookiejar
import http.server
import json
import os
import pty
import select
from pathlib import Path
import re
import shutil
import socket
import sqlite3
import ssl
import subprocess
import tempfile
import threading
import time
import tomllib
import urllib.error
import urllib.parse
import urllib.request
from publication import archive

PORT = int(os.environ.get("ENCORE_INDEX_PORT", "8137"))
BASE = f"http://127.0.0.1:{PORT}"
ROOT = Path(__file__).resolve().parents[1]


def browser_checks(session, code):
    executable = os.environ.get("ENCORE_TEST_BROWSER")
    if not executable:
        return
    from playwright.sync_api import sync_playwright
    output = ROOT / "target/auth-screenshots"
    output.mkdir(parents=True, exist_ok=True)
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(executable_path=executable)
        for theme in ("dark", "light"):
            for width in (375, 768, 1440):
                context = browser.new_context(color_scheme=theme, viewport={"width": width, "height": 900})
                context.add_cookies([{"name": "encore_session", "value": session, "url": BASE,
                                     "httpOnly": True, "sameSite": "Lax"}])
                page = context.new_page()
                for name, path in (("account", "/account"), ("device", "/auth/device?code=" + code)):
                    response = page.goto(BASE + path)
                    assert response.status == 200
                    assert page.locator("script[src='/assets/copy.js']").count() == 1
                    assert page.evaluate("document.documentElement.scrollWidth <= innerWidth"), (theme, width, name)
                    assert page.locator("h1").is_visible()
                    assert page.locator("form.account-form").count() > 0
                    page.screenshot(path=str(output / f"{name}-{theme}-{width}.png"), full_page=True)
                context.close()
        browser.close()


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *_args):
        return None


def main():
    with socket.socket() as probe:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        probe.bind(("127.0.0.1", PORT))
    with tempfile.TemporaryDirectory(prefix="encore-oauth-") as temporary:
        root = Path(temporary)
        project = root / "project"
        project.mkdir()
        for directory in ("src", "native", "assets"):
            shutil.copytree(ROOT / directory, project / directory)
        # The unpublished std transport must use its matching system ABI,
        # not the older platform package embedded in the installed compiler.
        for package in ("core", "platform"):
            shutil.copytree(ROOT.parent / "encore/packages" / package,
                            project / "index" / package,
                            ignore=shutil.ignore_patterns("target", ".git"))
        shutil.copy2(ROOT / "runtime.c", project / "runtime.c")
        manifest = (ROOT / "encore.toml").read_text()
        for reference in tomllib.loads(manifest)["project"]["dependencies"]:
            if reference.startswith("path@"):
                manifest = manifest.replace(reference, "path@" + str((ROOT / reference[5:]).resolve()))
        manifest += '\ncompile-args = ["-DENCORE_INDEX_AUTH_TEST"]\n'
        (project / "encore.toml").write_text(manifest)
        build = subprocess.run([os.environ.get("ENCORE_TEST_COMPILER", "encore"), "build"], cwd=project, text=True, capture_output=True, timeout=600)
        assert build.returncode == 0, build.stdout + build.stderr
        binary = project / "target/dev/encore-index"
        key, cert = root / "key.pem", root / "cert.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-days", "1", "-subj", "/CN=localhost", "-addext",
                        "subjectAltName=DNS:localhost", "-keyout", str(key),
                        "-out", str(cert)], check=True, capture_output=True)
        codes, provider_calls = {}, []

        class Provider(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def reply(self, status, value):
                data = json.dumps(value).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                parsed = urllib.parse.urlsplit(self.path)
                if parsed.path == "/authorize":
                    fields = urllib.parse.parse_qs(parsed.query)
                    assert fields["code_challenge_method"] == ["S256"]
                    assert fields["client_id"] == ["fixture-client"]
                    assert fields["redirect_uri"] == [BASE + "/auth/github/callback"]
                    code = "code-" + str(len(codes))
                    codes[code] = fields["code_challenge"][0]
                    self.send_response(302)
                    self.send_header("Location", BASE + "/auth/github/callback?" +
                                     urllib.parse.urlencode({"code": code, "state": fields["state"][0]}))
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                elif parsed.path == "/user":
                    assert self.headers.get("Authorization") == "Bearer provider-secret"
                    provider_calls.append("user")
                    self.reply(200, {"id": 4242, "login": "mutable-login"})
                else:
                    self.reply(404, {})

            def do_POST(self):
                fields = urllib.parse.parse_qs(self.rfile.read(int(self.headers["Content-Length"])).decode())
                expected = codes.pop(fields.get("code", [""])[0], None)
                challenge = base64.urlsafe_b64encode(hashlib.sha256(fields["code_verifier"][0].encode()).digest()).decode().rstrip("=")
                assert challenge == expected
                assert fields["client_secret"] == ["fixture-secret"]
                provider_calls.append("token")
                self.reply(200, {"access_token": "provider-secret", "token_type": "bearer", "scope": ""})

        with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider) as provider:
            tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            tls.load_cert_chain(cert, key)
            provider.socket = tls.wrap_socket(provider.socket, server_side=True)
            thread = threading.Thread(target=provider.serve_forever, daemon=True)
            thread.start()
            provider_base = f"https://localhost:{provider.server_port}"
            env = {**os.environ, "ENCORE_INDEX_DATA": str(root / "data"),
                   "ENCORE_INDEX_PUBLIC_URL": BASE, "ENCORE_INDEX_GITHUB_CLIENT_ID": "fixture-client",
                   "ENCORE_INDEX_GITHUB_CLIENT_SECRET": "fixture-secret",
                   "ENCORE_GITHUB_CLIENT_ID": "legacy-client-must-not-win",
                   "ENCORE_GITHUB_CLIENT_SECRET": "legacy-secret-must-not-win", "ENCORE_TEST_AUTHORIZE_URL": provider_base + "/authorize",
                   "ENCORE_TEST_TOKEN_URL": provider_base + "/token", "ENCORE_TEST_USER_URL": provider_base + "/user",
                   "ENCORE_TEST_CA": str(cert)}
            server = subprocess.Popen([binary, "serve"], cwd=project, env=env,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            jar = http.cookiejar.CookieJar()
            browser = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar),
                urllib.request.HTTPSHandler(context=ssl.create_default_context(cafile=str(cert))), NoRedirect())

            def request(path, fields=None, token=None, origin=None, client=browser, raw=None, method=None):
                headers = {}
                data = None
                if fields is not None:
                    data = urllib.parse.urlencode(fields).encode()
                    headers["Content-Type"] = "application/x-www-form-urlencoded"
                if token:
                    headers["Authorization"] = "Bearer " + token
                if origin:
                    headers["Origin"] = origin
                if raw is not None:
                    data = raw
                    headers["Content-Type"] = "application/octet-stream"
                try:
                    response = client.open(urllib.request.Request(
                        path if path.startswith("https://") else BASE + path,
                        data=data, headers=headers, method=method), timeout=10)
                except urllib.error.HTTPError as error:
                    response = error
                with response:
                    return response.status, response.headers, response.read().decode()

            try:
                for _ in range(100):
                    try:
                        if request("/healthz")[0] == 200:
                            break
                    except OSError:
                        time.sleep(.05)
                else:
                    raise AssertionError("registry startup failed")
                assert json.loads(request("/index/config.json")[2])["auth"]["version"] == 1
                status, headers, _ = request("/auth/github/start")
                assert status == 303 and "HttpOnly" in headers["Set-Cookie"]
                status, headers, _ = request(headers["Location"])
                callback = headers["Location"].removeprefix(BASE)
                other_browser = urllib.request.build_opener(NoRedirect())
                assert "invalid OAuth callback" in request(callback, client=other_browser)[2]
                assert not provider_calls
                status, headers, _ = request(callback)
                assert status == 303 and headers["Location"] == "/account"
                assert headers["Cache-Control"] == "no-store"
                assert provider_calls == ["token", "user"]
                assert "mismatched OAuth state" in request(callback)[2]
                assert provider_calls == ["token", "user"]
                status, headers, account = request("/account")
                assert status == 200 and "github_4242" in account
                csrf = html.unescape(re.search(r'name="csrf" value="([^"]+)"', account)[1])
                session = next(c.value for c in jar if c.name == "encore_session")
                assert request("/api/v1/me", token=session)[0] == 401
                fields = {"client_id": "encore", "device_name": "Test terminal"}
                status, _, body = request("/api/v1/auth/device", fields)
                assert status == 200
                device = json.loads(body)
                review = request("/auth/device?code=" + device["user_code"])[2]
                assert "Test terminal" in review and "Approve device" in review
                browser_checks(session, device["user_code"])
                approve = {"action": "approve", "code": device["user_code"], "csrf": csrf}
                assert request("/account/action", approve, origin="https://evil.invalid")[0] == 403
                assert request("/account/action", {**approve, "csrf": "wrong"}, origin=BASE)[0] == 403
                assert "Device request updated" in request("/account/action", approve, origin=BASE)[2]
                time.sleep(5.1)
                poll = {"client_id": "encore", "grant_type": "urn:ietf:params:oauth:grant-type:device_code",
                        "device_code": device["device_code"]}
                status, _, body = request("/api/v1/auth/token", poll)
                assert status == 200, body
                credential = json.loads(body)["access_token"]
                assert json.loads(request("/api/v1/me", token=credential)[2])["name"] == "github_4242"
                assert request("/api/v1/auth/token", poll)[0] == 400
                payload = archive(name="owned")
                status, _, body = request("/api/v1/uploads", {}, token=credential)
                assert status == 201, body
                upload = json.loads(body)["upload"]
                assert request("/api/v1/uploads/" + upload + "/0", token=credential, raw=payload, method="PUT")[0] == 200
                finish = json.dumps({"checksum": hashlib.sha256(payload).hexdigest(), "nametag": "neumann"}).encode()
                status, _, body = request("/api/v1/uploads/" + upload + "/finish", token=credential, raw=finish)
                assert status == 201, body
                settings = {"action": "ci", "csrf": csrf, "label": "Test CI", "days": "30", "packages": "owned", "publish": "on"}
                status, _, issued = request("/account/action", settings, origin=BASE)
                assert status == 200, issued
                ci_token = re.search(r'enc_[0-9a-f]{64}', issued)[0]
                assert request("/api/v1/me", token=ci_token)[0] == 200
                compiler = os.environ.get("ENCORE_TEST_CLI")
                if compiler:
                    cli_env = {**os.environ, "ENCORE_REGISTRY_TOKEN": ci_token, "ENCORE_REGISTRY_TOKEN_INDEX": BASE + "/index"}
                    def cli(action, binding=BASE + "/index"):
                        return subprocess.run([compiler, action, "--registry", BASE + "/index"],
                            env={**cli_env, "ENCORE_REGISTRY_TOKEN_INDEX": binding}, text=True,
                            capture_output=True, timeout=20)
                    account_cli = cli("whoami")
                    assert account_cli.returncode == 0 and "github_4242" in account_cli.stdout, account_cli.stderr
                    wrong = cli("whoami", "https://wrong.invalid/index")
                    assert wrong.returncode != 0 and "forwarding" in wrong.stderr
                    assert ci_token not in account_cli.stdout + account_cli.stderr + wrong.stdout + wrong.stderr
                    # Full interactive CLI flow through its actual native ABI,
                    # with an isolated test-only libsecret provider, not a user's keyring.
                    fixture = root / "keyring-provider"
                    fixture.mkdir()
                    subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/keyring-fixture.c"), "-o", str(fixture / "libsecret-1.so.0")], check=True)
                    interactive_env = {k: v for k, v in cli_env.items() if k not in ("ENCORE_REGISTRY_TOKEN", "ENCORE_REGISTRY_TOKEN_INDEX")}
                    interactive_env.update(LD_LIBRARY_PATH=str(fixture), ENCORE_TEST_KEYRING_FILE=str(root / "keyring-fixture"))
                    master, slave = pty.openpty()
                    login = subprocess.Popen([compiler, "login", "--registry", BASE + "/index"],
                        env=interactive_env, stdout=slave, stderr=slave)
                    os.close(slave)
                    transcript = b""
                    try:
                        until = time.monotonic() + 20
                        code_match = None
                        while time.monotonic() < until and login.poll() is None:
                            if select.select([master], [], [], .1)[0]:
                                transcript += os.read(master, 8192)
                                code_match = re.search(rb"enter code: ([A-Z0-9]{8})", transcript)
                                if code_match:
                                    break
                        assert code_match, transcript.decode(errors="replace")
                        approval = {"action": "approve", "code": code_match[1].decode(), "csrf": csrf}
                        assert request("/account/action", approval, origin=BASE)[0] == 200
                        assert login.wait(timeout=20) == 0
                    finally:
                        if login.poll() is None:
                            login.kill()
                            login.wait()
                        os.close(master)
                    saved = (root / "keyring-fixture").read_text().splitlines()[1]
                    assert saved.startswith("end_") and saved.encode() not in transcript
                    for action in ("whoami", "logout"):
                        result = subprocess.run([compiler, action, "--registry", BASE + "/index"],
                            env=interactive_env, text=True, capture_output=True, timeout=20)
                        assert result.returncode == 0, result.stdout + result.stderr
                        assert saved not in result.stdout + result.stderr
                    assert not (root / "keyring-fixture").exists()
                    assert request("/api/v1/me", token=saved)[0] == 401
                assert request("/api/v1/packages/owned/1.0.0/yank", {}, token=ci_token)[0] == 403
                assert request("/account/action", {**settings, "packages": "not-owned"}, origin=BASE)[0] == 400
                if compiler:
                    signed_out = cli("logout")
                    assert signed_out.returncode == 0, signed_out.stderr
                    assert cli("whoami").returncode != 0
                else:
                    assert request("/api/v1/auth/logout", {}, token=ci_token)[0] == 200
                assert request("/api/v1/me", token=ci_token)[0] == 401
                assert request("/api/v1/auth/logout", {}, token=credential)[0] == 200
                assert request("/api/v1/me", token=credential)[0] == 401
                assert request("/account/action", {"action": "logout", "csrf": csrf}, origin=BASE)[0] == 303
                assert "Sign in with GitHub" in request("/account")[2]
                with sqlite3.connect(root / "data/registry.sqlite3") as db:
                    dump = "\n".join(db.iterdump())
                    for secret in (session, credential, ci_token, device["device_code"], "provider-secret", "fixture-secret"):
                        assert secret not in dump
                    assert db.execute("SELECT count(*) FROM identities").fetchone() == (1,)
                    assert db.execute("SELECT count(*) FROM oauth_attempts").fetchone() == (0,)
            finally:
                server.terminate()
                try:
                    stdout, stderr = server.communicate(timeout=10)
                except subprocess.TimeoutExpired:
                    server.kill()
                    stdout, stderr = server.communicate()
                    raise AssertionError("registry shutdown timed out")
                provider.shutdown()
                thread.join()
                assert b"fixture-secret" not in stdout + stderr
                assert b"provider-secret" not in stdout + stderr
    print("OAuth E2E passed: TLS/PKCE, browser binding, replay, CSRF, device approval, CI scopes, publication and revocation" +
          ("; CLI login/whoami/logout with isolated test keyring" if os.environ.get("ENCORE_TEST_CLI") else ""))


if __name__ == "__main__":
    main()
