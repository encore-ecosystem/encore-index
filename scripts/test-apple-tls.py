#!/usr/bin/env python3
"""Native Network.framework TLS, certificate and cancellation regression checks."""
import contextlib
import argparse
import os
from pathlib import Path
import platform
import socketserver
import ssl
import subprocess
import tempfile
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures-only", action="store_true", help="validate certificate fixtures on any OS")
    fixtures_only = parser.parse_args().fixtures_only
    if platform.system() != "Darwin" and not fixtures_only:
        raise SystemExit("This test requires macOS and its Network.framework")
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="encore-network-") as directory:
        temp = Path(directory)
        binary = temp / "tls-test"
        if not fixtures_only:
            subprocess.run(["clang", "-std=c11", "-fblocks", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address", "-g", str(root / "packages/platform/tests/tls_apple.c"),
                            "-framework", "Network", "-framework", "Security", "-framework", "CoreFoundation",
                            "-o", str(binary)], check=True)
        key, cert = temp / "key.pem", temp / "cert.pem"
        cert_config = temp / "cert.cnf"
        cert_config.write_text("[req]\ndistinguished_name=dn\nx509_extensions=extensions\n"
                               "[dn]\n[extensions]\nsubjectAltName=DNS:localhost\n"
                               "basicConstraints=critical,CA:TRUE\n")
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-subj", "/CN=localhost", "-config", str(cert_config),
                        "-keyout", str(key), "-out", str(cert)], check=True, capture_output=True)
        invalid = temp / "invalid.pem"
        invalid.write_text("not a certificate")
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        leaf_key, csr, expired = temp / "leaf.key", temp / "leaf.csr", temp / "expired.pem"
        subprocess.run(["openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes",
                        "-subj", "/CN=localhost", "-keyout", str(leaf_key), "-out", str(csr)],
                       check=True, capture_output=True)
        (temp / "database").touch()
        (temp / "serial").write_text("01\n")
        ca_config = temp / "ca.cnf"
        ca_config.write_text("[ca]\ndefault_ca=authority\n[authority]\n"
                             f"database={temp}/database\nserial={temp}/serial\nnew_certs_dir={temp}\n"
                             f"certificate={cert}\nprivate_key={key}\n"
                             "default_md=sha256\npolicy=names\nx509_extensions=extensions\n"
                             "[names]\ncommonName=supplied\n[extensions]\n"
                             "subjectAltName=DNS:localhost\nbasicConstraints=critical,CA:FALSE\n")
        subprocess.run(["openssl", "ca", "-batch", "-notext", "-config", str(ca_config),
                        "-startdate", "20000101000000Z", "-enddate", "20000102000000Z",
                        "-in", str(csr), "-out", str(expired)], check=True, capture_output=True)
        expired_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        expired_context.load_cert_chain(expired, leaf_key)
        if fixtures_only:
            subprocess.run(["openssl", "verify", "-CAfile", str(cert), str(cert)], check=True, capture_output=True)
            rejected = subprocess.run(["openssl", "verify", "-CAfile", str(cert), str(expired)], capture_output=True)
            assert rejected.returncode != 0 and b"expired" in rejected.stdout + rejected.stderr
            print("TLS certificate fixtures: valid CA and expired signed leaf verified")
            return

        class Server(socketserver.ThreadingTCPServer):
            daemon_threads = True
            allow_reuse_address = True

        class Handler(socketserver.BaseRequestHandler):
            def handle(self):
                self.request.settimeout(3)
                with contextlib.suppress(ssl.SSLError, OSError):
                    if self.server.mode == "handshake":
                        time.sleep(1)
                        return
                    tls = expired_context if self.server.mode == "expired" else context
                    with tls.wrap_socket(self.request, server_side=True) as stream:
                        data = b""
                        while len(data) < 5:
                            chunk = stream.recv(5 - len(data))
                            if not chunk:
                                return
                            data += chunk
                        if self.server.mode == "timeout":
                            time.sleep(1)
                        else:
                            stream.sendall(data)
                            stream.unwrap().close()

        def probe(server, host="localhost", ca=cert, timeout=2000, mode="echo"):
            start = time.monotonic()
            subprocess.run([str(binary), host, str(server.server_address[1]), str(ca), str(timeout), mode],
                           check=True, timeout=8, env={**os.environ, "ASAN_OPTIONS": "abort_on_error=1"})
            if timeout == 100:
                assert time.monotonic() - start < 3, "TLS cancellation exceeded deadline allowance"

        for mode in ("echo", "expired", "timeout", "handshake"):
            with Server(("127.0.0.1", 0), Handler) as server:
                server.mode = mode
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                try:
                    if mode == "echo":
                        probe(server)
                        probe(server, host="127.0.0.1", mode="reject")
                        probe(server, ca="", mode="reject")
                        probe(server, ca=invalid, mode="reject")
                        probe(server, ca=temp / "missing.pem", mode="reject")
                        probe(server, timeout=0, mode="reject")
                    elif mode == "expired":
                        probe(server, mode="reject")
                    else:
                        for _ in range(4):
                            probe(server, timeout=100, mode="reject" if mode == "handshake" else "timeout")
                finally:
                    server.shutdown()
                    thread.join()
    print("Network.framework: echo/EOF, CA/hostname rejection and cancellation passed (ASan)")


if __name__ == "__main__":
    main()
