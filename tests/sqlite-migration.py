#!/usr/bin/env python3
"""Check the Encore SQLite migration against the previous native executable."""
import hashlib
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile


def main():
    old, new = (str(Path(path).resolve()) for path in sys.argv[1:3])
    with tempfile.TemporaryDirectory(prefix="registry-sqlite-migration-") as scratch:
        for version in (1, 2, 3):
            directory = Path(scratch) / f"schema-{version}"
            env = {**os.environ, "ENCORE_INDEX_DATA": str(directory)}

            def run(binary, *args):
                result = subprocess.run([binary, *args], env=env, capture_output=True,
                                        text=True, timeout=15)
                assert result.returncode == 0, result.stderr
                return result.stdout.strip()

            run(old, "init")
            run(old, "user-add", "alice")
            run(old, "reserve", "owned", "alice")
            token = run(old, "token-create", "alice")
            path = directory / "registry.sqlite3"
            with sqlite3.connect(path) as db:
                if version < 3:
                    db.execute("DROP TABLE oauth_attempts")
                if version < 2:
                    for table in ("auth_audit", "credential_packages", "credentials",
                                  "device_authorizations", "identities"):
                        db.execute(f"DROP TABLE {table}")
                    db.execute("ALTER TABLE tokens DROP COLUMN expires")
                db.execute(f"PRAGMA user_version={version}")
                before = {table: db.execute(f"SELECT * FROM {table}").fetchall()
                          for table in ("users", "packages", "owners", "audit")}
                digest = db.execute("SELECT digest FROM tokens").fetchone()[0]
                assert digest == hashlib.sha256(token.encode()).hexdigest()
            run(new, "init")
            run(new, "init")
            with sqlite3.connect(path) as db:
                assert db.execute("PRAGMA user_version").fetchone() == (3,)
                assert db.execute("PRAGMA integrity_check").fetchone() == ("ok",)
                assert db.execute("PRAGMA foreign_key_check").fetchall() == []
                for table, records in before.items():
                    assert db.execute(f"SELECT * FROM {table}").fetchall() == records
                assert db.execute("SELECT digest FROM tokens").fetchone() == (digest,)
                assert db.execute("SELECT expires > unixepoch() FROM tokens").fetchone() == (1,)
                assert token not in "\n".join(db.iterdump())
            run(new, "token-revoke", "1")
            with sqlite3.connect(path) as db:
                assert db.execute("SELECT revoked FROM tokens").fetchone() == (1,)
            backup = Path(scratch) / f"backup-{version}"
            restored = Path(scratch) / f"restore-{version}"
            for action, source, target in (("backup", directory, backup), ("restore", backup, restored)):
                subprocess.run([sys.executable, "maintenance.py", action, str(source), str(target)],
                               check=True, capture_output=True, timeout=15)
            with sqlite3.connect(restored / "registry.sqlite3") as db:
                assert db.execute("SELECT * FROM owners").fetchall() == before["owners"]
    print("SQLite migration: schemas 1/2/3, idempotence, credentials, ownership and backup/restore passed")


if __name__ == "__main__":
    main()
