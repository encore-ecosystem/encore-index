#!/usr/bin/env python3
"""Partial frames must not block background analysis or corrupt UTF-8 bodies."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from protocol_features import request, send
from scheduler import response, stop


def frame(message):
    body = json.dumps(message, ensure_ascii=False).encode()
    return f'content-length: {len(body)}\r\nContent-Type: application/vscode-jsonrpc; charset=utf-8\r\n\r\n'.encode() + body


def main():
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='encore-framing-') as directory:
        root = Path(directory)
        (root / 'src').mkdir()
        (root / 'encore.toml').write_text('[project]\nname="frames"\nversion="0.0.0"\ndependencies=[]\n')
        path = root / 'src/main.enq'
        source = 'fn main() -> u32 { ret 0 }\n'
        path.write_text(source)
        process = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            request(process, 1, 'initialize', {'rootUri': root.as_uri(), 'initializationOptions': {
                'jobs': 1, 'debounceMs': 200, 'cache': False}})
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 1, 'text': source}}})
            send(process, {'jsonrpc': '2.0', 'id': 2, 'method': 'textDocument/diagnostic',
                'params': {'textDocument': {'uri': path.as_uri()}}})
            pending = frame({'jsonrpc': '2.0', 'id': 3, 'method': 'textDocument/documentSymbol',
                'params': {'textDocument': {'uri': path.as_uri()}, 'extra': '🌍 Привет'}})
            process.stdin.write(pending[:7])
            process.stdin.flush()
            started = time.monotonic()
            assert 'result' in response(process, 2)
            assert time.monotonic() - started < 5, 'partial header blocked workers'
            split = pending.index('🌍'.encode()) + 2
            process.stdin.write(pending[7:split])
            process.stdin.flush()
            time.sleep(0.03)
            process.stdin.write(pending[split:])
            process.stdin.flush()
            assert 'result' in response(process, 3), 'split UTF-8 body corrupted framing'
            messages = [frame({'jsonrpc': '2.0', 'id': number, 'method': 'textDocument/documentSymbol',
                'params': {'textDocument': {'uri': path.as_uri()}}}) for number in range(4, 24)]
            process.stdin.write(b''.join(messages))
            process.stdin.flush()
            for number in range(4, 24):
                assert 'result' in response(process, number), number
            stop(process)
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
        for invalid in (b'Content-Length: 1x\r\n\r\n',
                        b'Content-Length: 1\r\nContent-Length: 2\r\n\r\n',
                        b'Content-Length: 999999999999999999999999\r\n\r\n',
                        b'X' * 16385):
            result = subprocess.run([binary], input=invalid, capture_output=True, timeout=5)
            assert b'"code":-32600' in result.stdout.replace(b' ', b''), result.stdout
    print('incremental framing, worker progress, UTF-8, pipelining and frame limits: ok')


if __name__ == '__main__':
    main()
