#!/usr/bin/env python3
"""Exercise large-document highlighting, UTF-16 ranges and repeat requests."""

import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from protocol_features import request, send


def main():
    binary = str(Path(sys.argv[1]).resolve())
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    with tempfile.TemporaryDirectory(prefix="encore-semantic-") as temporary:
        root = Path(temporary)
        (root / "encore.toml").write_text(
            '[project]\nname = "highlight"\nversion = "0.0.0"\ndependencies = []\n'
        )
        source = '/* Unicode: Привет\r\n 🌍 // still a block comment\r\n*/\r\n' + ''.join(
            f'fn item_{i}(value: u32) -> u32 {{ ret value + {i}_u32 }}\r\n'
            for i in range(count)
        )
        source += 'fn unicode() -> str { ret "🌍 Привет" } // конец\r\n'
        path = root / "src" / "main.enq"
        path.parent.mkdir()
        path.write_bytes(source.encode())
        process = subprocess.Popen(
            [binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        try:
            request(process, 1, "initialize", {
                "rootUri": root.as_uri(),
                "initializationOptions": {"jobs": 2, "cache": False, "debounceMs": 0},
            })
            send(process, {"jsonrpc": "2.0", "method": "initialized", "params": {}})
            send(process, {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": path.as_uri(), "languageId": "encore", "version": 1, "text": source},
            }})
            params = {"textDocument": {"uri": path.as_uri()}}
            baseline = None
            timings = []
            for number in range(3):
                started = time.monotonic()
                data = request(process, 10 + number, "textDocument/semanticTokens/full", params)["data"]
                timings.append(round((time.monotonic() - started) * 1000, 2))
                assert len(data) > 400 * 5, f"large document lost its tokens: {len(data)} values"
                assert len(data) % 5 == 0
                line = column = 0
                lines = source.splitlines()
                previous_end = 0
                for offset in range(0, len(data), 5):
                    delta_line, delta_column, length, kind, modifiers = data[offset:offset + 5]
                    line += delta_line
                    column = delta_column if delta_line else column + delta_column
                    if delta_line:
                        previous_end = 0
                    assert length > 0 and column >= previous_end, (line, column, length)
                    assert column + length <= len(lines[line].encode("utf-16-le")) // 2
                    previous_end = column + length
                assert line == len(lines) - 1, f"truncated highlighting at line {line}"
                if baseline is not None:
                    assert data == baseline, "unchanged document produced unstable highlighting"
                baseline = data
            print(json.dumps({"semantic_tokens": len(baseline) // 5, "latency_ms": timings}))
            changed = 'fn main() -> u32 { ret 0_u32 }\n'
            send(process, {"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": path.as_uri(), "version": 2},
                "contentChanges": [{"text": changed}],
            }})
            updated = request(process, 20, "textDocument/semanticTokens/full", params)["data"]
            assert 0 < len(updated) < 100, "stale large-document token cache after edit"
            send(process, {"jsonrpc": "2.0", "method": "textDocument/didClose", "params": params})
            send(process, {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": path.as_uri(), "languageId": "encore", "version": 1, "text": source},
            }})
            reopened = request(process, 21, "textDocument/semanticTokens/full", params)["data"]
            assert reopened == baseline, "stale token cache after reopening"
            request(process, 99, "shutdown", {})
            send(process, {"jsonrpc": "2.0", "method": "exit", "params": {}})
            process.wait(timeout=20)
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()


if __name__ == "__main__":
    main()
