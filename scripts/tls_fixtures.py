"""Disposable CA and server certificates shared by native and Encore TLS tests."""
from dataclasses import dataclass
from pathlib import Path
import subprocess


@dataclass(frozen=True)
class Certificates:
    ca: Path
    server: Path
    key: Path
    expired: Path


def generate_certificates(root: Path) -> Certificates:
    def openssl(*args):
        return subprocess.run(["openssl", *map(str, args)], check=True, capture_output=True)

    ca, ca_key = root / "ca.pem", root / "ca.key"
    config = root / "ca-cert.cnf"
    config.write_text("[req]\ndistinguished_name=dn\nx509_extensions=extensions\n"
                      "[dn]\n[extensions]\nbasicConstraints=critical,CA:TRUE\n"
                      "keyUsage=critical,keyCertSign,cRLSign\nsubjectKeyIdentifier=hash\n")
    openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256", "-days", "2",
            "-subj", "/CN=Encore Test CA", "-config", config, "-keyout", ca_key, "-out", ca)
    key, csr = root / "server.key", root / "server.csr"
    openssl("req", "-new", "-newkey", "rsa:2048", "-nodes", "-sha256",
            "-subj", "/CN=localhost", "-keyout", key, "-out", csr)
    extensions = ("subjectAltName=DNS:localhost\nbasicConstraints=critical,CA:FALSE\n"
                  "keyUsage=critical,digitalSignature,keyEncipherment\n"
                  "extendedKeyUsage=serverAuth\nsubjectKeyIdentifier=hash\n"
                  "authorityKeyIdentifier=keyid,issuer\n")
    extfile = root / "server.cnf"
    extfile.write_text(extensions)
    server, expired = root / "server.pem", root / "expired.pem"
    openssl("x509", "-req", "-in", csr, "-CA", ca, "-CAkey", ca_key, "-set_serial", "1",
            "-days", "1", "-sha256", "-extfile", extfile, "-out", server)
    (root / "database").touch()
    (root / "serial").write_text("02\n")
    signing_config = root / "signing.cnf"
    signing_config.write_text("[ca]\ndefault_ca=authority\n[authority]\n"
                              f'database="{(root / "database").as_posix()}"\n'
                              f'serial="{(root / "serial").as_posix()}"\n'
                              f'new_certs_dir="{root.as_posix()}"\n'
                              f'certificate="{ca.as_posix()}"\nprivate_key="{ca_key.as_posix()}"\n'
                              "default_md=sha256\npolicy=names\nx509_extensions=extensions\n"
                              "[names]\ncommonName=supplied\n[extensions]\n" + extensions)
    openssl("ca", "-batch", "-notext", "-config", signing_config,
            "-startdate", "20000101000000Z", "-enddate", "20000102000000Z",
            "-in", csr, "-out", expired)
    # Validate the actual server leaf, not just a self-signed trust anchor.
    openssl("verify", "-CAfile", ca, "-purpose", "sslserver", server)
    profile = openssl("x509", "-in", server, "-noout", "-text").stdout
    assert (b"TLS Web Server Authentication" in profile and b"CA:FALSE" in profile
            and b"DNS:localhost" in profile)
    rejected = subprocess.run(["openssl", "verify", "-CAfile", str(ca), "-purpose", "sslserver",
                               str(expired)], capture_output=True)
    assert rejected.returncode != 0 and b"expired" in rejected.stdout + rejected.stderr
    return Certificates(ca, server, key, expired)
