#!/usr/bin/env python3
"""Resolve the published SQLite package from an empty isolated registry cache."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    compiler = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='sqlite-index-consumer-') as scratch:
        root = Path(scratch)
        (root / 'src').mkdir()
        (root / 'encore.toml').write_text('[project]\nname="sqlite_consumer"\nversion="0.0.0"\ndependencies=[]\n')
        (root / 'src/main.enq').write_text('''import core::result::Result
import core::vec::Vec
import sqlite::{Connection, SqliteError, Value}
fn run() -> Result[bool, SqliteError] {
    with Connection::memory()? as db {
        let _create = db.execute("CREATE TABLE items(value INTEGER)", Vec[Value]::new())?
        with db.transaction(true)? as tx {
            let _insert = tx.execute("INSERT INTO items VALUES(42)", Vec[Value]::new())?
            let _commit = tx.commit()?
        }
        let rows = db.query("SELECT value FROM items", Vec[Value]::new())?
        for row in rows { ret Result::Ok(row.integer(0_usize)? == 42_i64) }
    }
    ret Result::Ok(false)
}
fn main() -> u32 {
    match run() {
        Result::Ok(value) => { ret if value { 0_u32 } else { 1_u32 } }
        Result::Err(_) => { ret 1_u32 }
    }
}
''')
        environment = {**os.environ, 'ENCORE_REGISTRY_CACHE': str(root / 'registry-cache')}
        for args in (('add', 'sqlite'), ('run',)):
            result = subprocess.run([compiler, *args], cwd=root, env=environment,
                                    capture_output=True, text=True, timeout=120)
            assert result.returncode == 0, result.stdout + result.stderr
        assert 'index@sqlite' in (root / 'encore.toml').read_text()
        lock = (root / 'encore.lock').read_text()
        assert 'name = "sqlite"' in lock and 'version = "0.1.0"' in lock
        assert 'path@' not in lock
    print('Published SQLite: empty-cache resolution, native compilation and transaction round-trip passed')


if __name__ == '__main__':
    main()
