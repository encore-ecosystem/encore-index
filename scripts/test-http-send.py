#!/usr/bin/env python3
"""Exercise std::http send using a local TLS server, no external network."""
import http.server
import os
from pathlib import Path
import ssl
import subprocess
import tempfile
import threading
import time
from tls_fixtures import generate_certificates


def main():
    requests = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def handle_request(self):
            body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
            requests.append((self.command, self.path, self.headers.get("Authorization")))
            if self.path == "/slow":
                self.send_response(200)
                self.send_header("Content-Length", "20")
                self.end_headers()
                try:
                    for _ in range(20):
                        self.wfile.write(b"x")
                        self.wfile.flush()
                        time.sleep(.05)
                except (BrokenPipeError, ConnectionResetError):
                    pass
                return
            if self.path == "/redirect":
                self.send_response(302)
                self.send_header("Location", "/must-not-follow")
                response = b""
            else:
                self.send_response(200)
                response = b"x" * 128 if self.path == "/oversized" else self.command.encode() + b":" + body
            self.send_header("Content-Length", str(len(response)))
            self.end_headers()
            self.wfile.write(response)

        do_GET = do_POST = do_PUT = do_DELETE = handle_request

        def log_message(self, *_args):
            pass

    with tempfile.TemporaryDirectory(prefix="encore-http-send-") as temporary:
        root = Path(temporary)
        certificates = generate_certificates(root)
        with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server, http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as local:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(certificates.server, certificates.key)
            server.socket = context.wrap_socket(server.socket, server_side=True)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            local_thread = threading.Thread(target=local.serve_forever, daemon=True)
            local_thread.start()
            try:
                subprocess.run(
                    [os.environ.get("ENCORE_TEST_COMPILER", "encore"), "test", "--filter", "http_send", "--jobs", "1"],
                    cwd=Path(__file__).resolve().parents[1] / "packages" / "std",
                    env={**os.environ, "ENCORE_HTTP_SEND_TEST_URL": f"https://localhost:{server.server_port}",
                         "ENCORE_HTTP_LOCAL_TEST_URL": f"http://localhost:{local.server_port}",
                         "ENCORE_HTTP_SEND_TEST_CA": str(certificates.ca)},
                    timeout=120, check=True)
            finally:
                server.shutdown()
                thread.join()
                local.shutdown()
                local_thread.join()
        assert requests == [
            ("POST", "/echo", "Bearer disposable-test"),
            ("PUT", "/echo", "Bearer disposable-test"),
            ("DELETE", "/echo", "Bearer disposable-test"),
            ("POST", "/redirect", "Bearer disposable-test"),
            ("GET", "/oversized", None),
            ("POST", "/echo", None),
            ("GET", "/slow", None),
        ], requests
    print("HTTP send: methods/body, TLS validation, no credential redirects, limits and injection passed")


if __name__ == "__main__":
    main()
