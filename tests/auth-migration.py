#!/usr/bin/env python3
"""Check schema-1 migration using an actual pre-auth registry executable."""
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile


def main():
    old, new = (str(Path(p).resolve()) for p in sys.argv[1:3])
    with tempfile.TemporaryDirectory(prefix="encore-auth-migration-") as temporary:
        root = Path(temporary)
        data = root / "data"
        env = {**os.environ, "ENCORE_INDEX_DATA": str(data)}

        def run(binary, *args):
            return subprocess.check_output([binary, *args], env=env, text=True).strip()

        run(old, "init")
        run(old, "user-add", "alice")
        run(old, "reserve", "owned", "alice")
        token = run(old, "token-create", "alice")
        db_path = data / "registry.sqlite3"
        with sqlite3.connect(db_path) as db:
            assert db.execute("PRAGMA user_version").fetchone() == (1,)
            before = db.execute("SELECT owner,digest FROM tokens").fetchall()
            owners = db.execute("SELECT * FROM owners").fetchall()
        run(new, "init")
        with sqlite3.connect(db_path) as db:
            assert db.execute("PRAGMA user_version").fetchone() == (3,)
            assert db.execute("SELECT owner,digest FROM tokens").fetchall() == before
            assert db.execute("SELECT * FROM owners").fetchall() == owners
            assert db.execute("SELECT expires BETWEEN unixepoch()+2591900 AND unixepoch()+2592100 FROM tokens").fetchone() == (1,)
            assert token not in "\n".join(db.iterdump())
        run(new, "init")  # Idempotent open, not a repeated migration.
        subprocess.run([sys.executable, "maintenance.py", "backup", str(data),
                        str(root / "backup")], check=True, capture_output=True)
        subprocess.run([sys.executable, "maintenance.py", "restore", str(root / "backup"),
                        str(root / "restored")], check=True, capture_output=True)
        with sqlite3.connect(root / "restored" / "registry.sqlite3") as db:
            assert db.execute("PRAGMA user_version").fetchone() == (3,)
            assert db.execute("SELECT * FROM owners").fetchall() == owners
        with sqlite3.connect(db_path) as db:
            db.execute("PRAGMA user_version=99")
        rejected = subprocess.run([new, "init"], env=env, capture_output=True)
        assert rejected.returncode != 0
    print("Auth migration: v1 -> v3, ownership/digests, legacy expiry, backup/restore and unknown-schema rejection passed")


if __name__ == "__main__":
    main()
